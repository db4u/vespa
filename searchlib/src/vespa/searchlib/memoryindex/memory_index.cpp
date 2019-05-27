// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "document_inverter.h"
#include "field_index_collection.h"
#include "memory_index.h"
#include "posting_iterator.h"
#include <vespa/document/fieldvalue/arrayfieldvalue.h>
#include <vespa/document/fieldvalue/document.h>
#include <vespa/vespalib/btree/btreenodeallocator.hpp>
#include <vespa/searchlib/common/sequencedtaskexecutor.h>
#include <vespa/searchlib/index/schemautil.h>
#include <vespa/searchlib/queryeval/booleanmatchiteratorwrapper.h>
#include <vespa/searchlib/queryeval/create_blueprint_visitor_helper.h>
#include <vespa/searchlib/queryeval/emptysearch.h>
#include <vespa/searchlib/queryeval/leaf_blueprints.h>

#include <vespa/log/log.h>
LOG_SETUP(".searchlib.memoryindex.memory_index");

using document::ArrayFieldValue;
using document::WeightedSetFieldValue;
using vespalib::LockGuard;
using vespalib::GenerationHandler;

namespace search {

using fef::TermFieldMatchDataArray;
using index::IndexBuilder;
using index::Schema;
using index::SchemaUtil;
using query::NumberTerm;
using query::LocationTerm;
using query::Node;
using query::PredicateQuery;
using query::PrefixTerm;
using query::RangeTerm;
using query::RegExpTerm;
using query::StringTerm;
using query::SubstringTerm;
using query::SuffixTerm;
using queryeval::SearchIterator;
using queryeval::Searchable;
using queryeval::CreateBlueprintVisitorHelper;
using queryeval::Blueprint;
using queryeval::BooleanMatchIteratorWrapper;
using queryeval::EmptyBlueprint;
using queryeval::FieldSpecBase;
using queryeval::FieldSpecBaseList;
using queryeval::FieldSpec;
using queryeval::IRequestContext;

}

namespace search::memoryindex {

MemoryIndex::MemoryIndex(const Schema &schema,
                         ISequencedTaskExecutor &invertThreads,
                         ISequencedTaskExecutor &pushThreads)
    : _schema(schema),
      _invertThreads(invertThreads),
      _pushThreads(pushThreads),
      _inverter0(std::make_unique<DocumentInverter>(_schema, _invertThreads, _pushThreads)),
      _inverter1(std::make_unique<DocumentInverter>(_schema, _invertThreads, _pushThreads)),
      _inverter(_inverter0.get()),
      _fieldIndexes(std::make_unique<FieldIndexCollection>(_schema)),
      _frozen(false),
      _maxDocId(0), // docId 0 is reserved
      _numDocs(0),
      _lock(),
      _hiddenFields(schema.getNumIndexFields(), false),
      _prunedSchema(),
      _indexedDocs(0),
      _staticMemoryFootprint(getMemoryUsage().allocatedBytes())
{
}

MemoryIndex::~MemoryIndex()
{
    _invertThreads.sync();
    _pushThreads.sync();
}

void
MemoryIndex::insertDocument(uint32_t docId, const document::Document &doc)
{
    if (_frozen) {
        LOG(warning, "Memory index frozen: ignoring insert of document '%s'(%u): '%s'",
            doc.getId().toString().c_str(), docId, doc.toString().c_str());
        return;
    }
    updateMaxDocId(docId);
    _inverter->invertDocument(docId, doc);
    if (_indexedDocs.insert(docId).second) {
        incNumDocs();
    }
}

void
MemoryIndex::removeDocument(uint32_t docId)
{
    if (_frozen) {
        LOG(warning, "Memory index frozen: ignoring remove of document (%u)", docId);
        return;
    }
    _inverter->removeDocument(docId);
    if (_indexedDocs.find(docId) != _indexedDocs.end()) {
        _indexedDocs.erase(docId);
        decNumDocs();
    }
}

void
MemoryIndex::commit(const std::shared_ptr<IDestructorCallback> &onWriteDone)
{
    _invertThreads.sync(); // drain inverting into this inverter
    _pushThreads.sync(); // drain use of other inverter
    _inverter->pushDocuments(*_fieldIndexes, onWriteDone);
    flipInverter();
}

void
MemoryIndex::flipInverter()
{
    _inverter = (_inverter != _inverter0.get()) ? _inverter0.get(): _inverter1.get();
}

void
MemoryIndex::freeze()
{
    _frozen = true;
}

void
MemoryIndex::dump(IndexBuilder &indexBuilder)
{
    _fieldIndexes->dump(indexBuilder);
}

namespace {

class MemTermBlueprint : public queryeval::SimpleLeafBlueprint {
private:
    GenerationHandler::Guard               _genGuard;
    FieldIndex::PostingList::ConstIterator _pitr;
    const FeatureStore                    &_featureStore;
    const uint32_t                         _fieldId;
    const bool                             _useBitVector;

public:
    MemTermBlueprint(GenerationHandler::Guard &&genGuard,
                     FieldIndex::PostingList::ConstIterator pitr,
                     const FeatureStore &featureStore,
                     const FieldSpecBase &field,
                     uint32_t fieldId,
                     bool useBitVector)
        : SimpleLeafBlueprint(field),
          _genGuard(),
          _pitr(pitr),
          _featureStore(featureStore),
          _fieldId(fieldId),
          _useBitVector(useBitVector)
    {
        _genGuard = std::move(genGuard);
        HitEstimate estimate(_pitr.size(), !_pitr.valid());
        setEstimate(estimate);
    }

    SearchIterator::UP createLeafSearch(const TermFieldMatchDataArray &tfmda, bool) const override {
        auto search = std::make_unique<PostingIterator>(_pitr, _featureStore, _fieldId, tfmda);
        if (_useBitVector) {
            LOG(debug, "Return BooleanMatchIteratorWrapper: fieldId(%u), docCount(%zu)",
                _fieldId, _pitr.size());
            return std::make_unique<BooleanMatchIteratorWrapper>(std::move(search), tfmda);
        }
        LOG(debug, "Return PostingIterator: fieldId(%u), docCount(%zu)",
            _fieldId, _pitr.size());
        return search;
    }

};

/**
 * Determines the correct Blueprint to use.
 **/
class CreateBlueprintVisitor : public CreateBlueprintVisitorHelper {
private:
    const FieldSpec &_field;
    const uint32_t   _fieldId;
    FieldIndexCollection &_fieldIndexes;

public:
    CreateBlueprintVisitor(Searchable &searchable,
                           const IRequestContext & requestContext,
                           const FieldSpec &field,
                           uint32_t fieldId,
                           FieldIndexCollection &fieldIndexes)
        : CreateBlueprintVisitorHelper(searchable, field, requestContext),
          _field(field),
          _fieldId(fieldId),
          _fieldIndexes(fieldIndexes) {}

    template <class TermNode>
    void visitTerm(TermNode &n) {
        const vespalib::string termStr = queryeval::termAsString(n);
        LOG(debug, "searching for '%s' in '%s'",
            termStr.c_str(), _field.getName().c_str());
        FieldIndex *fieldIndex = _fieldIndexes.getFieldIndex(_fieldId);
        GenerationHandler::Guard genGuard = fieldIndex->takeGenerationGuard();
        FieldIndex::PostingList::ConstIterator pitr = fieldIndex->findFrozen(termStr);
        bool useBitVector = _field.isFilter();
        setResult(std::make_unique<MemTermBlueprint>(std::move(genGuard), pitr,
                                                     fieldIndex->getFeatureStore(),
                                                     _field, _fieldId, useBitVector));
    }

    void visit(LocationTerm &n)  override { visitTerm(n); }
    void visit(PrefixTerm &n)    override { visitTerm(n); }
    void visit(RangeTerm &n)     override { visitTerm(n); }
    void visit(StringTerm &n)    override { visitTerm(n); }
    void visit(SubstringTerm &n) override { visitTerm(n); }
    void visit(SuffixTerm &n)    override { visitTerm(n); }
    void visit(RegExpTerm &n)    override { visitTerm(n); }
    void visit(PredicateQuery &) override { }

    void visit(NumberTerm &n) override {
        handleNumberTermAsText(n);
    }

};

} // namespace search::memoryindex::<unnamed>

Blueprint::UP
MemoryIndex::createBlueprint(const IRequestContext & requestContext,
                             const FieldSpec &field,
                             const Node &term)
{
    uint32_t fieldId = _schema.getIndexFieldId(field.getName());
    if (fieldId == Schema::UNKNOWN_FIELD_ID || _hiddenFields[fieldId]) {
        return std::make_unique<EmptyBlueprint>(field);
    }
    CreateBlueprintVisitor visitor(*this, requestContext, field, fieldId, *_fieldIndexes);
    const_cast<Node &>(term).accept(visitor);
    return visitor.getResult();
}

vespalib::MemoryUsage
MemoryIndex::getMemoryUsage() const
{
    vespalib::MemoryUsage usage;
    usage.merge(_fieldIndexes->getMemoryUsage());
    return usage;
}

uint64_t
MemoryIndex::getNumWords() const {
    return _fieldIndexes->getNumUniqueWords();
}

void
MemoryIndex::pruneRemovedFields(const Schema &schema)
{
    LockGuard lock(_lock);
    if (_prunedSchema.get() == nullptr) {
        auto newSchema = Schema::intersect(_schema, schema);
        if (_schema == *newSchema) {
            return;
        }
        _prunedSchema.reset(newSchema.release());
    } else {
        auto newSchema = Schema::intersect(*_prunedSchema, schema);
        if (*_prunedSchema == *newSchema) {
            return;
        }
        _prunedSchema.reset(newSchema.release());
    }
    SchemaUtil::IndexIterator i(_schema);
    for (; i.isValid(); ++i) {
        uint32_t packedIndex = i.getIndex();
        assert(packedIndex < _hiddenFields.size());
        SchemaUtil::IndexIterator wi(*_prunedSchema, i);
        _hiddenFields[packedIndex] = !wi.isValid();
    }
}

Schema::SP
MemoryIndex::getPrunedSchema() const
{
    LockGuard lock(_lock);
    return _prunedSchema;
}

}
