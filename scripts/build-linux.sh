#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -d vendor/pyrowave ]; then
  mkdir -p vendor
  git clone https://github.com/Themaister/pyrowave.git vendor/pyrowave
  git -C vendor/pyrowave checkout a0d2e71e98b85f6a23f62407df21a39e9745b875
fi
if [ ! -f vendor/pyrowave/Granite/third_party/volk/volk.h ]; then
  (cd vendor/pyrowave && bash checkout_granite.sh)
fi
cmake -S vendor/pyrowave -B build/codec -DCMAKE_BUILD_TYPE=Release
cmake --build build/codec --target pyrowave-shared -j 8
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j 8
