# MSM8956 DTB inventory

- Date: 2026-11-12
- Inputs: local `kenzo-dump/extracted_dtb_0.dtb` through `extracted_dtb_85.dtb` files in the workspace.
- Inspection: read root `model` and `compatible` properties with `fdtget -t s` under WSL for all 86 extracted DTBs; filtered for `kenzo` or `msm8956`.
- MSM8956 matches: DTB 0 (MSM8956 MTP), 59 (MSM 8956 Simulator), 60 (MSM 8956 RUMI), 61 and 63 (MSM8956 CDP), 62 (MSM8956 MTP), 66 (MSM8956v1.1 MTP), 67 (MSM8956v1.1 CDP), and 68 (MSM8956v1.1 QRD SKUN).
- Result: none of the extracted root model/compatible properties identify Xiaomi Kenzo. These Qualcomm reference-platform trees do not validate Kenzo CPU MPIDRs, GIC wiring, reserved memory, or peripheral wiring.
- Next step: obtain a device tree extracted from the matching Kenzo firmware/boot image and compare its enabled nodes and memory reservations against `dts/kenzo.dts` and the shared MSM8956 DTSI.
