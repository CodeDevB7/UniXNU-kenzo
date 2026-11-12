# Kenzo mainline DTS reference

- Date: 2026-11-12
- Scope: read-only review of the Kenzo device tree referenced by a public postmarketOS draft port. No source was copied into the UniXNU DTS and no kernel was built locally.

## Source and confidence

- Archived postmarketOS merge request !5389 adds `device-xiaomi-kenzo` and a Linux 6.11 kernel package. The MR was draft and reports occasional boot hangs, so it is an engineering reference, not a stable board validation.
- Its kernel package pins `msm8956-mainline/linux` commit `372bdae91dfec7bfb361b53250fc5716c87131be` and names the DTS `qcom/msm8956-xiaomi-kenzo`.
- The pinned DTS identifies the phone as `Xiaomi Redmi Note 3 Pro`, compatible `xiaomi,kenzo`, `qcom,msm8956`, with MSM8956 v1.1 and MTP board selector cells (`qcom,msm-id = <0x10a 0x10001>`, `qcom,board-id = <8 0>`). A later live capture from the connected Kenzo reports revision 0 (`qcom,msm-id = <0x10a 0>`) with the same MTP board selector; the draft's v1.1 selector must not be assumed for this device.

## Useful board facts from that reference

- The DTS uses the generic MSM8956 SoC include and adds Kenzo's PM8004, PM8950 and PMI8950 nodes, a 1080x1920 panel/framebuffer reservation, touchscreen, keys, USB extcon, and board regulators/peripheral setup.
- Its Kenzo-specific reserved-memory edits remove generic `ext-region@85b00000`, `memory@8da00000`, and `tz-apps@8dd00000`, then add `qseecom@8dd00000` of size `0x01900000` and `ramoops@dfc00000` of size `0x00400000`. These are Linux boot reservations from that draft port; they are not a verified UEFI handoff map and are not copied to the UniXNU board DTS.
- It reserves `0x83200000` for a 1080x1920x3 framebuffer. The node is disabled in the reference DTS, so this address is not evidence of an active bootloader framebuffer on this device.
- Its board selector matches extracted generic DTB 66 (MSM8956 v1.1 MTP) at the Qualcomm selector-cell level. The extracted tree does not identify Kenzo and the local Android property `androidboot.boardID=2` is a different data point. Do not treat this as proof the stock bootloader selected DTB 66.

## Assessment and next work

This source gives a concrete device-specific Linux hardware inventory to compare against the minimal UniXNU DTS and can guide future peripheral research. The live device capture now establishes the current Android kernel's populated FDT selector and memory window, but not the complete UEFI carveouts, UEFI service handoff, or a viable Kenzo UEFI launch path. The draft DTS's v1.1 selector differs from the connected device's live revision 0 tree.

## Sources

- Draft MR !5389: https://gitlab.com/postmarketOS/pmaports/-/merge_requests/5389
- MR changes API: https://gitlab.com/api/v4/projects/postmarketOS%2Fpmaports/merge_requests/5389/changes
- Pinned Linux source, Kenzo DTS: https://github.com/msm8956-mainline/linux/blob/372bdae91dfec7bfb361b53250fc5716c87131be/arch/arm64/boot/dts/qcom/msm8956-xiaomi-kenzo.dts
- Pinned Linux source, MSM8956 common DTSI: https://github.com/msm8956-mainline/linux/blob/372bdae91dfec7bfb361b53250fc5716c87131be/arch/arm64/boot/dts/qcom/msm8956.dtsi
