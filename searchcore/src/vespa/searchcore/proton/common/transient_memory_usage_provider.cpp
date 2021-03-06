// Copyright Verizon Media. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "transient_memory_usage_provider.h"

namespace proton {

TransientMemoryUsageProvider::TransientMemoryUsageProvider()
    : ITransientMemoryUsageProvider(),
      _transient_memory_usage(0u)
{
}

TransientMemoryUsageProvider::~TransientMemoryUsageProvider() = default;

size_t
TransientMemoryUsageProvider::get_transient_memory_usage() const
{
    return _transient_memory_usage.load(std::memory_order_relaxed);
}

void
TransientMemoryUsageProvider::set_transient_memory_usage(size_t transient_memory_usage)
{
    _transient_memory_usage.store(transient_memory_usage, std::memory_order_relaxed);
}

}
