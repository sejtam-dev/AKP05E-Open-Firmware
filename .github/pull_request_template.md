## What and why

<!-- What does this change, and why? Link the issue if there is one. -->

## How it was tested

- [ ] Host tool unit tests: `$env:PYTHONPATH="host-tool"; python -m unittest discover -s host-tool/tests`
- [ ] Firmware builds: `scripts\build.ps1`
- [ ] Tried on a real device (say which kit steps or commands, and the firmware version from the boot screen)
- [ ] Not tested on a device (explain why; unverified changes are marked as such in `docs/testing.md`)

## Checklist

- [ ] No private data: no dump (`*.bin`), vendor updater (`*.img`), boot logo or anything from `private/`
- [ ] Nothing can write the `spl` partition
- [ ] Documentation changed together with the code (`docs/host-protocol.md` section 5 if a host can see the difference,
      `docs/troubleshooting.md` for a new pitfall, `docs/testing.md` for verified / unverified)
- [ ] The "use at your own risk" notices are untouched
- [ ] No `goto`; everything in the repository is English
- [ ] If the defconfig changed: `scripts\maintenance\refresh_sdk_sources.ps1` was run and `cmake/sdk_sources.cmake` is committed
