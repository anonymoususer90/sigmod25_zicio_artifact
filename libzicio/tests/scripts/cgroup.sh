#!/bin/bash

sudo chmod o+w /sys/fs/cgroup/cgroup.procs

if [ ! -d /sys/fs/cgroup/small_16.slice ]; then
	sudo mkdir /sys/fs/cgroup/small_16.slice
	sudo chmod o+w /sys/fs/cgroup/small_16.slice/cgroup.procs
	echo 16G | sudo tee /sys/fs/cgroup/small_16.slice/memory.max > /dev/null
fi

if [ ! -d /sys/fs/cgroup/large_256.slice ]; then
	sudo mkdir /sys/fs/cgroup/large_256.slice
	sudo chmod o+w /sys/fs/cgroup/large_256.slice/cgroup.procs
	echo 256G | sudo tee /sys/fs/cgroup/large_256.slice/memory.max > /dev/null
fi
