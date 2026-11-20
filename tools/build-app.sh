#!/bin/sh
# adbng - SPDX-License-Identifier: MIT
#
# Builds the manager APK without Gradle: aapt2 -> javac -> d8 -> zip -> zipalign -> apksigner.
#
# On this aarch64 host the SDK's aapt2/zipalign are x86_64 ELF binaries, so they
# are run through qemu-x86_64-static (amd64 glibc installed alongside).
#
# env: ANDROID_SDK, ANDROID_BUILD_TOOLS, ANDROID_PLATFORM, ADBNG_KEYSTORE
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SDK=${ANDROID_SDK:-/root/android-sdk}
BT=${ANDROID_BUILD_TOOLS:-$SDK/build-tools/36.0.0}
PLATFORM=${ANDROID_PLATFORM:-$SDK/platforms/android-36/android.jar}
APP=$ROOT/app
OUT=$ROOT/build/app
APK=$ROOT/build/adbng.apk

[ -f "$PLATFORM" ] || { echo "missing $PLATFORM" >&2; exit 1; }
[ -x "$BT/d8" ] || { echo "missing $BT/d8" >&2; exit 1; }

if "$BT/aapt2" version >/dev/null 2>&1; then
    AAPT2="$BT/aapt2"
    ZIPALIGN="$BT/zipalign"
else
    QEMU=$(command -v qemu-x86_64-static || command -v qemu-x86_64 || true)
    [ -n "$QEMU" ] || { echo "aapt2 is x86_64 and no qemu-x86_64 is installed" >&2; exit 1; }
    AAPT2="$QEMU $BT/aapt2"
    ZIPALIGN="$QEMU $BT/zipalign"
    LD_LIBRARY_PATH="$BT/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi

echo "== staging"
rm -rf "$OUT"
mkdir -p "$OUT/gen" "$OUT/classes" "$OUT/dex" "$APP/assets"
cp -f "$ROOT/build/usbgwd" "$APP/assets/usbgwd"
cp -f "$ROOT/tools/adbngctl.sh" "$APP/assets/adbngctl.sh"
cp -f "$ROOT/tools/usb-gadget.sh" "$APP/assets/usb-gadget.sh"
chmod 644 "$APP/assets"/*

echo "== aapt2 compile"
$AAPT2 compile --dir "$APP/res" -o "$OUT/res.zip"

echo "== aapt2 link"
$AAPT2 link -o "$OUT/base.apk" \
    -I "$PLATFORM" \
    --manifest "$APP/AndroidManifest.xml" \
    -R "$OUT/res.zip" \
    -A "$APP/assets" \
    --java "$OUT/gen" \
    --min-sdk-version 24 \
    --target-sdk-version 36 \
    --auto-add-overlay

echo "== javac"
# shellcheck disable=SC2046
javac -nowarn -source 8 -target 8 \
    -bootclasspath "$PLATFORM" -classpath "$PLATFORM" \
    -d "$OUT/classes" \
    $(find "$APP/src" "$OUT/gen" -name '*.java')

echo "== d8"
# shellcheck disable=SC2046
"$BT/d8" --lib "$PLATFORM" --min-api 24 --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

echo "== package"
cp -f "$OUT/base.apk" "$OUT/unsigned.apk"
( cd "$OUT/dex" && zip -q -X "$OUT/unsigned.apk" classes.dex )
$ZIPALIGN -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo "== sign"
KS=${ADBNG_KEYSTORE:-$HOME/.android/adbng.keystore}
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    keytool -genkeypair -keystore "$KS" -alias adbng \
        -storepass android -keypass android \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=adbng debug,O=blazkness,C=CN" >/dev/null
    echo "   created $KS"
fi
"$BT/apksigner" sign --ks "$KS" --ks-key-alias adbng \
    --ks-pass pass:android --key-pass pass:android \
    --out "$APK" "$OUT/aligned.apk"

"$BT/apksigner" verify --print-certs "$APK" | head -n 3
ls -l "$APK"