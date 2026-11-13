# Kenzo (MSM8956) Memory Map & Carveouts Analysis

- Date: 2026-11-13
- Target: Xiaomi Redmi Note 3 (`kenzo`), Qualcomm Snapdragon 650 (MSM8956).
- Scope: Establish a verified physical memory map and carveouts inventory for UEFI/EDK2 firmware positioning (`FD_BASE`) and XNU memory handoff, based on raw device dumps and active FDT.

---

## 1. Verified Hardware Memory Banks

From live device inspection (`/proc/device-tree/memory/reg`) and `kenzo-dump/iomem.txt`:
- **Physical RAM Base**: `0x80000000`
- **Total Physical RAM**: `0x80000000` (2 GiB, ending at `0xffffffff`).
- Total usable reported by Linux kernel `meminfo`: `1889528 kB` (~1.80 GiB).

---

## 2. Bootloader Load Windows (from `kenzo-dump/boot.img` Header)

Android boot image header v0 verification:
- `page_size`: `2048` (`0x800`)
- `kernel_addr`: `0x80008000` (`base = 0x80000000`, `kernel_offset = 0x00008000`)
- `ramdisk_addr`: `0x81000000` (`ramdisk_offset = 0x01000000`)
- `tags_addr`: `0x80000100` (`tags_offset = 0x00000100`)

Qualcomm Little Kernel (`aboot`) places the unpacked `boot.img` kernel binary at `0x80008000` and the initrd/payload at `0x81000000`.

---

## 3. Discovered Reserved Memory & Carveouts Inventory

Cross-referencing `kenzo-dump/iomem.txt` and `kenzo-dump/extracted_dtb_0.dtb` (active FDT selector `msm-id=<0x10a 0>`, `board-id=<8 0>`):

| Region Name | DT Node | Address Range | Size | Attribute / Purpose |
|---|---|---|---:|---|
| **LK Kernel & Tags** | N/A | `0x80000000` - `0x80ffffff` | 16 MiB | Bootloader execution / tags / initial kernel entry |
| **LK Ramdisk Window** | N/A | `0x81000000` - `0x82ffffff` | 32 MiB | Android ramdisk / UniXNU FAT payload load window |
| **Continuous Splash FB** | `/memory/splash_region@83000000` | `0x83000000` - `0x857fffff` | 40 MiB (`0x02800000`) | Framebuffer for BOE NT35532 1080x1920 DSI panel (`phandle 0xac`) |
| **System RAM** | N/A | `0x85800000` - `0x862fffff` | ~11 MiB | Unreserved low System RAM |
| **Other Ext Region** | `/memory/other_ext_region@0` | `0x86300000` - `0x867fffff` | 5 MiB (`0x00500000`) | `linux,remove-completely` (firmware carveout) |
| **System RAM** | N/A | `0x86800000` - `0x86bfffff` | 4 MiB | Interstitial System RAM |
| **Modem Region** | `/memory/modem_region@0` | `0x86c00000` - `0x8c1fffff` | 86 MiB (`0x05600000`) | `linux,remove-completely` (Qualcomm Modem firmware carveout) |
| **Relocation Region** | `/memory/reloc_region@0` | `0x8c200000` - `0x8d8fffff` | 23 MiB (`0x01700000`) | `linux,remove-completely` (MBA / Peripheral firmware carveout) |
| **System RAM** | N/A | `0x8d900000` - `0x8dcfffff` | 4 MiB | Interstitial System RAM |
| **TZ Apps / QSEECom** | `/memory/tzapp_region@0` | `0x8dd00000` - `0x8f5fffff` | 25 MiB (`0x01900000`) | `linux,remove-completely` (Qualcomm TrustZone secure apps) |
| **High System RAM** | N/A | `0x8f600000` - `0xffffffff` | 1802 MiB (~1.76 GiB) | Contiguous clean System RAM (contains optional 4 MiB ramoops @ `0xdfc00000`) |

---

## 4. Key Architectural Implications for UniXNU / EDK2

1. **Continuous Splash Framebuffer (`0x83000000`)**:
   - `aboot` leaves the display active with the splash image on the BOE NT35532 1080p panel (`1080x1920`, 24bpp / 32bpp RGBX).
   - The buffer resides at physical address `0x83000000` with 40 MiB allocated.
   - In EDK2, `SimpleFBDxe` can bind directly to `0x83000000`, width 1080, height 1920, stride 1080 (or pitch 4320 bytes), providing an immediate Graphics Output Protocol (`EFI_GRAPHICS_OUTPUT_PROTOCOL`) without needing full MDP5 clock/DSI register configuration!

2. **UEFI Firmware FD Placement (`FD_BASE`)**:
   - Must avoid `0x80000000 - 0x83000000` (LK load and payload staging area).
   - Must avoid `0x83000000 - 0x85800000` (Continuous splash buffer).
   - Must strictly avoid `0x86300000 - 0x8f5fffff` (Modem, MBA, and TrustZone secure carveouts).
   - **Recommended UEFI FD Placement**: Within the high contiguous System RAM bank:
     - Candidate base: `0x90000000` or `0x9fc00000` (standard Qualcomm EDK2 offset).
     - Standard FD size: `0x00300000` (3 MiB) or `0x00400000` (4 MiB).
     - At `0x90000000`, the FD is safely situated above all firmware carveouts, preserving both low RAM for bootloader handoff and the full contiguous upper RAM block for DarwinBoot / XNU.

3. **Android Boot Image Packaging Parameters**:
   - `base`: `0x80000000`
   - `pagesize`: `2048`
   - `kernel_offset`: `0x00008000`
   - `ramdisk_offset`: `0x01000000`
   - `tags_offset`: `0x00000100`
   - `header_version`: `0`
