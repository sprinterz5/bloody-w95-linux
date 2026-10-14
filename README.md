# bloody-w95-linux

Linux tooling and reverse-engineering notes for the **A4Tech Bloody W95 Max** mouse
(USB `09da:1434`). A4Tech only ships configuration software for Windows; this project
talks to the mouse directly over hidraw.

> Status: early. RGB lighting and live DPI work. Saving settings to the mouse's
> onboard memory, buttons and macros are not decoded yet.

## What works

| Feature | Command | Persists after replug? |
|---|---|---|
| RGB per zone (wheel, logo, ring, left, right, back) | `bloodyctl.py color ff0000 --zone ring` | no |
| Single LED / LED walk (for mapping) | `bloodyctl.py led 7`, `bloodyctl.py walk` | no |
| Rainbow along the bottom ring | `bloodyctl.py rainbow [--animate]` | no |
| DPI per slot (100–5000) | `bloodyctl.py dpi 1600 --slot 0` | no |
| Raw packets (for research) | `bloodyctl.py raw 1f --read`, `bloodyctl.py listen` | — |

Polling rate is a hardware switch on the W95 Max — there is no software command for it.

## Setup

```bash
# let your user open the mouse's hidraw nodes (no root needed afterwards)
sudo cp udev/70-bloody-hidraw.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger

python3 bloodyctl.py info
python3 bloodyctl.py rainbow
```

No dependencies beyond Python 3 — it uses hidraw feature-report ioctls directly.

## Safety

The vendor channel also exposes raw SPI-flash access and a jump-to-bootloader command.
`bloodyctl.py raw` will send whatever you type: **do not send opcodes you do not
understand.** See [docs/protocol.md](docs/protocol.md) for the list of known-dangerous
commands (`07 28`, `07 00`, `07 21`). A mouse stuck in the bootloader shows up as
`09da:d023`; the official Bloody software on Windows recovers it.

## Repository layout

- `bloodyctl.py` — the CLI.
- `docs/protocol.md` — everything known about the protocol, with confidence levels.
- `docs/led_map.json` — physical LED map, made by hand with `bloodyctl.py walk`.
- `docs/wine.md` — how the protocol was captured: running Bloody under Wine with a
  packet logger and a write-guard, and the Wine bugs found on the way.
- `wine/` — the capture tooling (`hidsniff` LD_PRELOAD logger/guard, `hix.dll` HID shim,
  launcher, log decoder).
- `openrgb/w95max.patch` — adds the W95 Max to OpenRGB's `BloodyMouseController`.
- `udev/` — hidraw access rule.

## Credits

The RGB "direct mode" commands come from OpenRGB's `BloodyMouseController`
(W60 Pro / W70 Max / W90 Max), which turned out to work on the W95 Max unchanged.
Opcode 0x11 backlight semantics were first documented in
[maxmati/a4tech-bloody-linux-driver](https://github.com/maxmati/a4tech-bloody-linux-driver).

## License

MIT (see `LICENSE`), except `openrgb/w95max.patch`: it modifies OpenRGB source and,
like OpenRGB, is GPL-2.0-or-later. This repository contains no A4Tech code, firmware
or binaries — only independently written tools and observed protocol behaviour,
documented for interoperability.

Not affiliated with A4Tech. "A4Tech" and "Bloody" are used only to identify the
hardware. Use at your own risk.
