# Tobii Eye Tracker on Fedora Linux

Fedora support for Tobii consumer eye trackers of the IS4 generation: **Tobii Eye Tracker 4C** (USB `2104:0127` "EyeChip", model `IS4_Large_Peripheral`, firmware 2.27). Tested on Fedora 45, GNOME Wayland. The Eye Tracker 5 (IS5) is untested.

> **One-time requirement: screen setup and calibration on Windows.** On Linux the Stream Engine only gets the `consumer` license level, so it can't write the display area or calibrate. Until a tracker has been set up in Tobii Experience on Windows, its display area is a 4×4 mm placeholder and it reports almost no valid eye or gaze samples. After a single setup on Windows 11 (bare metal or a VM with USB passthrough), the display area and calibration are **stored on the tracker** and survive reboots into Linux. Check with `tobii-setup status` and `tobii-setup diag`.

Tobii doesn't support Linux for these devices, but it did once publish Linux builds of its USB host service and the Stream Engine SDK. Community projects ([Eitol](https://github.com/Eitol/tobii_eye_tracker_linux_installer), [MoscowskyAnton](https://github.com/MoscowskyAnton/tobii_eye_tracker_linux_installer), [johngebbie/tobii_4C_for_linux](https://github.com/johngebbie/tobii_4C_for_linux)) packaged those builds for Ubuntu and Arch. This repo builds on them for Fedora:

| Piece | Source | Notes |
|---|---|---|
| `tobiiusbserviced` 2.1.5 | Tobii (proprietary) | Root daemon that owns the USB device and serves clients over a local socket |
| `libtobii_stream_engine.so` 3.0.4 | Tobii (proprietary) | Client SDK: gaze point, head pose, device info |
| `systemd/tobiiusb.service` | this repo | Replaces the Debian SysV init script |
| `udev/70-tobii-eyetracker.rules` | this repo | Starts the service on hotplug and disables USB autosuspend |
| `tools/tobii-gaze` | this repo (MIT) | Diagnostics, gaze/head-pose streaming, gaze→pointer (Wayland/X11 via uinput) |

The proprietary binaries are **not committed** here. `scripts/fetch-vendor.sh` downloads them from a pinned commit of the MoscowskyAnton mirror and checks them against `vendor.sha256`.

## Install

```bash
./install.sh          # fetches vendor files, installs with sudo, enables tobiiusb.service
```

Install locations: `/usr/local/sbin/tobiiusbserviced`, `/usr/local/lib/tobiiusb/` (fixed by the daemon's RPATH), `/usr/local/lib64/tobii/libtobii_stream_engine.so`, `/usr/local/include/tobii/`, and the pkg-config module `tobii-stream-engine`.

Remove everything with `./uninstall.sh`.

## Use

```bash
tobii-gaze --info              # model, serial, firmware, frequency, display area
tobii-gaze                     # "<timestamp_us> gaze <x> <y>", x/y normalised 0..1 on the display area
tobii-gaze --head              # also head position (mm) and rotation
tobii-gaze --pointer -q        # move the pointer with your eyes (Ctrl+C stops)
tobii-gaze --pointer -s 0.8    # heavier smoothing
```

Screen setup and diagnostics:

```bash
tobii-setup status             # license level, display area, mounting, monitor size from EDID
tobii-setup diag 15            # per-second tracking quality: eyes found, gaze validity and range
tobii-setup display            # set display area from EDID (needs config license, see below)
tobii-setup calibrate          # fullscreen 9-point calibration + live gaze dot (needs config license)
```

`--pointer` creates a virtual absolute pointer, "Tobii Gaze Pointer", through `/dev/uinput`, so it works on GNOME/KDE Wayland as well as X11. Your user needs write access to `/dev/uinput` (for example, membership in the `uinput` group). With several monitors, the compositor maps the absolute pointer over the whole desktop.

### Build your own client

```bash
cc myapp.c $(PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig pkg-config --cflags --libs tobii-stream-engine)
```

The Stream Engine API reference is in `vendor/stream_engine/tobii_stream_engine_api.pdf`.

### Python

`examples/python/tobii_stream.py` is a dependency-free `ctypes` binding (one file, copy it into your project):

```python
from tobii_stream import EyeTracker

with EyeTracker() as et:
    for t_us, x, y in et.gaze():          # blocking generator, ~90 Hz
        print(x, y)
```

For GUI and game loops, run the tracker in a background thread and poll the latest sample:

```python
with EyeTracker() as et:
    et.subscribe_eye_position(lambda t, left, right: ...)   # optional; subscribe first
    q = et.gaze_in_thread()                                  # queue holding the newest point
    while running:
        t_us, x, y = q.get()                                 # or q.get_nowait()
```

Gaze is `(0,0)` at the top-left and `(1,1)` at the bottom-right of the display area. Values slightly outside 0..1 mean you're looking just off-screen. Eye positions are in mm in tracker coordinates (x right, y up, z towards you). Set `TOBII_STREAM_ENGINE=/path/to/libtobii_stream_engine.so` to load a different copy of the library.

> Tobii's library is marked as needing an executable stack, and glibc ≥ 2.41 refuses to `dlopen()` it ("cannot enable executable stack"). `install.sh` clears that flag on the installed copy with `scripts/clear-execstack.py`. Programs that link the library at build time (C, Rust) aren't affected.

### Rust

`examples/rust` is a small crate with no dependencies. It has a safe wrapper with RAII cleanup and closures for callbacks:

```toml
[dependencies]
tobii-stream-engine = { path = "/path/to/tobii-eyetracker-linux/examples/rust" }
```

```rust
let api = tobii_stream_engine::Api::new()?;
let mut dev = api.connect_first()?;
dev.subscribe_gaze(|g| println!("{} {:.3} {:.3}", g.timestamp_us, g.x, g.y))?;
dev.subscribe_eye_position(|e| { /* e.left_mm / e.right_mm: Option<[f32; 3]> */ })?;
loop { dev.process()?; }        // callbacks run inside process()
```

Try it with `cd examples/rust && cargo run --example gaze 5`. `build.rs` links `/usr/local/lib64/tobii` (override with `TOBII_LIB_DIR`) and embeds an rpath. `Device` isn't `Send`, because Stream Engine calls for one device must stay on one thread. To use gaze elsewhere, send samples from the callback over a channel.

## Troubleshooting

```bash
systemctl status tobiiusb          # service running?
journalctl -u tobiiusb -b
lsusb -d 2104:                     # device enumerated? ET5 = 2104:0127
```

* **`tobii_device_create` fails with "Connection to the eye tracker was lost"**, and the service log shows `claim_interface ERROR: 16` / `Binding ERROR?: 98`: the daemon got a duplicate "add" uevent for a tracker it already owned (for example from `udevadm trigger --action=add`), and it can't recover from that. Run `sudo systemctl restart tobiiusb`.
* **"no eye tracker found"**: the service isn't running, or it can't claim interface 0. Try `sudo systemctl restart tobiiusb` and replug the tracker.
* **Everything is "gaze invalid", or the numbers look random**: run `tobii-setup status`. If the display area shows `4 x 4 mm`, the tracker has no screen setup. With that placeholder the firmware reports almost no valid eyes, even though `presence` reads `present`. `tobii-setup display` / `calibrate` fail with *Insufficient permissions* (they need a `config` license). Run the screen setup and calibration in Tobii Experience on Windows once. Afterwards `diag` should show ~100% valid eyes and gaze spanning 0..1.
* **Few valid samples even for eye position**: run `tobii-setup diag` while looking at the screen. Check the tracker tilt (its magnetic mount), viewing distance (about 45–95 cm), reflective glasses and strong sunlight.
* **SELinux**: `install.sh` runs `restorecon`. If you see AVC denials, run `sudo ausearch -m avc -ts recent`.

## Not included (yet)

* Screen setup and calibration from Linux (blocked by the `config` license requirement; see above).
* `tobii_engine` and `tobii_config` from the same Tobii release need libuv 0.10, gconf2 and libappindicator1, which Fedora doesn't have. They're optional for basic gaze.
* RPM packaging (`.spec`).

## License

Scripts and `tools/` in this repo: MIT. Tobii binaries: Tobii's SDK license (`vendor/stream_engine/SDK-License_en.pdf`). Check that it permits your use.
