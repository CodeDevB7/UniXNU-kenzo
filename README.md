# UniXNU Kenzo

Initial format 2 device port for the Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650).

The current host-verified milestone includes configuration/patch preflight and standalone DTS-to-ADT generation for iPadOS 17.7.11. Kernel compilation, UEFI firmware, payload and boot-image creation, and device boot have not been verified.

## Check the current configuration

From this repository, with the UniXNU checkout beside it:

```sh
bash scripts/package.sh --check --os ipados-17.7.11-21H461
```

The command resolves dependencies, extracts the selected profile's IPSW inputs and checks configuration and patch resolution. See [the porting record](porting.toml) and [the latest preflight evidence](evidence/2026-11-12-package-check.md) for the exact revisions and limits.

## Outstanding work

- Identify and pin a Kenzo Mu-Silicium/EDK2 firmware port; replace the provisional DSC and FD paths.
- Verify the CPU/GIC and complete memory map against the matching Kenzo DTB and firmware handoff.
- Complete the board component selection and ADT inputs using measured hardware values.
- Build kernel, firmware, payload and boot image, then record hardware results individually.

Do not use `fastboot flash` for bring-up. For any physical boot test, use `fastboot boot <image>` and record the tested image and logs.
