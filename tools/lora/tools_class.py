"""Class analysis tools: class_overview, pure_virtual_interface, vftable, vft_method, deps."""

from __future__ import annotations

import re
import struct
from typing import Any

from pydia2 import cvconst

from ._constants import CC_NAMES, ACCESS_NAMES, NSF_REGEX
from ._state import manager
from . import _pdb as pdb_mod
from . import _pe as pe_mod


def _find_vtable_rva(sess, class_name: str) -> int | None:
    """Find the vtable RVA for a class by searching for ??_7 public symbols.
    MSVC mangles vftables as ??_7ClassName@@6B@ (or variants for namespaces)."""
    from ._constants import NSF_REGEX

    # Try searching for ??_7*ClassName*  with wildcards
    # Extract the short class name for matching
    short_name = class_name.split("::")[-1]

    # Search public symbols for vftable entries
    pattern = f"??_7*{short_name}*"
    pub_enum = sess.global_scope.findChildren(cvconst.SymTag.PublicSymbol, pattern, NSF_REGEX)

    # Collect all exact vtable matches
    parts = class_name.split("::")
    mangled_parts = "@".join(reversed(parts))
    exact_matches: list[tuple[str, int]] = []  # (mangled_name, rva)
    partial_match_rva = None

    for i in range(min(pub_enum.count, 50)):
        sym = pub_enum.Item(i)
        try:
            name = sym.name
            rva = int(sym.relativeVirtualAddress)
            if not name or rva == 0:
                continue
            if not name.startswith("??_7"):
                continue
            if mangled_parts in name:
                exact_matches.append((name, rva))
            elif short_name in name and partial_match_rva is None:
                partial_match_rva = rva
        except Exception:
            continue

    if not exact_matches:
        return partial_match_rva

    # Single vtable — no ambiguity
    if len(exact_matches) == 1:
        return exact_matches[0][1]

    # Multi-inheritance: pick primary vtable.
    # Single-inheritance form @@6B@ is always primary.
    for name, rva in exact_matches:
        if name.endswith("@@6B@"):
            return rva

    # MI form: @@6BBaseClass@ns@@  — find the base at offset 0 from PDB.
    primary_base = _find_primary_base(sess, class_name)
    if primary_base:
        pb_short = primary_base.split("::")[-1]
        for name, rva in exact_matches:
            if f"6B{pb_short}@" in name:
                return rva

    # Last resort: first match
    return exact_matches[0][1]


def _find_primary_base(sess, class_name: str) -> str | None:
    """Find the base class at offset 0 (primary base) from PDB."""
    try:
        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, class_name, 0)
        if enum.count == 0:
            return None
        udt = enum.Item(0)
        bases = udt.findChildren(cvconst.SymTag.BaseClass, None, 0)
        for i in range(bases.count):
            b = bases.Item(i)
            if int(b.offset) == 0:
                return b.type.name if b.type else b.name
    except Exception:
        pass
    return None


def register(mcp):
    """Register class analysis tools on the given FastMCP instance."""

    @mcp.tool()
    def class_overview(class_name: str, alias: str | None = None) -> dict:
        """Get a complete class overview: layout, vftable (if present), all method prototypes.
        Uses RVA-based prototype lookup so overloads are correctly distinguished."""
        sess = manager.get(alias)
        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, class_name, 0)
        if enum.count == 0:
            return {"error": f"Type '{class_name}' not found"}

        udt = enum.Item(0)
        result: dict[str, Any] = {
            "name": class_name,
            "size": int(udt.length),
            "fields": [],
            "base_classes": [],
            "methods": [],
        }

        all_children = udt.findChildren(cvconst.SymTag.Null, None, 0)
        for i in range(min(all_children.count, 500)):
            child = all_children.Item(i)
            tag = int(child.symTag)

            if tag == int(cvconst.SymTag.Data):
                field: dict[str, Any] = {}
                try:
                    field["name"] = child.name
                except Exception:
                    pass
                try:
                    field["offset"] = int(child.offset)
                except Exception:
                    pass
                try:
                    field["type"] = pdb_mod.type_name(child.type)
                except Exception:
                    pass
                try:
                    acc = int(child.access)
                    field["access"] = ACCESS_NAMES.get(acc, str(acc))
                except Exception:
                    pass
                result["fields"].append(field)

            elif tag == int(cvconst.SymTag.BaseClass):
                bc: dict[str, Any] = {}
                try:
                    bc["name"] = child.name
                except Exception:
                    pass
                try:
                    bc["offset"] = int(child.offset)
                except Exception:
                    pass
                result["base_classes"].append(bc)

            elif tag == int(cvconst.SymTag.Function):
                try:
                    mname = child.name
                    mrva = int(child.relativeVirtualAddress)
                    mlen = int(child.length)
                    if mname and mrva > 0:
                        proto = pdb_mod.get_prototype_by_rva(sess, mrva)
                        mentry: dict[str, Any] = {
                            "name": mname,
                            "rva": hex(mrva),
                            "length": mlen,
                        }
                        if proto and "signature" in proto:
                            mentry["signature"] = proto["signature"]
                        result["methods"].append(mentry)
                except Exception:
                    pass

        return result

    @mcp.tool()
    def pure_virtual_interface(class_name: str, alias: str | None = None,
                               slot_start: int = 0, slot_end: int = 0) -> dict:
        """Extract full method signatures for a pure-virtual interface class (like IRenderer).
        These classes have all methods at RVA=0 (implemented in a DLL), so class_overview
        skips them. This reads function types from the UDT to recover signatures.

        Pagination (for large interfaces like IRenderer with 345 methods):
          slot_start/slot_end — return only methods in this slot index range.
        Result always includes total_methods count."""
        sess = manager.get(alias)
        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, class_name, 0)
        if enum.count == 0:
            return {"error": f"Type '{class_name}' not found"}

        udt = enum.Item(0)
        result: dict[str, Any] = {
            "name": class_name,
            "size": int(udt.length),
            "base_classes": [],
            "methods": [],
        }

        all_children = udt.findChildren(cvconst.SymTag.Null, None, 0)
        method_index = 0
        for i in range(all_children.count):
            child = all_children.Item(i)
            tag = int(child.symTag)

            if tag == int(cvconst.SymTag.BaseClass):
                bc: dict[str, Any] = {}
                try:
                    bc["name"] = child.name
                    bc["offset"] = int(child.offset)
                except Exception:
                    pass
                result["base_classes"].append(bc)

            elif tag == int(cvconst.SymTag.Function):
                try:
                    mname = child.name
                    if mname in ("__local_vftable_ctor_closure", "__vecDelDtor"):
                        method_index += 1
                        continue
                    short_name = class_name.split("::")[-1]
                    if mname == short_name:
                        method_index += 1
                        continue

                    ret_type = "void"
                    params: list = []
                    cc_name = ""
                    is_virtual = False

                    try:
                        is_virtual = bool(child.virtual)
                    except Exception:
                        pass

                    try:
                        ft = child.type
                        if ft:
                            try:
                                ret_type = pdb_mod.type_name(ft.type)
                            except Exception:
                                pass
                            try:
                                cc_name = CC_NAMES.get(int(ft.callingConvention), "")
                            except Exception:
                                pass
                            args_enum = ft.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
                            for j in range(args_enum.count):
                                at = pdb_mod.type_name(args_enum.Item(j).type)
                                if at == "...":
                                    params.append("...")
                                else:
                                    params.append({"type": at, "name": f"a{j}"})
                    except Exception:
                        pass

                    prefix = "virtual " if is_virtual else ""
                    cc_part = f" {cc_name}" if cc_name and cc_name != "__thiscall" else ""
                    param_str = ", ".join(
                        p if isinstance(p, str) else f"{p['type']} {p['name']}"
                        for p in params
                    )
                    sig = f"{prefix}{ret_type}{cc_part} {mname}({param_str})"
                    sig = re.sub(r"\s+", " ", sig).strip()

                    mentry: dict[str, Any] = {
                        "index": method_index,
                        "vtable_offset": hex(method_index * 4),
                        "name": mname,
                        "signature": sig,
                        "return_type": ret_type,
                        "params": params,
                        "virtual": is_virtual,
                    }
                    result["methods"].append(mentry)
                    method_index += 1
                except Exception:
                    method_index += 1

        result["total_methods"] = len(result["methods"])

        # Apply slot range filter
        if slot_start > 0 or slot_end > 0:
            se = slot_end if slot_end > 0 else 999999
            result["methods"] = [m for m in result["methods"]
                                 if slot_start <= m["index"] <= se]
            result["returned_methods"] = len(result["methods"])
            result["slot_range"] = f"{slot_start}-{se}"

        return result

    @mcp.tool()
    def vftable(rva: str, max_entries: int = 50, alias: str | None = None) -> list[dict]:
        """Dump a vftable at the given RVA (hex string like '0x58e770'). Always pass RVA, not VA.
        Returns [{index, rva, name, signature}] for each virtual method slot.
        Stops when pointer leaves .text or when the parent class changes. Cached."""
        sess = manager.get(alias)
        vt_rva = int(rva, 16)

        if vt_rva in sess.vftable_cache:
            return sess.vftable_cache[vt_rva]

        ptr_size = 8 if sess.is_64 else 4
        pack_fmt = "<Q" if sess.is_64 else "<I"
        raw = sess.pe.get_data(vt_rva, max_entries * ptr_size)

        results = []
        owner_class: str | None = None

        for i in range(max_entries):
            va = struct.unpack_from(pack_fmt, raw, i * ptr_size)[0]
            entry_rva = va - sess.image_base
            if not any(s <= entry_rva < e for s, e in sess.exec_ranges):
                break

            name = pdb_mod.sym_at_rva(sess, entry_rva)

            if name and "::" in name:
                parent = pdb_mod.func_parent_class(sess, entry_rva)
                if i == 0:
                    owner_class = parent
                elif parent and owner_class and parent != owner_class:
                    break

            entry: dict[str, Any] = {"index": i, "rva": hex(entry_rva)}
            if name:
                entry["name"] = name
            proto = pdb_mod.get_prototype_by_rva(sess, entry_rva)
            if proto and "signature" in proto:
                entry["signature"] = proto["signature"]
            results.append(entry)

        sess.vftable_cache[vt_rva] = results
        return results

    @mcp.tool()
    def vft_method(class_name: str, vtable_offset: str,
                   alias: str | None = None) -> dict:
        """Extract a single virtual method signature from a class by vtable offset (hex).
        Useful for resolving unknown virtual calls like 'call [edx + 0x3a0]'.
        Tries PE vtable bytes first (ground truth), falls back to PDB declaration walk.
        Result includes 'source': 'PE' or 'PDB' to indicate confidence level."""
        sess = manager.get(alias)
        target = int(vtable_offset, 16) if isinstance(vtable_offset, str) else vtable_offset
        target_idx = target // 4

        # --- Try PE-based resolution first (ground truth) ---
        vt_rva = _find_vtable_rva(sess, class_name)
        if vt_rva is not None:
            ptr_size = 8 if sess.is_64 else 4
            pack_fmt = "<Q" if sess.is_64 else "<I"
            try:
                raw = sess.pe.get_data(vt_rva + target, ptr_size)
                va = struct.unpack_from(pack_fmt, raw, 0)[0]
                func_rva = va - sess.image_base

                if any(s <= func_rva < e for s, e in sess.exec_ranges):
                    name = pdb_mod.sym_at_rva(sess, func_rva)
                    proto = pdb_mod.get_prototype_by_rva(sess, func_rva)

                    result: dict[str, Any] = {
                        "index": target_idx,
                        "vtable_offset": hex(target),
                        "rva": hex(func_rva),
                        "vtable_rva": hex(vt_rva),
                        "source": "PE",
                    }
                    if name:
                        result["name"] = name
                    if proto and "signature" in proto:
                        result["signature"] = proto["signature"]
                        result["return_type"] = proto.get("return_type", "")
                        result["params"] = proto.get("params", [])
                        result["virtual"] = proto.get("virtual", True)
                    return result
            except Exception:
                pass  # fall through to PDB

        # --- Fallback: PDB declaration walk (for pure virtual interfaces, DLL types) ---
        classes_to_search: list[tuple[str, Any]] = []
        visited: set[str] = set()

        def _collect_classes(cname: str):
            if cname in visited:
                return
            visited.add(cname)
            e = sess.global_scope.findChildren(cvconst.SymTag.UDT, cname, 0)
            if e.count == 0:
                return
            udt = e.Item(0)
            classes_to_search.append((cname, udt))
            bases = udt.findChildren(cvconst.SymTag.BaseClass, None, 0)
            for i in range(bases.count):
                base = bases.Item(i)
                try:
                    base_name = pdb_mod.type_name(base.type) if base.type else base.name
                    _collect_classes(base_name)
                except Exception:
                    try:
                        _collect_classes(base.name)
                    except Exception:
                        pass

        _collect_classes(class_name)

        for cname, udt in classes_to_search:
            result = pdb_mod.find_virtual_at_offset(udt, target)
            if result:
                result["index"] = target_idx
                result["vtable_offset"] = hex(target)
                result["source"] = "PDB"
                if cname != class_name:
                    result["inherited_from"] = cname
                return result

        return {"error": f"No virtual method at offset {vtable_offset} (index {target_idx}) in '{class_name}'"}

    @mcp.tool()
    def prototype(func_name: str, alias: str | None = None) -> dict:
        """Get the full prototype of a function as a readable C signature.
        For overloaded functions, returns the first match. Use prototype_at_rva for specific overload."""
        sess = manager.get(alias)
        result = pdb_mod.get_prototype_by_name(sess, func_name)
        if not result:
            return {"error": f"Function '{func_name}' not found"}
        return result

    @mcp.tool()
    def prototype_at_rva(rva: str, alias: str | None = None) -> dict:
        """Get the prototype of the function at a specific RVA.
        Use this instead of prototype() when dealing with overloaded functions."""
        sess = manager.get(alias)
        result = pdb_mod.get_prototype_by_rva(sess, int(rva, 16))
        if not result:
            return {"error": f"No function at RVA {rva}"}
        return result

    @mcp.tool()
    def prototypes(func_names: list[str], alias: str | None = None) -> list[dict]:
        """Batch get prototypes for multiple functions at once."""
        sess = manager.get(alias)
        return [pdb_mod.get_prototype_by_name(sess, n) or {"name": n, "error": "not found"}
                for n in func_names]

    @mcp.tool()
    def deps(class_name: str, alias: str | None = None) -> dict:
        """Analyze a class and return all its type dependencies with source file paths.
        Finds types referenced in: fields, base classes, method params, return types."""
        sess = manager.get(alias)
        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, class_name, 0)
        if enum.count == 0:
            return {"error": f"Type '{class_name}' not found"}

        udt = enum.Item(0)
        dep_types: dict[str, set] = {}

        def _add_dep(type_str: str, context: str):
            base = type_str.rstrip("*").rstrip("[]").strip()
            if base in ("void", "char", "int", "float", "double", "bool",
                        "unsigned int", "unsigned long", "int8_t", "int16_t",
                        "uint8_t", "uint16_t", "__int64", "unsigned __int64",
                        "long", "HRESULT", "?", "...", "FunctionType",
                        "HWND__", "HWND__*", class_name):
                return
            if base.startswith("std::") or base.startswith("base("):
                return
            if base not in dep_types:
                dep_types[base] = set()
            dep_types[base].add(context)

        all_children = udt.findChildren(cvconst.SymTag.Null, None, 0)
        for i in range(min(all_children.count, 500)):
            child = all_children.Item(i)
            tag = int(child.symTag)

            if tag == int(cvconst.SymTag.Data):
                try:
                    _add_dep(pdb_mod.type_name(child.type), f"field:{child.name}")
                except Exception:
                    pass
            elif tag == int(cvconst.SymTag.BaseClass):
                try:
                    _add_dep(child.name, "base_class")
                except Exception:
                    pass
            elif tag == int(cvconst.SymTag.Function):
                try:
                    mname = child.name
                    mrva = int(child.relativeVirtualAddress)
                    if mrva > 0:
                        proto = pdb_mod.get_prototype_by_rva(sess, mrva)
                        if proto:
                            rt = proto.get("return_type", "")
                            if rt:
                                _add_dep(rt, f"return:{mname}")
                            for p in proto.get("params", []):
                                parts = p.rsplit(" ", 1)
                                if len(parts) >= 1:
                                    _add_dep(parts[0], f"param:{mname}")
                except Exception:
                    pass

        deps_list = []
        for dep_name, contexts in sorted(dep_types.items()):
            sf = pdb_mod.source_file_for_type(sess, dep_name)
            deps_list.append({
                "type": dep_name,
                "source_file": sf,
                "used_in": sorted(contexts),
            })

        class_sf = pdb_mod.source_file_for_type(sess, class_name)
        return {
            "class": class_name,
            "source_file": class_sf,
            "dependencies": deps_list,
        }
