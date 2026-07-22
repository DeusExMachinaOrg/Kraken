"""Persifal — enriched Capstone decoder.

All disasm tools route through the persifal pipeline:
  decode (Capstone → IR) → enrich (PDB types, fields, symbols, locals)

Output renderers:
  disasm_typed      — enriched IR as annotated asm lines
  disasm_compressed — token-minimal semantic stream for LLM
  disasm_detailed   — source-line groups + locals + calls (in tools_disasm.py)
  disasm_raw        — raw Capstone, no PDB (in tools_pe.py)
"""

from __future__ import annotations


def register(mcp):
    """Register persifal tools on the MCP instance."""

    from .._state import manager
    from .. import _pdb as pdb_mod

    def _run_pipeline(name_or_rva: str, alias: str | None = None):
        """Common pipeline: resolve → decode → enrich. Returns tuple or error string."""
        sess = manager.get(alias)
        func_rva, func_len, func_name = pdb_mod.resolve_function(
            sess, name_or_rva)
        if func_rva == 0:
            return f"Function '{name_or_rva}' not found"
        if func_len == 0:
            return f"Function '{name_or_rva}' has zero length"

        from ._decode import decode
        from ._enrich import enrich, build_known

        tp = _get_typeprovider(sess)

        this_class = ""
        if "::" in func_name:
            this_class = func_name.rsplit("::", 1)[0]

        known = build_known(tp, func_rva)
        stream = decode(sess, func_rva, func_len)
        stream = enrich(tp, stream, this_class=this_class, known=known)

        return sess, tp, stream, known, func_rva, func_len, func_name, this_class

    @mcp.tool()
    def disasm_typed(name_or_rva: str, max_nodes: int = 500,
                     alias: str | None = None) -> list[str]:
        """Enriched disassembly with PDB types, fields, symbols, strings.

        Like disasm but with full type annotations from the enricher:
        field names, resolved vtable calls, string literals, stack locals.
        Each line: RVA KIND mnemonic operands [type/field annotations].
        """
        result = _run_pipeline(name_or_rva, alias)
        if isinstance(result, str):
            return [result]

        sess, tp, stream, known, func_rva, func_len, func_name, this_class = result
        from ._ir import Kind

        lines = [f"; === {func_name} (RVA {hex(func_rva)}, {func_len} bytes) ==="]

        for nd in stream._nodes:
            if len(lines) >= max_nodes:
                lines.append(f"; ... truncated at {max_nodes} nodes")
                break

            mn = nd.mn.value if nd.mn else "?"
            kind = nd.kind.value[0] if nd.kind else "?"  # P/L/E/I

            parts = []
            for op in nd.ops:
                p = []
                if op.string is not None:
                    s = op.string
                    if len(s) > 40:
                        s = s[:37] + "..."
                    p.append(f'"{s}"')
                elif op.field and op.kind == "mem" and op.mem_base:
                    p.append(f"{op.mem_base}.{op.field}")
                elif op.symbol:
                    p.append(op.symbol)
                elif op.kind == "reg":
                    p.append(op.reg or "?")
                elif op.kind == "imm":
                    v = op.imm
                    p.append(str(v) if isinstance(v, int) and -16 <= v <= 256 else hex(v or 0))
                elif op.kind == "mem":
                    base = op.mem_base or ""
                    disp = f"+{hex(op.mem_disp)}" if op.mem_disp else ""
                    p.append(f"[{base}{disp}]")

                # Type annotation
                if op.sym and hasattr(op.sym, 'short_name') and op.sym.short_name:
                    sn = op.sym.short_name
                    if sn not in ("void", "int", "unsigned int"):
                        p.append(f":{sn}")
                if op.field and not (op.kind == "mem" and op.mem_base):
                    p.append(f".{op.field}")

                parts.append("".join(p))

            ops_str = ", ".join(parts)
            lex = f" L{nd.lexical}" if nd.lexical >= 0 else ""
            lines.append(f"  {hex(nd.rva)} {kind} {mn:6s} {ops_str}{lex}")

        return lines

    @mcp.tool()
    def disasm_compressed(name_or_rva: str,
                          alias: str | None = None) -> str:
        """Token-compressed semantic disassembly for LLM reconstruction.

        Runs decode → enrich → compress pipeline. Output structure:
          ## jumpmap   — control flow edges (forward/back/switch)
          ## inlinee   — candidate inline regions (hints for LLM)
          ## stream    — compressed semantic actions (calls, fields, flow)

        Enricher resolves: field names, symbols, types, vtable calls,
        string literals, stack locals. Compressor drops register mechanics.

        Returns compact multi-line text (~3-5x fewer tokens than raw disasm).
        """
        result = _run_pipeline(name_or_rva, alias)
        if isinstance(result, str):
            return result

        sess, tp, stream, known, func_rva, func_len, func_name, this_class = result

        from ._jumpmap import build_jumpmap
        from ._lexical import LexicalMap
        from ._inlinee import detect_inlinees, build_hierarchy
        from ._compress import compress

        jm = build_jumpmap(stream, sess=sess)
        lm = LexicalMap(list(stream._nodes))

        marks = None
        if this_class:
            hierarchy = build_hierarchy(tp, this_class)
            marks = detect_inlinees(
                stream, lm, hierarchy=hierarchy,
                this_class=this_class, provider=tp)

        return compress(stream, known=known,
                        func_name=func_name, func_len=func_len,
                        jm=jm, inlinee_marks=marks)

    @mcp.tool()
    def rebuild_cache(purge: bool = False,
                      alias: str | None = None) -> str:
        """Rebuild the enrichment cache (decode+enrich all PDB functions).

        SQLite-backed — skips already-cached functions unless purge=True.
        Cache is used for inlinee fingerprinting in disasm_compressed.

        purge=True: drop existing cache first, then rescan everything.
        purge=False (default): incremental — only process uncached functions.
        """
        sess = manager.get(alias)
        tp = _get_typeprovider(sess)

        from ._analyzer import Analyzer

        az = _get_analyzer(sess, tp)

        if purge:
            dropped = az.purge_cache()
        else:
            dropped = 0

        done = az.scan_all()

        parts = [f"Cache: {done} functions"]
        if purge:
            parts.append(f"(purged {dropped})")
        stats = az.stats()
        parts.append(f"in_memory={stats['in_memory']} in_db={stats['in_db']}")
        return " | ".join(parts)


# ── Singletons ───────────────────────────────────────────────────────

_tp_cache: dict[str, object] = {}
_az_cache: dict[str, object] = {}


def _get_typeprovider(sess):
    """Get or create TypeProvider for a session (cached by PDB path)."""
    key = sess.pdb_path
    if key in _tp_cache:
        return _tp_cache[key]

    from ._typedb import TypeDB
    from ._typeprovider import TypeProvider, _IAT

    db_path = sess.pdb_path.replace(".pdb", ".persifal.db")
    db = TypeDB(db_path)
    tp = TypeProvider(db)
    tp.preheat()
    tp.annotate_batch(_IAT)
    _tp_cache[key] = tp
    return tp


def _get_analyzer(sess, tp):
    """Get or create Analyzer for a session (cached by PDB path)."""
    key = sess.pdb_path
    if key in _az_cache:
        return _az_cache[key]

    from ._analyzer import Analyzer

    az = Analyzer(sess, provider=tp)
    _az_cache[key] = az
    return az
