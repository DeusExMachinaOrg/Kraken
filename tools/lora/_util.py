"""Small utilities shared across lora modules."""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import re


def parse_template_args(full_name: str, base_name: str) -> list[str]:
    """Extract template arguments from 'base<A, B, C>' given the base name."""
    idx = full_name.find("<", len(base_name))
    if idx < 0:
        return []
    depth = 0
    args: list[str] = []
    current = ""
    for ch in full_name[idx + 1:]:
        if ch == "<":
            depth += 1
            current += ch
        elif ch == ">":
            if depth == 0:
                if current.strip():
                    args.append(current.strip())
                break
            depth -= 1
            current += ch
        elif ch == "," and depth == 0:
            if current.strip():
                args.append(current.strip())
            current = ""
        else:
            current += ch
    return args


def parse_rva(v: int | str) -> int:
    """Accept RVA as int or hex string ('0x58fdc8')."""
    if isinstance(v, str):
        return int(v, 16) if v.startswith(("0x", "0X")) else int(v)
    return v


_dbghelp = None


def _get_dbghelp():
    global _dbghelp
    if _dbghelp is None:
        _dbghelp = ctypes.windll.dbghelp
    return _dbghelp


def demangle_name(decorated: str) -> str | None:
    """Demangle a single MSVC decorated name. Returns None on failure."""
    dbghelp = _get_dbghelp()
    buf = ctypes.create_string_buffer(2048)
    ret = dbghelp.UnDecorateSymbolName(
        decorated.encode("ascii", errors="replace"),
        buf, ctypes.sizeof(buf), 0,
    )
    if ret:
        return buf.value.decode("ascii", errors="replace")
    return None
