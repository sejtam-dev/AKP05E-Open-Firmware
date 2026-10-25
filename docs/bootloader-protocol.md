# Upgrade protocol (BootROM, updater and HID bootloader)

The protocol the host tool uses to dump and flash the device. It is ArtInChip's "aicupg" protocol; the device side is
in the SDK (`application/baremetal/bootloader/lib/aicupg`, `include/aicupg.h`), the host side is
`host-tool/upg_proto.py` plus `host-tool/akp05_tool.py`. Everything below was verified on a real AKP05E unless it says
otherwise. `host-tool/tests/` contains software models of the device (`FakeDevice`) that implement the same rules.

## 1. Where the protocol is spoken

| State | USB id | Transport | Notes |
|---|---|---|---|
| BootROM upgrade mode | `33C3:6677` | bulk IN / OUT | answers `GET_HWINFO`, `WRITE` and `EXEC` to RAM (the only ones used here); entered with the PA.0 strap |
| Updater loaded into RAM | `33C3:6677` | bulk IN / OUT | the full command set; `boot_stage = 1` |
| HID bootloader of the SPL | `33C3:8899` | HID interrupt IN `0x83` / OUT `0x04`, reports of 1024 bytes | the full command set; entered with `CRT APPNEW`, `upghid` |

HID transport details: a transfer is always padded to whole **1024-byte reports**; on Windows a report is written with a
leading report-id byte (0), so `WriteFile` takes 1025 bytes. A bare 31-byte command block makes `WriteFile` fail.
`host-tool/winhid.py` talks to the HID class directly with ctypes (overlapped I/O), `--transport libusb` uses pyusb.

## 2. Framing

Every operation is a mass-storage-like triple: **CBW**, optional data, **CSW**.

CBW (31 bytes, little endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | signature `USBC` (`0x43425355`) |
| 4 | 4 | tag (incremented per transfer, echoed in the CSW) |
| 8 | 4 | data length |
| 12 | 1 | flags: `0x00` host to device, `0x80` device to host |
| 13 | 1 | LUN = 0 |
| 14 | 1 | **command block length = 1** (see pitfalls) |
| 15 | 1 | opcode: `1` write, `2` read |
| 16 | 15 | zero |

CSW (13 bytes): signature `USBS` (`0x53425355`), tag, residue, status (0 = ok).

One transfer carries at most one 64 KiB slice (`TRANS_DATA_BUFF_SIZE`); larger payloads are split.

## 3. Commands

Commands are 16-byte `UPGC` headers sent in a WRITE transfer, optionally followed by an argument in a second WRITE.
The answer is a 16-byte `UPGR` header read with a READ transfer, then the response data.

Header (little endian): `magic u32` (`UPGC` = `0x43475055`, `UPGR` = `0x52475055`), `protocol u8 = 1`, `version u8 = 1`,
`command u8`, `status u8` (response only), `length u32` (argument or response length), `checksum u32`
= `magic + (status << 24 | command << 16 | version << 8 | protocol) + length` (the host sends status 0).

| Command | Id | Argument | Response | Use |
|---|---|---|---|---|
| `GET_HWINFO` | 0x00 | - | 108 bytes: `HWINFO`, build date of the BootROM / bootloader, chip ids, byte 38 `init_mode`, 39 `curr_mode`, 40 **`boot_stage`** | detect the state (`1` = updater / bootloader running) |
| `WRITE` | 0x02 | `addr u32, length u32` + data | status | BootROM stage: write to RAM |
| `EXEC` | 0x04 | `addr u32` | status | BootROM stage: call the function at `addr` (it returns 0 on success, the PSRAM initialiser returns; the second stage never does) |
| `SET_UPG_CFG` | 0x0A | `u32 32` + 32-byte config, byte 0 = mode | status | select the mode, below |
| `SET_UPG_END` | 0x0B | `u32 32` + 32 zero bytes | status | finish an upgrade |
| `SET_FWC_META` | 0x10 | 512-byte `META` record | status | announce one firmware component (FWC) |
| `GET_BLOCK_SIZE` | 0x11 | - | `u32` | block size of the current medium |
| `SEND_FWC_DATA` | 0x12 | the component data (in slices) | status | transfer |
| `GET_FWC_CRC` | 0x13 | - | `u32` | CRC32 computed by the device |
| `GET_FWC_BURN_RESULT` | 0x14 | - | `u32` | 0 = written |
| `GET_PARTITION_TABLE` | 0x17 | 68-byte `storage_media` (`"spi-nor"`, device id) | the `mtd=` string | read the partition table of the medium |
| `READ_FWC_DATA` | 0x18 | **304-byte** `media_partition` | the partition data | dump |

Modes of `SET_UPG_CFG`: `0` full disk, `1` partition upgrade (flash), `3` dump partition (read), 2 / 4 / 5 are user-id,
forced and frozen burn, unused here.

`META` record (512 bytes): `"META"` at 0, FWC name (64 bytes) at 8, partition(s) (64 bytes, `;`-separated, e.g.
`env;env_r`) at 0x48, then `offset, size, crc32, ram u32` at 0x88..0x97 and the attribute string (64 bytes, e.g.
`mtd;required`) at 0x98.

`media_partition` for `READ_FWC_DATA`: `storage_media` (68 bytes), the partition name at offset 72 (`name[144]`), start (u64)
at 216, size (u64) at 224, attribute string at 240 (`required` for `spl*`, `mtd:required` for every other
partition); total **304 bytes**.

## 4. Sequences

### Dump (read only)

1. `GET_HWINFO` (`boot_stage` must be 1), `GET_PARTITION_TABLE`,
2. `SET_UPG_CFG` mode 3,
3. per partition: `READ_FWC_DATA` with its `media_partition`, then read `size` bytes in slices.

### Flash (partition upgrade)

1. `GET_HWINFO`, `GET_PARTITION_TABLE`; the tool **refuses** to continue if a partition it is about to write is not
   at the stock offset / size (see section 6),
2. `SET_UPG_CFG` mode 1,
3. the **`image.info` component**: a 2048-byte image header (platform, product, version, `media_type = "spi-nor"`) sent
   as a normal FWC named `image.info` with no partition. It selects the medium for the components that follow
   (`ram_fwc.c`: "switch the device type for following FWCs"). Without it a fresh updater writes the data into its RAM
   medium: `Media: RAM(0)`, `GET_BLOCK_SIZE` = 1, CRC and burn result look fine, nothing reaches the flash,
4. per component (`os`, `rodata`, `data`, `env`): `SET_FWC_META`, `GET_BLOCK_SIZE`, `SEND_FWC_DATA`, `GET_FWC_CRC`
   (compared with the local CRC32), `GET_FWC_BURN_RESULT` (must be 0). The device's UART shows
   `Media: SPI_NOR(3)` for these,
5. `SET_UPG_END`,
6. verification: `SET_UPG_CFG` mode 3 and read every written partition back; the tool compares byte for byte.

### Loading the updater through the BootROM

`load-updater --img updater\default.img` reads the vendor image (`AIC.FW` header, `META` records from offset 0x800,
512 bytes each: `image.updater.psram` (`0x30044000`, attribute `run`), `image.updater.spl` (`0x40100000`, `run`),
`image.info`, `image.target.*`), then for each `run` item: `WRITE` the data to its RAM address and `EXEC` the address
(offset 0). The first stage initialises the PSRAM and returns, the BootROM answers again, the second stage is the
bootloader; its `EXEC` never answers because the device re-enumerates (still as `33C3:6677`, now with the full command
set).

## 5. Pitfalls found on the real device

| Symptom | Cause |
|---|---|
| writes time out, the device ignores data | CBW byte 14 (command block length) must be **1**, not 0 |
| `READ_FWC_DATA` fails with CSW status 1 | the argument must be exactly `sizeof(struct media_partition)` of the device = **304** bytes (184 bytes as sent by a 32-bit vendor tool, or 512 bytes, are rejected) |
| the first flash "succeeds" but the read-back differs, the second flash works | the medium was still the RAM one: send `image.info` first (section 4) |
| `EXEC` at `+0x108` or `+0x100` hangs the BootROM | execute at offset **0**: the BootROM evidently handles the image header itself |
| a write through the vendor updater lands at the wrong offset | its table has `rodata 5m / data 5m`; only partitions at identical offsets are safe (section 6) |

## 6. The layout check

`akp05_tool.py flash` compares the partition table the device reports with the expected stock layout
(`EXPECTED_LAYOUT` in `akp05_tool.py`: spl 0 / 512k, env 0x80000 / 128k, env_r 0xA0000 / 128k, os 0xC0000 / 2m,
rodata 0x2C0000 / 1m, data 0x3C0000 / 10m) and refuses to write any partition whose offset or size differs. Through the
vendor updater therefore only `os` and `env` can be written; through the SPL's HID bootloader (real table) all four.
