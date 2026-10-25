#!/usr/bin/env bash
# Build the LSPosed APK without Gradle: aapt2 -> javac -> d8 -> apksigner.
# Everything comes from the local Android SDK; no network access needed.
set -euo pipefail

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) BIN=.exe; SCRIPT=.bat ;;
  *)                    BIN=;     SCRIPT= ;;
esac

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
[ -n "$SDK" ] || {
  echo "ANDROID_SDK_ROOT is not set (point it at the Android SDK)"
  exit 1
}

BT="$(ls -d "$SDK/build-tools"/* 2>/dev/null | sort -V | tail -1)"
PLATFORM="$(ls -d "$SDK/platforms"/android-* 2>/dev/null | sort -V | tail -1)/android.jar"
[ -n "$BT" ] && [ -f "$PLATFORM" ] || {
  echo "build-tools or platform not found under $SDK"
  exit 1
}

ROOT="$(cd "$(dirname "$0")/.." && { pwd -W 2>/dev/null || pwd; })"
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
"$BT/aapt2$BIN" compile --dir "$APP/res" -o "$OUT/res.zip"
"$BT/aapt2$BIN" link -o "$OUT/base.apk" -I "$PLATFORM" \
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
"$BT/d8$SCRIPT" --lib "$PLATFORM" --min-api 26 --output "$OUT/dex" "$OUT/classes.jar"

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
"$BT/zipalign$BIN" -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

echo "[6/6] sign"
# Release key: kept outside the repo; the properties file below names it
PROPS="${RELEASE_PROPS:-$HOME/.android/adbhide-release.properties}"
[ -f "$PROPS" ] || { echo "missing $PROPS"; exit 1; }
prop() { sed -n "s/^$1=//p" "$PROPS" | head -1; }
KS="$(prop storeFile)"
command -v cygpath >/dev/null 2>&1 && KS="$(cygpath -w "$KS")"
KSPASS="$(prop storePassword)"
ALIAS="$(prop keyAlias)"
KEYPASS="$(prop keyPassword)"
APK="$ROOT/dist/AdbHide-$VERSION_NAME.apk"
"$BT/apksigner$SCRIPT" sign --ks "$KS" --ks-pass "pass:$KSPASS" --ks-key-alias "$ALIAS" \
  --key-pass "pass:$KEYPASS" --min-sdk-version 26 --out "$APK" "$OUT/aligned.apk"
"$BT/apksigner$SCRIPT" verify --print-certs "$APK" | head -4
ls -l "$APK"
