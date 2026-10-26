# Standalone hardware selftest

The selftest mode runs a set of hardware diagnostics directly on the device — no host PC required.
Code: `firmware/app/selftest.c`, `firmware/app/selftest.h`.

## Entry and exit

| Method | Condition | Action |
|---|---|---|
| Hold left + right encoder buttons for **3 s** | idle / logo screen (no host connected) | enters selftest |
| Hold left + right encoder buttons for **3 s** | selftest active | exits selftest, logo shown |
| UART MSH command `selftest` | any | toggles selftest |
| Host command `CRT SELFT` (`CMD_SELFTEST = 17`) | running firmware | enters selftest (see [host-protocol.md](host-protocol.md)) |

Left encoder = code `0x37` (PE.11), right encoder = code `0x36` (PB.11).

## Menu

On entry a menu is shown. The screen uses visual coordinates: `(0, 0)` is the top-left of the
visible panel (the panel is mounted 180° rotated relative to the frame buffer;
`display_fill` / `display_text` convert internally).

```
[ BUTTONS ] [ DISPLAY ] [ TOUCH ] [ ENCODERS ]   ← physical top row, keys 1–4
[         ] [         ] [       ] [          ] [ EXIT ]   ← middle row, key 10
```

Key 10 (middle row, rightmost) exits selftest and restores the logo screen.

## Tests

### BUTTONS (key 1)

Tests all 14 physical inputs: matrix keys 1–10 and encoder push buttons E1–E4.

- Each button starts dark grey; turns bright green on first press, dark green when released.
- Pressing key 10 is also tested here (it is **not** an exit key in this subtest).
- **Exit**: tap touch strip tile 4 (the rightmost sector, shown as a red EXIT tile next to the encoder buttons).
- The footer shows `TESTED: N / 14`.

Physical encoder button codes (left → right): `0x37`, `0x35`, `0x33`, `0x36`.

### DISPLAY (key 2)

Cycles through full-screen test patterns automatically (~2 s per phase):

1. Red fill
2. Green fill
3. Blue fill
4. White fill
5. Eight vertical colour bars

After the last phase the menu is shown again.

### TOUCH (key 3)

Tests the I²C touch strip below the key matrix.

- Four strip tiles (T1–T4, codes `0x40`–`0x43`) are shown at the bottom of the screen; the active tile lights up green while a finger rests on it.
- Swipes are shown as text in the footer: `SWIPE LEFT`, `SWIPE RIGHT`, `SWIPE UP`, `SWIPE DOWN`.
- **Exit**: key 10.

Tile positions on the touch sensor (x range 0–800): T1 `5–181`, T2 `204–380`, T3 `403–579`, T4 `602–778`.

### ENCODERS (key 4)

Tests the four rotary encoders.

- Each encoder shows its cumulative rotation count (positive = clockwise) and a button indicator.
- `BTN?` → button not yet pressed; `PRESS` → currently held; `OK` → pressed at least once.
- **Exit**: key 10.

Encoder rotation codes (CCW / CW): E1 `0xa0/0xa1`, E2 `0x50/0x51`, E3 `0x90/0x91`, E4 `0x70/0x71`.

## Display implementation notes

`display_fill(vx, vy, vw, vh, argb)` and `display_text(vx, vy, s, scale, rgb565)` both accept **visual
coordinates** and are the only drawing functions used by the selftest. They convert to buffer coordinates
internally (`buf_x = LCD_W - vx - vw`, `buf_y = LCD_H - vy - vh`).

`display_text` calls `aicos_dcache_clean_invalid_range` before and after `banner_line_ex` so that
CPU-written pixels are visible to the display DMA after GE fills (which bypass the CPU cache).

Every render function toggles `g_clear_toggle` before drawing so all output goes to the back frame
buffer page; `display_flush(g_clear_toggle)` makes it visible when the frame is complete. This
prevents visible tearing during screen updates.

## Selftest from a host

```powershell
akp05_tool.exe selftest          # send CRT SELFT; device enters selftest
```

The device does not send ACK frames during selftest. The host tool just fires the packet and returns.
