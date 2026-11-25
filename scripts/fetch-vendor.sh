#!/usr/bin/env bash
# Download the proprietary Tobii Linux binaries this project builds on and
# unpack them into ./vendor. Nothing here is redistributed by this repo: the
# files come from Tobii's (since withdrawn) Linux developer downloads, as
# mirrored by https://github.com/MoscowskyAnton/tobii_eye_tracker_linux_installer
# at a pinned commit, and are checked against vendor.sha256.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/vendor"
COMMIT=1ad41b5b898b946893b25adb5fc702dde7af0cb2
BASE="https://raw.githubusercontent.com/MoscowskyAnton/tobii_eye_tracker_linux_installer/$COMMIT"
FILES=(tobiiusbservice_l64U14_2.1.5-28fd4a.deb stream_engine_linux_3.0.4.6031.tar.gz)

for tool in curl ar tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool (sudo dnf install binutils curl tar coreutils)" >&2; exit 1; }
done

mkdir -p "$VENDOR/dl"
cd "$VENDOR/dl"
for f in "${FILES[@]}"; do
    if [[ ! -f $f ]]; then
        echo "Downloading $f"
        curl -fL --retry 3 -o "$f.part" "$BASE/$f"
        mv "$f.part" "$f"
    fi
done
sha256sum -c "$ROOT/vendor.sha256"

# tobiiusbservice: a .deb -> unpack the data payload only (maintainer scripts
# are Debian/SysV specific and are replaced by our systemd/udev files).
rm -rf "$VENDOR/usbservice" "$VENDOR/stream_engine"
mkdir -p "$VENDOR/usbservice/deb" "$VENDOR/stream_engine"
( cd "$VENDOR/usbservice/deb" && ar x "$VENDOR/dl/tobiiusbservice_l64U14_2.1.5-28fd4a.deb" )
tar -xf "$VENDOR/usbservice/deb"/data.tar.* -C "$VENDOR/usbservice"
rm -rf "$VENDOR/usbservice/deb"

tar -xzf "$VENDOR/dl/stream_engine_linux_3.0.4.6031.tar.gz" -C "$VENDOR/stream_engine"

echo
echo "Vendor files ready in $VENDOR"
echo "  daemon : $VENDOR/usbservice/usr/local/sbin/tobiiusbserviced"
echo "  library: $VENDOR/stream_engine/lib/x64/libtobii_stream_engine.so"
