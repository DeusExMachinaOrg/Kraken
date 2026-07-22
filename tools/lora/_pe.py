"""PE file operations: loading, IAT map, section info, memory reads."""

from __future__ import annotations

import pefile


def load_pe(path: str) -> pefile.PE:
    """Load and parse a PE file."""
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories()
    return pe


def is_64bit(pe: pefile.PE) -> bool:
    return pe.FILE_HEADER.Machine == pefile.MACHINE_TYPE["IMAGE_FILE_MACHINE_AMD64"]


def build_iat_map(pe: pefile.PE, image_base: int) -> dict[int, dict]:
    """Build {RVA -> {dll, name, ordinal}} from PE import directory."""
    iat_map: dict[int, dict] = {}
    if not hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        return iat_map
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode(errors="replace")
        for imp in entry.imports:
            rva = imp.address - image_base
            iat_map[rva] = {
                "dll": dll,
                "name": imp.name.decode(errors="replace") if imp.name else None,
                "ordinal": imp.ordinal,
            }
    return iat_map


def build_exec_ranges(pe: pefile.PE) -> list[tuple[int, int]]:
    """Return list of (start_rva, end_rva) for executable sections."""
    ranges = []
    for section in pe.sections:
        if section.Characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            ranges.append((
                section.VirtualAddress,
                section.VirtualAddress + section.Misc_VirtualSize,
            ))
    return ranges


def import_at_rva(iat_map: dict[int, dict], rva: int) -> str | None:
    """Resolve IAT RVA to function name."""
    entry = iat_map.get(rva)
    if entry:
        return entry["name"] or f"ord_{entry['ordinal']}"
    return None


def read_bytes_at_rva(pe: pefile.PE, rva: int, size: int) -> bytes:
    """Read raw bytes at an RVA."""
    return pe.get_data(rva, size)


def read_cstring(pe: pefile.PE, rva: int, max_len: int = 128) -> str | None:
    """Try to read a null-terminated ASCII string at RVA."""
    try:
        data = pe.get_data(rva, max_len)
        null = data.find(b"\x00")
        if null >= 0:
            data = data[:null]
        text = data.decode("ascii", errors="strict")
        if len(text) >= 2 and all(0x20 <= ord(c) < 0x7F for c in text):
            return text
    except Exception:
        pass
    return None


def read_wide_string(pe: pefile.PE, rva: int, max_len: int = 512) -> str:
    """Read a null-terminated UTF-16LE string at RVA."""
    data = pe.get_data(rva, max_len)
    for i in range(0, len(data) - 1, 2):
        if data[i] == 0 and data[i + 1] == 0:
            data = data[:i]
            break
    return data.decode("utf-16-le", errors="replace")
