# UniXNU Kenzo

Initial format 2 device port for the Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650).

The current host-verified milestone includes configuration/patch preflight, standalone DTS-to-ADT generation, and a successful macOS GitHub Actions kernel build for iPadOS 17.7.11. UEFI firmware, payload and boot-image creation, and device boot have not been verified.

## Check the current configuration

From this repository, with the UniXNU checkout beside it:

```sh
bash scripts/package.sh --check --os ipados-17.7.11-21H461
```

The command resolves dependencies, extracts the selected profile's IPSW inputs and checks configuration and patch resolution. See [the porting record](porting.toml) and [the latest preflight evidence](evidence/2026-11-12-package-check.md) for the exact revisions and limits.

To compile the kernel on GitHub Actions, run **Kenzo macOS kernel build** under Actions. It builds only the kernel, uploads the `kernel.macho` artifact and stores redacted text logs. It does not upload IPSW inputs.

## Outstanding work

- Research found no public Kenzo/MSM8956 Mu-Silicium/EDK2 board package. Implement one only after obtaining revision-identified Kenzo bootloader inputs and the matching DTB; the current DSC/FD paths are placeholders.
- Verify the CPU/GIC and complete memory map against the matching Kenzo DTB and firmware handoff.
- Complete the board component selection and ADT inputs using measured hardware values.
- Build kernel, firmware, payload and boot image, then record hardware results individually.

Do not use `fastboot flash` for bring-up. For any physical boot test, use `fastboot boot <image>` and record the tested image and logs.
