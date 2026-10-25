# Releases

> **Use at your own risk.** A release is an unofficial build; flashing it can brick a device. The authors accept no
> responsibility or liability for bricked or damaged devices.

Releases are **automatic**: pushing a tag `v<version>` runs `.github/workflows/release.yml`, which builds everything from
that commit on GitHub and creates the release. Nothing has to be uploaded by hand.

## Making a release

1. Set the version at the top of `CMakeLists.txt`: `set(AKP05_FW_VERSION "1.0.0")`. It is shown on the boot screen
   and must match the tag.
2. Update the documentation that describes behaviour (`docs/testing.md`, `docs/host-protocol.md` section 5 if a host can
   see a difference), commit and push to `main`. Check that the `ci` workflow is green.
3. Tag and push:
   ```
   git tag v1.0.0
   git push origin v1.0.0
   ```
   A tag with a hyphen (`v1.1.0-rc1`) creates a **pre-release**.
4. The `release` workflow (Actions tab) checks that the tag is `v` + `AKP05_FW_VERSION`, runs the host tool tests, builds
   the firmware and the tool, makes the test kit and publishes the release with generated release notes. It takes
   several minutes; the SDK is cached after the first run.

If a step fails nothing is published. Fix the cause, then move the tag (`git tag -d v1.0.0`,
`git push origin :refs/tags/v1.0.0`, tag again) or use the next version number. A release that was published by mistake
is deleted on the GitHub release page (and its tag with `git push origin :refs/tags/<tag>`).

## What is in a release

| File | Content |
|---|---|
| `d13x_os.itb` | the firmware, the `os` partition |
| `rodata.fatfs`, `data.lfs`, `env.bin` | the other partitions (`data.lfs` has no boot logo: the logo is private and git-ignored) |
| `akp05_tool.exe` | the Windows host tool |
| `akp05_test_kit.zip` | tool + firmware + numbered launchers + `README.txt`, without the unpacked folder |
| `SHA256SUMS.txt` | SHA-256 of the files above |

The release is built **from the repository only**. It never contains a dump of a device, the vendor updater image or
any other manufacturer file: the kit has no `backup_original\` and no `updater\` folder. A user makes their own dump
(kit step 08) and, for the BootROM recovery, copies the vendor updater to `updater\default.img`.

## Do not upload a locally built kit

`scripts\make_test_kit.ps1` on a developer machine copies the private dump and the vendor updater from `private\` into
`dist\akp05_test_kit` and its zip. That kit is for your own test PC and must **never** be attached to a release or sent
to anyone. This is why the release is built by the workflow and not from `dist\`.

## Status

The workflow follows the same steps as the `ci` workflow's firmware job and the local scripts, but neither has run on
GitHub yet (see `docs/testing.md`). Run it first with a pre-release tag (`v1.0.0-rc1`, after setting
`AKP05_FW_VERSION` to `1.0.0-rc1`) and delete that release when the result looks right.
