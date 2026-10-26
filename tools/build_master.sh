#!/bin/bash
# Builds the reference master server, the query tool and the browser's load-test servers (docs/pc_multiplayer.md "N3"):
# build/master/csx-master, csx-query and csx-fakeservers, from src/net (the transport's socket and wire format, the
# query, the master protocol, the browser's discovery) and tools/master. Needs clang++ and libsodium only; no game code.
#   tools/build_master.sh && build/master/csx-master --port 27010
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
mkdir -p build/master
net="src/net/transport.cpp src/net/udp.cpp src/net/query.cpp src/net/master.cpp src/net/browser.cpp"
flags="-std=c++20 -O2 -g -Wall -Werror -Isrc"
clang++ $flags tools/master/master_main.cpp $net -lsodium -o build/master/csx-master
clang++ $flags tools/master/query_main.cpp $net -lsodium -o build/master/csx-query
clang++ $flags tools/master/fake_main.cpp $net -lsodium -o build/master/csx-fakeservers
echo "built build/master/csx-master, csx-query and csx-fakeservers"
