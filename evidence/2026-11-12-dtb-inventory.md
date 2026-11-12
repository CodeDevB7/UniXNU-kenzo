# MSM8956 DTB inventory

- Date: 2026-11-12
- Inputs: local `kenzo-dump/extracted_dtb_0.dtb` through `extracted_dtb_85.dtb` files in the workspace.
- Inspection: read root `model` and `compatible` properties with `fdtget -t s` under WSL for all 86 extracted DTBs; filtered for `kenzo` or `msm8956`. Qualcomm `qcom,msm-id` and `qcom,board-id` cells were also inspected in the matching reference trees.
- MSM8956 matches: DTB 0 (MSM8956 MTP), 59 (MSM 8956 Simulator), 60 (MSM 8956 RUMI), 61 and 63 (MSM8956 CDP), 62 (MSM8956 MTP), 66 (MSM8956v1.1 MTP), 67 (MSM8956v1.1 CDP), and 68 (MSM8956v1.1 QRD SKUN).
- Result: none of the extracted root model/compatible properties identify Xiaomi Kenzo. These Qualcomm reference-platform trees do not validate Kenzo CPU MPIDRs, GIC wiring, reserved memory, or peripheral wiring.
- Comparison against the Kenzo-specific mainline DTS used by the archived postmarketOS port (see `2026-11-12-kenzo-mainline-dts.md`): that DTS requests MSM8956 v1.1 (`qcom,msm-id = <0x10a 0x10001>`) and MTP board ID 8 (`qcom,board-id = <8 0>`). Extracted DTB 66 is the only listed v1.1 MTP candidate and has those selector cells, but its root remains the generic `MSM8956v1.1 MTP`, not Kenzo. This is a candidate match for Qualcomm's selector, not proof DTB 66 was selected for this handset.
- The Android property `androidboot.boardID=2` in the captured `getprop.txt` does not match MTP board ID 8 and is not itself the Qualcomm QCDT `qcom,board-id` cell. It must not be used to select a reference DTB.
- Next step: obtain a device tree extracted from the matching Kenzo firmware/boot image or capture the bootloader-selected tree, then compare its enabled nodes and memory reservations against `dts/kenzo.dts` and the shared MSM8956 DTSI.
