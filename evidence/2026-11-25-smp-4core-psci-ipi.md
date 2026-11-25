# Device Verification: Multi-Core SMP (`cpu0..cpu3`), PSCI `CPU_ON`, and 266/266 Zero-Loss GICv2 SGI IPIs on Physical Kenzo (`d6a8a3b1`)

- **Date**: 2026-11-25
- **Device**: Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, serial `d6a8a3b1`)
- **Kernel**: `CodeDevB7/UniXNU-kenzo` @ `1f9298c91e9d3eab18d59ceb04b7a2719a40fef7` (CI run [45571373335](https://github.com/CodeDevB7/UniXNU-kenzo/actions/runs/45571373335))
- **Command**: `fastboot -s d6a8a3b1 boot artifacts_v9\kenzo-xnu-smp.img` (temporary RAM-only boot)
- **Boot Args**: `-v debug=0x144 keepsyms=1 security-mode-change-enable=1 amfi_get_out_of_my_way=1 smp=1 armcpu-self-ipi=1`

## Observed Live SMP Telemetry on Physical Hardware

At `_bsd_init()` -> `Failed to mount root device` (~30s live uptime):

```text
SMP=03  IPIS=00010A  IPIR=00010A
STEP: 00 07 07 07 00 00
FAIL: 00 00 00 00 00 00
PSCI: 00000000 00000000 00000000 00000000 00000000
TMR : 000019 000BB6 000003 000000 000000 000000
SGI : 0000EC 00000C 000010 000002 000000 000000
```

## Hardware Proof & Findings

1. **4/4 Configured Cores Online (`cpu0..cpu3`, Cortex-A53 Little Cluster)**:
   - `armcpu_smp_running = 3` (all 3 secondary A53 cores `cpu1`, `cpu2`, `cpu3` reached `STEP=07` `kIOCPUStateRunning`, `FAIL=0`, PSCI `CPU_ON` `rc = 0`).
2. **100% Zero-Loss Cross-Core GICv2 SGI IPI Delivery**:
   - `armcpu_ipi_sent = 0x10A` (`266`), `armcpu_ipi_interrupts = 0x10A` (`266`).
   - Per-core SGI breakdown: `cpu0 = 236 (0xEC)`, `cpu1 = 12 (0x0C)`, `cpu2 = 16 (0x10)`, `cpu3 = 2 (0x02)` -> sum = `266` exact!
3. **Multi-Core Virtual Timer PPI 20 Active**:
   - `cpu0 = 25 (0x19)`, `cpu1 = 2,998 (0xBB6)`, `cpu2 = 3 (0x03)` timer IRQs handled concurrently.
4. **Why `cpu4` (`0x100`) & `cpu5` (`0x103`) Cortex-A72 Big Cluster Were `STEP=00`**:
   - Disassembly of `_ml_parse_cpu_topology` (`0xFFFFFFF0073F4058`: `mov w8, #0x4`) confirmed `BCM2837` default `MAX_CPUS = 4` was compiled in (`native-smp-secondary.patch`, which raises `MAX_CPUS` to `8` and masks `MPIDR_EL1` with `#0xFFFF` in `start.s` for non-zero `Aff1`, is not yet included in `UniXNU-kenzo/patches/xnu/series`).
5. **Early Userland Finding (`"AMFI: No PMGR?"`)**:
   - Under concurrent SMP, a background AMFI thread hit `"AMFI: No PMGR?"` because `kenzo.adt` does not yet publish an `/arm-io/pmgr` node or `AppleKeyStoreState` / PMGR provider (a clean item to add alongside `GopFramebuffer` and storage for userland bring-up).

## Next Steps

- Include `MAX_CPUS = 8` + `start.s` `Aff1` mask (`0xFFFF`) in `UniXNU-kenzo/patches/xnu/` to unlock `cpu4`/`cpu5` (Cortex-A72 Big Cluster) for full 6-core SMP.
- Move to **Stage 2 (Userland / `launchd`)**: enable `GopFramebuffer`, `AppleKeyStoreState`, and `RamBlock` / eMMC `SdhciBlock` + iPadOS 17.7.11 IPSW rootfs & TrustCache.
