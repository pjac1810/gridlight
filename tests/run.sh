#!/bin/sh
# Host test of GRIDLIGHT: a song through the six modes and every sound to build/render-out/*.wav, then a fuzz (UBSan).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/render-out
${CC:-cc} -O1 -g -fsanitize=undefined -fno-sanitize-recover -Wall -Wextra -Isrc -Iplatform \
  tests/render.c src/seq.c src/snd.c src/tones.c -lm -o build/render
build/render build/render-out
