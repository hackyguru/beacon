#!/usr/bin/env bash
# Offline checks on station identity, replay and reordering. Opens no socket
# and needs no Basecamp.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH=/nix/var/nix/profiles/default/bin:$PATH
NIX="nix --extra-experimental-features 'nix-command flakes'"
SODIUM=$(eval $NIX build --impure --no-link --print-out-paths --expr "'(import <nixpkgs> {}).libsodium.dev'")
SODIUM_LIB=$(eval $NIX build --impure --no-link --print-out-paths --expr "'(import <nixpkgs> {}).libsodium'")
clang++ -std=c++17 -O1 -o "$HERE/harness" \
    "$HERE/harness.cpp" "$HERE/../src/station.cpp" "$HERE/../src/stream_buffer.cpp" \
    -I"$SODIUM/include" -L"$SODIUM_LIB/lib" -lsodium -Wl,-rpath,"$SODIUM_LIB/lib"
"$HERE/harness"
