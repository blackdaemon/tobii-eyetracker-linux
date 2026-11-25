#!/usr/bin/env bash
set -euo pipefail
SUDO=sudo; [[ $EUID -eq 0 ]] && SUDO=

$SUDO systemctl disable --now tobiiusb.service 2>/dev/null || true
$SUDO rm -f /etc/systemd/system/tobiiusb.service /etc/udev/rules.d/70-tobii-eyetracker.rules \
    /etc/ld.so.conf.d/tobii-stream-engine.conf /usr/local/lib64/pkgconfig/tobii-stream-engine.pc \
    /usr/local/sbin/tobiiusbserviced /usr/local/bin/tobii-gaze /usr/local/bin/tobii-setup
$SUDO rm -rf /usr/local/lib/tobiiusb /usr/local/lib64/tobii /usr/local/include/tobii \
    /usr/local/share/doc/tobii-eyetracker-linux
$SUDO systemctl daemon-reload
$SUDO udevadm control --reload
$SUDO ldconfig
echo "Tobii eye tracker support removed."
