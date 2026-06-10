#!/usr/bin/env bash
# Golden test: the C core must replay the JS game logic bit-for-bit.
set -e
cd "$(dirname "$0")/.."
mkdir -p tests/build

node tests/gen_golden.mjs > tests/build/expected.txt
cc -DHOST_BUILD -Iinclude -Wall -Wextra -O1 \
    src/core/rng.c src/core/board.c src/core/spin.c src/core/rules.c src/core/seams.c tests/host_main.c \
    -o tests/build/host_golden
tests/build/host_golden > tests/build/actual.txt

if diff -q tests/build/expected.txt tests/build/actual.txt > /dev/null; then
    echo "GOLDEN OK: $(wc -l < tests/build/expected.txt | tr -d ' ') lines identical (JS vs C)"
else
    echo "GOLDEN MISMATCH — first differences:"
    diff tests/build/expected.txt tests/build/actual.txt | head -30
    exit 1
fi
