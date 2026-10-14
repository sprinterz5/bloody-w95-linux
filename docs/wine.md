# Capturing the protocol with Bloody under Wine

The protocol was captured on Linux by running the official BloodyWorkShop8 under Wine
11 and logging every hidraw packet Wine sends — no Windows machine or USB sniffer needed.

## Tooling (`wine/`)

- **`hidsniff.c`** → `hidsniff.so`, an `LD_PRELOAD` shim for all Wine processes. Logs
  every `/dev/hidraw*` feature/output/input packet and acts as a **write guard**:
  `SET_FEATURE` only reaches the device if it matches an allowlist (default: known reads
  + RGB direct mode); output writes are dropped. Blocked requests get the echo the
  mouse itself would give, so the app keeps going. `HIDSNIFF_DRYRUN=1` tests rules
  without touching the device; `HIDSNIFF_ALLOW` overrides the allowlist.
  ```bash
  gcc -shared -fPIC -O2 -o hidsniff.so hidsniff.c -ldl
  ```
- **`hix/`** → `hix.dll` (32-bit), a shim between the app and Wine's `hid.dll`: logs HID
  calls with caller addresses and implements `HidD_GetIndexedString` (see below).
  ```bash
  i686-w64-mingw32-gcc -shared -O2 -fno-omit-frame-pointer -o hix.dll hix/hix.c hix/hix.def -lhid
  ```
- **`bloody-wine.sh`** — creates the prefix, enables hidraw for the mouse, runs the app
  under the guard (refuses to run without it), writes `captures/session.log`.
- **`marker.sh`** — type a label before each action in the app; it lands in the log.
- **`decode.py`** — splits the log at markers and diffs sections.

Getting BloodyWorkShop8 itself to start under Wine needed local changes to the vendor
binary, which are not distributed here.

## Wine issues found

1. **winebus hides HID mice/keyboards from hidraw.** Needed
   `HKLM\System\CurrentControlSet\Services\WineBus\Devices\09da/1434` → `Hidraw=1`
   (by design, documented in `winebus.sys/main.c`).
2. **`HidD_GetIndexedString` always fails** — `hidclass.sys` has no handler for
   `IOCTL_HID_GET_INDEXED_STRING` (`0xb01e2`). Apps that read USB string descriptors by
   index cannot identify devices. Worked around in `hix.dll` by mapping indices 1/2/3 to
   manufacturer/product/serial.
3. **`EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS)` returns `dmLogPixels = 0`.**
   Apps comparing it with the monitor DPI conclude the scaling changed.

## Lessons

- Bloody flashes the mouse whenever its bundled firmware version differs from the
  device's — use a Bloody build whose `FW/USB_M0/W95Max_*_<ver>` matches the mouse.
- Never run the vendor app against real hardware without the guard.
