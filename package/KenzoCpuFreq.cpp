// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoCpuFreq - Qualcomm MSM8956 (Snapdragon 650) cluster clock probe & safe Nominal scaler.
//
// Hardware addresses verified from kenzo-dump/extracted_dtb_0.dtb & stock clock-cpu-8976.c:
//   NOTE Cluster 0 vs Cluster 1 address inversion on MSM8956:
//   - Cluster 0 (4x Cortex-A53): MUX 0x0B111050, SR PLL  0x0B116000, PMIC pm8950_s5 (PPID 0x120)
//   - Cluster 1 (2x Cortex-A72): MUX 0x0B011050, HFPLL   0x0B016000, PMIC pm8004_s2 (PPID 0x517)
//   - CCI Interconnect:          MUX 0x0B1D1050, SR PLL  0x0B1D0000
//   - Speed/PVS efuse:           0x000A412C
//
// Boot-arg:
//   kenzo-cpufreq=1 : Read-only hardware probe (dumps live MUX, PLL, lock bits, and SPMI mV).
//   kenzo-cpufreq=2 : Safe Nominal boost ONLY if live Vdd >= 965 mV (A53 -> 1017.6 MHz L=53,
//                     A72 -> 1382.4 MHz L=72). Never enters Turbo without RPM-SMD vdd_mx.

#include "KenzoSpmi.h"
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" {
volatile uint32_t kenzo_cpufreq_state[8] = {};
void kenzo_cpufreq_run(void);
}

namespace {

constexpr uint64_t kC0MuxPhys  = 0x0B111000ULL; // +0x50 CMD_RCGR, +0x54 CFG_RCGR
constexpr uint64_t kC0PllPhys  = 0x0B116000ULL; // a53ss_sr_pll (SR PLL)
constexpr uint64_t kC1MuxPhys  = 0x0B011000ULL; // +0x50 CMD_RCGR, +0x54 CFG_RCGR
constexpr uint64_t kC1PllPhys  = 0x0B016000ULL; // a72ss_hf_pll (HFPLL)
constexpr uint64_t kCciMuxPhys = 0x0B1D1000ULL; // +0x50 CMD_RCGR, +0x54 CFG_RCGR
constexpr uint64_t kCciPllPhys = 0x0B1D0000ULL; // cci_sr_pll (SR PLL)

constexpr uint32_t kMuxCmdOff  = 0x50U;
constexpr uint32_t kMuxCfgOff  = 0x54U;

constexpr uint32_t kPllMode    = 0x00U;
constexpr uint32_t kPllLVal    = 0x04U;
constexpr uint32_t kPllMVal    = 0x08U;
constexpr uint32_t kPllNVal    = 0x0CU;
constexpr uint32_t kPllUserCtl = 0x10U;
constexpr uint32_t kPllCfgCtl  = 0x14U;
constexpr uint32_t kPllStatus  = 0x1CU;

constexpr uint32_t kPllOutCtrl = 1U << 0;
constexpr uint32_t kPllBypass  = 1U << 1;
constexpr uint32_t kPllResetN  = 1U << 2;
constexpr uint32_t kPllLockMsk = (1U << 16) | (1U << 31);

constexpr uint16_t kPpidPm8950S5 = 0x120U; // A53 + CCI rail
constexpr uint16_t kPpidPm8004S2 = 0x517U; // A72 rail

static uint32_t rd32(uintptr_t base, uint32_t off)
{
    return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static void wr32(uintptr_t base, uint32_t off, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t *>(base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t estimateFreqKhz(uint32_t cfgRcgr, uint32_t pllL, uint32_t pllUser)
{
    const uint32_t src = (cfgRcgr >> 8) & 0x7U;
    const uint32_t div2 = cfgRcgr & 0x1FU; // divider = (div2 + 1) / 2
    uint32_t parentKhz = 19200U;
    if (src == 1U) {
        parentKhz = 1200000U; // GPLL4
    } else if (src == 4U) {
        parentKhz = 800000U;  // GPLL0_AO
    } else if (src == 5U) {
        parentKhz = pllL * 19200U;
    } else if (src == 3U) {
        const uint32_t pdiv = ((pllUser >> 8) & 0x3U) + 1U;
        parentKhz = (pllL * 19200U) / pdiv;
    }
    return (div2 <= 1U) ? parentKhz : (parentKhz * 2U) / (div2 + 1U);
}

static bool switchMux(uintptr_t muxBase, uint32_t srcSel, uint32_t srcDiv)
{
    uint32_t cfg = rd32(muxBase, kMuxCfgOff);
    cfg &= ~((0x7U << 8) | 0x1FU);
    cfg |= ((srcSel & 0x7U) << 8) | (srcDiv & 0x1FU);
    wr32(muxBase, kMuxCfgOff, cfg);
    wr32(muxBase, kMuxCmdOff, rd32(muxBase, kMuxCmdOff) | 0x1U);
    for (uint32_t spin = 0; spin < 5000U; ++spin) {
        if ((rd32(muxBase, kMuxCmdOff) & 0x1U) == 0U) {
            return true;
        }
        IODelay(1);
    }
    return false;
}

// Read Qualcomm PM8950/PM8004 FTS2.5/HF buck voltage in mV via SPMI observer (read-only).
static uint32_t readBuckMv(uint16_t ppid)
{
    kenzo_spmi::Channel ch;
    if (!kenzo_spmi::openChannel(ch, ppid, false)) {
        return 0;
    }
    uint8_t vset[2] = {};
    uint8_t range = 0;
    uint8_t en = 0;
    const bool ok = kenzo_spmi::readBytes(ch, 0x40U, vset, 2) &&
                    kenzo_spmi::readBytes(ch, 0x43U, &range, 1) &&
                    kenzo_spmi::readBytes(ch, 0x46U, &en, 1);
    ch.release();
    if (!ok || (en & 0x80U) == 0) {
        return 0;
    }
    // FTS2.5 range 0: 350 mV + 5 mV * step; range 1: 700 mV + 10 mV * step.
    const uint32_t step = vset[0];
    if ((range & 0x7U) == 0U) {
        return 350U + step * 5U;
    }
    return 700U + step * 10U;
}

static bool reprogramClusterPll(const char *tag, uintptr_t muxBase, uintptr_t pllBase,
                                uint32_t targetL, uint32_t cfgCtl, uint32_t userClearMask, uint32_t userSetMask)
{
    // 1. Park cluster safely on GPLL0_AO / 2 (400 MHz) before touching its PLL.
    if (!switchMux(muxBase, 4U, 3U)) {
        IOLog("KenzoCpuFreq: %s failed to park on GPLL0/2\n", tag);
        return false;
    }

    // 2. Disable PLL, program integer L multiplier & VCO config, then re-lock.
    wr32(pllBase, kPllMode, 0U);
    IODelay(5);
    wr32(pllBase, kPllLVal, targetL);
    wr32(pllBase, kPllMVal, 0U);
    wr32(pllBase, kPllNVal, 0U);
    uint32_t uctl = rd32(pllBase, kPllUserCtl);
    uctl = (uctl & ~userClearMask) | userSetMask | (1U << 3) | (1U << 0); // EARLY_OUT_EN | MAIN_OUT_EN
    wr32(pllBase, kPllUserCtl, uctl);
    wr32(pllBase, kPllCfgCtl, cfgCtl);

    wr32(pllBase, kPllMode, kPllBypass);
    IODelay(10);
    wr32(pllBase, kPllMode, kPllBypass | kPllResetN);

    bool locked = false;
    for (uint32_t spin = 0; spin < 2000U; ++spin) {
        if ((rd32(pllBase, kPllStatus) & kPllLockMsk) != 0U) {
            locked = true;
            break;
        }
        IODelay(5);
    }
    if (!locked) {
        IOLog("KenzoCpuFreq: %s PLL lock timeout (status=0x%x), staying on 800 MHz GPLL0\n",
              tag, rd32(pllBase, kPllStatus));
        (void)switchMux(muxBase, 4U, 1U); // 800 MHz GPLL0 safe fallback
        return false;
    }

    wr32(pllBase, kPllMode, kPllBypass | kPllResetN | kPllOutCtrl);
    IODelay(5);

    // 3. Switch mux directly to cluster PLL early output (src_sel=5, div=1).
    if (!switchMux(muxBase, 5U, 1U)) {
        IOLog("KenzoCpuFreq: %s failed to switch mux to PLL\n", tag);
        return false;
    }
    return true;
}

} // namespace

extern "C" void kenzo_cpufreq_run(void)
{
    unsigned mode = 0;
    PE_parse_boot_argn("kenzo-cpufreq", &mode, sizeof(mode));
    if (mode == 0U) {
        return;
    }

    IOMemoryMap *c0MuxMap = kenzo_spmi::mapPhys(kC0MuxPhys, 0x1000ULL);
    IOMemoryMap *c0PllMap = kenzo_spmi::mapPhys(kC0PllPhys, 0x1000ULL);
    IOMemoryMap *c1MuxMap = kenzo_spmi::mapPhys(kC1MuxPhys, 0x1000ULL);
    IOMemoryMap *c1PllMap = kenzo_spmi::mapPhys(kC1PllPhys, 0x1000ULL);
    IOMemoryMap *cciMuxMap = kenzo_spmi::mapPhys(kCciMuxPhys, 0x1000ULL);
    IOMemoryMap *cciPllMap = kenzo_spmi::mapPhys(kCciPllPhys, 0x1000ULL);
    if (!c0MuxMap || !c0PllMap || !c1MuxMap || !c1PllMap || !cciMuxMap || !cciPllMap) {
        IOLog("KenzoCpuFreq: MMIO map failed\n");
        OSSafeReleaseNULL(c0MuxMap); OSSafeReleaseNULL(c0PllMap);
        OSSafeReleaseNULL(c1MuxMap); OSSafeReleaseNULL(c1PllMap);
        OSSafeReleaseNULL(cciMuxMap); OSSafeReleaseNULL(cciPllMap);
        return;
    }

    const uintptr_t c0Mux = static_cast<uintptr_t>(c0MuxMap->getVirtualAddress());
    const uintptr_t c0Pll = static_cast<uintptr_t>(c0PllMap->getVirtualAddress());
    const uintptr_t c1Mux = static_cast<uintptr_t>(c1MuxMap->getVirtualAddress());
    const uintptr_t c1Pll = static_cast<uintptr_t>(c1PllMap->getVirtualAddress());
    const uintptr_t cciMux = static_cast<uintptr_t>(cciMuxMap->getVirtualAddress());
    const uintptr_t cciPll = static_cast<uintptr_t>(cciPllMap->getVirtualAddress());

    const uint32_t c0Cfg = rd32(c0Mux, kMuxCfgOff);
    const uint32_t c0L = rd32(c0Pll, kPllLVal);
    const uint32_t c0U = rd32(c0Pll, kPllUserCtl);
    const uint32_t c0St = rd32(c0Pll, kPllStatus);

    const uint32_t c1Cfg = rd32(c1Mux, kMuxCfgOff);
    const uint32_t c1L = rd32(c1Pll, kPllLVal);
    const uint32_t c1U = rd32(c1Pll, kPllUserCtl);
    const uint32_t c1St = rd32(c1Pll, kPllStatus);

    const uint32_t cciCfg = rd32(cciMux, kMuxCfgOff);
    const uint32_t cciL = rd32(cciPll, kPllLVal);
    const uint32_t cciU = rd32(cciPll, kPllUserCtl);

    const uint32_t c0Khz = estimateFreqKhz(c0Cfg, c0L, c0U);
    const uint32_t c1Khz = estimateFreqKhz(c1Cfg, c1L, c1U);
    const uint32_t cciKhz = estimateFreqKhz(cciCfg, cciL, cciU);
    const uint32_t c0Mv = readBuckMv(kPpidPm8950S5);
    const uint32_t c1Mv = readBuckMv(kPpidPm8004S2);

    kenzo_cpufreq_state[0] = mode;
    kenzo_cpufreq_state[1] = c0Khz;
    kenzo_cpufreq_state[2] = c1Khz;
    kenzo_cpufreq_state[3] = c0Mv;
    kenzo_cpufreq_state[4] = c1Mv;

    IOLog("KenzoCpuFreq: boot C0(A53)=%u kHz (cfg=0x%x L=%u st=0x%x %u mV) C1(A72)=%u kHz (cfg=0x%x L=%u st=0x%x %u mV) CCI=%u kHz\n",
          c0Khz, c0Cfg, c0L, c0St, c0Mv, c1Khz, c1Cfg, c1L, c1St, c1Mv, cciKhz);

    if (mode == 2U) {
        // Strict safety gate: only scale up to Nominal max (1017.6 MHz A53 / 1382.4 MHz A72)
        // when live buck voltage is confirmed >= 965 mV.
        if (c0Mv >= 965U && c0Khz < 1017600U) {
            if (reprogramClusterPll("C0(A53)", c0Mux, c0Pll, 53U, 0x00341600U, 0x00300000U, 0U)) {
                kenzo_cpufreq_state[1] = 1017600U;
            }
        }
        if (c1Mv >= 965U && c1Khz < 1382400U) {
            if (reprogramClusterPll("C1(A72)", c1Mux, c1Pll, 72U, 0x04E0405DU, 0x30000000U, 0U)) {
                kenzo_cpufreq_state[2] = 1382400U;
            }
        }
        IOLog("KenzoCpuFreq: post-boost C0(A53)=%u kHz C1(A72)=%u kHz\n",
              kenzo_cpufreq_state[1], kenzo_cpufreq_state[2]);
    }

    c0MuxMap->release();
    c0PllMap->release();
    c1MuxMap->release();
    c1PllMap->release();
    cciMuxMap->release();
    cciPllMap->release();
}
