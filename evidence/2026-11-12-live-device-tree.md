# Live Kenzo device-tree capture

- Date: 2026-11-12
- Device identification: ADB reports product `nad_kenzo`, device `kenzo`; `/sys/devices/soc0/soc_id` reports decimal `266` (`0x10a`, MSM8956). The Android build fingerprint identifies a `redfin` build, so it is treated as a spoofed/custom OS identity and not as the phone model.
- Acquisition: read `/proc/device-tree` through `adb exec-out` as root. Root reads were denied while SELinux was Enforcing. With the user's explicit authorization, SELinux was temporarily set to Permissive for the read and restored to Enforcing immediately afterward; the restored state was checked. No device files or partitions were written.
- Boot image cross-check: the read-only copy of the 64 MiB `boot` partition has SHA-256 `9f4558462976546d9ba1a62562931c36f58ebee32c9b7a226f86872493b7a493`, identical to the existing `kenzo-dump/boot.img`.

## Active FDT identity and memory

- `/model`: `Qualcomm Technologies, Inc. MSM8956 MTP`.
- `/compatible`: `qcom,msm8956-mtp`, `qcom,msm8956`, `qcom,mtp`.
- `/qcom,msm-id`: `<0x10a 0>`; `/qcom,board-id`: `<8 0>`.
- These selector cells and root identity match extracted reference DTB 0. DTB 66 is the MSM8956 v1.1 MTP tree; it is not the active tree captured here. The postmarketOS draft Kenzo DTS requests v1.1, so its selector is not a match for the currently booted device tree.
- The bootloader-populated `/memory/reg` is `<0 0x80000000 0 0x80000000>`, a 2 GiB range. The local source DTB 0 has zeroes in `/memory/reg`; the active FDT shows that firmware fills it before the kernel starts.
- No root `/reserved-memory` node is exposed in this live tree. The kernel bootargs include `ramoops_memreserve=4M`; this is not a complete UEFI memory map or proof that other firmware carveouts are absent.
- `ro.boot.boardID=2` appears in Android bootargs, while the active Qualcomm FDT selector is `qcom,board-id=<8 0>`. They are distinct fields; use the active FDT selector when matching QCDT candidates.

## CPU, PSCI, interrupt controller, timer and UART

- CPU nodes use `enable-method = "psci"`. MPIDRs `0`, `1`, `2`, `3`, `0x100`, and `0x103` have `device_type = "cpu"`. Nodes `0x101` and `0x102` have `device_type = "nak"`. Linux reports CPUs `0-5` present and online.
- `/psci` reports `compatible = "arm,psci-1.0"`, `method = "smc"`.
- GIC node `/soc/interrupt-controller@b000000` reports `compatible = "qcom,msm-qgic2"` and ranges `0x0b000000/0x1000`, `0x0b002000/0x1000`.
- Architectural timer compatible is `arm,armv8-timer`; its interrupt tuples match the existing SoC definition.
- Debug UART `/soc/serial@78b0000` reports `qcom,msm-lsuart-v14`, `reg = <0x78b0000 0x200>`, interrupt `<0 0x6c 0>`, and Linux status `disabled`. The UniXNU DTS keeps the node enabled for its serial-console use, but its register size and interrupt now match the captured tree.

## Implementation impact and limits

- Corrected the shared MSM8956 DTSI's second Cortex-A72 MPIDR from `0x101` to the observed `0x103` and corrected the debug UART range/interrupt. The two `nak` CPU placeholders are not emitted as CPUs by the UniXNU DTS.
- Updated DTB inventory and porting record to identify DTB 0 as the current active-tree selector match and to distinguish the live Android tree from a UEFI handoff map.
- This capture verifies the current Android kernel's populated FDT and gives a direct hardware-grounded input for CPU/GIC/UART/memory nodes. It does not prove the same tree or reservations will be passed to a future UEFI payload, and no XNU core bring-up or UEFI boot has been performed.
