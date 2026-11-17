#!/usr/bin/env bash
# Builds spectral_sidechain.so for Gen1 MPC OS (armv7 hard-float, glibc <= 2.31) WITHOUT Docker, with zig (pip install ziglang),
# tests the real .so with the framework's host test under qemu-arm (apt: qemu-user libc6-armhf-cross), and packages it.
#   MV=../mpc-vst-plugins tools/build_arm_zig.sh 1.0.0      -> dist/Kick-Duck-<version>-mpc-armv7.zip (portable/ = the VST2 Plugins folder)
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; MV="${MV:-$HERE/../mpc-vst-plugins}"; VER="${1:-1.0.0}"
Z="python3 -m ziglang"; T="-target arm-linux-gnueabihf.2.31 -mcpu=cortex_a9-neon"; B="$HERE/vst/build/armv7"
mkdir -p "$B"; cd "$HERE"
python3 "$MV/tools/gen_vst.py" vst/vst.json
printf '{ global: VSTPluginMain; local: *; };\n' > "$B/exports.map"
$Z c++ $T -O2 -fPIC -fvisibility=hidden -std=gnu++11 -Ivst/build -I"$MV/wrapper" -c src/engine.cpp -o "$B/engine.o"
# this port's copy of the framework's wrapper (wrapper/vst2_wrap.c): the same, plus the 32-bit float audio path
$Z cc  $T -O2 -fPIC -fvisibility=hidden -std=gnu11 -Ivst/build -I"$MV/wrapper" -c wrapper/vst2_wrap.c -o "$B/wrap.o"
$Z c++ $T -O2 -shared -fPIC -s "$B/engine.o" "$B/wrap.o" -lm -lpthread -ldl -Wl,--no-undefined -Wl,--version-script="$B/exports.map" -o "$B/spectral_sidechain.so"
echo "exports: $(readelf --dyn-syms -W "$B/spectral_sidechain.so" | grep ' GLOBAL ' | grep -v UND | awk '{print $8}')"
echo "highest glibc: $(readelf -V "$B/spectral_sidechain.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
if command -v qemu-arm >/dev/null; then
  $Z cc $T -O1 -std=gnu11 -Ivst/build -I"$MV/wrapper" "$MV/tools/host_test.c" "$B/spectral_sidechain.so" -lm -lpthread -o "$B/host_test" 2>/dev/null
  (cd "$B" && qemu-arm -L /usr/arm-linux-gnueabihf -E LD_LIBRARY_PATH="$B" ./host_test | tail -1)
  $Z cc $T -O1 tests/vst_f32_test.c "$B/spectral_sidechain.so" -lm -o "$B/vst_f32_test" 2>/dev/null
  (cd "$B" && qemu-arm -L /usr/arm-linux-gnueabihf -E LD_LIBRARY_PATH="$B" ./vst_f32_test | tail -1)
fi
cd vst && python3 "$MV/tools/release.py" --so "$B/spectral_sidechain.so" --skin "build/skin/DAVIS - VST - Spectral Sidechain" --entry build/pluginlist-entry.xml \
  --version "$VER" ${KD_REPO:+--repo "$KD_REPO"} --about "DAVIS Spectral Sidechain: a one-band sidechain dynamic EQ." --license MIT -o "$HERE/dist"
