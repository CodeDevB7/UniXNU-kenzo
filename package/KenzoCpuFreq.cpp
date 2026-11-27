// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoCpuFreq - Qualcomm MSM8956 (Snapdragon 650) cluster clock probe, SAW2 VCTL
// voltage scaler, and stock Nominal / Full-Stock Turbo PLL scaler.
//
// Hardware registers & tables verified from kenzo-dump/extracted_dtb_0.dtb and
// disassembly of stock Kenzo Linux kernel (clock-cpu-8976.c, spm-regulator.c, spm_v2):
//   - Cluster 0 (4x Cortex-A53): MUX 0x0B111050, SR PLL 0x0B116000, SAW2 0x0B1D2000, pm8950_s5 (PPID 0x120)
//   - Cluster 1 (2x Cortex-A72): MUX 0x0B011050, HFPLL  0x0B016000, SAW2 0x0B012000, pm8004_s2 (PPID 0x517)
//   - CCI Interconnect:          MUX 0x0B1D1050, SR PLL 0x0B1D0000 (shares pm8950_s5, stock max 614.4 MHz)
//   - vdd_mx:                    pm8950_s6 (PPID 0x123), voted to TURBO (384) by uefi-kenzo RPM-SMD
//
// Boot-arg:
//   kenzo-cpufreq=1 : Read-only hardware probe (dumps live MUX, PLL, and SPMI FTS2.5 mV).
//   kenzo-cpufreq=2 : Safe Nominal boost (A53 -> 1017.6 MHz L=53, A72 -> 1382.4 MHz L=72) when Vdd >= 965 mV.
//   kenzo-cpufreq=3 : Full Stock Turbo (SAW2 VCTL -> 1100 mV [0xCC], verify live Vdd >= 1080 mV, then
//                     A53 -> 1401.6 MHz L=73, A72 -> 1804.8 MHz L=94, CCI -> 614.4 MHz L=32;
//                     falls back safely to Nominal if Vdd < 1080 mV).

#include "KenzoSpmi.h"
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" {
volatile uint32_t kenzo_cpufreq_state[8] = {};
volatile uint32_t kenzo_cpufreq_bcd = 0; // BCD 0xCC11: top 2 digits = A53 (0.1 GHz), bottom 2 = A72 (0.1 GHz), e.g. 0x1418 = 1.4/1.8 GHz
void kenzo_cpufreq_run(void);
}

namespace {

constexpr uint64_t kC0MuxPhys  = 0x0B111000ULL; // +0x50 CMD_RCGR, +0x54 CFG_RCGR
constexpr uint64_t kC0PllPhys  = 0x0B116000ULL; // a53ss_sr_pll (SR PLL)
constexpr uint64_t kC1MuxPhys  = 0x0B011000ULL; // +0x50 CMD_RCGR, +0x54 CFG_RCGR
constexpr uint64_t kC1PllPhys  = 0x0B016000ULL; // a72ss_hf_pll (HFPLL)
constexpr uint64_t kCciMuxPhys = 0x0B1D1000ULL; // +0x50 CMD_RCGR, +0x54 CFG_RCGR
constexpr uint64_t kCciPllPhys = 0x0B1D0000ULL; // cci_sr_pll (SR PLL)
constexpr uint64_t kC0SawPhys  = 0x0B1D2000ULL; // system-cci SAW2 (owns pm8950_s5)
constexpr uint64_t kC1SawPhys  = 0x0B012000ULL; // a72-l2 SAW2 (owns pm8004_s2)

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
constexpr uint32_t kPllLockBit = 1U << 16;

constexpr uint32_t kSawId       = 0x004U; // bit 2 = VCTL_PRESENT
constexpr uint32_t kSawPmicSts  = 0x014U; // [17:16] busy/err, [7:0] active vset
constexpr uint32_t kSawRst      = 0x018U;
constexpr uint32_t kSawVctl     = 0x01CU;
constexpr uint32_t kSawPmicDat3 = 0x04CU;

constexpr uint16_t kPpidPm8950S5 = 0x120U; // A53 + CCI rail (FTS2.5)
constexpr uint16_t kPpidPm8950S6 = 0x123U; // vdd_mx rail
constexpr uint16_t kPpidPm8004S2 = 0x517U; // A72 rail (FTS2.5)

constexpr uint8_t  kVsetTurbo1100mV = 0xCCU; // FTS2.5 range 0: 80 + 204*5 = 1100 mV (65 mV under 1165 mV cap)

static uint32_t rd32(uintptr_t base, uint32_t off)
{
    return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static void wr32(uintptr_t base, uint32_t off, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t *>(base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t toBcd2(uint32_t tenthsGhz)
{
    if (tenthsGhz > 99U) {
        tenthsGhz = 99U;
    }
    return ((tenthsGhz / 10U) << 4) | (tenthsGhz % 10U);
}

static void publishBcd(uint32_t c0Khz, uint32_t c1Khz)
{
    const uint32_t c0t = (c0Khz + 50000U) / 100000U; // e.g. 1401600 -> 14 (1.4 GHz)
    const uint32_t c1t = (c1Khz + 50000U) / 100000U; // e.g. 1804800 -> 18 (1.8 GHz)
    kenzo_cpufreq_bcd = (toBcd2(c0t) << 8) | toBcd2(c1t);
}

static uint32_t estimateFreqKhz(uint32_t cfgRcgr, uint32_t pllL, uint32_t pllUser)
{
    const uint32_t src = (cfgRcgr >> 8) & 0x7U;
    const uint32_t div2 = cfgRcgr & 0x1FU;
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

// Verified QPNP FTS2.5 / FTS2 / ULT_HF buck decoder via SPMI Observer (read-only):
//   reg 0x04=TYPE, 0x05=SUBTYPE, 0x40=VOLTAGE_RANGE, 0x41=VOLTAGE_SETPOINT, 0x46=EN_CTL
static uint32_t readBuckMv(uint16_t ppid, uint8_t *typeOut = nullptr, uint8_t *subOut = nullptr)
{
    kenzo_spmi::Channel ch;
    if (!kenzo_spmi::openChannel(ch, ppid, false)) {
        return 0;
    }
    uint8_t id[2] = {};
    uint8_t ctl[2] = {};
    uint8_t en = 0;
    const bool ok = kenzo_spmi::readBytes(ch, 0x04U, id, 2) &&
                    kenzo_spmi::readBytes(ch, 0x40U, ctl, 2) &&
                    kenzo_spmi::readBytes(ch, 0x46U, &en, 1);
    ch.release();
    if (typeOut) { *typeOut = id[0]; }
    if (subOut)  { *subOut = id[1]; }
    if (!ok || (en & 0x80U) == 0) {
        return 0;
    }
    const uint8_t type = id[0], sub = id[1];
    const uint8_t range = ctl[0], vset = ctl[1];
    if (type == 0x1CU && sub == 0x09U) { // FTS2.5 (pm8950_s5, pm8004_s2)
        return (range == 0U) ? (80U + static_cast<uint32_t>(vset) * 5U)
                             : (160U + static_cast<uint32_t>(vset) * 10U);
    }
    if (type == 0x1CU && sub == 0x08U) { // FTS2
        return (range == 0U) ? (static_cast<uint32_t>(vset) * 5U)
                             : (static_cast<uint32_t>(vset) * 10U);
    }
    if (type == 0x22U && sub == 0x0DU) { // ULT_HF
        return (vset <= 0x5FU) ? (375U + (static_cast<uint32_t>(vset) * 25U) / 2U)
                               : (750U + static_cast<uint32_t>(vset & 0x9FU) * 25U);
    }
    return (range == 0U) ? (80U + static_cast<uint32_t>(vset) * 5U)
                         : (160U + static_cast<uint32_t>(vset) * 10U);
}

// Verified SAW2 VCTL voltage ramp (disassembled from stock msm_spm_drv_set_vdd)
static bool saw2SetVset(const char *tag, uint64_t sawPhys, uint8_t vset, uint32_t *mvOut = nullptr)
{
    if (sawPhys == kC1SawPhys) {
        // Ensure A72 SPM/L2 VCTL clock enable bit (0x0B011210 BIT(1)) is active before SAW2 access
        IOMemoryMap *spmClkMap = kenzo_spmi::mapPhys(0x0B011000ULL, 0x1000ULL);
        if (spmClkMap) {
            const uintptr_t sc = static_cast<uintptr_t>(spmClkMap->getVirtualAddress());
            wr32(sc, 0x210U, rd32(sc, 0x210U) | 0x2U);
            spmClkMap->release();
        }
    }
    IOMemoryMap *map = kenzo_spmi::mapPhys(sawPhys, 0x1000ULL);
    if (!map) {
        return false;
    }
    const uintptr_t base = static_cast<uintptr_t>(map->getVirtualAddress());
    const uint32_t id = rd32(base, kSawId);
    if ((id & (1U << 2)) == 0U) {
        IOLog("KenzoCpuFreq: %s SAW2 @ 0x%llx has no VCTL (id=0x%x)\n", tag, sawPhys, id);
        map->release();
        return false;
    }
    wr32(base, kSawRst, 1U);
    const uint32_t vctl = (rd32(base, kSawVctl) & 0xFFF8FF00U) | static_cast<uint32_t>(vset);
    wr32(base, kSawVctl, vctl);
    const uint32_t d3 = (rd32(base, kSawPmicDat3) & 0xFFF8FF00U) | static_cast<uint32_t>(vset);
    wr32(base, kSawPmicDat3, d3);

    bool ok = false;
    uint32_t sts = 0;
    for (uint32_t spin = 0; spin < 500U; ++spin) {
        sts = rd32(base, kSawPmicSts);
        if ((sts & 0x30000U) == 0U && (sts & 0xFFU) == static_cast<uint32_t>(vset)) {
            ok = true;
            break;
        }
        IODelay(1);
    }
    map->release();
    if (!ok) {
        IOLog("KenzoCpuFreq: %s SAW2 VCTL vset=0x%02x timeout (sts=0x%x)\n", tag, vset, sts);
        return false;
    }
    if (mvOut) {
        *mvOut = 80U + static_cast<uint32_t>(sts & 0xFFU) * 5U;
    }
    IODelay(350); // slew + FTS2.5 settle margin
    return true;
}

static uint32_t readSawMv(uint64_t sawPhys)
{
    IOMemoryMap *map = kenzo_spmi::mapPhys(sawPhys, 0x1000ULL);
    if (!map) {
        return 0;
    }
    const uintptr_t base = static_cast<uintptr_t>(map->getVirtualAddress());
    const uint32_t sts = rd32(base, kSawPmicSts);
    map->release();
    const uint32_t vset = sts & 0xFFU;
    return (vset >= 54U) ? (80U + vset * 5U) : 0U;
}

static bool reprogramClusterPll(const char *tag, uintptr_t muxBase, uintptr_t pllBase,
                                uint32_t targetL, uint32_t cfgCtl,
                                uint32_t userClearMask, uint32_t userSetMask, bool isHfPll)
{
    // 1. Park cluster safely on GPLL0_AO / 2 (400 MHz) before touching its PLL.
    if (!switchMux(muxBase, 4U, 3U)) {
        IOLog("KenzoCpuFreq: %s failed to park on GPLL0/2\n", tag);
        return false;
    }

    // 2. Disable PLL, program integer L multiplier & verified USER_CTL / CONFIG_CTL, then lock.
    wr32(pllBase, kPllMode, 0U);
    IODelay(5);
    wr32(pllBase, kPllLVal, targetL);
    wr32(pllBase, kPllMVal, 0U);
    wr32(pllBase, kPllNVal, 0U);
    const uint32_t uctl = (rd32(pllBase, kPllUserCtl) & ~userClearMask) | userSetMask;
    wr32(pllBase, kPllUserCtl, uctl);
    wr32(pllBase, kPllCfgCtl, cfgCtl);

    if (isHfPll) {
        wr32(pllBase, kPllMode, kPllBypass);
        IODelay(10);
    }
    wr32(pllBase, kPllMode, kPllBypass | kPllResetN);
    IODelay(50);

    bool locked = false;
    for (uint32_t spin = 0; spin < 2000U; ++spin) {
        if ((rd32(pllBase, kPllStatus) & (kPllLockBit | (1U << 31))) != 0U) {
            locked = true;
            break;
        }
        IODelay(5);
    }
    if (!locked) {
        IOLog("KenzoCpuFreq: %s PLL lock timeout (status=0x%x), staying on 800 MHz GPLL0\n",
              tag, rd32(pllBase, kPllStatus));
        (void)switchMux(muxBase, 4U, 1U);
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

    uint32_t c0Khz = estimateFreqKhz(c0Cfg, c0L, c0U);
    uint32_t c1Khz = estimateFreqKhz(c1Cfg, c1L, c1U);
    uint32_t cciKhz = estimateFreqKhz(cciCfg, cciL, cciU);
    uint8_t t0 = 0, s0 = 0, t1 = 0, s1 = 0;
    uint32_t c0Mv = readBuckMv(kPpidPm8950S5, &t0, &s0);
    uint32_t c1Mv = readBuckMv(kPpidPm8004S2, &t1, &s1);
    if (c1Mv == 0U) {
        c1Mv = readSawMv(kC1SawPhys);
    }
    const uint32_t mxMv = readBuckMv(kPpidPm8950S6);

    kenzo_cpufreq_state[0] = mode;
    kenzo_cpufreq_state[1] = c0Khz;
    kenzo_cpufreq_state[2] = c1Khz;
    kenzo_cpufreq_state[3] = c0Mv;
    kenzo_cpufreq_state[4] = c1Mv;
    kenzo_cpufreq_state[5] = mxMv;
    publishBcd(c0Khz, c1Khz);

    IOLog("KenzoCpuFreq: boot C0(A53)=%u kHz (cfg=0x%x L=%u st=0x%x %u mV %02x/%02x) C1(A72)=%u kHz (cfg=0x%x L=%u st=0x%x %u mV %02x/%02x) CCI=%u kHz MX=%u mV\n",
          c0Khz, c0Cfg, c0L, c0St, c0Mv, t0, s0, c1Khz, c1Cfg, c1L, c1St, c1Mv, t1, s1, cciKhz, mxMv);

    if (mode >= 3U && mxMv >= 900U) {
        // Full Stock Turbo: raise C0 & C1 rails via SAW2 VCTL to 1100 mV (0xCC), verify >= 1080 mV
        uint32_t sawMv = 0;
        if (c0Mv < 1080U && saw2SetVset("C0(A53)", kC0SawPhys, kVsetTurbo1100mV, &sawMv)) {
            const uint32_t spmiMv = readBuckMv(kPpidPm8950S5);
            c0Mv = spmiMv ? spmiMv : sawMv;
            kenzo_cpufreq_state[3] = c0Mv;
        }
        sawMv = 0;
        if (c1Mv < 1080U && saw2SetVset("C1(A72)", kC1SawPhys, kVsetTurbo1100mV, &sawMv)) {
            const uint32_t spmiMv = readBuckMv(kPpidPm8004S2);
            c1Mv = spmiMv ? spmiMv : sawMv;
            kenzo_cpufreq_state[4] = c1Mv;
        }
        if (c0Mv >= 1080U) {
            if (reprogramClusterPll("C0(A53)", c0Mux, c0Pll, 73U, 0x00341600U, 0x00307300U, 0x00000109U, false)) {
                c0Khz = 1401600U;
            }
            if (reprogramClusterPll("CCI", cciMux, cciPll, 32U, 0x00141400U, 0x00307300U, 0x00100109U, false)) {
                cciKhz = 614400U;
            }
        }
        if (c1Mv >= 1080U) {
            if (reprogramClusterPll("C1(A72)", c1Mux, c1Pll, 94U, 0x04E0405DU, 0x30001300U, 0x00100109U, true)) {
                c1Khz = 1804800U;
            }
        }
    }

    if (mode >= 2U) {
        // Safe Nominal fallback if Turbo was not requested or Vdd < 1080 mV
        if (c0Khz < 1017600U && c0Mv >= 965U) {
            if (reprogramClusterPll("C0(A53)", c0Mux, c0Pll, 53U, 0x00341600U, 0x00307300U, 0x00000109U, false)) {
                c0Khz = 1017600U;
            }
        }
        if (c1Khz < 1382400U && c1Mv >= 965U) {
            if (reprogramClusterPll("C1(A72)", c1Mux, c1Pll, 72U, 0x04E0405DU, 0x30001300U, 0x00100109U, true)) {
                c1Khz = 1382400U;
            }
        }
        // Never leave A72 parked at 19.2 MHz XO if HFPLL/buck failed: switch safely to 800 MHz GPLL0_AO
        if (c1Khz < 800000U) {
            if (switchMux(c1Mux, 4U, 1U)) {
                c1Khz = 800000U;
            }
        }
    }

    kenzo_cpufreq_state[1] = c0Khz;
    kenzo_cpufreq_state[2] = c1Khz;
    kenzo_cpufreq_state[6] = cciKhz;
    publishBcd(c0Khz, c1Khz);

    IOLog("KenzoCpuFreq: post-boost C0(A53)=%u kHz (%u mV) C1(A72)=%u kHz (%u mV) CCI=%u kHz bcd=0x%04x\n",
          c0Khz, c0Mv, c1Khz, c1Mv, cciKhz, kenzo_cpufreq_bcd);

    c0MuxMap->release();
    c0PllMap->release();
    c1MuxMap->release();
    c1PllMap->release();
    cciMuxMap->release();
    cciPllMap->release();
}
