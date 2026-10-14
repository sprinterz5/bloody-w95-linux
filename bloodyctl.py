#!/usr/bin/env python3
"""bloodyctl — experimental control tool for A4Tech Bloody W95 Max (09da:1434).

Talks to the vendor channel: interface 2, report ID 0x07, 64-byte feature reports
(vendor usage page 0xFF33). Protocol bits come from OpenRGB's BloodyMouseController
(W60/W70/W90) and maxmati/a4tech-bloody-linux-driver; the rest is to be reversed.
"""
import argparse
import fcntl
import glob
import os
import select
import sys
import time

VID, PID = 0x09DA, 0x1434
REPORT_ID = 0x07
PACKET = 64  # report ID + 63 data bytes

# W95 Max layout, mapped by hand (led_map.json). Matches OpenRGB's W90 MAX.
# The bottom rim is a ring: right side 0-5, back 6, left side 8-13.
RING = [0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13]
ZONES = {
    "wheel": [14],
    "logo": [7],  # triangular cutout on top
    "ring": RING,
    "right": RING[:6],
    "back": [6],
    "left": RING[7:],
}
LED_COUNT = 15


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("H") << 8) | nr


def HIDIOCSFEATURE(n):
    return _ioc(3, 0x06, n)


def HIDIOCGFEATURE(n):
    return _ioc(3, 0x07, n)


def find_hidraw():
    for node in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        dev = os.path.realpath(os.path.join(node, "device"))
        try:
            uevent = open(os.path.join(dev, "uevent")).read()
        except OSError:
            continue
        if f"HID_ID=0003:{VID:08X}:{PID:08X}" not in uevent:
            continue
        # .../3-1:1.2/0003:09DA:1434.000X -> interface 2 is the vendor channel
        if os.path.basename(os.path.dirname(dev)).endswith(".2"):
            return "/dev/" + os.path.basename(node)
    sys.exit("Bloody W95 Max vendor interface not found (is the mouse plugged in?)")


def hexdump(data, prefix=""):
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        print(f"{prefix}{off:02x}: " + " ".join(f"{b:02x}" for b in chunk))


class Mouse:
    def __init__(self, path=None):
        self.path = path or find_hidraw()
        try:
            self.fd = os.open(self.path, os.O_RDWR)
        except PermissionError:
            sys.exit(f"No access to {self.path}. Install the udev rule (see README) and replug the mouse.")

    def send(self, payload):
        buf = bytearray(PACKET)
        buf[0] = REPORT_ID
        buf[1:1 + len(payload)] = payload
        fcntl.ioctl(self.fd, HIDIOCSFEATURE(PACKET), bytes(buf))

    def recv(self):
        buf = bytearray(PACKET)
        buf[0] = REPORT_ID
        fcntl.ioctl(self.fd, HIDIOCGFEATURE(PACKET), buf)
        return bytes(buf)

    def query(self, payload):
        self.send(payload)
        return self.recv()

    # --- RGB (OpenRGB "direct" mode, volatile: the mouse reverts on replug) ---
    def rgb_take_control(self):
        self.send(bytes([0x03, 0x06, 0x01]))
        self.send(bytes([0x03, 0x06, 0x00, 0, 0, 0, 0, 0x01]))

    def rgb_release(self):
        # Guess: same switch with data byte 0 hands lighting back to the firmware.
        self.send(bytes([0x03, 0x06, 0x00, 0, 0, 0, 0, 0x00]))

    def rgb_set(self, colors):
        """colors: dict led_index -> (r, g, b)"""
        payload = bytearray(PACKET - 1)
        payload[0:3] = bytes([0x03, 0x06, 0x02])
        for idx, (r, g, b) in colors.items():
            off = 7 + 3 * idx  # data starts at byte 8 of the full report
            payload[off:off + 3] = bytes([r, g, b])
        self.send(payload)


# DPI codes captured from Bloody (07 0d byte 12), range byte 0x40. Above ~5000 Bloody
# switches bytes 10/11 to 0x57 (8400 -> 0x91); that range is not decoded yet.
DPI_POINTS = [(800, 0x11), (900, 0x13), (1200, 0x1A), (1600, 0x23), (3000, 0x43), (3200, 0x48)]
DPI_MAX_SAFE = 5000


def dpi_code(dpi):
    """Linear fit through the captured points (~43.6 DPI per step)."""
    (x0, y0), (x1, y1) = DPI_POINTS[0], DPI_POINTS[-1]
    return max(1, round(y0 + (dpi - x0) * (y1 - y0) / (x1 - x0)))


def cmd_dpi(args):
    """Set DPI live (RAM only — the mouse reverts to its saved value on replug)."""
    if not 100 <= args.dpi <= DPI_MAX_SAFE:
        sys.exit(f"DPI must be 100..{DPI_MAX_SAFE} for now (higher range not decoded yet)")
    code = dpi_code(args.dpi)
    Mouse(args.dev).send(bytes([0x0D, 0, 0, 0, 0, 0, 0, 0x04, args.slot, 0x40, 0x40, code, 0x02]))
    print(f"slot {args.slot}: ~{args.dpi} DPI (code {code:#04x}), until replug")


def parse_color(s):
    s = s.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("color must be RRGGBB")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def parse_bytes(items):
    return bytes(int(x, 16) for x in items)


def cmd_info(args):
    path = find_hidraw()
    print(f"vendor interface: {path}")
    print(f"accessible: {os.access(path, os.R_OK | os.W_OK)}")


def cmd_color(args):
    m = Mouse(args.dev)
    leds = range(LED_COUNT) if args.zone == "all" else ZONES[args.zone]
    m.rgb_take_control()
    m.rgb_set({i: args.color for i in leds})


def cmd_rainbow(args):
    """Static rainbow along the ring (optionally animated) — checks LED order."""
    import colorsys
    m = Mouse(args.dev)
    m.rgb_take_control()
    shift = 0.0
    try:
        while True:
            colors = {}
            for pos, idx in enumerate(RING):
                r, g, b = colorsys.hsv_to_rgb((pos / len(RING) + shift) % 1.0, 1.0, 1.0)
                colors[idx] = (int(r * 255), int(g * 255), int(b * 255))
            colors[7] = colors[14] = colors[RING[0]]
            m.rgb_set(colors)
            if not args.animate:
                break
            shift += 0.01
            time.sleep(0.03)
    except KeyboardInterrupt:
        pass


def cmd_led(args):
    """Light exactly one LED index — used to map physical LEDs."""
    m = Mouse(args.dev)
    m.rgb_take_control()
    m.rgb_set({args.index: args.color})


def cmd_walk(args):
    """Light LEDs 0..N one by one so you can note which index is where."""
    m = Mouse(args.dev)
    m.rgb_take_control()
    for i in range(args.count):
        print(f"LED {i}", flush=True)
        m.rgb_set({i: (255, 255, 255)})
        time.sleep(args.delay)
    m.rgb_set({})


def cmd_map(args):
    """Interactive LED mapping: light each index and record where it shows up."""
    import json
    m = Mouse(args.dev)
    m.rgb_take_control()
    result = {}
    print("Опиши, где горит белый светодиод (например: колесо, лого, зад-1, бок-лев).")
    print("Пусто / Enter = ничего не горит. q = закончить.\n")
    for i in range(args.count):
        m.rgb_set({i: (255, 255, 255)})
        answer = input(f"LED {i:2d}: ").strip()
        if answer == "q":
            break
        result[i] = answer or None
    m.rgb_set({})
    with open(args.out, "w") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
    print(f"\nсохранено в {args.out}")


def cmd_release(args):
    Mouse(args.dev).rgb_release()


def cmd_raw(args):
    """Send a raw feature report (bytes after the 0x07 report ID), optionally read back."""
    m = Mouse(args.dev)
    payload = parse_bytes(args.bytes)
    print("TX:")
    hexdump(bytes([REPORT_ID]) + payload, "  ")
    m.send(payload)
    if args.read:
        print("RX:")
        hexdump(m.recv(), "  ")


def cmd_listen(args):
    """Print every input report from the vendor interface (press buttons / switch DPI)."""
    m = Mouse(args.dev)
    print(f"listening on {m.path}, Ctrl+C to stop")
    try:
        while True:
            r, _, _ = select.select([m.fd], [], [], 1.0)
            if r:
                data = os.read(m.fd, 256)
                print(f"{time.strftime('%H:%M:%S')} id={data[0]:02x} " + " ".join(f"{b:02x}" for b in data[1:]))
    except KeyboardInterrupt:
        pass


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--dev", help="hidraw node (auto-detected by default)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info").set_defaults(fn=cmd_info)

    s = sub.add_parser("color", help="set a zone to one color (volatile)")
    s.add_argument("color", type=parse_color)
    s.add_argument("--zone", choices=["all", *ZONES], default="all")
    s.set_defaults(fn=cmd_color)

    s = sub.add_parser("dpi", help="set DPI live (lost on replug)")
    s.add_argument("dpi", type=int)
    s.add_argument("--slot", type=int, default=0, choices=range(5))
    s.set_defaults(fn=cmd_dpi)

    s = sub.add_parser("rainbow", help="rainbow along the ring")
    s.add_argument("--animate", action="store_true", help="spin until Ctrl+C")
    s.set_defaults(fn=cmd_rainbow)

    s = sub.add_parser("led", help="light a single LED index")
    s.add_argument("index", type=int)
    s.add_argument("color", type=parse_color, nargs="?", default=(255, 255, 255))
    s.set_defaults(fn=cmd_led)

    s = sub.add_parser("walk", help="light LED indices one by one")
    s.add_argument("--count", type=int, default=LED_COUNT)
    s.add_argument("--delay", type=float, default=1.5)
    s.set_defaults(fn=cmd_walk)

    s = sub.add_parser("map", help="interactive LED mapping, saves JSON")
    s.add_argument("--count", type=int, default=18)  # (63 - 7) // 3 LEDs fit in one packet
    s.add_argument("--out", default="led_map.json")
    s.set_defaults(fn=cmd_map)

    sub.add_parser("release", help="return lighting to firmware (guess)").set_defaults(fn=cmd_release)

    s = sub.add_parser("raw", help="send raw bytes after report ID, e.g. raw 03 06 01")
    s.add_argument("bytes", nargs="+")
    s.add_argument("--read", action="store_true", help="GET_FEATURE afterwards")
    s.set_defaults(fn=cmd_raw)

    sub.add_parser("listen", help="dump input reports").set_defaults(fn=cmd_listen)

    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
