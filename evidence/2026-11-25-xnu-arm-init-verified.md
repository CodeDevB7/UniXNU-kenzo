# 2026-11-25 — Physical Device XNU `_arm_init()` -> `_machine_startup()` Verified (Kenzo `d6a8a3b1`)

## Summary
Using a freestanding ARM64 Crash Dumper (`_ExceptionVectorsBase` + `_panic`/`panic_common`/`_Assert`) and `_vinfo`-backed milestone probes on physical **Xiaomi Redmi Note 3 (`kenzo`, serial `d6a8a3b1`)**, we diagnosed and resolved two hardware blockers in `kernel.macho` (iPadOS 17.7.11 `21H461`) and verified XNU execution all the way through **XNU 4 KiB MMU enable, `_arm_vm_init()`, `_PE_create_console()`, `_PE_init_printf()`, `_cpu_machine_idle_init()`, `_PE_init_platform(TRUE)`, `_cpu_timebase_init()`, `_PE_init_cpu()`, and into `_machine_startup()` -> `_kernel_bootstrap()`**!

## Hardware Blockers Found & Proven by Evidence
1. **ARMv8.1 `PAN` Undefined Instruction (`ELR=0xFFFFFFF0079B5464`, `ESR=0x02000000`, `EC=0`)**:
   - Physical Crash Dumper captured `SP0_SYNC` at `0xFFFFFFF0079B5464` (`msr PAN, #0x1`).
   - Snapdragon 650 (Cortex-A53 + Cortex-A72) is ARMv8.0-A and does not implement ARMv8.1 Privileged Access Never (`PAN`).
   - `kernel.macho` contained 38 `PAN` instructions (`9` `msr PAN,#0`, `15` `msr PAN,#1`, `14` `mrs xN,PAN`). Patching them (`nop` / `mov xN, xzr`) unblocked entry into `_arm_vm_init()`.
2. **Hardcoded Sheng/SM8650 pstore PA `0x3C0FC000` in `_PE_create_console()` (`0xFFFFFFF007948C78`)**:
   - Disassembly of `_PE_create_console()` revealed hardcoded `mov w0, #0xc000; movk w0, #0x3c0f, lsl #16` (`0x3C0FC000`) passed to `_ml_io_map_wcomb` and immediately dereferenced (`ldr w8, [x0]`).
   - On Kenzo (MSM8956), DDR begins at `0x80000000`; reading `0x3C0FC000` caused a silent Qualcomm NOC bus hang.
   - Remapping `0x3C0FC000` -> `0xDFC00000` (Kenzo's reserved 4 MiB high-DRAM ramoops region) unblocked `_PE_create_console()` and allowed `_arm_init()` to run to completion.
3. **Apple S5L UART `_serial_init` (`0xFFFFFFF0079494CC`)**:
   - Stubbed `_serial_init` to return `0` during bring-up so `_kprintf` does not poll Qualcomm MSM UARTDM (`0x078B0000`) with Apple S5L UART register offsets.

## Observed Device Result (6 Verified Milestone Bars)
1. **Green**: `DarwinBoot` `ExitBootServices()` -> XNU `_start`
2. **Blue**: XNU `_start` (MMU off)
3. **Magenta**: XNU `common_start` -> `msr SCTLR_EL1, x0` (4 KiB ARM64 MMU enabled)
4. **Yellow**: `_cpu_machine_idle_init()` completed (post `_arm_vm_init`, `_PE_create_console`, `_PE_init_printf`)
5. **Cyan**: `_PE_init_platform(TRUE)` completed (`_pe_arm_init_interrupts`, `_pe_arm_init_debug`)
6. **White**: `_arm_init()` completed -> entered **`_machine_startup()` -> `_kernel_bootstrap()`**!

## Next Milestone
- Add permanent XNU kernel patches in `UniXNU-kenzo/patches/xnu/` for ARMv8.0-A (disable `PAN`) and Kenzo pstore address (`0xDFC00000`).
- Bisect `_kernel_bootstrap()` (`_vm_mem_bootstrap` -> `_console_init` -> `_load_context`) and bring up XNU verbose video console (`-v`) output on `0x83200000`.
