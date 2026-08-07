"""Analysis tools: xrefs, calls, strings, calling_info, scan_usercalls, template_recover."""

from __future__ import annotations

import re
from typing import Any

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_MODE_64
from pydia2 import cvconst

from ._constants import CC_NAMES, LOC_NAMES, REG_NAMES_X86, NSF_REGEX
from ._state import manager
from . import _pdb as pdb_mod
from . import _pe as pe_mod
from . import _disasm
from . import _util


def register(mcp):
    """Register analysis tools on the given FastMCP instance."""

    @mcp.tool()
    def xrefs_to(name_or_rva: str, search_rva: int = 0, search_len: int = 0,
                 alias: str | None = None) -> list[dict]:
        """Find cross-references to a function/address within a given range (or entire .text)."""
        sess = manager.get(alias)

        if name_or_rva.startswith("0x"):
            target_rva = int(name_or_rva, 16)
        else:
            enum = sess.global_scope.findChildren(
                cvconst.SymTag.Function, name_or_rva, NSF_REGEX
            )
            if enum.count == 0:
                return [{"error": f"'{name_or_rva}' not found"}]
            target_rva = int(enum.Item(0).relativeVirtualAddress)

        target_va = sess.image_base + target_rva

        if search_rva == 0:
            for section in sess.pe.sections:
                if b".text" in section.Name:
                    search_rva = section.VirtualAddress
                    search_len = section.Misc_VirtualSize
                    break
        if search_len == 0:
            return [{"error": "No .text section found and no range specified"}]

        code = sess.pe.get_data(search_rva, search_len)

        # A LINEAR capstone sweep of a whole .text does NOT work and fails SILENTLY: md.disasm()
        # is a generator that stops dead at the first byte it cannot decode, and a 5.8 MB .text is
        # full of jump tables, alignment padding and data islands. The old implementation did
        # exactly that and therefore returned [] for essentially every target - including
        # dJointSetHinge2Param, which calls_in_function proves is called from
        # ai::Vehicle::_KeepGearBox at 0x1e1265. Two separate investigations recorded "no call site
        # found" on the strength of that empty list before the cause was noticed.
        #
        # So direct rel32 branches are found by scanning for the OPCODE instead. E8 (call rel32)
        # and E9 (jmp rel32) are five bytes, displacement relative to the end of the instruction.
        # A false positive needs four data bytes that happen to encode exactly the displacement to
        # the target, which is not a coincidence that happens by accident; a hit can always be
        # confirmed with disasm at the reported address. Everything found this way is reported with
        # method="opcode-scan".
        results = []
        seen: set[int] = set()
        n = len(code)
        for i in range(n - 5):
            op = code[i]
            if op != 0xE8 and op != 0xE9:
                continue
            disp = int.from_bytes(code[i + 1:i + 5], "little", signed=True)
            if sess.image_base + search_rva + i + 5 + disp != target_va:
                continue
            caller_rva = search_rva + i
            seen.add(caller_rva)
            results.append({
                "from_rva": hex(caller_rva),
                "from_va": hex(sess.image_base + caller_rva),
                "caller": pdb_mod.sym_at_rva(sess, caller_rva) or hex(caller_rva),
                "type": "call" if op == 0xE8 else "jmp",
                "method": "opcode-scan",
            })

        # Short/conditional branches and anything else capstone can reach are still worth having,
        # so the decode pass is kept - but restarted after each bad byte instead of giving up, and
        # only as a SUPPLEMENT to the scan above. Results already found are not duplicated.
        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)
        branch_mnemonics = {"call", "jmp", "je", "jne", "jg", "jl", "jge", "jle", "ja", "jb", "jae", "jbe"}
        offset = 0
        while offset < n:
            last_end = offset
            for insn in md.disasm(code[offset:], sess.image_base + search_rva + offset):
                last_end = insn.address - sess.image_base - search_rva + insn.size
                if insn.mnemonic not in branch_mnemonics:
                    continue
                try:
                    op_val = int(insn.op_str, 16)
                except ValueError:
                    continue
                if op_val != target_va:
                    continue
                caller_rva = insn.address - sess.image_base
                if caller_rva in seen:
                    continue
                seen.add(caller_rva)
                results.append({
                    "from_rva": hex(caller_rva),
                    "from_va": hex(insn.address),
                    "caller": pdb_mod.sym_at_rva(sess, caller_rva) or hex(caller_rva),
                    "type": insn.mnemonic,
                    "method": "decode",
                })
            offset = max(last_end, offset + 1)

        results.sort(key=lambda r: int(r["from_rva"], 16))
        return results

    @mcp.tool()
    def calls_in_function(name_or_rva: str, alias: str | None = None) -> list[dict]:
        """List all calls from a function with resolved names."""
        sess = manager.get(alias)
        func_rva, func_len, _ = pdb_mod.resolve_function(sess, name_or_rva)
        if func_rva == 0 and not name_or_rva.startswith("0x"):
            return [{"error": f"'{name_or_rva}' not found"}]
        if func_len == 0:
            func_len = 4096

        code = sess.pe.get_data(func_rva, func_len)
        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)

        results = []
        seen: set = set()
        for insn in md.disasm(code, sess.image_base + func_rva):
            if insn.mnemonic != "call":
                continue

            entry: dict[str, Any] = {"insn_rva": hex(insn.address - sess.image_base)}

            m = re.match(r"^0x([0-9a-fA-F]+)$", insn.op_str)
            if m:
                va = int(m.group(1), 16)
                rva = va - sess.image_base
                name = pdb_mod.sym_at_rva(sess, rva)
                entry["target_rva"] = hex(rva)
                entry["name"] = name or "???"
                entry["type"] = "direct"
                key = ("direct", rva)
            else:
                m2 = re.search(r"\[0x([0-9a-fA-F]+)\]", insn.op_str)
                if m2:
                    va = int(m2.group(1), 16)
                    iat_rva = va - sess.image_base
                    name = pe_mod.import_at_rva(sess.iat_map, iat_rva)
                    entry["iat_rva"] = hex(iat_rva)
                    entry["name"] = name or f"[{hex(va)}]"
                    entry["type"] = "import"
                    key = ("import", iat_rva)
                else:
                    entry["operand"] = insn.op_str
                    entry["type"] = "indirect"
                    key = ("indirect", insn.op_str)

            if key not in seen:
                seen.add(key)
                results.append(entry)

        return results

    @mcp.tool()
    def strings_in_function(name_or_rva: str, alias: str | None = None) -> list[dict]:
        """Extract all string references from a function."""
        sess = manager.get(alias)
        func_rva, func_len, _ = pdb_mod.resolve_function(sess, name_or_rva)
        if func_rva == 0 and not name_or_rva.startswith("0x"):
            return [{"error": f"'{name_or_rva}' not found"}]
        if func_len == 0:
            func_len = 4096

        code = sess.pe.get_data(func_rva, func_len)
        mode = CS_MODE_64 if sess.is_64 else CS_MODE_32
        md = Cs(CS_ARCH_X86, mode)

        results = []
        for insn in md.disasm(code, sess.image_base + func_rva):
            if insn.mnemonic == "push":
                m = re.match(r"^0x([0-9a-fA-F]+)$", insn.op_str)
                if m:
                    va = int(m.group(1), 16)
                    rva = va - sess.image_base
                    s = pe_mod.read_cstring(sess.pe, rva)
                    if s:
                        results.append({
                            "insn_rva": hex(insn.address - sess.image_base),
                            "string_rva": hex(rva),
                            "value": s,
                        })
        return results

    @mcp.tool()
    def calling_info(name_or_rva: str, alias: str | None = None) -> dict:
        """Analyze a function's calling convention and parameter locations.
        Detects __usercall patterns by comparing actual param registers/stack
        against what the declared calling convention expects."""
        sess = manager.get(alias)
        func_rva, func_len, func_name = pdb_mod.resolve_function(sess, name_or_rva)
        if func_rva == 0:
            return {"error": f"Function '{name_or_rva}' not found"}

        sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
        if not sym:
            return {"error": f"No symbol at RVA {hex(func_rva)}"}

        result: dict[str, Any] = {"name": func_name, "rva": hex(func_rva)}

        ret_type = "void"
        cc_id = -1
        try:
            ft = sym.type
            if ft:
                try:
                    ret_type = pdb_mod.type_name(ft.type)
                except Exception:
                    pass
                try:
                    cc_id = int(ft.callingConvention)
                except Exception:
                    pass
        except Exception:
            pass

        cc_name = CC_NAMES.get(cc_id, f"cc({cc_id})")
        result["calling_convention"] = cc_name
        result["return_type"] = ret_type

        # Parameter/local locations
        params = []
        data_children = sym.findChildren(cvconst.SymTag.Data, None, 0)
        for i in range(min(data_children.count, 50)):
            child = data_children.Item(i)
            pinfo: dict[str, Any] = {}
            try:
                pinfo["name"] = child.name
            except Exception:
                continue
            try:
                pinfo["type"] = pdb_mod.type_name(child.type)
            except Exception:
                pinfo["type"] = "?"
            try:
                loc = int(child.locationType)
                pinfo["location"] = LOC_NAMES.get(loc, str(loc))
            except Exception:
                pinfo["location"] = "?"
            try:
                reg = int(child.registerId)
                pinfo["register_id"] = reg
                pinfo["register"] = REG_NAMES_X86.get(reg, f"reg({reg})")
            except Exception:
                pass
            try:
                pinfo["offset"] = int(child.offset)
            except Exception:
                pass
            try:
                rva_val = int(child.relativeVirtualAddress)
                if rva_val > 0:
                    pinfo["rva"] = hex(rva_val)
            except Exception:
                pass
            params.append(pinfo)

        result["params"] = params

        # Detect usercall pattern
        enregs = [p for p in params if p.get("location") == "enreg"]
        non_this_enregs = [p for p in enregs if p.get("name") != "this"]
        this_param = [p for p in enregs if p.get("name") == "this"]

        is_usercall = False
        notes = []

        actual_this_reg = None
        if this_param and func_len > 0:
            actual_this_reg = _disasm.detect_this_register(sess, func_rva, func_len)
            if actual_this_reg:
                this_param[0]["register"] = actual_this_reg
                this_param[0]["register_id"] = 18 if actual_this_reg == "ecx" else 19

        if cc_id == 11:  # thiscall
            if actual_this_reg and actual_this_reg != "ecx":
                notes.append(f"this in {actual_this_reg} instead of ecx (detected from disasm)")
                is_usercall = True
            if non_this_enregs:
                for p in non_this_enregs:
                    notes.append(f"{p['name']} in {p.get('register', '?')} (expected on stack)")
                is_usercall = True
        elif cc_id == 0:  # cdecl
            if enregs:
                for p in enregs:
                    if p.get("name") == "this":
                        reg = actual_this_reg or p.get("register", "?")
                        notes.append(f"has 'this' in {reg} for cdecl")
                    else:
                        notes.append(f"{p['name']} in {p.get('register', '?')} (expected on stack)")
                is_usercall = True
        elif cc_id == 7:  # stdcall
            if enregs:
                for p in enregs:
                    notes.append(f"{p['name']} in {p.get('register', '?')} (expected on stack)")
                is_usercall = True
        elif cc_id == 4:  # fastcall
            if len(enregs) > 2:
                for p in enregs[2:]:
                    notes.append(f"extra reg param: {p['name']} in {p.get('register', '?')}")
                is_usercall = True

        result["is_usercall"] = is_usercall
        if notes:
            result["notes"] = notes

        if is_usercall:
            sig_parts = []
            for p in params:
                loc = p.get("location", "")
                reg = p.get("register", "")
                ptype = p.get("type", "?")
                pname = p.get("name", "?")
                if loc == "enreg":
                    sig_parts.append(f"{ptype} {pname}@<{reg}>")
                else:
                    sig_parts.append(f"{ptype} {pname}")
            result["usercall_signature"] = f"{ret_type} __usercall {func_name}({', '.join(sig_parts)})"

        return result

    @mcp.tool()
    def scan_usercalls(class_name: str = "", pattern: str = "",
                       max_results: int = 100, alias: str | None = None) -> list[dict]:
        """Scan functions for non-standard calling conventions (__usercall patterns).
        Optionally filter by class name or function name pattern."""
        sess = manager.get(alias)

        if class_name:
            enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, class_name, 0)
            if enum.count == 0:
                return [{"error": f"Type '{class_name}' not found"}]
            udt = enum.Item(0)
            funcs = udt.findChildren(cvconst.SymTag.Function, None, 0)
        elif pattern:
            funcs = sess.global_scope.findChildren(cvconst.SymTag.Function, pattern, NSF_REGEX)
        else:
            return [{"error": "Provide class_name or pattern"}]

        results = []
        for i in range(min(funcs.count, max_results * 5)):
            func = funcs.Item(i)
            try:
                rva = int(func.relativeVirtualAddress)
                if rva == 0:
                    continue

                data_children = func.findChildren(cvconst.SymTag.Data, None, 0)
                has_unusual = False
                cc_id = -1
                try:
                    cc_id = int(func.type.callingConvention)
                except Exception:
                    continue

                for j in range(min(data_children.count, 20)):
                    child = data_children.Item(j)
                    try:
                        loc = int(child.locationType)
                        name = child.name
                        if loc == 5 and name != "this":
                            has_unusual = True
                            break
                    except Exception:
                        continue

                if not has_unusual and cc_id == 11:
                    func_len = int(func.length) if func.length else 0
                    if func_len > 0:
                        actual_this = _disasm.detect_this_register(sess, rva, func_len)
                        if actual_this and actual_this != "ecx":
                            has_unusual = True

                if has_unusual:
                    info = calling_info(hex(rva), alias)
                    if info.get("is_usercall"):
                        results.append(info)
                        if len(results) >= max_results:
                            break
            except Exception:
                continue

        return results

    @mcp.tool()
    def template_recover(template_base: str, max_instantiations: int = 30,
                         alias: str | None = None) -> dict:
        """Recover a C++ template definition by analyzing all its PDB instantiations.
        Pass the template name WITHOUT angle brackets, e.g. 'ref_ptr', 'std::vector'.
        Finds all instantiations, compares layouts, deduces template-dependent parts."""
        sess = manager.get(alias)

        search = f"{template_base}<*"
        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, search, NSF_REGEX)
        if enum.count == 0:
            return {"error": f"No instantiations of '{template_base}' found"}

        instantiations = []
        for i in range(min(enum.count, max_instantiations)):
            udt = enum.Item(i)
            full_name = udt.name
            size = int(udt.length)
            args = _util.parse_template_args(full_name, template_base)
            if not args:
                continue

            fields = []
            children = udt.findChildren(cvconst.SymTag.Data, None, 0)
            for j in range(min(children.count, 100)):
                child = children.Item(j)
                try:
                    fields.append({
                        "name": child.name,
                        "offset": int(child.offset),
                        "type": pdb_mod.type_name(child.type),
                        "size": int(child.type.length) if child.type else 0,
                    })
                except Exception:
                    pass

            methods = []
            mchildren = udt.findChildren(cvconst.SymTag.Function, None, 0)
            for j in range(min(mchildren.count, 100)):
                child = mchildren.Item(j)
                try:
                    mname = child.name
                    minfo: dict[str, Any] = {"name": mname}
                    ft = child.type
                    if ft:
                        try:
                            minfo["return_type"] = pdb_mod.type_name(ft.type)
                        except Exception:
                            pass
                        pargs = ft.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
                        ptypes = []
                        for k in range(pargs.count):
                            ptypes.append(pdb_mod.type_name(pargs.Item(k).type))
                        minfo["param_types"] = ptypes
                    methods.append(minfo)
                except Exception:
                    pass

            bases = []
            bchildren = udt.findChildren(cvconst.SymTag.BaseClass, None, 0)
            for j in range(min(bchildren.count, 10)):
                child = bchildren.Item(j)
                try:
                    bases.append({"name": child.name, "offset": int(child.offset)})
                except Exception:
                    pass

            instantiations.append({
                "full_name": full_name,
                "args": args,
                "size": size,
                "fields": fields,
                "methods": methods,
                "base_classes": bases,
            })

        if not instantiations:
            return {"error": "Could not parse any instantiations"}

        # Determine template parameter names
        num_params = max(len(inst["args"]) for inst in instantiations)
        param_names = ["T", "U", "V", "W", "X"][:num_params]

        # Analyze fields across instantiations
        field_analysis: dict[tuple, list] = {}
        for inst in instantiations:
            for f in inst["fields"]:
                key = (f["name"], f["offset"])
                if key not in field_analysis:
                    field_analysis[key] = []
                field_analysis[key].append({
                    "type": f["type"], "args": inst["args"], "size": f["size"],
                })

        template_fields = []
        for (fname, foffset), occurrences in sorted(field_analysis.items(), key=lambda x: x[0][1]):
            types_seen = set(o["type"] for o in occurrences)
            if len(types_seen) == 1:
                template_fields.append({
                    "name": fname, "offset": foffset,
                    "type": list(types_seen)[0],
                    "is_template_dependent": False,
                })
            else:
                param_idx = -1
                for pi in range(num_params):
                    matches = True
                    for o in occurrences:
                        arg_val = o["args"][pi] if pi < len(o["args"]) else ""
                        if arg_val not in o["type"]:
                            matches = False
                            break
                    if matches:
                        param_idx = pi
                        break

                if param_idx >= 0:
                    sample = occurrences[0]
                    pattern = sample["type"].replace(
                        sample["args"][param_idx], param_names[param_idx]
                    )
                    template_fields.append({
                        "name": fname, "offset": foffset, "type": pattern,
                        "is_template_dependent": True,
                        "depends_on": param_names[param_idx],
                        "observed_types": sorted(types_seen),
                    })
                else:
                    template_fields.append({
                        "name": fname, "offset": foffset,
                        "type": f"/* varies: {sorted(types_seen)} */",
                        "is_template_dependent": True,
                        "observed_types": sorted(types_seen),
                    })

        # Analyze methods
        method_analysis: dict[str, list] = {}
        for inst in instantiations:
            for m in inst["methods"]:
                norm_name = m["name"]
                for pi, arg in enumerate(inst["args"]):
                    if pi < num_params and arg in norm_name:
                        norm_name = norm_name.replace(arg, param_names[pi])
                norm_name = re.sub(r"operator\s+(class|struct)\s+\S+\s*\*", "operator T*", norm_name)
                norm_name = re.sub(r"operator\s+const\s+(class|struct)\s+\S+\s*\*", "operator const T*", norm_name)

                if norm_name not in method_analysis:
                    method_analysis[norm_name] = []
                method_analysis[norm_name].append({
                    "return_type": m.get("return_type", "void"),
                    "param_types": m.get("param_types", []),
                    "args": inst["args"],
                    "orig_name": m["name"],
                })

        template_methods = []
        seen_methods: set[str] = set()
        for mname, occurrences in method_analysis.items():
            if mname.startswith("__") or mname.startswith("`"):
                continue
            if mname in seen_methods:
                continue
            seen_methods.add(mname)

            ret_types = set(o["return_type"] for o in occurrences)
            all_param_sets = set(tuple(o["param_types"]) for o in occurrences)

            minfo: dict[str, Any] = {"name": mname}

            if len(ret_types) == 1:
                minfo["return_type"] = list(ret_types)[0]
            else:
                for pi in range(num_params):
                    for o in occurrences:
                        arg_val = o["args"][pi] if pi < len(o["args"]) else ""
                        if arg_val in o["return_type"]:
                            minfo["return_type"] = o["return_type"].replace(arg_val, param_names[pi])
                            break
                    if "return_type" in minfo:
                        break
                if "return_type" not in minfo:
                    minfo["return_type"] = "/* varies */"

            if len(all_param_sets) == 1:
                minfo["params"] = list(list(all_param_sets)[0])
            else:
                sample = occurrences[0]
                params = []
                for pt in sample["param_types"]:
                    replaced = pt
                    for pi in range(num_params):
                        arg_val = sample["args"][pi] if pi < len(sample["args"]) else ""
                        if arg_val and arg_val in replaced:
                            replaced = replaced.replace(arg_val, param_names[pi])
                    params.append(replaced)
                minfo["params"] = params

            template_methods.append(minfo)

        # Base classes
        base_analysis: dict[int, set] = {}
        for inst in instantiations:
            for b in inst["base_classes"]:
                off = b["offset"]
                if off not in base_analysis:
                    base_analysis[off] = set()
                base_analysis[off].add(b["name"])

        template_bases = []
        for off, names in sorted(base_analysis.items()):
            if len(names) == 1:
                template_bases.append({"name": list(names)[0], "offset": off})
            else:
                for pi in range(num_params):
                    for n in names:
                        for inst in instantiations:
                            if any(b["name"] == n for b in inst["base_classes"]):
                                arg_val = inst["args"][pi] if pi < len(inst["args"]) else ""
                                if arg_val in n:
                                    template_bases.append({
                                        "name": n.replace(arg_val, param_names[pi]),
                                        "offset": off,
                                        "is_template_dependent": True,
                                    })
                                    break
                            break
                        break

        # Build reconstructed template
        size_set = set(inst["size"] for inst in instantiations)
        size_note = list(size_set)[0] if len(size_set) == 1 else f"varies: {sorted(size_set)}"

        tparams = ", ".join(f"class {p}" for p in param_names)
        lines = [f"template<{tparams}>"]
        lines.append(f"class {template_base}")
        if template_bases:
            base_strs = [b["name"] for b in template_bases]
            lines[-1] += " : " + ", ".join(f"public {b}" for b in base_strs)
        lines.append("{")
        lines.append(f"    // size: {size_note}")

        if template_fields:
            lines.append("")
            for f in template_fields:
                dep = f"  // depends on {f['depends_on']}" if f.get("depends_on") else ""
                lines.append(f"    {f['type']:30s} {f['name']};  // +0x{f['offset']:02X}{dep}")

        if template_methods:
            lines.append("")
            for m in template_methods:
                ret = m.get("return_type", "void")
                mn = m["name"]
                mparams = m.get("params", [])
                if instantiations:
                    for pi, arg in enumerate(instantiations[0]["args"]):
                        if pi < num_params and arg:
                            ret = ret.replace(arg, param_names[pi])
                            mn = mn.replace(arg, param_names[pi])
                            mparams = [p.replace(arg, param_names[pi]) for p in mparams]
                lines.append(f"    {ret} {mn}({', '.join(mparams)});")

        lines.append("};")

        return {
            "template_base": template_base,
            "num_instantiations": len(instantiations),
            "template_params": param_names,
            "instantiation_args": [{"name": inst["full_name"], "args": inst["args"]} for inst in instantiations],
            "size": size_note,
            "fields": template_fields,
            "methods": template_methods,
            "base_classes": template_bases,
            "reconstructed": "\n".join(lines),
        }
