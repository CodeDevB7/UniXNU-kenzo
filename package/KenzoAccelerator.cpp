// SPDX-License-Identifier: BSD-2-Clause-Patent
// KenzoAccelerator - Registry-visible AGXAccelerator subclassing iPadOS 17.7.11
// IOAcceleratorES (exact 136-byte base ABI from ioaccelerator-base-abi.json).
// Staged out of __mod_init_func by build-kenzo-display.py so its static
// initializer runs only after OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily").

#include "OriginalAcceleratorABI.h"
#include <IOKit/IOUserClient.h>

extern "C" {
IOReturn kenzo_gpu_new_user_client(IOService *provider,
                                   task_t owningTask,
                                   void *securityID,
                                   UInt32 type,
                                   OSDictionary *properties,
                                   IOUserClient **handler);
void kenzo_gpu_note_property_read(const char *key);
}

class AGXAccelerator : public IOAcceleratorES {
    OSDeclareDefaultStructors(AGXAccelerator)

public:
    using IOService::newUserClient;
    using IOService::copyProperty;
    IOReturn newUserClient(task_t owningTask,
                           void *securityID,
                           UInt32 type,
                           OSDictionary *properties,
                           IOUserClient **handler) override;
    OSObject *copyProperty(const OSSymbol *key) const override;
    OSObject *copyProperty(const OSString *key) const override;
    OSObject *copyProperty(const char *key) const override;
};

OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES)

IOReturn AGXAccelerator::newUserClient(task_t owningTask,
                                       void *securityID,
                                       UInt32 type,
                                       OSDictionary *properties,
                                       IOUserClient **handler)
{
    return kenzo_gpu_new_user_client(this, owningTask, securityID, type, properties, handler);
}

OSObject *AGXAccelerator::copyProperty(const OSSymbol *key) const
{
    kenzo_gpu_note_property_read(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const OSString *key) const
{
    kenzo_gpu_note_property_read(key ? key->getCStringNoCopy() : nullptr);
    return IORegistryEntry::copyProperty(key);
}

OSObject *AGXAccelerator::copyProperty(const char *key) const
{
    kenzo_gpu_note_property_read(key);
    return IORegistryEntry::copyProperty(key);
}

extern "C" IOService *kenzo_gpu_accelerator_allocate(void)
{
    return new AGXAccelerator;
}
