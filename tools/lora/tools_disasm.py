"""Disassembly tools: disasm, func_detail, batch_func_detail."""

from __future__ import annotations

import re
from typing import Any

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64
from pydia2 import cvconst

from ._constants import CC_NAMES, LOC_NAMES, REG_NAMES_X86
from ._state import manager
from . import _pdb as pdb_mod
from . import _pe as pe_mod
from . import _disasm


def register(mcp):
    """Register disassembly tools on the given FastMCP instance."""

    @mcp.tool()
    def disasm(name_or_rva: str, max_insns: int = 300,
              alias: str | None = None) -> list[str]:
        """Annotated disassembly of a function by name or RVA (hex '0x18c480').
        Enhanced with symbol names, strings, import resolution."""
        sess = manager.get(alias)
        func_rva, func_len, func_name = pdb_mod.resolve_function(sess, name_or_rva)
        if func_rva == 0 and not name_or_rva.startswith("0x"):
            return [f"Function '{name_or_rva}' not found"]
        if func_len == 0:
            func_len = 4096

        code = sess.pe.get_data(func_rva, func_len)
        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)
        md.detail = True

        lines = []
        if func_name:
            lines.append(f"; === {func_name} (RVA {hex(func_rva)}, {func_len} bytes) ===")

        for insn in md.disasm(code, sess.image_base + func_rva):
            op = _disasm.annotate_operand(insn.op_str, insn.mnemonic, sess)
            lines.append(f"  {hex(insn.address)}: {insn.mnemonic:8s} {op}")
            if len(lines) >= max_insns:
                lines.append(f"  ; ... truncated at {max_insns} instructions")
                break

        return lines

    @mcp.tool()
    def disasm_detailed(name_or_rva: str, alias: str | None = None,
                    line_start: int = 0, line_end: int = 0,
                    group_offset: int = 0, group_limit: int = 0) -> dict:
        """Source-line-grouped disassembly with locals, calls, strings.
        Full PDB context: prototype, calling convention, params, source lines.
        Highest detail level — use disasm_compressed for LLM token efficiency.

        Pagination (for large functions like Application::init):
          line_start/line_end — return only groups within this source line range
          group_offset/group_limit — return groups[offset:offset+limit]
        Result always includes a 'header' with total function info."""
        sess = manager.get(alias)
        func_rva, func_len, func_name = pdb_mod.resolve_function(sess, name_or_rva)
        if func_rva == 0:
            return {"error": f"Function '{name_or_rva}' not found"}

        # 1. Prototype
        proto = pdb_mod.get_prototype_by_rva(sess, func_rva)

        # 2. Locals/params
        locals_info = []
        try:
            sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
            if sym:
                data_children = sym.findChildren(cvconst.SymTag.Data, None, 0)
                for i in range(min(data_children.count, 100)):
                    child = data_children.Item(i)
                    entry: dict[str, Any] = {}
                    try:
                        entry["name"] = child.name
                    except Exception:
                        pass
                    try:
                        entry["type"] = pdb_mod.type_name(child.type)
                    except Exception:
                        pass
                    try:
                        loc = int(child.locationType)
                        entry["location"] = LOC_NAMES.get(loc, str(loc))
                    except Exception:
                        pass
                    try:
                        reg = int(child.registerId)
                        entry["register"] = REG_NAMES_X86.get(reg, f"reg({reg})")
                    except Exception:
                        pass
                    try:
                        entry["offset"] = int(child.offset)
                    except Exception:
                        pass
                    try:
                        rva_val = int(child.relativeVirtualAddress)
                        if rva_val > 0:
                            entry["rva"] = hex(rva_val)
                    except Exception:
                        pass
                    locals_info.append(entry)
        except Exception:
            pass

        # 3. Source line mapping
        line_map: list[dict] = []
        source_file = ""
        try:
            lines_enum = sess.session.findLinesByRVA(func_rva, func_len)
            for i in range(min(lines_enum.count, 500)):
                le = lines_enum.Item(i)
                lrva = int(le.relativeVirtualAddress)
                lnum = int(le.lineNumber)
                if not source_file:
                    try:
                        source_file = le.sourceFile.fileName
                    except Exception:
                        pass
                line_map.append({"line": lnum, "rva": lrva})
            line_map.sort(key=lambda x: x["rva"])
        except Exception:
            pass

        # 4. Disassemble
        code = sess.pe.get_data(func_rva, func_len)
        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)
        md.detail = True

        rva_to_line: dict[int, int] = {e["rva"]: e["line"] for e in line_map}

        source_groups: list[dict] = []
        current_line = 0
        current_group: dict[str, Any] | None = None
        all_calls = []
        all_strings = []

        for insn in md.disasm(code, sess.image_base + func_rva):
            insn_rva = insn.address - sess.image_base
            op = _disasm.annotate_operand(insn.op_str, insn.mnemonic, sess)
            asm_line = f"{hex(insn.address)}: {insn.mnemonic:8s} {op}"

            new_line = rva_to_line.get(insn_rva, 0)
            if new_line and new_line != current_line:
                current_line = new_line
                current_group = {
                    "source_line": current_line,
                    "rva_start": hex(insn_rva),
                    "asm": [],
                    "calls": [],
                    "strings": [],
                }
                source_groups.append(current_group)
            elif current_group is None:
                current_group = {
                    "source_line": 0,
                    "rva_start": hex(insn_rva),
                    "asm": [],
                    "calls": [],
                    "strings": [],
                    "note": "prologue",
                }
                source_groups.append(current_group)

            current_group["asm"].append(asm_line)

            if insn.mnemonic == "call":
                call_info = _disasm.resolve_call_target(insn.op_str, sess)
                if call_info:
                    current_group["calls"].append(call_info)
                    all_calls.append(call_info)

            if insn.mnemonic in ("push", "mov"):
                m = re.search(r"0x([0-9a-fA-F]+)", insn.op_str)
                if m:
                    va = int(m.group(1), 16)
                    s = pe_mod.read_cstring(sess.pe, va - sess.image_base)
                    if s:
                        sinfo = {"rva": hex(va - sess.image_base), "value": s}
                        current_group["strings"].append(sinfo)
                        all_strings.append(sinfo)

        # Clean up empty fields
        for g in source_groups:
            if not g["calls"]:
                del g["calls"]
            if not g["strings"]:
                del g["strings"]

        # Calling convention info
        cc_info: dict[str, Any] = {}
        try:
            sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
            if sym and sym.type:
                ft = sym.type
                cc_id = int(ft.callingConvention)
                cc_info["convention"] = CC_NAMES.get(cc_id, f"cc({cc_id})")
                try:
                    cc_info["return_type"] = pdb_mod.type_name(ft.type)
                except Exception:
                    pass
        except Exception:
            pass

        result: dict[str, Any] = {
            "name": func_name,
            "rva": hex(func_rva),
            "length": func_len,
        }
        if proto and "signature" in proto:
            result["signature"] = proto["signature"]
        if cc_info:
            result["calling_convention"] = cc_info
        if source_file:
            result["source_file"] = source_file
        if line_map:
            result["source_lines"] = f"{line_map[0]['line']}-{line_map[-1]['line']}"
        result["locals"] = locals_info

        # Apply pagination
        total_groups = len(source_groups)
        paginated = False

        if line_start > 0 or line_end > 0:
            ls = line_start if line_start > 0 else 0
            le = line_end if line_end > 0 else 999999
            source_groups = [g for g in source_groups if ls <= g["source_line"] <= le]
            paginated = True
        elif group_offset > 0 or group_limit > 0:
            go = group_offset
            gl = group_limit if group_limit > 0 else len(source_groups)
            source_groups = source_groups[go:go + gl]
            paginated = True

        if paginated:
            # Recompute calls/strings from filtered groups only
            all_calls = []
            all_strings = []
            for g in source_groups:
                all_calls.extend(g.get("calls", []))
                all_strings.extend(g.get("strings", []))
            result["pagination"] = {
                "total_groups": total_groups,
                "returned_groups": len(source_groups),
            }
            if line_start > 0 or line_end > 0:
                result["pagination"]["line_range"] = f"{line_start}-{line_end}"
            else:
                result["pagination"]["group_range"] = f"{group_offset}-{group_offset + len(source_groups)}"

        result["groups"] = source_groups
        result["summary"] = {
            "total_groups": total_groups,
            "returned_groups": len(source_groups),
            "total_instructions": sum(len(g["asm"]) for g in source_groups),
            "source_line_groups": len([g for g in source_groups if g["source_line"] > 0]),
            "unique_calls": list({c.get("name", c.get("operand", "?")): c for c in all_calls}.values()),
            "strings": list({s["value"]: s for s in all_strings}.values()),
        }

        return result

    # Keep old name as alias for backward compatibility
    func_detail = disasm_detailed

    @mcp.tool()
    def batch_disasm_detailed(rvas: list[str], alias: str | None = None) -> list[dict]:
        """Batch version of disasm_detailed — fetch multiple functions in one call.
        Pass a list of RVA hex strings. Much faster than individual calls."""
        results = []
        for rva_str in rvas:
            try:
                results.append(disasm_detailed(rva_str, alias))
            except Exception as e:
                results.append({"rva": rva_str, "error": str(e)})
        return results

    # Keep old name as alias
    batch_func_detail = batch_disasm_detailed
