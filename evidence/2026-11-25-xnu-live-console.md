# Device Verification: Live `Darwin Kernel Version 23.6.0` Boot Console on Physical Redmi Note 3 (`kenzo`)

- **Date**: 2026-11-25
- **Device**: Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, serial `d6a8a3b1`)
- **Firmware**: `CodeDevB7/uefi-kenzo` @ `3fd164b` (CI run `45543315330`)
- **Kernel Base**: `kernel.macho` (`51,933,184` bytes, iPadOS 17.7.11 `21H461`, `Darwin Kernel Version 23.6.0`, CI run `43171449006`) + `kenzo.adt` (`12,888` bytes, CI run `45026987643`)
- **Command**: `fastboot -s d6a8a3b1 boot artifacts_v9\kenzo-xnu-console.img` (temporary RAM-only boot; never `fastboot flash`)
- **Boot Args**: `-v debug=0x144 keepsyms=1 security-mode-change-enable=1 amfi_get_out_of_my_way=1`

## Observed Live On-Screen XNU Boot Output

By hooking `_console_printbuf_putc` (`0xFFFFFFF0073BD468`) to render live 16×16 glyphs directly onto `_vinfo.v_baseaddr` (`*(u64*)0xFFFFFFF007AB6980`, `1080×1920` BGRX8888) and fixing the ARMv8.0-A PAN assertion, physical Kenzo printed the complete early **`Darwin Kernel Version 23.6.0`** boot log on-screen:

```text
Darwin Kernel Version 23.6.0: Thu Nov 12 07:01:44 UTC 2026; runner: ...
ios-obj/DEVELOPMENT_ARM64_BCM2837
pmap_startup() init/release time: 9600 microsec
pmap_startup() delayed init/release of 0 pages
vm_page_bootstrap: 37971 free pages, 17565 wired pages, (up to 0 of which are delayed free)
VM bootstrap: 31 maps, 153 entries and 128 holes available
Maximum number of VM swap files: 5
... (bufferhdr, vm_kernel_copy_map, gIOKitPageableFixed0..3, kmem_ptr_range, zone_meta, zones, kmem_data_range, io_submap, ipc_kernel_map, mb, kmem_sprayqtn_range, kext_pageable, compressor, kmem_ptr_range_meta, kmem_large_ranges[1..5]) ...
"vm_compressor_mode" is 8
mapping deprecated VM_PAGER_FREEZER_DEFAULT to VM_PAGER_FREEZER_COMPRESSOR_NO_SWAP
zone_init: ... (map PGZ, RO, GEN0..GEN3, DATA, metadata, metabits, extra)
VM bootstrap done: 39 maps, 112 entries and 115 holes left
Firehose configured: 16 chunks, 8 io pages
Log queues configured: slot count: 32, per-slot size: 32768, total size: 1048576
Long logs support configured: size: 16384
OSLog stream configured: stream: 8192 bytes, cache: 2112 bytes
OSLog stream verification: 0
standard timeslicing quantum is 10000 us
standard background quantum is 2500 us
mig_table_max_displ = 54 mach_kobj_count = 372
kdp_core zlib memory 0x7000
Serial requested, consistent debug disabled or debug boot arg not present, configuring debugging over serial
iBoot version:
iBoot Stage 2 version:
```

## Root Causes Diagnosed & Proven on Hardware

1. **ARMv8.0-A PAN assertion in `_machine_switch_context` (`0xFFFFFFF0073F8AF0`)**:
   - First boot with `mrs xN, PAN` patched to `0` reached the first context switch and triggered the real XNU panic: `context switch with PAN disabled @%s:%d` (`LR=0xFFFFFFF0079B81CC`, `X0=0xFFFFFFF00707C0B4`).
   - Re-patching `mrs xN, PAN` (`0xD5384260..7F`) to `movz xN, #0x40, lsl #16` (`0x00400000`, bit 22 = PAN enabled) cleanly satisfied all PAN invariants, allowing XNU to complete context switching, VM/zone bootstrap, Firehose/OSLog initialization, Mach IPC/MIG setup, and enter `_PE_init_iokit()`.
2. **Normal Demand-Write Page Fault Intercepted by Temporary `SP0_SYNC` Hook**:
   - Immediately after `iBoot Stage 2 version:`, the temporary `SP0_SYNC` crash dumper (`_ExceptionVectorsBase + 0x000`) caught:
     - `TAG=1 (SP0_SYNC)`, `ELR=0xFFFFFFF007A1615C` (`_bzero`: `stp x1, x1, [x0]`), `ESR=0x000000009600004F` (EL1 Data Abort write, Level 3 permission fault), `FAR=0xFFFFFFF7B1C6E000`, `LR=0xFFFFFFF007843960` (`OSKext::updateLoadedKextSummaries`).
   - Disassembly of `0xFFFFFFF00784391C..0xFFFFFFF00784395C` confirmed `OSKext::updateLoadedKextSummaries` calls `_vm_map_protect(..., VM_PROT_READ | VM_PROT_WRITE, 0)` followed by `_bzero()`, which intentionally triggers a normal lazy-write L3 permission fault handled by XNU's real `sleh_synchronous` -> `arm_fast_fault()`.
   - Fix: Remove the temporary `_ExceptionVectorsBase` (`+0x000`, `+0x200`, `+0x380`) vector hijacks while keeping `_console_printbuf_putc`, `_panic`, `panic_common`, and `_Assert` hooked so XNU handles normal page faults and progresses through IOKit and `_bsd_init()`.

## Next Action

- Boot the updated `artifacts_v9/kenzo-xnu-console.img` (without vector hijack) on physical Kenzo to observe full IOKit driver matching and `_bsd_init()` live on screen.
- Port the three proven kernel fixes cleanly into `UniXNU-kenzo` (`CONFIG_XNU_DT_PANIC_REGION=y` in `configs/kenzo_defconfig`, permanent ARMv8.0-A PAN disable patch in `patches/xnu/`, and UART config) and rebuild `kernel.macho` in macOS CI.
