# 2026-11-25 — Physical Device UEFI & UniXNU Boot Menu Verification (Kenzo `d6a8a3b1`)

## Summary
Temporary RAM-only `fastboot boot` of `kenzo-uefi-only-boot.img` built from `CodeDevB7/uefi-kenzo` commit `dc84ec0fd17f0429b06bda01b123898beeb5b862` (GitHub Actions run [45539339069](https://github.com/CodeDevB7/uefi-kenzo/actions/runs/45539339069)) booted all the way through `PeilessSec`, `ArmConfigureMmu`, `DxeCore`, `KenzoSimpleFbDxe`, `ArmCpuDxe`, `ArmGicV2Dxe`, `ArmTimerDxe`, `BdsDxe`, `UniXNUDxe`, `UniXNURamDiskDxe`, and launched **`UniXNU.efi` (`version 0.1.0`)** on physical **Xiaomi Redmi Note 3 (`kenzo`, serial `d6a8a3b1`)** with crisp 1080×1920 32-bit BGRX8888 graphics and working 19.2 MHz real-time countdown.

## Verified Artifacts & Checksums
- **Firmware Repository**: `CodeDevB7/uefi-kenzo` @ `dc84ec0fd17f0429b06bda01b123898beeb5b862`
- **CI Run**: [45539339069](https://github.com/CodeDevB7/uefi-kenzo/actions/runs/45539339069) (`ubuntu-latest`, `CLANGPDB`, `DEBUG`)
- **`SILICIUM_UEFI.fd`**: `3,145,728` bytes (`0x300000`), SHA-256 `a4bc49083e731135d3352bc5f19126f05b72fd9dc531aa00901e1f110ad621ae`
- **`kenzo-uefi-only-boot.img`**: `616,448` bytes, SHA-256 `c6001a3e064ba7e26ff543328bf73fba438dc61312bd8152e6f3a4b0280b942f`

## Command & Output
```
fastboot -s d6a8a3b1 boot artifacts_v7\kenzo-uefi-only-boot.img
Sending 'boot.img' (602 KB)                        OKAY [  0.022s]
Booting                                            OKAY [  0.060s]
Finished. Total time: 0.086s
```
No partitions were flashed (`fastboot boot` only).

## Hardware Bring-Up Root Causes Resolved During Testing
1. **Qualcomm LK `aboot` 32-bit `text_offset` wrap**: Stock `aboot` computes `(kernel_addr & ~0x1fffff) + text_offset` in 32-bit arithmetic. Using `FD_BASE` (`0x9FC00000`) overflowed to `0x1FC00000`. Fixed in `BootShim.S` (`text_offset = 0x80000`, `image_size = 0`, matching stock Kenzo kernel) with 4-byte dummy ramdisk at `0x81000000`.
2. **`TPIDRRO_EL0` TransferList hygiene**: Zeroed `TPIDRRO_EL0` in `BootShim.S` before jumping to `PeilessSec` so `mu_basecore` does not dereference stale non-zero register state as an ARM TransferList.
3. **Splash Framebuffer MMU attribute & MDP5 `RGB0` scanout**: Verified live hardware MDP5 pipe `RGB0` (`0x01A15000`, `addr=0x83200000`, `pitch=0x0CA8` = `1080*3`, `fmt=0x0002243F`, `unpack=0x00020001`). Mapped `0x83000000..0x85800000` as `ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED` (Normal Non-Cacheable) and reconfigured `RGB0` to 32-bit BGRX8888 (`0x000236FF` / `0x03020001` / `1080*4`).
4. **AArch64 DXE Exception/IRQ Vector Table**: Switched `CpuExceptionHandlerLib` for `DXE_CORE` and `DXE_DRIVER` from `CpuExceptionHandlerLibNull` to `UefiCpuPkg/Library/CpuExceptionHandlerLib/DxeCpuExceptionHandlerLib.inf`.
5. **MSM8956 `CNTFRQ_EL0 == 0` Timer Infinite Loop**: Confirmed via on-screen `KenzoFbSerialPortLib` debug console that `ArmTimerDxe.efi` hung on the first timer IRQ when `CNTFRQ_EL0` read as `0` (`mTimerTicks == 0`). Added `KenzoArchTimerCounterLib` with fallback to `19,200,000` Hz (verified from `kenzo.dtb` `clock-frequency = <0x0124f800>`). Verified GICv2 PPIs `17/18/19/20` and real-time BDS countdown (`BdsWait(3)..(2)..(1)..`).
6. **`BdsDxe` `BmDriverHealth` FormBrowser2 ASSERT**: Added `MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf` to `kenzo.dsc` and `kenzo.fdf`, allowing `BdsDxe` to launch `Boot0000: UniXNU`.

## Observed Device Result
- Physical Kenzo display rendered the full graphical **UniXNU `version 0.1.0`** boot menu (`built-in configuration`, `1 Return to firmware`, `2 Restart`, `Booting Return to firmware in 1 s`, `no boot entry found`, `drivers 0 loaded / 0 failed kits 0 acpi 0`).
- `fastboot getvar max-download-size` previously confirmed `0x20000000` (512 MiB).

## Next Milestone
- Relocate `ramdisk_offset` in `board.toml` to clean high DRAM (e.g., `0x20000000` -> `0xA0000000`) so the ~192 MiB UniXNU FAT payload does not collide with `splash_region@83000000`.
- Build and test a full UniXNU + iPadOS 17.7.11 (`21H461`) bootable image (`kernel.macho` + `kenzo.adt` + `unixnu.toml` + trust cache + Data ramdisk) via `fastboot boot` to verify `DarwinBoot` -> XNU kernel handoff.
