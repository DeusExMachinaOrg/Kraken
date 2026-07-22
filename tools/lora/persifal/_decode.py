"""Decode: PE bytes → IRStream.

Mechanical Capstone disassembly + static PDB queries per instruction.
Asks PDB for what it can resolve without state tracking:
  - source line for each RVA
  - call target → symbol name
  - immediate → symbol name or string literal
  - absolute memory address → global symbol

Does NOT track registers, stack, or scopes — that's fold + VM.
"""

from __future__ import annotations

from capstone import Cs, CS_ARCH_X86, CS_MODE_32, x86_const

from .._state import Session
from .. import _pdb as pdb_mod
from .. import _pe as pe_mod
from ._ir import (
    IR, IROp, Mn, Kind, classify,
    IRMov, IRPush, IRPop, IRCall, IRArith, IRJcc, IRJmp, IRRet,
)
from ._stream import IRStream


def decode(sess: Session, func_rva: int, func_len: int) -> IRStream:
    """Decode function bytes into a frozen IRStream.

    Each instruction becomes one classified IR node.
    Operands carry static PDB enrichments (no state tracking).
    """
    image_base = sess.image_base
    line_map = _build_line_map(sess, func_rva, func_len)

    code = sess.pe.get_data(func_rva, func_len)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    nodes: list[IR] = []
    current_lex = -1

    for insn in md.disasm(code, image_base + func_rva):
        rva = insn.address - image_base

        # Source line
        new_line = line_map.get(rva, 0)
        if new_line:
            current_lex = new_line

        # Classify
        cls, mn = classify(insn.mnemonic)

        # Extract operands
        ops = [_make_op(insn, cap_op) for cap_op in insn.operands]

        # Build node
        if cls is IRJcc:
            target = _jump_target(ops, image_base)
            node = IRJcc(mn=mn, rva=rva, lexical=current_lex,
                         ops=ops, target_rva=target)
        elif cls is IRJmp:
            target = _jump_target(ops, image_base)
            node = IRJmp(mn=mn, rva=rva, lexical=current_lex,
                         ops=ops, target_rva=target)
        elif cls is IRCall:
            cleanup = 0
            if ops and ops[0].kind == "imm":
                target_rva = ops[0].imm - image_base
                if target_rva > 0:
                    cleanup = _scan_ret_cleanup(sess, target_rva)
            node = IRCall(mn=mn, rva=rva, lexical=current_lex,
                          ops=ops, cleanup=cleanup)
        elif cls is IRRet:
            cleanup = ops[0].imm if ops and ops[0].kind == "imm" else 0
            node = IRRet(mn=mn, rva=rva, lexical=current_lex,
                         ops=ops, cleanup=cleanup)
        else:
            node = cls(mn=mn, rva=rva, lexical=current_lex, ops=ops)

        # Static PDB enrichment
        _enrich(sess, node)

        nodes.append(node)

    # Tag prologue/epilogue
    _tag_kinds(nodes)

    return IRStream(nodes)


# ── Prologue / epilogue detection ────────────────────────────────────

_CALLEE_SAVED = {"ebx", "esi", "edi", "ebp"}


def _tag_kinds(nodes: list[IR]) -> None:
    """Detect and tag prologue/epilogue nodes. Everything else stays PAYLOAD."""
    if not nodes:
        return

    i = 0
    n = len(nodes)

    # ── Prologue ─────────────────────────────────────────────────
    # Pattern 1: push ebp / mov ebp, esp / sub esp, N / push regs
    # Pattern 2: sub esp, N / push regs  (FPO, no frame pointer)

    # push ebp
    if (i < n and isinstance(nodes[i], IRPush)
            and nodes[i].ops and nodes[i].ops[0].reg == "ebp"):
        nodes[i].kind = Kind.PROLOGUE
        i += 1
        # mov ebp, esp
        if (i < n and isinstance(nodes[i], IRMov)
                and nodes[i].mn == Mn.MOV
                and len(nodes[i].ops) >= 2
                and nodes[i].ops[0].reg == "ebp"
                and nodes[i].ops[1].reg == "esp"):
            nodes[i].kind = Kind.PROLOGUE
            i += 1

    # sub esp, N
    if (i < n and isinstance(nodes[i], IRArith)
            and nodes[i].mn == Mn.SUB
            and len(nodes[i].ops) >= 2
            and nodes[i].ops[0].kind == "reg"
            and nodes[i].ops[0].reg == "esp"
            and nodes[i].ops[1].kind == "imm"):
        nodes[i].kind = Kind.PROLOGUE
        i += 1

    # push callee-saved regs
    while (i < n and isinstance(nodes[i], IRPush)
           and nodes[i].ops
           and nodes[i].ops[0].kind == "reg"
           and nodes[i].ops[0].reg in _CALLEE_SAVED):
        nodes[i].kind = Kind.PROLOGUE
        i += 1

    # ── Epilogue ─────────────────────────────────────────────────
    # Walk backwards from each ret
    for idx in range(n):
        if not isinstance(nodes[idx], IRRet):
            continue

        nodes[idx].kind = Kind.EPILOGUE
        j = idx - 1

        # pop ebp
        if (j >= 0 and isinstance(nodes[j], IRPop)
                and nodes[j].ops and nodes[j].ops[0].reg == "ebp"):
            nodes[j].kind = Kind.EPILOGUE
            j -= 1

        # add esp, N  or  mov esp, ebp
        if (j >= 0 and isinstance(nodes[j], IRArith)
                and nodes[j].mn == Mn.ADD
                and len(nodes[j].ops) >= 2
                and nodes[j].ops[0].kind == "reg"
                and nodes[j].ops[0].reg == "esp"
                and nodes[j].ops[1].kind == "imm"):
            nodes[j].kind = Kind.EPILOGUE
            j -= 1
        elif (j >= 0 and isinstance(nodes[j], IRMov)
              and nodes[j].mn == Mn.MOV
              and len(nodes[j].ops) >= 2
              and nodes[j].ops[0].reg == "esp"
              and nodes[j].ops[1].reg == "ebp"):
            nodes[j].kind = Kind.EPILOGUE
            j -= 1

        # pop callee-saved regs
        while (j >= 0 and isinstance(nodes[j], IRPop)
               and nodes[j].ops
               and nodes[j].ops[0].kind == "reg"
               and nodes[j].ops[0].reg in _CALLEE_SAVED):
            nodes[j].kind = Kind.EPILOGUE
            j -= 1


# ── PDB line map ─────────────────────────────────────────────────────

def _build_line_map(sess: Session, func_rva: int, func_len: int) -> dict[int, int]:
    """RVA → source line number from PDB."""
    result: dict[int, int] = {}
    try:
        lines = sess.session.findLinesByRVA(func_rva, func_len)
        for i in range(min(lines.count, 500)):
            le = lines.Item(i)
            result[int(le.relativeVirtualAddress)] = int(le.lineNumber)
    except Exception:
        pass
    return result


# ── Operand construction ─────────────────────────────────────────────

def _make_op(insn, cap_op) -> IROp:
    """Convert Capstone operand to IROp."""
    op = IROp(size=cap_op.size)

    if cap_op.type == x86_const.X86_OP_REG:
        op.kind = "reg"
        op.reg = insn.reg_name(cap_op.reg)

    elif cap_op.type == x86_const.X86_OP_IMM:
        op.kind = "imm"
        op.imm = cap_op.imm

    elif cap_op.type == x86_const.X86_OP_MEM:
        op.kind = "mem"
        m = cap_op.mem
        op.mem_base = insn.reg_name(m.base) if m.base else ""
        op.mem_index = insn.reg_name(m.index) if m.index else ""
        op.mem_scale = m.scale
        op.mem_disp = m.disp
        # Segment prefix (fs:[0] → SEH, gs:[0] → TLS)
        if not op.mem_base and m.segment:
            op.mem_base = insn.reg_name(m.segment)

    return op


# ── Static enrichment (no state tracking) ────────────────────────────

def _enrich(sess: Session, node: IR) -> None:
    """Enrich operands with what PDB can resolve statically."""
    image_base = sess.image_base

    for op in node.ops:
        if op.kind == "imm" and node.mn == Mn.CALL:
            _enrich_call_target(sess, op, image_base)
        elif op.kind == "imm":
            _enrich_imm(sess, op, image_base)
        elif op.kind == "mem" and not op.mem_base and op.mem_disp:
            _enrich_absolute(sess, op, image_base)


def _enrich_call_target(sess: Session, op: IROp, image_base: int) -> None:
    """Resolve direct call target to symbol name."""
    rva = op.imm - image_base
    if rva > 0:
        name = pdb_mod.sym_at_rva(sess, rva)
        if name:
            op.symbol = name


def _enrich_imm(sess: Session, op: IROp, image_base: int) -> None:
    """Resolve immediate as symbol or string literal."""
    rva = op.imm - image_base
    if rva <= 0:
        return
    name = pdb_mod.sym_at_rva(sess, rva)
    if name:
        op.symbol = name
        # ??_C@ mangled string constants: read with relaxed length (len >= 0)
        if name.startswith("??_C@"):
            try:
                data = sess.pe.get_data(rva, 128)
                null = data.find(b"\x00")
                if null >= 0:
                    data = data[:null]
                text = data.decode("ascii", errors="strict")
                if all(0x20 <= ord(c) < 0x7F for c in text):
                    op.string = text
                    op.symbol = ""  # string wins over mangled name
                    return
                elif len(data) == 0:
                    # Empty string constant ""
                    op.string = ""
                    op.symbol = ""
                    return
            except Exception:
                pass
    s = pe_mod.read_cstring(sess.pe, rva)
    if s:
        op.string = s


def _enrich_absolute(sess: Session, op: IROp, image_base: int) -> None:
    """Resolve [absolute_addr] — global symbol or IAT import."""
    rva = op.mem_disp - image_base if op.mem_disp > image_base else op.mem_disp
    if rva <= 0:
        return
    # IAT import?
    imp = pe_mod.import_at_rva(sess.iat_map, rva)
    if imp:
        op.symbol = imp
        return
    # Global symbol?
    name = pdb_mod.sym_at_rva(sess, rva)
    if name:
        op.symbol = name
        return
    # String?
    s = pe_mod.read_cstring(sess.pe, rva)
    if s:
        op.string = s


# ── Helpers ──────────────────────────────────────────────────────────

def _jump_target(ops: list[IROp], image_base: int) -> int:
    """Extract jump target RVA from operands."""
    if ops and ops[0].kind == "imm":
        return ops[0].imm - image_base
    return 0


_ret_cache: dict[int, int] = {}

def _scan_ret_cleanup(sess: Session, target_rva: int) -> int:
    """Scan callee for ret N. Returns cleanup bytes (0 for cdecl).

    Follows direct jmp thunks. For virtual thunks (jmp [reg+disp]),
    falls back to PDB parameter count.
    """
    if target_rva in _ret_cache:
        return _ret_cache[target_rva]
    if target_rva <= 0:
        _ret_cache[target_rva] = 0
        return 0
    try:
        code = sess.pe.get_data(target_rva, min(2048, 4096))
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        for insn in md.disasm(code, sess.image_base + target_rva):
            if insn.mnemonic in ("ret", "retn"):
                if insn.op_str:
                    try:
                        n = int(insn.op_str, 0)
                        _ret_cache[target_rva] = n
                        return n
                    except ValueError:
                        pass
                _ret_cache[target_rva] = 0
                return 0
            if insn.mnemonic == "jmp":
                # Direct jmp thunk: follow the target
                if insn.operands and insn.operands[0].type == x86_const.X86_OP_IMM:
                    jmp_target = insn.operands[0].imm - sess.image_base
                    if jmp_target > 0:
                        result = _scan_ret_cleanup(sess, jmp_target)
                        _ret_cache[target_rva] = result
                        return result
                # Virtual/indirect thunk (jmp [reg+disp]): can't follow
                # Fall back to PDB param count
                break
    except Exception:
        pass

    # PDB fallback: count parameters from function type info
    result = _pdb_cleanup(sess, target_rva)
    _ret_cache[target_rva] = result
    return result


def _pdb_cleanup(sess: Session, rva: int) -> int:
    """Get stack cleanup bytes from PDB function signature.

    For __thiscall/__stdcall: cleanup = n_params * 4
    For __cdecl: cleanup = 0
    """
    from pydia2 import cvconst
    try:
        sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.Function)
        if not sym:
            sym = sess.session.findSymbolByRVA(rva, cvconst.SymTag.PublicSymbol)
        if not sym:
            return 0

        # Get function type
        func_type = sym.type
        if not func_type:
            return 0

        cc = int(func_type.callingConvention)
        # CV_CALL_THISCALL=11, CV_CALL_STDCALL=7
        if cc not in (7, 11):
            return 0  # cdecl or other — caller cleans

        # Count parameters
        n_params = 0
        children = func_type.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
        if children:
            n_params = children.count

        return n_params * 4
    except Exception:
        return 0
