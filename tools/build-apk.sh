#!/usr/bin/env bash
# Build the LSPosed APK without Gradle: aapt2 -> javac -> d8 -> apksigner.
# Everything comes from the local Android SDK; no network access needed.
set -euo pipefail

SDK="${ANDROID_SDK_ROOT:-D:/Android_SDK}"
BT="$SDK/build-tools/36.0.0"
PLATFORM="$SDK/platforms/android-37.0/android.jar"
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
APP="$ROOT/app"
OUT="$ROOT/build/apk"
VERSION_NAME="1.3.1"
VERSION_CODE=30

rm -rf "$OUT"
mkdir -p "$OUT/stubs-classes" "$OUT/classes" "$OUT/gen" "$OUT/dex" "$ROOT/dist"

echo "[1/6] stubs"
find "$APP/stubs" -name '*.java' > "$OUT/stubs.list"
javac --release 11 -nowarn -encoding UTF-8 -d "$OUT/stubs-classes" @"$OUT/stubs.list"
(cd "$OUT/stubs-classes" && jar cf "$OUT/stubs.jar" .)

echo "[2/6] resources"
"$BT/aapt2.exe" compile --dir "$APP/res" -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/base.apk" -I "$PLATFORM" \
  --manifest "$APP/AndroidManifest.xml" \
  --min-sdk-version 26 --target-sdk-version 33 \
  --version-code "$VERSION_CODE" --version-name "$VERSION_NAME" \
  -R "$OUT/res.zip" --java "$OUT/gen" --auto-add-overlay

echo "[3/6] javac"
{ find "$APP/src" -name '*.java'; find "$OUT/gen" -name '*.java'; } > "$OUT/src.list"
javac --release 11 -nowarn -encoding UTF-8 \
  -cp "$PLATFORM;$OUT/stubs.jar" -d "$OUT/classes" @"$OUT/src.list"
(cd "$OUT/classes" && jar cf "$OUT/classes.jar" .)

echo "[4/6] d8"
"$BT/d8.bat" --lib "$PLATFORM" --min-api 26 --output "$OUT/dex" "$OUT/classes.jar"

echo "[5/6] package"
python - "$OUT" "$APP" <<'PY'
import os, sys, zipfile
out, app = sys.argv[1], sys.argv[2]
src = os.path.join(out, "base.apk")
dst = os.path.join(out, "unsigned.apk")
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zo:
    for item in zi.infolist():
        zo.writestr(item, zi.read(item.filename))
    zo.write(os.path.join(out, "dex", "classes.dex"), "classes.dex")
    zo.write(os.path.join(app, "assets", "xposed_init"), "assets/xposed_init")
print("  packed", dst)
PY
"$BT/zipalign.exe" -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo "[6/6] sign"
KS="$(cygpath -w "$HOME/.android/debug.keystore")"
if [ ! -f "$KS" ]; then
  keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
    -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" \
    -keyalg RSA -keysize 2048 -validity 10000
fi
"$BT/apksigner.bat" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --min-sdk-version 26 --out "$ROOT/dist/AdbHide-$VERSION_NAME.apk" "$OUT/aligned.apk"
"$BT/apksigner.bat" verify --print-certs "$ROOT/dist/AdbHide-$VERSION_NAME.apk" | head -4
ls -l "$ROOT/dist/AdbHide-$VERSION_NAME.apk"
