# Read-only Qualcomm boot-chain partition inventory

- Date: 2026-11-12
- Device: connected Kenzo (`soc_id=266`, MSM8956). Images were read from block partitions only; no write, flash, reboot, or bootloader command was issued. SELinux stayed Enforcing for these reads.
- Hashes identify the exact bytes observed on this device. The temporary raw images were not committed.

| Partition | Block device | Size | SHA-256 | Readable version/target evidence |
|---|---|---:|---|---|
| `sbl1` | `mmcblk0p4` | 512 KiB | `2170c4daa5cfc4dc3a085debe79ad71aee837d9d3175983231a54b62af354140` | Strings include `8976A-EAASANAZA-40000000`, `MSM8952`, and `MSM8956`; no release date/tag found. |
| `aboot` | `mmcblk0p19` | 1 MiB | `d04cd1adf5e6ff1ce37206ba19e55edf21d137353a06cf2ed54b2f926b12daec` | Qualcomm Little Kernel / `MSM8952` / `KENZO`; includes `fastboot boot command is not available.` `ro.bootloader` is `unknown`; no release tag found. |
| `hyp` | `mmcblk0p10` | 512 KiB | `680077d3c7c491f596b7ba3b561b878fd98a7e23f8ef6f2866bb1ec7f7e9bcc6` | No obvious version/build string found. |
| `tz` | `mmcblk0p8` | 2 MiB | `583509ab05424daac00e2dd010f66a3b774549ce0617b616e701b8d465c47837` | `QC_IMAGE_VERSION_STRING=TZ.BF.3.0.3-00047`. |

## Assessment

The observed boot chain contains the expected Qualcomm MSM8952/MSM8956 Kenzo LK images, and the TrustZone release string is identifiable. The normal `fastboot boot` path appears unavailable in the observed aboot image. The exact SBL1, aboot, and HYP vendor releases remain unknown; the hashes are recorded so the bytes can be matched if a vendor package or known-good source becomes available. No Kenzo UEFI implementation or supported alternative first-stage launch path has been established.

## Reproduction

Use `adb exec-out su -c 'dd if=/dev/block/mmcblk0pN bs=1M status=none'` for the listed partition number, then hash the captured bytes. This command reads the block device and does not write to it.
