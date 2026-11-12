# UniXNU Kenzo

Initial format 2 device port for the Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650).

The current milestone includes configuration/patch preflight, a successful GitHub Actions DTS-to-ADT build, and a successful macOS GitHub Actions kernel build for iPadOS 17.7.11. The connected device's live Android FDT now grounds the MSM8956 selector, CPU MPIDRs, GIC, UART and 2 GiB RAM window. UEFI firmware, payload and boot-image creation, and XNU device boot have not been verified.

## Check the current configuration

From this repository, with the UniXNU checkout beside it:

```sh
bash scripts/package.sh --check --os ipados-17.7.11-21H461
```

The command resolves dependencies, extracts the selected profile's IPSW inputs and checks configuration and patch resolution. See [the porting record](porting.toml) and [the latest preflight evidence](evidence/2026-11-12-package-check.md) for the exact revisions and limits.

The **Kenzo DTS and ADT build** workflow compiles the board tree and generates an ADT on a macOS runner when DTS, feed or board inputs change. It uploads `kenzo.adt` for inspection. To compile the kernel on GitHub Actions, run **Kenzo macOS kernel build** under Actions. It builds only the kernel, uploads the `kernel.macho` artifact and stores redacted text logs. It does not upload IPSW inputs.

## Outstanding work

- Research found no public Kenzo/MSM8956 Mu-Silicium/EDK2 board package. The live Android FDT matches extracted QCDT DTB 0 at its MSM8956 MTP selector, and the boot-chain partition hashes are recorded, but SBL1/aboot/HYP release tags and a UEFI entry path remain unknown. The current DSC/FD paths are placeholders.
- Reconcile the Android kernel memory map with UEFI-specific reservations and handoff before changing payload memory values.
- Complete the board component selection and ADT inputs using measured hardware values.
- Build kernel, firmware, payload and boot image, then record hardware results individually.

The user reports repeatedly booting `boot.img` and `recovery.img` with `fastboot boot` on this device. The observed LK `aboot` binary also contains the message `fastboot boot command is not available`, so that string alone does not describe the device's actual behavior. A UEFI payload still needs a compatible boot-image wrapper and validation. Do not use `fastboot flash` for exploration.
