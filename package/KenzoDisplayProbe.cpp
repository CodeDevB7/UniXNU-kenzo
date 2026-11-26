// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// KenzoDisplayProbe - Late registration & board glue for GopFramebuffer
// (NativeLegacyDisplay + NativeGOPPresenter), IOSurface, and AppleKeyStoreState
// (AppleKeyStore unlocked/no-passcode state) on Xiaomi Redmi Note 3 (MSM8956 / Kenzo).

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);

void kenzo_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
void kenzo_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
void kenzo_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");

IOService *native_legacy_display_allocate(void);
IOService *native_keyevents_allocate(void);

// Observable telemetry for Kenzo Stage 3 bring-up.
// kenzo_display_stage milestones (single hex digit on top bar):
//   1 = enter startLegacyDisplay
//   2 = IOMobileGraphicsFamily loaded & NativeLegacyDisplay registered
//   3 = IOSurface checked
//   4 = PE_current_console validated
//   5 = NativeGOPFramebuffer nub attached & started
//   6 = /arm-io/gop-display device_type=display-subsystem attached in gIODTPlane
//   7 = NativeLegacyDisplay init/probe/attach succeeded
//   8 = calling NativeLegacyDisplay::start()
//   A = native_legacy_registration_observed() called
//   B = NativeLegacyDisplay::start() returned true (100% online!)
volatile uint32_t kenzo_display_stage = 0;
volatile uint32_t kenzo_display_frames = 0;
volatile uint32_t kenzo_display_clients = 0;
volatile uint32_t kenzo_aks_ready = 0;
volatile uint32_t kenzo_aks_calls = 0;
volatile uint32_t kenzo_aks_opens = 0;

static bool publishChosenWord(IORegistryEntry *entry, const char *key, uint32_t value)
{
    if (!entry || !key) {
        return false;
    }
    OSObject *existing = entry->getProperty(key);
    if (existing) {
        OSData *data = OSDynamicCast(OSData, existing);
        uint32_t current = 0;
        if (!data || data->getLength() != sizeof(current)) {
            return false;
        }
        memcpy(&current, data->getBytesNoCopy(), sizeof(current));
        return current == value;
    }
    OSData *data = OSData::withBytes(&value, sizeof(value));
    const bool ok = data && entry->setProperty(key, data);
    OSSafeReleaseNULL(data);
    return ok;
}

// --- NativeLegacyDisplay board callbacks ---
__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *self, void *caller)
{
    panic("KenzoDisplayProbe: unimplemented IOMobileFramebufferLegacy slot %u (self=%p caller=%p)",
          slot, self, caller);
}

bool native_legacy_gop_mapping_observed(IOService *display, IOMemoryMap *mapping,
                                        uint32_t width, uint32_t height)
{
    IOLog("KenzoDisplayProbe: GOP framebuffer mapped phys=0x%llx len=0x%llx %ux%u (display=%p)\n",
          mapping ? static_cast<uint64_t>(mapping->getPhysicalAddress()) : 0ULL,
          mapping ? static_cast<uint64_t>(mapping->getLength()) : 0ULL,
          width, height, display);
    if (!mapping || width == 0 || height == 0) {
        return false;
    }
    unsigned scale = 2;
    PE_parse_boot_argn("offline-legacy-unit-scale", &scale, sizeof(scale));
    if (scale >= 1 && scale <= 3) {
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
        if (chosen) {
            const bool sOk = publishChosenWord(chosen, "display-scale", scale);
            const bool rOk = publishChosenWord(chosen, "display-rotation", 0U);
            IOLog("KenzoDisplayProbe: /chosen display-scale=%u (%u) display-rotation=0 (%u)\n",
                  scale, static_cast<unsigned>(sOk), static_cast<unsigned>(rOk));
            chosen->release();
        }
    }
    return true;
}

void native_legacy_registration_observed(IOService *display)
{
    kenzo_display_stage = 0xA;
    IOLog("KenzoDisplayProbe: NativeLegacyDisplay registered (%p)\n", display);
}

void native_legacy_swap_stage(unsigned) {}

void native_legacy_client_enter(task_t, uint32_t) {}

void native_legacy_client_observed(IOReturn result, IOUserClient *client)
{
    if (result == kIOReturnSuccess) {
        const uint32_t n = __atomic_add_fetch(&kenzo_display_clients, 1U, __ATOMIC_RELAXED);
        IOLog("KenzoDisplayProbe: display client #%u opened (%s)\n",
              n, client ? client->getMetaClass()->getClassName() : "null");
    } else {
        IOLog("KenzoDisplayProbe: display client open failed status=0x%x\n",
              static_cast<unsigned>(result));
    }
}

void native_legacy_pm_observed(unsigned, IOReturn, unsigned long) {}

void native_legacy_frame_observed(unsigned kind, uint64_t count,
                                  uint64_t minx, uint64_t miny, uint64_t maxx, uint64_t maxy)
{
    const uint32_t n = __atomic_add_fetch(&kenzo_display_frames, 1U, __ATOMIC_RELAXED);
    if (n <= 8U) {
        IOLog("KenzoDisplayProbe: frame #%u kind=%u lit=%llu (%llu,%llu)-(%llu,%llu)\n",
              n, kind, static_cast<unsigned long long>(count),
              static_cast<unsigned long long>(minx), static_cast<unsigned long long>(miny),
              static_cast<unsigned long long>(maxx), static_cast<unsigned long long>(maxy));
    }
}

void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}

void native_legacy_swap_validation_observed(bool accepted, unsigned reason,
                                            const void *request, const void *surface)
{
    static volatile uint32_t logged = 0;
    if (!accepted && __atomic_add_fetch(&logged, 1U, __ATOMIC_RELAXED) <= 5U && request) {
        const uint8_t *b = static_cast<const uint8_t *>(request);
        uint32_t r0[4] = {}, r1[4] = {}, dims[2] = {};
        memcpy(r0, b + 17, sizeof(r0));
        memcpy(r1, b + 81, sizeof(r1));
        memcpy(dims, b + 816, sizeof(dims));
        IOLog("KenzoDisplayProbe: swap fallback reason=%u surf=%p r0=%u,%u,%u,%u r1=%u,%u,%u,%u dims=%ux%u\n",
              reason, surface, r0[0], r0[1], r0[2], r0[3], r1[0], r1[1], r1[2], r1[3], dims[0], dims[1]);
    }
}

void native_legacy_surface_rejected(uint64_t w, uint64_t h, uint32_t fmt, uint32_t planes,
                                    uint64_t stride, uint64_t alloc, uint64_t descLen)
{
    static volatile uint32_t rejLogged = 0;
    if (__atomic_add_fetch(&rejLogged, 1U, __ATOMIC_RELAXED) <= 5U) {
        IOLog("KenzoDisplayProbe: surface info %llux%llu fmt=0x%x planes=%u stride=%llu alloc=%llu desc=%llu\n",
              w, h, fmt, planes, stride, alloc, descLen);
    }
}

bool native_legacy_prepare_scanout(void)
{
    return true;
}

void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes)
{
    if (status == kIOReturnSuccess) {
        const uint32_t n = __atomic_add_fetch(&kenzo_display_frames, 1U, __ATOMIC_RELAXED);
        if (n <= 4U) {
            IOLog("KenzoDisplayProbe: transfer #%u bytes=%llu\n", n, static_cast<unsigned long long>(bytes));
        }
    }
}

void native_legacy_copy_timing_observed(uint64_t, uint64_t) {}

void native_legacy_retirement_observed(IOService *) {}

// CPU copy / portrait scaler to GOP framebuffer (no Adreno GPU hardware composition yet on MSM8956).
bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t)
{
    return false;
}

void native_legacy_gpu_fallback_observed(int) {}

void native_legacy_vsync_observed(unsigned, unsigned) {}

// Optional Adreno/DPU/GPU-trace/boot-progress weak_import stubs (keep CPU GOP scanout path cleanly linked).
volatile unsigned boot_progress_panel_top = 0;
bool native_adreno_fences(uint32_t *, uint32_t *) { return false; }
bool native_adreno_fences_detail(uint32_t *, uint32_t *, uint32_t *) { return false; }
uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) { return 0; }
volatile uint32_t native_ufs_dpu_state = 0;
int native_gpu_trace_enabled(void) { return 0; }
void native_gpu_trace_record(uint32_t, uint32_t, const uint64_t *, uint32_t) {}

// --- AppleKeyStoreState (NativeKeyStoreEvents) board callbacks ---
bool native_aks_state_enabled(void) { return true; }

uint32_t native_aks_lock_state(void)
{
    static int cached = -1;
    if (cached < 0) {
        unsigned value = 3; // MobileKeyBag "no passcode" (unlocked since boot)
        if (PE_parse_boot_argn("aks-sls", &value, sizeof(value)) && value > 7) {
            value = 3;
        }
        cached = static_cast<int>(value);
    }
    return static_cast<uint32_t>(cached);
}

size_t native_aks_device_id(uint8_t *out, size_t capacity)
{
    static const uint8_t kKenzoId[16] = {
        0x4B, 0x45, 0x4E, 0x5A, 0x4F, 0x38, 0x39, 0x35,
        0x36, 0x55, 0x4E, 0x49, 0x58, 0x4E, 0x55, 0x01
    };
    if (!out || capacity < sizeof(kKenzoId)) {
        return 0;
    }
    memcpy(out, kKenzoId, sizeof(kKenzoId));
    return sizeof(kKenzoId);
}

void native_aks_opened(void)
{
    __atomic_fetch_add(&kenzo_aks_opens, 1U, __ATOMIC_RELAXED);
}

void native_aks_call(uint32_t, int32_t)
{
    __atomic_fetch_add(&kenzo_aks_calls, 1U, __ATOMIC_RELAXED);
}

void native_keyevents_open_refused(void) {}

static bool gKenzoDisplayProbed = false;

static bool registerLateClass(void (*initFn)(void), const char *label)
{
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        IOLog("KenzoDisplayProbe: preModLoad failed for %s\n", label);
        return false;
    }
    initFn();
    const kern_return_t kr = OSMetaClass::postModLoad(handle);
    if (kr != KERN_SUCCESS) {
        IOLog("KenzoDisplayProbe: postModLoad failed for %s (0x%x)\n", label, kr);
        return false;
    }
    return true;
}

static void startKeyStore(void)
{
    IOService *resources = IOService::getResourceService();
    if (!resources) {
        IOLog("KenzoDisplayProbe: no IOResources for AppleKeyStore\n");
        return;
    }
    if (!registerLateClass(kenzo_keyevents_static_init, "AppleKeyStore")) {
        return;
    }
    IOService *aks = native_keyevents_allocate();
    if (!aks) {
        return;
    }
    if (!aks->init(nullptr) || !aks->attach(resources)) {
        aks->release();
        return;
    }
    if (!aks->start(resources)) {
        aks->detach(resources);
        aks->release();
        return;
    }
    kenzo_aks_ready = 1;
    IOLog("KenzoDisplayProbe: AppleKeyStore ready (sls=%u)\n", native_aks_lock_state());
}

static void startLegacyDisplay(void)
{
    kenzo_display_stage = 1;

    // 1. Load IOMobileGraphicsFamily and register NativeLegacyDisplay / NativeGOPPresenter.
    const kern_return_t gfxKr = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    if (gfxKr != KERN_SUCCESS) {
        IOLog("KenzoDisplayProbe: IOMobileGraphicsFamily load failed (0x%x)\n", gfxKr);
        return;
    }
    kenzo_display_stage = 2;
    if (!registerLateClass(kenzo_legacy_display_static_init, "NativeLegacyDisplay")) {
        return;
    }
    (void)registerLateClass(kenzo_gop_presenter_static_init, "NativeGOPPresenter");

    // 2. Load IOSurface and probe for IOCoreSurfaceRoot (non-fatal if still starting asynchronously,
    //    since IOMobileFramebufferLegacy::start() itself calls waitForService("IOCoreSurfaceRoot")).
    const kern_return_t surfKr = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    bool surfaceFound = false;
    if (surfKr == KERN_SUCCESS) {
        OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
        IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
        surfaceFound = (surface != nullptr);
        OSSafeReleaseNULL(surface);
        OSSafeReleaseNULL(matching);
    }
    IOLog("KenzoDisplayProbe: IOSurface load=0x%x root_found=%u\n",
          static_cast<unsigned>(surfKr), static_cast<unsigned>(surfaceFound));
    kenzo_display_stage = 3;

    // 3. Query UEFI GOP console geometry.
    PE_Video video = {};
    if (PE_current_console(&video) != 0 || !video.v_baseAddr || !video.v_width ||
        !video.v_height || !video.v_rowBytes) {
        IOLog("KenzoDisplayProbe: PE_current_console unavailable (base=0x%lx %lux%lu depth=%lu row=%lu disp=%lu)\n",
              static_cast<unsigned long>(video.v_baseAddr), static_cast<unsigned long>(video.v_width),
              static_cast<unsigned long>(video.v_height), static_cast<unsigned long>(video.v_depth),
              static_cast<unsigned long>(video.v_rowBytes), static_cast<unsigned long>(video.v_display));
        return;
    }
    kenzo_display_stage = 4;

    // 4. Create NativeGOPFramebuffer nub under IOResources (fallback to IOPlatformExpert)
    //    AND attach to IODeviceTree:/arm-io/gop-display with device_type="display-subsystem"
    //    so IOMobileFramebuffer / backboardd / SpringBoard discover the display!
    IOService *parent = IOService::getResourceService();
    if (!parent) {
        parent = IOService::getPlatform();
    }
    const uint64_t fbBytes = static_cast<uint64_t>(video.v_rowBytes) * static_cast<uint64_t>(video.v_height);
    IODeviceMemory *fbMem = IODeviceMemory::withRange(video.v_baseAddr, fbBytes);
    OSArray *memArray = fbMem ? OSArray::withCapacity(1) : nullptr;
    IOService *gopNub = new IOService;
    if (!parent || !fbMem || !memArray || !gopNub || !gopNub->init(nullptr)) {
        OSSafeReleaseNULL(fbMem);
        OSSafeReleaseNULL(memArray);
        OSSafeReleaseNULL(gopNub);
        return;
    }
    memArray->setObject(fbMem);
    gopNub->setDeviceMemory(memArray);
    gopNub->setName("NativeGOPFramebuffer");
    memArray->release();
    fbMem->release();

    bool nubStarted = gopNub->attach(parent) && gopNub->start(parent);
    if (!nubStarted) {
        gopNub->detach(parent);
        IOPlatformExpert *plat = IOService::getPlatform();
        if (plat && plat != parent) {
            parent = plat;
            nubStarted = gopNub->attach(parent) && gopNub->start(parent);
        }
    }
    if (!nubStarted) {
        IOLog("KenzoDisplayProbe: gopNub attach/start failed\n");
        gopNub->release();
        return;
    }
    kenzo_display_stage = 5;

    IORegistryEntry *armIo = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    static const char kDisplayType[] = "display-subsystem";
    OSData *typeData = OSData::withBytes(kDisplayType, sizeof(kDisplayType));
    bool dtAttached = false;
    if (armIo && typeData && gopNub->setProperty("device_type", typeData)) {
        gopNub->setName("gop-display", gIODTPlane);
        dtAttached = gopNub->attachToParent(armIo, gIODTPlane);
    }
    OSSafeReleaseNULL(typeData);
    OSSafeReleaseNULL(armIo);
    IOLog("KenzoDisplayProbe: /arm-io/gop-display device_type=display-subsystem dt_attached=%u\n",
          static_cast<unsigned>(dtAttached));
    if (dtAttached) {
        kenzo_display_stage = 6;
    }

    // 5. Probe, attach, and start NativeLegacyDisplay (phase 4 registers itself inside start()).
    IOService *display = native_legacy_display_allocate();
    if (!display || !display->init(nullptr)) {
        OSSafeReleaseNULL(display);
        return;
    }
    SInt32 score = 0;
    const bool probeOk = (display->probe(gopNub, &score) == display);
    if (!probeOk || !display->attach(gopNub)) {
        IOLog("KenzoDisplayProbe: NativeLegacyDisplay probe/attach failed (probe=%u)\n",
              static_cast<unsigned>(probeOk));
        display->release();
        return;
    }
    kenzo_display_stage = 7;
    kenzo_display_stage = 8;
    if (!display->start(gopNub)) {
        IOLog("KenzoDisplayProbe: NativeLegacyDisplay::start failed\n");
        display->detach(gopNub);
        display->release();
        return;
    }
    kenzo_display_stage = 0xB;
    gopNub->release();
    IOLog("KenzoDisplayProbe: NativeLegacyDisplay online %lux%lu @ 0x%lx (dt=%u surf=%u)\n",
          static_cast<unsigned long>(video.v_width),
          static_cast<unsigned long>(video.v_height),
          static_cast<unsigned long>(video.v_baseAddr),
          static_cast<unsigned>(dtAttached),
          static_cast<unsigned>(surfaceFound));
}

void native_legacy_display_late_probe(void)
{
    if (gKenzoDisplayProbed) {
        return;
    }
    gKenzoDisplayProbed = true;
    startKeyStore();
    startLegacyDisplay();
}

void native_legacy_display_probe(void)
{
    // Early bsd_init() hook runs before IOKitBSDInit() publishes IOBSD;
    // IOSurfaceRoot requires IOResourceMatch="IOBSD", so defer all setup
    // to native_legacy_display_late_probe() in bsd_autoconf().
}

} // extern "C"
