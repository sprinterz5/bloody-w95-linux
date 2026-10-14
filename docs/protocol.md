# A4Tech Bloody W95 Max — protocol notes

Device: USB `09da:1434`, firmware `bcdDevice 0.31`, model id `0x0578`, sensor PixArt
PMW3332 (per the firmware file names `W95Max_P3332A_578_<core>_<ver>` that Bloody ships).

Confidence tags: **[verified]** tried on hardware, **[captured]** seen in Bloody's
traffic but not replayed, **[guess]** inferred.

## Transport

Three HID interfaces:

| Interface | hidraw | Purpose |
|---|---|---|
| 0 | boot keyboard | firmware-emulated keys (macros) |
| 1 | boot mouse | pointer |
| 2 | composite | reports 1–4 (NKRO keyboard, system control, consumer, absolute-ish mouse) + **report 7, vendor page `0xFF33` usage `0x0578`** |

All configuration goes through **report 7** on interface 2:
Feature 63 bytes (+ report id = 64), Input 63, Output 255.

Request/response pattern: `SET_FEATURE` a 64-byte packet, then `GET_FEATURE` — the mouse
returns the request echoed with the answer filled in from byte 8 on. A `GET_FEATURE`
without a preceding request just returns the last command.

Packet layout: `07 <opcode> <b2> <b3> <b4> 00 00 <b7> <data from byte 8 …>`.
Read vs. write is usually a flag: the same opcode with `0x00` reads and with `0x80`
writes (in byte 2 or byte 4 depending on the opcode). **[captured]**

## Identification (safe reads) [verified]

| Request | Response (bytes 8+) | Meaning |
|---|---|---|
| `07 1f` | `00 24 78 05 31` | `0x0578` = model id, `0x31` = firmware version |
| `07 2d` | `01 00 00 00 31 00 78 05 01` | firmware `0x31`, model `0x0578` |
| `07 29` | `b2 3b b7 17 01 02 19 20` | id/serial + date-like bytes |
| `07 05` | `01 00 e8 03 00 e0 …` | device/sensor info |
| `07 2a`, `07 2f 00`, `07 0e 00`, `07 20 00` | various | profile/state reads |

## RGB lighting [verified]

From OpenRGB (W90 MAX), works unchanged:

```
07 03 06 01 …                     take control (direct mode)
07 03 06 00 00 00 00 00 01 …      enable
07 03 06 02 00 00 00 00 RGB×15    set colours, LED n at byte 8 + 3n
```

Volatile — the mouse returns to its stored lighting on replug.
LED map (see `led_map.json`): wheel `14`, top triangle `7`, bottom ring right side
`0–5`, back `6`, left side `8–13`. Bloody also uses `07 03 06 05`, `07 03 06 07…0c`
(not decoded).

## DPI [verified, live only]

```
07 0d 00 00 00 00 00 00 04 <slot> <rx> <ry> <code> 02
```

- `slot` 0–4.
- `rx ry` = `40 40` up to at least 3200 DPI; Bloody sends `57 57` for 8400 **[captured]**.
- `code` (with `40 40`): 800→`0x11`, 900→`0x13`, 1200→`0x1a`, 1600→`0x23`,
  3000→`0x43`, 3200→`0x48` — linear, ≈ DPI / 43.6 − 1.3 (`bloodyctl.dpi_code`).
  8400 with `57 57` → `0x91` **[captured]**.

Takes effect immediately, **lost on replug** — this writes RAM only.

## Saving / "Apply" [captured, not replayed]

Bloody's Apply sends, in order: `07 2f 01 …` ×9, `07 13 00 00 80 00 00 00 80`, `07 0d …`.
`2f 01` is the prime suspect for committing to flash. Not tried yet.

On connect Bloody pushes its whole profile: `07 07`, `07 1e 01`, `07 09 … 01`,
`07 0f … 07`, `07 0e 80 … 64 64 64`, `07 14`, `07 11 00 00 80 … 03` (backlight level
write, cf. maxmati), `07 0a`, `07 0c … 06 80 01`, `07 13 …`, `07 0d …`, `07 06`.
On exit: `07 0a`, `07 09`, `07 16`, `07 08`.

## Dangerous — do not send

| Packet | Effect |
|---|---|
| `07 28 01 00 00 00 00 00 00 00 88 f3` then `07 00` | **jump to bootloader** — mouse re-enumerates as `09da:d023` and stays there until reflashed (Bloody does this when its bundled firmware version differs from the mouse's) |
| `07 21 00 00 00 00 00 <len> <spi cmd> …` | raw SPI-flash passthrough (`9f` JEDEC id, `05` status, `06` write-enable, `02` page program, `20` sector erase). Even a bare `9f` read **resets the MCU**. Bloody uses it to rewrite the profile flash (~190 page programs + sector erases) |
