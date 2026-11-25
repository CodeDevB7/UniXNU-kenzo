#!/usr/bin/env python3
"""Build libNativeLegacyDisplay.late.a for Kenzo (iPadOS 17.7.11 21H461).

Uses the verified 358-slot IOMobileFramebufferLegacy prelink table committed in
kits/kenzo/legacy-display-bindings.json (extracted from Kenzo's prelinked
iPadOS 17.7.11 kernel.macho), with zero dependency on pruned Sheng artifacts.
"""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/native-legacy-display"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / "kits/XnuPrelink"))
from native_prelink import macho  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernel_target import ARCH, IOS_VERSION, TRIPLE  # noqa: E402

if ARCH != "arm64" or IOS_VERSION.split(".")[0] != "17":
    raise SystemExit("build-kenzo-display.py requires arm64 iOS/iPadOS 17")


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=120, **kw)


def require(cond, why):
    if not cond:
        raise SystemExit(why)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def find_llvm_objcopy():
    for candidate in (
        shutil.which("llvm-objcopy"),
        "/opt/homebrew/opt/llvm/bin/llvm-objcopy",
        "/usr/local/opt/llvm/bin/llvm-objcopy",
    ):
        if candidate and Path(candidate).is_file():
            return candidate
    raise SystemExit("llvm-objcopy not found (install llvm via homebrew)")


def text_section(data, info):
    sec = next(s for g in info["segments"] for s in g["sections"] if s["name"] == "__text")
    return data[sec["fileoff"]:sec["fileoff"] + sec["size"]]


def main():
    table_path = ROOT / "kits/kenzo/legacy-display-bindings.json"
    require(table_path.is_file(), f"Missing {table_path}")
    table = json.loads(table_path.read_text())
    pure = table["abstract_slots"]
    slots = [int(v, 16) for v in table["slots"]]
    require(len(slots) == 358, f"Expected 358 vtable slots, got {len(slots)}")
    legacy_vptr = int(table["legacy_vptr"], 16)
    inherited_symbols = table.get("inherited_symbols", {})

    # 1. Generate OriginalLegacyFramebufferABI.h (exact iPadOS 17.7.11 0x18a0 ABI)
    header = """// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];};
static constexpr unsigned kLegacyVsyncClients=5592;
static constexpr unsigned kRequestRect0=17,kRequestRect1=81,kRequestMasks=792,kRequestOptions=804,kRequestDims=816,kRequestSurfaces=2624;
#define NATIVE_LEGACY_LEAVES(LEAF) LEAF(211) LEAF(245) LEAF(246) LEAF(306) LEAF(307) LEAF(338) LEAF(345)
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
"""
    for i in range(168, 358):
        if i == 184:
            header += " virtual bool isMainDisplay();\n"
        elif i == 188:
            header += " virtual IOReturn getDotPitch(uint32_t *);\n"
        elif i == 213:
            header += " virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n"
        elif i == 249:
            header += " virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n"
        elif i == 290:
            header += " virtual void kickSwapQueue();\n"
        elif i == 296:
            header += " virtual bool validateSwap(const OriginalLegacySwapRequest *);\n"
        elif i == 295:
            header += " virtual void completeCurrentSwap();\n"
        elif i == 337:
            header += " virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n"
        elif i == 288:
            header += " virtual bool startHardware();\n"
        elif i == 354:
            header += " virtual int applyStateRequest(OriginalLegacyStateRequest *,bool);\n"
        elif i == 301:
            header += " virtual bool getRefreshAttributes(uint64_t *,bool);\n"
        elif i in pure:
            header += f" virtual __attribute__((noreturn)) void opaque{i}()=0;\n"
        else:
            header += f' virtual __attribute__((deprecated("Unclassified ABI"))) void opaque{i}();\n'
    header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
    (OUT / "OriginalLegacyFramebufferABI.h").write_text(header)

    # 2. Build NativeLegacyDisplay.o and bind exact 358-slot vtable
    print(run(["python3", ROOT / "scripts/build-arm-driver.py", "NativeLegacyDisplay"], text=True), end="")
    obj = OUT / "NativeLegacyDisplay.o"
    raw = obj.read_bytes()
    image = macho(raw)
    st = next(p for c, p, _ in image["commands"] if c == 2)
    sy, count, strings, _ = struct.unpack_from("<4I", raw, st + 8)
    symbols = []
    for i in range(count):
        ix, kind, section, _, va = struct.unpack_from("<IBBHQ", raw, sy + i * 16)
        symbols.append((raw[strings + ix:raw.index(b"\0", strings + ix)].decode(), va, kind))

    vtable = next(va for name, va, _ in symbols if name == "__ZTV19NativeLegacyDisplay")
    relocations = {}
    for segment in image["segments"]:
        for sec in segment["sections"]:
            if sec["va"] <= vtable < sec["va"] + sec["size"]:
                ro, rc = struct.unpack_from("<2I", raw, sec["command_offset"] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from("<2I", raw, ro + i * 8)
                    if (bits >> 27) & 1 and (bits >> 28) == 0 and ((bits >> 25) & 3) == 3:
                        relocations[sec["va"] + offset] = symbols[bits & 0xFFFFFF][0]

    compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]
    require(all(compiled) and (vtable + 16 + 358 * 8) not in relocations, "Exact owned 358-entry vtable")

    # Verified iPadOS 17.7.11 (21H461, slide -0x1000000) prelink addresses
    bindings = {
        "__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass": 0xFFFFFFF0057865CC,
        "__ZN25IOMobileFramebufferLegacyD2Ev": 0xFFFFFFF005786618,
        "__ZN25IOMobileFramebufferLegacy10gMetaClassE": 0xFFFFFFF006B35940,
        "__ZTV25IOMobileFramebufferLegacy": legacy_vptr - 16,
        "_native_legacy_surface_descriptor": 0xFFFFFFF004B9EAD0,
        "_native_legacy_surface_width": 0xFFFFFFF004B9EBBC,
        "_native_legacy_surface_height": 0xFFFFFFF004B9EBC4,
        "_native_legacy_surface_stride": 0xFFFFFFF004B9EBD4,
        "_native_legacy_surface_allocation": 0xFFFFFFF004B9EBFC,
        "_native_legacy_surface_format": 0xFFFFFFF004B9EBAC,
        "_native_legacy_surface_planes": 0xFFFFFFF004B9EBF4,
        "_native_legacy_vsync_notify": 0xFFFFFFF005796720,
        "_native_legacy_set_refresh_properties": 0xFFFFFFF00579EBB8,
    }
    owned = []
    native = []
    for i, name in enumerate(compiled):
        if name.startswith(("__ZN19NativeLegacyDisplay", "__ZNK19NativeLegacyDisplay")):
            owned.append(i)
        elif "25IOMobileFramebufferLegacy" in name:
            require(i not in pure, f"Unresolved abstract slot {i}")
            bindings[name] = slots[i]
        else:
            require(i < 168, f"Unexpected non-legacy symbol {name} at slot {i}")
            if str(i) in inherited_symbols and inherited_symbols[str(i)]:
                require(name in inherited_symbols[str(i)], f"Inherited slot {i} mismatch: {name}")
            native.append(i)

    undefined = {n for n, _, k in symbols if k == 1 and "IOMobileFramebufferLegacy" in n}
    missing = undefined - set(bindings)
    require(not missing, f"Unbound IOMobileFramebufferLegacy symbols: {sorted(missing)}")

    asm = OUT / "original-bindings.S"
    asm.write_text("".join(f".globl {name}\n.set {name}, {va:#x}\n" for name, va in sorted(bindings.items())))
    sdk = run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
    run(["xcrun", "clang", "-target", TRIPLE, "-isysroot", sdk, "-c", asm, "-o", OUT / "original-bindings.o"])

    objcopy = find_llvm_objcopy()
    late = OUT / "NativeLegacyDisplay.late.o"
    run([
        objcopy,
        "--remove-section=__DATA,__mod_init_func",
        "--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp",
        obj, late,
    ])
    require(text_section(raw, image) == text_section(late.read_bytes(), macho(late.read_bytes())),
            "NativeLegacyDisplay __text unchanged by late-init staging")

    # 3. Build NativeKeyStoreEvents & NativeGOPPresenter (late-staged)
    print(run(["python3", ROOT / "scripts/build-arm-driver.py", "NativeKeyStoreEvents"], text=True), end="")
    keyevents = ROOT / "build/apple-key-store-state"
    keyevents_late = keyevents / "NativeKeyStoreEvents.late.o"
    run([
        objcopy,
        "--remove-section=__DATA,__mod_init_func",
        "--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp",
        keyevents / "NativeKeyStoreEvents.o", keyevents_late,
    ])

    print(run(["python3", ROOT / "scripts/build-arm-driver.py", "NativeGOPPresenter"], text=True), end="")
    gop = ROOT / "build/native-gop-presenter"
    gop_late = gop / "NativeGOPPresenter.late.o"
    run([
        objcopy,
        "--remove-section=__DATA,__mod_init_func",
        "--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp",
        gop / "NativeGOPPresenter.o", gop_late,
    ])

    # 4. Compile kits/kenzo/KenzoDisplayProbe.cpp
    probe_src = ROOT / "kits/kenzo/KenzoDisplayProbe.cpp"
    require(probe_src.is_file(), f"Missing {probe_src}")
    probe_obj = OUT / "KenzoDisplayProbe.o"
    rec = json.loads(
        (ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json")
        .read_text().strip().rstrip(",")
    )
    cmd, i = [], 0
    arguments = rec["arguments"]
    while i < len(arguments):
        val = arguments[i]
        if val == rec["file"]:
            cmd.append(str(probe_src))
        elif val == "-o":
            cmd += ["-o", str(probe_obj)]
            i += 1
        elif val in ("-MF", "-MJ", "-MT", "-MD", "-MP"):
            if val in ("-MF", "-MJ", "-MT"):
                i += 1
        elif val == "-Xclang" and arguments[i + 1].startswith("-W"):
            i += 1
        elif not val.startswith("-W"):
            cmd.append(val)
        i += 1
    cmd += [
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-Wno-c99-designator", "-Wno-reorder-init-list",
        "-Wno-deprecated-enum-enum-conversion", "-Wno-unnecessary-virtual-specifier",
        "-I", str(ROOT / ".cache/xnu-work/osfmk"),
    ]
    run(cmd, cwd=rec["directory"])

    # 5. Archive libNativeLegacyDisplay.late.a and emit build-report.json
    archive = OUT / "libNativeLegacyDisplay.late.a"
    archive.unlink(missing_ok=True)
    run([
        "xcrun", "ar", "rcs", archive,
        late, OUT / "original-bindings.o", probe_obj, keyevents_late, gop_late,
    ])

    sources = [
        ROOT / "drivers/GopFramebuffer/NativeLegacyDisplay.cpp",
        ROOT / "drivers/GopFramebuffer/NativeLegacyRequest.h",
        ROOT / "drivers/GopFramebuffer/NativeGOPPresenter.cpp",
        ROOT / "drivers/GopFramebuffer/NativeGOPWire.h",
        ROOT / "drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp",
        probe_src,
        table_path,
    ]
    report = {
        "source_sha256": {str(p.relative_to(ROOT)): sha(p) for p in sources},
        "generated_abi_sha256": sha(OUT / "OriginalLegacyFramebufferABI.h"),
        "compiled_object_sha256": sha(obj),
        "late_object_sha256": sha(late),
        "archive_sha256": sha(archive),
        "bindings": {n: hex(v) for n, v in bindings.items()},
        "virtual_entries": 358,
        "owned_entries": owned,
        "inherited_native_entries": native,
    }
    (OUT / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"KenzoDisplay: built {archive} ({len(bindings)} bindings, 358 slots)")


if __name__ == "__main__":
    main()
