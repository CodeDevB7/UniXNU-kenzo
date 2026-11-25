# 2026-11-25 — Device Verification: 6-Core Heterogeneous SMP (4×A53 + 2×A72), ApplePMGRNub, and MSM8956 SDHCI eMMC Probe

## Build & Boot Identity
- **Device**: Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, serial `d6a8a3b1`)
- **Kernel commit**: `fb7ea65f27414d1c1d90b11859962af944cf11bd` (`CodeDevB7/UniXNU-kenzo`)
- **CI Run**: `45580061642` (`macos-26`, green, `kernel.macho` SHA-256 `510A538179CA217B469CF4D9498248864BCA0C6DADD387FC1B7D9D5B1F7B883C`, `0` ARMv8.1 PAN instructions)
- **UEFI**: `CodeDevB7/uefi-kenzo` @ `3fd164b50c7f5279388c39943b885f1511690a09` (KLZM high-RAM FAT32 -> DarwinBoot)
- **Boot command** (temporary RAM-only): `fastboot -s d6a8a3b1 boot artifacts_v9\kenzo-xnu-stage2.img`
- **Boot args**: `-v debug=0x144 keepsyms=1 security-mode-change-enable=1 amfi_get_out_of_my_way=1 smp=1 armcpu-self-ipi=1 sheng-pmgr=2`

## Observed Hardware Results

### 1. 6-Core Heterogeneous SMP (4× Cortex-A53 + 2× Cortex-A72) — VERIFIED
- `Log queues configured: slot count: 48` (scaled up from `32` with 4 CPUs).
- `SMP=05` — **all 5 secondary cores (`cpu1..cpu3` A53 + `cpu4` `0x100` A72 + `cpu5` `0x103` A72) online!**
- `ST: 00 07 07 07 07 07` — every secondary core across both clusters reached `STEP=07`.
- `IS=0000EB` (`235` sent) / `IR=0000EC` (`236` received) — zero-loss cross-cluster GICv2 SGI IPIs (`SG: 000068 000070 000006 000009 000003 000002`).
- Multi-cluster Virtual Timer PPI 20 interrupts (`TM: 0309A8 030D69 00031C 000063 000005 000000`, `~400,000` ticks across A53 + A72).

### 2. `ApplePMGRNub` Publisher (`ShengPlatformPower`) — VERIFIED
- `PM: 05 00000000` (`stage = 5`, `status = kIOReturnSuccess`).
- Subclassed prelinked `ApplePMGR` (iPadOS 17.7.11 `21H461`) and published `ApplePMGRNub` synchronously into IORegistry.
- Completely eliminated `"AMFI: No PMGR?"` panic; kernel booted cleanly through `_bsd_init()` to root-wait timeout (`IOKitBSDInit.cpp:984` `"Failed to mount root device"`).

### 3. ADT Interrupt-Parent Finding & MSM8956 eMMC `NativeSdhciBlock` Probe Diagnosis
- First boot with `interrupts = <0 0x7b 0>` on `/arm-io/sdhci` faulted in `IODTGetICellCounts(NULL)` (`PC=0xFFFFFFF007891D24`, `FL=0xFFFFFFF00788FFE0` `IODTMapInterruptsSharing`) because `make-adt.py` emits Linux `phandle` without `AAPL,phandle`. Removing `interrupts` from the polled-PIO `sdhci@7824900` node resolved the fault immediately.
- On the clean ADT boot, `NativeSdhciBlock` matched `/arm-io/sdhci`, mapped `0x07824900`, and issued `CMD8` and `CMD17`:
  ```
  NativeSdhciBlock: cmd 8 arg=0x0 fail norm=0x0000 err=0x0000 ps=0x01f80001 resp=0x00000600
  NativeSdhciBlock: CMD8 SEND_EXT_CSD failed
  NativeSdhciBlock: cmd 17 arg=0x1 fail norm=0x0000 err=0x0000 ps=0x01f80001 resp=0x00000000
  NativeSdhciBlock: eMMC read path not ready
  ```
- **Root Cause**: Qualcomm LittleKernel `aboot` `target_uninit()` runs `mmc_put_card_to_sleep(dev)` (leaving leftover `resp=0x00000600` = `STBY` -> `SLEEP` state) and `sdhci_mode_disable(&dev->host)` (clearing `CORE_HC_MODE` bit 0 `HC_MODE_EN` at `core_mem` `0x07824000 + 0x78`).
- **Fix (Stage 2b)**: Export `core_mem` (`0x07824000 0x800`) alongside `hc_mem` (`0x07824900 0x500`) in `dts/kenzo.dts`, re-assert `CORE_HC_MODE = HC_MODE_EN | FF_CLK_SW_RST_DIS` (`0x2001`), ack `CORE_PWRCTL`, ensure `SD_CLK_EN`, wake eMMC via `CMD5(awake)` + `CMD7(select)` -> `TRAN` (with full JEDEC 1-bit DS `CMD0..CMD7` fallback).
