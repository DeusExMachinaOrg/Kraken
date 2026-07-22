"""PDB query tools: find symbols, type layouts, enums, source files."""

from __future__ import annotations

from typing import Any

from pydia2 import cvconst

from ._constants import NSF_REGEX
from ._state import manager
from ._util import parse_rva
from . import _pdb as pdb_mod


def register(mcp):
    """Register all PDB query tools on the given FastMCP instance."""

    @mcp.tool()
    def find_functions(pattern: str | None = None, alias: str | None = None,
                       max_results: int = 50) -> list[dict]:
        """Find functions in the PDB. Pattern supports DIA wildcards (* and ?)."""
        sess = manager.get(alias)
        return pdb_mod.enum_children(sess.global_scope, "Function", pattern, max_results)

    @mcp.tool()
    def find_public_symbols(pattern: str | None = None, alias: str | None = None,
                            max_results: int = 50) -> list[dict]:
        """Find public (exported) symbols. Pattern supports DIA wildcards."""
        sess = manager.get(alias)
        return pdb_mod.enum_children(sess.global_scope, "PublicSymbol", pattern, max_results)

    @mcp.tool()
    def find_types(pattern: str | None = None, alias: str | None = None,
                   max_results: int = 50) -> list[dict]:
        """Find UDT (struct/class/union) types by name pattern."""
        sess = manager.get(alias)
        return pdb_mod.enum_children(sess.global_scope, "UDT", pattern, max_results)

    @mcp.tool()
    def find_enums(pattern: str | None = None, alias: str | None = None,
                   max_results: int = 50) -> list[dict]:
        """Find enum types by name pattern."""
        sess = manager.get(alias)
        return pdb_mod.enum_children(sess.global_scope, "Enum", pattern, max_results)

    @mcp.tool()
    def find_typedefs(pattern: str | None = None, alias: str | None = None,
                      max_results: int = 50) -> list[dict]:
        """Find typedefs by name pattern."""
        sess = manager.get(alias)
        return pdb_mod.enum_children(sess.global_scope, "Typedef", pattern, max_results)

    @mcp.tool()
    def find_data(pattern: str | None = None, alias: str | None = None,
                  max_results: int = 50) -> list[dict]:
        """Find global/static data symbols (variables) by name pattern."""
        sess = manager.get(alias)
        return pdb_mod.enum_children(sess.global_scope, "Data", pattern, max_results)

    @mcp.tool()
    def get_type_layout(type_name: str, alias: str | None = None,
                        offset_start: int = -1, offset_end: int = -1,
                        field_offset: int = 0, field_limit: int = 0) -> dict:
        """Get the layout of a struct/class/union: fields, offsets, sizes, base classes.

        Pagination (for large types like Application):
          offset_start/offset_end — return only fields within this byte-offset range
          field_offset/field_limit — return fields[offset:offset+limit]
        Result always includes total_fields count and type size."""
        sess = manager.get(alias)
        udts = pdb_mod.enum_children(sess.global_scope, "UDT", type_name, 1)
        if not udts:
            return {"error": f"Type '{type_name}' not found"}

        enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, type_name, 0)
        udt = enum.Item(0)

        result: dict[str, Any] = pdb_mod.symbol_info(udt)

        all_fields: list[dict] = []
        base_classes: list[dict] = []
        methods: list[dict] = []

        all_children = udt.findChildren(cvconst.SymTag.Null, None, 0)
        for i in range(min(all_children.count, 2000)):
            child = all_children.Item(i)
            tag = int(child.symTag)
            info = pdb_mod.symbol_info(child)

            if tag == int(cvconst.SymTag.Data):
                all_fields.append(info)
            elif tag == int(cvconst.SymTag.BaseClass):
                base_classes.append(info)
            elif tag == int(cvconst.SymTag.Function):
                methods.append(info)

        result["base_classes"] = base_classes
        result["total_fields"] = len(all_fields)
        result["total_methods"] = len(methods)

        # Apply offset range filter
        if offset_start >= 0 or offset_end >= 0:
            lo = offset_start if offset_start >= 0 else 0
            hi = offset_end if offset_end >= 0 else 0x7FFFFFFF
            all_fields = [f for f in all_fields if lo <= f.get("offset", -1) <= hi]
            methods = [m for m in methods if lo <= m.get("offset", -1) <= hi]

        # Apply pagination
        if field_limit > 0:
            all_fields = all_fields[field_offset:field_offset + field_limit]
            methods = methods[field_offset:field_offset + field_limit]

        result["fields"] = all_fields
        result["methods"] = methods
        return result

    @mcp.tool()
    def fields_at_offsets(type_name: str, offsets: list[int | str],
                          alias: str | None = None) -> list[dict]:
        """Look up fields at specific byte-offsets in a class/struct.
        Accepts hex strings ('0x8b240') or ints. Returns the field at each offset,
        or the nearest field before it with the delta.
        Walks base classes to include inherited fields.
        Much faster than get_type_layout for targeted lookups."""
        sess = manager.get(alias)

        def _collect_fields(udt_name: str, base_offset: int,
                            visited: set) -> list[tuple[int, str, str]]:
            if udt_name in visited:
                return []
            visited.add(udt_name)
            enum_res = sess.global_scope.findChildren(cvconst.SymTag.UDT, udt_name, 0)
            if enum_res.count == 0:
                return []
            udt = enum_res.Item(0)
            result: list[tuple[int, str, str]] = []
            # Collect own data fields
            children = udt.findChildren(cvconst.SymTag.Data, None, 0)
            for i in range(min(children.count, 2000)):
                f = children.Item(i)
                try:
                    o = int(f.offset)
                    n = f.name or ""
                    try:
                        t = f.type.name if f.type else "?"
                    except Exception:
                        t = "?"
                    if t is None:
                        t = "?"
                    result.append((base_offset + o, n, t))
                except Exception:
                    pass
            # Walk base classes
            bases = udt.findChildren(cvconst.SymTag.BaseClass, None, 0)
            for i in range(bases.count):
                b = bases.Item(i)
                try:
                    bname = b.type.name if b.type else b.name
                    boff = int(b.offset)
                    result.extend(_collect_fields(bname, base_offset + boff, visited))
                except Exception:
                    pass
            return result

        field_list = _collect_fields(type_name, 0, set())
        if not field_list:
            return [{"error": f"Type '{type_name}' not found or has no fields"}]
        field_list.sort(key=lambda x: x[0])

        results = []
        for raw_off in offsets:
            off = parse_rva(raw_off) if isinstance(raw_off, str) else raw_off

            # Exact match
            exact = [(o, n, t) for o, n, t in field_list if o == off]
            if exact:
                o, n, t = exact[0]
                results.append({
                    "offset": hex(off),
                    "field": n,
                    "type": t,
                })
                continue

            # Nearest field before
            before = [(o, n, t) for o, n, t in field_list if o < off]
            if before:
                o, n, t = before[-1]
                results.append({
                    "offset": hex(off),
                    "nearest_field": n,
                    "nearest_offset": hex(o),
                    "delta": off - o,
                    "type": t,
                })
            else:
                results.append({"offset": hex(off), "error": "no field found"})

        return results

    @mcp.tool()
    def get_enum_values(enum_name: str, alias: str | None = None) -> dict:
        """Get all values of a named enum."""
        sess = manager.get(alias)
        enum = sess.global_scope.findChildren(cvconst.SymTag.Enum, enum_name, 0)
        if enum.count == 0:
            return {"error": f"Enum '{enum_name}' not found"}
        sym = enum.Item(0)
        info = pdb_mod.symbol_info(sym)
        values = []
        children = sym.findChildren(cvconst.SymTag.Null, None, 0)
        for i in range(min(children.count, 1000)):
            child = children.Item(i)
            entry: dict[str, Any] = {}
            try:
                entry["name"] = child.name
            except Exception:
                pass
            try:
                entry["value"] = int(child.value)
            except Exception:
                pass
            values.append(entry)
        info["values"] = values
        return info

    @mcp.tool()
    def pdb_function_detail(func_name: str, alias: str | None = None) -> dict:
        """Get detailed PDB info for a function: parameters, locals, type, RVA.
        For deep disassembly analysis, use func_detail instead."""
        sess = manager.get(alias)
        enum = sess.global_scope.findChildren(cvconst.SymTag.Function, func_name, 0)
        if enum.count == 0:
            return {"error": f"Function '{func_name}' not found"}
        func = enum.Item(0)
        info = pdb_mod.symbol_info(func)

        try:
            ft = func.type
            if ft:
                try:
                    info["returnType"] = pdb_mod.type_name(ft.type)
                except Exception:
                    pass
                args = ft.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
                info["parameters"] = []
                for i in range(args.count):
                    arg = args.Item(i)
                    info["parameters"].append({"type": pdb_mod.type_name(arg.type)})
        except Exception:
            pass

        data_children = func.findChildren(cvconst.SymTag.Data, None, 0)
        info["locals"] = []
        for i in range(min(data_children.count, 100)):
            info["locals"].append(pdb_mod.symbol_info(data_children.Item(i)))

        return info

    @mcp.tool()
    def symbol_at_rva(rva: int | str, alias: str | None = None) -> dict:
        """Look up the symbol at a given RVA. Accepts int or hex string."""
        sess = manager.get(alias)
        rva = parse_rva(rva)
        sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.Null)
        if sym is None:
            return {"error": f"No symbol at RVA {hex(rva)}"}
        return pdb_mod.symbol_info(sym)

    @mcp.tool()
    def get_source_files(pattern: str | None = None, alias: str | None = None,
                         max_results: int = 50) -> list[str]:
        """List source files referenced in the PDB."""
        sess = manager.get(alias)
        files_enum = sess.session.findFile(None, pattern, 0)
        results = []
        for i in range(min(files_enum.count, max_results)):
            try:
                results.append(files_enum.Item(i).fileName)
            except Exception:
                pass
        return results

    @mcp.tool()
    def find_lines_by_rva(rva: int | str, length: int = 1,
                          alias: str | None = None) -> list[dict]:
        """Get source line info for an RVA range. Accepts int or hex string."""
        sess = manager.get(alias)
        rva = parse_rva(rva)
        lines = sess.session.findLinesByRVA(rva, length)
        results = []
        for i in range(min(lines.count, 100)):
            line = lines.Item(i)
            entry: dict[str, Any] = {}
            try:
                entry["file"] = line.sourceFile.fileName
            except Exception:
                pass
            try:
                entry["line"] = int(line.lineNumber)
            except Exception:
                pass
            try:
                entry["rva"] = hex(int(line.relativeVirtualAddress))
            except Exception:
                pass
            results.append(entry)
        return results

    @mcp.tool()
    def section_contributions(alias: str | None = None,
                              min_size: int = 100) -> list[dict]:
        """Get code size per compiland. Useful for measuring compilation unit sizes."""
        sess = manager.get(alias)
        compilands = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)
        results = []
        for i in range(compilands.count):
            comp = compilands.Item(i)
            name = comp.name or ""
            funcs = comp.findChildren(cvconst.SymTag.Function, None, 0)
            code_size = 0
            func_count = 0
            if funcs:
                func_count = funcs.count
                for j in range(funcs.count):
                    try:
                        code_size += int(funcs.Item(j).length)
                    except Exception:
                        pass
            if code_size < min_size:
                continue
            short = name.replace("\\", "/").split("/")[-1]
            results.append({
                "name": short,
                "full_path": name,
                "code_size": code_size,
                "functions": func_count,
            })
        results.sort(key=lambda x: -x["code_size"])
        return results

    @mcp.tool()
    def compiland_source_files(pattern: str, alias: str | None = None) -> dict:
        """List all source files (.h, .cpp, .inl) referenced by a compiland."""
        sess = manager.get(alias)
        compilands = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)

        target = None
        for i in range(compilands.count):
            comp = compilands.Item(i)
            if pattern.lower() in (comp.name or "").lower():
                target = comp
                break
        if not target:
            return {"error": f"Compiland '{pattern}' not found"}

        source_files: set[str] = set()
        funcs = target.findChildren(cvconst.SymTag.Function, None, 0)
        if funcs:
            for i in range(min(funcs.count, 500)):
                f = funcs.Item(i)
                try:
                    rva = int(f.relativeVirtualAddress)
                    length = int(f.length)
                    if rva and length:
                        lines = sess.session.findLinesByRVA(rva, min(length, 8))
                        if lines:
                            for j in range(min(lines.count, 5)):
                                try:
                                    sf = lines.Item(j).sourceFile.fileName
                                    if sf:
                                        source_files.add(sf)
                                except Exception:
                                    pass
                except Exception:
                    pass

        files = sorted(source_files)
        return {
            "compiland": target.name,
            "source_files": files,
            "count": len(files),
        }

    @mcp.tool()
    def build_file_map(alias: str | None = None) -> dict:
        """Build complete compiland→source file mapping for ALL compilands.
        Returns {compiland_name: {cpp: path, headers: [paths]}} for truxx/ files only.
        Iterates all compilands and extracts source file references."""
        sess = manager.get(alias)
        compilands = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)

        result = {}
        total = compilands.count if compilands else 0

        for ci in range(total):
            comp = compilands.Item(ci)
            comp_name = comp.name or ""
            if not comp_name:
                continue

            # Skip STL/CRT compilands
            if "\\vc7\\" in comp_name.lower() or "\\vc\\crt" in comp_name.lower():
                continue

            source_files = set()
            funcs = comp.findChildren(cvconst.SymTag.Function, None, 0)
            if funcs:
                for fi in range(funcs.count):  # full scan all funcs
                    f = funcs.Item(fi)
                    try:
                        rva = int(f.relativeVirtualAddress)
                        length = int(f.length)
                        if rva and length:
                            lines = sess.session.findLinesByRVA(rva, min(length, 8))
                            if lines:
                                for j in range(min(lines.count, 10)):
                                    try:
                                        sf = lines.Item(j).sourceFile.fileName
                                        if sf and "truxx\\" in sf.lower():
                                            idx = sf.lower().index("truxx\\") + 6
                                            source_files.add(sf[idx:])
                                    except Exception:
                                        pass
                    except Exception:
                        pass

            if source_files:
                # Separate into .h and .cpp
                headers = sorted(f for f in source_files if f.endswith('.h'))
                sources = sorted(f for f in source_files if f.endswith('.cpp') or f.endswith('.c'))
                obj_name = comp_name.split("\\")[-1].replace(".obj", "")
                result[obj_name] = {
                    "obj_path": comp_name,
                    "headers": headers,
                    "sources": sources,
                }

        return {
            "total_compilands": total,
            "mapped": len(result),
            "file_map": result,
        }

    @mcp.tool()
    def emit_headers(output_root: str = "source/include",
                     file_map_path: str = "file_map.json",
                     namespace_filter: str = "",
                     overwrite: bool = False,
                     alias: str | None = None) -> dict:
        """Batch-generate C++ headers from PDB class layouts.
        Reads file_map.json, looks up each class in PDB, writes .h files with
        fields at correct offsets, base classes, and method count.

        Args:
            output_root: where to write headers (default source/include)
            file_map_path: path to file_map.json from build_file_map
            namespace_filter: only emit types matching this namespace (e.g. "ai::")
            overwrite: if True, overwrite existing headers
        """
        import json as _json
        import os as _os

        sess = manager.get(alias)

        with open(file_map_path) as f:
            file_map = _json.load(f)

        generated = 0
        skipped = 0
        failed = 0
        errors = []

        for comp_name, info in file_map.items():
            headers = info.get('headers', [])
            obj_path = info.get('obj_path', '')

            # Find main header path
            main_header = None
            for h in headers:
                h_base = _os.path.splitext(_os.path.basename(h))[0].lower()
                if h_base == comp_name.lower():
                    main_header = h.replace('\\\\', '/').replace('\\', '/')
                    break

            if not main_header:
                if 'Server' in obj_path:
                    main_header = f'server/objects/{comp_name.lower()}.h'
                elif 'game' in obj_path:
                    main_header = f'game/{comp_name.lower()}.h'
                elif 'engine' in obj_path:
                    main_header = f'include/{comp_name.lower()}.h'
                else:
                    continue

            out_path = _os.path.join(output_root, main_header)
            if _os.path.exists(out_path) and not overwrite:
                skipped += 1
                continue

            # Try class names
            class_names = [
                f'ai::{comp_name}',
                f'm3d::{comp_name}',
                f'm3d::ui::{comp_name}',
                f'm3d::fs::{comp_name}',
                comp_name,
            ]

            if namespace_filter:
                class_names = [cn for cn in class_names if namespace_filter in cn]

            layout = None
            matched_name = None
            for cn in class_names:
                enum = sess.global_scope.findChildren(cvconst.SymTag.UDT, cn, 0)
                if enum and enum.count > 0:
                    sym = enum.Item(0)
                    size = int(sym.length)
                    fields = []
                    bases = []
                    mc = 0

                    children = sym.findChildren(cvconst.SymTag.Null, None, 0)
                    if children:
                        for ci in range(children.count):
                            ch = children.Item(ci)
                            t = int(ch.symTag)
                            if t == int(cvconst.SymTag.BaseClass):
                                bases.append({'name': ch.name or '', 'offset': int(getattr(ch, 'offset', 0))})
                            elif t == int(cvconst.SymTag.Data):
                                loc = int(getattr(ch, 'locationType', 0))
                                if loc == 6:
                                    fields.append({
                                        'name': ch.name or '',
                                        'offset': int(ch.offset),
                                        'type': (ch.type.name if ch.type else 'unknown'),
                                    })
                            elif t == int(cvconst.SymTag.Function):
                                mc += 1

                    layout = {'name': cn, 'size': size, 'fields': sorted(fields, key=lambda x: x['offset']),
                              'bases': bases, 'methods_count': mc}
                    matched_name = cn
                    break

            if not layout:
                failed += 1
                continue

            # Write header
            _os.makedirs(_os.path.dirname(out_path), exist_ok=True)
            short = matched_name.split('::')[-1]
            ns = matched_name.rsplit('::', 1)[0] if '::' in matched_name else ''

            base_decl = ''
            if layout['bases']:
                base_decl = ' : ' + ', '.join(f'public {b["name"]}' for b in layout['bases'])

            # Estimate parent size
            parent_size = 0
            for b in layout['bases']:
                bn = b['name']
                known = {'ai::Obj':192, 'ai::PhysicObj':288, 'ai::ComplexPhysicObj':332,
                         'ai::PhysicBody':344, 'm3d::Object':52, 'm3d::ui::Wnd':544,
                         'ai::Vehicle':2380}
                parent_size = known.get(bn, 0)

            own_fields = [fld for fld in layout['fields'] if fld['offset'] >= parent_size]

            with open(out_path, 'w') as hf:
                hf.write(f'// {matched_name} — {layout["size"]} bytes\n')
                hf.write(f'// Auto-generated from PDB (emit_headers tool)\n')
                hf.write(f'#pragma once\n\n')
                if ns:
                    hf.write(f'namespace {ns} {{\n\n')
                hf.write(f'class {short}{base_decl} {{\npublic:\n')
                hf.write(f'    // {layout["methods_count"]} methods\n\n')
                if own_fields:
                    for fld in own_fields:
                        hf.write(f'    // +0x{fld["offset"]:X}: {fld["type"]} {fld["name"]}\n')
                hf.write(f'\n    // Size: {layout["size"]} (0x{layout["size"]:X})\n')
                hf.write(f'}};\n')
                if ns:
                    hf.write(f'\n}} // namespace {ns}\n')

            generated += 1

        return {
            "generated": generated,
            "skipped": skipped,
            "failed": failed,
            "output_root": output_root,
        }
