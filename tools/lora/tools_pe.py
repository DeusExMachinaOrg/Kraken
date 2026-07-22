"""PE-level tools: sections, imports, exports, raw reads, raw disasm."""

from __future__ import annotations

import struct

from ._state import manager
from ._util import parse_rva
from . import _pe as pe_mod
from . import _pdb as pdb_mod
from . import _disasm


def register(mcp):
    """Register all PE tools on the given FastMCP instance."""

    @mcp.tool()
    def pe_sections(alias: str | None = None) -> list[dict]:
        """List PE sections with their RVA, size, and characteristics."""
        sess = manager.get(alias)
        results = []
        for section in sess.pe.sections:
            results.append({
                "name": section.Name.rstrip(b"\x00").decode(errors="replace"),
                "virtual_address": hex(section.VirtualAddress),
                "virtual_size": hex(section.Misc_VirtualSize),
                "raw_size": hex(section.SizeOfRawData),
                "characteristics": hex(section.Characteristics),
            })
        return results

    @mcp.tool()
    def pe_imports(alias: str | None = None, max_results: int = 200) -> list[dict]:
        """List imported functions grouped by DLL."""
        sess = manager.get(alias)
        if not hasattr(sess.pe, "DIRECTORY_ENTRY_IMPORT"):
            return []
        results = []
        count = 0
        for entry in sess.pe.DIRECTORY_ENTRY_IMPORT:
            dll_name = entry.dll.decode(errors="replace")
            for imp in entry.imports:
                results.append({
                    "dll": dll_name,
                    "name": imp.name.decode(errors="replace") if imp.name else None,
                    "ordinal": imp.ordinal,
                    "address": hex(imp.address),
                })
                count += 1
                if count >= max_results:
                    return results
        return results

    @mcp.tool()
    def pe_exports(alias: str | None = None) -> list[dict]:
        """List exported functions."""
        sess = manager.get(alias)
        if not hasattr(sess.pe, "DIRECTORY_ENTRY_EXPORT"):
            return []
        results = []
        for exp in sess.pe.DIRECTORY_ENTRY_EXPORT.symbols:
            results.append({
                "name": exp.name.decode(errors="replace") if exp.name else None,
                "ordinal": exp.ordinal,
                "rva": hex(exp.address),
            })
        return results

    @mcp.tool()
    def read_bytes(rva: int | str, size: int = 64, alias: str | None = None) -> str:
        """Read raw bytes at an RVA. Accepts int or hex string ('0x58fdc8'). Returns hex string."""
        sess = manager.get(alias)
        data = pe_mod.read_bytes_at_rva(sess.pe, parse_rva(rva), size)
        return data.hex()

    @mcp.tool()
    def read_string(rva: int | str, max_len: int = 256, encoding: str = "ascii",
                    alias: str | None = None) -> str:
        """Read a null-terminated string at an RVA. Accepts int or hex string."""
        sess = manager.get(alias)
        data = pe_mod.read_bytes_at_rva(sess.pe, parse_rva(rva), max_len)
        null = data.find(b"\x00")
        if null >= 0:
            data = data[:null]
        return data.decode(encoding, errors="replace")

    @mcp.tool()
    def read_wide_string(rva: int | str, max_len: int = 512,
                         alias: str | None = None) -> str:
        """Read a null-terminated wide (UTF-16LE) string at an RVA. Accepts int or hex string."""
        sess = manager.get(alias)
        return pe_mod.read_wide_string(sess.pe, parse_rva(rva), max_len)

    @mcp.tool()
    def read_value(rva: int | str, alias: str | None = None) -> dict:
        """Read data at RVA and return all plausible interpretations:
        i32, u32, i16, u16, float, double, hex bytes, ASCII string, pointer->symbol.
        One call instead of guessing which read_* to use."""
        sess = manager.get(alias)
        r = parse_rva(rva)
        data = pe_mod.read_bytes_at_rva(sess.pe, r, 128)

        result: dict = {"rva": hex(r)}

        # Integer interpretations (first 4 bytes)
        if len(data) >= 4:
            u32 = struct.unpack_from("<I", data)[0]
            i32 = struct.unpack_from("<i", data)[0]
            f32 = struct.unpack_from("<f", data)[0]
            result["u32"] = u32
            result["i32"] = i32
            result["hex32"] = f"0x{u32:08x}"
            # Float: show if it looks like a real float (not denormal/inf/nan garbage)
            if 1e-10 < abs(f32) < 1e10 or f32 == 0.0:
                result["float"] = f32

        # Double (first 8 bytes)
        if len(data) >= 8:
            f64 = struct.unpack_from("<d", data)[0]
            if 1e-30 < abs(f64) < 1e30 or f64 == 0.0:
                result["double"] = f64

        # Raw hex (first 16 bytes)
        result["bytes"] = data[:16].hex(" ")

        # ASCII string attempt
        null = data.find(b"\x00")
        if null > 0:
            try:
                s = data[:null].decode("ascii", errors="strict")
                if len(s) >= 2 and all(0x20 <= ord(c) < 0x7F for c in s):
                    result["string"] = s
            except (UnicodeDecodeError, ValueError):
                pass

        # Pointer: treat u32 as VA, resolve to symbol
        if len(data) >= 4:
            u32 = struct.unpack_from("<I", data)[0]
            ptr_rva = u32 - sess.image_base
            if 0 < ptr_rva < 0x1000000:  # reasonable RVA range
                sym = pdb_mod.sym_at_rva(sess, ptr_rva)
                if sym:
                    result["ptr_symbol"] = sym
                    result["ptr_rva"] = hex(ptr_rva)

        return result

    @mcp.tool()
    def batch_read_value(rvas: list[int | str], alias: str | None = None) -> list[dict]:
        """Read data at multiple RVAs and return all plausible interpretations for each.
        Same as read_value but batched — one call instead of N."""
        sess = manager.get(alias)
        results = []
        for rva_raw in rvas:
            r = parse_rva(rva_raw)
            entry: dict = {"rva": hex(r)}
            try:
                data = pe_mod.read_bytes_at_rva(sess.pe, r, 128)
                if len(data) >= 4:
                    u32 = struct.unpack_from("<I", data)[0]
                    i32 = struct.unpack_from("<i", data)[0]
                    f32 = struct.unpack_from("<f", data)[0]
                    entry["u32"] = u32
                    entry["i32"] = i32
                    entry["hex32"] = f"0x{u32:08x}"
                    if 1e-10 < abs(f32) < 1e10 or f32 == 0.0:
                        entry["float"] = f32
                if len(data) >= 8:
                    f64 = struct.unpack_from("<d", data)[0]
                    if 1e-30 < abs(f64) < 1e30 or f64 == 0.0:
                        entry["double"] = f64
                entry["bytes"] = data[:16].hex(" ")
                null = data.find(b"\x00")
                if null > 0:
                    try:
                        s = data[:null].decode("ascii", errors="strict")
                        if len(s) >= 2 and all(0x20 <= ord(c) < 0x7F for c in s):
                            entry["string"] = s
                    except (UnicodeDecodeError, ValueError):
                        pass
                if len(data) >= 4:
                    u32 = struct.unpack_from("<I", data)[0]
                    ptr_rva = u32 - sess.image_base
                    if 0 < ptr_rva < 0x1000000:
                        sym = pdb_mod.sym_at_rva(sess, ptr_rva)
                        if sym:
                            entry["ptr_symbol"] = sym
                            entry["ptr_rva"] = hex(ptr_rva)
            except Exception as e:
                entry["error"] = str(e)
            results.append(entry)
        return results

    @mcp.tool()
    def resolve_import(iat_va: int | str, alias: str | None = None) -> dict:
        """Resolve an IAT entry (virtual address from call [addr]) to DLL + function name.
        Accepts int or hex string."""
        sess = manager.get(alias)
        va = parse_rva(iat_va)
        target_rva = va - sess.image_base
        entry = sess.iat_map.get(target_rva)
        if entry:
            return {
                "dll": entry["dll"],
                "name": entry["name"],
                "ordinal": entry["ordinal"],
                "iat_rva": hex(target_rva),
            }
        return {"error": f"No import at VA {hex(va)} (RVA {hex(target_rva)})"}

    @mcp.tool()
    def resolve_imports_batch(iat_vas: list[int], alias: str | None = None) -> list[dict]:
        """Resolve multiple IAT entries at once. Pass a list of virtual addresses."""
        sess = manager.get(alias)
        results = []
        for va in iat_vas:
            rva = va - sess.image_base
            entry = sess.iat_map.get(rva)
            if entry:
                r = dict(entry)
                r["va"] = hex(va)
                r["iat_rva"] = hex(rva)
                results.append(r)
            else:
                results.append({"va": hex(va), "error": f"No import at RVA {hex(rva)}"})
        return results

    @mcp.tool()
    def disasm_raw(rva: int | str, size: int = 256, max_insns: int = 100,
                   alias: str | None = None) -> list[dict]:
        """Disassemble code at a given RVA without PDB annotation. Accepts int or hex string."""
        sess = manager.get(alias)
        return _disasm.disassemble_raw(
            sess.pe, parse_rva(rva), size, sess.is_64, sess.image_base, max_insns
        )

    @mcp.tool()
    def find_ret(rva: int | str, max_insns: int = 200,
                 alias: str | None = None) -> dict:
        """Scan forward from RVA to find the first ret/retn/ret N instruction.
        Returns the ret instruction details and stack cleanup bytes.
        Useful for quick calling convention checks (ret 4 = 1 stack param, etc.)."""
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64

        sess = manager.get(alias)
        start_rva = parse_rva(rva)

        # Read enough bytes to find the ret (most functions < 4KB)
        scan_size = min(max_insns * 8, 4096)
        try:
            code = sess.pe.get_data(start_rva, scan_size)
        except Exception:
            return {"error": f"Cannot read at RVA {hex(start_rva)}"}

        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)

        for insn in md.disasm(code, sess.image_base + start_rva):
            if insn.mnemonic in ("ret", "retn"):
                ret_rva = insn.address - sess.image_base
                # Parse stack cleanup from operand
                cleanup = 0
                if insn.op_str:
                    try:
                        cleanup = int(insn.op_str, 0)
                    except ValueError:
                        pass
                return {
                    "rva": hex(ret_rva),
                    "va": hex(insn.address),
                    "instruction": f"{insn.mnemonic} {insn.op_str}".strip(),
                    "stack_cleanup": cleanup,
                    "distance": ret_rva - start_rva,
                }

        return {"error": f"No ret found within {max_insns} instructions from RVA {hex(start_rva)}"}

    @mcp.tool()
    def vtable_method_at(vtable_rva: int | str, offset: int | str,
                         alias: str | None = None) -> dict:
        """Look up a single vtable slot from PE .rdata bytes.
        vtable_rva: RVA of the vtable (find via find_public_symbols('??_7ClassName@@6B@'))
        offset: byte offset into vtable (e.g. 0x3c from 'call [edx + 0x3c]')
        Returns: {index, offset, rva, name, signature} for the method at that slot.
        Always accurate — reads actual PE data, not PDB declaration order."""
        sess = manager.get(alias)
        vt_rva = parse_rva(vtable_rva)
        off = parse_rva(offset)

        ptr_size = 8 if sess.is_64 else 4
        slot_index = off // ptr_size

        # Read just the one pointer
        try:
            data = pe_mod.read_bytes_at_rva(sess.pe, vt_rva + off, ptr_size)
        except Exception:
            return {"error": f"Cannot read vtable at RVA {hex(vt_rva)} + {hex(off)}"}

        pack_fmt = "<Q" if sess.is_64 else "<I"
        va = struct.unpack_from(pack_fmt, data)[0]
        func_rva = va - sess.image_base

        # Check if it points to executable code
        in_exec = any(s <= func_rva < e for s, e in sess.exec_ranges)

        result: dict = {
            "index": slot_index,
            "offset": hex(off),
            "rva": hex(func_rva),
        }

        if not in_exec:
            result["warning"] = "pointer does not point to executable section"
            return result

        # Resolve symbol
        sym = pdb_mod.sym_at_rva(sess, func_rva)
        if sym:
            result["name"] = sym

        # Get prototype
        from . import _pdb as pdb_full
        proto = pdb_full.get_prototype_by_rva(sess, func_rva)
        if proto and "signature" in proto:
            result["signature"] = proto["signature"]

        return result

    @mcp.tool()
    def decode_switch(func_rva: int | str, alias: str | None = None) -> list[dict]:
        """Decode MSVC switch/case jump tables inside a function.
        Scans for the pattern: cmp reg, N / ja default / movzx reg, byte [reg+table] / jmp [reg*4+jmptable].
        Returns [{case_value, handler_rva, handler_symbol}] for each case.
        func_rva: RVA of the function containing the switch."""
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64, CS_GRP_JUMP

        sess = manager.get(alias)
        start_rva = parse_rva(func_rva)

        # Find function length from PDB
        func_len = 8192  # default scan size
        sym_info = pdb_mod.sym_at_rva(sess, start_rva)
        if sym_info:
            from . import _pdb as pdb_full
            proto = pdb_full.get_prototype_by_rva(sess, start_rva)
            # Try to get length from PDB function
            # Fall back to generous scan
        try:
            code = sess.pe.get_data(start_rva, func_len)
        except Exception:
            return [{"error": f"Cannot read code at RVA {hex(start_rva)}"}]

        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)
        md.detail = True

        base = sess.image_base
        insns = list(md.disasm(code, base + start_rva))

        results = []

        for i, insn in enumerate(insns):
            # Look for: jmp dword ptr [reg*4 + jmptable_va]
            if insn.mnemonic != "jmp":
                continue
            op_str = insn.op_str
            # Match patterns like "dword ptr [eax*4 + 0x54826c]"
            import re
            m = re.search(r'\[.*\*4\s*\+\s*(0x[0-9a-fA-F]+)\]', op_str)
            if not m:
                m = re.search(r'\[(0x[0-9a-fA-F]+)\s*\+\s*.*\*4\]', op_str)
            if not m:
                continue

            jmptable_va = int(m.group(1), 16)
            jmptable_rva = jmptable_va - base

            # Find the preceding movzx + cmp to get byte table and case count
            byte_table_va = None
            max_cases = 0
            base_value = 0  # the value subtracted before the switch (e.g., lea eax, [ebp - 8])

            for j in range(max(0, i - 15), i):
                prev = insns[j]
                # Look for: movzx reg, byte ptr [reg + byte_table_va]
                if prev.mnemonic == "movzx" and "byte ptr" in prev.op_str:
                    m2 = re.search(r'\[.*\+\s*(0x[0-9a-fA-F]+)\]', prev.op_str)
                    if m2:
                        byte_table_va = int(m2.group(1), 16)
                # Look for: cmp reg, N  (max case index)
                if prev.mnemonic == "cmp" and prev.op_str.count(",") == 1:
                    parts = prev.op_str.split(",")
                    val_str = parts[1].strip()
                    try:
                        max_cases = int(val_str, 0) + 1
                    except ValueError:
                        pass
                # Look for: lea/add/sub that computes base offset
                # e.g., lea eax, [ebp - 8] or sub eax, 8
                if prev.mnemonic == "lea" and "-" in prev.op_str:
                    m3 = re.search(r'-\s*(0x[0-9a-fA-F]+|\d+)', prev.op_str)
                    if m3:
                        val = m3.group(1)
                        base_value = int(val, 16) if val.startswith("0x") else int(val)
                elif prev.mnemonic == "sub":
                    parts = prev.op_str.split(",")
                    if len(parts) == 2:
                        try:
                            base_value = int(parts[1].strip(), 0)
                        except ValueError:
                            pass
                elif prev.mnemonic == "add":
                    parts = prev.op_str.split(",")
                    if len(parts) == 2:
                        try:
                            base_value = -int(parts[1].strip(), 0)
                        except ValueError:
                            pass

            if max_cases == 0:
                max_cases = 256  # fallback

            # Read byte dispatch table
            byte_table_rva = (byte_table_va - base) if byte_table_va else None

            # Read jump table pointers
            ptr_size = 4
            # Determine number of unique handler groups
            handler_group_count = 0

            if byte_table_rva is not None:
                try:
                    byte_data = pe_mod.read_bytes_at_rva(sess.pe, byte_table_rva, max_cases)
                except Exception:
                    byte_data = b""
                handler_group_count = max(byte_data) + 1 if byte_data else 0
            else:
                # No byte table — jump table is direct
                handler_group_count = max_cases

            # Read jump table entries
            try:
                jt_data = pe_mod.read_bytes_at_rva(
                    sess.pe, jmptable_rva, handler_group_count * ptr_size
                )
            except Exception:
                jt_data = b""

            handler_rvas = []
            for k in range(handler_group_count):
                off = k * ptr_size
                if off + ptr_size <= len(jt_data):
                    va_val = struct.unpack_from("<I", jt_data, off)[0]
                    handler_rvas.append(va_val - base)
                else:
                    handler_rvas.append(None)

            # Build case → handler mapping
            switch_result = {
                "jmp_insn_va": hex(insn.address),
                "jump_table_rva": hex(jmptable_rva),
                "byte_table_rva": hex(byte_table_rva) if byte_table_rva else None,
                "base_value": base_value,
                "num_cases": max_cases,
                "cases": [],
            }

            if byte_table_rva is not None and byte_data:
                for case_idx in range(min(max_cases, len(byte_data))):
                    group = byte_data[case_idx]
                    case_value = case_idx + base_value
                    h_rva = handler_rvas[group] if group < len(handler_rvas) else None
                    entry = {
                        "case": case_value,
                        "case_hex": hex(case_value),
                        "group": group,
                        "handler_rva": hex(h_rva) if h_rva else None,
                    }
                    # Resolve handler symbol
                    if h_rva is not None:
                        sym = pdb_mod.sym_at_rva(sess, h_rva)
                        if sym:
                            entry["handler"] = sym
                    switch_result["cases"].append(entry)
            else:
                for case_idx in range(min(max_cases, len(handler_rvas))):
                    case_value = case_idx + base_value
                    h_rva = handler_rvas[case_idx]
                    entry = {
                        "case": case_value,
                        "case_hex": hex(case_value),
                        "handler_rva": hex(h_rva) if h_rva else None,
                    }
                    if h_rva is not None:
                        sym = pdb_mod.sym_at_rva(sess, h_rva)
                        if sym:
                            entry["handler"] = sym
                    switch_result["cases"].append(entry)

            results.append(switch_result)

        return results if results else [{"error": "No switch/jump table found in function"}]
