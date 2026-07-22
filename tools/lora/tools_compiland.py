"""Compiland tools: dump, list, includes, inline detection."""

from __future__ import annotations

from typing import Any

from pydia2 import cvconst

from ._constants import DK_NAMES, NSF_REGEX
from ._state import manager
from . import _pdb as pdb_mod


def register(mcp):
    """Register compiland tools on the given FastMCP instance."""

    @mcp.tool()
    def compiland_dump(obj_pattern: str, alias: str | None = None) -> dict:
        """Dump everything the PDB knows about a compiland (.obj file).
        Pass a pattern like 'Kernel.obj' or 'kernel' (case-insensitive substring match).
        Returns: compiler info, all functions (with prototypes), data symbols, typedefs, labels."""
        sess = manager.get(alias)
        comps = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)
        target = None
        pat_lower = obj_pattern.lower()
        for i in range(comps.count):
            c = comps.Item(i)
            if pat_lower in c.name.lower():
                target = c
                break

        if target is None:
            return {"error": f"No compiland matching '{obj_pattern}'"}

        result: dict[str, Any] = {"compiland": target.name}

        # Compiler info
        envs = target.findChildren(cvconst.SymTag.CompilandEnv, None, 0)
        compiler_info = {}
        for i in range(envs.count):
            env = envs.Item(i)
            try:
                compiler_info[env.name] = str(env.value)
            except Exception:
                pass
        if compiler_info:
            result["compiler"] = compiler_info

        details = target.findChildren(cvconst.SymTag.CompilandDetails, None, 0)
        for i in range(details.count):
            try:
                result["compiler_name"] = details.Item(i).compilerName
            except Exception:
                pass

        # Data symbols
        datas = target.findChildren(cvconst.SymTag.Data, None, 0)
        data_list = []
        for i in range(datas.count):
            d = datas.Item(i)
            entry: dict[str, Any] = {}
            try:
                entry["name"] = d.name
            except Exception:
                continue
            try:
                dk = int(d.dataKind)
                entry["kind"] = DK_NAMES.get(dk, str(dk))
            except Exception:
                pass
            try:
                rva = int(d.relativeVirtualAddress)
                if rva > 0:
                    entry["rva"] = hex(rva)
            except Exception:
                pass
            try:
                entry["type"] = pdb_mod.type_name(d.type)
            except Exception:
                pass
            try:
                if int(d.dataKind) == 9:
                    entry["value"] = str(d.value)
            except Exception:
                pass
            data_list.append(entry)
        result["data"] = data_list

        # Functions
        funcs = target.findChildren(cvconst.SymTag.Function, None, 0)
        func_list = []
        std_funcs = 0
        for i in range(funcs.count):
            f = funcs.Item(i)
            try:
                fname = f.name
                frva = int(f.relativeVirtualAddress)
                flen = int(f.length)

                if fname.startswith("std::"):
                    std_funcs += 1
                    continue

                entry: dict[str, Any] = {
                    "name": fname,
                    "rva": hex(frva),
                    "length": flen,
                }

                if frva > 0:
                    proto = pdb_mod.get_prototype_by_rva(sess, frva)
                    if proto and "signature" in proto:
                        entry["signature"] = proto["signature"]
                    sf = pdb_mod.source_file_for_rva(sess, frva, flen)
                    if sf:
                        entry["source_file"] = sf
                    try:
                        lines = sess.session.findLinesByRVA(frva, 1)
                        if lines.count > 0:
                            entry["start_line"] = int(lines.Item(0).lineNumber)
                    except Exception:
                        pass

                func_list.append(entry)
            except Exception:
                continue

        result["functions"] = func_list
        result["std_template_functions"] = std_funcs

        # Typedefs
        tds = target.findChildren(cvconst.SymTag.Typedef, None, 0)
        typedef_list = []
        std_typedefs = 0
        for i in range(tds.count):
            td = tds.Item(i)
            try:
                tname = td.name
                ttype = pdb_mod.type_name(td.type) if td.type else "?"
                if tname.startswith("std::") or "std::" in ttype:
                    std_typedefs += 1
                    continue
                typedef_list.append({"name": tname, "type": ttype})
            except Exception:
                pass
        result["typedefs"] = typedef_list
        result["std_typedefs"] = std_typedefs

        # Labels
        labels = target.findChildren(cvconst.SymTag.Label, None, 0)
        label_list = []
        for i in range(labels.count):
            lb = labels.Item(i)
            try:
                label_list.append({
                    "name": lb.name,
                    "rva": hex(int(lb.relativeVirtualAddress)),
                })
            except Exception:
                pass
        if label_list:
            result["labels"] = label_list

        # Source files
        try:
            src_files = set()
            for f_entry in func_list:
                sf = f_entry.get("source_file")
                if sf:
                    src_files.add(sf)
            result["source_files"] = sorted(src_files)
        except Exception:
            pass

        return result

    @mcp.tool()
    def list_compilands(pattern: str = "", alias: str | None = None) -> list[dict]:
        """List all compilands (obj files) in the PDB. Optionally filter by pattern."""
        sess = manager.get(alias)
        comps = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)
        results = []
        pat_lower = pattern.lower()
        for i in range(comps.count):
            c = comps.Item(i)
            name = c.name
            if pat_lower and pat_lower not in name.lower():
                continue

            funcs = c.findChildren(cvconst.SymTag.Function, None, 0)
            datas = c.findChildren(cvconst.SymTag.Data, None, 0)

            user_funcs = 0
            for j in range(funcs.count):
                try:
                    if not funcs.Item(j).name.startswith("std::"):
                        user_funcs += 1
                except Exception:
                    pass

            if user_funcs > 0 or datas.count > 0:
                results.append({
                    "name": name,
                    "functions": user_funcs,
                    "std_functions": funcs.count - user_funcs,
                    "data_symbols": datas.count,
                })
        return results

    @mcp.tool()
    def list_compilands_summary(path_filter: str = "", min_functions: int = 1,
                                sort_by: str = "functions", limit: int = 50,
                                alias: str | None = None) -> list[dict]:
        """List compilands filtered and sorted. Useful for finding compilation units and sizes."""
        sess = manager.get(alias)
        compilands = []
        comps = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)
        for i in range(comps.count):
            comp = comps.Item(i)
            name = comp.name or ""
            if path_filter and path_filter.lower() not in name.lower():
                continue
            funcs = comp.findChildren(cvconst.SymTag.Function, None, 0)
            nf = funcs.count if funcs else 0
            if nf < min_functions:
                continue
            data = comp.findChildren(cvconst.SymTag.Data, None, 0)
            nd = data.count if data else 0
            parts = name.replace("\\", "/").split("/")
            short = parts[-1] if parts else name
            compilands.append({
                "name": short,
                "full_path": name,
                "functions": nf,
                "data_symbols": nd,
            })

        key = {"functions": lambda x: x["functions"],
               "name": lambda x: x["name"],
               "data": lambda x: x["data_symbols"]}.get(sort_by, lambda x: x["functions"])
        compilands.sort(key=key, reverse=(sort_by != "name"))
        return compilands[:limit]

    @mcp.tool()
    def compiland_includes(obj_pattern: str, alias: str | None = None) -> dict:
        """List all source files referenced by a compiland.
        Useful for auto-generating #include directives."""
        sess = manager.get(alias)
        comps = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)
        target = None
        for i in range(comps.count):
            comp = comps.Item(i)
            if obj_pattern.lower() in (comp.name or "").lower():
                target = comp
                break
        if not target:
            return {"error": f"Compiland matching '{obj_pattern}' not found"}

        source_files: list[str] = []
        try:
            sf_enum = sess.session.findFile(target, None, 0)
            if sf_enum:
                for i in range(sf_enum.count):
                    sf = sf_enum.Item(i)
                    fname = sf.fileName if hasattr(sf, 'fileName') else str(sf)
                    if fname:
                        source_files.append(fname)
        except Exception:
            # Fallback: collect from line info
            funcs = target.findChildren(cvconst.SymTag.Function, None, 0)
            seen: set[str] = set()
            if funcs:
                for i in range(min(funcs.count, 200)):
                    f = funcs.Item(i)
                    try:
                        rva = int(f.relativeVirtualAddress)
                        length = int(f.length)
                        if rva and length:
                            lines = sess.session.findLinesByRVA(rva, min(length, 4))
                            if lines and lines.count > 0:
                                sf = lines.Item(0).sourceFile.fileName
                                if sf and sf not in seen:
                                    seen.add(sf)
                                    source_files.append(sf)
                    except Exception:
                        pass

        headers = [f for f in source_files if f.lower().endswith(('.h', '.hpp', '.inl'))]
        sources = [f for f in source_files if f.lower().endswith(('.cpp', '.c', '.cxx'))]
        other = [f for f in source_files if f not in headers and f not in sources]

        return {
            "compiland": target.name,
            "headers": sorted(headers),
            "sources": sorted(sources),
            "other": sorted(other),
            "total": len(source_files),
        }

    @mcp.tool()
    def inline_functions(obj_pattern: str, alias: str | None = None) -> dict:
        """Detect functions in a compiland that are likely inlined (defined in headers).
        Functions whose source file is a .h/.inl are probably inline/header-only."""
        sess = manager.get(alias)
        comps = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)
        target = None
        for i in range(comps.count):
            comp = comps.Item(i)
            if obj_pattern.lower() in (comp.name or "").lower():
                target = comp
                break
        if not target:
            return {"error": f"Compiland matching '{obj_pattern}' not found"}

        funcs = target.findChildren(cvconst.SymTag.Function, None, 0)
        if not funcs:
            return {"compiland": target.name, "inline": [], "out_of_line": []}

        inline_funcs = []
        ool_funcs = []

        for i in range(funcs.count):
            f = funcs.Item(i)
            try:
                rva = int(f.relativeVirtualAddress)
                length = int(f.length)
                name = f.name or ""
                source_file = ""
                if rva and length:
                    lines = sess.session.findLinesByRVA(rva, min(length, 4))
                    if lines and lines.count > 0:
                        try:
                            source_file = lines.Item(0).sourceFile.fileName
                        except Exception:
                            pass

                entry = {"name": name, "rva": hex(rva), "length": length}
                if source_file:
                    entry["source_file"] = source_file

                if source_file and any(source_file.lower().endswith(ext) for ext in ('.h', '.hpp', '.inl')):
                    inline_funcs.append(entry)
                else:
                    ool_funcs.append(entry)
            except Exception:
                pass

        return {
            "compiland": target.name,
            "inline_count": len(inline_funcs),
            "out_of_line_count": len(ool_funcs),
            "inline": inline_funcs,
            "out_of_line": ool_funcs,
        }
