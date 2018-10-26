// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.hosted.node.admin.integrationTests;

import com.yahoo.vespa.hosted.dockerapi.Container;
import com.yahoo.vespa.hosted.dockerapi.ContainerName;
import com.yahoo.vespa.hosted.dockerapi.ContainerResources;
import com.yahoo.vespa.hosted.dockerapi.ContainerStats;
import com.yahoo.vespa.hosted.dockerapi.Docker;
import com.yahoo.vespa.hosted.dockerapi.DockerImage;
import com.yahoo.vespa.hosted.dockerapi.ProcessResult;

import java.net.InetAddress;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;

/**
 * Mock with some simple logic
 *
 * @author freva
 */
public class DockerMock implements Docker {
    private final Map<ContainerName, Container> containersByContainerName = new HashMap<>();
    private static final Object monitor = new Object();

    @Override
    public CreateContainerCommand createContainerCommand(
            DockerImage dockerImage,
            ContainerResources containerResources,
            ContainerName containerName,
            String hostName) {
        synchronized (monitor) {
            containersByContainerName.put(
                    containerName, new Container(hostName, dockerImage, containerResources, containerName, Container.State.RUNNING, 2));
        }

        return new StartContainerCommandMock();
    }

    @Override
    public List<Container> getAllContainersManagedBy(String manager) {
        synchronized (monitor) {
            return new ArrayList<>(containersByContainerName.values());
        }
    }

    @Override
    public Optional<ContainerStats> getContainerStats(ContainerName containerName) {
        return Optional.empty();
    }

    @Override
    public void startContainer(ContainerName containerName) {

    }

    @Override
    public void stopContainer(ContainerName containerName) {
        synchronized (monitor) {
            Container container = containersByContainerName.get(containerName);
            containersByContainerName.put(containerName,
                            new Container(container.hostname, container.image, container.resources, container.name, Container.State.EXITED, 0));
        }
    }

    @Override
    public void deleteContainer(ContainerName containerName) {
        synchronized (monitor) {
            containersByContainerName.remove(containerName);
        }
    }

    @Override
    public Optional<Container> getContainer(ContainerName containerName) {
        synchronized (monitor) {
            return Optional.ofNullable(containersByContainerName.get(containerName));
        }
    }

    @Override
    public boolean pullImageAsyncIfNeeded(DockerImage image) {
        synchronized (monitor) {
            return false;
        }
    }

    @Override
    public boolean deleteUnusedDockerImages(List<DockerImage> excludes, Duration minImageAgeToDelete) {
        return false;
    }

    @Override
    public ProcessResult executeInContainerAsUser(ContainerName containerName, String user, OptionalLong timeout, String... args) {
        return new ProcessResult(0, null, "");
    }


    public static class StartContainerCommandMock implements CreateContainerCommand {
        @Override
        public CreateContainerCommand withLabel(String name, String value) {
            return this;
        }

        @Override
        public CreateContainerCommand withEnvironment(String name, String value) {
            return this;
        }

        @Override
        public CreateContainerCommand withVolume(Path path, Path volumePath) {
            return this;
        }

        @Override
        public CreateContainerCommand withSharedVolume(Path path, Path volumePath) {
            return this;
        }

        @Override
        public CreateContainerCommand withNetworkMode(String mode) {
            return this;
        }

        @Override
        public CreateContainerCommand withIpAddress(InetAddress address) {
            return this;
        }

        @Override
        public CreateContainerCommand withUlimit(String name, int softLimit, int hardLimit) {
            return this;
        }

        @Override
        public CreateContainerCommand withEntrypoint(String... entrypoint) {
            return this;
        }

        @Override
        public CreateContainerCommand withManagedBy(String manager) {
            return this;
        }

        @Override
        public CreateContainerCommand withAddCapability(String capabilityName) {
            return this;
        }

        @Override
        public CreateContainerCommand withDropCapability(String capabilityName) {
            return this;
        }

        @Override
        public CreateContainerCommand withPrivileged(boolean privileged) {
            return this;
        }

        @Override
        public void create() {

        }
    }
}
