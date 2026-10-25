# Documentation index

> **Use at your own risk.** Nothing in this repository comes with a warranty. Flashing can brick the device; the authors
> accept no responsibility or liability for bricked or damaged devices or any other consequence.

| Document | Read it to learn |
|---|---|
| [architecture.md](architecture.md) | how the firmware is composed (SDK + board + application), threads, data flow, display, memory, image packing |
| [hardware.md](hardware.md) | the board: SoC, flash layout, pin map, keys / encoders / touch, UART pads, memory map |
| [boot-and-recovery.md](boot-and-recovery.md) | the boot chain, reboot reasons, the PA.0 strap, the FIT the bootloader accepts, **how to revive a dead device** |
| [host-protocol.md](host-protocol.md) | the USB HID protocol of the running firmware: commands, uploads, events |
| [bootloader-protocol.md](bootloader-protocol.md) | the ArtInChip upgrade protocol used to dump and flash (BootROM, updater, HID bootloader) |
| [build-system.md](build-system.md) | CMake build, SDK handling, the generated source list, the defconfig rules, CI |
| [host-tool.md](host-tool.md) | the Windows tool `akp05_tool`: commands, safety rules, test kit |
| [stock-firmware.md](stock-firmware.md) | what is known about the original firmware, the address map of every rebuilt function, how it was analysed |
| [releasing.md](releasing.md) | how a release is made (tag `v<version>`), what it contains, what must never be uploaded |
| [testing.md](testing.md) | automated tests, what is verified on hardware, what is not, a checklist for new builds |
| [troubleshooting.md](troubleshooting.md) | symptom -> cause -> fix, for every problem met during the bring-up |

Reading order for a newcomer: README.md (root) -> architecture.md -> hardware.md -> boot-and-recovery.md -> the document
for the part you work on. For an AI assistant: CLAUDE.md in the repository root.
