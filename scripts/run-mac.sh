#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -x build/ThunderDisplay.app/Contents/MacOS/ThunderDisplay ]; then
  bash scripts/build-mac.sh
fi
exec open "$PWD/build/ThunderDisplay.app"
