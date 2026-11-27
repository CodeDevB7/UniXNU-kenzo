// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoTouchButtons - Hardware touchscreen (Atmel maXTouch @ 0x4A on BLSP1 QUP4 0x078B8000),
// physical buttons (PM8950 PON Power/VolDown + TLMM GPIO 113 VolUp), and NativeHidInput
// late-registration bridge for Xiaomi Redmi Note 3 (kenzo, MSM8956).
//
// Hardware verified directly from live /proc/interrupts + extracted_dtb_0.dtb + stock kernel:
//   - Touch IC: Atmel maXTouch (mxt-ts-1, family 0x82) @ 7-bit I2C 0x4A
//   - I2C bus:  BLSP1 QUP4 @ 0x078B8000 (QUP v2 tagged FIFO), TLMM GPIO 14 (SDA) & 15 (SCL) func 3
//   - GCC clks: APCS vote 0x01845004 BIT(10), blsp1_ahb CBCR 0x01801008,
//               qup4_i2c_apps RCG 0x01805000/0x01805004, CBCR 0x01805020
//   - GPIOs:    134 = Atmel VDD LDO (active-HIGH), 64 = Reset (active-LOW), 65 = CHG IRQ (active-LOW)
//   - Buttons:  Power = PM8950 PON (PPID 0x008, reg 0x10 bit 0), VolDown = PON bit 1,
//               VolUp = TLMM GPIO 113 (0x01071004 bit 0 == 0, active-LOW)
//
// Boot-args:
//   kenzo-hid=1     : register NativeHidInput (IOHIDDevice) with IOHIDFamily / IOHIDEventDrive
//   kenzo-buttons=1 : poll Power / VolUp / VolDown and deliver HID Report 2 Consumer buttons
//   kenzo-touch=1   : power/init BLSP1 QUP4 + Atmel maXTouch @ 0x4A and deliver 10-finger HID Report 3

#include "KenzoSpmi.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);

void kenzo_hid_input_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
IOService *native_hid_input_allocate(void);
IOService *native_hid_touch_allocate(void);

// Seqlock touch globals read by NativeHidInput.cpp (scaled to 0..30479 x 0..20319)
volatile uint32_t native_touch_seq = 0;
volatile uint32_t native_touch_count = 0;
volatile uint32_t native_touch_hid_reports = 0;
volatile int native_touch_xs[12] = {};
volatile int native_touch_ys[12] = {};
volatile int native_touch_ids[12] = {};

volatile uint32_t kenzo_touch_events = 0;
volatile uint32_t kenzo_touch_status = 0; // 0=off, 1=probing, 2=online, 0xE1..=fail
volatile uint32_t kenzo_button_mask = 0;

static void (*gTouchSink)(void *) = nullptr;
static void *gTouchSinkCtx = nullptr;
static void (*gButtonSink)(void *, uint32_t) = nullptr;
static void *gButtonSinkCtx = nullptr;
static volatile uint32_t gHomePulsePending = 0;

void native_touch_set_sink(void (*sink)(void *), void *ctx)
{
    gTouchSinkCtx = ctx;
    __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);
}

void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *ctx)
{
    gButtonSinkCtx = ctx;
    __atomic_store_n(&gButtonSink, sink, __ATOMIC_RELEASE);
}

void native_hid_input_observed(unsigned event, int status)
{
    static volatile uint32_t logged = 0;
    if (__atomic_add_fetch(&logged, 1U, __ATOMIC_RELAXED) <= 12U) {
        IOLog("KenzoHidInput: event=%u status=%d\n", event, status);
    }
}

static bool stampTouchUsageRecursive(IORegistryEntry *entry, unsigned depth)
{
    if (!entry || depth == 0U) {
        return false;
    }
    bool foundDriver = false;
    if (entry->metaCast("IOHIDEventDriver") || entry->metaCast("IOHIDInterface")) {
        OSNumber *page = OSNumber::withNumber(0x0DULL, 32);
        OSNumber *usage = OSNumber::withNumber(0x04ULL, 32);
        if (page && usage) {
            entry->setProperty("Built-In", kOSBooleanTrue);
            entry->setProperty("DisplayIntegrated", kOSBooleanTrue);
            entry->setProperty("DigitizerCollectionDispatch", kOSBooleanTrue);
            entry->setProperty("PrimaryUsagePage", page);
            entry->setProperty("PrimaryUsage", usage);
        }
        OSSafeReleaseNULL(page);
        OSSafeReleaseNULL(usage);
        if (entry->metaCast("IOHIDEventDriver")) {
            foundDriver = true;
        }
    }
    OSIterator *it = entry->getChildIterator(gIOServicePlane);
    if (it) {
        while (OSObject *child = it->getNextObject()) {
            IORegistryEntry *ce = OSDynamicCast(IORegistryEntry, child);
            if (ce && stampTouchUsageRecursive(ce, depth - 1U)) {
                foundDriver = true;
            }
        }
        it->release();
    }
    return foundDriver;
}

void native_hid_live_checkpoint(IOService *device)
{
    static volatile uint32_t stamped = 0;
    if (!device || __atomic_load_n(&stamped, __ATOMIC_ACQUIRE) != 0U) {
        return;
    }
    if (stampTouchUsageRecursive(device, 4U)) {
        __atomic_store_n(&stamped, 1U, __ATOMIC_RELEASE);
        if (kenzo_touch_status == 2U) {
            kenzo_touch_status = 3U; // 3 = IOHIDEventDriver stamped as PrimaryUsagePage=0x0D/4 for backboardd!
        }
        IOLog("KenzoHidInput: stamped IOHIDEventDriver as Built-In Digitizer/Touch Screen (0x0D/4)\n");
    }
}

bool native_hid_input_requested(void)
{
    return __atomic_exchange_n(&gHomePulsePending, 0U, __ATOMIC_ACQ_REL) != 0U;
}

void kenzo_input_start(void);
} // extern "C"

namespace {

// Physical MMIO bases
constexpr uint64_t kGccBase  = 0x01800000ULL;
constexpr uint64_t kTlmmBase = 0x01000000ULL;
constexpr uint64_t kQup4Base = 0x078B8000ULL;

// GCC offsets for BLSP1 QUP4 I2C
constexpr uint32_t kGccApcsVote       = 0x45004U; // BIT(10) = blsp1_ahb
constexpr uint32_t kGccBlsp1AhbCbcr   = 0x01008U;
constexpr uint32_t kGccQup4CmdRcgr    = 0x05000U;
constexpr uint32_t kGccQup4CfgRcgr    = 0x05004U;
constexpr uint32_t kGccQup4I2cCbcr    = 0x05020U;

// TLMM GPIOs (stride 0x1000)
constexpr uint32_t kGpioSda     = 14U;
constexpr uint32_t kGpioScl     = 15U;
constexpr uint32_t kGpioTsReset = 64U;
constexpr uint32_t kGpioTsIrq   = 65U;
constexpr uint32_t kGpioVolUp   = 113U;
constexpr uint32_t kGpioTsLdo   = 134U;

// PM8950 PON SPMI PPID
constexpr uint16_t kPpidPm8950Pon = 0x008U;
constexpr uint8_t  kPonRtStsReg   = 0x10U; // bit 0 = Power, bit 1 = VolDown

// Qualcomm QUP v2 I2C registers (verified against MSM8956 i2c-msm-v2.c & mainline i2c-qup.c)
constexpr uint32_t kQupConfig          = 0x000U;
constexpr uint32_t kQupState           = 0x004U;
constexpr uint32_t kQupIoModes         = 0x008U;
constexpr uint32_t kQupSwReset         = 0x00CU;
constexpr uint32_t kQupOperational     = 0x018U;
constexpr uint32_t kQupErrorFlags      = 0x01CU;
constexpr uint32_t kQupErrorFlagsEn    = 0x020U;
constexpr uint32_t kQupOperationalMask = 0x028U;
constexpr uint32_t kQupMxOutputCnt     = 0x100U;
constexpr uint32_t kQupOutFifoBase     = 0x110U;
constexpr uint32_t kQupMxWriteCnt      = 0x150U;
constexpr uint32_t kQupMxInputCnt      = 0x200U;
constexpr uint32_t kQupMxReadCnt       = 0x208U;
constexpr uint32_t kQupInFifoBase      = 0x218U;
constexpr uint32_t kQupI2cClkCtl       = 0x400U;
constexpr uint32_t kQupI2cMasterStatus = 0x404U;
constexpr uint32_t kQupI2cMasterCfg    = 0x408U;

constexpr uint32_t kQupIoModesFifo     = (1U << 14) | (1U << 15); // UNPACK_EN | PACK_EN, FIFO in/out
constexpr uint32_t kI2cStatusErrMask   = 0x038000FCU; // BUS_ERR|NACK|ARB|FAILED|INV_TAG|INV_RD_ADDR|INV_RD_SEQ
constexpr uint32_t kQupClk400k         = 0x000E031CU; // 19.2 MHz XO -> 400 kHz (HT=14, HS=3, FS=28)
constexpr uint32_t kQupClk100k         = 0x003E007CU; // 19.2 MHz XO -> 100 kHz (HT=62, FS=124)

// QUP v2 I2C tags (verified from i2c-msm-v2.h & i2c-qup.c)
constexpr uint8_t kTagStart        = 0x81U;
constexpr uint8_t kTagDataWrite    = 0x82U;
constexpr uint8_t kTagDataReadStop = 0x87U;

// Atmel maXTouch 7-bit I2C address & coordinate range expected by NativeHidInput.cpp
constexpr uint8_t  kMxtI2cAddr   = 0x4AU;
constexpr uint32_t kHidMaxX      = 30479U;
constexpr uint32_t kHidMaxY      = 20319U;
constexpr unsigned kMaxSlots     = 10U;

static inline uint32_t r32(uintptr_t base, uint32_t off)
{
    return *reinterpret_cast<volatile uint32_t *>(base + off);
}

static inline void w32(uintptr_t base, uint32_t off, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t *>(base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void tlmmCfg(uintptr_t tlmm, uint32_t gpio, uint32_t cfg)
{
    w32(tlmm, gpio * 0x1000U + 0x00U, cfg);
}

static inline void tlmmOut(uintptr_t tlmm, uint32_t gpio, bool high)
{
    w32(tlmm, gpio * 0x1000U + 0x04U, high ? 2U : 0U);
}

static inline bool tlmmIn(uintptr_t tlmm, uint32_t gpio)
{
    return (r32(tlmm, gpio * 0x1000U + 0x04U) & 1U) != 0U;
}

static bool qupSetState(uintptr_t qup, uint32_t state)
{
    for (uint32_t i = 0; i < 2000U; ++i) {
        if (r32(qup, kQupState) & 0x4U) {
            break;
        }
        IODelay(1);
    }
    w32(qup, kQupState, state);
    for (uint32_t i = 0; i < 2000U; ++i) {
        const uint32_t s = r32(qup, kQupState);
        if ((s & 0x4U) && (s & 0x3U) == state) {
            return true;
        }
        IODelay(1);
    }
    return false;
}

static bool qupI2cReadReg16(uintptr_t qup, uint32_t clkCtl, uint8_t slaveAddr,
                            uint16_t regAddr, uint8_t *out, uint8_t len,
                            uint32_t *diagMst = nullptr, uint32_t *diagOp = nullptr, uint32_t *diagErr = nullptr)
{
    if (!qup || !out || len == 0 || len > 32) {
        return false;
    }

    w32(qup, kQupSwReset, 1U);
    IODelay(5);
    if (!qupSetState(qup, 0U)) { // RESET
        return false;
    }

    w32(qup, kQupConfig, (2U << 8) | 0x07U); // MINI_CORE=I2C, N=8-bit
    w32(qup, kQupI2cMasterCfg, 1U);          // QUP_I2C_EN_VERSION_TWO_TAG
    for (uint32_t i = 0; i < 1000U && !(r32(qup, kQupState) & (1U << 4)); ++i) {
        IODelay(1); // wait QUP_I2C_MAST_GEN
    }
    w32(qup, kQupIoModes, kQupIoModesFifo);  // UNPACK_EN | PACK_EN, FIFO in/out
    w32(qup, kQupMxOutputCnt, 0U);
    w32(qup, kQupMxInputCnt, 0U);

    // TX sequence (10 bytes): START, addrW, DATA_WR, 2, regLo, regHi, START, addrR, DATA_RD_STOP (0x87), len
    const uint8_t tx[12] = {
        kTagStart, static_cast<uint8_t>(slaveAddr << 1),
        kTagDataWrite, 2U,
        static_cast<uint8_t>(regAddr & 0xFFU),
        static_cast<uint8_t>((regAddr >> 8) & 0xFFU),
        kTagStart, static_cast<uint8_t>((slaveAddr << 1) | 1U),
        kTagDataReadStop, len,
        0U, 0U,
    };
    const uint32_t rxTotal = static_cast<uint32_t>(len) + 2U; // 2-byte v2 RX tag + payload

    w32(qup, kQupMxWriteCnt, 10U);
    w32(qup, kQupMxReadCnt, rxTotal);
    w32(qup, kQupErrorFlags, 0x3CU);
    w32(qup, kQupErrorFlagsEn, 0x3CU);
    w32(qup, kQupOperationalMask, 0U);
    w32(qup, kQupOperational, 0x300U);       // clear OUT/IN SVC flags
    w32(qup, kQupI2cMasterStatus, kI2cStatusErrMask);

    if (!qupSetState(qup, 1U)) { // RESET -> RUN
        return false;
    }
    w32(qup, kQupI2cClkCtl, clkCtl); // must be written in RUN state

    if (!qupSetState(qup, 3U)) { // RUN -> PAUSE before loading OUT FIFO
        (void)qupSetState(qup, 0U);
        return false;
    }

    // Push 3 packed 32-bit words (10 bytes) into OUT FIFO while PAUSED
    for (uint32_t w = 0; w < 3U; ++w) {
        const uint32_t word = static_cast<uint32_t>(tx[w * 4U + 0U]) |
                              (static_cast<uint32_t>(tx[w * 4U + 1U]) << 8) |
                              (static_cast<uint32_t>(tx[w * 4U + 2U]) << 16) |
                              (static_cast<uint32_t>(tx[w * 4U + 3U]) << 24);
        w32(qup, kQupOutFifoBase, word);
    }

    if (!qupSetState(qup, 1U)) { // PAUSE -> RUN to start transfer
        (void)qupSetState(qup, 0U);
        return false;
    }

    uint8_t rxBuf[40] = {};
    uint32_t rxGot = 0;
    uint32_t lastMst = 0, lastOp = 0, lastErr = 0;
    bool failed = false;
    for (uint32_t spin = 0; spin < 10000U; ++spin) {
        lastMst = r32(qup, kQupI2cMasterStatus);
        lastErr = r32(qup, kQupErrorFlags);
        lastOp = r32(qup, kQupOperational);
        if ((lastMst & kI2cStatusErrMask) != 0U || (lastErr & 0x3CU) != 0U) {
            failed = true;
            break;
        }
        while ((r32(qup, kQupOperational) & (1U << 5)) && rxGot < sizeof(rxBuf)) {
            const uint32_t word = r32(qup, kQupInFifoBase);
            for (uint32_t b = 0; b < 4U && rxGot < sizeof(rxBuf); ++b) {
                rxBuf[rxGot++] = static_cast<uint8_t>((word >> (b * 8U)) & 0xFFU);
            }
        }
        lastOp = r32(qup, kQupOperational);
        if (rxGot >= rxTotal && (lastOp & (1U << 11))) { // MAX_INPUT_DONE_FLAG
            break;
        }
        IODelay(2);
    }

    if (!failed) {
        for (uint32_t i = 0; i < 2000U && (r32(qup, kQupI2cMasterStatus) & (1U << 8)); ++i) {
            IODelay(1); // wait BUS_ACTIVE == 0
        }
    }
    if (diagMst) { *diagMst = lastMst; }
    if (diagOp)  { *diagOp = lastOp; }
    if (diagErr) { *diagErr = lastErr; }

    (void)qupSetState(qup, 0U);
    if (failed || rxGot < rxTotal) {
        return false;
    }
    memcpy(out, rxBuf + 2, len);
    return true;
}

struct FingerSlot {
    bool active = false;
    uint16_t x = 0;
    uint16_t y = 0;
};

} // namespace

class KenzoInputController : public IOService {
    OSDeclareDefaultStructors(KenzoInputController)

public:
    bool startInput(bool enableButtons, bool enableTouch);
    void stop(IOService *provider) override;
    void free() override;

private:
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *pollTimer = nullptr;
    IOMemoryMap *tlmmMap = nullptr;
    IOMemoryMap *gccMap = nullptr;
    IOMemoryMap *qupMap = nullptr;
    uintptr_t tlmm = 0;
    uintptr_t qup = 0;
    kenzo_spmi::Channel ponCh;

    bool buttonsActive = false;
    bool touchActive = false;
    uint32_t lastBtnMask = 0;
    uint32_t btnPollDiv = 0;

    uint32_t qupClkCtl = kQupClk100k;
    uint32_t touchRot = 0U; // 0 = portrait, 90 = landscape CW (matches KenzoLegacyDisplay)
    uint16_t t5Addr = 0;
    uint8_t  t5Size = 0;
    uint16_t t44Addr = 0;
    uint8_t  t100MinId = 0;
    uint8_t  t100MaxId = 0;
    uint8_t  t9MinId = 0;
    uint8_t  t9MaxId = 0;
    uint16_t maxX = 1023U;
    uint16_t maxY = 1023U;
    FingerSlot slots[kMaxSlots] = {};

    bool initTouchHardware();
    void pollButtons();
    void pollTouch();
    void publishTouches();
    static void onPoll(OSObject *owner, IOTimerEventSource *sender);
    void handlePoll();
};

OSDefineMetaClassAndStructors(KenzoInputController, IOService)

bool KenzoInputController::initTouchHardware()
{
    gccMap = kenzo_spmi::mapPhys(kGccBase, 0x50000ULL);
    qupMap = kenzo_spmi::mapPhys(kQup4Base, 0x1000ULL);
    if (!gccMap || !qupMap || !tlmm) {
        return false;
    }
    const uintptr_t gcc = static_cast<uintptr_t>(gccMap->getVirtualAddress());
    qup = static_cast<uintptr_t>(qupMap->getVirtualAddress());

    // 1. Enable GCC clocks for BLSP1 AHB & BLSP1 QUP4 I2C (19.2 MHz XO)
    w32(gcc, kGccApcsVote, r32(gcc, kGccApcsVote) | (1U << 10));
    for (uint32_t i = 0; i < 1000U && (r32(gcc, kGccBlsp1AhbCbcr) & (1U << 31)); ++i) {
        IODelay(1);
    }
    w32(gcc, kGccQup4CfgRcgr, 0U); // XO 19.2 MHz, div 1
    w32(gcc, kGccQup4CmdRcgr, r32(gcc, kGccQup4CmdRcgr) | 1U);
    for (uint32_t i = 0; i < 1000U && (r32(gcc, kGccQup4CmdRcgr) & 1U); ++i) {
        IODelay(1);
    }
    w32(gcc, kGccQup4I2cCbcr, r32(gcc, kGccQup4I2cCbcr) | 1U);
    for (uint32_t i = 0; i < 1000U && (r32(gcc, kGccQup4I2cCbcr) & (1U << 31)); ++i) {
        IODelay(1);
    }

    // 2. Configure TLMM I2C pins (GPIO 14 SDA, GPIO 15 SCL, func=3, 2mA, no-pull)
    tlmmCfg(tlmm, kGpioSda, 3U << 2);
    tlmmCfg(tlmm, kGpioScl, 3U << 2);

    // 3. Power up Atmel maXTouch in verified stock order: Reset LOW -> VDD LDO HIGH -> 150 ms -> Reset HIGH -> 160 ms
    kenzo_touch_status = 1U;
    tlmmCfg(tlmm, kGpioTsIrq, (3U << 6) | 3U);                // input, 8mA, pull-up
    tlmmCfg(tlmm, kGpioTsReset, (1U << 9) | (3U << 6) | 3U);  // OE, 8mA, pull-up
    tlmmCfg(tlmm, kGpioTsLdo, (1U << 9) | (7U << 6) | 3U);    // OE, 16mA, pull-up
    tlmmOut(tlmm, kGpioTsReset, false);
    tlmmOut(tlmm, kGpioTsLdo, false);
    IOSleep(20);
    tlmmOut(tlmm, kGpioTsLdo, true);
    IOSleep(150);
    tlmmOut(tlmm, kGpioTsReset, true);
    IOSleep(160);

    // 4. Probe Atmel maXTouch Info Block @ 0x0000 (try 400 kHz first, fall back to 100 kHz)
    uint8_t info[7] = {};
    uint32_t dMst = 0, dOp = 0, dErr = 0;
    qupClkCtl = kQupClk400k;
    if (!qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, 0x0000U, info, 7, &dMst, &dOp, &dErr)) {
        qupClkCtl = kQupClk100k;
        if (!qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, 0x0000U, info, 7, &dMst, &dOp, &dErr)) {
            kenzo_touch_status = 0xE1U;
            IOLog("KenzoTouch: maXTouch @ 0x4A probe failed (mst=0x%x op=0x%x err=0x%x)\n", dMst, dOp, dErr);
            return false;
        }
    }

    const uint8_t numObjs = info[6];
    if (numObjs == 0 || numObjs > 64) {
        kenzo_touch_status = 0xE2U;
        IOLog("KenzoTouch: invalid object count %u (fam=0x%02x)\n", numObjs, info[0]);
        return false;
    }

    uint8_t nextId = 1U;
    uint16_t t100Addr = 0;
    for (uint8_t i = 0; i < numObjs; ++i) {
        uint8_t entry[6] = {};
        if (!qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, static_cast<uint16_t>(7U + i * 6U), entry, 6)) {
            break;
        }
        const uint8_t type = entry[0];
        const uint16_t addr = static_cast<uint16_t>(entry[1]) | (static_cast<uint16_t>(entry[2]) << 8);
        const uint8_t size = static_cast<uint8_t>(entry[3] + 1U);
        const uint8_t insts = static_cast<uint8_t>(entry[4] + 1U);
        const uint8_t repPerInst = entry[5];
        const uint8_t minId = repPerInst ? nextId : 0U;
        const uint8_t maxId = repPerInst ? static_cast<uint8_t>(nextId + insts * repPerInst - 1U) : 0U;
        nextId = static_cast<uint8_t>(nextId + insts * repPerInst);

        if (type == 5U) {
            t5Addr = addr;
            t5Size = size > 16U ? 16U : size;
        } else if (type == 44U) {
            t44Addr = addr;
        } else if (type == 100U && repPerInst >= 3U) {
            t100Addr = addr;
            t100MinId = minId;
            t100MaxId = maxId;
        } else if (type == 9U && repPerInst >= 1U) {
            t9MinId = minId;
            t9MaxId = maxId;
        }
    }

    if (t100Addr) {
        uint8_t cfg1 = 0;
        uint8_t xbuf[2] = {}, ybuf[2] = {};
        (void)qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, static_cast<uint16_t>(t100Addr + 1U), &cfg1, 1);
        if (qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, static_cast<uint16_t>(t100Addr + 13U), xbuf, 2) &&
            qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, static_cast<uint16_t>(t100Addr + 24U), ybuf, 2)) {
            uint16_t xr = static_cast<uint16_t>(xbuf[0]) | (static_cast<uint16_t>(xbuf[1]) << 8);
            uint16_t yr = static_cast<uint16_t>(ybuf[0]) | (static_cast<uint16_t>(ybuf[1]) << 8);
            // Per Atmel maXTouch T100 spec & live Kenzo Android getevent -p (0..1023 x 0..1023):
            // XRANGE == 0 / YRANGE == 0 means hardware default 1023 (10-bit normalized coordinates).
            if (xr == 0U) { xr = 1023U; }
            if (yr == 0U) { yr = 1023U; }
            if (cfg1 & 0x20U) { // MXT_T100_CFG_SWITCHXY
                const uint16_t tmp = xr;
                xr = yr;
                yr = tmp;
            }
            if (xr >= 200U && xr <= 8192U) { maxX = xr; }
            if (yr >= 200U && yr <= 8192U) { maxY = yr; }
        }
    }

    unsigned landscapeArg = 0;
    PE_parse_boot_argn("kenzo-landscape", &landscapeArg, sizeof(landscapeArg));
    touchRot = (landscapeArg == 1U) ? 90U : 0U;
    PE_parse_boot_argn("kenzo-touch-rot", &touchRot, sizeof(touchRot));

    const bool ok = (t5Addr != 0 && t5Size >= 6 && (t100MinId != 0 || t9MinId != 0));
    kenzo_touch_status = ok ? 2U : 0xE3U;
    IOLog("KenzoTouch: maXTouch fam=0x%02x var=0x%02x ver=0x%02x objs=%u T5=0x%x(%u) T44=0x%x T100=%u..%u range=%ux%u rot=%u clk=0x%x\n",
          info[0], info[1], info[2], numObjs, t5Addr, t5Size, t44Addr, t100MinId, t100MaxId, maxX + 1U, maxY + 1U, touchRot, qupClkCtl);
    return ok;
}

void KenzoInputController::publishTouches()
{
    uint32_t seq = __atomic_load_n(&native_touch_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&native_touch_seq, seq + 1U, __ATOMIC_RELAXED);
    __asm__ volatile("dmb ishst" ::: "memory");

    const uint32_t mx = maxX ? maxX : 1023U;
    const uint32_t my = maxY ? maxY : 1023U;
    uint32_t count = 0;
    for (unsigned s = 0; s < kMaxSlots; ++s) {
        if (!slots[s].active) {
            continue;
        }
        const uint32_t rx = slots[s].x > mx ? mx : slots[s].x;
        const uint32_t ry = slots[s].y > my ? my : slots[s].y;
        uint32_t sx = 0, sy = 0;
        if (touchRot == 90U) {
            // Exact inverse of KenzoLegacyDisplay 90 deg CW landscape rotation (dx = physW-1-sy, dy = sx):
            // Portrait Atmel MXT336T outputs 0..1023 x 0..1023 (ry=0 earpiece/left -> ry=1023 USB/right;
            // rx=1023 power/top -> rx=0 SIM/bottom).
            sx = (ry * kHidMaxX) / my;
            sy = ((mx - rx) * kHidMaxY) / mx;
        } else if (touchRot == 270U) {
            sx = ((my - ry) * kHidMaxX) / my;
            sy = (rx * kHidMaxY) / mx;
        } else {
            sx = (rx * kHidMaxX) / mx;
            sy = (ry * kHidMaxY) / my;
        }
        native_touch_xs[count] = static_cast<int>(sx > kHidMaxX ? kHidMaxX : sx);
        native_touch_ys[count] = static_cast<int>(sy > kHidMaxY ? kHidMaxY : sy);
        native_touch_ids[count] = static_cast<int>(s + 1U);
        ++count;
    }
    native_touch_count = count;
    __asm__ volatile("dmb ishst" ::: "memory");
    __atomic_store_n(&native_touch_seq, seq + 2U, __ATOMIC_RELEASE);
    const uint32_t evNum = __atomic_add_fetch(&kenzo_touch_events, 1U, __ATOMIC_RELAXED);
    if (evNum <= 5U && count > 0U) {
        IOLog("KenzoTouch: ev#%u cnt=%u hid=(%d,%d) rot=%u\n",
              evNum, count, native_touch_xs[0], native_touch_ys[0], touchRot);
    }

    void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE);
    if (sink) {
        sink(gTouchSinkCtx);
    }
}

void KenzoInputController::pollTouch()
{
    if (!touchActive || tlmmIn(tlmm, kGpioTsIrq)) {
        return; // CHG high = no pending messages
    }
    uint8_t msgCount = 1U;
    if (t44Addr) {
        if (!qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, t44Addr, &msgCount, 1) || msgCount == 0U) {
            return;
        }
        if (msgCount > 16U) {
            msgCount = 16U;
        }
    }
    bool dirty = false;
    for (uint8_t m = 0; m < msgCount; ++m) {
        uint8_t msg[16] = {};
        if (!qupI2cReadReg16(qup, qupClkCtl, kMxtI2cAddr, t5Addr, msg, t5Size)) {
            break;
        }
        const uint8_t rid = msg[0];
        if (rid == 0xFFU || rid == 0x00U) {
            break;
        }
        if (t100MinId && rid >= static_cast<uint8_t>(t100MinId + 2U) && rid <= t100MaxId) {
            const unsigned slot = static_cast<unsigned>(rid - (t100MinId + 2U));
            if (slot < kMaxSlots) {
                const bool down = (msg[1] & 0x80U) != 0U;
                slots[slot].active = down;
                if (down) {
                    slots[slot].x = static_cast<uint16_t>(msg[2]) | (static_cast<uint16_t>(msg[3]) << 8);
                    slots[slot].y = static_cast<uint16_t>(msg[4]) | (static_cast<uint16_t>(msg[5]) << 8);
                }
                dirty = true;
            }
        } else if (t9MinId && rid >= t9MinId && rid <= t9MaxId) {
            const unsigned slot = static_cast<unsigned>(rid - t9MinId);
            if (slot < kMaxSlots) {
                const bool down = (msg[1] & 0x80U) != 0U;
                slots[slot].active = down;
                if (down) {
                    slots[slot].x = static_cast<uint16_t>((static_cast<uint16_t>(msg[2]) << 4) | ((msg[4] >> 4) & 0x0FU));
                    slots[slot].y = static_cast<uint16_t>((static_cast<uint16_t>(msg[3]) << 4) | (msg[4] & 0x0FU));
                }
                dirty = true;
            }
        }
    }
    if (dirty) {
        publishTouches();
    }
}

void KenzoInputController::pollButtons()
{
    if (!buttonsActive) {
        return;
    }
    uint32_t mask = 0U;
    uint8_t ponSts = 0U;
    if (ponCh.rdBase && kenzo_spmi::readBytes(ponCh, kPonRtStsReg, &ponSts, 1)) {
        if (ponSts & 0x01U) { mask |= (1U << 1); } // Power (HID bit 1)
        if (ponSts & 0x02U) { mask |= (1U << 3); } // VolDown (HID bit 3)
    }
    if (tlmm && !tlmmIn(tlmm, kGpioVolUp)) {
        mask |= (1U << 2); // VolUp (active-LOW, HID bit 2)
    }
    if (mask != lastBtnMask) {
        lastBtnMask = mask;
        kenzo_button_mask = mask;
        // VolUp + VolDown chord also pulses synthetic Home (Consumer Menu 0x40)
        if ((mask & 0x0CU) == 0x0CU) {
            __atomic_store_n(&gHomePulsePending, 1U, __ATOMIC_RELEASE);
        }
        void (*sink)(void *, uint32_t) = __atomic_load_n(&gButtonSink, __ATOMIC_ACQUIRE);
        if (sink) {
            sink(gButtonSinkCtx, mask);
        }
    }
}

void KenzoInputController::onPoll(OSObject *owner, IOTimerEventSource *)
{
    static_cast<KenzoInputController *>(owner)->handlePoll();
}

void KenzoInputController::handlePoll()
{
    pollTouch();
    if ((++btnPollDiv % 3U) == 0U) {
        pollButtons(); // ~40 Hz buttons while touch runs at ~125 Hz
    }
    if (pollTimer) {
        pollTimer->setTimeoutMS(touchActive ? 8U : 25U);
    }
}

bool KenzoInputController::startInput(bool enableButtons, bool enableTouch)
{
    tlmmMap = kenzo_spmi::mapPhys(kTlmmBase, 0x100000ULL);
    if (!tlmmMap) {
        return false;
    }
    tlmm = static_cast<uintptr_t>(tlmmMap->getVirtualAddress());

    if (enableButtons) {
        tlmmCfg(tlmm, kGpioVolUp, 3U); // input, pull-up (active-LOW)
        const bool ponOk = kenzo_spmi::openChannel(ponCh, kPpidPm8950Pon, false);
        buttonsActive = true;
        IOLog("KenzoButtons: online (PON apid=%d, VolUp=GPIO113)\n", ponOk ? ponCh.apid : -1);
    }

    if (enableTouch) {
        touchActive = initTouchHardware();
    }

    if (!buttonsActive && !touchActive) {
        return false;
    }

    workLoop = IOWorkLoop::workLoop();
    pollTimer = workLoop ? IOTimerEventSource::timerEventSource(this, &KenzoInputController::onPoll) : nullptr;
    if (!workLoop || !pollTimer || workLoop->addEventSource(pollTimer) != kIOReturnSuccess) {
        return false;
    }
    pollTimer->setTimeoutMS(25U);
    return true;
}

void KenzoInputController::stop(IOService *provider)
{
    if (pollTimer) {
        pollTimer->cancelTimeout();
        if (workLoop) {
            workLoop->removeEventSource(pollTimer);
        }
        OSSafeReleaseNULL(pollTimer);
    }
    OSSafeReleaseNULL(workLoop);
    ponCh.release();
    OSSafeReleaseNULL(qupMap);
    OSSafeReleaseNULL(gccMap);
    OSSafeReleaseNULL(tlmmMap);
    tlmm = 0;
    qup = 0;
    IOService::stop(provider);
}

void KenzoInputController::free()
{
    ponCh.release();
    OSSafeReleaseNULL(qupMap);
    OSSafeReleaseNULL(gccMap);
    OSSafeReleaseNULL(tlmmMap);
    IOService::free();
}

static bool registerHidLateClass()
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        return false;
    }
    kenzo_hid_input_static_init();
    return OSMetaClass::postModLoad(handle) == KERN_SUCCESS;
}

extern "C" void kenzo_input_start(void)
{
    unsigned hidEn = 0, btnEn = 0, tsEn = 0;
    PE_parse_boot_argn("kenzo-hid", &hidEn, sizeof(hidEn));
    PE_parse_boot_argn("kenzo-buttons", &btnEn, sizeof(btnEn));
    PE_parse_boot_argn("kenzo-touch", &tsEn, sizeof(tsEn));
    if (hidEn != 1U && btnEn != 1U && tsEn != 1U) {
        return;
    }

    IOService *parent = IOService::getResourceService();
    if (!parent) {
        parent = IOService::getPlatform();
    }
    if (!parent) {
        return;
    }

    if (hidEn == 1U) {
        const kern_return_t kr1 = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
        const kern_return_t kr2 = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver");
        IOLog("KenzoHidInput: kext load HIDFamily=0x%x EventDriver=0x%x\n",
              static_cast<unsigned>(kr1), static_cast<unsigned>(kr2));
        if (kr1 == KERN_SUCCESS && registerHidLateClass()) {
            // Combined keyboard + consumer buttons + 10-finger digitizer service:
            // keyboard collection matches in-kernel IOHIDEventDriver (dk=0), and
            // native_hid_live_checkpoint stamps PrimaryUsagePage=0x0D/4 for backboardd!
            IOService *hid = native_hid_input_allocate();
            if (hid && hid->init(nullptr) && hid->attach(parent)) {
                if (hid->start(parent)) {
                    hid->registerService();
                    IOLog("KenzoHidInput: NativeHidInput (combined buttons+touch) registered\n");
                } else {
                    hid->detach(parent);
                }
            }
            OSSafeReleaseNULL(hid);
        }
    }

    if (btnEn == 1U || tsEn == 1U) {
        KenzoInputController *ctl = OSTypeAlloc(KenzoInputController);
        if (ctl && ctl->init(nullptr) && ctl->attach(parent) && ctl->startInput(btnEn == 1U, tsEn == 1U)) {
            ctl->release();
        } else if (ctl) {
            ctl->detach(parent);
            ctl->release();
        }
    }
}
