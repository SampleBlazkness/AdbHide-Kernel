#!/usr/bin/env bash
# Build the Zygisk native module and package it as a flashable zip.
# Requires the Android NDK (r27c by default) and python3 for packaging.
set -euo pipefail

NDK="${ANDROID_NDK_HOME:-D:/Android_SDK/ndk/android-ndk-r27c}"
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
OUT="$ROOT/build/native"
DIST="$ROOT/dist"
TARGET="arm64-v8a"
API="26"
CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android${API}-clang++.cmd"

[ -f "$CC" ] || { echo "clang++ not found: $CC"; exit 1; }

VERSION="$(sed -n 's/^version=//p' "$ROOT/native/module.prop" | head -1)"

mkdir -p "$OUT/zygisk" "$DIST"
rm -f "$OUT/zygisk/$TARGET.so"

echo "[1/2] compile $TARGET"
"$CC" -shared -fPIC -O2 -std=c++17 -DNDEBUG -static-libstdc++ \
  -I "$ROOT/native" \
  "$ROOT/native/adbhide_native.cpp" \
  -o "$OUT/zygisk/$TARGET.so" \
  -llog
ls -l "$OUT/zygisk/$TARGET.so"

echo "[2/2] package"
python - "$ROOT" "$OUT" "$VERSION" <<'PY'
import os, stat, sys, zipfile

root, out, version = sys.argv[1], sys.argv[2], sys.argv[3]
dst = os.path.join(root, "dist", f"AdbHide-native-{version}.zip")

with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    zi = zipfile.ZipInfo("module.prop")
    zi.external_attr = (stat.S_IFREG | 0o644) << 16
    z.writestr(zi, open(os.path.join(root, "native", "module.prop"), "rb").read())
    zi = zipfile.ZipInfo("zygisk/arm64-v8a.so")
    zi.external_attr = (stat.S_IFREG | 0o644) << 16
    zi.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(zi, open(os.path.join(out, "zygisk", "arm64-v8a.so"), "rb").read())

print("  packed", dst, os.path.getsize(dst), "bytes")
PY
