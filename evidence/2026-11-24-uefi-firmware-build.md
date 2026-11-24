# Kenzo UEFI Firmware Build & Boot Image Verification

- Date: 2026-11-24 (build executed 2026-11-23 UTC)
- Target: Xiaomi Redmi Note 3 (`kenzo`), Qualcomm MSM8956 (Snapdragon 650).
- GitHub Actions Run: [45101031181](https://github.com/CodeDevB7/uefi-kenzo/actions/runs/45101031181) (`ubuntu-latest`, `CLANGPDB`, `AARCH64`, `DEBUG`, status: `success`).

---

## 1. Source Revisions Built

- `CodeDevB7/uefi-kenzo`: `61f5b82d8e634c635687d0800ec0f1e99e1adcbb`
- `CodeDevB7/UniXNU-kenzo`: `6be5969` (main)
- `UniXNU/UniXNU`: `c34f018aafd6befd5dc93c05a8dccd5bdce6724b` (`drivers/DarwinBoot` initialized)
- `microsoft/mu_basecore`: `cfb93724a06ff86ff5ba971413368a7809ee42a5` (`release/202608`, managed by `unixnu`)

---

## 2. Verified Build Outputs

Commands executed in CI:
```sh
./unixnu defconfig kenzo_defconfig -D ../device
./unixnu build firmware -D ../device --uefi ../uefi-kenzo --target DEBUG -j "$(nproc)"
./unixnu bootimg -D ../device --uefi ../uefi-kenzo --dtb ../uefi-kenzo/Resources/DTBs/kenzo.dtb --ramdisk none --target DEBUG -o ../kenzo-uefi-only-boot.img
```

| Artifact | Size | SHA-256 |
|---|---:|---|
| `SILICIUM_UEFI.fd` | `3,145,728` bytes (`0x300000`, 3.0 MiB) | `b8e8431502c4d5a6bd03bbfec352052ec3ad223f03a03023baad1308c0c2f414` |
| `kenzo-uefi-only-boot.img` | `507,904` bytes (496 KiB) | `7b3e28d032c51335766d214c6d7221233f26a2333695cfc3b72cf050f1fb91e3` |

Both artifacts were downloaded locally to `artifacts_new/` and their SHA-256 digests match the CI log identically. `unixnu bootimg` round-trip unpack verification (`bootimg.py verify()`) passed.

---

## 3. Verified Technical Findings Implemented

1. **Pure `mu_basecore` (`release/202608`) `PeilessSec` Architecture**:
   - MSM8956 uses Qualcomm Little Kernel (`aboot`), not XBL UEFI blobs. `kenzoPkg` builds directly against `microsoft/mu_basecore` (`PeilessSec`, `ArmGicV2Dxe`, `TimerDxe`, `CpuDxe`, `ArmPsciResetSystemLib`, `Fat.inf`, `DiskIoDxe`) plus `UniXNUPkg` and `DarwinBootPkg`.
   - `KenzoPlatformLib` saves incoming `x0` (FDT pointer from `aboot`) in `ArmPlatformPeiBootAction` and publishes `mEntryArgHobGuid` (`DAEE82E6-8090-4E72-BFB9-78C13CD4E359`) for `UniXNURamDiskDxe`.
   - Carveouts (`other_ext` `0x86300000`, `modem` `0x86C00000`, `reloc` `0x8C200000`, `tzapp/QSEECom` `0x8DD00000`) are declared `EFI_RESOURCE_MEMORY_RESERVED` and left unmapped in the ARM64 MMU page tables.
   - `KenzoPlatformLib` exposes Kenzo's 6 verified live-FDT MPIDRs: `0x0, 0x1, 0x2, 0x3, 0x100, 0x103`.

2. **MSM8956 GICv2 Architectural Timer PPIs**:
   - Verified from `kenzo-dump/interrupts.txt` (`20: ... GIC arch_timer`) and `UniXNU-msm8956-common/dts/msm8956.dtsi` (`<1 2>, <1 3>, <1 4>, <1 1>`):
     - `PcdArmArchTimerHypIntrNum = 17`
     - `PcdArmArchTimerSecIntrNum = 18`
     - `PcdArmArchTimerIntrNum = 19`
     - `PcdArmArchTimerVirtIntrNum = 20`
   - Linked with `ArmGenericTimerVirtCounterLib` for EL1 virtual timer operation under Qualcomm HYP.

3. **MDP5 Continuous Splash 24-bit RGB888 -> 32-bit BGRX8888 (`KenzoSimpleFbDxe`)**:
   - `msm8956-xiaomi-kenzo.dts` and `msm8916-mainline/lk2nd` (`platform/msm_shared/mdp5.c`, `lk2nd/display/cont-splash/pipe.c`) show `aboot` leaves the continuous splash framebuffer in 24-bit packed RGB888 (e.g. `0x83200000`, `stride = 1080 * 3`), whereas `DarwinBootPkg/Driver/BootVideo.h` strictly requires 32-bit `PixelBlueGreenRedReserved8BitPerColor`.
   - `KenzoSimpleFbDxe` checks `MDP_GDSCR` (`0x0184D078`, `BIT31`), probes `VIG_0` (`0x01A05000`), `RGB_0` (`0x01A15000`), and `DMA_0` (`0x01A25000`) for active `PIPE_SSPP_SRC0_ADDR`, reconfigures `PIPE_SSPP_SRC_FORMAT` (`0x000236FF`), `PIPE_SSPP_SRC_UNPACK_PATTERN` (`0x03020001`), and `PIPE_SSPP_SRC_YSTRIDE` (`Width * 4`), flushes via `MDP_CTL_0_FLUSH` (`0x01A02018`), and installs GOP + DevicePath.

4. **`BootShim.S` and `board.toml` Packaging Fixes**:
   - Fixed `_StackSize` in `BootShim.S` to `.quad (_Payload - _Head) + FD_SIZE`, added `.ltorg` + `.balign 16` before `_Payload:` (preventing literal pool mismatch in `bootimg.py`), and added `dsb sy; ic iallu; dsb sy; isb` before `br x5`.
   - Removed duplicate `[[bootimg.dtb_node]]` for `/memory/splash_region@83000000` in `UniXNU-kenzo/board.toml` (`6be5969`) because `Resources/DTBs/kenzo.dtb` already contains it (`fdtput -c` failed with `FDT_ERR_EXISTS`).

---

## 4. Scope Limits & Open Constraints

- **Host/CI-verified only**: No physical `fastboot boot` test on Kenzo hardware is recorded in this evidence.
- **Large payload ramdisk window constraint**: Current `[bootimg.image] ramdisk_offset = 0x01000000` (`0x81000000`) leaves only 32 MiB before `splash_region@83000000`. That is sufficient for UEFI-only testing (`--ramdisk none`), but before packing a 192 MiB UniXNU FAT payload, `fastboot getvar max-download-size` and relocating `ramdisk_offset` into clean high RAM (e.g. `0x20000000` -> `0xA0000000`) must be verified on hardware.
