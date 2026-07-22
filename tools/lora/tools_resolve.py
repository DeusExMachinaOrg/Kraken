"""Symbol resolution tools: resolve, demangle, missing_symbols, batch_symbol_at_rva."""

from __future__ import annotations

from typing import Any

from pydia2 import cvconst

from ._state import manager
from . import _pe as pe_mod
from . import _pdb as pdb_mod
from . import _util


def register(mcp):
    """Register resolve/demangle tools on the given FastMCP instance."""

    @mcp.tool()
    def resolve(rvas: list[str], alias: str | None = None) -> list[dict]:
        """Batch resolve RVA hex strings to symbol names. Fast (cached)."""
        sess = manager.get(alias)
        results = []
        for rva_str in rvas:
            rva = int(rva_str, 16)
            name = pdb_mod.sym_at_rva(sess, rva) or pe_mod.import_at_rva(sess.iat_map, rva)
            results.append({"rva": rva_str, "name": name or "???"})
        return results

    @mcp.tool()
    def demangle(names: list[str]) -> list[dict]:
        """Batch demangle MSVC decorated symbol names using dbghelp.dll."""
        results = []
        for name in names:
            demangled = _util.demangle_name(name)
            if demangled:
                results.append({"decorated": name, "demangled": demangled})
            else:
                results.append({"decorated": name, "error": "failed"})
        return results

    @mcp.tool()
    def batch_symbol_at_rva(rvas: list[int], alias: str | None = None) -> list[dict]:
        """Resolve multiple RVAs to symbols at once."""
        sess = manager.get(alias)
        results = []
        for rva in rvas:
            try:
                sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.Null)
                if sym:
                    info = pdb_mod.symbol_info(sym)
                    info["query_rva"] = hex(rva)
                    results.append(info)
                else:
                    results.append({"query_rva": hex(rva), "error": "not found"})
            except Exception as e:
                results.append({"query_rva": hex(rva), "error": str(e)})
        return results

    @mcp.tool()
    def missing_symbols(mangled_names: list[str], alias: str | None = None) -> list[dict]:
        """Resolve mangled/decorated symbol names to PDB info.
        Useful for mapping linker 'unresolved external' errors to RVAs."""
        sess = manager.get(alias)
        results = []
        for mangled in mangled_names:
            entry: dict[str, Any] = {"mangled": mangled}
            demangled = _util.demangle_name(mangled)
            if demangled:
                entry["demangled"] = demangled
            try:
                pub_enum = sess.global_scope.findChildren(
                    cvconst.SymTag.PublicSymbol, mangled, 0
                )
                if pub_enum and pub_enum.count > 0:
                    sym = pub_enum.Item(0)
                    entry["rva"] = hex(int(sym.relativeVirtualAddress))
                    entry["found"] = True
                else:
                    entry["found"] = False
            except Exception:
                entry["found"] = False
            results.append(entry)
        return results

    @mcp.tool()
    def globals_by_pattern(pattern: str, max_results: int = 50,
                           alias: str | None = None) -> list[dict]:
        """Find global/static data symbols by name pattern (DIA wildcards * ?)."""
        sess = manager.get(alias)
        from ._constants import NSF_REGEX
        enum = sess.global_scope.findChildren(cvconst.SymTag.Data, pattern, NSF_REGEX)
        results = []
        for i in range(min(enum.count, max_results)):
            sym = enum.Item(i)
            entry: dict[str, Any] = {}
            try:
                entry["name"] = sym.name
            except Exception:
                pass
            try:
                entry["rva"] = hex(int(sym.relativeVirtualAddress))
            except Exception:
                pass
            try:
                entry["type"] = pdb_mod.type_name(sym.type)
            except Exception:
                pass
            try:
                entry["length"] = int(sym.length)
            except Exception:
                pass
            results.append(entry)
        return results
