# Device Verification: Qualcomm MSM8956 SDCC1 (eMMC) & SDCC2 (32 GB microSD) Live Sector Reads + RPM-SMD Power

- **Date**: 2026-11-25
- **Device**: Xiaomi Redmi Note 3 (`kenzo`, Qualcomm MSM8956 / Snapdragon 650, serial `d6a8a3b1`)
- **Firmware**: `CodeDevB7/uefi-kenzo` commit `4c06de6` (CI run `45620754645`)
- **Command**: `fastboot -s d6a8a3b1 boot artifacts_uefi_probe\kenzo-uefi-firmware-debug\kenzo-uefi-only-boot.img` (temporary RAM-only boot)

## Hardware-Verified Findings

1. **RPM-SMD PM8950 Regulator Voting (`LDO11` + `LDO12` @ 2.95V)**:
   - Located `rpm_requests` SMD channel (`cid=4`, `info=0x60000`, `fifo=0x60800(1024)`, `tx=2`, `rx=2`) via SMEM (`0x86300000`) and rang APCS IPC doorbell (`0x0B011008` `BIT0`).
   - Voted PM8950 `ldoa` `id=11` (`2.95V`, `800 mA`, `swen=1`, `lsmd=1`) and `id=12` (`2.95V`, `22 mA`, `swen=1`, `lsmd=1`).
   - RPM consumed TX (`tx=480/480`) and returned clean 40-byte ACKs (`len=20`, `hdr=00716572,0000000C`, `msg#` matched).

2. **Root Causes Fixed for `SDCC2` (microSD)**:
   - On MSM8916/MSM8956 (`qcom,msm-tlmm-8916`), `TLMM_SDC2_HDRV_PULL_CTL` is at `0x01109000` (whereas `0x0110A000` is `TLMM_SDC1` eMMC). Reprogramming `0x01109000` from `0x00000BDB` (CMD/DATA pull-down) to `0x00001FE4` (CMD/DATA pull-up 10 mA, CLK 16 mA) released the bus.
   - `CORE_PWRCTL_CLEAR` is `core + 0xE4` (`0xE0` is `CORE_PWRCTL_MASK`), and `CORE_PWRCTL_CTL` (`core + 0xE8`) expects `BUS_SUCCESS = 0x1`, `IO_SUCCESS = 0x4` (never `BIT1` `BUS_FAIL`).
   - `CORE_VENDOR_SPEC` (`hc + 0x10C`) configured with `IO_PAD_PWR_SWITCH_EN (BIT15) = 1`, `IO_PAD_PWR_SWITCH (BIT16) = 0` (2.95V), and `CORE_CLK_PWRSAVE (BIT1) = 0` during 400 kHz init.

3. **Live On-Device Output**:
   - `SDCC1` (16 GB internal eMMC): `HC_MODE 0->1 PWRCTL=2->0 PS=01F80000`, `eMMC CMD1 OCR=C0FF8080 (tries=13)`, `eMMC TRAN! CMD13=00000900`, `eMMC LBA1 SIG: 'EFI PART' first=22 last=1D59FDE`.
   - `SDCC2` (32 GB external microSD): `tlmm2=00000BDB->00001FE4`, `HC_MODE 0->1 PWRCTL=0->0 PS=01F80000`, `SDCC2 microSD CMD8 OK! resp=000001AA`, `SDCC2 ACMD41 OCR=C0FF8000 (tries=22)`, `SDCC2 microSD TRAN! RCA=AAAA CMD13=00000900`, `SDCC2 LBA0 MBR: 55 AA (part0 type=0C start=00000800 len=03B71BDF)`.
