"""Capstone disassembly, operand annotation, calling convention detection."""

from __future__ import annotations

import re
from typing import Any

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64

from ._state import Session
from . import _pe as pe_mod
from . import _pdb as pdb_mod


def disassemble_raw(pe_obj, rva: int, size: int, is_64: bool,
                    image_base: int, max_insns: int = 200) -> list[dict]:
    """Raw disassembly without PDB annotation. Returns list of instruction dicts."""
    code = pe_obj.get_data(rva, size)
    mode = CS_MODE_64 if is_64 else CS_MODE_32
    md = Cs(CS_ARCH_X86, mode)
    md.detail = True

    results = []
    for insn in md.disasm(code, image_base + rva):
        results.append({
            "address": hex(insn.address),
            "rva": hex(insn.address - image_base),
            "mnemonic": insn.mnemonic,
            "op_str": insn.op_str,
            "bytes": insn.bytes.hex(),
            "size": insn.size,
        })
        if len(results) >= max_insns:
            break
    return results


def annotate_operand(op_str: str, mnemonic: str, sess: Session) -> str:
    """Add inline annotations (symbol names, strings, imports) to an operand."""
    if mnemonic == "call":
        m = re.match(r"^0x([0-9a-fA-F]+)$", op_str)
        if m:
            va = int(m.group(1), 16)
            rva = va - sess.image_base
            name = pdb_mod.sym_at_rva(sess, rva)
            if name:
                return f"{op_str}  ; {name}"
        m = re.search(r"\[0x([0-9a-fA-F]+)\]", op_str)
        if m:
            va = int(m.group(1), 16)
            name = pe_mod.import_at_rva(sess.iat_map, va - sess.image_base)
            if name:
                return f"{op_str}  ; {name}"

    if mnemonic in ("push", "mov"):
        m = re.search(r"0x([0-9a-fA-F]+)", op_str)
        if m:
            va = int(m.group(1), 16)
            rva = va - sess.image_base
            if rva > 0:
                s = pe_mod.read_cstring(sess.pe, rva)
                if s:
                    preview = s[:60] + ("..." if len(s) > 60 else "")
                    return f'{op_str}  ; "{preview}"'

    if mnemonic == "jmp":
        m = re.match(r"^0x([0-9a-fA-F]+)$", op_str)
        if m:
            va = int(m.group(1), 16)
            rva = va - sess.image_base
            name = pdb_mod.sym_at_rva(sess, rva)
            if name:
                return f"{op_str}  ; {name}"

    return op_str


def resolve_call_target(op_str: str, sess: Session) -> dict | None:
    """Resolve a call operand to a name dict."""
    # Direct call
    m = re.match(r"^0x([0-9a-fA-F]+)$", op_str)
    if m:
        va = int(m.group(1), 16)
        rva = va - sess.image_base
        name = pdb_mod.sym_at_rva(sess, rva)
        if name:
            return {"rva": hex(rva), "name": name}
        return {"rva": hex(rva), "name": "???"}

    # Indirect call through IAT
    m = re.search(r"\[0x([0-9a-fA-F]+)\]", op_str)
    if m:
        va = int(m.group(1), 16)
        rva = va - sess.image_base
        name = pe_mod.import_at_rva(sess.iat_map, rva)
        if name:
            return {"rva": hex(rva), "name": name, "type": "import"}

    # Virtual/register call
    if re.search(r"\[e[a-z]", op_str):
        return {"operand": op_str, "type": "virtual"}

    return None


def detect_this_register(sess: Session, func_rva: int, func_len: int) -> str | None:
    """Detect whether 'this' is in ECX or EDX by analyzing the function prologue.
    DIA's registerId for 'this' is unreliable (always returns EDX)."""
    try:
        scan_len = min(func_len, 64)
        code = sess.pe.get_data(func_rva, scan_len)
        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)
        md.detail = True

        for insn in md.disasm(code, sess.image_base + func_rva):
            mn = insn.mnemonic
            ops = insn.op_str

            if mn in ("push", "pop", "sub", "add", "nop", "int3",
                      "xorps", "pxor", "xor"):
                continue

            if mn == "mov":
                if ", ecx" in ops and "ecx," not in ops:
                    return "ecx"
                if ", edx" in ops and "edx," not in ops:
                    return "edx"
                if "[ecx" in ops:
                    return "ecx"
                if "[edx" in ops:
                    return "edx"

            if "[ecx" in ops:
                return "ecx"
            if "[edx" in ops:
                return "edx"

            if mn in ("call", "ret", "jmp"):
                break
    except Exception:
        pass
    return None
