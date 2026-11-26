// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared Qualcomm MSM8956 SPMI PMIC Arbiter v2 helper (0x0200F000 / 0x02400000 / 0x02C00000).
// Used by KenzoBacklight (PMI8950 WLED4), KenzoTouchButtons (PM8950 PON), and KenzoCpuFreq (buck Vdd observer).
#pragma once

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <stdint.h>

namespace kenzo_spmi {

constexpr uint64_t kCoreBase   = 0x0200F000ULL;
constexpr uint64_t kChnlsBase  = 0x02400000ULL;
constexpr uint64_t kObsrvrBase = 0x02C00000ULL;
constexpr uint32_t kMaxApids   = 256U;
constexpr uint32_t kApidStride = 0x8000U; // PMIC Arbiter v2, EE=0

// Channel register offsets (within 4 KiB per-APID window).
constexpr uint32_t kRegCmd    = 0x00U;
constexpr uint32_t kRegStatus = 0x08U;
constexpr uint32_t kRegWdata0 = 0x10U;
constexpr uint32_t kRegRdata0 = 0x18U;

constexpr uint32_t kOpcExtWriteL = 0U;
constexpr uint32_t kOpcExtReadL  = 1U;

struct Channel {
    IOMemoryMap *wrMap = nullptr;
    IOMemoryMap *rdMap = nullptr;
    uintptr_t wrBase = 0;
    uintptr_t rdBase = 0;
    int apid = -1;
    uint16_t ppid = 0;

    void release()
    {
        if (wrMap) { wrMap->release(); wrMap = nullptr; }
        if (rdMap) { rdMap->release(); rdMap = nullptr; }
        wrBase = 0;
        rdBase = 0;
        apid = -1;
    }
};

static inline uint32_t mmioRead32(uintptr_t base, uint32_t off)
{
    return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static inline void mmioWrite32(uintptr_t base, uint32_t off, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t *>(base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline IOMemoryMap *mapPhys(uint64_t phys, uint64_t size)
{
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, size);
    if (!mem) {
        return nullptr;
    }
    IOMemoryMap *map = mem->map(kIOMapAnywhere | kIOMapInhibitCache);
    mem->release();
    return map;
}

// Scan PMIC_ARB_REG_CHNL(0..255) at core+0x800 for matching 12-bit PPID = (sid << 8) | periph.
static inline int findApid(uint16_t ppid, uint32_t *arbVerOut = nullptr)
{
    IOMemoryMap *coreMap = mapPhys(kCoreBase, 0x1000ULL);
    if (!coreMap) {
        return -1;
    }
    const uintptr_t core = static_cast<uintptr_t>(coreMap->getVirtualAddress());
    if (arbVerOut) {
        *arbVerOut = mmioRead32(core, 0x00U);
    }
    int found = -1;
    for (uint32_t n = 0; n < kMaxApids; ++n) {
        const uint32_t entry = mmioRead32(core, 0x800U + n * 4U);
        const uint16_t entryPpid = static_cast<uint16_t>((entry >> 8) & 0xFFFU);
        if (entryPpid == ppid) {
            found = static_cast<int>(n);
            break;
        }
    }
    coreMap->release();
    return found;
}

static inline bool openChannel(Channel &ch, uint16_t ppid, bool needWrite, uint32_t *arbVerOut = nullptr)
{
    ch.release();
    const int apid = findApid(ppid, arbVerOut);
    if (apid < 0) {
        return false;
    }
    ch.rdMap = mapPhys(kObsrvrBase + static_cast<uint64_t>(apid) * kApidStride, 0x1000ULL);
    if (!ch.rdMap) {
        return false;
    }
    ch.rdBase = static_cast<uintptr_t>(ch.rdMap->getVirtualAddress());
    if (needWrite) {
        ch.wrMap = mapPhys(kChnlsBase + static_cast<uint64_t>(apid) * kApidStride, 0x1000ULL);
        if (!ch.wrMap) {
            ch.release();
            return false;
        }
        ch.wrBase = static_cast<uintptr_t>(ch.wrMap->getVirtualAddress());
    }
    ch.apid = apid;
    ch.ppid = ppid;
    return true;
}

static inline bool readBytes(const Channel &ch, uint8_t reg, uint8_t *out, uint8_t len)
{
    if (!ch.rdBase || !out || len == 0 || len > 4) {
        return false;
    }
    const uint32_t cmd = (kOpcExtReadL << 27) |
                         (static_cast<uint32_t>(reg) << 4) |
                         static_cast<uint32_t>(len - 1U);
    mmioWrite32(ch.rdBase, kRegCmd, cmd);
    uint32_t status = 0;
    for (uint32_t spin = 0; spin < 2000U; ++spin) {
        status = mmioRead32(ch.rdBase, kRegStatus);
        if (status & 0x1U) {
            break;
        }
        IODelay(1);
    }
    if (!(status & 0x1U) || (status & 0xEU) != 0) {
        return false;
    }
    const uint32_t data = mmioRead32(ch.rdBase, kRegRdata0);
    for (uint8_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>((data >> (i * 8U)) & 0xFFU);
    }
    return true;
}

static inline bool writeBytes(const Channel &ch, uint8_t reg, const uint8_t *in, uint8_t len)
{
    if (!ch.wrBase || !in || len == 0 || len > 4) {
        return false;
    }
    uint32_t data = 0;
    for (uint8_t i = 0; i < len; ++i) {
        data |= static_cast<uint32_t>(in[i]) << (i * 8U);
    }
    mmioWrite32(ch.wrBase, kRegWdata0, data);
    const uint32_t cmd = (kOpcExtWriteL << 27) |
                         (static_cast<uint32_t>(reg) << 4) |
                         static_cast<uint32_t>(len - 1U);
    mmioWrite32(ch.wrBase, kRegCmd, cmd);
    uint32_t status = 0;
    for (uint32_t spin = 0; spin < 2000U; ++spin) {
        status = mmioRead32(ch.wrBase, kRegStatus);
        if (status & 0x1U) {
            break;
        }
        IODelay(1);
    }
    return (status & 0x1U) != 0 && (status & 0xEU) == 0;
}

} // namespace kenzo_spmi
