"""Coverage and source mapping tools."""

from __future__ import annotations

import glob
import os
from typing import Any

from pydia2 import cvconst

from ._state import manager
from . import _pdb as pdb_mod


def register(mcp):
    """Register coverage/mapping tools on the given FastMCP instance."""

    @mcp.tool()
    def source_map(type_names: list[str], alias: str | None = None) -> list[dict]:
        """For each type name, find the original source file path from PDB line info."""
        sess = manager.get(alias)
        results = []
        for tn in type_names:
            sf = pdb_mod.source_file_for_type(sess, tn)
            results.append({"type": tn, "source_file": sf})
        return results

    @mcp.tool()
    def coverage_report(source_dir: str = "source", alias: str | None = None) -> dict:
        """Compare reconstructed source against PDB compilands to measure coverage.
        Scans source directory for .cpp files and matches them against PDB compilands."""
        sess = manager.get(alias)

        abs_dir = os.path.abspath(source_dir)
        our_cpps = set()
        for f in glob.glob(os.path.join(abs_dir, "**/*.cpp"), recursive=True):
            base = os.path.basename(f).lower().replace(".cpp", "")
            our_cpps.add(base)

        compilands = sess.global_scope.findChildren(cvconst.SymTag.Compiland, None, 0)

        matched = []
        unmatched = []
        total_funcs = 0
        matched_funcs = 0

        for i in range(compilands.count):
            comp = compilands.Item(i)
            name = comp.name or ""
            short = name.replace("\\", "/").split("/")[-1].lower().replace(".obj", "")

            funcs = comp.findChildren(cvconst.SymTag.Function, None, 0)
            nf = funcs.count if funcs else 0
            if nf == 0:
                continue

            total_funcs += nf
            entry = {"name": short, "functions": nf, "full_path": name}

            if short in our_cpps:
                matched.append(entry)
                matched_funcs += nf
            else:
                unmatched.append(entry)

        unmatched.sort(key=lambda x: -x["functions"])

        return {
            "total_compilands": len(matched) + len(unmatched),
            "reconstructed_compilands": len(matched),
            "total_functions": total_funcs,
            "reconstructed_functions": matched_funcs,
            "coverage_percent": round(matched_funcs * 100 / max(total_funcs, 1), 1),
            "matched": sorted(matched, key=lambda x: -x["functions"]),
            "top_missing": unmatched[:30],
        }
