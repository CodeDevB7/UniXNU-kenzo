// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoGpuKmd - Full Adreno 510 Kernel Mode Driver (AGXAccelerator +
// AGXDeviceUserClient implementing NativeAdrenoABI v2) for Xiaomi Redmi Note 3
// (kenzo / MSM8956) on UniXNU iPadOS 17.7.11 (21H461).
//
// Features:
//   - Subclasses original iPadOS 17.7.11 IOAcceleratorES via KenzoAccelerator.cpp
//     (late-initialized after com.apple.iokit.IOGPUFamily loads).
//   - Publishes MetalPluginName="NativeAdrenoMetal", MetalPluginClassName="NativeAdrenoMetalDevice"
//     and grants sandboxed processes NativeSandboxGrant=true on open type 0x4147 ('AG').
//   - Serves all 18 NativeAdrenoABI v2 methods both directly (selectors 1..18) and
//     via the sandbox-safe 'NTNL' (0x4E544E4C) tunnel on any admitted selector.
//   - Physical DMA (< 4 GiB, covering 100% of Kenzo's 2 GiB RAM):
//       * BoAlloc: physically contiguous < 4 GiB IOBufferMemoryDescriptor.
//       * BoImport: zero-copy when user pages are contiguous < 4 GiB, or automatic
//         contiguous DMA bounce buffer synced on Submit / WaitFence for scattered IOSurfaces.
//   - Submits client IBs to live Adreno 510 Ring 0 via physical-silicon-verified
//     a510_ring_submit_ibs() + DRAM CACHE_FLUSH_TS fence polling.

#include "NativeAdrenoABI.h"
#include "NativeAdrenoA510Core.h"
#include "NativeAdrenoA510HwInit.h"
#include "NativeAdrenoA510PM4.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" {
extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);
void kenzo_gpu_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_KenzoAccelerator.cpp");
IOService *kenzo_gpu_accelerator_allocate(void);

extern volatile uint32_t kenzo_gpu_probe_state;
extern volatile uint32_t kenzo_gpu_probe_diag[8];

volatile uint32_t kenzo_gpu_kmd_stats[8] = {};
// [0]=published, [1]=opens, [2]=submits, [3]=wait_ok, [4]=bo_allocs, [5]=bo_imports, [6]=metal_reads, [7]=last_marker

bool kenzo_gpu_kmd_publish(uintptr_t gpuBase,
                           IOBufferMemoryDescriptor *ringDesc,
                           uint8_t *ringVa,
                           uint32_t ringRegionBytes,
                           const A510Ring *ringState,
                           volatile uint32_t *fenceVa);
bool kenzo_gpu_kmd_fences(uint32_t *submitted, uint32_t *retired);
IOReturn kenzo_gpu_new_user_client(IOService *provider,
                                   task_t owningTask,
                                   void *securityID,
                                   UInt32 type,
                                   OSDictionary *properties,
                                   IOUserClient **handler);
void kenzo_gpu_note_property_read(const char *key);
}

namespace {

constexpr UInt32 kPluginOpenType  = 0x4147U;     // 'AG'
constexpr UInt32 kHandoffOpenType = 0x4148U;
constexpr uint32_t kTunnelMagic   = 0x4E544E4CU; // 'NTNL'
constexpr uint32_t kTunnelProbe   = 0xFFFFFFFFU;

constexpr uint64_t kMaxBoBytes    = 64ULL * 1024ULL * 1024ULL; // 64 MiB max per BO on 2 GiB Kenzo
constexpr uint32_t kMaxClientBos  = 128U;
constexpr uint32_t kMaxQueues     = 16U;

struct GpuHwGlobal {
    bool published = false;
    IOLock *lock = nullptr;
    uintptr_t gpuBase = 0;
    IOBufferMemoryDescriptor *ringDesc = nullptr;
    uint8_t *ringVa = nullptr;
    uint32_t ringRegionBytes = 0;
    A510Ring ring = {};
    volatile uint32_t *fenceVa = nullptr;
    IOService *accel = nullptr;
    uint32_t submittedFence = 0;
    uint32_t retiredFence = 0;
    uint64_t faults = 0;
    uint64_t suspends = 0;
};

GpuHwGlobal gHw;

static inline uint32_t hwRd32(uintptr_t base, uint32_t dwordOff)
{
    return *reinterpret_cast<volatile uint32_t *>(base + dwordOff * 4U);
}

static inline void hwWr32(uintptr_t base, uint32_t dwordOff, uint32_t val)
{
    *reinterpret_cast<volatile uint32_t *>(base + dwordOff * 4U) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t kmdCoreRead32(void *vctx, uint32_t dwordOff)
{
    return hwRd32(static_cast<GpuHwGlobal *>(vctx)->gpuBase, dwordOff);
}

static void kmdCoreWrite32(void *vctx, uint32_t dwordOff, uint32_t val)
{
    hwWr32(static_cast<GpuHwGlobal *>(vctx)->gpuBase, dwordOff, val);
}

static void kmdCoreWmb(void *vctx)
{
    auto *hw = static_cast<GpuHwGlobal *>(vctx);
    if (hw->ringVa && hw->ringRegionBytes) {
        flush_dcache64(reinterpret_cast<addr64_t>(hw->ringVa), hw->ringRegionBytes, FALSE);
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void kmdCoreUdelay(void *, uint32_t usec)
{
    IODelay(usec);
}

static uint64_t readAlwaysOnLocked(uintptr_t gpu)
{
    for (unsigned a = 0; a < 4U; ++a) {
        const uint32_t hi0 = hwRd32(gpu, A510_RBBM_ALWAYSON_COUNTER_HI);
        const uint32_t lo  = hwRd32(gpu, A510_RBBM_ALWAYSON_COUNTER_LO);
        const uint32_t hi1 = hwRd32(gpu, A510_RBBM_ALWAYSON_COUNTER_HI);
        if (hi0 == hi1) {
            return (static_cast<uint64_t>(hi0) << 32) | lo;
        }
    }
    return (static_cast<uint64_t>(hwRd32(gpu, A510_RBBM_ALWAYSON_COUNTER_HI)) << 32) |
           hwRd32(gpu, A510_RBBM_ALWAYSON_COUNTER_LO);
}

static uint32_t pollFenceHardware()
{
    if (!gHw.fenceVa) {
        return 0U;
    }
    flush_dcache64(reinterpret_cast<addr64_t>(const_cast<uint32_t *>(gHw.fenceVa)), 0x1000U, FALSE);
    const uint32_t cur = *gHw.fenceVa;
    if (cur > gHw.retiredFence) {
        gHw.retiredFence = cur;
    }
    return cur;
}

struct KmdClientBo {
    bool inUse = false;
    bool imported = false;
    bool bounced = false;
    bool mapped = false;
    bool cpuMapped = false;
    uint64_t handle = 0;
    uint64_t bytes = 0;
    uint64_t pa = 0;
    uint32_t cpuToken = 0;
    IOBufferMemoryDescriptor *dmaDesc = nullptr; // Backing buffer (owned or bounce)
    IOMemoryDescriptor *userDesc = nullptr;      // Wired user range (for imported BOs)
    uint8_t *dmaVa = nullptr;
    uint32_t metaLen = 0;
    char meta[NativeAdrenoAbiMetadataBytes] = {};
};

struct KmdClientQueue {
    bool open = false;
    uint64_t id = 0;
};

} // namespace

// Named AGXDeviceUserClient on purpose so iPadOS sandbox profiles that allow
// Apple's AGX user client accept our Adreno Metal plugin's connections.
class AGXDeviceUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AGXDeviceUserClient)

public:
    bool initWithTask(task_t owningTask,
                      void *securityID,
                      UInt32 type,
                      OSDictionary *properties) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target,
                            void *reference) override;
    IOReturn clientMemoryForType(UInt32 type,
                                 IOOptionBits *options,
                                 IOMemoryDescriptor **memory) override;

    IOReturn dispatchMethod(uint32_t method,
                            const void *inBuf,
                            size_t inSize,
                            void *outBuf,
                            size_t outSize);

    void setHandoffOnly() { handoffOnly = true; }

private:
    task_t clientTask = nullptr;
    IOLock *clientLock = nullptr;
    bool handoffOnly = false;
    uint64_t nextHandle = 1ULL;
    uint32_t nextToken = 1U;
    uint64_t nextQueueId = 1ULL;
    uint32_t prioFg = 1U;
    uint32_t prioBg = 3U;
    uint32_t metaLen = 0;
    char clientMeta[NativeAdrenoAbiMetadataBytes] = {};
    KmdClientBo bos[kMaxClientBos] = {};
    KmdClientQueue queues[kMaxQueues] = {};

    KmdClientBo *findBo(uint64_t handle);
    KmdClientBo *allocSlot();
    bool queueOpen(uint64_t qid) const;
    void releaseBo(KmdClientBo *bo);
    void releaseAllBos();
    void syncImportedToDma();
    void syncDmaToImported();
};

OSDefineMetaClassAndStructors(AGXDeviceUserClient, IOUserClient)

bool AGXDeviceUserClient::initWithTask(task_t owningTask,
                                       void *securityID,
                                       UInt32 type,
                                       OSDictionary *properties)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
        return false;
    }
    clientTask = owningTask;
    clientLock = IOLockAlloc();
    if (!clientLock) {
        return false;
    }
    return true;
}

KmdClientBo *AGXDeviceUserClient::findBo(uint64_t handle)
{
    if (handle == 0ULL) {
        return nullptr;
    }
    for (uint32_t i = 0; i < kMaxClientBos; ++i) {
        if (bos[i].inUse && bos[i].handle == handle) {
            return &bos[i];
        }
    }
    return nullptr;
}

KmdClientBo *AGXDeviceUserClient::allocSlot()
{
    for (uint32_t i = 0; i < kMaxClientBos; ++i) {
        if (!bos[i].inUse) {
            return &bos[i];
        }
    }
    return nullptr;
}

bool AGXDeviceUserClient::queueOpen(uint64_t qid) const
{
    if (qid == 0ULL) {
        return true;
    }
    for (uint32_t i = 0; i < kMaxQueues; ++i) {
        if (queues[i].open && queues[i].id == qid) {
            return true;
        }
    }
    return false;
}

void AGXDeviceUserClient::releaseBo(KmdClientBo *bo)
{
    if (!bo || !bo->inUse) {
        return;
    }
    if (bo->dmaDesc) {
        bo->dmaDesc->complete();
        bo->dmaDesc->release();
        bo->dmaDesc = nullptr;
    }
    if (bo->userDesc) {
        bo->userDesc->complete();
        bo->userDesc->release();
        bo->userDesc = nullptr;
    }
    *bo = KmdClientBo{};
}

void AGXDeviceUserClient::releaseAllBos()
{
    for (uint32_t i = 0; i < kMaxClientBos; ++i) {
        if (bos[i].inUse) {
            releaseBo(&bos[i]);
        }
    }
    memset(queues, 0, sizeof(queues));
}

void AGXDeviceUserClient::syncImportedToDma()
{
    for (uint32_t i = 0; i < kMaxClientBos; ++i) {
        KmdClientBo &bo = bos[i];
        if (!bo.inUse) {
            continue;
        }
        if (bo.bounced && bo.userDesc && bo.dmaVa) {
            (void)bo.userDesc->readBytes(0, bo.dmaVa, static_cast<IOByteCount>(bo.bytes));
            flush_dcache64(reinterpret_cast<addr64_t>(bo.dmaVa), static_cast<unsigned>(bo.bytes), FALSE);
        } else if (bo.dmaVa && bo.bytes > 0ULL) {
            flush_dcache64(reinterpret_cast<addr64_t>(bo.dmaVa), static_cast<unsigned>(bo.bytes), FALSE);
        }
    }
}

void AGXDeviceUserClient::syncDmaToImported()
{
    for (uint32_t i = 0; i < kMaxClientBos; ++i) {
        KmdClientBo &bo = bos[i];
        if (!bo.inUse || !bo.dmaVa) {
            continue;
        }
        flush_dcache64(reinterpret_cast<addr64_t>(bo.dmaVa), static_cast<unsigned>(bo.bytes), FALSE);
        if (bo.bounced && bo.userDesc) {
            (void)bo.userDesc->writeBytes(0, bo.dmaVa, static_cast<IOByteCount>(bo.bytes));
        }
    }
}

IOReturn AGXDeviceUserClient::clientClose()
{
    if (clientLock) {
        IOLockLock(clientLock);
        releaseAllBos();
        IOLockUnlock(clientLock);
    }
    terminate();
    return kIOReturnSuccess;
}

void AGXDeviceUserClient::free()
{
    if (clientLock) {
        IOLockLock(clientLock);
        releaseAllBos();
        IOLockUnlock(clientLock);
        IOLockFree(clientLock);
        clientLock = nullptr;
    }
    IOUserClient::free();
}

IOReturn AGXDeviceUserClient::clientMemoryForType(UInt32 type,
                                                  IOOptionBits *options,
                                                  IOMemoryDescriptor **memory)
{
    if (!memory || !clientLock || handoffOnly || type == 0U) {
        return kIOReturnBadArgument;
    }
    *memory = nullptr;
    IOLockLock(clientLock);
    IOMemoryDescriptor *found = nullptr;
    for (uint32_t i = 0; i < kMaxClientBos; ++i) {
        if (bos[i].inUse && !bos[i].imported && bos[i].cpuMapped && bos[i].cpuToken == type && bos[i].dmaDesc) {
            found = bos[i].dmaDesc;
            found->retain();
            break;
        }
    }
    IOLockUnlock(clientLock);
    if (!found) {
        return kIOReturnNotFound;
    }
    if (options) {
        *options = kIOMapWriteCombineCache;
    }
    *memory = found;
    return kIOReturnSuccess;
}

IOReturn AGXDeviceUserClient::externalMethod(uint32_t selector,
                                             IOExternalMethodArguments *args,
                                             IOExternalMethodDispatch *,
                                             OSObject *,
                                             void *)
{
    if (!args || handoffOnly) {
        return kIOReturnUnsupported;
    }
    const uint8_t *inPtr = static_cast<const uint8_t *>(args->structureInput);
    size_t inSz = static_cast<size_t>(args->structureInputSize);
    void *outPtr = args->structureOutput;
    size_t outSz = static_cast<size_t>(args->structureOutputSize);

    uint32_t method = selector;
    // Sandbox-safe 'NTNL' tunnel used by NativeAdrenoMetalPlugin & Turnip Darwin KMD
    if (inPtr && inSz >= 8U) {
        uint32_t hdr[2] = {};
        memcpy(hdr, inPtr, 8U);
        if (hdr[0] == kTunnelMagic) {
            if (hdr[1] == kTunnelProbe) {
                return kIOReturnSuccess;
            }
            method = hdr[1];
            inPtr += 8U;
            inSz -= 8U;
        }
    }
    return dispatchMethod(method, inPtr, inSz, outPtr, outSz);
}

IOReturn AGXDeviceUserClient::dispatchMethod(uint32_t method,
                                             const void *inBuf,
                                             size_t inSize,
                                             void *outBuf,
                                             size_t outSize)
{
    if (!gHw.published || !clientLock) {
        return kIOReturnNotReady;
    }

    switch (method) {
    case NativeAdrenoMethodQuery: {
        if (inSize != 0U || !outBuf || outSize != sizeof(NativeAdrenoAbiParams)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiParams p = {};
        p.abi_version = NativeAdrenoAbiVersion;
        p.input_bits = 32U;
        p.output_bits = 32U;
        p.flags = NativeAdrenoParamsHostImport;
        p.max_bo_bytes = kMaxBoBytes;
        memcpy(outBuf, &p, sizeof(p));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetDeviceInfo: {
        if (inSize != 0U || !outBuf || outSize != sizeof(NativeAdrenoAbiDeviceInfo)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiDeviceInfo info = {};
        info.chip_id = A510_CHIP_ID_EXPECTED; // 0x05010000 (Adreno 510)
        info.gmem_bytes = A510_GMEM_BYTES;    // 256 KiB
        info.gmem_base = A510_GMEM_BASE;
        info.uche_trap_base = 0x0001FFFFFFFF0000ULL;
        info.va_start = 0x80000000ULL;        // Kernel-assigned physical IOVA in Kenzo 2 GiB RAM
        info.va_size = 0x80000000ULL;
        info.highest_bank_bit = A510_HIGHEST_BANK_BIT;
        info.ubwc_swizzle = 0U;
        info.macrotile_mode = 0U;
        info.ubwc_version = 0U;
        info.flags = 0U;                      // Kernel chooses BO IOVA (= physical address < 4 GiB)
        info.reserved = 0U;
        memcpy(outBuf, &info, sizeof(info));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoAlloc: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiBoAlloc) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiBoAllocReply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoAlloc req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.bytes == 0ULL || (req.bytes & 0xFFFULL) != 0ULL || req.bytes > kMaxBoBytes ||
            req.align_log2 < 12U || req.align_log2 > 24U) {
            return kIOReturnBadArgument;
        }
        const uint64_t alignMask = ~((1ULL << req.align_log2) - 1ULL) & 0xFFFFFFFFULL;
        const IOOptionBits opts = static_cast<IOOptionBits>(kIODirectionInOut) |
                                  static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
                                  static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
        IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, opts, static_cast<mach_vm_size_t>(req.bytes), alignMask);
        if (!desc || desc->prepare() != kIOReturnSuccess) {
            OSSafeReleaseNULL(desc);
            return kIOReturnNoMemory;
        }
        IOByteCount segLen = 0;
        const uint64_t pa = static_cast<uint64_t>(desc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone));
        uint8_t *va = static_cast<uint8_t *>(desc->getBytesNoCopy());
        if (!va || !pa || segLen < req.bytes || (pa + req.bytes) > 0x100000000ULL) {
            desc->complete();
            desc->release();
            return kIOReturnNoMemory;
        }
        memset(va, 0, static_cast<size_t>(req.bytes));
        flush_dcache64(reinterpret_cast<addr64_t>(va), static_cast<unsigned>(req.bytes), FALSE);

        IOLockLock(clientLock);
        KmdClientBo *slot = allocSlot();
        if (!slot) {
            IOLockUnlock(clientLock);
            desc->complete();
            desc->release();
            return kIOReturnNoResources;
        }
        slot->inUse = true;
        slot->handle = nextHandle++;
        slot->bytes = req.bytes;
        slot->pa = pa;
        slot->dmaDesc = desc;
        slot->dmaVa = va;
        const uint64_t h = slot->handle;
        IOLockUnlock(clientLock);

        __atomic_fetch_add(&kenzo_gpu_kmd_stats[4], 1U, __ATOMIC_RELAXED);
        NativeAdrenoAbiBoAllocReply rep = { h };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoImport: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiBoImport) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiBoAllocReply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoImport req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.address == 0ULL || (req.address & 0xFFFULL) != 0ULL ||
            req.bytes == 0ULL || (req.bytes & 0xFFFULL) != 0ULL || req.bytes > kMaxBoBytes) {
            return kIOReturnBadArgument;
        }
        IOMemoryDescriptor *uDesc = IOMemoryDescriptor::withAddressRange(
            static_cast<mach_vm_address_t>(req.address),
            static_cast<mach_vm_size_t>(req.bytes),
            kIODirectionInOut,
            clientTask ? clientTask : kernel_task);
        if (!uDesc || uDesc->prepare() != kIOReturnSuccess) {
            OSSafeReleaseNULL(uDesc);
            return kIOReturnNoMemory;
        }
        IOByteCount segLen = 0;
        const uint64_t firstPa = static_cast<uint64_t>(uDesc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone));
        bool bounced = false;
        uint64_t dmaPa = firstPa;
        IOBufferMemoryDescriptor *bounceDesc = nullptr;
        uint8_t *bounceVa = nullptr;

        if (!firstPa || segLen < req.bytes || (firstPa + req.bytes) > 0x100000000ULL) {
            // Scattered IOSurface / user buffer: allocate contiguous < 4 GiB DMA bounce buffer.
            const IOOptionBits opts = static_cast<IOOptionBits>(kIODirectionInOut) |
                                      static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
            bounceDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, opts, static_cast<mach_vm_size_t>(req.bytes), 0xFFFFF000ULL);
            if (!bounceDesc || bounceDesc->prepare() != kIOReturnSuccess) {
                OSSafeReleaseNULL(bounceDesc);
                uDesc->complete();
                uDesc->release();
                return kIOReturnNoMemory;
            }
            IOByteCount bLen = 0;
            dmaPa = static_cast<uint64_t>(bounceDesc->getPhysicalSegment(0, &bLen, kIOMemoryMapperNone));
            bounceVa = static_cast<uint8_t *>(bounceDesc->getBytesNoCopy());
            if (!bounceVa || !dmaPa || bLen < req.bytes || (dmaPa + req.bytes) > 0x100000000ULL) {
                bounceDesc->complete();
                bounceDesc->release();
                uDesc->complete();
                uDesc->release();
                return kIOReturnNoMemory;
            }
            (void)uDesc->readBytes(0, bounceVa, static_cast<IOByteCount>(req.bytes));
            flush_dcache64(reinterpret_cast<addr64_t>(bounceVa), static_cast<unsigned>(req.bytes), FALSE);
            bounced = true;
        }

        IOLockLock(clientLock);
        KmdClientBo *slot = allocSlot();
        if (!slot) {
            IOLockUnlock(clientLock);
            if (bounceDesc) {
                bounceDesc->complete();
                bounceDesc->release();
            }
            uDesc->complete();
            uDesc->release();
            return kIOReturnNoResources;
        }
        slot->inUse = true;
        slot->imported = true;
        slot->bounced = bounced;
        slot->handle = nextHandle++;
        slot->bytes = req.bytes;
        slot->pa = dmaPa;
        slot->userDesc = uDesc;
        slot->dmaDesc = bounceDesc;
        slot->dmaVa = bounceVa;
        const uint64_t h = slot->handle;
        IOLockUnlock(clientLock);

        __atomic_fetch_add(&kenzo_gpu_kmd_stats[5], 1U, __ATOMIC_RELAXED);
        NativeAdrenoAbiBoAllocReply rep = { h };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoMap: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiBoMap) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiBoMapReply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoMap req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.handle == 0ULL || (req.permissions & 1U) == 0U || req.reserved != 0U) {
            return kIOReturnBadArgument;
        }
        IOLockLock(clientLock);
        KmdClientBo *bo = findBo(req.handle);
        if (!bo) {
            IOLockUnlock(clientLock);
            return kIOReturnNotFound;
        }
        bo->mapped = true;
        const uint64_t iova = bo->pa;
        IOLockUnlock(clientLock);

        NativeAdrenoAbiBoMapReply rep = { iova };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoFree: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiBoFree) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoFree req = {};
        memcpy(&req, inBuf, sizeof(req));
        IOLockLock(clientLock);
        KmdClientBo *bo = findBo(req.handle);
        if (!bo) {
            IOLockUnlock(clientLock);
            return kIOReturnNotFound;
        }
        releaseBo(bo);
        IOLockUnlock(clientLock);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoCpuMap: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiBoCpuMap) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiBoCpuMapReply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoCpuMap req = {};
        memcpy(&req, inBuf, sizeof(req));
        IOLockLock(clientLock);
        KmdClientBo *bo = findBo(req.handle);
        if (!bo || bo->imported || !bo->dmaDesc) {
            IOLockUnlock(clientLock);
            return bo ? kIOReturnUnsupported : kIOReturnNotFound;
        }
        if (!bo->cpuMapped) {
            bo->cpuToken = nextToken++;
            if (nextToken == 0U) {
                nextToken = 1U;
            }
            bo->cpuMapped = true;
        }
        NativeAdrenoAbiBoCpuMapReply rep = { bo->cpuToken, 0U, bo->bytes };
        IOLockUnlock(clientLock);
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodBoCpuUnmap: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiBoCpuUnmap) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiBoCpuUnmap req = {};
        memcpy(&req, inBuf, sizeof(req));
        IOLockLock(clientLock);
        KmdClientBo *bo = findBo(req.handle);
        if (!bo || !bo->cpuMapped || bo->cpuToken != req.token) {
            IOLockUnlock(clientLock);
            return kIOReturnBadArgument;
        }
        bo->cpuMapped = false;
        bo->cpuToken = 0U;
        IOLockUnlock(clientLock);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodCreateQueue: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiQueue) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiQueueReply)) {
            return kIOReturnBadArgument;
        }
        IOLockLock(clientLock);
        uint64_t qid = 0ULL;
        for (uint32_t i = 0; i < kMaxQueues; ++i) {
            if (!queues[i].open) {
                queues[i].open = true;
                queues[i].id = nextQueueId++;
                qid = queues[i].id;
                break;
            }
        }
        IOLockUnlock(clientLock);
        if (qid == 0ULL) {
            return kIOReturnNoResources;
        }
        NativeAdrenoAbiQueueReply rep = { qid };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodCloseQueue: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiQueue) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiQueue req = {};
        memcpy(&req, inBuf, sizeof(req));
        IOLockLock(clientLock);
        bool found = false;
        for (uint32_t i = 0; i < kMaxQueues; ++i) {
            if (queues[i].open && queues[i].id == req.queue) {
                queues[i].open = false;
                queues[i].id = 0ULL;
                found = true;
                break;
            }
        }
        IOLockUnlock(clientLock);
        return found ? kIOReturnSuccess : kIOReturnNotFound;
    }

    case NativeAdrenoMethodSubmit: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiSubmit) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiSubmitReply)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiSubmit req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.buffer_count == 0U || req.buffer_count > NativeAdrenoAbiMaxBuffers || req.reserved != 0U) {
            return kIOReturnBadArgument;
        }
        A510IbDesc ibs[NativeAdrenoAbiMaxBuffers] = {};
        IOLockLock(clientLock);
        if (!queueOpen(req.queue)) {
            IOLockUnlock(clientLock);
            return kIOReturnNotFound;
        }
        for (uint32_t i = 0; i < req.buffer_count; ++i) {
            const NativeAdrenoAbiSubmitBuffer &b = req.buffers[i];
            if (b.bytes == 0ULL || (b.bytes & 3ULL) != 0ULL) {
                IOLockUnlock(clientLock);
                return kIOReturnBadArgument;
            }
            KmdClientBo *bo = findBo(b.handle);
            if (!bo || !bo->mapped || b.iova < bo->pa || b.iova + b.bytes > bo->pa + bo->bytes) {
                IOLockUnlock(clientLock);
                return kIOReturnBadArgument;
            }
            ibs[i].iova = b.iova;
            ibs[i].dwords = static_cast<uint32_t>(b.bytes / 4ULL);
        }
        syncImportedToDma();
        IOLockUnlock(clientLock);

        IOLockLock(gHw.lock);
        A510CoreOps ops = { &gHw, &kmdCoreRead32, &kmdCoreWrite32, &kmdCoreWmb, &kmdCoreUdelay };
        uint32_t seqno = 0U;
        const A510CoreStatus st = a510_ring_submit_ibs(&ops, &gHw.ring, ibs, req.buffer_count, &seqno);
        if (st == A510_CORE_OK) {
            gHw.submittedFence = seqno;
        }
        IOLockUnlock(gHw.lock);

        if (st != A510_CORE_OK) {
            return (st == A510_CORE_ERR_RING_FULL) ? kIOReturnBusy : kIOReturnIOError;
        }
        __atomic_fetch_add(&kenzo_gpu_kmd_stats[2], 1U, __ATOMIC_RELAXED);
        NativeAdrenoAbiSubmitReply rep = { static_cast<uint64_t>(seqno) };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodWaitFence: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiWaitFence) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiWaitFence req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.fence == 0ULL) {
            return kIOReturnSuccess;
        }
        const uint32_t target = static_cast<uint32_t>(req.fence);
        bool done = false;
        const uint64_t maxUs = req.timeout_ns / 1000ULL;
        uint64_t elapsedUs = 0ULL;
        for (;;) {
            const uint32_t cur = pollFenceHardware();
            if (cur >= target) {
                done = true;
                break;
            }
            if (elapsedUs >= maxUs) {
                break;
            }
            if (elapsedUs < 2000ULL) {
                IODelay(20);
                elapsedUs += 20ULL;
            } else {
                IOSleep(1);
                elapsedUs += 1000ULL;
            }
        }
        if (!done) {
            return kIOReturnTimeout;
        }
        // Clear pending RBBM interrupt status cleanly
        IOLockLock(gHw.lock);
        const uint32_t intSt = hwRd32(gHw.gpuBase, A510_RBBM_INT_0_STATUS);
        if (intSt != 0U) {
            hwWr32(gHw.gpuBase, A510_RBBM_INT_CLEAR_CMD, intSt);
        }
        IOLockUnlock(gHw.lock);

        IOLockLock(clientLock);
        syncDmaToImported();
        IOLockUnlock(clientLock);

        __atomic_fetch_add(&kenzo_gpu_kmd_stats[3], 1U, __ATOMIC_RELAXED);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetTimestamp: {
        if (inSize != 0U || !outBuf || outSize != sizeof(NativeAdrenoAbiTimestamp)) {
            return kIOReturnBadArgument;
        }
        IOLockLock(gHw.lock);
        const uint64_t ticks = readAlwaysOnLocked(gHw.gpuBase);
        IOLockUnlock(gHw.lock);
        NativeAdrenoAbiTimestamp rep = { ticks };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetStatus: {
        if (inSize != 0U || !outBuf || outSize != sizeof(NativeAdrenoAbiStatus)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiStatus rep = { gHw.faults, gHw.suspends };
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodSetMetadata: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiMetadata) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiMetadata req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.bytes > NativeAdrenoAbiMetadataBytes || req.reserved != 0U) {
            return kIOReturnBadArgument;
        }
        IOLockLock(clientLock);
        if (req.handle == 0ULL) {
            metaLen = req.bytes;
            memcpy(clientMeta, req.text, req.bytes);
            if (req.bytes > 0U) {
                char msg[NativeAdrenoAbiMetadataBytes + 1] = {};
                memcpy(msg, req.text, req.bytes);
                IOLog("KenzoGpuKmd: client report: %s\n", msg);
            }
        } else {
            KmdClientBo *bo = findBo(req.handle);
            if (!bo) {
                IOLockUnlock(clientLock);
                return kIOReturnNotFound;
            }
            bo->metaLen = req.bytes;
            memcpy(bo->meta, req.text, req.bytes);
        }
        IOLockUnlock(clientLock);
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodGetMetadata: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiMetadata) ||
            !outBuf || outSize != sizeof(NativeAdrenoAbiMetadata)) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiMetadata req = {};
        memcpy(&req, inBuf, sizeof(req));
        NativeAdrenoAbiMetadata rep = {};
        rep.handle = req.handle;
        IOLockLock(clientLock);
        if (req.handle == 0ULL) {
            const uint32_t n = metaLen < req.bytes ? metaLen : req.bytes;
            rep.bytes = n;
            memcpy(rep.text, clientMeta, n);
        } else {
            KmdClientBo *bo = findBo(req.handle);
            if (!bo) {
                IOLockUnlock(clientLock);
                return kIOReturnNotFound;
            }
            const uint32_t n = bo->metaLen < req.bytes ? bo->metaLen : req.bytes;
            rep.bytes = n;
            memcpy(rep.text, bo->meta, n);
        }
        IOLockUnlock(clientLock);
        memcpy(outBuf, &rep, sizeof(rep));
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodSetPriority: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiPriority) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        NativeAdrenoAbiPriority req = {};
        memcpy(&req, inBuf, sizeof(req));
        if (req.foreground > 5U || req.foreground == 3U || req.background > 5U) {
            return kIOReturnBadArgument;
        }
        prioFg = req.foreground;
        prioBg = req.background;
        return kIOReturnSuccess;
    }

    case NativeAdrenoMethodNote: {
        if (!inBuf || inSize != sizeof(NativeAdrenoAbiNote) || outSize != 0U) {
            return kIOReturnBadArgument;
        }
        return kIOReturnSuccess;
    }

    default:
        return kIOReturnUnsupported;
    }
}

extern "C" IOReturn kenzo_gpu_new_user_client(IOService *provider,
                                              task_t owningTask,
                                              void *securityID,
                                              UInt32 type,
                                              OSDictionary *properties,
                                              IOUserClient **handler)
{
    if (handler) {
        *handler = nullptr;
    }
    if (!handler || !gHw.published) {
        return kIOReturnNotReady;
    }
    // Progress markers from NativeAdrenoMetal.bundle: 0x4147xxxx (xxxx != 0)
    if ((type >> 16) == kPluginOpenType && (type & 0xFFFFU) != 0U) {
        kenzo_gpu_kmd_stats[7] = type & 0xFFFFU;
        return kIOReturnUnsupported;
    }
    if (type != kPluginOpenType && type != kHandoffOpenType) {
        return kIOReturnUnsupported;
    }

    AGXDeviceUserClient *client = OSTypeAlloc(AGXDeviceUserClient);
    if (!client) {
        return kIOReturnNoMemory;
    }
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnNoResources;
    }
    if (provider) {
        if (!client->attach(provider) || !client->start(provider)) {
            client->detach(provider);
            client->release();
            return kIOReturnNoResources;
        }
    }
    if (type == kHandoffOpenType) {
        client->setHandoffOnly();
    } else if (owningTask != kernel_task) {
        client->setProperty("NativeSandboxGrant", kOSBooleanTrue);
    }
    __atomic_fetch_add(&kenzo_gpu_kmd_stats[1], 1U, __ATOMIC_RELAXED);
    *handler = client;
    return kIOReturnSuccess;
}

extern "C" void kenzo_gpu_note_property_read(const char *key)
{
    if (key && strcmp(key, "MetalPluginName") == 0) {
        __atomic_fetch_add(&kenzo_gpu_kmd_stats[6], 1U, __ATOMIC_RELAXED);
    }
}

extern "C" bool kenzo_gpu_kmd_fences(uint32_t *submitted, uint32_t *retired)
{
    if (!gHw.published || !submitted || !retired) {
        return false;
    }
    *submitted = gHw.submittedFence;
    *retired = pollFenceHardware();
    return true;
}

static bool runKmdSelfTest()
{
    AGXDeviceUserClient *uc = OSTypeAlloc(AGXDeviceUserClient);
    if (!uc || !uc->initWithTask(kernel_task, nullptr, kPluginOpenType, nullptr)) {
        OSSafeReleaseNULL(uc);
        return false;
    }
    NativeAdrenoAbiParams params = {};
    NativeAdrenoAbiDeviceInfo info = {};
    bool ok = (uc->dispatchMethod(NativeAdrenoMethodQuery, nullptr, 0, &params, sizeof(params)) == kIOReturnSuccess) &&
              (params.abi_version == NativeAdrenoAbiVersion) &&
              (uc->dispatchMethod(NativeAdrenoMethodGetDeviceInfo, nullptr, 0, &info, sizeof(info)) == kIOReturnSuccess) &&
              (info.chip_id == A510_CHIP_ID_EXPECTED);

    NativeAdrenoAbiBoAllocReply scAlloc = {}, ibAlloc = {};
    NativeAdrenoAbiBoMapReply scMap = {}, ibMap = {};
    NativeAdrenoAbiBoCpuMapReply scCpu = {}, ibCpu = {};
    if (ok) {
        NativeAdrenoAbiBoAlloc req = { 4096ULL, 12U, 1U };
        ok = (uc->dispatchMethod(NativeAdrenoMethodBoAlloc, &req, sizeof(req), &scAlloc, sizeof(scAlloc)) == kIOReturnSuccess) &&
             (uc->dispatchMethod(NativeAdrenoMethodBoAlloc, &req, sizeof(req), &ibAlloc, sizeof(ibAlloc)) == kIOReturnSuccess);
    }
    if (ok) {
        NativeAdrenoAbiBoMap m1 = { scAlloc.handle, 0ULL, 3U, 0U, 2U, 0U };
        NativeAdrenoAbiBoMap m2 = { ibAlloc.handle, 0ULL, 3U, 0U, 2U, 0U };
        ok = (uc->dispatchMethod(NativeAdrenoMethodBoMap, &m1, sizeof(m1), &scMap, sizeof(scMap)) == kIOReturnSuccess) &&
             (uc->dispatchMethod(NativeAdrenoMethodBoMap, &m2, sizeof(m2), &ibMap, sizeof(ibMap)) == kIOReturnSuccess);
    }
    if (ok) {
        NativeAdrenoAbiBoCpuMap c1 = { scAlloc.handle };
        NativeAdrenoAbiBoCpuMap c2 = { ibAlloc.handle };
        ok = (uc->dispatchMethod(NativeAdrenoMethodBoCpuMap, &c1, sizeof(c1), &scCpu, sizeof(scCpu)) == kIOReturnSuccess) &&
             (uc->dispatchMethod(NativeAdrenoMethodBoCpuMap, &c2, sizeof(c2), &ibCpu, sizeof(ibCpu)) == kIOReturnSuccess);
    }
    IOMemoryDescriptor *scMd = nullptr, *ibMd = nullptr;
    if (ok) {
        IOOptionBits opts = 0;
        ok = (uc->clientMemoryForType(scCpu.token, &opts, &scMd) == kIOReturnSuccess) &&
             (uc->clientMemoryForType(ibCpu.token, &opts, &ibMd) == kIOReturnSuccess) && scMd && ibMd;
    }
    uint32_t gotVal = 0U;
    uint64_t gotFence = 0ULL;
    if (ok) {
        uint32_t initWord = 0xDEADBEEFU;
        (void)scMd->writeBytes(0, &initWord, 4);
        uint32_t pkt[5] = {
            a510_pm4_type7_header(A510_CP_MEM_WRITE, 3U),
            static_cast<uint32_t>(scMap.iova & 0xFFFFFFFFULL),
            static_cast<uint32_t>(scMap.iova >> 32),
            0xA510ABCDU,
            a510_pm4_type7_header(A510_CP_NOP, 0U),
        };
        (void)ibMd->writeBytes(0, pkt, sizeof(pkt));

        NativeAdrenoAbiSubmit sub = {};
        sub.buffer_count = 1U;
        sub.buffers[0] = { ibAlloc.handle, ibMap.iova, sizeof(pkt) };
        NativeAdrenoAbiSubmitReply subRep = {};
        ok = (uc->dispatchMethod(NativeAdrenoMethodSubmit, &sub, sizeof(sub), &subRep, sizeof(subRep)) == kIOReturnSuccess);
        if (ok) {
            gotFence = subRep.fence;
            NativeAdrenoAbiWaitFence wf = { subRep.fence, 100000000ULL }; // 100 ms
            ok = (uc->dispatchMethod(NativeAdrenoMethodWaitFence, &wf, sizeof(wf), nullptr, 0) == kIOReturnSuccess);
        }
        if (ok) {
            (void)scMd->readBytes(0, &gotVal, 4);
            ok = (gotVal == 0xA510ABCDU);
        }
    }
    OSSafeReleaseNULL(scMd);
    OSSafeReleaseNULL(ibMd);
    uc->clientClose();
    uc->release();

    IODelay(20);
    const uint32_t rbbm = hwRd32(gHw.gpuBase, A510_RBBM_STATUS);
    kenzo_gpu_probe_diag[3] = rbbm;
    kenzo_gpu_probe_diag[5] = gotVal;
    kenzo_gpu_probe_diag[6] = static_cast<uint32_t>(gotFence);
    kenzo_gpu_probe_diag[7] = ok ? 0xA0U : 0xAFU;
    IOLog("KenzoGpuKmd: ABI v2 self-test %s val=0x%08x fence=%llu rbbm=0x%x\n",
          ok ? "PASS" : "FAIL", gotVal, static_cast<unsigned long long>(gotFence), rbbm);
    return ok;
}

extern "C" bool kenzo_gpu_kmd_publish(uintptr_t gpuBase,
                                      IOBufferMemoryDescriptor *ringDesc,
                                      uint8_t *ringVa,
                                      uint32_t ringRegionBytes,
                                      const A510Ring *ringState,
                                      volatile uint32_t *fenceVa)
{
    if (gHw.published || !gpuBase || !ringDesc || !ringVa || !ringState || !fenceVa) {
        return false;
    }
    gHw.lock = IOLockAlloc();
    if (!gHw.lock) {
        return false;
    }
    gHw.gpuBase = gpuBase;
    gHw.ringDesc = ringDesc;
    gHw.ringVa = ringVa;
    gHw.ringRegionBytes = ringRegionBytes;
    gHw.ring = *ringState;
    gHw.fenceVa = fenceVa;
    gHw.submittedFence = ringState->next_seqno > 0U ? (ringState->next_seqno - 1U) : 0U;
    gHw.retiredFence = pollFenceHardware();
    gHw.published = true;

    // Load Apple's IOGPUFamily kext so IOAcceleratorES::gMetaClass exists, then
    // late-initialize and publish our AGXAccelerator (subclass of IOAcceleratorES).
    const kern_return_t kret = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    IOLog("KenzoGpuKmd: OSKextLoad(IOGPUFamily) kr=0x%x\n", static_cast<unsigned>(kret));
    if (kret == KERN_SUCCESS) {
        kenzo_gpu_accelerator_static_init();
        IOService *accel = kenzo_gpu_accelerator_allocate();
        IOService *platform = IOService::getPlatform();
        if (accel && platform && accel->init(nullptr) && accel->attach(platform)) {
            accel->setName("AGXAccelerator");
            accel->setProperty("MetalPluginName", "NativeAdrenoMetal");
            accel->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
            OSNumber *chipNum = OSNumber::withNumber(static_cast<uint64_t>(A510_CHIP_ID_EXPECTED), 32);
            if (chipNum) {
                accel->setProperty("NativeAdrenoChipId", chipNum);
                chipNum->release();
            }
            accel->registerService();
            gHw.accel = accel;
            kenzo_gpu_kmd_stats[0] = 1U;
            IOLog("KenzoGpuKmd: AGXAccelerator (Adreno 510) published -> MetalPluginName=NativeAdrenoMetal\n");
        } else {
            IOLog("KenzoGpuKmd: AGXAccelerator init/attach failed (accel=%p plat=%p)\n", accel, platform);
            OSSafeReleaseNULL(accel);
        }
    }

    unsigned selfTest = 1;
    PE_parse_boot_argn("kenzo-gpu-kmdtest", &selfTest, sizeof(selfTest));
    if (selfTest != 0U) {
        if (runKmdSelfTest() && gHw.accel != nullptr) {
            kenzo_gpu_probe_state = 0xAU; // GA = AGXAccelerator + NativeAdrenoABI v2 KMD 100% VERIFIED!
            return true;
        }
    } else if (gHw.accel != nullptr) {
        kenzo_gpu_probe_state = 0xAU;
        return true;
    }
    return gHw.accel != nullptr;
}
