# Kenzo UEFI firmware research

- Date: 2026-11-12
- Target: Xiaomi Redmi Note 3 (`kenzo`), Qualcomm MSM8956 / Snapdragon 650.
- Scope: locate a reusable Kenzo/MSM8956 UEFI port or a close, verifiable platform baseline. No firmware was built and no boot image or device partition was written.

## Findings

- No Kenzo device configuration or MSM8956 board package was found in the checked public Qualcomm EDK2/Mu-Silicium sources. The current `edk2-porting/edk2-msm` tree has device configs for Xiaomi `jason` and `nitrogen`, but both set `SOC_PLATFORM="SDM660"`; they are not MSM8956 ports. The current Mu-Silicium tree has a Xiaomi `nitrogenPkg` and `nitrogen.toml`, but no Kenzo configuration. Its nitrogen FD base `0x9FC00000`, FD size `0x300000` and Android boot-image header version 1 are platform-specific and must not be copied to Kenzo.
- The Renegade supported-device page lists selected Qualcomm families including MSM8992, MSM8994 and newer device ports, but no MSM8956 device. The `edk2-msm` issue tracker has an open request for MSM8996 (Xiaomi Mi 5); this is evidence that even the adjacent older Qualcomm generation lacks a ready config, not proof about every possible private port.
- `lk2nd` maps the `lk2nd-msm8952` target to MSM8956, so it is a possible Linux bring-up reference. It is a second-stage LK bootloader, not the Mu-Silicium/EDK2 firmware interface required by this UniXNU board package. Its MSM8956 Sony Xperia X Compact issue reports `fastboot boot` returning to Android and a boot loop after `fastboot flash`; this is a cautionary report, not a Kenzo test. No image was built or flashed here.
- The local Kenzo dump identifies Android partitions such as `sbl1`, `aboot`, `hyp` and `tz`, but contains no extracted, revision-identified EDK2/Mu-Silicium checkout or complete bootloader input set. The 64 MiB `boot.img` is an Android boot image with an older header and `dt_size = 0`; it is not a UEFI FD. The captured Linux `/proc/iomem` and `MemTotal` describe one running Android kernel, not a complete firmware handoff or UEFI memory map.
- `board.toml` still names the template-like `Platforms/Xiaomi/kenzoPkg/kenzo.dsc` and `Build/kenzoPkg/.../SILICIUM_UEFI.fd` paths. Neither path is backed by an existing local firmware checkout, source revision, board package, build, or measured FD. Those values remain placeholders.

## Assessment

No public, source-backed Kenzo/MSM8956 UEFI baseline was identified. Adapting the SDM660 Xiaomi port or using an unrelated SoC's FD addresses would be unsupported. A real Kenzo firmware port remains new board bring-up work: identify a compatible Qualcomm boot-chain entry, source and pin required vendor inputs, implement the board package, and verify memory reservations plus serial/display/storage services. The device's exact bootloader partitions and matching Kenzo DTB are still needed to ground that work.

No firmware values in `board.toml` were changed because the research did not provide Kenzo-specific values.

## Sources checked

- `edk2-porting/edk2-msm`, device config directory at commit `e1952621f419f8db60ed28271264e1b5184c571d`: https://github.com/edk2-porting/edk2-msm/tree/e1952621f419f8db60ed28271264e1b5184c571d/configs/devices
- Xiaomi SDM660 `jason.conf`: https://github.com/edk2-porting/edk2-msm/blob/e1952621f419f8db60ed28271264e1b5184c571d/configs/devices/jason.conf
- Xiaomi SDM660 `nitrogen.conf`: https://github.com/edk2-porting/edk2-msm/blob/e1952621f419f8db60ed28271264e1b5184c571d/configs/devices/nitrogen.conf
- Mu-Silicium tree at commit `46e4ac0800862fb3ee6da09f0028fbef4e767648`: https://github.com/Project-Silicium/Mu-Silicium/tree/46e4ac0800862fb3ee6da09f0028fbef4e767648/Platforms/Xiaomi
- Mu-Silicium nitrogen config: https://github.com/Project-Silicium/Mu-Silicium/blob/46e4ac0800862fb3ee6da09f0028fbef4e767648/Resources/Configs/nitrogen.toml
- Renegade supported devices: https://renegade-project.tech/en/devices
- Open MSM8996 support request: https://github.com/edk2-porting/edk2-msm/issues/388
- `lk2nd` target-to-chipset mapping at commit `cf45aefd22d744b2bf9b39455afae697ce430063`: https://github.com/msm8916-mainline/lk2nd/blob/cf45aefd22d744b2bf9b39455afae697ce430063/Documentation/targets.md
- Open MSM8956 Xperia X Compact boot issue: https://github.com/msm8916-mainline/lk2nd/issues/563
- UniXNU's firmware integration requirements: https://unixnu.com/porting/uefi-firmware

## Next action

Obtain a revision-identified Kenzo stock firmware/bootloader input set and matching Kenzo DTB, then determine whether a supported first-stage boot path can launch a new UEFI board package. Keep all firmware and boot-image builds on GitHub Actions. Do not use `fastboot flash` for exploration.
