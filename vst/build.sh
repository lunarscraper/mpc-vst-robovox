#!/usr/bin/env bash
# Build Robovox as a VST2 insert effect for the MPC OS plugin host (armhf).
#   vst/build/robovox.so            -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 -> /sdcard/Synths/ on the device
# One source file (robovox_vst.cpp), hand-written VST2 shell, no JUCE, no GUI. libasound is NOT
# linked: the MIDI-in port opens it with dlopen at run time (it ships with MPC OS).
# vst.json/module.json only feed tools/gen_vst.py for params.h + the skin. Offline test + bench: test.sh.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer (host binary; the renderer vendored in mpc-vst-plugins)
if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi

# 2. params.h, skin, pluginlist-entry.xml (needs Pillow). SHADOW_TITLE_FONT: frame titles and
#    selector labels in Titillium, large enough to read on the Force.
export SHADOW_TITLE_FONT="$PWD/fonts/TitilliumWeb-SemiBold.ttf"
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
else
  docker run --rm -u "$U" -e SHADOW_TITLE_FONT=/w/fonts/TitilliumWeb-SemiBold.ttf -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json"
fi
cp "$MPC_VST/wrapper/popup.h" build/

# 3. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39), tuned for the RK3288's Cortex-A17.
docker run --rm --platform linux/arm/v7 -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euxc '
  CXXF="-O2 -fPIC -fvisibility=hidden -std=c++17 -mtune=cortex-a17 -mfpu=neon-vfpv4 -mfloat-abi=hard -fno-math-errno"
  g++ $CXXF -Wall -Wextra -Wno-unused-parameter -Ibuild -shared -o build/robovox.so robovox_vst.cpp \
      -static-libstdc++ -static-libgcc -lpthread -ldl -lm -Wl,--no-undefined
  strip build/robovox.so
  echo "-- size --"; ls -la build/robovox.so
  echo "-- needed --"; readelf -d build/robovox.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/robovox.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/robovox.so
