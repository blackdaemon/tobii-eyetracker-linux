#!/usr/bin/env bash
# Install Tobii eye tracker support (IS4 / Eye Tracker 4C, IS5 / Eye Tracker 5)
# on Fedora. Run as a regular user; it uses sudo for the system parts.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V="$ROOT/vendor"
SUDO=sudo; [[ $EUID -eq 0 ]] && SUDO=

[[ -f "$V/usbservice/usr/local/sbin/tobiiusbserviced" ]] || "$ROOT/scripts/fetch-vendor.sh"

echo "==> Installing build dependencies"
rpm -q gcc make gtk4-devel >/dev/null 2>&1 || $SUDO dnf install -y gcc make gtk4-devel

echo "==> Installing tobiiusbserviced (USB host daemon)"
# The binary has RPATH=/usr/local/lib/tobiiusb, so its libraries must live there.
$SUDO install -D -m 0755 "$V/usbservice/usr/local/sbin/tobiiusbserviced" /usr/local/sbin/tobiiusbserviced
$SUDO install -d -m 0755 /usr/local/lib/tobiiusb
for l in "$V"/usbservice/usr/local/lib/tobiiusb/*.so; do
    $SUDO install -m 0755 "$l" /usr/local/lib/tobiiusb/
done

echo "==> Installing Tobii Stream Engine 3.0.4 (client library + headers)"
$SUDO install -D -m 0755 "$V/stream_engine/lib/x64/libtobii_stream_engine.so" /usr/local/lib64/tobii/libtobii_stream_engine.so
$SUDO install -d -m 0755 /usr/local/include/tobii
$SUDO install -m 0644 "$V"/stream_engine/include/tobii/*.h /usr/local/include/tobii/
$SUDO install -D -m 0644 "$ROOT/tobii-stream-engine.pc.in" /usr/local/lib64/pkgconfig/tobii-stream-engine.pc
echo /usr/local/lib64/tobii | $SUDO tee /etc/ld.so.conf.d/tobii-stream-engine.conf >/dev/null
$SUDO ldconfig

echo "==> Installing systemd unit and udev rule"
$SUDO install -D -m 0644 "$ROOT/systemd/tobiiusb.service" /etc/systemd/system/tobiiusb.service
$SUDO install -D -m 0644 "$ROOT/udev/70-tobii-eyetracker.rules" /etc/udev/rules.d/70-tobii-eyetracker.rules
$SUDO install -D -m 0644 "$ROOT/README.md" /usr/local/share/doc/tobii-eyetracker-linux/README.md

echo "==> Building and installing tobii-gaze and tobii-setup"
make -C "$ROOT/tools" clean
make -C "$ROOT/tools" TOBII_INC=/usr/local/include TOBII_LIB=/usr/local/lib64/tobii
$SUDO install -m 0755 "$ROOT/tools/tobii-gaze" "$ROOT/tools/tobii-setup" /usr/local/bin/

if command -v restorecon >/dev/null; then
    echo "==> Restoring SELinux labels"
    $SUDO restorecon -R /usr/local/sbin/tobiiusbserviced /usr/local/lib/tobiiusb /usr/local/lib64/tobii \
        /usr/local/bin/tobii-gaze /usr/local/bin/tobii-setup /etc/systemd/system/tobiiusb.service /etc/udev/rules.d/70-tobii-eyetracker.rules
fi

echo "==> Enabling service"
$SUDO systemctl daemon-reload
$SUDO udevadm control --reload
# Never replay "add" uevents (udevadm trigger --action=add) for the tracker:
# tobiiusbserviced watches udev itself and a duplicate "add" for a device it
# already owns leaves it unable to serve clients. A restart is safe.
$SUDO systemctl enable tobiiusb.service
$SUDO systemctl restart tobiiusb.service
sleep 2
systemctl --no-pager --lines=5 status tobiiusb.service || true

cat <<EOF

Done. Try:
    tobii-gaze --info          # device info
    tobii-gaze                 # stream gaze points
    tobii-gaze --pointer       # move the mouse pointer with your gaze (needs /dev/uinput access)
EOF
