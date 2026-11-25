// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSdhciBlock - Qualcomm MSM8956 (Snapdragon 650 / Kenzo) SDHCI 3.0 eMMC
// read-only block transport (hc_mem 0x07824900, core_mem 0x07824000, mmc0).
//
// Re-enables Qualcomm SDCC1 Host Controller mode (CORE_HC_MODE = 0x2001) after
// LittleKernel aboot's target_uninit() (sdhci_mode_disable + mmc_put_card_to_sleep),
// wakes the eMMC card from SLEEP -> STBY -> TRAN (with full JEDEC 1-bit DS
// fallback), and reads via standard SDHCI 3.0 polled PIO (Buffer Data Port 0x20).
// Strictly read-only: write requests are refused.

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>

extern "C" {
volatile uint32_t native_sdhci_ready = 0;
volatile uint32_t native_sdhci_gpt_userdata = 0;
volatile uint64_t native_sdhci_userdata_first = 0;
volatile uint64_t native_sdhci_userdata_last = 0;
volatile uint32_t native_sdhci_ext_csd_ok = 0;
volatile uint32_t native_sdhci_ext_csd_sectors = 0;
volatile uint32_t native_sdhci_root_layout = 0;
volatile uint64_t native_sdhci_window_base_lba = 0;
volatile uint64_t native_sdhci_window_blocks = 0;
volatile uint32_t native_sdhci_read_requests = 0;
volatile uint32_t native_sdhci_read_failures = 0;
volatile uint64_t native_sdhci_sectors = 0;
volatile uint64_t native_sdhci_mmio_physical = 0;
// Diagnostic words: [0]=(core_hc_before<<16)|core_hc_after, [1]=(clk<<16)|(pwr<<8)|hc1,
// [2]=last_cmd_resp, [3]=(stage<<24)|(err<<16)|norm
volatile uint32_t native_sdhci_diag[4] = {};
}

namespace {

// Standard SDHCI 3.0 register offsets (Qualcomm sdhci-msm hc_mem @ 0x07824900).
constexpr uint32_t kBlockSize       = 0x04;  // 16-bit
constexpr uint32_t kBlockCount      = 0x06;  // 16-bit
constexpr uint32_t kArgument        = 0x08;  // 32-bit
constexpr uint32_t kTransferMode    = 0x0C;  // 16-bit
constexpr uint32_t kCommand         = 0x0E;  // 16-bit
constexpr uint32_t kResponse        = 0x10;  // 32-bit
constexpr uint32_t kBufferPort      = 0x20;  // 32-bit PIO data port
constexpr uint32_t kPresentState    = 0x24;  // 32-bit
constexpr uint32_t kHostControl1    = 0x28;  // 8-bit
constexpr uint32_t kPowerControl    = 0x29;  // 8-bit
constexpr uint32_t kClockControl    = 0x2C;  // 16-bit
constexpr uint32_t kTimeoutControl  = 0x2E;  // 8-bit
constexpr uint32_t kSoftwareReset   = 0x2F;  // 8-bit
constexpr uint32_t kNormalIntStatus = 0x30;  // 16-bit (32-bit pair with 0x32)
constexpr uint32_t kErrorIntStatus  = 0x32;  // 16-bit
constexpr uint32_t kNormalIntEnable = 0x34;  // 32-bit pair (0x34/0x36)
constexpr uint32_t kNormalIntSignal = 0x38;  // 32-bit pair (0x38/0x3A)
constexpr uint32_t kHostControl2    = 0x3E;  // 16-bit
constexpr uint32_t kCapabilities    = 0x40;  // 32-bit
constexpr uint32_t kVersion         = 0xFE;  // 16-bit
constexpr uint32_t kVendorSpecFunc  = 0x10C; // 32-bit Qualcomm HC_VENDOR_SPECIFIC_FUNC

// Qualcomm SDCC core_mem offsets (@ 0x07824000).
constexpr uint32_t kCoreMciClk       = 0x04;
constexpr uint32_t kCoreHcMode       = 0x78;
constexpr uint32_t kCorePwrctlStatus = 0xDC;
constexpr uint32_t kCorePwrctlClear  = 0xE0;
constexpr uint32_t kCorePwrctlCtl    = 0xE8;

constexpr uint32_t kHcModeEn         = 1U << 0;
constexpr uint32_t kFfClkSwRstDis    = 1U << 13;

constexpr uint32_t kPresentCmdInhibit  = 1U << 0;
constexpr uint32_t kPresentDatInhibit  = 1U << 1;
constexpr uint32_t kPresentDat0Level   = 1U << 20;
constexpr uint16_t kIntCommandComplete = 1U << 0;
constexpr uint16_t kIntTransferComplete = 1U << 1;
constexpr uint16_t kIntBufferReadReady = 1U << 5;
constexpr uint16_t kIntError           = 1U << 15;

constexpr uint32_t kBlockBytes         = 512;
constexpr uint16_t kTrnsReadPio        = 0x0010U; // Data Read, PIO (DMA = 0)
constexpr uint16_t kCmdRespNone        = 0x0000U;
constexpr uint16_t kCmdRespR2          = 0x0009U; // 136-bit + CRC
constexpr uint16_t kCmdRespR3          = 0x0002U; // 48-bit (OCR, no CRC/idx)
constexpr uint16_t kCmdRespR1          = 0x001AU; // 48-bit + CRC + Index
constexpr uint16_t kCmdRespR1b         = 0x001BU; // 48-bit busy + CRC + Index
constexpr uint16_t kCmdDataReadFlags   = 0x003AU; // Data Present | Index | CRC | Resp 48-bit

bool compatibleContains(OSData *compatible, const char *needle)
{
    if (!compatible || !needle) {
        return false;
    }
    const char *bytes = static_cast<const char *>(compatible->getBytesNoCopy());
    size_t remaining = compatible->getLength();
    const size_t nlen = strlen(needle);
    while (bytes && remaining) {
        size_t len = 0;
        while (len < remaining && bytes[len] != '\0') {
            ++len;
        }
        if (len == nlen && memcmp(bytes, needle, nlen) == 0) {
            return true;
        }
        if (len >= remaining) {
            break;
        }
        bytes += len + 1;
        remaining -= len + 1;
    }
    return false;
}

} // namespace

class NativeSdhciBlock : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(NativeSdhciBlock)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char *getVendorString() override { return const_cast<char *>("Qualcomm"); }
    char *getProductString() override { return const_cast<char *>("MSM8956 SDHCI eMMC"); }
    char *getRevisionString() override { return const_cast<char *>("SDHCI3"); }
    char *getAdditionalDeviceInfoString() override
    {
        return const_cast<char *>("userdata partition, PIO, read-only");
    }
    IOReturn reportBlockSize(UInt64 *value) override
    {
        *value = kBlockBytes;
        return kIOReturnSuccess;
    }
    IOReturn reportEjectability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        *value = capacity > 0 ? capacity - 1 : 0;
        return kIOReturnSuccess;
    }
    IOReturn reportMediaState(bool *present, bool *changed) override
    {
        *present = online;
        if (changed) {
            *changed = false;
        }
        return kIOReturnSuccess;
    }
    IOReturn reportRemovability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportWriteProtection(bool *value) override
    {
        *value = true;
        return kIOReturnSuccess;
    }
    IOReturn doSynchronizeCache() override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override
    {
        return online ? kIOReturnSuccess : kIOReturnNotReady;
    }
    IOReturn getWriteCacheState(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                              IOStorageAttributes *attributes,
                              IOStorageCompletion *completion) override;

private:
    IOMemoryMap *mapping = nullptr;
    IOMemoryMap *coreMapping = nullptr;
    IOLock *gate = nullptr;
    uintptr_t registers = 0;
    uintptr_t coreRegs = 0;
    uint64_t capacity = 0;
    uint64_t partitionFirstLba = 0;
    uint64_t userdataFirstLba = 0;
    uint64_t userdataBlocks = 0;
    bool windowPublished = false;
    bool online = false;
    bool ownsDevice = false;
    uint8_t sectorBuf[kBlockBytes] = {};
    uint8_t manifestBlock[4096] = {};

    uint32_t read32(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint32_t *>(registers + offset);
    }
    uint16_t read16(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint16_t *>(registers + offset);
    }
    uint8_t read8(uint32_t offset) const
    {
        return *reinterpret_cast<volatile uint8_t *>(registers + offset);
    }
    void write32(uint32_t offset, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t *>(registers + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void write16(uint32_t offset, uint16_t value)
    {
        *reinterpret_cast<volatile uint16_t *>(registers + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void write8(uint32_t offset, uint8_t value)
    {
        *reinterpret_cast<volatile uint8_t *>(registers + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    uint32_t coreRead32(uint32_t offset) const
    {
        return coreRegs ? *reinterpret_cast<volatile uint32_t *>(coreRegs + offset) : 0U;
    }
    void coreWrite32(uint32_t offset, uint32_t value)
    {
        if (!coreRegs) {
            return;
        }
        *reinterpret_cast<volatile uint32_t *>(coreRegs + offset) = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }

    void ackPwrctlIrq();
    void enableQualcommHcMode();
    void setClockDivider(uint16_t divider);
    void ensureBusClockAndPower();
    void resetCmdDat();
    bool waitCommandIdle(uint32_t milliseconds);
    bool sendCommand(uint32_t commandIndex, uint32_t argument, uint16_t flags, uint32_t *respOut);
    bool wakeCardFromSleep();
    bool reinitCardLegacy1Bit();
    bool dataCommandPio(uint32_t commandIndex, uint32_t argument);
    bool readSector(uint64_t emmcLba);
    bool readExtCsd();
    bool readGptHeader(uint64_t *entryLbaOut, uint32_t *entryCountOut, uint32_t *entrySizeOut);
    bool findUserdataPartition(uint64_t entryLba, uint32_t entryCount, uint32_t entrySize);
    bool readRootLayout();
};

OSDefineMetaClassAndStructors(NativeSdhciBlock, IOBlockStorageDevice)

IOService *NativeSdhciBlock::probe(IOService *provider, SInt32 *score)
{
    if (!provider) {
        return nullptr;
    }
    OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
    if (!compatibleContains(compatible, "unixnu,sdhci-block") &&
        !compatibleContains(compatible, "qcom,sdhci-msm")) {
        return nullptr;
    }
    IOLog("NativeSdhciBlock: probe matched node %s\n", provider->getName());
    return IOBlockStorageDevice::probe(provider, score);
}

void NativeSdhciBlock::ackPwrctlIrq()
{
    if (!coreRegs) {
        return;
    }
    const uint32_t st = coreRead32(kCorePwrctlStatus);
    if (st != 0) {
        coreWrite32(kCorePwrctlClear, st);
        // Acknowledge bus power / IO switch success so SDHCI state machine proceeds.
        uint32_t ctl = 0;
        if (st & 0x03U) { // BUS_OFF | BUS_ON
            ctl |= 0x01U;
        }
        if (st & 0x0CU) { // IO_LOW | IO_HIGH
            ctl |= 0x02U;
        }
        if (ctl != 0) {
            coreWrite32(kCorePwrctlCtl, ctl);
        }
    }
}

void NativeSdhciBlock::enableQualcommHcMode()
{
    if (!coreRegs) {
        return;
    }
    const uint32_t before = coreRead32(kCoreHcMode);
    const uint32_t mciClk = coreRead32(kCoreMciClk);
    const uint32_t pwrSt = coreRead32(kCorePwrctlStatus);
    ackPwrctlIrq();
    coreWrite32(kCoreHcMode, kHcModeEn);
    (void)coreRead32(kCoreHcMode);
    coreWrite32(kCoreHcMode, kHcModeEn | kFfClkSwRstDis);
    const uint32_t after = coreRead32(kCoreHcMode);
    IODelay(1000);
    native_sdhci_diag[0] = ((before & 0xFFFFU) << 16) | (after & 0xFFFFU);
    IOLog("NativeSdhciBlock: core HC_MODE 0x%x->0x%x mci_clk=0x%x pwrctl=0x%x\n",
          before, after, mciClk, pwrSt);
}

void NativeSdhciBlock::setClockDivider(uint16_t divider)
{
    // Disable SDCLK before changing divider.
    write16(kClockControl, 0U);
    IODelay(100);
    const uint16_t clk = static_cast<uint16_t>(((divider & 0xFFU) << 8) |
                                               (((divider >> 8) & 0x03U) << 6) | 0x0001U);
    write16(kClockControl, clk);
    for (uint32_t i = 0; i < 5000; ++i) {
        if ((read16(kClockControl) & 0x0002U) != 0) {
            break;
        }
        IODelay(10);
    }
    write16(kClockControl, static_cast<uint16_t>(clk | 0x0004U)); // SD_CLK_EN
    IODelay(1000);
}

void NativeSdhciBlock::ensureBusClockAndPower()
{
    const uint8_t pwr = read8(kPowerControl);
    if ((pwr & 0x01U) == 0) {
        write8(kPowerControl, 0x0FU); // 3.3V + SD Bus Power ON
        IODelay(2000);
        ackPwrctlIrq();
    }
    uint16_t clk = read16(kClockControl);
    if ((clk & 0x0005U) != 0x0005U) {
        uint16_t div = static_cast<uint16_t>(((clk >> 8) & 0xFFU) | (((clk >> 6) & 0x03U) << 8));
        if (div == 0) {
            div = 4U; // Safe conservative divider
        }
        setClockDivider(div);
    }
    native_sdhci_diag[1] = (static_cast<uint32_t>(read16(kClockControl)) << 16) |
                           (static_cast<uint32_t>(read8(kPowerControl)) << 8) |
                           static_cast<uint32_t>(read8(kHostControl1));
}

void NativeSdhciBlock::resetCmdDat()
{
    write8(kSoftwareReset, 0x06U); // Reset CMD + DAT lines only
    for (uint32_t i = 0; i < 10000; ++i) {
        if ((read8(kSoftwareReset) & 0x06U) == 0) {
            break;
        }
        IODelay(10);
    }
    write32(kNormalIntStatus, 0xFFFFFFFFU);
}

bool NativeSdhciBlock::waitCommandIdle(uint32_t milliseconds)
{
    for (uint32_t waited = 0; waited < milliseconds; ++waited) {
        if ((read32(kPresentState) & (kPresentCmdInhibit | kPresentDatInhibit)) == 0) {
            return true;
        }
        IODelay(1000);
    }
    return false;
}

bool NativeSdhciBlock::sendCommand(uint32_t commandIndex, uint32_t argument, uint16_t flags, uint32_t *respOut)
{
    if (!ownsDevice || !registers) {
        return false;
    }
    write32(kNormalIntSignal, 0U);
    write32(kNormalIntEnable, 0xFFFFFFFFU);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    write8(kTimeoutControl, 0x0EU);

    if (!waitCommandIdle(500)) {
        resetCmdDat();
        if (!waitCommandIdle(200)) {
            return false;
        }
    }

    write32(kArgument, argument);
    write16(kTransferMode, 0U);
    write16(kCommand, static_cast<uint16_t>(((commandIndex & 0x3FU) << 8) | (flags & 0x00FFU)));

    uint16_t normStatus = 0;
    uint16_t errStatus = 0;
    bool cmdDone = false;
    const bool isR1b = (flags == kCmdRespR1b);
    bool busyDone = !isR1b;

    for (uint32_t spin = 0; spin < 50000; ++spin) {
        normStatus = read16(kNormalIntStatus);
        if ((normStatus & kIntError) != 0) {
            errStatus = read16(kErrorIntStatus);
            break;
        }
        if (!cmdDone && (normStatus & kIntCommandComplete) != 0) {
            cmdDone = true;
            write16(kNormalIntStatus, kIntCommandComplete);
        }
        if (cmdDone && !busyDone) {
            if ((normStatus & kIntTransferComplete) != 0 ||
                (read32(kPresentState) & kPresentDat0Level) != 0) {
                busyDone = true;
                if ((normStatus & kIntTransferComplete) != 0) {
                    write16(kNormalIntStatus, kIntTransferComplete);
                }
            }
        }
        if (cmdDone && busyDone) {
            break;
        }
        IODelay(5);
    }

    const uint32_t resp = read32(kResponse);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    native_sdhci_diag[2] = resp;
    native_sdhci_diag[3] = (commandIndex << 24) | (static_cast<uint32_t>(errStatus) << 16) | normStatus;

    if (respOut) {
        *respOut = resp;
    }
    if (errStatus != 0 || !cmdDone || !busyDone) {
        resetCmdDat();
        return false;
    }
    return true;
}

bool NativeSdhciBlock::wakeCardFromSleep()
{
    const uint32_t rcas[2] = {2U, 1U};
    for (uint32_t rca : rcas) {
        uint32_t resp = 0;
        bool statusOk = sendCommand(13U, rca << 16, kCmdRespR1, &resp);
        if (!statusOk) {
            // Card is likely in SLEEP state (from LK mmc_put_card_to_sleep): send CMD5 AWAKE (bit 15 = 0).
            uint32_t r5 = 0;
            if (sendCommand(5U, rca << 16, kCmdRespR1b, &r5)) {
                IOLog("NativeSdhciBlock: CMD5 AWAKE rca=%u resp=0x%08x\n", rca, r5);
                IODelay(5000);
                statusOk = sendCommand(13U, rca << 16, kCmdRespR1, &resp);
            }
        }
        if (!statusOk) {
            continue;
        }
        uint32_t state = (resp >> 9) & 0xFU;
        if (state == 3U) { // STBY -> select card into TRAN
            uint32_t r7 = 0;
            if (sendCommand(7U, rca << 16, kCmdRespR1b, &r7)) {
                IODelay(1000);
                if (sendCommand(13U, rca << 16, kCmdRespR1, &resp)) {
                    state = (resp >> 9) & 0xFU;
                }
            }
        }
        if (state == 4U) { // TRAN
            IOLog("NativeSdhciBlock: woke eMMC rca=%u state=TRAN resp=0x%08x\n", rca, resp);
            return true;
        }
    }
    return false;
}

bool NativeSdhciBlock::reinitCardLegacy1Bit()
{
    IOLog("NativeSdhciBlock: running clean JEDEC 1-bit DS eMMC init\n");
    // Clear Qualcomm HS200/HS400 clock-mux override bits (HC_SELECT_IN [21:19], HC_SELECT_IN_EN [18]).
    uint32_t vfunc = read32(kVendorSpecFunc);
    vfunc &= ~(0xFU << 18);
    write32(kVendorSpecFunc, vfunc);

    // 1-bit bus, Default Speed (no HS200/HS400 DLL tuning needed).
    write8(kHostControl1, 0x00U);
    write16(kHostControl2, 0x0000U);
    setClockDivider(0x00FFU); // ~400 kHz identification clock
    resetCmdDat();

    uint32_t resp = 0;
    (void)sendCommand(0U, 0U, kCmdRespNone, nullptr);
    IODelay(10000);

    bool ocrReady = false;
    for (uint32_t attempt = 0; attempt < 400; ++attempt) {
        if (sendCommand(1U, 0x40FF8080U, kCmdRespR3, &resp) && (resp & 0x80000000U) != 0) {
            ocrReady = true;
            break;
        }
        IODelay(2000);
    }
    if (!ocrReady) {
        IOLog("NativeSdhciBlock: CMD1 OCR timeout resp=0x%08x\n", resp);
        return false;
    }
    if (!sendCommand(2U, 0U, kCmdRespR2, &resp)) {
        IOLog("NativeSdhciBlock: CMD2 ALL_SEND_CID failed\n");
        return false;
    }
    if (!sendCommand(3U, 2U << 16, kCmdRespR1, &resp)) {
        IOLog("NativeSdhciBlock: CMD3 SET_RCA failed\n");
        return false;
    }
    if (!sendCommand(7U, 2U << 16, kCmdRespR1b, &resp)) {
        IOLog("NativeSdhciBlock: CMD7 SELECT_CARD failed\n");
        return false;
    }
    (void)sendCommand(16U, kBlockBytes, kCmdRespR1, &resp);

    // Bump to conservative ~12-25 MHz Default Speed clock for reliable PIO reads.
    setClockDivider(0x0008U);
    if (!sendCommand(13U, 2U << 16, kCmdRespR1, &resp) || (((resp >> 9) & 0xFU) != 4U)) {
        IOLog("NativeSdhciBlock: post-init CMD13 failed resp=0x%08x\n", resp);
        return false;
    }
    IOLog("NativeSdhciBlock: JEDEC 1-bit DS init OK resp=0x%08x clk=0x%04x\n",
          resp, static_cast<unsigned>(read16(kClockControl)));
    return true;
}

bool NativeSdhciBlock::dataCommandPio(uint32_t commandIndex, uint32_t argument)
{
    if (!ownsDevice || !registers) {
        return false;
    }
    write32(kNormalIntSignal, 0U);
    write32(kNormalIntEnable, 0xFFFFFFFFU);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    write8(kTimeoutControl, 0x0EU);

    if (!waitCommandIdle(2000)) {
        IOLog("NativeSdhciBlock: cmd %u inhibited ps=0x%08x\n", commandIndex, read32(kPresentState));
        resetCmdDat();
        return false;
    }

    write16(kBlockSize, static_cast<uint16_t>(kBlockBytes));
    write16(kBlockCount, 1U);
    write32(kArgument, argument);
    write16(kTransferMode, kTrnsReadPio);
    write16(kCommand, static_cast<uint16_t>(((commandIndex & 0x3FU) << 8) | kCmdDataReadFlags));

    bool cmdDone = false;
    bool bufRead = false;
    bool xferDone = false;
    uint16_t errStatus = 0;
    uint16_t normStatus = 0;

    for (uint32_t spin = 0; spin < 200000; ++spin) {
        normStatus = read16(kNormalIntStatus);
        if ((normStatus & kIntError) != 0) {
            errStatus = read16(kErrorIntStatus);
            break;
        }
        if (!cmdDone && (normStatus & kIntCommandComplete) != 0) {
            cmdDone = true;
            write16(kNormalIntStatus, kIntCommandComplete);
        }
        if (!bufRead && (normStatus & kIntBufferReadReady) != 0) {
            write16(kNormalIntStatus, kIntBufferReadReady);
            uint32_t *words = reinterpret_cast<uint32_t *>(sectorBuf);
            for (uint32_t w = 0; w < kBlockBytes / 4; ++w) {
                words[w] = read32(kBufferPort);
            }
            bufRead = true;
        }
        if (bufRead && (normStatus & kIntTransferComplete) != 0) {
            xferDone = true;
            write16(kNormalIntStatus, kIntTransferComplete);
            break;
        }
        IODelay(5);
    }

    const uint32_t resp = read32(kResponse);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    write32(kNormalIntSignal, 0U);
    native_sdhci_diag[2] = resp;
    native_sdhci_diag[3] = (commandIndex << 24) | (static_cast<uint32_t>(errStatus) << 16) | normStatus;

    if (errStatus != 0 || !bufRead || !xferDone) {
        IOLog("NativeSdhciBlock: cmd %u arg=0x%x fail norm=0x%04x err=0x%04x ps=0x%08x resp=0x%08x\n",
              commandIndex, argument, static_cast<unsigned>(normStatus),
              static_cast<unsigned>(errStatus), read32(kPresentState), resp);
        resetCmdDat();
        return false;
    }
    return true;
}

bool NativeSdhciBlock::readSector(uint64_t emmcLba)
{
    if (emmcLba > 0xFFFFFFFFULL || !dataCommandPio(17U, static_cast<uint32_t>(emmcLba))) {
        __atomic_fetch_add(&native_sdhci_read_failures, 1U, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&native_sdhci_read_requests, 1U, __ATOMIC_RELAXED);
    return true;
}

bool NativeSdhciBlock::readExtCsd()
{
    if (!dataCommandPio(8U, 0U)) {
        IOLog("NativeSdhciBlock: CMD8 SEND_EXT_CSD failed\n");
        return false;
    }
    uint32_t sectors = 0;
    memcpy(&sectors, sectorBuf + 212, sizeof(sectors));
    native_sdhci_ext_csd_sectors = sectors;
    if (sectors != 0 && sectors != 0xFFFFFFFFU) {
        native_sdhci_ext_csd_ok = 1;
    }
    IOLog("NativeSdhciBlock: EXT_CSD rev=0x%02x sec_count=0x%x (%llu MiB)\n",
          static_cast<unsigned>(sectorBuf[192]), sectors,
          (static_cast<uint64_t>(sectors) * kBlockBytes) >> 20);
    return native_sdhci_ext_csd_ok != 0;
}

bool NativeSdhciBlock::readGptHeader(uint64_t *entryLbaOut, uint32_t *entryCountOut, uint32_t *entrySizeOut)
{
    if (!readSector(1)) {
        return false;
    }
    if (memcmp(sectorBuf, "EFI PART", 8) != 0) {
        IOLog("NativeSdhciBlock: LBA 1 is not a GPT header\n");
        return false;
    }
    uint64_t firstUsable = 0;
    uint64_t lastUsable = 0;
    uint64_t entryLba = 0;
    uint32_t entryCount = 0;
    uint32_t entrySize = 0;
    memcpy(&firstUsable, sectorBuf + 40, sizeof(firstUsable));
    memcpy(&lastUsable, sectorBuf + 48, sizeof(lastUsable));
    memcpy(&entryLba, sectorBuf + 72, sizeof(entryLba));
    memcpy(&entryCount, sectorBuf + 80, sizeof(entryCount));
    memcpy(&entrySize, sectorBuf + 84, sizeof(entrySize));
    IOLog("NativeSdhciBlock: GPT first=0x%llx last=0x%llx entries@0x%llx count=%u size=%u\n",
          firstUsable, lastUsable, entryLba, entryCount, entrySize);
    *entryLbaOut = entryLba;
    *entryCountOut = entryCount;
    *entrySizeOut = entrySize;
    return true;
}

bool NativeSdhciBlock::findUserdataPartition(uint64_t entryLba, uint32_t entryCount, uint32_t entrySize)
{
    if (entrySize < 128 || entrySize > kBlockBytes || entryLba == 0) {
        return false;
    }
    const uint32_t entriesPerSector = kBlockBytes / entrySize;
    const uint32_t sectors = (entryCount + entriesPerSector - 1U) / entriesPerSector;
    for (uint32_t sector = 0; sector < sectors; ++sector) {
        if (!readSector(entryLba + sector)) {
            return false;
        }
        for (uint32_t slot = 0; slot < entriesPerSector; ++slot) {
            const uint32_t index = sector * entriesPerSector + slot;
            if (index >= entryCount) {
                break;
            }
            const uint8_t *entry = sectorBuf + slot * entrySize;
            bool empty = true;
            for (unsigned b = 0; b < 16; ++b) {
                if (entry[b] != 0) { empty = false; break; }
            }
            if (empty) {
                continue;
            }
            char name[37] = {};
            for (unsigned c = 0; c < 36; ++c) {
                uint16_t code = 0;
                memcpy(&code, entry + 56 + c * 2, sizeof(code));
                if (code == 0) { break; }
                name[c] = (code < 0x80) ? static_cast<char>(code) : '?';
            }
            uint64_t first = 0, last = 0;
            memcpy(&first, entry + 32, sizeof(first));
            memcpy(&last, entry + 40, sizeof(last));
            if (strcmp(name, "userdata") == 0 && last >= first) {
                native_sdhci_userdata_first = first;
                native_sdhci_userdata_last = last;
                native_sdhci_gpt_userdata = 1;
                userdataFirstLba = first;
                userdataBlocks = last - first + 1;
                partitionFirstLba = userdataFirstLba;
                capacity = userdataBlocks;
                IOLog("NativeSdhciBlock: GPT [%02u] userdata first=0x%llx last=0x%llx (%llu MiB)\n",
                      index, first, last, (userdataBlocks * kBlockBytes) >> 20);
            }
        }
    }
    return native_sdhci_gpt_userdata != 0;
}

bool NativeSdhciBlock::readRootLayout()
{
    constexpr uint32_t kManifestBytes = 4096;
    constexpr uint32_t kManifestSectors = kManifestBytes / kBlockBytes;
    constexpr uint32_t kEntryBytes = 128;
    constexpr uint32_t kMaxEntries = (kManifestBytes - 24) / kEntryBytes;

    if (userdataFirstLba == 0 || userdataBlocks < kManifestSectors) {
        return false;
    }
    for (uint32_t s = 0; s < kManifestSectors; ++s) {
        if (!readSector(userdataFirstLba + s)) {
            return false;
        }
        memcpy(manifestBlock + s * kBlockBytes, sectorBuf, kBlockBytes);
    }
    if (memcmp(manifestBlock, "SHNGROOT", 8) != 0) {
        IOLog("NativeSdhciBlock: no SHNGROOT manifest at userdata start (Android userdata untouched)\n");
        return false;
    }
    uint32_t version = 0, count = 0;
    uint64_t imageBytes = 0;
    memcpy(&version, manifestBlock + 8, sizeof(version));
    memcpy(&count, manifestBlock + 12, sizeof(count));
    memcpy(&imageBytes, manifestBlock + 16, sizeof(imageBytes));
    if (version != 1 || count == 0 || count > kMaxEntries) {
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t *entry = manifestBlock + 24 + index * kEntryBytes;
        char name[33] = {};
        memcpy(name, entry, 32);
        uint64_t offset = 0, size = 0;
        uint32_t blockSize = 0;
        memcpy(&offset, entry + 32, sizeof(offset));
        memcpy(&size, entry + 40, sizeof(size));
        memcpy(&blockSize, entry + 48, sizeof(blockSize));
        if (strcmp(name, "OS") != 0) { continue; }
        if (blockSize != kBlockBytes || (offset % kBlockBytes) != 0 || (size % kBlockBytes) != 0) {
            return false;
        }
        const uint64_t firstSector = offset / kBlockBytes;
        const uint64_t blocks = size / kBlockBytes;
        if (blocks == 0 || firstSector >= userdataBlocks || blocks > userdataBlocks - firstSector) {
            return false;
        }
        partitionFirstLba = userdataFirstLba + firstSector;
        capacity = blocks;
        windowPublished = true;
        native_sdhci_root_layout = 1;
        native_sdhci_window_base_lba = partitionFirstLba;
        native_sdhci_window_blocks = blocks;
        IOLog("NativeSdhciBlock: SHNGROOT OS window base_lba=0x%llx blocks=0x%llx\n",
              partitionFirstLba, blocks);
        return true;
    }
    return false;
}

bool NativeSdhciBlock::start(IOService *provider)
{
    if (!provider || !IOBlockStorageDevice::start(provider)) {
        return false;
    }
    if (provider->getResources() != kIOReturnSuccess) {
        IOLog("NativeSdhciBlock: no memory resource\n");
        return false;
    }
    mapping = provider->mapDeviceMemoryWithIndex(0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!mapping || mapping->getLength() < 0x100) {
        IOLog("NativeSdhciBlock: cannot map hc_mem MMIO\n");
        return false;
    }
    registers = static_cast<uintptr_t>(mapping->getVirtualAddress());
    coreMapping = provider->mapDeviceMemoryWithIndex(1, kIOMapAnywhere | kIOMapInhibitCache);
    if (coreMapping && coreMapping->getLength() >= 0x100) {
        coreRegs = static_cast<uintptr_t>(coreMapping->getVirtualAddress());
    }
    ownsDevice = true;
    native_sdhci_mmio_physical = mapping->getPhysicalAddress();

    enableQualcommHcMode();
    ensureBusClockAndPower();
    resetCmdDat();

    IOLog("NativeSdhciBlock: QCOM SDHCI phys=0x%llx core=0x%llx CAP=0x%08x ver=0x%04x clk=0x%04x pwr=0x%02x hc=0x%02x hc2=0x%04x ps=0x%08x\n",
          static_cast<uint64_t>(mapping->getPhysicalAddress()),
          coreMapping ? static_cast<uint64_t>(coreMapping->getPhysicalAddress()) : 0ULL,
          read32(kCapabilities), static_cast<unsigned>(read16(kVersion)),
          static_cast<unsigned>(read16(kClockControl)), static_cast<unsigned>(read8(kPowerControl)),
          static_cast<unsigned>(read8(kHostControl1)), static_cast<unsigned>(read16(kHostControl2)),
          read32(kPresentState));

    gate = IOLockAlloc();
    if (!gate) {
        return false;
    }

    bool ready = wakeCardFromSleep();
    bool extOk = ready && readExtCsd();
    if (!extOk) {
        // Fallback to clean JEDEC 1-bit Default Speed init (avoids HS200/HS400 DLL tuning).
        if (reinitCardLegacy1Bit()) {
            (void)readExtCsd();
        }
    }

    uint64_t entryLba = 0;
    uint32_t entryCount = 0, entrySize = 0;
    if (!readGptHeader(&entryLba, &entryCount, &entrySize) ||
        !findUserdataPartition(entryLba, entryCount, entrySize)) {
        IOLog("NativeSdhciBlock: eMMC read path not ready\n");
        return false;
    }

    readRootLayout();
    online = true;
    native_sdhci_ready = 1;
    native_sdhci_sectors = capacity;
    IOLog("NativeSdhciBlock: online %llu MiB (%s, read-only)\n",
          (capacity * kBlockBytes) >> 20, windowPublished ? "SHNGROOT OS" : "raw userdata");
    registerService();
    return true;
}

IOReturn NativeSdhciBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                            IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count || !online ||
        block >= capacity || count > capacity - block || count > UINT64_MAX / kBlockBytes ||
        buffer->getLength() < count * kBlockBytes) {
        return kIOReturnBadArgument;
    }
    if (buffer->getDirection() != kIODirectionIn) {
        return kIOReturnNotWritable;
    }
    uint64_t actual = 0;
    IOReturn result = kIOReturnSuccess;
    IOLockLock(gate);
    while (online && actual < count * kBlockBytes) {
        if (!readSector(partitionFirstLba + block + actual / kBlockBytes)) {
            result = kIOReturnIOError;
            break;
        }
        if (buffer->writeBytes(actual, sectorBuf, kBlockBytes) != kBlockBytes) {
            result = kIOReturnIOError;
            break;
        }
        actual += kBlockBytes;
    }
    IOLockUnlock(gate);
    completion->action(completion->target, completion->parameter, result, actual);
    return kIOReturnSuccess;
}

void NativeSdhciBlock::stop(IOService *provider)
{
    online = false;
    IOBlockStorageDevice::stop(provider);
}

void NativeSdhciBlock::free()
{
    if (coreMapping) {
        coreMapping->release();
        coreMapping = nullptr;
    }
    if (mapping) {
        mapping->release();
        mapping = nullptr;
    }
    if (gate) {
        IOLockFree(gate);
        gate = nullptr;
    }
    registers = 0;
    coreRegs = 0;
    ownsDevice = false;
    IOBlockStorageDevice::free();
}
