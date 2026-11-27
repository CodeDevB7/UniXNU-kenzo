// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoBacklight - PMI8950 WLED4 backlight driver over SPMI PMIC Arbiter v2 fo
// Xiaomi Redmi Note 3 (kenzo, BOE NT35532 1080x1920 panel).
//
// Hardware verified from kenzo-dump/extracted_dtb_0.dtb:
//   - PMI8950 WLED4 on SPMI SID 3: ctrl=0xD800 (PPID 0x3D8), sink=0xD900 (PPID 0x3D9)
//   - 2 active LED strings (0 & 1), 12-bit linear brightness (0..4095)
//   - MOD_EN = 0xD846 (0x80 on / 0x00 off)
//   - SYNC   = 0xD947 (write 0x03, delay 1000 us, write 0x00)
//   - BRIGHT0 = 0xD957..0xD958 (LE), BRIGHT1 = 0xD967..0xD968 (LE)
//   - Gated behind boot-arg backlight=1.

#include "KenzoSpmi.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

extern "C" {
volatile int32_t kenzo_backlight_stats[8] = {};
void kenzo_backlight_start(void);
}

namespace {

constexpr uint16_t kPpidWledCtrl = 0x3D8U;
constexpr uint16_t kPpidWledSink = 0x3D9U;

constexpr uint8_t kRegModEn      = 0x46U; // 0xD846: bit 7 = enable
constexpr uint8_t kRegSync       = 0x47U; // 0xD947: bits [1:0] = strings 0 & 1
constexpr uint8_t kRegBright0Lsb = 0x57U; // 0xD957..0xD958
constexpr uint8_t kRegBright1Lsb = 0x67U; // 0xD967..0xD968

constexpr uint32_t kMaxLevel     = 4095U;
constexpr uint32_t kFadeStepMs   = 16U;

const OSMetaClass *gAppleBacklightClass = nullptr;
volatile uint32_t gImpersonating = 0;

} // namespace

class KenzoBacklight : public IOService {
    OSDeclareDefaultStructors(KenzoBacklight)

public:
    bool startBacklight();
    void stop(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *properties) override;

private:
    kenzo_spmi::Channel ctrlCh;
    kenzo_spmi::Channel sinkCh;
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *fadeTimer = nullptr;
    uint32_t currentLevel = 2048U;
    uint32_t startLevel = 2048U;
    uint32_t targetLevel = 2048U;
    uint32_t fadeStepsTotal = 0;
    uint32_t fadeStepIndex = 0;
    bool modEnabled = true;
    volatile uint64_t pendingReq = 0;

    bool applyHardwareLevel(uint32_t level);
    void publishLevel(uint32_t level);
    static void onTimer(OSObject *owner, IOTimerEventSource *sender);
    void handleTimer();
};

// Custom getMetaClass so CoreBrightness / IOPMrootDomain match AppleARMBacklight
// without colliding with AppleARMPlatform's existing AppleARMBacklight class name.
KenzoBacklight::MetaClass KenzoBacklight::gMetaClass;
const OSMetaClass * const KenzoBacklight::metaClass = &KenzoBacklight::gMetaClass;
const OSMetaClass * const KenzoBacklight::superClass = &IOService::gMetaClass;
KenzoBacklight::KenzoBacklight(const OSMetaClass *meta) : IOService(meta) {}
KenzoBacklight::~KenzoBacklight() {}
const OSMetaClass *KenzoBacklight::getMetaClass() const
{
    return (__atomic_load_n(&gImpersonating, __ATOMIC_ACQUIRE) && gAppleBacklightClass)
        ? gAppleBacklightClass
        : &gMetaClass;
}
OSMetaClassConstructorInit(KenzoBacklight, IOService, )
OSDefineDefaultStructors(KenzoBacklight, IOService)

bool KenzoBacklight::applyHardwareLevel(uint32_t level)
{
    if (level > kMaxLevel) {
        level = kMaxLevel;
    }
    if (level == 0) {
        const uint8_t off = 0x00U;
        if (!kenzo_spmi::writeBytes(ctrlCh, kRegModEn, &off, 1)) {
            __atomic_fetch_add(&kenzo_backlight_stats[4], 1, __ATOMIC_RELAXED);
            return false;
        }
        modEnabled = false;
        currentLevel = 0;
        kenzo_backlight_stats[2] = 0;
        return true;
    }

    const uint8_t brt[2] = {
        static_cast<uint8_t>(level & 0xFFU),
        static_cast<uint8_t>((level >> 8) & 0x0FU),
    };
    const uint8_t syncSet = 0x03U;
    const uint8_t syncClr = 0x00U;

    bool ok = true;
    if (!modEnabled) {
        const uint8_t on = 0x80U;
        ok = kenzo_spmi::writeBytes(ctrlCh, kRegModEn, &on, 1);
        if (ok) {
            modEnabled = true;
            IODelay(1000);
        }
    }
    if (ok) {
        ok = kenzo_spmi::writeBytes(sinkCh, kRegBright0Lsb, brt, 2) &&
             kenzo_spmi::writeBytes(sinkCh, kRegBright1Lsb, brt, 2) &&
             kenzo_spmi::writeBytes(sinkCh, kRegSync, &syncSet, 1);
    }
    if (ok) {
        IODelay(1000);
        ok = kenzo_spmi::writeBytes(sinkCh, kRegSync, &syncClr, 1);
    }
    if (!ok) {
        __atomic_fetch_add(&kenzo_backlight_stats[4], 1, __ATOMIC_RELAXED);
        return false;
    }
    currentLevel = level;
    kenzo_backlight_stats[2] = static_cast<int32_t>(level);
    return true;
}

void KenzoBacklight::publishLevel(uint32_t level)
{
    OSDictionary *params = OSDictionary::withCapacity(1);
    OSDictionary *brt = OSDictionary::withCapacity(3);
    OSNumber *nMin = OSNumber::withNumber(0ULL, 32);
    OSNumber *nMax = OSNumber::withNumber(kMaxLevel, 32);
    OSNumber *nVal = OSNumber::withNumber(level, 32);
    if (params && brt && nMin && nMax && nVal) {
        brt->setObject("min", nMin);
        brt->setObject("max", nMax);
        brt->setObject("value", nVal);
        params->setObject("brightness", brt);
        setProperty("IODisplayParameters", params);
    }
    OSSafeReleaseNULL(nVal);
    OSSafeReleaseNULL(nMax);
    OSSafeReleaseNULL(nMin);
    OSSafeReleaseNULL(brt);
    OSSafeReleaseNULL(params);
}

void KenzoBacklight::onTimer(OSObject *owner, IOTimerEventSource *)
{
    static_cast<KenzoBacklight *>(owner)->handleTimer();
}

void KenzoBacklight::handleTimer()
{
    const uint64_t req = __atomic_exchange_n(&pendingReq, 0ULL, __ATOMIC_ACQ_REL);
    if (req & (1ULL << 63)) {
        const uint32_t goal = static_cast<uint32_t>((req >> 16) & 0xFFFFU);
        const uint32_t ms = static_cast<uint32_t>(req & 0xFFFFU);
        startLevel = currentLevel;
        targetLevel = goal > kMaxLevel ? kMaxLevel : goal;
        fadeStepsTotal = ms / kFadeStepMs;
        fadeStepIndex = 0;
        if (fadeStepsTotal <= 1U) {
            (void)applyHardwareLevel(targetLevel);
            return;
        }
    }
    if (fadeStepsTotal > 1U && fadeStepIndex < fadeStepsTotal) {
        ++fadeStepIndex;
        const int32_t delta = static_cast<int32_t>(targetLevel) - static_cast<int32_t>(startLevel);
        const int32_t next = static_cast<int32_t>(startLevel) +
                             (delta * static_cast<int32_t>(fadeStepIndex)) / static_cast<int32_t>(fadeStepsTotal);
        (void)applyHardwareLevel(next < 0 ? 0U : static_cast<uint32_t>(next));
        if (fadeStepIndex < fadeStepsTotal && fadeTimer) {
            fadeTimer->setTimeoutMS(kFadeStepMs);
        } else {
            fadeStepsTotal = 0;
        }
    }
}

IOReturn KenzoBacklight::setProperties(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) {
        return kIOReturnBadArgument;
    }
    uint32_t goal = 0;
    uint32_t ms = 0;
    if (OSNumber *nits = OSDynamicCast(OSNumber, dict->getObject("brightness-nits"))) {
        const int32_t val = static_cast<int32_t>(nits->unsigned32BitValue());
        if (val <= 0) {
            goal = 0;
        } else {
            const int64_t scaled = (static_cast<int64_t>(val) * static_cast<int64_t>(kMaxLevel)) / (500LL << 16);
            goal = scaled < 1 ? 1U : (scaled > kMaxLevel ? kMaxLevel : static_cast<uint32_t>(scaled));
        }
    } else if (OSNumber *brightness = OSDynamicCast(OSNumber, dict->getObject("brightness"))) {
        const int32_t requested = static_cast<int32_t>(brightness->unsigned32BitValue());
        goal = requested < 0 ? 0U : (static_cast<uint32_t>(requested) > kMaxLevel ? kMaxLevel : static_cast<uint32_t>(requested));
        if (OSNumber *fade = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"))) {
            uint64_t fms = (static_cast<uint64_t>(fade->unsigned32BitValue()) * 1000ULL) >> 16;
            if (fms > 5000ULL) {
                fms = 5000ULL;
            }
            ms = static_cast<uint32_t>(fms);
        }
    } else {
        return kIOReturnSuccess;
    }

    __atomic_fetch_add(&kenzo_backlight_stats[1], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&pendingReq, (1ULL << 63) | (static_cast<uint64_t>(goal) << 16) | ms, __ATOMIC_RELEASE);
    if (fadeTimer) {
        fadeTimer->setTimeoutUS(1);
    }
    publishLevel(goal);
    return kIOReturnSuccess;
}

bool KenzoBacklight::startBacklight()
{
    uint32_t arbVer = 0;
    if (!kenzo_spmi::openChannel(ctrlCh, kPpidWledCtrl, true, &arbVer) ||
        !kenzo_spmi::openChannel(sinkCh, kPpidWledSink, true)) {
        kenzo_backlight_stats[0] = -1;
        IOLog("KenzoBacklight: failed to locate WLED4 SPMI APIDs (arbVer=0x%x)\n", arbVer);
        return false;
    }
    kenzo_backlight_stats[0] = 1;

    uint8_t modVal = 0;
    uint8_t brt[2] = {};
    if (kenzo_spmi::readBytes(ctrlCh, kRegModEn, &modVal, 1) &&
        kenzo_spmi::readBytes(sinkCh, kRegBright0Lsb, brt, 2)) {
        modEnabled = (modVal & 0x80U) != 0;
        currentLevel = static_cast<uint32_t>(brt[0]) | (static_cast<uint32_t>(brt[1] & 0x0FU) << 8);
        if (currentLevel == 0 && modEnabled) {
            currentLevel = 2048U;
        }
    }
    kenzo_backlight_stats[2] = static_cast<int32_t>(currentLevel);
    kenzo_backlight_stats[3] = static_cast<int32_t>(currentLevel);

    workLoop = IOWorkLoop::workLoop();
    fadeTimer = workLoop ? IOTimerEventSource::timerEventSource(this, &KenzoBacklight::onTimer) : nullptr;
    if (!workLoop || !fadeTimer || workLoop->addEventSource(fadeTimer) != kIOReturnSuccess) {
        kenzo_backlight_stats[0] = -2;
        return false;
    }

    setName("backlight");
    setProperty("NativeSandboxGrantProcess", "backboardd");
    publishLevel(currentLevel);

    unsigned blCtrl = 1;
    PE_parse_boot_argn("backlight-control", &blCtrl, sizeof(blCtrl));
    if (blCtrl != 0) {
        setProperty("backlight-control", kOSBooleanTrue);
        const OSSymbol *blSym = OSSymbol::withCStringNoCopy("AppleARMBacklight");
        gAppleBacklightClass = blSym ? OSMetaClass::getMetaClassWithName(blSym) : nullptr;
        OSSafeReleaseNULL(blSym);
        if (gAppleBacklightClass) {
            __atomic_store_n(&gImpersonating, 1U, __ATOMIC_RELEASE);
            kenzo_backlight_stats[0] = 4;
        } else {
            kenzo_backlight_stats[0] = 3;
        }
    } else {
        kenzo_backlight_stats[0] = 3;
    }

    registerService();
    IOLog("KenzoBacklight: WLED4 online arb=0x%x apids=%d/%d mod=0x%x level=%u/4095 st=%d\n",
          arbVer, ctrlCh.apid, sinkCh.apid, modVal, currentLevel, kenzo_backlight_stats[0]);
    return true;
}

void KenzoBacklight::stop(IOService *provider)
{
    if (fadeTimer) {
        fadeTimer->cancelTimeout();
        if (workLoop) {
            workLoop->removeEventSource(fadeTimer);
        }
        OSSafeReleaseNULL(fadeTimer);
    }
    OSSafeReleaseNULL(workLoop);
    ctrlCh.release();
    sinkCh.release();
    IOService::stop(provider);
}

void KenzoBacklight::free()
{
    ctrlCh.release();
    sinkCh.release();
    IOService::free();
}

extern "C" void kenzo_backlight_start(void)
{
    unsigned enabled = 0;
    PE_parse_boot_argn("backlight", &enabled, sizeof(enabled));
    if (enabled != 1U) {
        return;
    }
    IOService *parent = IOService::getPlatform();
    if (!parent) {
        parent = IOService::getResourceService();
    }
    KenzoBacklight *bl = OSTypeAlloc(KenzoBacklight);
    if (!bl || !bl->init(nullptr) || !parent || !bl->attach(parent) || !bl->startBacklight()) {
        if (bl) {
            if (parent) {
                bl->detach(parent);
            }
            bl->release();
        }
        return;
    }
    bl->release();
}
