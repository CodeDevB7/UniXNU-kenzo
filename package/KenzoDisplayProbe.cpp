// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// KenzoDisplayProbe - Late registration & board glue for GopFramebuffer
// (NativeLegacyDisplay + NativeGOPPresenter) and AppleKeyStoreState
// (AppleKeyStore unlocked state) on Xiaomi Redmi Note 3 (MSM8956 / Kenzo).

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
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
volatile uint32_t kenzo_display_stage = 0;
volatile uint32_t kenzo_display_frames = 0;
volatile uint32_t kenzo_display_clients = 0;
volatile uint32_t kenzo_aks_ready = 0;
volatile uint32_t kenzo_aks_calls = 0;
volatile uint32_t kenzo_aks_opens = 0;

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
    return mapping != nullptr && width > 0 && height > 0;
}

void native_legacy_registration_observed(IOService *display)
{
    kenzo_display_stage = 4;
    IOLog("KenzoDisplayProbe: NativeLegacyDisplay registered (%p)\n", display);
}

void native_legacy_swap_stage(unsigned) {}

void native_legacy_client_enter(task_t, uint32_t) {}

void native_legacy_client_observed(IOReturn result, IOUserClient *)
{
    if (result == kIOReturnSuccess) {
        __atomic_fetch_add(&kenzo_display_clients, 1U, __ATOMIC_RELAXED);
    }
}

void native_legacy_pm_observed(unsigned, IOReturn, unsigned long) {}

void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
    __atomic_fetch_add(&kenzo_display_frames, 1U, __ATOMIC_RELAXED);
}

void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}

void native_legacy_swap_validation_observed(bool, unsigned, const void *, const void *) {}

void native_legacy_surface_rejected(uint64_t w, uint64_t h, uint32_t fmt, uint32_t planes,
                                    uint64_t stride, uint64_t alloc, uint64_t descLen)
{
    IOLog("KenzoDisplayProbe: surface rejected %llux%llu fmt=0x%x planes=%u stride=%llu alloc=%llu desc=%llu\n",
          w, h, fmt, planes, stride, alloc, descLen);
}

bool native_legacy_prepare_scanout(void)
{
    return true;
}

void native_legacy_transfer_observed(IOReturn, const void *, uint64_t) {}

void native_legacy_copy_timing_observed(uint64_t, uint64_t) {}

void native_legacy_retirement_observed(IOService *) {}

// CPU copy to GOP framebuffer (no Adreno GPU hardware composition yet on MSM8956).
bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t)
{
    return false;
}

void native_legacy_gpu_fallback_observed(int) {}

void native_legacy_vsync_observed(unsigned, unsigned) {}

// Optional Adreno/DPU/GPU-trace weak_import stubs (keep CPU GOP scanout path cleanly linked).
bool native_adreno_fences(uint32_t *, uint32_t *) { return false; }
bool native_adreno_fences_detail(uint32_t *, uint32_t *, uint32_t *) { return false; }
uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) { return 0; }
volatile uint32_t native_ufs_dpu_state = 0;
int native_gpu_trace_enabled(void) { return 0; }
void native_gpu_trace_record(uint32_t, uint32_t, const uint64_t *, uint32_t) {}

// --- AppleKeyStoreState (NativeKeyStoreEvents) board callbacks ---
bool native_aks_state_enabled(void) { return true; }
uint32_t native_aks_lock_state(void) { return 0; } // Unlocked since boot

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

static void startKeyStore(IOService *platform)
{
    if (!registerLateClass(kenzo_keyevents_static_init, "AppleKeyStore")) {
        return;
    }
    IOService *aks = native_keyevents_allocate();
    if (!aks) {
        return;
    }
    if (!aks->init(nullptr) || !aks->attach(platform)) {
        aks->release();
        return;
    }
    if (!aks->start(platform)) {
        aks->detach(platform);
        aks->release();
        return;
    }
    kenzo_aks_ready = 1;
    IOLog("KenzoDisplayProbe: AppleKeyStore ready (unlocked state)\n");
}

static void startLegacyDisplay(IOService *platform)
{
    kenzo_display_stage = 1;
    const kern_return_t kr = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    if (kr != KERN_SUCCESS) {
        IOLog("KenzoDisplayProbe: IOMobileGraphicsFamily load failed (0x%x)\n", kr);
        return;
    }
    kenzo_display_stage = 2;
    if (!registerLateClass(kenzo_legacy_display_static_init, "NativeLegacyDisplay")) {
        return;
    }
    (void)registerLateClass(kenzo_gop_presenter_static_init, "NativeGOPPresenter");

    PE_Video video = {};
    if (PE_current_console(&video) != 0 || !video.v_baseAddr || !video.v_width ||
        !video.v_height || !video.v_rowBytes) {
        IOLog("KenzoDisplayProbe: PE_current_console unavailable\n");
        return;
    }

    const uint64_t fbBytes = static_cast<uint64_t>(video.v_rowBytes) * static_cast<uint64_t>(video.v_height);
    IODeviceMemory *fbMem = IODeviceMemory::withRange(video.v_baseAddr, fbBytes);
    const OSObject *memObjs[1] = { fbMem };
    OSArray *memArray = fbMem ? OSArray::withObjects(memObjs, 1, 1) : nullptr;
    IOService *gopNub = new IOService;
    if (!fbMem || !memArray || !gopNub || !gopNub->init(nullptr)) {
        OSSafeReleaseNULL(fbMem);
        OSSafeReleaseNULL(memArray);
        OSSafeReleaseNULL(gopNub);
        return;
    }
    gopNub->setName("kenzo-gop-display");
    gopNub->setDeviceMemory(memArray);
    memArray->release();
    fbMem->release();
    if (!gopNub->attach(platform)) {
        gopNub->release();
        return;
    }
    gopNub->registerService();

    IOService *display = native_legacy_display_allocate();
    if (!display) {
        return;
    }
    if (!display->init(nullptr) || !display->attach(gopNub)) {
        display->release();
        return;
    }
    kenzo_display_stage = 3;
    if (!display->start(gopNub)) {
        IOLog("KenzoDisplayProbe: NativeLegacyDisplay::start failed\n");
        display->detach(gopNub);
        display->release();
        return;
    }
    display->registerService();
    IOLog("KenzoDisplayProbe: NativeLegacyDisplay online %lux%lu @ 0x%lx\n",
          static_cast<unsigned long>(video.v_width),
          static_cast<unsigned long>(video.v_height),
          static_cast<unsigned long>(video.v_baseAddr));
}

void native_legacy_display_late_probe(void)
{
    if (gKenzoDisplayProbed) {
        return;
    }
    gKenzoDisplayProbed = true;
    IOPlatformExpert *platform = IOService::getPlatform();
    if (!platform) {
        IOLog("KenzoDisplayProbe: no IOPlatformExpert\n");
        return;
    }
    startKeyStore(platform);
    startLegacyDisplay(platform);
}

void native_legacy_display_probe(void)
{
    native_legacy_display_late_probe();
}

} // extern "C"
