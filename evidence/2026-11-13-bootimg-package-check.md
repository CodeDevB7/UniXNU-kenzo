# Kenzo boot-image packaging preflight

- Date: 2026-11-13
- Command: `bash scripts/package.sh --check --os ipados-17.7.11-21H461` (executed via WSL)
- Target: Xiaomi Redmi Note 3 (`kenzo`), Qualcomm MSM8956 (Snapdragon 650).
- Updates tested:
  - `board.toml [bootimg.image]`: `base = 0x80000000`, `kernel_offset = 0x8000`, `ramdisk_offset = 0x01000000`, `tags_offset = 0x100`, `pagesize = 2048`, `header_version = 0`.
  - `board.toml [[bootimg.dtb_node]]`: Appends continuous splash framebuffer reservation `/memory/splash_region@83000000` (40 MiB @ `0x83000000`, label `cont_splash_mem`).
  - `board.toml [payload]`: `dram_size = "0x80000000"`.
- Pinned SoC feed: `CodeDevB7/UniXNU-msm8956-common` at `d6845aea8fc3e5c14993b269fcca92927e85fb45`.
- Result: **Passed cleanly (Exit Code 0)**.
  - Resolved `kenzo_defconfig` configuration.
  - Verified remote IPSW profile components (`kernelcache.decompressed`, `DeviceTree.j120ap.im4p`, `044-76040-038.dmg.trustcache`, `044-76034-038.dmg.trustcache`).
  - Resolved all 59 XNU kernel patches in order.
- Output summary: `check done: 59 patches, configuration kenzo_defconfig, OS ipados-17.7.11-21H461`.
- Limits: This validates dependencies, preflight configuration, and patch resolution. It does not compile firmware or kernel, assemble final boot.img, or boot a physical device.
