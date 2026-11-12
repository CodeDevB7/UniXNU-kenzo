# Read-only aboot inspection

- Date: 2026-11-12
- Device partition: `/dev/block/bootdevice/by-name/aboot` resolves to `/dev/block/mmcblk0p19`, size 1,048,576 bytes.
- Acquisition: copied the partition to a temporary host file with `adb exec-out su -c 'dd if=/dev/block/mmcblk0p19 bs=1M status=none'`. This is a read-only block read. The image is retained only in the host temporary directory and is not committed. SELinux remained Enforcing for this read.
- SHA-256: `d04cd1adf5e6ff1ce37206ba19e55edf21d137353a06cf2ed54b2f926b12daec`.

## Findings

- Extracted strings identify a Qualcomm Little Kernel (`welcome to lk`) Android bootloader built for the MSM8952 target and Kenzo (`target/msm8952`, `MSM8952`, `KENZO`).
- The binary includes the message `fastboot boot command is not available.` The user reports repeatedly booting both `boot.img` and `recovery.img` with `fastboot boot` on this device. The static string therefore does not establish that the command is unavailable; its condition/context is unknown. No fastboot command was sent during this inspection, and no output from the prior user attempts is available.
- Android property `ro.bootloader` reports `unknown`; the image strings did not expose a build date or release tag. The binary hash pins the observed image content but does not identify its vendor release/version.

## Impact and limits

This confirms the observed boot chain uses a Kenzo/MSM8952 LK `aboot` image. The user's repeated successful `fastboot boot` use makes it a promising temporary launch route, but does not yet prove a UEFI payload will boot. Subsequent read-only hash/string inspection of SBL1, TZ and HYP is recorded in `2026-11-12-boot-chain-readonly.md`; SBL1, aboot and HYP release tags remain unknown. Do not flash or overwrite the bootloader during exploration.
