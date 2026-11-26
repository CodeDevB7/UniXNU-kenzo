# UniXNU Kenzo

Format 2 device port for the **Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, 2 GiB RAM)** running **Darwin 23.6.0 / XNU + iPadOS 17.7.11 (`21H461`)**.

## Current Progress (Device-Verified on Physical `kenzo`)

- **UEFI & Display Scanout (`CodeDevB7/uefi-kenzo`)**: PeilessSec EDK II boots via temporary `fastboot boot`, reconfigures MDP5 RGB0 to 1080×1920 32-bit `BGRX8888` GOP (`KenzoSimpleFbDxe`), decompresses KLZM FAT32 payloads into high DRAM (`0xA0000000`), and hands off to XNU via `DarwinBoot`.
- **6-Core Heterogeneous SMP & Interrupts**: All 6/6 cores (4× Cortex-A53 + 2× Cortex-A72) online via PSCI `CPU_ON` with GICv2 Virtual Timer PPI 20 IRQs and cross-cluster SGI IPIs.
- **Platform Power (`ApplePMGRNub`)**: `ShengPlatformPower` publishes `ApplePMGRNub` (`sheng-pmgr=2`) for AMFI.
- **Storage & Root (`SHNGROOT` on microSD + RAM `Data`)**: `NativeSdhciBlock` runs `SDCC2` external microSD in 4-bit 50 MHz High-Speed mode, mounts `OS` + `SystemOS` (`SHNGROOT`), and leaves internal eMMC Android (`mmcblk0`) 100% untouched. `NativeRamBlock` provides a writable 3 GiB sparse `Data` volume at `/private/var`.
- **iPadOS 17.7.11 Userland (`/sbin/launchd`)**: External TrustCaches load cleanly, 100% of the 3.07 GiB `dyld_shared_cache` passes 4K code-signature validation (`B0C45`), `Early boot complete` finishes with 90+ active daemons, `AppleKeyStore` answers userland calls, and `NativeLegacyDisplay` + `IOSurface` + `/arm-io/gop-display` bring-up is active toward `backboardd` / `SpringBoard` graphical presentation.

See [the porting record](porting.toml) and [the latest Stage 3 evidence](evidence/2026-11-26-ipados-17.7.11-launchd-springboard-bringup.md) for exact revisions and logs.

## On-Screen Telemetry Status Bar (`Q E X N r w D C F K B`)

All values on the top status bar are displayed in **hexadecimal (hex)** to monitor kernel & iPadOS boot progress in real time:

- **`Q`** (6 digits): Number of read requests to **microSD** (`NativeSdhciBlock`).
- **`E`** (2 digits): Number of **microSD read errors** (normally `00`).
- **`X`** (3 digits): Number of **4K code-signature** blobs validated.
- **`N`** (2 digits): Number of **active processes** in the kernel (`_nprocs`) — rises as `launchd` spawns iPadOS daemons.
- **`r`** (4 digits): Number of **read** operations to the RAM `Data` disk (`/private/var`).
- **`w`** (4 digits): Number of **write** operations to the RAM `Data` disk (`/private/var`).
- **`D`** (1 digit): **`NativeLegacyDisplay`** driver stage (`4` = registered & online).
- **`C`** (2 digits): Number of **userland graphics clients** (e.g. `backboardd` / `SpringBoard`) connected to the display.
- **`F`** (3 digits): Number of **iPadOS graphical frames** rendered (`> 0` stops debug text and hands the screen over to iPadOS).
- **`K`** (3 digits): Number of userland calls to **`AppleKeyStore`**.
- **`B`** (4 digits, green): Total **MiB** hashed by code-signature validation (mainly `dyld_shared_cache`, e.g. `0C45` = 3,141 MiB).

## Check Configuration & Build

From this repository, with the `UniXNU` checkout beside it:

```sh
bash scripts/package.sh --check --os ipados-17.7.11-21H461
```

- **Kenzo DTS and ADT build** (GitHub Actions): compiles `dts/kenzo.dts` and generates `kenzo.adt`.
- **Kenzo macOS kernel build** (GitHub Actions): compiles `kernel.macho` (`0` PAN instructions) and uploads redacted build logs.

> **Note**: Always boot test images temporarily into RAM with `fastboot boot <image>`. Never use `fastboot flash`.
