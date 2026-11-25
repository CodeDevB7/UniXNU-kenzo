#!/usr/bin/env python3
"""Build Kenzo's SdhciBlock + RamBlock archive and bind shared IOStorageFamily imports."""
import argparse
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECORD = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
STORAGE_INCLUDE = ROOT / "build/storage-abi/include"
BINDINGS = ROOT / "kits/kenzo/storage-bindings.json"
SLOTS = 190

SPECIAL = {
    "__ZTV20IOBlockStorageDevice": "vtable",
    "__ZN20IOBlockStorageDevice10gMetaClassE": "metaclass",
    "__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass": "constructor",
    "__ZN20IOBlockStorageDeviceD2Ev": "destructor",
}


def run(command, **kwargs):
    result = subprocess.run(command, **kwargs)
    if result.returncode:
        raise SystemExit(result.stdout[-3500:] if isinstance(result.stdout, str) else result.returncode)
    return result


def macho_object(data):
    magic, cpu, subtype, kind, count, size = struct.unpack_from("<6I", data)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x100000C, 0, 1):
        raise SystemExit("Unexpected platform object architecture")
    commands, offset = [], 32
    for _ in range(count):
        command, length = struct.unpack_from("<II", data, offset)
        commands.append((command, offset, length))
        offset += length
    return commands


def parse_symbols(data, commands):
    command = next(c for c in commands if c[0] == 2)
    symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", data, command[1] + 8)
    strings = data[stroff:stroff + strsize]
    symbols = []
    for index in range(nsyms):
        strx, kind, sect, desc, value = struct.unpack_from("<IBBHQ", data, symoff + index * 16)
        end = strings.find(b"\0", strx)
        name = strings[strx:end].decode() if 0 <= strx < strsize and end >= 0 else ""
        symbols.append(dict(name=name, kind=kind, sect=sect, desc=desc, value=value))
    return symbols


def parse_sections(data, commands):
    sections = []
    for command, offset, length in commands:
        if command != 0x19:
            continue
        nsects = struct.unpack_from("<I", data, offset + 64)[0]
        for index in range(nsects):
            at = offset + 72 + index * 80
            sectname, segname, address, size, file_offset = struct.unpack_from("<16s16sQQI", data, at)
            reloff, nreloc = struct.unpack_from("<II", data, at + 56)
            sections.append(dict(name=sectname.rstrip(b"\0").decode(), segment=segname.rstrip(b"\0").decode(),
                                 address=address, size=size, offset=file_offset,
                                 reloff=reloff, nreloc=nreloc))
    return sections


def vtable_bindings(data, commands, symbols, sections, vtable_name, slot_count):
    vtable_symbol = next((s for s in symbols if s["name"] == vtable_name), None)
    if vtable_symbol is None:
        raise SystemExit(f"the object has no {vtable_name} vtable")
    section = next((s for s in sections if s["address"] <= vtable_symbol["value"] <
                    s["address"] + s["size"]), None)
    if section is None:
        raise SystemExit(f"the vtable {vtable_name} is outside every section")
    relocations = {}
    for index in range(section["nreloc"]):
        address, info = struct.unpack_from("<iI", data, section["reloff"] + index * 8)
        if (info >> 27) & 1 and (info >> 28) == 0 and ((info >> 25) & 3) == 3:
            relocations[address] = symbols[info & 0xFFFFFF]["name"]
    start = vtable_symbol["value"] - section["address"] + 16
    return [relocations.get(start + index * 8, "") for index in range(slot_count)]


def compile_storage_driver(record, source, destination, extra_includes):
    command, index = [], 0
    arguments = record["arguments"]
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(destination)])
            index += 1
        elif value in ("-MF", "-MJ", "-MT", "-MD", "-MP"):
            if value in ("-MF", "-MJ", "-MT"):
                index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1
    command += [
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
        "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
        "-Wno-unnecessary-virtual-specifier", "-Wno-deprecated-declarations",
        "-I", str(ROOT / ".cache/xnu-work/osfmk"), "-I", str(STORAGE_INCLUDE),
        *extra_includes,
    ]
    result = subprocess.run(command, cwd=record["directory"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(f"{source.name} compile failed:\n" + result.stdout[-3500:])
    return command


def collect_bindings(obj_path, vtable_name, bindings, resolved):
    data = obj_path.read_bytes()
    commands = macho_object(data)
    deployment = None
    for command_id, offset, length in commands:
        if command_id == 0x32:
            deployment = struct.unpack_from("<II", data, offset + 8)
    if deployment != (2, 0x110000):
        raise SystemExit(f"{obj_path.name}: unexpected platform deployment {deployment}")

    symbols = parse_symbols(data, commands)
    sections = parse_sections(data, commands)
    slots = vtable_bindings(data, commands, symbols, sections, vtable_name, len(bindings["slots"]))
    by_name = {}
    for slot, name in enumerate(slots):
        if name:
            by_name.setdefault(name, []).append(slot)

    unresolved = []
    for symbol in symbols:
        name = symbol["name"]
        if not name or symbol["sect"] != 0 or not (symbol["kind"] & 0x01):
            continue
        if name in SPECIAL:
            addr = bindings[SPECIAL[name]]
        elif "IOBlockStorageDevice" in name:
            if name not in by_name:
                unresolved.append(name)
                continue
            if len(by_name[name]) != 1:
                raise SystemExit(f"{name} appears in several vtable slots {by_name[name]}")
            addr = bindings["slots"][by_name[name][0]]
        elif name.startswith("__ZTI"):
            unresolved.append(name)
            continue
        else:
            continue
        if name in resolved and resolved[name] != addr:
            raise SystemExit(f"Conflicting IOStorageFamily binding for {name}: {resolved[name]} vs {addr}")
        resolved[name] = addr
    if unresolved:
        raise SystemExit(f"{obj_path.name}: unbound IOStorageFamily imports: " + ", ".join(sorted(unresolved)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", nargs="?", default="SdhciBlock")
    parser.parse_args()

    out = ROOT / "build/sdhciblock"
    out.mkdir(parents=True, exist_ok=True)
    if not BINDINGS.is_file():
        raise SystemExit(f"{BINDINGS} is missing")
    bindings = json.loads(BINDINGS.read_text())
    if len(bindings["slots"]) != SLOTS:
        raise SystemExit(f"the binding table has {len(bindings['slots'])} slots, expected {SLOTS}")

    record = json.loads(RECORD.read_text().strip().rstrip(","))
    sdhci_src = ROOT / "kits/kenzo/NativeSdhciBlock.cpp"
    ram_src = ROOT / "drivers/RamBlock/NativeRamBlock.cpp"
    if not sdhci_src.is_file():
        raise SystemExit(f"missing {sdhci_src}")
    if not ram_src.is_file():
        raise SystemExit(f"missing {ram_src}")

    sdhci_obj = out / "NativeSdhciBlock.o"
    ram_obj = out / "NativeRamBlock.o"

    command = compile_storage_driver(record, sdhci_src, sdhci_obj, [])
    compile_storage_driver(record, ram_src, ram_obj, ["-I", str(ROOT / "drivers/RamBlock")])

    resolved = {}
    collect_bindings(sdhci_obj, "__ZTV16NativeSdhciBlock", bindings, resolved)
    collect_bindings(ram_obj, "__ZTV14NativeRamBlock", bindings, resolved)

    assembly = out / "original-bindings.S"
    assembly.write_text("".join(f".globl {name}\n.set {name}, {address}\n"
                                for name, address in sorted(resolved.items())))
    bindings_object = out / "original-bindings.o"
    assembler, index = [], 0
    while index < len(command):
        value = command[index]
        if value in ("-x", "-include"):
            index += 2
            continue
        if value.startswith(("-x", "-include")):
            index += 1
            continue
        if value in ("-o", str(sdhci_src), str(sdhci_obj)) or value.startswith("-std="):
            index += 1
            continue
        assembler.append(value)
        index += 1
    assembler = [value for value in assembler if value != "-c"]
    run([*assembler, "-x", "assembler-with-cpp", "-c", str(assembly), "-o", str(bindings_object)],
        cwd=record["directory"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    archive = out / "libSdhciBlock.a"
    archive.unlink(missing_ok=True)
    subprocess.run(["xcrun", "ar", "rcs", str(archive), str(sdhci_obj), str(ram_obj), str(bindings_object)], check=True)
    print(f"SdhciBlock+RamBlock: {len(resolved)} shared IOStorageFamily imports bound -> {archive}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
