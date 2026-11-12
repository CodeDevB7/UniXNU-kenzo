# MSM8956 DTB inventory

- Date: 2026-11-12
- Inputs: local `kenzo-dump/extracted_dtb_0.dtb` through `extracted_dtb_85.dtb` files in the workspace.
- Inspection: read root `model` and `compatible` properties with `fdtget -t s` under WSL for all 86 extracted DTBs; filtered for `kenzo` or `msm8956`. Qualcomm `qcom,msm-id` and `qcom,board-id` cells were also inspected in the matching reference trees.
- MSM8956 matches: DTB 0 (MSM8956 MTP), 59 (MSM 8956 Simulator), 60 (MSM 8956 RUMI), 61 and 63 (MSM8956 CDP), 62 (MSM8956 MTP), 66 (MSM8956v1.1 MTP), 67 (MSM8956v1.1 CDP), and 68 (MSM8956v1.1 QRD SKUN).
- Result: none of the extracted root model/compatible properties identify Xiaomi Kenzo. These Qualcomm reference-platform trees do not validate Kenzo CPU MPIDRs, GIC wiring, reserved memory, or peripheral wiring.
- A live `/proc/device-tree` capture from the connected Kenzo reports root model `Qualcomm Technologies, Inc. MSM8956 MTP`, compatible `qcom,msm8956-mtp`, MSM ID `<0x10a 0>`, and board ID `<8 0>`. These root identity and selector cells match extracted DTB 0; the active kernel FDT is bootloader-populated, so the zero-memory fields in the source DTB are filled with the 2 GiB range at runtime. See `2026-11-12-live-device-tree.md`.
- The postmarketOS draft Kenzo DTS requests MSM8956 v1.1 (`qcom,msm-id = <0x10a 0x10001>`) with MTP board ID 8. This differs from the live Kenzo tree, which reports MSM8956 revision 0. Its v1.1 selector/DTB 66 should not be used for this current device capture.
- `androidboot.boardID=2` in the kernel command line differs from active FDT `/qcom,board-id = <8 0>`; the two fields are distinct. The live FDT selector is the relevant value when matching QCDT entries.
- Next step: reconcile this Android kernel's populated tree with the future UEFI handoff and verify any UEFI-specific memory reservations before booting XNU.
