// Copyright 2018 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.config.server.maintenance;

import com.yahoo.config.provision.SystemName;
import com.yahoo.config.provision.Zone;
import com.yahoo.vespa.config.server.ApplicationRepository;
import com.yahoo.vespa.curator.Curator;

import java.time.Duration;

/**
 * Removes inactive sessions
 * <p>
 * Note: Unit test is in ApplicationRepositoryTest
 *
 * @author hmusum
 */
public class SessionsMaintainer extends Maintainer {
    private final boolean hostedVespa;

    SessionsMaintainer(ApplicationRepository applicationRepository, Curator curator, Duration interval) {
        super(applicationRepository, curator, interval);
        this.hostedVespa = applicationRepository.configserverConfig().hostedVespa();
    }

    @Override
    protected void maintain() {
        applicationRepository.deleteExpiredLocalSessions();

        // Expired remote sessions are not expected to exist, they should have been deleted when
        // a deployment happened or when the application was deleted. We still see them from time to time,
        // probably due to some race or another bug
        if (hostedVespa) {
            Duration expiryTime = Duration.ofDays(30);
            Zone zone = applicationRepository.zone();
            // TODO: Delete in all zones
            boolean deleteFromZooKeeper = zone.system() == SystemName.cd ||
                    zone.environment().isTest() ||
                    zone.region().value().equals("us-central-1");
            applicationRepository.deleteExpiredRemoteSessions(expiryTime, deleteFromZooKeeper);
        }
    }
}
