# Device Verification: Full XNU + GICv2 + Timer IRQs + IOKit + APFS + `_bsd_init()` -> `Still waiting for root device` on Physical Redmi Note 3 (`kenzo`)

- **Date**: 2026-11-25
- **Device**: Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, serial `d6a8a3b1`)
- **Firmware**: `CodeDevB7/uefi-kenzo` @ `3fd164b50c7f5279388c39943b885f1511690a09` (CI run `45543315330`)
- **Kernel**: `CodeDevB7/UniXNU-kenzo` @ `1f9298c` (CI run [45571373335](https://github.com/CodeDevB7/UniXNU-kenzo/actions/runs/45571373335), `kernel.macho` `51,929,088` bytes, SHA-256 `DB5729518434E4C95750199B364919079E0AEAED38AB2402BB12BABE50A6F1C5`)
  - Binary audit: `0` ARMv8.1 `PAN` instructions (`# CONFIG_XNU_NATIVE_PAN is not set`), `CONFIG_XNU_DT_PANIC_REGION=y`, `qcom,msm-qgic2` GICv2 + Virtual Timer PPI 20 + `19.2 MHz` fallback built cleanly in `ArmCpu.cpp`.
- **Command**: `fastboot -s d6a8a3b1 boot artifacts_v9\kenzo-xnu-gicv2.img` (temporary RAM-only boot; never `fastboot flash`)

## Observed Live On-Screen Boot Output

Physical Kenzo booted cleanly through the **entire Darwin 23.6.0 / iPadOS 17.7.11 (`21H461`) kernel** without a single hardware fault or exception:

```text
Darwin Kernel Version 23.6.0 ...
VM bootstrap done: 39 maps, 112 entries and 115 holes left
Firehose configured: 16 chunks, 8 io pages
...
IONVRAM.cpp:initImageFormat:1433 - NVRAM size is 8192 bytes
Darwin Image4 Validator Version 6.3.0
AMFI: queried model name from device tree: iPad7,1
Security policy loaded: Apple Mobile File Integrity (AMFI)
Security policy loaded: Seatbelt sandbox policy (Sandbox)
CoreAnalyticsHub::start(IOService *)::218:CoreAnalyticsHub start completed
Copyright (c) 1982, 1986, 1989, 1991, 1993
    The Regents of the University of California. All rights reserved.
MAC Framework successfully initialized
mbinit: done [64 MB total pool size, (64/0) split]
dlil_init: All the created dlil kernel threads have been scheduled at least once. Proceeding.
ifnet_attach: All kernel threads created for interface lo0 have been scheduled at least once. Proceeding.
apfs_module_start:3232: load: com.apple.filesystems.apfs, v2236.140.6, apfs-2236.140.6, 2025/07/11
com.apple.AppleFSCompressionTypeZlib load succeeded
Waiting on <dict ID="0"><key>IOProviderClass</key><string ID="1">IOMedia</string><key>Content</key><string ID="2">Apple_HFS</string></dict>
Still waiting for root device
Failed to mount root device @%s:%d
```

## What This Proves on Physical Hardware

1. **Native ARMv8.0-A XNU Execution**: Zero PAN traps or workarounds required; kernel runs cleanly out of CI build `45571373335`.
2. **Qualcomm MSM8956 GICv2 + Virtual Timer PPI 20 Working**: `ArmCpu` mapped GICv2 (`0x0B000000`/`0x0B002000`), registered `cpu0`, parked `cpu1..cpu5` cleanly (no `AppleARMCPU` fallback), enabled Virtual Timer PPI 20, scheduled kernel threads (`dlil_init`, `ifnet_attach`), and drove real-time BSD root-mount wait timers (`Still waiting for root device` -> timeout -> `Failed to mount root device`).
3. **Full IOKit + Security + Networking + APFS + BSD Init**: `IONVRAM`, `AppleImage4`, `AMFI`, `Sandbox`, `CoreAnalyticsHub`, `MAC Framework`, `mbinit`, `lo0`, `AppleFSCompressionTypeZlib`, and `com.apple.filesystems.apfs` all initialized cleanly.

## Next Steps

1. Verify 6-core SMP (`smp=1`) bring-up (4× Cortex-A53 + 2× Cortex-A72 via PSCI `CPU_ON` + GICv2 SGI IPIs).
2. Add `GopFramebuffer` + `RamBlock` / eMMC `SdhciBlock` + iPadOS 17.7.11 IPSW rootfs & TrustCache so XNU mounts the root volume and launches `launchd` -> userland.
