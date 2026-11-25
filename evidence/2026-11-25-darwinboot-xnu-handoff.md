# 2026-11-25 — Physical Device DarwinBoot -> XNU Kernel Handoff Verified (Kenzo `d6a8a3b1`)

## Summary
Temporary RAM-only `fastboot boot` of `kenzo-xnu-klzm-boot.img` (`7,182,336` bytes) on physical **Xiaomi Redmi Note 3 (`kenzo`, serial `d6a8a3b1`)** completed the entire UEFI -> LZMA initrd decompression -> FAT32 mount -> `UniXNU.efi` -> **`DarwinBoot` -> `ExitBootServices()` -> XNU `kernel.macho` branch** handoff!

## Key Hardware Constraint Discovered & Solved
- Stock Kenzo LittleKernel `aboot` ignores `hdr->ramdisk_addr` and forces `ABOOT_FORCE_RAMDISK_ADDR = 0x82000000` (only 16 MiB before `splash_region@83000000` and XPU-protected `0x86300000+` carveouts). Passing an uncompressed 64 MiB ramdisk triggered an immediate XPU fault into Qualcomm EDL 9008.
- **Solution (`KenzoInitrdDecompressDxe`, `CodeDevB7/uefi-kenzo` @ `3fd164b`, CI run [45543315330](https://github.com/CodeDevB7/uefi-kenzo/actions/runs/45543315330))**:
  - Compressed the 64 MiB FAT32 volume (`EFI/UniXNU/unixnu.toml`, `Payloads/kernel.macho` 51.9 MiB, `Payloads/kernel.macho.adt` 12.8 KB) with strict known-size LZMA1 (`KLZM` magic + 13-byte header) -> **`6.26 MiB`** (fits comfortably in `0x82000000..0x82640000`).
  - `KenzoInitrdDecompressDxe` (in `APRIORI DXE`, before `UniXNURamDiskDxe`) decompresses the 64 MiB FAT32 volume into protected high DRAM `0xA0000000..0xAFFFFFFF` (`EfiBootServicesData`) and patches `/chosen/linux,initrd-{start,end}` in-place.

## Verified Inputs & Command
- **UEFI**: `CodeDevB7/uefi-kenzo` @ `3fd164b` (CI run [45543315330](https://github.com/CodeDevB7/uefi-kenzo/actions/runs/45543315330))
- **Kernel**: `kenzo-kernel-ipados-17.7.11-21H461` (`kernel.macho`, `51,933,184` bytes, CI run [43171449006](https://github.com/CodeDevB7/UniXNU-kenzo/actions/runs/43171449006))
- **ADT**: `kenzo-adt-ipados-17.7.11-21H461` (`kenzo.adt`, `12,888` bytes, CI run [45026987643](https://github.com/CodeDevB7/UniXNU-kenzo/actions/runs/45026987643))
- **Command**:
  ```
  fastboot -s d6a8a3b1 boot artifacts_v9\kenzo-xnu-klzm-boot.img
  Sending 'boot.img' (7014 KB)                       OKAY [  0.236s]
  Booting                                            OKAY [  0.061s]
  Finished. Total time: 0.308s
  ```

## Observed Device Output (Photographed on Physical Screen)
- `UniXNU` `version 0.1.0` loaded `config: \EFI\UniXNU\unixnu.toml` with boot entry `1 UniXNU iPadOS 17.7.11 (xnu)`.
- `DarwinBoot` loaded `kernel.macho` and `kernel.macho.adt` and printed:
  - `video base 0x83200000 1080x1920 row 4320 bytes 32 bpp`
  - `entry words 140006F5 00000000 00000000 00000000`
  - `CurrentEL 1 SCTLR_EL1 0xC0180D TCR_EL1 0x280803518`
  - `ID_AA64MMFR0_EL1 0x1122 (PARange 2, 4 KiB granule supported)`
  - `ID_AA64PFR0_EL1 0x2222`
  - `Exiting boot services. A green bar across the top marks the loader handoff before the kernel branch.`
- **Solid green handoff bar appeared across the top of the screen**, confirming `ExitBootServices()` succeeded and CPU branched into XNU `_start` (`0x140006F5`).

## Next Milestone (XNU Early Kernel Bring-Up)
- Enable UniXNU's `panic_screen = true` / entry probe (`scripts/patch-xnu-entry-probe.py`) in `board.toml` `[payload]` so XNU draws early stage bars and on-screen verbose/panic output directly on the `0x83200000` 32-bit BGRX8888 framebuffer after `_start`.
