// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// NativeSdhciBlock - Qualcomm MSM8956 (Snapdragon 650 / Kenzo) SDHCI 3.0
// read-only block transport for microSD (SDCC2 @ 0x07864900) / eMMC (SDCC1 @ 0x07824900).
//
// Supports:
//  - Inheriting live TRAN-state microSD/eMMC pre-initialized by uefi-kenzo (fast path)
//    with full hardware-verified Qualcomm SDCC4 fallback init (CORE_PWRCTL_CLEAR @ 0xE4).
//  - Multi-block PIO reads (CMD18 + AUTO_CMD12, up to 64 sectors/cmd) with CMD17 fallback.
//  - SHNGROOT v1/v2 manifest verification (SHA-256) at raw LBA 0 or GPT userdata,
//    publishing both "OS" (self) and "SystemOS" (child NativeSdhciBlock window).
//  - Owner-root verification exports (native_verified_block_*) for iPadOS 17.7.11 launchd.
// Strictly read-only: write requests are refused.

#include <IOKit/IOBSD.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <sys/param.h>
#include <sys/types.h>

extern "C" {
volatile uint32_t native_sdhci_ready = 0;
volatile uint32_t native_sdhci_gpt_userdata = 0;
volatile uint64_t native_sdhci_userdata_first = 0;
volatile uint64_t native_sdhci_userdata_last = 0;
volatile uint32_t native_sdhci_ext_csd_ok = 0;
volatile uint32_t native_sdhci_ext_csd_sectors = 0;
volatile uint32_t native_sdhci_root_layout = 0;
volatile uint32_t native_sdhci_windows_published = 0;
volatile uint64_t native_sdhci_window_base_lba = 0;
volatile uint64_t native_sdhci_window_blocks = 0;
volatile uint32_t native_sdhci_read_requests = 0;
volatile uint32_t native_sdhci_read_failures = 0;
volatile uint64_t native_sdhci_sectors = 0;
volatile uint64_t native_sdhci_mmio_physical = 0;
volatile uint32_t native_sdhci_diag[4] = {};

// Owner-root telemetry & verification exports consumed by XNU patches.
volatile uint32_t native_verified_block_root_device = 0;
volatile uint32_t native_verified_block_root_media_matches = 0;
volatile uint32_t native_verified_block_root_provider_depth = 0;
volatile uint32_t native_verified_block_root_uuid_match = 0;
volatile uint32_t native_verified_block_wrong_uuid_refused = 0;
char native_verified_block_root_uuid[37] = {};

int native_verified_block_root_authorized(dev_t device);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);

// Weak-import persistence & loader-release stubs for NativeRamBlock (run Data volume purely in RAM).
unsigned native_ufs_store_mode(void) { return 0; }
int native_ufs_store_open(uint64_t, uint64_t *, const char **) { return -1; }
int native_ufs_store_read(uint64_t, uint32_t, void *) { return -1; }
int native_ufs_store_write(uint64_t, uint32_t, const void *) { return -1; }
int native_ufs_store_write_cached(uint64_t, uint32_t, const void *) { return -1; }
int native_ufs_store_sync(void) { return 0; }
unsigned native_ufs_store_gpt_check(void) { return 0; }
bool ml_static_mfree_prepare(vm_offset_t, vm_size_t) { return false; }
}

namespace {

constexpr const char *kOsVolumeUuid       = "04769D26-2CA0-4825-B443-E2E5F88300F2";
constexpr const char *kSystemOsVolumeUuid = "8846FE78-2910-4297-AF97-D59BB6F8FC16";

// Standard SDHCI 3.0 register offsets (Qualcomm sdhci-msm hc_mem).
constexpr uint32_t kBlockSize       = 0x04;  // 16-bit
constexpr uint32_t kBlockCount      = 0x06;  // 16-bit
constexpr uint32_t kArgument        = 0x08;  // 32-bit
constexpr uint32_t kTransferCmd     = 0x0C;  // 32-bit atomic TRANSFER_MODE[15:0] | COMMAND[31:16]
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
constexpr uint32_t kNormalIntEnable = 0x34;  // 32-bit pair
constexpr uint32_t kNormalIntSignal = 0x38;  // 32-bit pair
constexpr uint32_t kHostControl2    = 0x3E;  // 16-bit
constexpr uint32_t kVendorSpecFunc  = 0x10C; // 32-bit Qualcomm CORE_VENDOR_SPEC

// Qualcomm SDCC core_mem offsets (verified from stock Kenzo sdhci-msm binary).
constexpr uint32_t kCorePower        = 0x00;
constexpr uint32_t kCoreHcMode       = 0x78;
constexpr uint32_t kCorePwrctlStatus = 0xDC;
constexpr uint32_t kCorePwrctlClear  = 0xE4; // NOTE: 0xE0 is CORE_PWRCTL_MASK!
constexpr uint32_t kCorePwrctlCtl    = 0xE8;

constexpr uint32_t kHcModeEn         = 1U << 0;
constexpr uint32_t kFfClkSwRstDis    = 1U << 13;

constexpr uint32_t kPresentCmdInhibit   = 1U << 0;
constexpr uint32_t kPresentDatInhibit   = 1U << 1;
constexpr uint32_t kPresentDat0Level    = 1U << 20;
constexpr uint16_t kIntCommandComplete  = 1U << 0;
constexpr uint16_t kIntTransferComplete = 1U << 1;
constexpr uint16_t kIntBufferReadReady  = 1U << 5;
constexpr uint16_t kIntError            = 1U << 15;

constexpr uint32_t kBlockBytes          = 512;
constexpr uint32_t kMaxMultiSectors     = 64;
constexpr uint16_t kTrnsReadSinglePio   = 0x0010U; // Read, Single, PIO
constexpr uint16_t kTrnsReadMultiPio    = 0x0036U; // Multi | Read | AutoCMD12 | BlkCntEn, PIO
constexpr uint16_t kCmdRespNone         = 0x0000U;
constexpr uint16_t kCmdRespR2           = 0x0009U;
constexpr uint16_t kCmdRespR3           = 0x0002U;
constexpr uint16_t kCmdRespR1           = 0x001AU;
constexpr uint16_t kCmdRespR1b          = 0x001BU;
constexpr uint16_t kCmdDataReadFlags    = 0x003AU;

struct ShngWindowSpec {
    char name[33];
    uint64_t firstLba;
    uint64_t blocks;
    uint8_t sha256[32];
};

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
    char *getProductString() override
    {
        return const_cast<char *>(isWindowChild ? "MSM8956 SDHCI SystemOS" : "MSM8956 SDHCI Storage");
    }
    char *getRevisionString() override { return const_cast<char *>("SDHCI3"); }
    char *getAdditionalDeviceInfoString() override
    {
        return const_cast<char *>("SHNGROOT PIO, read-only");
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

    void configureWindowChild(NativeSdhciBlock *parent, const ShngWindowSpec &spec);
    bool authenticatesVolume(const char *uuid, uint8_t *manifestDigest) const;
    bool readBlocksToDescriptor(IOMemoryDescriptor *buffer, uint64_t lba, uint64_t count, uint64_t *actualOut);

private:
    IOMemoryMap *mapping = nullptr;
    IOMemoryMap *coreMapping = nullptr;
    IOLock *gate = nullptr;
    NativeSdhciBlock *parentController = nullptr;
    uintptr_t registers = 0;
    uintptr_t coreRegs = 0;
    uint64_t capacity = 0;
    uint64_t partitionFirstLba = 0;
    uint64_t userdataFirstLba = 0;
    uint64_t userdataBlocks = 0;
    uint32_t intClearDelayUs = 2; // 100us during 400kHz init, 2us at 9.6MHz+ data clock
    bool isWindowChild = false;
    bool windowPublished = false;
    bool online = false;
    bool ownsDevice = false;
    bool useMultiBlock = true;
    char windowName[33] = {};
    uint8_t windowSha256[32] = {};
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
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void write16(uint32_t offset, uint16_t value)
    {
        *reinterpret_cast<volatile uint16_t *>(registers + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void write8(uint32_t offset, uint8_t value)
    {
        *reinterpret_cast<volatile uint8_t *>(registers + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
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
        __asm__ volatile("dsb sy" ::: "memory");
    }

    void ackPwrctlIrq();
    void setClockDivider(uint16_t divider);
    void resetCmdDat();
    bool waitCommandIdle(uint32_t milliseconds);
    bool sendCommand(uint32_t commandIndex, uint32_t argument, uint16_t flags, uint32_t *respOut);
    bool reinitCardFallback(bool isSdCard);
    bool readSingleSectorLocked(uint64_t lba, uint8_t *dst512);
    bool readMultiSectorsToDescLocked(IOMemoryDescriptor *buffer, uint64_t byteOffset,
                                      uint64_t lba, uint32_t count);
    bool readGptHeader(uint64_t *entryLbaOut, uint32_t *entryCountOut, uint32_t *entrySizeOut);
    bool findUserdataPartition(uint64_t entryLba, uint32_t entryCount, uint32_t entrySize);
    bool readRootLayoutAt(uint64_t baseLba, uint64_t maxBlocks,
                          ShngWindowSpec *extraWindows, uint32_t *extraCountOut, uint32_t maxExtra);
};

OSDefineMetaClassAndStructors(NativeSdhciBlock, IOBlockStorageDevice)

void NativeSdhciBlock::configureWindowChild(NativeSdhciBlock *parent, const ShngWindowSpec &spec)
{
    isWindowChild = true;
    parentController = parent;
    partitionFirstLba = spec.firstLba;
    capacity = spec.blocks;
    windowPublished = true;
    strlcpy(windowName, spec.name, sizeof(windowName));
    memcpy(windowSha256, spec.sha256, sizeof(windowSha256));
}

bool NativeSdhciBlock::authenticatesVolume(const char *uuid, uint8_t *manifestDigest) const
{
    if (!online || !windowPublished || !uuid || strlen(uuid) != 36) {
        return false;
    }
    const char *expected = nullptr;
    if (strcmp(windowName, "OS") == 0) {
        expected = kOsVolumeUuid;
    } else if (strcmp(windowName, "SystemOS") == 0) {
        expected = kSystemOsVolumeUuid;
    } else {
        return false;
    }
    if (strcmp(uuid, expected) != 0) {
        return false;
    }
    if (manifestDigest) {
        static const char kDomain[] = "Kenzo.SHNGROOT.v1";
        SHA384_CTX ctx;
        SHA384_Init(&ctx);
        SHA384_Update(&ctx, kDomain, sizeof(kDomain));
        SHA384_Update(&ctx, windowName, strlen(windowName) + 1);
        SHA384_Update(&ctx, windowSha256, sizeof(windowSha256));
        SHA384_Update(&ctx, uuid, 36);
        SHA384_Final(manifestDigest, &ctx);
    }
    return true;
}

static int verifiedVolumeQuery(dev_t device, const char *expectedUUID, uint8_t *manifestDigest)
{
    native_verified_block_root_device = static_cast<uint32_t>(device);
    native_verified_block_root_media_matches = 0;
    native_verified_block_root_provider_depth = 0;
    native_verified_block_root_uuid_match = 0;
    native_verified_block_wrong_uuid_refused = 0;
    bzero(native_verified_block_root_uuid, sizeof(native_verified_block_root_uuid));

    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(2);
    OSNumber *mjr = OSNumber::withNumber(static_cast<unsigned>(major(static_cast<uint32_t>(device))), 32);
    OSNumber *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(static_cast<uint32_t>(device))), 32);
    if (!matching || !properties || !mjr || !mnr) {
        OSSafeReleaseNULL(mnr);
        OSSafeReleaseNULL(mjr);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(matching);
        return 0;
    }
    bool ready = properties->setObject(kIOBSDMajorKey, mjr) &&
                 properties->setObject(kIOBSDMinorKey, mnr) &&
                 matching->setObject("IOPropertyMatch", properties);
    mnr->release();
    mjr->release();
    properties->release();
    OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
    matching->release();
    if (!iterator) {
        return 0;
    }

    bool authenticated = false;
    unsigned matches = 0;
    unsigned depth = 0;
    while (OSObject *object = iterator->getNextObject()) {
        IOService *media = OSDynamicCast(IOService, object);
        if (!media) {
            continue;
        }
        if (++matches != 1) {
            authenticated = false;
            break;
        }
        OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
        if (!uuid || uuid->getLength() != 36 ||
            (expectedUUID && strcmp(expectedUUID, uuid->getCStringNoCopy()) != 0)) {
            continue;
        }
        strlcpy(native_verified_block_root_uuid, uuid->getCStringNoCopy(), sizeof(native_verified_block_root_uuid));
        for (IOService *prov = media; prov && depth < 64; prov = prov->getProvider()) {
            ++depth;
            NativeSdhciBlock *block = OSDynamicCast(NativeSdhciBlock, prov);
            if (!block) {
                continue;
            }
            authenticated = block->authenticatesVolume(uuid->getCStringNoCopy(), manifestDigest);
            native_verified_block_root_uuid_match = authenticated ? 1U : 0U;
            char wrong[37];
            strlcpy(wrong, uuid->getCStringNoCopy(), sizeof(wrong));
            wrong[0] ^= 1;
            native_verified_block_wrong_uuid_refused = !block->authenticatesVolume(wrong, nullptr) ? 1U : 0U;
            break;
        }
    }
    iterator->release();
    native_verified_block_root_media_matches = matches;
    native_verified_block_root_provider_depth = depth;
    if (!authenticated || matches != 1) {
        if (manifestDigest) {
            bzero(manifestDigest, SHA384_DIGEST_LENGTH);
        }
        return 0;
    }
    return 1;
}

extern "C" int native_verified_block_root_authorized(dev_t device)
{
    return verifiedVolumeQuery(device, nullptr, nullptr);
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest) {
        return 0;
    }
    bzero(digest, SHA384_DIGEST_LENGTH);
    if (!uuid || strlen(uuid) != 36) {
        return 0;
    }
    return verifiedVolumeQuery(device, uuid, digest);
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    if (!uuid || !device || !path || size < 6 || strlen(uuid) != 36) {
        return 0;
    }
    *device = NODEV;
    path[0] = 0;

    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *wanted = OSString::withCString(uuid);
    if (!matching || !properties || !wanted) {
        OSSafeReleaseNULL(wanted);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(matching);
        return 0;
    }
    bool ready = properties->setObject("UUID", wanted) && matching->setObject("IOPropertyMatch", properties);
    wanted->release();
    properties->release();
    OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
    matching->release();
    if (!iterator) {
        return 0;
    }

    unsigned matches = 0;
    bool authenticated = false;
    dev_t candidate = NODEV;
    char candidatePath[64] = {};
    while (OSObject *object = iterator->getNextObject()) {
        IOService *media = OSDynamicCast(IOService, object);
        if (!media) {
            continue;
        }
        if (++matches != 1) {
            authenticated = false;
            break;
        }
        OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
        OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (!identity || strcmp(identity->getCStringNoCopy(), uuid) != 0 || !name || !mjr || !mnr ||
            !name->getLength() || name->getLength() >= sizeof(candidatePath) - 6 || name->getLength() >= size - 6 ||
            mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) {
            continue;
        }
        candidate = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
        if (!native_verified_block_root_authorized(candidate)) {
            continue;
        }
        snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
        authenticated = true;
    }
    iterator->release();
    if (!authenticated || matches != 1) {
        return 0;
    }
    *device = candidate;
    strlcpy(path, candidatePath, size);
    return 1;
}

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
    for (uint32_t spin = 0; spin < 500; ++spin) {
        const uint32_t st = coreRead32(kCorePwrctlStatus);
        if (st != 0) {
            // CORE_PWRCTL_CLEAR is 0xE4 (0xE0 is CORE_PWRCTL_MASK).
            coreWrite32(kCorePwrctlClear, st);
            for (uint32_t w = 0; w < 500; ++w) {
                if ((coreRead32(kCorePwrctlStatus) & st) == 0) {
                    break;
                }
                IODelay(10);
            }
            // BUS_SUCCESS = BIT0 (0x1), IO_SUCCESS = BIT2 (0x4). Never set BIT1 (BUS_FAIL).
            uint32_t ctl = 0;
            if ((st & 0x03U) != 0) {
                ctl |= 0x01U;
            }
            if ((st & 0x0CU) != 0) {
                ctl |= 0x04U;
            }
            if (ctl != 0) {
                coreWrite32(kCorePwrctlCtl, ctl);
            }
            IODelay(100);
            return;
        }
        IODelay(10);
    }
}

void NativeSdhciBlock::setClockDivider(uint16_t divider)
{
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
    write16(kClockControl, static_cast<uint16_t>(clk | 0x0004U));
    IODelay(1000);
    intClearDelayUs = (divider >= 8U) ? 100U : 2U;
}

void NativeSdhciBlock::resetCmdDat()
{
    write8(kSoftwareReset, 0x06U);
    for (uint32_t i = 0; i < 10000; ++i) {
        if ((read8(kSoftwareReset) & 0x06U) == 0) {
            break;
        }
        IODelay(10);
    }
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    IODelay(intClearDelayUs);
}

bool NativeSdhciBlock::waitCommandIdle(uint32_t milliseconds)
{
    for (uint32_t waited = 0; waited < milliseconds * 100U; ++waited) {
        if ((read32(kPresentState) & (kPresentCmdInhibit | kPresentDatInhibit)) == 0) {
            return true;
        }
        IODelay(10);
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
    IODelay(intClearDelayUs);
    write8(kTimeoutControl, 0x0EU);

    if (!waitCommandIdle(500)) {
        resetCmdDat();
        if (!waitCommandIdle(200)) {
            return false;
        }
    }

    write32(kArgument, argument);
    write32(kTransferCmd, static_cast<uint32_t>(((commandIndex & 0x3FU) << 8) | (flags & 0x00FFU)) << 16);

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
    IODelay(intClearDelayUs);
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

bool NativeSdhciBlock::reinitCardFallback(bool isSdCard)
{
    IOLog("NativeSdhciBlock: running Qualcomm SDCC4 %s fallback init\n", isSdCard ? "microSD" : "eMMC");
    if (coreRegs) {
        coreWrite32(kCoreHcMode, 0U);
        coreWrite32(kCorePower, coreRead32(kCorePower) | (1U << 7)); // CORE_SW_RST
        for (uint32_t i = 0; i < 50000; ++i) {
            if ((coreRead32(kCorePower) & (1U << 7)) == 0) {
                break;
            }
            IODelay(10);
        }
        coreWrite32(kCoreHcMode, kHcModeEn);
        coreWrite32(kCoreHcMode, kHcModeEn | kFfClkSwRstDis);
        ackPwrctlIrq();
    }

    write8(kSoftwareReset, 0x01U);
    for (uint32_t i = 0; i < 10000; ++i) {
        if ((read8(kSoftwareReset) & 0x01U) == 0) {
            break;
        }
        IODelay(10);
    }
    ackPwrctlIrq();

    uint32_t vfunc = read32(kVendorSpecFunc) & ~((0xFU << 18) | (1U << 1));
    if (isSdCard) {
        vfunc = (vfunc | (1U << 15)) & ~(1U << 16);
    }
    write32(kVendorSpecFunc, vfunc);
    write8(kHostControl1, 0x00U);
    write16(kHostControl2, 0x0000U);
    write8(kPowerControl, isSdCard ? 0x0FU : 0x0BU);
    ackPwrctlIrq();
    ackPwrctlIrq();
    IODelay(5000);
    setClockDivider(0x0018U); // 400 kHz from 19.2 MHz XO

    uint32_t resp = 0;
    (void)sendCommand(0U, 0U, kCmdRespNone, nullptr);
    IODelay(5000);

    if (isSdCard) {
        if (!sendCommand(8U, 0x1AAU, kCmdRespR1, &resp)) {
            return false;
        }
        bool ready = false;
        for (uint32_t attempt = 0; attempt < 200; ++attempt) {
            if (!sendCommand(55U, 0U, kCmdRespR1, &resp)) {
                break;
            }
            if (sendCommand(41U, 0x40FF8000U, kCmdRespR3, &resp) && (resp & 0x80000000U) != 0) {
                ready = true;
                break;
            }
            IODelay(5000);
        }
        if (!ready || !sendCommand(2U, 0U, kCmdRespR2, &resp) || !sendCommand(3U, 0U, kCmdRespR1, &resp)) {
            return false;
        }
        const uint32_t rca = resp & 0xFFFF0000U;
        if (!sendCommand(7U, rca, kCmdRespR1b, &resp)) {
            return false;
        }
        (void)sendCommand(16U, kBlockBytes, kCmdRespR1, &resp);
        setClockDivider(0x0001U); // 9.6 MHz DS
        return sendCommand(13U, rca, kCmdRespR1, &resp) && (((resp >> 9) & 0xFU) == 4U);
    }

    bool ocrReady = false;
    for (uint32_t attempt = 0; attempt < 200; ++attempt) {
        if (sendCommand(1U, 0x40FF8080U, kCmdRespR3, &resp) && (resp & 0x80000000U) != 0) {
            ocrReady = true;
            break;
        }
        IODelay(2000);
    }
    if (!ocrReady || !sendCommand(2U, 0U, kCmdRespR2, &resp) ||
        !sendCommand(3U, 2U << 16, kCmdRespR1, &resp) ||
        !sendCommand(7U, 2U << 16, kCmdRespR1b, &resp)) {
        return false;
    }
    (void)sendCommand(16U, kBlockBytes, kCmdRespR1, &resp);
    setClockDivider(0x0001U); // 9.6 MHz DS
    return sendCommand(13U, 2U << 16, kCmdRespR1, &resp) && (((resp >> 9) & 0xFU) == 4U);
}

bool NativeSdhciBlock::readSingleSectorLocked(uint64_t lba, uint8_t *dst512)
{
    if (!ownsDevice || !registers || lba > 0xFFFFFFFFULL) {
        return false;
    }
    write32(kNormalIntSignal, 0U);
    write32(kNormalIntEnable, 0xFFFFFFFFU);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    IODelay(intClearDelayUs);
    write8(kTimeoutControl, 0x0EU);

    if (!waitCommandIdle(500)) {
        resetCmdDat();
        if (!waitCommandIdle(200)) {
            return false;
        }
    }

    write16(kBlockSize, static_cast<uint16_t>(kBlockBytes));
    write16(kBlockCount, 1U);
    write32(kArgument, static_cast<uint32_t>(lba));
    write32(kTransferCmd, static_cast<uint32_t>(kTrnsReadSinglePio) |
                          (static_cast<uint32_t>((17U << 8) | kCmdDataReadFlags) << 16));

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
        if (!bufRead && (normStatus & kIntBufferReadReady) != 0) {
            write16(kNormalIntStatus, kIntBufferReadReady);
            uint32_t *words = reinterpret_cast<uint32_t *>(dst512);
            for (uint32_t w = 0; w < kBlockBytes / 4; ++w) {
                words[w] = read32(kBufferPort);
            }
            bufRead = true;
        }
        if (bufRead && (normStatus & kIntTransferComplete) != 0) {
            write16(kNormalIntStatus, kIntTransferComplete);
            xferDone = true;
            break;
        }
        IODelay(1);
    }

    write32(kNormalIntStatus, 0xFFFFFFFFU);
    IODelay(intClearDelayUs);

    if (errStatus != 0 || !bufRead || !xferDone) {
        native_sdhci_diag[3] = (17U << 24) | (static_cast<uint32_t>(errStatus) << 16) | normStatus;
        __atomic_fetch_add(&native_sdhci_read_failures, 1U, __ATOMIC_RELAXED);
        resetCmdDat();
        return false;
    }
    __atomic_fetch_add(&native_sdhci_read_requests, 1U, __ATOMIC_RELAXED);
    return true;
}

bool NativeSdhciBlock::readMultiSectorsToDescLocked(IOMemoryDescriptor *buffer, uint64_t byteOffset,
                                                    uint64_t lba, uint32_t count)
{
    if (!ownsDevice || !registers || count == 0 || count > kMaxMultiSectors || lba > 0xFFFFFFFFULL) {
        return false;
    }
    write32(kNormalIntSignal, 0U);
    write32(kNormalIntEnable, 0xFFFFFFFFU);
    write32(kNormalIntStatus, 0xFFFFFFFFU);
    IODelay(intClearDelayUs);
    write8(kTimeoutControl, 0x0EU);

    if (!waitCommandIdle(500)) {
        resetCmdDat();
        if (!waitCommandIdle(200)) {
            return false;
        }
    }

    write16(kBlockSize, static_cast<uint16_t>(kBlockBytes));
    write16(kBlockCount, static_cast<uint16_t>(count));
    write32(kArgument, static_cast<uint32_t>(lba));
    write32(kTransferCmd, static_cast<uint32_t>(kTrnsReadMultiPio) |
                          (static_cast<uint32_t>((18U << 8) | kCmdDataReadFlags) << 16));

    uint32_t done = 0;
    bool xferDone = false;
    uint16_t errStatus = 0;
    uint16_t normStatus = 0;

    for (uint32_t spin = 0; spin < 500000; ++spin) {
        normStatus = read16(kNormalIntStatus);
        if ((normStatus & kIntError) != 0) {
            errStatus = read16(kErrorIntStatus);
            break;
        }
        if (done < count && (normStatus & kIntBufferReadReady) != 0) {
            write16(kNormalIntStatus, kIntBufferReadReady);
            uint32_t *words = reinterpret_cast<uint32_t *>(sectorBuf);
            for (uint32_t w = 0; w < kBlockBytes / 4; ++w) {
                words[w] = read32(kBufferPort);
            }
            if (buffer->writeBytes(byteOffset + static_cast<uint64_t>(done) * kBlockBytes,
                                   sectorBuf, kBlockBytes) != kBlockBytes) {
                resetCmdDat();
                return false;
            }
            ++done;
            spin = 0;
        }
        if (done == count && (normStatus & kIntTransferComplete) != 0) {
            write16(kNormalIntStatus, kIntTransferComplete);
            xferDone = true;
            break;
        }
        IODelay(1);
    }

    write32(kNormalIntStatus, 0xFFFFFFFFU);
    IODelay(intClearDelayUs);

    if (errStatus != 0 || done != count || !xferDone) {
        native_sdhci_diag[3] = (18U << 24) | (static_cast<uint32_t>(errStatus) << 16) | normStatus;
        resetCmdDat();
        // Ensure card returns cleanly to TRAN if CMD18 aborted before AUTO_CMD12.
        uint32_t stopResp = 0;
        (void)sendCommand(12U, 0U, kCmdRespR1b, &stopResp);
        return false;
    }
    __atomic_fetch_add(&native_sdhci_read_requests, count, __ATOMIC_RELAXED);
    return true;
}

bool NativeSdhciBlock::readBlocksToDescriptor(IOMemoryDescriptor *buffer, uint64_t lba,
                                              uint64_t count, uint64_t *actualOut)
{
    uint64_t doneBlocks = 0;
    IOLockLock(gate);
    while (doneBlocks < count) {
        const uint64_t remain = count - doneBlocks;
        if (useMultiBlock && remain >= 2) {
            const uint32_t chunk = static_cast<uint32_t>(remain > kMaxMultiSectors ? kMaxMultiSectors : remain);
            if (readMultiSectorsToDescLocked(buffer, doneBlocks * kBlockBytes, lba + doneBlocks, chunk)) {
                doneBlocks += chunk;
                continue;
            }
            IOLog("NativeSdhciBlock: CMD18 fallback to CMD17 at lba=0x%llx\n", lba + doneBlocks);
            useMultiBlock = false;
        }
        if (!readSingleSectorLocked(lba + doneBlocks, sectorBuf)) {
            break;
        }
        if (buffer->writeBytes(doneBlocks * kBlockBytes, sectorBuf, kBlockBytes) != kBlockBytes) {
            break;
        }
        ++doneBlocks;
    }
    IOLockUnlock(gate);
    *actualOut = doneBlocks * kBlockBytes;
    return doneBlocks == count;
}

bool NativeSdhciBlock::readGptHeader(uint64_t *entryLbaOut, uint32_t *entryCountOut, uint32_t *entrySizeOut)
{
    if (!readSingleSectorLocked(1, sectorBuf)) {
        return false;
    }
    if (memcmp(sectorBuf, "EFI PART", 8) != 0) {
        return false;
    }
    uint64_t entryLba = 0;
    uint32_t entryCount = 0;
    uint32_t entrySize = 0;
    memcpy(&entryLba, sectorBuf + 72, sizeof(entryLba));
    memcpy(&entryCount, sectorBuf + 80, sizeof(entryCount));
    memcpy(&entrySize, sectorBuf + 84, sizeof(entrySize));
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
        if (!readSingleSectorLocked(entryLba + sector, sectorBuf)) {
            return false;
        }
        for (uint32_t slot = 0; slot < entriesPerSector; ++slot) {
            const uint32_t index = sector * entriesPerSector + slot;
            if (index >= entryCount) {
                break;
            }
            const uint8_t *entry = sectorBuf + slot * entrySize;
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
                return true;
            }
        }
    }
    return false;
}

bool NativeSdhciBlock::readRootLayoutAt(uint64_t baseLba, uint64_t maxBlocks,
                                        ShngWindowSpec *extraWindows, uint32_t *extraCountOut, uint32_t maxExtra)
{
    constexpr uint32_t kManifestBytes = 4096;
    constexpr uint32_t kManifestSectors = kManifestBytes / kBlockBytes;
    constexpr uint32_t kEntryBytes = 128;
    constexpr uint32_t kMaxEntries = 8;

    *extraCountOut = 0;
    if (maxBlocks < kManifestSectors) {
        return false;
    }
    for (uint32_t s = 0; s < kManifestSectors; ++s) {
        if (!readSingleSectorLocked(baseLba + s, manifestBlock + s * kBlockBytes)) {
            return false;
        }
    }
    if (memcmp(manifestBlock, "SHNGROOT", 8) != 0) {
        return false;
    }

    // Verify SHA-256 of bytes 0..4063 matches trailer at 4064..4095.
    uint8_t digest[32] = {};
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, manifestBlock, 4064);
    SHA256_Final(digest, &ctx);
    if (memcmp(digest, manifestBlock + 4064, sizeof(digest)) != 0) {
        IOLog("NativeSdhciBlock: SHNGROOT header SHA-256 mismatch at lba=0x%llx\n", baseLba);
        return false;
    }

    uint32_t version = 0, count = 0;
    uint64_t imageBytes = 0;
    memcpy(&version, manifestBlock + 8, sizeof(version));
    memcpy(&count, manifestBlock + 12, sizeof(count));
    memcpy(&imageBytes, manifestBlock + 16, sizeof(imageBytes));
    if ((version != 1 && version != 2) || count == 0 || count > kMaxEntries) {
        return false;
    }

    bool foundOs = false;
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t *entry = manifestBlock + 24 + index * kEntryBytes;
        ShngWindowSpec spec = {};
        memcpy(spec.name, entry, 32);
        spec.name[32] = '\0';
        uint64_t offset = 0, size = 0;
        uint32_t blockSize = 0, flags = 0;
        memcpy(&offset, entry + 32, sizeof(offset));
        memcpy(&size, entry + 40, sizeof(size));
        memcpy(&blockSize, entry + 48, sizeof(blockSize));
        memcpy(&flags, entry + 52, sizeof(flags));
        memcpy(spec.sha256, entry + 56, 32);

        if (flags != 1 || blockSize != kBlockBytes || (offset % kBlockBytes) != 0 || (size % kBlockBytes) != 0) {
            continue;
        }
        const uint64_t firstSector = offset / kBlockBytes;
        const uint64_t blocks = size / kBlockBytes;
        if (blocks == 0 || firstSector >= maxBlocks || blocks > maxBlocks - firstSector) {
            continue;
        }
        spec.firstLba = baseLba + firstSector;
        spec.blocks = blocks;

        if (strcmp(spec.name, "OS") == 0 && !foundOs) {
            partitionFirstLba = spec.firstLba;
            capacity = spec.blocks;
            strlcpy(windowName, spec.name, sizeof(windowName));
            memcpy(windowSha256, spec.sha256, sizeof(windowSha256));
            windowPublished = true;
            native_sdhci_root_layout = 1;
            native_sdhci_window_base_lba = partitionFirstLba;
            native_sdhci_window_blocks = capacity;
            foundOs = true;
            IOLog("NativeSdhciBlock: SHNGROOT [OS] lba=0x%llx blocks=0x%llx (%llu MiB)\n",
                  partitionFirstLba, capacity, (capacity * kBlockBytes) >> 20);
        } else if (*extraCountOut < maxExtra) {
            extraWindows[(*extraCountOut)++] = spec;
            IOLog("NativeSdhciBlock: SHNGROOT [%s] lba=0x%llx blocks=0x%llx (%llu MiB)\n",
                  spec.name, spec.firstLba, spec.blocks, (spec.blocks * kBlockBytes) >> 20);
        }
    }
    return foundOs;
}

bool NativeSdhciBlock::start(IOService *provider)
{
    if (!provider || !IOBlockStorageDevice::start(provider)) {
        return false;
    }

    if (isWindowChild) {
        online = true;
        __atomic_fetch_add(&native_sdhci_windows_published, 1U, __ATOMIC_RELAXED);
        IOLog("NativeSdhciBlock: child window [%s] online %llu MiB (read-only)\n",
              windowName, (capacity * kBlockBytes) >> 20);
        registerService();
        return true;
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
    const uint64_t phys = mapping->getPhysicalAddress();
    native_sdhci_mmio_physical = phys;
    const bool isSdCard = (phys == 0x07864900ULL);

    gate = IOLockAlloc();
    if (!gate) {
        return false;
    }

    IOLog("NativeSdhciBlock: QCOM SDHCI phys=0x%llx (%s) PS=0x%08x CLK=0x%04x PWR=0x%02x HC1=0x%02x\n",
          phys, isSdCard ? "SDCC2 microSD" : "SDCC1 eMMC",
          read32(kPresentState), static_cast<unsigned>(read16(kClockControl)),
          static_cast<unsigned>(read8(kPowerControl)), static_cast<unsigned>(read8(kHostControl1)));

    // Fast path: uefi-kenzo (4c06de6+) leaves SDCC1 & SDCC2 powered and in TRAN 9.6 MHz DS.
    intClearDelayUs = 2U;
    bool mediaReadable = readSingleSectorLocked(0, sectorBuf);
    if (!mediaReadable) {
        mediaReadable = reinitCardFallback(isSdCard) && readSingleSectorLocked(0, sectorBuf);
    }
    if (!mediaReadable) {
        IOLog("NativeSdhciBlock: media unreadable at phys=0x%llx\n", phys);
        return false;
    }

    ShngWindowSpec extraWindows[4] = {};
    uint32_t extraCount = 0;

    // 1. Prefer raw-disk SHNGROOT at LBA 0 (microSD dedicated rootfs).
    bool foundShng = readRootLayoutAt(0, 0x10000000ULL, extraWindows, &extraCount, 4);
    if (!foundShng) {
        // 2. Fallback: GPT userdata partition (eMMC layout).
        uint64_t entryLba = 0;
        uint32_t entryCount = 0, entrySize = 0;
        if (readGptHeader(&entryLba, &entryCount, &entrySize) &&
            findUserdataPartition(entryLba, entryCount, entrySize)) {
            foundShng = readRootLayoutAt(userdataFirstLba, userdataBlocks, extraWindows, &extraCount, 4);
            if (!foundShng) {
                partitionFirstLba = userdataFirstLba;
                capacity = userdataBlocks;
            }
        } else {
            IOLog("NativeSdhciBlock: neither SHNGROOT at LBA 0 nor GPT userdata found\n");
            return false;
        }
    }

    online = true;
    native_sdhci_ready = 1;
    native_sdhci_sectors = capacity;
    __atomic_fetch_add(&native_sdhci_windows_published, 1U, __ATOMIC_RELAXED);
    IOLog("NativeSdhciBlock: online %llu MiB (%s, read-only, extra_windows=%u)\n",
          (capacity * kBlockBytes) >> 20, windowPublished ? windowName : "raw userdata", extraCount);
    registerService();

    // Publish additional SHNGROOT windows (e.g. SystemOS) as sibling IOBlockStorageDevice children.
    for (uint32_t i = 0; i < extraCount; ++i) {
        NativeSdhciBlock *child = OSTypeAlloc(NativeSdhciBlock);
        if (!child) {
            continue;
        }
        if (!child->init(nullptr)) {
            child->release();
            continue;
        }
        child->configureWindowChild(this, extraWindows[i]);
        if (!child->attach(this)) {
            child->release();
            continue;
        }
        if (!child->start(this)) {
            child->detach(this);
            child->release();
            continue;
        }
        child->release(); // Retained by IORegistry attachment
    }

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
    NativeSdhciBlock *ctrl = isWindowChild ? parentController : this;
    if (!ctrl || !ctrl->online) {
        return kIOReturnNotReady;
    }
    uint64_t actual = 0;
    const bool ok = ctrl->readBlocksToDescriptor(buffer, partitionFirstLba + block, count, &actual);
    completion->action(completion->target, completion->parameter, ok ? kIOReturnSuccess : kIOReturnIOError, actual);
    return kIOReturnSuccess;
}

void NativeSdhciBlock::stop(IOService *provider)
{
    online = false;
    IOBlockStorageDevice::stop(provider);
}

void NativeSdhciBlock::free()
{
    if (!isWindowChild) {
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
    }
    parentController = nullptr;
    IOBlockStorageDevice::free();
}
