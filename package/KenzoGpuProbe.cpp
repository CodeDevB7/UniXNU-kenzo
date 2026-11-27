// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoGpuProbe - Multi-stage Hardware Gates #1, #2, #3A & #3B bring-up probe
// for A510Metal (Adreno 510 on MSM8956 / Xiaomi Redmi Note 3 kenzo).
//
// Boot-arg stages (switchable via <5s local WSL repack with ZERO kernel rebuild!):
//   kenzo-gpu-probe=0 : Disabled.
//   kenzo-gpu-probe=2 : Gate #1 (vdd_gfx 965 mV + Oxili CX/GX GDSC + 6 clocks + RBBM_ALWAYSON ~19.2k/ms -> G5)
//                       + SMMU clock CBCR diagnostics (BIT 1,2,3,10,13,20) + TZ SCM restore_sec_cfg(0x12, 0).
//                       100% SAFE (does not touch 0x01F08000).
//   kenzo-gpu-probe=3 : Same as =2, plus Gate #2 SMMU CB0 (0x01F08000) & CB2 (0x01F0A000) probe (-> G6).
//                       Optional boot-arg kenzo-gpu-smmu-pre=1 writes 0xFFFFFFFF to 0x01F02000 before SCM.
//   kenzo-gpu-probe=4 : Same as =2, plus Gate #3A RBBM_SECVID_TRUST_CNTL (0x3D000) read/clear(0) & CP status (-> G7).
//   kenzo-gpu-probe=5 : Same as =2, plus Gate #3B FULL a510_hw_init() + /chosen/a510-pm4-fw & a510-pfp-fw
//                       + Ring 0 CP_ME_INIT + CP_MEM_WRITE(0xA510CAFE) hardware smoke test (-> G8!).

#include "KenzoSpmi.h"
#include "NativeAdrenoA510Core.h"
#include "NativeAdrenoA510HwInit.h"
#include "NativeAdrenoA510PM4.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" {
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
volatile uint32_t kenzo_gpu_probe_state = 0; // 0=off,1=vdd,2=cx,3=gx,4=clk,5=Gate1_ALIVE,6=Gate2_SMMU,7=Gate3A_SECVID,8=Gate3B_PM4_OK!
volatile uint32_t kenzo_gpu_probe_diag[8] = {};
void kenzo_gpu_probe_run(void);
}

// Compile A510Metal's host-verified C core directly into this translation unit.
#include "NativeAdrenoA510Core.c"

namespace {

constexpr uint64_t kGccPhys         = 0x01800000ULL;
constexpr uint64_t kGccSize         = 0x00080000ULL;
constexpr uint64_t kGpuRegPhys      = 0x01C00000ULL;
constexpr uint64_t kGpuRegSize      = 0x00040000ULL;
constexpr uint64_t kMemAccGfxPhys   = 0x01944000ULL; // +0x130 MEM_ACC_GFX
constexpr uint64_t kSmmuPhys        = 0x01F00000ULL;
constexpr uint64_t kSmmuSize        = 0x00010000ULL;

constexpr uint16_t kPpidVddGfx      = 0x520U; // PM8004 S5 (SID 5, periph 0x20, FTS2.5 1c/09)

constexpr uint32_t kGccOxiliBcr     = 0x59014U;
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
// 100% silicon-verified directly from stock Kenzo boot.img (0x148eb5c..0x148f7f4):
//   BIT(1)  = gcc_gfx_tcu_clk   (0x12018)
//   BIT(2)  = gcc_gfx_tbu_clk   (0x12020)
//   BIT(3)  = gcc_gfx_1_tbu_clk (0x12010)
//   BIT(10) = gcc_gtcu_ahb_clk  (0x12034)
//   BIT(13) = gcc_apss_tcu_clk  (0x12044)
//   BIT(20) = gcc_smmu_cfg_clk  (0x1209C)
constexpr uint32_t kSmmuVoteBits    = (1U << 1) | (1U << 2) | (1U << 3) | (1U << 10) | (1U << 13) | (1U << 20);

constexpr uint32_t kRbbmAlwaysOnLo  = A510_RBBM_ALWAYSON_COUNTER_LO * 4U;
constexpr uint32_t kRbbmAlwaysOnHi  = A510_RBBM_ALWAYSON_COUNTER_HI * 4U;
constexpr uint32_t kRbbmStatus      = A510_RBBM_STATUS * 4U;

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

static int64_t qcomScmCall3(uint64_t fnId, uint64_t argInfo, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t *r1Out)
{
    register uint64_t x0 __asm__("x0") = fnId;
    register uint64_t x1 __asm__("x1") = argInfo;
    register uint64_t x2 __asm__("x2") = arg0;
    register uint64_t x3 __asm__("x3") = arg1;
    register uint64_t x4 __asm__("x4") = arg2;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    for (unsigned retry = 0; retry < 32U; ++retry) {
        __asm__ volatile("smc #0"
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
                         :
                         : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 != 1ULL) {
            break;
        }
    }
    if (r1Out) {
        *r1Out = x1;
    }
    return static_cast<int64_t>(x0);
}

static int64_t qcomScmCallAuto(uint32_t svcCmd, uint64_t argInfo, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t *r1Out)
{
    int64_t ret = qcomScmCall3(0x42000000ULL | svcCmd, argInfo, arg0, arg1, arg2, r1Out);
    if (ret == -1LL || static_cast<uint32_t>(ret) == 0xFFFFFFFFU) {
        ret = qcomScmCall3(0x02000000ULL | svcCmd, argInfo, arg0, arg1, arg2, r1Out);
    }
    return ret;
}

static int64_t qcomScmRestoreSecCfg(uint32_t devId, uint32_t spare, uint64_t *r1Out)
{
    return qcomScmCallAuto(0x0C02U, 2ULL, devId, spare, 0ULL, r1Out);
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

struct A510ProbeCtx {
    uintptr_t gpuBase;
    uint8_t *dmaVirt;
    uint32_t dmaSize;
};

static uint32_t a510Read32(void *vctx, uint32_t dwordOff)
{
    auto *c = static_cast<A510ProbeCtx *>(vctx);
    return rd32(c->gpuBase, dwordOff * 4U);
}

static void a510Write32(void *vctx, uint32_t dwordOff, uint32_t val)
{
    auto *c = static_cast<A510ProbeCtx *>(vctx);
    wr32(c->gpuBase, dwordOff * 4U, val);
}

static void a510Wmb(void *vctx)
{
    auto *c = static_cast<A510ProbeCtx *>(vctx);
    if (c->dmaVirt && c->dmaSize) {
        flush_dcache64(reinterpret_cast<addr64_t>(c->dmaVirt), c->dmaSize, FALSE);
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void a510Udelay(void *, uint32_t usec)
{
    IODelay(usec);
}

static bool runGate3BSmoke(uintptr_t gpu, unsigned mode)
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) {
        IOLog("KenzoGpuProbe: Gate3B no /chosen node\n");
        return false;
    }
    OSData *pm4Data = OSDynamicCast(OSData, chosen->getProperty("a510-pm4-fw"));
    OSData *pfpData = OSDynamicCast(OSData, chosen->getProperty("a510-pfp-fw"));
    const uint32_t pm4Len = pm4Data ? pm4Data->getLength() : 0U;
    const uint32_t pfpLen = pfpData ? pfpData->getLength() : 0U;
    if (!pm4Data || !pfpData || pm4Len < 1024U || pm4Len > 0x6000U || pfpLen < 1024U || pfpLen > 0x5000U) {
        IOLog("KenzoGpuProbe: Gate3B missing /chosen firmware (pm4=%u pfp=%u)\n", pm4Len, pfpLen);
        chosen->release();
        return false;
    }

    // Single 128 KiB physically contiguous DMA block strictly below 4 GiB:
    //   +0x00000 (32 KiB) Ring 0
    //   +0x08000 (24 KiB) a530_pm4.fw (header-stripped)
    //   +0x0E000 (20 KiB) a530_pfp.fw (header-stripped)
    //   +0x13000 ( 4 KiB) RPTR shadow
    //   +0x14000 ( 4 KiB) Fence page
    //   +0x15000 ( 4 KiB) Scratch target #1 (expects 0xA510CAFE) & #2 (+0x100: expects 0xA5101B01)
    //   +0x16000 ( 4 KiB) UCHE trap page
    //   +0x17000 ( 4 KiB) Indirect Buffer (Gate #4 CP_INDIRECT_BUFFER_PFE)
    constexpr uint32_t kDmaBytes    = 0x20000U;
    constexpr uint32_t kPm4Off      = 0x08000U;
    constexpr uint32_t kPfpOff      = 0x0E000U;
    constexpr uint32_t kRptrOff     = 0x13000U;
    constexpr uint32_t kFenceOff    = 0x14000U;
    constexpr uint32_t kScratchOff  = 0x15000U;
    constexpr uint32_t kScratch2Off = 0x15100U;
    constexpr uint32_t kTrapOff     = 0x16000U;
    constexpr uint32_t kIbOff       = 0x17000U;

    const IOOptionBits opts = static_cast<IOOptionBits>(kIODirectionInOut) |
                              static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    IOBufferMemoryDescriptor *dmaDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, opts, kDmaBytes, 0xFFFFF000ULL);
    if (!dmaDesc || dmaDesc->prepare() != kIOReturnSuccess) {
        IOLog("KenzoGpuProbe: Gate3B DMA alloc/prepare failed\n");
        OSSafeReleaseNULL(dmaDesc);
        chosen->release();
        return false;
    }

    IOByteCount segLen = 0;
    const uint64_t dmaPa = static_cast<uint64_t>(dmaDesc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone));
    uint8_t *dmaVa = static_cast<uint8_t *>(dmaDesc->getBytesNoCopy());
    if (!dmaVa || !dmaPa || segLen < kDmaBytes || (dmaPa + kDmaBytes) > 0x100000000ULL) {
        IOLog("KenzoGpuProbe: Gate3B bad DMA seg pa=0x%llx len=0x%llx\n",
              static_cast<unsigned long long>(dmaPa), static_cast<unsigned long long>(segLen));
        dmaDesc->complete();
        dmaDesc->release();
        chosen->release();
        return false;
    }

    memset(dmaVa, 0, kDmaBytes);
    memcpy(dmaVa + kPm4Off, pm4Data->getBytesNoCopy(), pm4Len);
    memcpy(dmaVa + kPfpOff, pfpData->getBytesNoCopy(), pfpLen);
    chosen->release();

    volatile uint32_t *fenceVa = reinterpret_cast<volatile uint32_t *>(dmaVa + kFenceOff);
    volatile uint32_t *scratchVa = reinterpret_cast<volatile uint32_t *>(dmaVa + kScratchOff);
    volatile uint32_t *scratch2Va = reinterpret_cast<volatile uint32_t *>(dmaVa + kScratch2Off);
    *fenceVa = 0U;
    *scratchVa = 0xDEADBEEFU;
    *scratch2Va = 0xDEADBEEFU;

    // Prepare Gate #4 Indirect Buffer at +0x17000: CP_MEM_WRITE(scratch2, 0xA5101B01) + CP_NOP
    uint32_t *ibWords = reinterpret_cast<uint32_t *>(dmaVa + kIbOff);
    const uint64_t scratch2Pa = dmaPa + kScratch2Off;
    ibWords[0] = a510_pm4_type7_header(A510_CP_MEM_WRITE, 3U);
    ibWords[1] = static_cast<uint32_t>(scratch2Pa & 0xFFFFFFFFULL);
    ibWords[2] = static_cast<uint32_t>(scratch2Pa >> 32);
    ibWords[3] = 0xA5101B01U;
    ibWords[4] = a510_pm4_type7_header(A510_CP_NOP, 0U);
    flush_dcache64(reinterpret_cast<addr64_t>(dmaVa), kDmaBytes, FALSE);

    A510ProbeCtx ctx = { gpu, dmaVa, kDmaBytes };
    A510CoreOps ops = { &ctx, &a510Read32, &a510Write32, &a510Wmb, &a510Udelay };
    A510Ring ring = {};
    (void)a510_ring_init(&ring,
                         reinterpret_cast<uint32_t *>(dmaVa),
                         dmaPa,
                         dmaPa + kRptrOff,
                         dmaPa + kFenceOff);

    unsigned noSecvid = 0, noHwcg = 0, noProt = 0, noIrq = 0, noEv = 0, clrInt = 0, diagMode = 0, trapPage = 0;
    PE_parse_boot_argn("kenzo-gpu-nosecvid", &noSecvid, sizeof(noSecvid));
    PE_parse_boot_argn("kenzo-gpu-nohwcg", &noHwcg, sizeof(noHwcg));
    PE_parse_boot_argn("kenzo-gpu-noprot", &noProt, sizeof(noProt));
    PE_parse_boot_argn("kenzo-gpu-noirq", &noIrq, sizeof(noIrq));
    PE_parse_boot_argn("kenzo-gpu-noev", &noEv, sizeof(noEv));
    PE_parse_boot_argn("kenzo-gpu-clrint", &clrInt, sizeof(clrInt));
    PE_parse_boot_argn("kenzo-gpu-diag", &diagMode, sizeof(diagMode));
    PE_parse_boot_argn("kenzo-gpu-trappage", &trapPage, sizeof(trapPage));

    A510HwInitConfig cfg = {};
    cfg.pm4_fw_iova = dmaPa + kPm4Off;
    cfg.pfp_fw_iova = dmaPa + kPfpOff;
    cfg.uche_trap_iova = (trapPage != 0U) ? (dmaPa + kTrapOff) : 0x0001FFFFFFFF0000ULL;
    cfg.clear_secvid_trust_cntl = (noSecvid == 0U);
    cfg.skip_hwcg = (noHwcg != 0U);
    cfg.skip_cp_protect = (noProt != 0U);

    IOLog("KenzoGpuProbe: Gate3B starting a510_hw_init pa=0x%llx pm4=%u pfp=%u nohwcg=%u noirq=%u noev=%u\n",
          static_cast<unsigned long long>(dmaPa), pm4Len, pfpLen, noHwcg, noIrq, noEv);

    const A510CoreStatus initSt = a510_hw_init(&ops, &ring, &cfg);
    uint32_t seqno = 0, seqno2 = 0;
    A510CoreStatus smokeSt = A510_CORE_ERR_INVALID;
    A510CoreStatus idleSt = A510_CORE_ERR_INVALID;
    A510CoreStatus ibSt = A510_CORE_ERR_INVALID;
    A510CoreStatus idle2St = A510_CORE_ERR_INVALID;
    if (initSt == A510_CORE_OK) {
        smokeSt = a510_ring_submit_smoke_write_ex(&ops, &ring, dmaPa + kScratchOff, 0xA510CAFEU,
                                                  (noEv == 0U), (noIrq == 0U), &seqno);
        if (smokeSt == A510_CORE_OK) {
            if (clrInt != 0U) {
                IODelay(2000);
                wr32(gpu, A510_RBBM_INT_CLEAR_CMD * 4U, rd32(gpu, A510_RBBM_INT_0_STATUS * 4U));
            }
            idleSt = a510_wait_idle(&ops, &ring, 2000U);
            if (mode >= 6U && idleSt == A510_CORE_OK) {
                const A510IbDesc ib = { dmaPa + kIbOff, 5U };
                ibSt = a510_ring_submit_ibs_ex(&ops, &ring, &ib, 1U, (noIrq == 0U), &seqno2);
                if (ibSt == A510_CORE_OK) {
                    idle2St = a510_wait_idle(&ops, &ring, 2000U);
                }
            }
        }
    }

    flush_dcache64(reinterpret_cast<addr64_t>(dmaVa + kRptrOff), 0x3000U, FALSE);
    const uint32_t gotScratch = *scratchVa;
    const uint32_t gotScratch2 = *scratch2Va;
    const uint32_t gotFence = *fenceVa;
    const uint32_t rptr = rd32(gpu, A510_CP_RB_RPTR * 4U);
    const uint32_t rbbmPost = rd32(gpu, kRbbmStatus);
    const uint32_t intSt = rd32(gpu, A510_RBBM_INT_0_STATUS * 4U);
    const uint32_t hwFault = rd32(gpu, A510_CP_HW_FAULT * 4U);
    if (intSt != 0U) {
        wr32(gpu, A510_RBBM_INT_CLEAR_CMD * 4U, intSt);
    }

    kenzo_gpu_probe_diag[3] = rbbmPost;
    if (diagMode == 1U) {
        // Diagnostic view: x=RBBM_INT_0_STATUS[15:0], y=RBBM_INT_0_STATUS[31:16], z=(fence<<4)|(idleSt&0xF)
        kenzo_gpu_probe_diag[5] = intSt & 0xFFFFU;
        kenzo_gpu_probe_diag[6] = (intSt >> 16) & 0xFFFFU;
        kenzo_gpu_probe_diag[7] = ((gotFence & 0xFU) << 4) | (static_cast<uint32_t>(idleSt) & 0xFU);
    } else if (diagMode == 2U) {
        // Diagnostic view 2: x=CP_HW_FAULT[15:0], y=CP_HW_FAULT[31:16], z=(gotScratch2==0xA5101B01 ? 0x1B : gotFence)
        kenzo_gpu_probe_diag[5] = hwFault & 0xFFFFU;
        kenzo_gpu_probe_diag[6] = (hwFault >> 16) & 0xFFFFU;
        kenzo_gpu_probe_diag[7] = (gotFence & 0xFFU);
    } else {
        kenzo_gpu_probe_diag[5] = (mode >= 6U && gotScratch2 == 0xA5101B01U) ? gotScratch2 : gotScratch;
        kenzo_gpu_probe_diag[6] = (gotFence << 16) | (rptr & 0xFFFFU);
        kenzo_gpu_probe_diag[7] = ((gotFence & 0xFU) << 4) | (static_cast<uint32_t>(initSt == A510_CORE_OK ? idleSt : initSt) & 0xFU);
    }

    IOLog("KenzoGpuProbe: Gate3B/4 init=%d smoke=%d idle=%d ib=%d idle2=%d sc1=0x%08x sc2=0x%08x fence=%u rptr=%u/%u rbbm=0x%x int=0x%x flt=0x%x\n",
          static_cast<int>(initSt), static_cast<int>(smokeSt), static_cast<int>(idleSt),
          static_cast<int>(ibSt), static_cast<int>(idle2St),
          gotScratch, gotScratch2, gotFence, rptr, ring.wptr, rbbmPost, intSt, hwFault);

    // Keep dmaDesc alive if GPU successfully ran (ring remains referenced by CP_RB_BASE).
    if (mode >= 6U && initSt == A510_CORE_OK && idleSt == A510_CORE_OK && idle2St == A510_CORE_OK &&
        gotScratch == 0xA510CAFEU && gotScratch2 == 0xA5101B01U) {
        kenzo_gpu_probe_state = 9U; // G9 = PHYSICAL ADRENO 510 INDIRECT BUFFER 0xA5101B01 + FENCE VERIFIED!
        return true;
    }
    if (initSt == A510_CORE_OK && idleSt == A510_CORE_OK && gotScratch == 0xA510CAFEU) {
        kenzo_gpu_probe_state = 8U; // G8 = PHYSICAL ADRENO 510 PM4 SMOKE WRITE 0xA510CAFE + CLEAN IDLE VERIFIED!
        return true;
    }
    return false;
}

} // namespace

extern "C" void kenzo_gpu_probe_run(void)
{
    unsigned mode = 0;
    PE_parse_boot_argn("kenzo-gpu-probe", &mode, sizeof(mode));
    if (mode == 0U) {
        return;
    }

    // Gate #1A: Inspect/enable PM8004 S5 (vdd_gfx, PPID 0x520, QPNP FTS2.5 1c/09) via SPMI
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
            const uint8_t vcfg[2] = { 0x00U, 0xB1U }; // 965 mV (Nominal)
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

    IOLog("KenzoGpuProbe: Gate1 pre mode=%u vdd_gfx=%u mV (en=0x%02x sts1=0x%02x %02x/%02x) CX=0x%x GX=0x%x RCG=0x%x\n",
          mode, gfxMv, en, sts1, id[0], id[1], cxBefore, gxBefore, cfgBefore);

    if (gfxMv == 0U) {
        kenzo_gpu_probe_state = 1U;
        gccMap->release(); gpuMap->release(); smmuMap->release();
        return;
    }

    // MEM_ACC_GFX = 0 (safe for corners 1..3)
    IOMemoryMap *accMap = kenzo_spmi::mapPhys(kMemAccGfxPhys, 0x1000ULL);
    if (accMap) {
        wr32(static_cast<uintptr_t>(accMap->getVirtualAddress()), 0x130U, 0U);
        accMap->release();
    }

    // Step 1: Enable gdsc_oxili_cx (0x5904C) + AHB/AON
    if (!enableGdsc(gcc, kGccOxiliCxGdscr)) {
        kenzo_gpu_probe_state = 2U;
        gccMap->release(); gpuMap->release(); smmuMap->release();
        return;
    }
    (void)enableCbcr(gcc, kGccOxiliAhb);
    (void)enableCbcr(gcc, kGccOxiliAon);

    // Step 2: Deassert Oxili BCR, clear CLAMP_IO, start gfx3d_clk_src (XO -> GPLL0/16 50 MHz fallback)
    if (rd32(gcc, kGccOxiliBcr) & 1U) {
        wr32(gcc, kGccOxiliBcr, rd32(gcc, kGccOxiliBcr) & ~1U);
        IODelay(20);
    }
    wr32(gcc, kGccOxiliGxClamp, rd32(gcc, kGccOxiliGxClamp) & ~1U);

    wr32(gcc, kGccGfx3dCfgRcgr, 0U);
    wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 1U);
    for (uint32_t i = 0; i < 1000U && (rd32(gcc, kGccGfx3dCmdRcgr) & 1U); ++i) { IODelay(1); }
    wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 2U);
    for (uint32_t i = 0; i < 1000U && (rd32(gcc, kGccGfx3dCmdRcgr) & (1U << 31)); ++i) { IODelay(1); }

    bool gxOk = enableGdsc(gcc, kGccOxiliGxGdscr);
    if (!gxOk) {
        wr32(gcc, kGccGfx3dCfgRcgr, 0x0000011FU); // GPLL0 / 16 = 50 MHz
        wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 1U);
        for (uint32_t i = 0; i < 1000U && (rd32(gcc, kGccGfx3dCmdRcgr) & 1U); ++i) { IODelay(1); }
        wr32(gcc, kGccGfx3dCmdRcgr, rd32(gcc, kGccGfx3dCmdRcgr) | 2U);
        wr32(gcc, kGccOxiliGfx3d, rd32(gcc, kGccOxiliGfx3d) | (1U << 14));
        wr32(gcc, kGccOxiliGmem, rd32(gcc, kGccOxiliGmem) | (1U << 14));
        gxOk = enableGdsc(gcc, kGccOxiliGxGdscr);
    }
    kenzo_gpu_probe_diag[1] = rd32(gcc, kGccOxiliCxGdscr);
    kenzo_gpu_probe_diag[2] = rd32(gcc, kGccOxiliGxGdscr);
    if (!gxOk) {
        kenzo_gpu_probe_state = 3U;
        gccMap->release(); gpuMap->release(); smmuMap->release();
        return;
    }
    wr32(gcc, kGccOxiliGxClamp, rd32(gcc, kGccOxiliGxClamp) & ~1U);

    // Step 3: Vote SMMU clocks (BIT 1,2,3,10,13,20) + record exact CBCR status + enable 6 Oxili clocks
    wr32(gcc, kGccApcsVote2, rd32(gcc, kGccApcsVote2) | kSmmuVoteBits);
    const uint32_t smmuHalts[6] = { 0x12018U, 0x12020U, 0x12010U, 0x12034U, 0x12044U, 0x1209CU };
    uint32_t smmuVals[6] = {};
    uint32_t smmuOnMask = 0U;
    for (unsigned i = 0; i < 6U; ++i) {
        for (uint32_t s = 0; s < 500U && (rd32(gcc, smmuHalts[i]) & (1U << 31)); ++s) { IODelay(10); }
        smmuVals[i] = rd32(gcc, smmuHalts[i]);
        if ((smmuVals[i] & (1U << 31)) == 0U) {
            smmuOnMask |= (1U << i);
        }
    }
    const uint32_t voteRb = rd32(gcc, kGccApcsVote2);
    IOLog("KenzoGpuProbe: SMMU vote=0x%x onMask=0x%x cbcr=0x%x,0x%x,0x%x,0x%x,0x%x,0x%x\n",
          voteRb, smmuOnMask,
          smmuVals[0], smmuVals[1], smmuVals[2], smmuVals[3], smmuVals[4], smmuVals[5]);

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

    // Gate #1B: RBBM_STATUS & 1 ms ALWAYSON_COUNTER liveness (~19,200 ticks/ms)
    const uint32_t rbbmSt = rd32(gpu, kRbbmStatus);
    const uint64_t t0 = readAlwaysOn(gpu);
    IODelay(1000);
    const uint64_t t1 = readAlwaysOn(gpu);
    const uint64_t delta = t1 - t0;

    kenzo_gpu_probe_diag[3] = rbbmSt;
    kenzo_gpu_probe_diag[4] = static_cast<uint32_t>(delta);
    kenzo_gpu_probe_diag[5] = smmuOnMask;
    kenzo_gpu_probe_diag[6] = smmuVals[5]; // gcc_smmu_cfg_clk (0x1209C)
    kenzo_gpu_probe_state = (delta > 0ULL) ? 5U : 4U;

    IOLog("KenzoGpuProbe: Gate1 Adreno510 %s! rbbm=0x%x aon_delta=%llu ticks/ms\n",
          (delta > 0ULL) ? "ALIVE" : "STUCK", rbbmSt,
          static_cast<unsigned long long>(delta));

    // Call TZ SCM restore_sec_cfg(0x12, 0) (verified ret=0 on Kenzo)
    unsigned smmuPre = 0;
    PE_parse_boot_argn("kenzo-gpu-smmu-pre", &smmuPre, sizeof(smmuPre));
    if (mode == 3U && smmuPre == 1U) {
        wr32(smmu, 0x2000U, 0xFFFFFFFFU);
    }
    uint64_t scmR1 = 0;
    const int64_t scmRet = qcomScmRestoreSecCfg(0x12U, 0U, &scmR1);
    kenzo_gpu_probe_diag[7] = static_cast<uint32_t>(scmRet);
    IOLog("KenzoGpuProbe: SCM restore_sec_cfg(0x12,0) ret=%lld r1=0x%llx\n",
          static_cast<long long>(scmRet), static_cast<unsigned long long>(scmR1));

    if (mode == 3U && scmRet == 0LL) {
        // Optional TZ IOMMU_SECURE_PTBL_SIZE (0x02000C03) / PTBL_INIT (0x02000C04) (kenzo-gpu-smmu-ptbl=1/2/3)
        unsigned smmuPtbl = 0;
        PE_parse_boot_argn("kenzo-gpu-smmu-ptbl", &smmuPtbl, sizeof(smmuPtbl));
        if (smmuPtbl >= 1U) {
            uint64_t ptblSz = 0;
            const int64_t ptblRet = qcomScmCallAuto(0x0C03U, 1ULL, 0ULL, 0ULL, 0ULL, &ptblSz);
            kenzo_gpu_probe_diag[5] = static_cast<uint32_t>(ptblSz & 0xFFFFU);
            kenzo_gpu_probe_diag[6] = static_cast<uint32_t>((ptblSz >> 16) & 0xFFFFU);
            kenzo_gpu_probe_diag[7] = static_cast<uint32_t>(ptblRet);
            IOLog("KenzoGpuProbe: SCM IOMMU_SECURE_PTBL_SIZE ret=%lld size=0x%llx\n",
                  static_cast<long long>(ptblRet), static_cast<unsigned long long>(ptblSz));
            if (smmuPtbl >= 2U && ptblRet == 0LL && ptblSz >= 0x1000ULL && ptblSz <= 0x2000000ULL) {
                const IOOptionBits pOpts = static_cast<IOOptionBits>(kIODirectionInOut) |
                                           static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
                IOBufferMemoryDescriptor *ptblDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                    kernel_task, pOpts, static_cast<vm_size_t>(ptblSz), 0xFFF00000ULL);
                if (ptblDesc && ptblDesc->prepare() == kIOReturnSuccess) {
                    IOByteCount pLen = 0;
                    const uint64_t pPa = static_cast<uint64_t>(ptblDesc->getPhysicalSegment(0, &pLen, kIOMemoryMapperNone));
                    uint8_t *pVa = static_cast<uint8_t *>(ptblDesc->getBytesNoCopy());
                    if (pVa && pPa && pLen >= ptblSz) {
                        memset(pVa, 0, static_cast<size_t>(ptblSz));
                        flush_dcache64(reinterpret_cast<addr64_t>(pVa), static_cast<unsigned>(ptblSz), FALSE);
                        uint64_t initR1 = 0, rsc2R1 = 0;
                        // arginfo = 0x23 (3 args: RO_BUF_ADDR=2, VAL=0, VAL=0 -> verified at 0xd07154!)
                        const int64_t initRet = qcomScmCallAuto(0x0C04U, 0x23ULL, pPa, ptblSz, 0ULL, &initR1);
                        const int64_t rsc2Ret = qcomScmRestoreSecCfg(0x12U, 0U, &rsc2R1);
                        kenzo_gpu_probe_diag[5] = static_cast<uint32_t>(initRet & 0xFFFFU);
                        kenzo_gpu_probe_diag[6] = static_cast<uint32_t>(initR1 & 0xFFFFU);
                        kenzo_gpu_probe_diag[7] = static_cast<uint32_t>(rsc2Ret);
                        IOLog("KenzoGpuProbe: SCM PTBL_INIT pa=0x%llx sz=0x%llx ret=%lld r1=0x%llx rsc2=%lld\n",
                              static_cast<unsigned long long>(pPa), static_cast<unsigned long long>(ptblSz),
                              static_cast<long long>(initRet), static_cast<unsigned long long>(initR1),
                              static_cast<long long>(rsc2Ret));
                        if (smmuPtbl >= 3U && initRet == 0LL && rsc2Ret == 0LL) {
                            const uint32_t cb0Sctlr = rd32(smmu, 0x8000U);
                            const uint32_t cb2Sctlr = rd32(smmu, 0xA000U);
                            kenzo_gpu_probe_diag[5] = cb0Sctlr;
                            kenzo_gpu_probe_diag[6] = cb2Sctlr;
                            kenzo_gpu_probe_state = 6U;
                            IOLog("KenzoGpuProbe: Gate2 post-PTBL_INIT CB0=0x%x CB2=0x%x\n", cb0Sctlr, cb2Sctlr);
                        }
                    }
                }
            }
        } else {
            // Gate #2: SMMUv2 Non-Secure Context Banks CB0 (0x8000) & CB2 (0xA000)
            const uint32_t cb0Sctlr = rd32(smmu, 0x8000U);
            const uint32_t cb2Sctlr = rd32(smmu, 0xA000U);
            kenzo_gpu_probe_diag[5] = cb0Sctlr;
            kenzo_gpu_probe_diag[6] = cb2Sctlr;
            if (delta > 0ULL) {
                kenzo_gpu_probe_state = 6U;
            }
            IOLog("KenzoGpuProbe: Gate2 SMMU CB0=0x%x CB2=0x%x\n", cb0Sctlr, cb2Sctlr);
        }
    } else if (mode == 4U && delta > 0ULL) {
        // Gate #3A: Test EL1 read/clear of RBBM_SECVID_TRUST_CNTL (0x0F400 * 4 = 0x3D000) & CP status
        const uint32_t secBefore = rd32(gpu, A510_RBBM_SECVID_TRUST_CNTL * 4U);
        wr32(gpu, A510_RBBM_SECVID_TRUST_CNTL * 4U, 0U);
        const uint32_t secAfter = rd32(gpu, A510_RBBM_SECVID_TRUST_CNTL * 4U);
        const uint32_t st3 = rd32(gpu, A510_RBBM_STATUS3 * 4U);
        const uint32_t cpCtl = rd32(gpu, A510_CP_PFP_ME_CNTL * 4U);
        kenzo_gpu_probe_diag[5] = (secBefore << 16) | (secAfter & 0xFFFFU);
        kenzo_gpu_probe_diag[6] = st3;
        kenzo_gpu_probe_diag[7] = cpCtl;
        kenzo_gpu_probe_state = 7U;
        IOLog("KenzoGpuProbe: Gate3A SECVID 0x%x->0x%x STATUS3=0x%x CP_CNTL=0x%x\n",
              secBefore, secAfter, st3, cpCtl);
    } else if (mode >= 5U && delta > 0ULL) {
        // Gate #3B (mode=5) & Gate #4 IB (mode=6)
        (void)runGate3BSmoke(gpu, mode);
    }

    gccMap->release();
    gpuMap->release();
    smmuMap->release();
}
