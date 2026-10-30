#!/bin/sh
# Compileaza harness-ul de pe gazda (ext4 + jbd2 + bcache reale, stub-uri pentru restul) in build/ext4_host
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build
g++ -std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter \
    -Itests/host/stubs -Ikernel/include -Ikernel \
    tests/host/ext4_host.cpp kernel/fs/ext4/*.cpp kernel/fs/jbd2/jbd2.cpp kernel/lib/crc.cpp kernel/block/bcache.cpp \
    -o build/ext4_host
