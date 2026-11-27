// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoGpuProbe - Safe Hardware Gates #1 & #2 probe for A510Metal (Adreno 510 on MSM8956 / Kenzo).
//
// Verified register offsets from CodeDevB7/A510Metal (Msm8956GpuPlatformRegs.h, NativeAdrenoA510HwInit.h):
//   - Gate #1: PM8004 S5 (vdd_gfx, SPMI PPID 0x520) + GCC Oxili CX/GX GDSC (0x0185904C / 0x0185901C)
//              + SMMU clocks (0x0184500C) + 6 Oxili CBCRs + RBBM_ALWAYSON_COUNTER 1 ms liveness (~19,200 ticks)
//   - Gate #2: SMMUv2 Non-Secure Context Banks CB0 (0x01F08000) & CB2 (0x01F0A000) EL1 accessibility
//
// Boot-arg:
//   kenzo-gpu-probe=1 : Read vdd_gfx & GDSCs; if vdd_gfx is ON, bring up Oxili clocks, probe RBBM_ALWAYSON & CB0/CB2.
//   kenzo-gpu-probe=2 : Same, plus attempt SPMI enable of PM8004 S5 (vdd_gfx = 965 mV) if off, and read SMMU GR0 IDR0.

#include "KenzoSpmi.h"
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" {
volatile uint32_t kenzo_gpu_probe_state = 0; // 0=off, 1=vdd_gfx_off, 2=gdsc_fail, 3=clk_fail, 4=counter_stuck, 5=ALIVE!
volatile uint32_t kenzo_gpu_probe_diag[8] = {};
void kenzo_gpu_probe_run(void);
}

namespace {

constexpr uint64_t kGccPhys         = 0x01800000ULL;
constexpr uint64_t kGccSize         = 0x00080000ULL;
constexpr uint64_t kGpuRegPhys      = 0x01C00000ULL;
constexpr uint64_t kGpuRegSize      = 0x00004000ULL;
constexpr uint64_t kMemAccGfxPhys   = 0x01944000ULL; // +0x130 MEM_ACC_GFX
constexpr uint64_t kSmmuPhys        = 0x01F00000ULL;
constexpr uint64_t kSmmuSize        = 0x00010000ULL;

constexpr uint16_t kPpidVddGfx      = 0x520U; // PM8004 S5 (SID 5, periph 0x20, FTS2.5)

constexpr uint32_t kGccOxiliGxGdscr = 0x5901CU;
constexpr uint32_t kGccOxiliGxClamp = 0x5B00CU;
constexpr uint32_t kGccOxiliCxGdscr = 0x5904CU;
constexpr uint32_t kGccGfx3dCmdRcgr = 0x59000U;
constexpr uint32_t kGccGfx3dCfgRcgr = 0x59004U;
constexpr uint32_t kGccOxiliGfx3d   = 0x59020U;
constexpr uint32_t kGccOxiliGmem    = 0x59024U;
constexpr uint32_t kGccOxiliAhb     = 0x59028U;
constexpr uint32_t kGccOxiliTimer   = 0x59040U;
constexpr uint32_t kGccOxiliAon     = 0x59044U;
constexpr uint32_t kGccBimcGfx      = 0x59048U;

constexpr uint32_t kGccApcsVote2    = 0x4500CU;
constexpr uint32_t kSmmuVoteBits    = (1U << 2) | (1U << 3) | (1U << 13) | (1U << 19);

// Adreno 510 byte offsets (dword index * 4)
constexpr uint32_t kRbbmAlwaysOnLo  = 0x004D2U * 4U;
constexpr uint32_t kRbbmAlwaysOnHi  = 0x004D3U * 4U;
constexpr uint32_t kRbbmStatus      = 0x004F5U * 4U;

static inline uint32_t rd32(uintptr_t base, uint32_t off)
{
    return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static inline void wr32(uintptr_t base, uint32_t off, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t *>(base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool enableGdsc(uintptr_t gcc, uint32_t off)
{
    // Clear SW_COLLAPSE (BIT(0)) and HW_CONTROL (BIT(1)) so SW enable is not gated.
    wr32(gcc, off, rd32(gcc, off) & ~3U);
    for (uint32_t i = 0; i < 1000U; ++i) {
        if ((rd32(gcc, off) & (1U << 31)) != 0U) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

static bool enableCbcr(uintptr_t gcc, uint32_t off)
{
    wr32(gcc, off, rd32(gcc, off) | 1U);
    for (uint32_t i = 0; i < 500U; ++i) {
        if ((rd32(gcc, off) & (1U << 31)) == 0U) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

static uint64_t readAlwaysOn(uintptr_t gpu)
{
    for (unsigned a = 0; a < 4U; ++a) {
        const uint32_t hi0 = rd32(gpu, kRbbmAlwaysOnHi);
        const uint32_t lo = rd32(gpu, kRbbmAlwaysOnLo);
        const uint32_t hi1 = rd32(gpu, kRbbmAlwaysOnHi);
        if (hi0 == hi1) {
            return (static_cast<uint64_t>(hi0) << 32) | lo;
        }
    }
    return (static_cast<uint64_t>(rd32(gpu, kRbbmAlwaysOnHi)) << 32) | rd32(gpu, kRbbmAlwaysOnLo);
}

} // namespace

extern "C" void kenzo_gpu_probe_run(void)
{
    unsigned mode = 0;
    PE_parse_boot_argn("kenzo-gpu-probe", &mode, sizeof(mode));
    if (mode == 0U) {
        return;
    }

    // Gate #1A: Inspect/enable PM8004 S5 (vdd_gfx, PPID 0x520, QPNP FTS2.5) via SPMI
    kenzo_spmi::Channel gfxCh;
    uint8_t id[2] = {}, ctl[2] = {}, en = 0, sts1 = 0;
    uint32_t gfxMv = 0;
    const bool chOk = kenzo_spmi::openChannel(gfxCh, kPpidVddGfx, mode >= 2U);
    if (chOk && kenzo_spmi::readBytes(gfxCh, 0x04U, id, 2) &&
        kenzo_spmi::readBytes(gfxCh, 0x08U, &sts1, 1) &&
        kenzo_spmi::readBytes(gfxCh, 0x40U, ctl, 2) &&
        kenzo_spmi::readBytes(gfxCh, 0x46U, &en, 1)) {
        if ((en & 0x80U) && (sts1 & 0x80U)) {
            gfxMv = (ctl[0] == 0U) ? (80U + static_cast<uint32_t>(ctl[1]) * 5U)
                                   : (160U + static_cast<uint32_t>(ctl[1]) * 10U);
        } else if (mode >= 2U) {
            // Request Nominal 965 mV (range 0, vset 0xB1), PWM mode (0x80), enable (0x80), poll VREG_OK (0x08 BIT7)
            const uint8_t vcfg[2] = { 0x00U, 0xB1U };
            const uint8_t pwm = 0x80U;
            const uint8_t on = 0x80U;
            if (kenzo_spmi::writeBytes(gfxCh, 0x40U, vcfg, 2) &&
                kenzo_spmi::writeBytes(gfxCh, 0x45U, &pwm, 1) &&
                kenzo_spmi::writeBytes(gfxCh, 0x46U, &on, 1)) {
                for (uint32_t s = 0; s < 50U; ++s) {
                    IODelay(100);
                    if (kenzo_spmi::readBytes(gfxCh, 0x08U, &sts1, 1) && (sts1 & 0x80U)) {
                        break;
                    }
                }
                (void)kenzo_spmi::readBytes(gfxCh, 0x46U, &en, 1);
                if ((en & 0x80U) && (sts1 & 0x80U)) {
                    gfxMv = 965U;
                }
            }
        }
    }
    gfxCh.release();

    IOMemoryMap *gccMap = kenzo_spmi::mapPhys(kGccPhys, kGccSize);
    IOMemoryMap *gpuMap = kenzo_spmi::mapPhys(kGpuRegPhys, kGpuRegSize);
    IOMemoryMap *smmuMap = kenzo_spmi::mapPhys(kSmmuPhys, kSmmuSize);
    if (!gccMap || !gpuMap || !smmuMap) {
        IOLog("KenzoGpuProbe: MMIO map failed\n");
        OSSafeReleaseNULL(gccMap); OSSafeReleaseNULL(gpuMap); OSSafeReleaseNULL(smmuMap);
        return;
    }
    const uintptr_t gcc = static_cast<uintptr_t>(gccMap->getVirtualAddress());
    const uintptr_t gpu = static_cast<uintptr_t>(gpuMap->getVirtualAddress());
    const uintptr_t smmu = static_cast<uintptr_t>(smmuMap->getVirtualAddress());

    const uint32_t cxBefore = rd32(gcc, kGccOxiliCxGdscr);
    const uint32_t gxBefore = rd32(gcc, kGccOxiliGxGdscr);
    const uint32_t cfgBefore = rd32(gcc, kGccGfx3dCfgRcgr);
    kenzo_gpu_probe_diag[0] = gfxMv;
    kenzo_gpu_probe_diag[1] = cxBefore;
    kenzo_gpu_probe_diag[2] = gxBefore;

    IOLog("KenzoGpuProbe: Gate1 pre vdd_gfx=%u mV (en=0x%02x sts1=0x%02x %02x/%02x) CX=0x%x GX=0x%x RCG=0x%x\n",
          gfxMv, en, sts1, id[0], id[1], cxBefore, gxBefore, cfgBefore);

    if (gfxMv == 0U) {
        kenzo_gpu_probe_state = 1U;
        IOLog("KenzoGpuProbe: vdd_gfx OFF/VREG_OK=0 (sts1=0x%02x en=0x%02x)\n", sts1, en);
        gccMap->release(); gpuMap->release(); smmuMap->release();
        return;
    }

    // Ensure MEM_ACC_GFX = 0 (safe for corners 1..3)
    IOMemoryMap *accMap = kenzo_spmi::mapPhys(kMemAccGfxPhys, 0x1000ULL);
    if (accMap) {
        wr32(static_cast<uintptr_t>(accMap->getVirtualAddress()), 0x130U, 0U);
        accMap->release();
    }

    // Step 1: Enable gdsc_oxili_cx (0x5904C) first, then its AHB/AON clocks
    if (!enableGdsc(gcc, kGccOxiliCxGdscr)) {
        kenzo_gpu_probe_state = 2U;
        IOLog("KenzoGpuProbe: CX GDSC timeout CX=0x%x\n", rd32(gcc, kGccOxiliCxGdscr));
        gccMap->release(); gpuMap->release(); smmuMap->release();
        return;
    }
    (void)enableCbcr(gcc, kGccOxiliAhb);
    (void)enableCbcr(gcc, kGccOxiliAon);

    // Step 2: Kenzo DTB /soc/qcom,gdsc@185901c requires qcom,enable-root-clk (gfx3d_clk_src ROOT_EN)!
    // Ensure Oxili BCR (0x59014) is deasserted, clear CLAMP_IO (0x5B00C), and bring up gfx3d_clk_src.
    constexpr uint32_t kGccOxiliBcr = 0x59014U;
    if (rd32(gcc, kGccOxiliBcr) & 1U) {
        wr32(gcc, kGccOxiliBcr, rd32(gcc, kGccOxiliBcr) & ~1U);
        IODelay(20);
    }
    wr32(gcc, kGccOxiliGxClamp, rd32(gcc, kGccOxiliGxClamp) & ~1U);

    // Start gfx3d_clk_src on safe XO 19.2 MHz (cfg=0), UPDATE (BIT0), and ROOT_EN (BIT1).
    wr32(gcc, kGccGfx3dCfgRcgr, 0U);
    wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 1U);
    for (uint32_t i = 0; i < 1000U && (rd32(gcc, kGccGfx3dCmdRcgr) & 1U); ++i) { IODelay(1); }
    wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 2U); // ROOT_EN
    for (uint32_t i = 0; i < 1000U && (rd32(gcc, kGccGfx3dCmdRcgr) & (1U << 31)); ++i) { IODelay(1); }

    // Step 3: Enable gdsc_oxili_gx (0x5901C). If XO did not bring up PWR_ON, retry on GPLL0/16 (50 MHz, cfg=0x11F)
    // and assert FORCE_MEM_CORE_ON on gcc_oxili_gfx3d_clk (0x59020) + gcc_oxili_gmem_clk (0x59024).
    bool gxOk = enableGdsc(gcc, kGccOxiliGxGdscr);
    if (!gxOk) {
        wr32(gcc, kGccGfx3dCfgRcgr, 0x0000011FU); // GPLL0 (src=1, 800 MHz) / 16 = 50 MHz (from Kenzo ftbl)
        wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 1U);
        for (uint32_t i = 0; i < 1000U && (rd32(gcc, kGccGfx3dCmdRcgr) & 1U); ++i) { IODelay(1); }
        wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 2U);
        wr32(gcc, kGccOxiliGfx3d, rd32(gcc, kGccOxiliGfx3d) | (1U << 14));
        wr32(gcc, kGccOxiliGmem, rd32(gcc, kGccOxiliGmem) | (1U << 14));
        gxOk = enableGdsc(gcc, kGccOxiliGxGdscr);
    }
    kenzo_gpu_probe_diag[1] = rd32(gcc, kGccOxiliCxGdscr);
    kenzo_gpu_probe_diag[2] = rd32(gcc, kGccOxiliGxGdscr);
    kenzo_gpu_probe_diag[7] = rd32(gcc, kGccGfx3dCmdRcgr);
    if (!gxOk) {
        kenzo_gpu_probe_state = 3U;
        IOLog("KenzoGpuProbe: GX GDSC timeout GX=0x%x CMD_RCGR=0x%x CLAMP=0x%x BCR=0x%x\n",
              rd32(gcc, kGccOxiliGxGdscr), rd32(gcc, kGccGfx3dCmdRcgr),
              rd32(gcc, kGccOxiliGxClamp), rd32(gcc, kGccOxiliBcr));
        gccMap->release(); gpuMap->release(); smmuMap->release();
        return;
    }
    wr32(gcc, kGccOxiliGxClamp, rd32(gcc, kGccOxiliGxClamp) & ~1U);

    // Step 4: Vote SMMU clocks + enable remaining Oxili branch clocks
    wr32(gcc, kGccApcsVote2, rd32(gcc, kGccApcsVote2) | kSmmuVoteBits);
    const uint32_t smmuHalts[4] = { 0x12020U, 0x12010U, 0x12044U, 0x12098U };
    for (unsigned i = 0; i < 4U; ++i) {
        for (uint32_t s = 0; s < 500U && (rd32(gcc, smmuHalts[i]) & (1U << 31)); ++s) { IODelay(10); }
    }
    const uint32_t clks[6] = {
        kGccOxiliAhb, kGccOxiliGfx3d, kGccOxiliGmem, kGccOxiliTimer, kGccOxiliAon, kGccBimcGfx,
    };
    for (unsigned i = 0; i < 6U; ++i) {
        if (!enableCbcr(gcc, clks[i])) {
            kenzo_gpu_probe_state = 4U;
            IOLog("KenzoGpuProbe: Oxili CBCR 0x%x halt timeout\n", clks[i]);
            gccMap->release(); gpuMap->release(); smmuMap->release();
            return;
        }
    }

    // Gate #1B: RBBM_STATUS & 1 ms ALWAYSON_COUNTER liveness (~19,200 ticks expected)
    const uint32_t rbbmSt = rd32(gpu, kRbbmStatus);
    const uint64_t t0 = readAlwaysOn(gpu);
    IODelay(1000);
    const uint64_t t1 = readAlwaysOn(gpu);
    const uint64_t delta = t1 - t0;

    // Gate #2: SMMUv2 Non-Secure Context Banks CB0 (0x8000) & CB2 (0xA000)
    const uint32_t cb0Sctlr = rd32(smmu, 0x8000U);
    const uint32_t cb2Sctlr = rd32(smmu, 0xA000U);

    kenzo_gpu_probe_diag[3] = rbbmSt;
    kenzo_gpu_probe_diag[4] = static_cast<uint32_t>(delta);
    kenzo_gpu_probe_diag[5] = cb0Sctlr;
    kenzo_gpu_probe_diag[6] = cb2Sctlr;
    kenzo_gpu_probe_state = (delta > 0ULL) ? 5U : 4U;

    IOLog("KenzoGpuProbe: Adreno510 %s! rbbm=0x%x aon_delta=%llu ticks/ms CB0=0x%x CB2=0x%x\n",
          (delta > 0ULL) ? "ALIVE" : "STUCK", rbbmSt,
          static_cast<unsigned long long>(delta), cb0Sctlr, cb2Sctlr);

    gccMap->release();
    gpuMap->release();
    smmuMap->release();
}
