#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
cmake -S . -B build/mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build build/mac -j 8
APP="$ROOT/build/ThunderDisplay.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/Licenses"
cp vendor/pyrowave-metal/LICENSE "$APP/Contents/Resources/Licenses/PyroWave.txt"
cp LICENSE "$APP/Contents/Resources/Licenses/ThunderDisplay.txt"
cp THIRD_PARTY_NOTICES "$APP/Contents/Resources/Licenses/THIRD_PARTY_NOTICES"
cp mac/Info.plist "$APP/Contents/Info.plist"
cp -L build/mac/vendor/pyrowave-metal/libpyrowave-metal.1.dylib "$APP/Contents/Frameworks/"
swiftc -target "$(uname -m)-apple-macosx14.0" -O -parse-as-library mac/App.swift -import-objc-header mac/Bridge.h \
  -L build/mac -ltd-engine -ltd-common -L build/mac/vendor/pyrowave-metal -lpyrowave-metal \
  -Xlinker -lc++ \
  -framework VideoToolbox -framework Cocoa -framework SwiftUI -framework ScreenCaptureKit -framework CoreMedia \
  -framework CoreVideo -framework Metal -framework MetalPerformanceShaders -framework IOSurface -framework Carbon -framework IOKit -framework ServiceManagement \
  -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
  -o "$APP/Contents/MacOS/ThunderDisplay"
python3 scripts/sign-mac.py "$APP"
echo "$APP"
