# Device Verification: XNU IOKit Live Execution & GICv2 Root-Cause Diagnosis on Physical Kenzo (`d6a8a3b1`)

- **Date**: 2026-11-25
- **Device**: Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, serial `d6a8a3b1`)
- **Command**: `fastboot -s d6a8a3b1 boot artifacts_v9\kenzo-xnu-console.img` (without `_ExceptionVectorsBase` hijack; temporary RAM-only boot)

## Observed Live On-Screen IOKit Output

After removing the temporary `_ExceptionVectorsBase` hijack so XNU's real `sleh_synchronous` handled normal kernel write page faults cleanly, physical Kenzo booted all the way into **live IOKit kext matching and start**, filling the entire 1080p console and wrapping to the top:

```text
AppleARMBootPerf: Error: failed to publish profile data (1)
AppleARMSFRManifest: Starting
AppleARMSFRManifest: Error: Failed to open reg-entry /chosen/secure-boot-hashes
...
virtual bool CoreAnalyticsHub::start(IOService *)::139:CoreAnalyticsHub start
Registered CoreAnalyticsHub functions with xnu:virtual bool CoreAnalyticsHub::start(IOService *)::218:CoreAnalyticsHub start completed
```

Followed by a clean English XNU panic in the red crash box:
- `TAG=4 (_panic)`, `LR=0xFFFFFFF0049BCFE8`, `X0=0xFFFFFFF004289E44`
- Message: `"CPU configuration in device tree is invalid" @%s:%d` (`AppleARMCPU.cpp:111`)

## Hardware & Disassembly Root-Cause Proof

1. Disassembly of `0xFFFFFFF0049868F0` (caller of `0xFFFFFFF0049BCFC0` in `__PLK_TEXT_EXEC` `AppleARMCPU.kext`) confirmed `AppleARMCPU::start()` called `_ml_processor_register()`, received `KERN_FAILURE` (`5`), and panicked.
2. Disassembly of `ArmCpu::mapGic()` (`0xFFFFFFF00796C134`) and `ArmCpu::start()` (`0xFFFFFFF00796CA30`) + inspection of upstream `UniXNU/ArmCpu` (`d90e457b8392623853ec85c860feef546c767621`) proved three exact MSM8956 incompatibilities:
   - **GICv3-only `ArmCpu::mapGic()`**: `ArmCpu` only matched `compatible == "arm,gic-v3"` with `GICD >= 0x10000` / `GICR >= 0x20000` and used GICv3 system registers (`ICC_SRE_EL1`, `ICC_IAR1_EL1`, `ICC_EOIR1_EL1`, `ICC_SGI1R_EL1`). Physical Kenzo has **GICv2** (`compatible = "arm,cortex-a15-gic", "qcom,msm-qgic2"`, `GICD = 0x0B000000/0x1000`, `GICC = 0x0B002000/0x1000`). Because `ArmCpu::mapGic()` failed, `ArmCpu::start()` returned `false` on `cpu0` and IOKit fell back to Apple's `AppleARMCPU.kext` (`IOProbeScore = 100`).
   - **Hardcoded Virtual Timer PPI 27**: `ArmCpu` hardcoded PPI `27`; physical MSM8956 wires the EL1 Virtual Timer to **PPI `20`** (`<1 4 0xf08>`, verified in live FDT and UEFI `ArmTimerDxe`).
   - **`CNTFRQ_EL0 == 0` & secondary CPU fallback**: MSM8956 leaves `CNTFRQ_EL0 == 0` (needs `19,200,000` Hz fallback), and `ArmCpu::probe()` declining secondary CPUs when `smp=0` allowed `AppleARMCPU.kext` to claim `cpu1..cpu5` and panic.

## Permanent Fixes Committed for Clean macOS CI Kernel Build (`kernel.yml`)

1. `configs/kenzo_defconfig`:
   - `# CONFIG_XNU_NATIVE_PAN is not set` (native ARMv8.0-A Cortex-A53/A72 build with zero ARMv8.1 PAN instructions)
   - `CONFIG_XNU_DT_PANIC_REGION=y` (disables hardcoded BCM2837 `0x3C0FC000` pstore PA)
2. `patches/armcpu-gicv2.patch` + `scripts/prepare-kernel-patches.py`:
   - Dual **GICv2 + GICv3** support in `ArmCpu.cpp` (`arm,cortex-a15-gic` / `qcom,msm-qgic2` / `arm,gic-400`, MMIO `GICD` + `GICC`, zero `ICC_*_EL1` instructions on GICv2).
   - Automatic **PPI 20** virtual timer selection on `qcom,msm-qgic2` (PPI 27 on standard GICv3).
   - `19,200,000` Hz fallback when `CNTFRQ_EL0 == 0`.
   - `ArmCpu::probe()` claims all valid CPU nubs (preventing `AppleARMCPU.kext` fallback) while `ArmCpu::start()` cleanly parks secondary CPUs when `smp=0` and boots them via PSCI `CPU_ON` when `smp=1`.
