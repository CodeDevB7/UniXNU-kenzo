# Kenzo device tree and ADT preflight

- Date: 2026-11-12
- Device repository revision: `b9280aaa8c6f7fd2622bdc0b28aea8b9c35438a7`
- Shared SoC revision: `894aea231e7ac7133b2947777b2dbc89bdd4c6aa`
- UniXNU revision: `5a00e20157f3975d155f13ced7593347f9f5bf73`
- Commands, run under WSL/Linux:

  ```sh
  dtc -I dts -O dtb -i /path/to/UniXNU-msm8956-common/dts \
    -o /tmp/kenzo-hardware.dtb /path/to/UniXNU-kenzo/dts/kenzo.dts
  python3 /path/to/UniXNU/scripts/make-adt.py \
    --dtb /tmp/kenzo-hardware.dtb \
    --os-profile /path/to/UniXNU/profiles/os/ipados-17.7.11-21H461/profile.toml \
    --board /path/to/UniXNU-kenzo/board.toml --output /tmp/kenzo.adt
  ```

- Result: DTC exited successfully; `/cpus/timebase-frequency` reads `19200000`; the GIC node has the two expected register ranges; standalone ADT generation completed and wrote 12,888 bytes.
- DTC emitted one non-fatal `unit_address_vs_reg` warning for the `/soc` bus, which has `ranges` but no unit address.
- Scope: this verifies DTS syntax and host ADT generation only. The inputs use the preliminary Kenzo memory/CPU description and a pinned shared SoC layer; they do not verify a Kenzo firmware handoff, XNU boot, or hardware CPU/GIC behavior. The extracted DTB inventory shows no Kenzo-compatible root.
