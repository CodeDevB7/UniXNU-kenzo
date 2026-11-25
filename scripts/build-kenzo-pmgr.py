#!/usr/bin/env python3
"""Build libShengPlatformPower.a for Kenzo (iPadOS 17.7.11 21H461 ApplePMGRNub).

Uses the exact prelink ApplePMGR addresses verified directly in Kenzo's prelinked
iPadOS 17.7.11 kernel.macho (pinned kernelcache f18b4df8..., slide -0x1000000),
without requiring pruned Sheng QEMU reference artifacts on CI runners.
"""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/sheng-platform-power"
OUT.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / "kits/XnuPrelink"))
from native_prelink import macho
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernel_target import DEPLOYMENT, TRIPLE


def run(command, **kwargs):
    return subprocess.check_output([str(v) for v in command], timeout=120, **kwargs)


def find_llvm_objcopy():
    for candidate in (
        shutil.which("llvm-objcopy"),
        "/opt/homebrew/opt/llvm/bin/llvm-objcopy",
        "/usr/local/opt/llvm/bin/llvm-objcopy",
    ):
        if candidate and Path(candidate).is_file():
            return candidate
    raise SystemExit("llvm-objcopy not found (install llvm via homebrew)")


header = """// Generated exact iPadOS 17.7.11 ApplePMGR storage/slot ABI.
#pragma once
#include <IOKit/IOPMGR.h>
class ApplePMGR : public IOPMGR {
 OSDeclareAbstractStructors(ApplePMGR)
 unsigned char originalPrivate[0x4b8a8-sizeof(IOPMGR)];
public:
"""
guards = ""
for i in range(177, 340):
    if i == 320:
        header += " virtual uint32_t readAon();\n"
    elif i == 321:
        header += " virtual void writeAon(uint64_t);\n"
    else:
        header += f" virtual void opaque{i}() = 0;\n"
        guards += f" void opaque{i}() override {{ unavailable({i}); }}\n"
header += '};\nstatic_assert(sizeof(ApplePMGR)==0x4b8a8,"Original provider storage");\n'
(OUT / "OriginalPMGRABI.h").write_text(header)
(OUT / "GuardedPMGRSlots.inc").write_text(guards)

source = ROOT / "kits/kenzo/ShengPlatformPower.cpp"
if not source.is_file():
    raise SystemExit(f"Missing {source}")
obj = OUT / "ShengPlatformPower.o"
record = json.loads(
    (ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json")
    .read_text()
    .strip()
    .rstrip(",")
)

command, i = [], 0
arguments = record["arguments"]
while i < len(arguments):
    arg = arguments[i]
    if arg == record["file"]:
        command.append(source)
    elif arg == "-o":
        command += ["-o", obj]
        i += 1
    elif arg in ("-MF", "-MJ", "-MT", "-MD", "-MP"):
        if arg in ("-MF", "-MJ", "-MT"):
            i += 1
    elif arg == "-Xclang" and arguments[i + 1].startswith("-W"):
        i += 1
    elif not arg.startswith("-W"):
        command.append(arg)
    i += 1
command += [
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
    "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
    "-Wno-unnecessary-virtual-specifier", "-I", OUT, "-I", ROOT / ".cache/xnu-work/osfmk",
]
run(command, cwd=record["directory"])

raw = obj.read_bytes()
info = macho(raw)
if info["kind"] != 1:
    raise SystemExit("Require ARM64 Mach-O object")
build_version = [struct.unpack_from("<2I", raw, at + 8) for cmd, at, _ in info["commands"] if cmd == 0x32]
if build_version != [DEPLOYMENT]:
    raise SystemExit(f"Unexpected deployment {build_version}, expected {[DEPLOYMENT]}")

sym_at = next(at for cmd, at, _ in info["commands"] if cmd == 2)
at, count, strings, _ = struct.unpack_from("<4I", raw, sym_at + 8)
symbols = []
for idx in range(count):
    ix, kind, _, _, address = struct.unpack_from("<IBBHQ", raw, at + idx * 16)
    symbols.append((raw[strings + ix:raw.index(b"\0", strings + ix)].decode(), address, kind))

# Verified fixed iPadOS 17.7.11 (21H461, slide -0x1000000) ApplePMGR prelink addresses
bindings = {
    "__ZN9ApplePMGRC2EPK11OSMetaClass": 0xFFFFFFF0049F003C,
    "__ZN9ApplePMGRD2Ev":               0xFFFFFFF0049F019C,
    "__ZN9ApplePMGR10gMetaClassE":      0xFFFFFFF006ADF490,
    "__ZN9ApplePMGR7readAonEv":         0xFFFFFFF004A20FBC,
    "__ZN9ApplePMGR8writeAonEy":        0xFFFFFFF004A20FC4,
    "_sheng_original_pmgr_nub_create":  0xFFFFFFF004A22568,
    "_sheng_original_pmgr_nub_read":    0xFFFFFFF004A24ADC,
    "_sheng_original_pmgr_nub_write":   0xFFFFFFF004A230EC,
}
undefined_pmgr = {n for n, _, k in symbols if k == 1 and "ApplePMGR" in n}
missing = undefined_pmgr - set(bindings)
if missing:
    raise SystemExit(f"Unbound ApplePMGR symbols: {sorted(missing)}")

asm = OUT / "original-bindings.S"
asm.write_text("".join(f".globl {name}\n.set {name}, {va:#x}\n" for name, va in sorted(bindings.items())))
sdk = run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
run(["xcrun", "clang", "-target", TRIPLE, "-isysroot", sdk, "-c", asm, "-o", OUT / "original-bindings.o"])

late = OUT / "ShengPlatformPower.late.o"
run([
    find_llvm_objcopy(),
    "--remove-section=__DATA,__mod_init_func",
    "--globalize-symbol=__GLOBAL__sub_I_ShengPlatformPower.cpp",
    obj, late,
])

archive = OUT / "libShengPlatformPower.a"
archive.unlink(missing_ok=True)
run(["xcrun", "ar", "rcs", archive, late, OUT / "original-bindings.o"])
print(f"KenzoPlatformPower: built {archive} ({len(bindings)} verified ApplePMGR bindings)")
