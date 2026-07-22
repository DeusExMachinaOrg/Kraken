"""Token compressor: enriched IR → minimal deterministic text for LLM.

Output structure:
  ## jumpmap        — control flow edges (forward/back/switch)
  ## inlinee        — candidate regions with confidence (LLM decides)
  ## stream         — compressed semantic IR (calls, fields, control flow)

Enricher already resolved: fields, symbols, types, strings, vframe locals.
This pass drops register/stack mechanics, emits only semantic actions.
"""

from __future__ import annotations
from ._ir import (
    IR, IROp, Mn, Kind,
    IRMov, IRPush, IRPop, IRCall, IRCmp, IRJcc, IRJmp, IRRet, IRArith, IRNop,
)
from ._stream import IRStream
from ._vm import Stack
from ._jumpmap import JumpMap


# ── Operand → compact string ────────────────────────────────────────

def _resolve(op: IROp, known: Stack | None = None) -> str:
    """Operand → shortest semantic string. '' if unresolvable."""
    if op.string is not None:
        s = op.string
        if len(s) > 50:
            s = s[:47] + "..."
        return f'"{s}"'
    if op.field and op.kind == "mem" and op.mem_base:
        base = _base(op, known)
        return f"{base}.{op.field}"
    if op.symbol:
        return op.symbol
    if op.vframe is not None and op.kind == "mem" and known:
        try:
            ref = known[op.vframe]
            return ref.var_name or f"v{op.vframe}"
        except KeyError:
            return f"v{op.vframe}"
    if op.field:
        return op.field
    if op.kind == "imm":
        v = op.imm
        if v is None:
            return "0"
        if isinstance(v, int):
            return str(v) if -16 <= v <= 256 else hex(v)
        return str(v)
    return ""


def _base(op: IROp, known: Stack | None = None) -> str:
    if not op.mem_base:
        return hex(op.mem_disp) if op.mem_disp else "?"
    if op.mem_base in ("esp", "ebp") and op.vframe is not None and known:
        try:
            ref = known[op.vframe]
            return ref.var_name or f"v{ref.base}"
        except KeyError:
            pass
    return op.mem_base


def _type_short(op: IROp) -> str:
    if op.sym is None:
        return ""
    n = getattr(op.sym, 'short_name', "") or ""
    if n in ("", "void", "int", "unsigned int"):
        return ""
    return n


# ── Section 1: Jump map ─────────────────────────────────────────────

def _emit_jumpmap(jm: JumpMap) -> list[str]:
    """Compact jump map: one line per edge."""
    lines = ["## jumpmap"]
    for e in sorted(jm.edges, key=lambda e: e.source_rva):
        mn = e.mn.value if e.mn else "jmp"
        tag = ""
        if e.is_backward:
            tag = " back"
        elif not e.is_conditional:
            tag = " jmp"
        lines.append(f"{hex(e.source_rva)}→{hex(e.target_rva)} {mn}{tag}")

    for sw in jm.switches:
        cases = []
        for handler_rva, vals in sorted(sw.handlers.items()):
            vals_str = ",".join(str(v) for v in vals)
            cases.append(f"{vals_str}:{hex(handler_rva)}")
        lines.append(f"switch {hex(sw.jmp_rva)} default={hex(sw.default_rva)} [{' '.join(cases)}]")

    return lines


# ── Section 2: Inlinee candidates ────────────────────────────────────

def _emit_inlinees(marks: list | None) -> list[str]:
    """Inlinee candidates as hints — LLM decides what to collapse."""
    if not marks:
        return []
    lines = ["## inlinee"]
    for m in marks:
        sz = m.end - m.begin
        lines.append(f"{hex(m.begin)}-{hex(m.end)} {sz}B {m.target} owner={m.owner_class}")
    return lines


# ── Section 3: Compressed stream ─────────────────────────────────────

class _Ctx:
    """Accumulates context, emits only semantic actions."""
    def __init__(self, known: Stack | None = None):
        self.known = known
        self.push_args: list[str] = []
        self.cmp_ops: list[str] = []
        self.lines: list[str] = []
        self.prev_lex: int = -1
        self._reg: dict[str, str] = {}

    def _prefix(self, lex: int) -> str:
        if lex >= 0 and lex != self.prev_lex:
            self.prev_lex = lex
            return f"@{lex} "
        return "  "

    def emit(self, lex: int, text: str):
        self.lines.append(f"{self._prefix(lex)}{text}")

    def track_reg(self, reg: str, val: str):
        if val:
            self._reg[reg] = val

    def get_reg(self, reg: str) -> str:
        return self._reg.get(reg, reg)

    def process(self, nd: IR):
        lex = nd.lexical if nd.lexical >= 0 else -1

        if nd.kind in (Kind.PROLOGUE, Kind.EPILOGUE):
            if isinstance(nd, IRRet):
                self.emit(lex, "ret")
            return

        if isinstance(nd, IRNop):
            return

        if isinstance(nd, IRPush):
            if nd.ops:
                val = _resolve(nd.ops[0], self.known)
                if not val:
                    val = self.get_reg(nd.ops[0].reg) if nd.ops[0].reg else "?"
                self.push_args.append(val)
            return

        if isinstance(nd, IRPop):
            return

        if isinstance(nd, IRCall):
            self._emit_call(nd, lex)
            return

        if isinstance(nd, IRCmp):
            if len(nd.ops) >= 2:
                lhs = _resolve(nd.ops[0], self.known) or self.get_reg(nd.ops[0].reg or "")
                rhs = _resolve(nd.ops[1], self.known) or self.get_reg(nd.ops[1].reg or "")
                self.cmp_ops = [lhs, rhs]
            return

        if isinstance(nd, IRJcc):
            mn = nd.mn.value if nd.mn else "jcc"
            cond = f"{self.cmp_ops[0]},{self.cmp_ops[1]}" if len(self.cmp_ops) >= 2 else "flags"
            self.emit(lex, f"{mn} {cond} →{hex(nd.target_rva)}")
            return

        if isinstance(nd, IRJmp):
            self.emit(lex, f"jmp →{hex(nd.target_rva)}")
            return

        if isinstance(nd, IRRet):
            self.emit(lex, "ret")
            return

        if isinstance(nd, IRMov):
            self._emit_mov(nd, lex)
            return

        if isinstance(nd, IRArith):
            self._emit_arith(nd, lex)
            return

    def _emit_call(self, nd: IRCall, lex: int):
        if not nd.ops:
            return
        target = _resolve(nd.ops[0], self.known)
        if not target:
            op = nd.ops[0]
            base = self.get_reg(op.mem_base) if op.mem_base else "?"
            if op.mem_disp:
                target = f"[{base}+{hex(op.mem_disp)}]"
            else:
                target = f"[{base}]"

        args = ", ".join(reversed(self.push_args)) if self.push_args else ""
        self.push_args.clear()

        ret = _type_short(nd.ops[0])
        sfx = f":{ret}" if ret else ""

        self.emit(lex, f"{target}({args}){sfx}" if args else f"{target}(){sfx}")

        self._reg.pop("eax", None)
        self._reg.pop("ecx", None)
        self._reg.pop("edx", None)

    def _emit_mov(self, nd: IRMov, lex: int):
        if len(nd.ops) < 2:
            return
        dst, src = nd.ops[0], nd.ops[1]
        dst_str = _resolve(dst, self.known)
        src_str = _resolve(src, self.known)

        # Register destination: track silently
        if dst.kind == "reg":
            val = src_str or self.get_reg(src.reg or "")
            typ = _type_short(src) or _type_short(dst)
            if typ and val:
                val = f"{val}:{typ}"
            if nd.mn == Mn.LEA and src_str:
                val = f"&{src_str}"
            self.track_reg(dst.reg, val)
            return

        # Field write → emit
        if dst_str and "." in dst_str:
            if not src_str:
                src_str = self.get_reg(src.reg or "") if src.kind == "reg" else "?"
            self.emit(lex, f"{dst_str}={src_str}")
            return

        # Local write → emit
        if dst.vframe is not None and dst_str:
            if not src_str:
                src_str = self.get_reg(src.reg or "") if src.kind == "reg" else "?"
            self.emit(lex, f"{dst_str}={src_str}")
            return

    def _emit_arith(self, nd: IRArith, lex: int):
        if not nd.ops:
            return
        dst = nd.ops[0]
        dst_str = _resolve(dst, self.known)

        # Field/local arith → emit
        if dst_str and ("." in dst_str or dst.vframe is not None):
            if len(nd.ops) >= 2:
                src_str = _resolve(nd.ops[1], self.known) or "?"
                ops = {Mn.ADD: "+=", Mn.SUB: "-=", Mn.AND: "&=", Mn.OR: "|=", Mn.XOR: "^="}
                self.emit(lex, f"{dst_str} {ops.get(nd.mn, '?=')} {src_str}")
            elif nd.mn in (Mn.INC, Mn.DEC):
                self.emit(lex, f"{dst_str}{'++' if nd.mn == Mn.INC else '--'}")
            return

        # Register: track silently
        if dst.kind == "reg" and dst.reg:
            if nd.mn == Mn.XOR and len(nd.ops) >= 2 and nd.ops[1].reg == dst.reg:
                self.track_reg(dst.reg, "0")
            elif len(nd.ops) >= 2:
                old = self.get_reg(dst.reg)
                src = _resolve(nd.ops[1], self.known) or "?"
                op_map = {Mn.ADD: "+", Mn.SUB: "-", Mn.IMUL: "*", Mn.SHL: "<<", Mn.SHR: ">>"}
                self.track_reg(dst.reg, f"({old}{op_map.get(nd.mn, '?')}{src})")


# ── Main entry ───────────────────────────────────────────────────────

def compress(stream: IRStream, known: Stack | None = None,
             func_name: str = "", func_len: int = 0,
             jm: JumpMap | None = None,
             inlinee_marks: list | None = None) -> str:
    """Compress enriched IR to minimal deterministic text for LLM.

    Structure:
      ## jumpmap         — control flow edges
      ## inlinee         — candidate inline regions (hints)
      ## stream          — compressed semantic actions
    """
    out: list[str] = []

    if func_name:
        out.append(f"# {func_name} ({func_len}B)")

    # Jump map
    if jm is not None:
        out.extend(_emit_jumpmap(jm))

    # Inlinee candidates
    out.extend(_emit_inlinees(inlinee_marks))

    # Compressed stream
    out.append("## stream")
    ctx = _Ctx(known)
    for nd in stream._nodes:
        ctx.process(nd)
    out.extend(ctx.lines)

    return "\n".join(out)
