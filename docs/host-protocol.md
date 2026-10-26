# Host protocol of the running firmware

The protocol between a PC and the **running** firmware (USB `0300:3004`). It is the stock vendor protocol; the
firmware in this repository implements it identically (parser and dispatcher rebuilt from stock `0x40241ce0` /
`0x40242494`) apart from the extra event details listed in the last section. Code: `firmware/app/protocol.c`,
`firmware/app/threads.c`. A host-side exerciser is `akp05_tool.exe hid-talk`.

## 1. USB interfaces

| Interface | Class | Endpoints | Use |
|---|---|---|---|
| 0 | HID, vendor page `0xFFA0` | OUT `0x03` 1024 B, IN `0x82` 512 B (interrupt) | commands, uploads, events |
| 1 | HID boot keyboard | IN `0x81` 8 B | keyboard reports sent on host request (`KEY`) |

Reports have no report id. On Windows the output report is 1025 bytes (leading report-id byte 0) and the input
report 513 bytes; with HidSharp / `HidStream.Write` pass a 1025-byte buffer whose first byte is 0.

## 2. Commands (host -> device)

Every OUT report is **1024 bytes**. A command report starts with `43 52 54 00 00` (`CRT\0\0`), the ASCII command name
follows at byte 5, parameters at byte 8 or later. The rest is zero. Unknown reports are ignored.

| Command | Bytes 5.. | Parameters | Effect |
|---|---|---|---|
| `DIS` | `44 49 53` | - | connect / wake: starts the watchdog, flips the frame-buffer page and clears it, host state = connected, backlight on |
| `HAN` | `48 41 4e` | - | sleep: backlight off (LED brightness saved) |
| `LIG` | `4c 49 47` | byte 10: level 0..60 | backlight level (values above 60 are clamped) |
| `CLE` | `43 4c 45` | byte 11: `0x43` ('C'), `0xff` or key 1..15 | `'C'`: clear the page **and disconnect** (watchdog stops, host state = off, the boot logo is shown again); `0xff`: flip the page and clear it; key: clear that tile |
| `STP` | `53 54 50` | - | make the page that was drawn visible (only after a `CLE`) |
| `CON` | `43 4f 4e` | - | accepted, no effect |
| `KEY` | `4b 45 59` | bytes 10..17: HID keyboard report | sent on endpoint 0x81 |
| `APPNEW` | `41 50 50 4e 45 57` | byte 11: `0xff` = BootROM upgrade mode (reboot reason 4), anything else = HID bootloader (reason 6) | reboot into the bootloader (HID bootloader `33C3:8899`, BootROM `33C3:6677`) |
| `SETLB` | `53 45 54 4c 42` | bytes 10..81: 24 x RGB | LED colours (stored; the LED output is unused on this board, see hardware.md) |
| `LBLIG` | `4c 42 4c 49 47` | byte 10: brightness | LED brightness |
| `QUCMD` | `51 55 43 4d 44` | byte 10: `0x11`, `0x1f` or `0xff` | LED mode |
| `DELED` | `44 45 4c 45 44` | - | LEDs off |
| `BAT` | `42 41 54` | upload header, see below | key image |
| `LOG` | `4c 4f 47` | upload header, see below | boot logo |
| `BGPIC` / `BGCLE` | `42 47 50 49 43` / `42 47 43 4c 45` | `BGPIC`: upload header | background picture: **received and ignored** (also by the stock firmware) |
| `SELFT` | `53 45 4c 46 54` | - | enter the standalone hardware selftest mode (see [selftest.md](selftest.md)); **not in the stock firmware** |

Typical session: `DIS`, `LIG n`, then for every key `BAT` + payload, then `STP`; later `CLE <key>` + `STP` to change a
tile, `HAN` to sleep, `CLE C` to disconnect.

### Watchdog

`DIS` starts a hardware watchdog (20 s) that a 15 s timer feeds **only if the host sent at least one report since the
previous tick**. A connected host must therefore send something (any command, `CON` is the cheapest) at least every
15 s, otherwise the device resets itself (this is deliberate in the stock firmware: a dead host reboots the dock).
`CLE C` stops the watchdog.

## 3. Uploads (`BAT`, `LOG`, `BGPIC`)

An upload is one **header report** followed by the payload in 1024-byte reports (the last one zero padded).

| Command | Header fields | Payload |
|---|---|---|
| `BAT` | bytes 10..11: payload length, big endian u16; byte 12: key number 1..15 | JPEG or PNG picture of one key |
| `LOG` | bytes 8..11: payload length, big endian u32 | JPEG or PNG (800x480 recommended, 512 KiB at most) |
| `BGPIC` | bytes 10..13: payload length, big endian u32 | ignored |

The firmware counts reports; the upload is complete when `received >= length + 1024`. It is **accepted** only if the
payload ends with a JPEG end-of-image (`FF D9`) or a PNG `IEND` chunk; otherwise the whole upload is dropped silently.
The total (header + padded payload) must stay below 512 KiB.

* `BAT`: the picture is decoded and drawn into the tile of the key on the current page; call `STP` afterwards.
  Key numbers map to tiles through a table of the stock firmware (keys 1..4 to tiles 4..1, key 5 skipped, keys 6..10
  to 10..6, keys 11..15 to 15..11); tiles are 112x112 pixels at pitch 169 x 174. Keys 1..3 take a 182 pixel wide image.
* `LOG`: the payload is written to `/data/logo_jpg_dir/test_logo.jpg`, an ACK frame (all fields zero) is sent and the
  logo is drawn again. The file is read at every boot.

## 4. Events (device -> host)

Every IN report is **512 bytes** and starts with the signature `41 43 4b 00 00 4f 4b` (`ACK\0\0OK`). Byte 9 is the
code, byte 10 the state. Events are only sent while a host is connected (after `DIS`).

| Source | Code (byte 9) | State (byte 10) | Other bytes |
|---|---|---|---|
| LCD keys (matrix, 2 rows of 5) | 1..10 | 1 = pressed, 0 = released | - |
| Encoder 1 / 2 / 3 / 4 turned | counter-clockwise `0x50 / 0x90 / 0xa0 / 0x70`, clockwise = code + 1 | 0 | - |
| Encoder push buttons | `0x35 / 0x33 / 0x37 / 0x36` | 1 pressed, 0 released | - |
| Touch strip tap / long press | tile `0x40 - 0x43`, or `0x38` outside the tiles | 1 then 0 | bytes 11/12: x, 13/14: y of the landing point (big endian). A tap sends both frames one after the other when the finger is lifted; a finger that rests for 100 ms sends the 1 frame at once, the 0 frame follows when it is lifted |
| Touch strip swipe (after the up frame) | `0x39` horizontal, `0xb2` down, `0xb1` up | 1 | byte 11: direction (1 left, 2 right, 3 up, 4 down), bytes 12/13: dx and 14/15: dy from the landing point to the last point, signed 16 bit big endian (not in stock, zero there) |
| Upload finished (`LOG`) | all zero ACK | - | - |

The keys and encoder pushes use down and up events so a host can detect long presses by timing. Without a host
(before `DIS`, or after `CLE C`) the keys only switch the backlight on and off.

## 5. Differences from the stock firmware (read this if a plugin or host program depends on stock behaviour)

Commands, uploads, the ACK frame layout, the key codes and the key press / release events are **identical** to the stock
firmware. What changed is limited to the encoder push buttons, the touch strip and how events are delivered. Software
written against the original dock may react differently; the table says how and what to adapt.

| # | Item | Stock firmware | This firmware | What may break, how to adapt |
|---|---|---|---|---|
| 1 | Encoder push (codes `0x35 0x33 0x37 0x36`) | one frame, state 1, when pressed; no release frame | state 1 on press **and** state 0 on release (10 ms debounce) | A plugin that runs its action on *every* frame with that code now fires twice per click. Act only on state 1 (click) or time 1 -> 0 (long press) |
| 2 | Encoder rotation while a push button is held | not reported: the stock code blocks scanning until all buttons are released | reported as usual (push-and-turn works) | A plugin that never saw rotation during a push now gets rotation frames between the down and the up frame |
| 3 | Touch strip, tap / long press | nothing at landing; after the finger left a fake pair: state 1, then state 0 about 50 ms later | tap: state 1 and state 0 one after the other when the finger is lifted; a finger resting for 100 ms sends state 1 at once and state 0 when it is lifted; code = tile `0x40 - 0x43` or `0x38` outside the tiles, landing point in bytes 11/12 (x) and 13/14 (y), big endian | Plugins that act on state 1 see the same single press as before for a tap; a long press can now be timed from 1 to 0 |
| 4 | Touch strip, finger leaves | see 3 | state 0, same code and position as the down frame | The press now lasts as long as the finger rested (long press), a tap is the pair back to back |
| 5 | Swipe | single frame (`0x39` horizontal, `0xb2` down, `0xb1` up), state 1, no tile press | the same single frame, **no tile down / up pair** (unless the finger rested on a tile for 100 ms before it moved: then the press and its release come first) | A swipe does not press the tile it starts on, as in stock |
| 5a | Swipe direction | the frame has only the code; a horizontal swipe is `0x39` whether it goes left or right | same code and state, plus byte 11 = direction (1 left, 2 right, 3 up, 4 down) and signed dx / dy in bytes 12 - 15; left / right = towards lower / higher touch x, i.e. towards tile `0x40` / `0x43` | Hosts that ignore bytes 11 - 15 see exactly the stock frame; a host that derives the direction itself from the landing point can keep doing so |
| 6 | Swipe detection | the stock state machine | the stock thresholds (14 px horizontal, 29 px vertical) with a simplified classification | Borderline and diagonal gestures can be classified differently (not compared on a real stock dock) |
| 7 | Event delivery | each thread sent its own frame; a busy IN endpoint dropped it silently | one 64 entry queue, frames leave in order, one at a time; a full queue drops the new event | No more lost or corrupted frames under bursts; under extreme load events are delayed instead (a slow host reads them late, in order) |
| 8 | Events while no host is connected | not sent | not sent (same); frames still queued are discarded on a bus reset | none |
| 9 | Boot screen | the logo, or a dark screen without a logo file | the logo (or black) plus a strip with the firmware version and build time at the bottom, until the first host command that clears the screen | none for protocol users; the strip is visible if a plugin never clears the screen |
| 10 | USB | - | enumeration watchdog (re-attach after 5 s unconfigured) and filters for two defects of the CherryUSB port (see [troubleshooting.md](troubleshooting.md)) | none; after a bus reset the device re-enumerates faster and more reliably |
| 11 | `SELFT` command | not present | `CRT SELFT` enters the standalone selftest mode; the device stops sending HID events for the duration and exits selftest on the same command or on the L+R encoder hold; see [selftest.md](selftest.md) | no existing plugin sends this command, so no breakage; a host that monitors `g_host_state` will see no events while selftest is active |

Not changed, so existing assumptions stay valid: matrix key codes and states, rotation codes (counter-clockwise, clockwise
= code + 1, state 0), `LOG` acknowledgement, `BAT` / `STP` / `CLE` / `DIS` / `HAN` semantics, the 15 s host watchdog.

Not compared on a real stock dock: items 3, 5, 5a and 6 are based on the disassembly of the stock touch code, the behaviour of a
swipe that starts on a tile in the stock firmware was not measured.

## 6. Trying it

```
akp05_tool.exe hid-list
akp05_tool.exe hid-talk --cmd DIS --cmd LIG:30 --listen 20     # then press keys, turn knobs, touch the strip
akp05_tool.exe logo my_logo.jpg                                 # LOG upload
```

Close every other program that has the device open first: HidSharp / hidapi keep the handle and the next writer
blocks until the 3 s timeout.
