"""Type flow enrichment pass.

Walks the IR stream, feeds each node to Machine, resolves types
on every operand. Emits a new stream with the same shape but
richer annotations.

Enrich is a flow engine — it tracks what Type is in each register
and stack slot. When it needs a type answer, it asks the TypeProvider.
Contains zero type resolution logic itself.

Slot type resolution:
  - First access is a call (lea ecx + call) → type from provider
  - First access is a mov → check `known` (PDB locals)
  - Neither → raw slot

Register type propagation:
  - mov reg, reg → propagate Type
  - mov reg, [slot] → Type from stack_map
  - mov reg, [base+disp] where base is typed → provider.resolve_offset
  - mov reg, [global] where symbol is typed → provider.resolve_global
  - call → clobber eax/ecx/edx, eax gets return type
  - xor reg, reg → clear Type
"""

from __future__ import annotations

from ._ir import (
    IR, IROp, Mn, Kind,
    IRMov, IRPush, IRPop, IRCall, IRCmp, IRJcc, IRJmp, IRRet, IRArith,
)
from ._stream import IRStream
from ._jumpmap import build_jumpmap
from ._vm import Machine, Stack
from ._types import (
    Type, UDTType, FieldInfo, VtSlotInfo,
)
from ._typeprovider import TypeProvider, DtorPattern


# ── Enrichment context ───────────────────────────────────────────────

class _TypeCtx:
    """Tracks type flow during enrichment pass.

    Stores Types in registers and stack. All type resolution goes
    through self.provider — no PDB logic here.
    """

    def __init__(self, provider: TypeProvider, this_class: str,
                 known: Stack | None = None):
        self.provider = provider
        self.machine = Machine()
        self.known = known or Stack()
        self.reg_types: dict[str, Type] = {}
        self.ecx_op: IROp | None = None

        if this_class:
            this_type = provider.make_type(this_class)
            if this_type.size > 0:
                self.reg_types["ecx"] = provider.ptr_of(this_type)

        # Pre-populate stack with function args (positive vframe offsets)
        for ref in self.known:
            if ref.base > 0:
                try:
                    self.stack_map[ref.base] = ref.type
                except KeyError:
                    pass

    @property
    def stack_map(self):
        """Alias: stack_map is the current machine scope."""
        return self.machine.scope

    # ── Register Types ──────────────────────────────────────────

    def set_reg_type(self, reg: str, typ: Type | None) -> None:
        if typ:
            self.reg_types[reg] = typ
        else:
            self.reg_types.pop(reg, None)

    def get_reg_type(self, reg: str) -> Type | None:
        return self.reg_types.get(reg)

    def clobber_caller_saved(self) -> None:
        for r in ("eax", "ecx", "edx"):
            self.reg_types.pop(r, None)

    # ── Slot resolution ──────────────────────────────────────────

    def resolve_slot(self, vf: int, nodes: tuple, start_idx: int) -> None:
        """Resolve an unknown stack slot by looking ahead in the stream."""
        if vf in self.stack_map:
            return

        sim_delta = self.machine.esp_delta

        for i in range(start_idx, min(start_idx + 80, len(nodes))):
            n = nodes[i]

            # Stop lookahead at basic block boundaries
            if isinstance(n, (IRJmp, IRRet)) and i > start_idx:
                break

            # Simulate esp changes
            if isinstance(n, IRPush):
                sim_delta -= 4
            elif isinstance(n, IRPop):
                sim_delta += 4
            elif isinstance(n, IRArith) and len(n.ops) >= 2:
                if n.ops[0].kind == "reg" and n.ops[0].reg == "esp" and n.ops[1].kind == "imm":
                    if n.mn == Mn.SUB:
                        sim_delta -= n.ops[1].imm
                    elif n.mn == Mn.ADD:
                        sim_delta += n.ops[1].imm

            # lea REG, [esp+X] where X + sim_delta == vf → followed by call
            if isinstance(n, IRMov) and n.mn == Mn.LEA and len(n.ops) >= 2:
                dst, src = n.ops[0], n.ops[1]
                if (dst.kind == "reg"
                        and src.kind == "mem" and src.mem_base in ("esp", "ebp")):
                    src_vf = src.mem_disp + sim_delta
                    if src_vf == vf:
                        lea_reg = dst.reg
                        if lea_reg == "ecx":
                            if self._resolve_slot_ecx(vf, nodes, i):
                                return
                        else:
                            if self._resolve_slot_param(vf, lea_reg, nodes, i, sim_delta):
                                return

            # mov [esp+X], value where X + sim_delta == vf → check known
            if isinstance(n, IRMov) and n.mn == Mn.MOV and len(n.ops) >= 2:
                dst = n.ops[0]
                if (dst.kind == "mem" and dst.mem_base in ("esp", "ebp")):
                    dst_vf = dst.mem_disp + sim_delta
                    if dst_vf == vf:
                        if vf in self.known:
                            ref = self.known[vf]
                            try:
                                self.stack_map[vf] = ref.type
                            except KeyError:
                                pass
                        return

        # Fallback: check PDB-declared locals
        if vf in self.known:
            ref = self.known[vf]
            try:
                self.stack_map[ref.base] = ref.type
            except KeyError:
                pass

    def _resolve_slot_ecx(self, vf: int, nodes: tuple, lea_idx: int) -> bool:
        """lea ecx, [slot] → call Class::Class — thiscall ctor pattern."""
        for j in range(lea_idx + 1, min(lea_idx + 5, len(nodes))):
            if isinstance(nodes[j], IRCall):
                sym_name = nodes[j].ops[0].symbol if nodes[j].ops else ""
                cls = _extract_class(sym_name)
                if cls:
                    typ = self.provider.make_type(cls)
                    if typ.size > 0:
                        try:
                            self.stack_map[vf] = typ
                        except KeyError:
                            pass
                        return True
                # No class name — try return type
                if sym_name:
                    call_op = nodes[j].ops[0] if nodes[j].ops else None
                    call_rva = (call_op.imm - self.provider.image_base
                                if call_op and call_op.kind == "imm" and call_op.imm > 0 else 0)
                    ret = self.provider.resolve_return_type(sym_name, rva=call_rva)
                    if ret:
                        try:
                            self.stack_map[vf] = ret.deref()
                        except KeyError:
                            pass
                        return True
                return False
        return False

    def _resolve_slot_param(self, vf: int, lea_reg: str,
                            nodes: tuple, lea_idx: int,
                            sim_delta: int) -> bool:
        """lea reg, [slot] → push reg → call — param-by-pointer pattern."""
        total_pushes = 0
        our_push_idx = -1

        for j in range(lea_idx + 1, min(lea_idx + 20, len(nodes))):
            n = nodes[j]
            if isinstance(n, IRPush) and n.ops:
                if (n.ops[0].kind == "reg" and n.ops[0].reg == lea_reg
                        and our_push_idx < 0):
                    our_push_idx = total_pushes
                total_pushes += 1
            elif isinstance(n, IRCall):
                if our_push_idx < 0:
                    return False
                sym_name = n.ops[0].symbol if n.ops else ""
                if not sym_name:
                    return False
                call_op = n.ops[0] if n.ops else None
                call_rva = (call_op.imm - self.provider.image_base
                            if call_op and call_op.kind == "imm" and call_op.imm > 0 else 0)
                params = self.provider.resolve_params(sym_name, rva=call_rva)
                if not params:
                    return False
                param_idx = total_pushes - 1 - our_push_idx
                if param_idx < 0 or param_idx >= len(params):
                    return False
                _, ptype = params[param_idx]
                if ptype.is_pointer:
                    pointee = ptype.deref()
                    if pointee.size > 0:
                        try:
                            self.stack_map[vf] = pointee
                        except KeyError:
                            pass
                        return True
                return False
        return False

    # ── Operand enrichment ───────────────────────────────────────

    def _annotate_field(self, op: IROp, result: FieldInfo, **extra) -> IROp:
        """Apply FieldInfo annotation to an operand."""
        # Build field name from chain
        field_name = ".".join(s.name for s in result.chain) if result.chain else result.name
        return _copy_op(op, field=field_name, type=result.type.name, **extra)

    def _annotate_vtslot(self, op: IROp, result: VtSlotInfo, **extra) -> IROp:
        """Apply VtSlotInfo annotation to an operand."""
        return _copy_op(op, symbol=result.method_name, **extra)

    def enrich_op(self, op: IROp, nodes: tuple = (), idx: int = 0,
                  is_lea: bool = False) -> IROp:
        """Return a copy of op with type/field annotations filled in."""
        if op.kind == "reg":
            typ = self.get_reg_type(op.reg)
            if typ:
                overrides: dict = {"sym": typ}
                if not op.type:
                    overrides["type"] = typ.name
                op = _copy_op(op, **overrides)

        elif op.kind == "mem" and op.mem_base and op.mem_base in self.reg_types:
            base_type = self.reg_types[op.mem_base]
            result = self.provider.resolve_offset(base_type, op.mem_disp)
            if result:
                # Root UDT = the type being accessed through (deref pointer)
                root = base_type.deref() if base_type.is_pointer else base_type
                root_uid = root.uid if root.is_udt else 0
                if isinstance(result, VtSlotInfo):
                    op = self._annotate_vtslot(op, result, owner=root_uid)
                else:
                    op = self._annotate_field(op, result, owner=root_uid)

        elif op.kind == "mem" and op.mem_base in ("ebp", "esp"):
            vf = self.machine.vframe(op.mem_disp)
            if vf not in self.stack_map and nodes:
                self.resolve_slot(vf, nodes, idx)
            if vf in self.stack_map:
                ref = self.stack_map[vf]
                # Root UDT for stack locals
                root = ref.type
                root_uid = root.uid if root.is_udt else 0
                if ref.offset == 0 and (is_lea or ref.type.is_pointer):
                    op = _copy_op(op, type=ref.type.name, vframe=vf,
                                  owner=root_uid)
                else:
                    result = self.provider.resolve_offset(ref.type, ref.offset)
                    if result:
                        if isinstance(result, VtSlotInfo):
                            op = self._annotate_vtslot(op, result, vframe=vf,
                                                       owner=root_uid)
                        else:
                            op = self._annotate_field(op, result, vframe=vf,
                                                      owner=root_uid)
                    else:
                        op = _copy_op(op, type=ref.type.name, vframe=vf,
                                      owner=root_uid)

        elif op.kind == "imm" and op.symbol:
            gsym = self.provider.resolve_global(op.symbol)
            if gsym and not op.type:
                op = _copy_op(op, type=gsym.name)

        return op

    # ── Mov handling ─────────────────────────────────────────────

    def handle_mov(self, node: IRMov, nodes: tuple = (), idx: int = 0) -> None:
        """Update type state from a mov instruction."""
        if len(node.ops) < 2:
            return
        dst, src = node.ops[0], node.ops[1]

        is_lea = node.mn == Mn.LEA

        if dst.kind == "reg":
            if src.kind == "reg":
                self.set_reg_type(dst.reg, self.get_reg_type(src.reg))
            elif src.kind == "mem" and src.mem_base and src.mem_base in self.reg_types:
                base_type = self.reg_types[src.mem_base]
                owner = base_type.deref()
                result = self.provider.resolve_offset(owner, src.mem_disp)
                if isinstance(result, FieldInfo) and result.name == "__vfptr":
                    # vfptr load → register gets vtable type (with correct vt_rva)
                    self.set_reg_type(dst.reg, result.type)
                elif isinstance(result, FieldInfo):
                    typ = result.type
                    if is_lea:
                        typ = self.provider.ptr_of(typ)
                    self.set_reg_type(dst.reg, typ)
                else:
                    self.set_reg_type(dst.reg, None)
            elif src.kind == "mem" and src.mem_base in ("esp", "ebp"):
                vf = self.machine.vframe(src.mem_disp)
                if vf not in self.stack_map:
                    if vf in self.known:
                        ref = self.known[vf]
                        try:
                            self.stack_map[ref.base] = ref.type
                        except KeyError:
                            pass
                    elif nodes:
                        self.resolve_slot(vf, nodes, idx)
                if vf in self.stack_map:
                    ref = self.stack_map[vf]
                    if ref.offset == 0:
                        typ = self.provider.ptr_of(ref.type) if is_lea else ref.type
                        self.set_reg_type(dst.reg, typ)
                    else:
                        result = self.provider.resolve_offset(ref.type, ref.offset)
                        if isinstance(result, FieldInfo):
                            typ = result.type
                            if is_lea:
                                typ = self.provider.ptr_of(typ)
                            self.set_reg_type(dst.reg, typ)
                        else:
                            self.set_reg_type(dst.reg, None)
                else:
                    self.set_reg_type(dst.reg, None)
            elif src.symbol:
                self.set_reg_type(dst.reg, self.provider.resolve_global(src.symbol))
            else:
                self.set_reg_type(dst.reg, None)

            if dst.reg == "ecx":
                self.ecx_op = src

        elif (dst.kind == "mem" and dst.mem_base in ("esp", "ebp")
              and dst.mem_base not in self.reg_types):
            vf = self.machine.vframe(dst.mem_disp)
            if vf not in self.stack_map:
                if vf in self.known:
                    ref = self.known[vf]
                    base_vf = ref.base
                    if base_vf not in self.stack_map:
                        try:
                            self.stack_map[base_vf] = ref.type
                        except KeyError:
                            pass
                elif src.kind == "reg" and src.reg in self.reg_types:
                    typ = self.reg_types[src.reg]
                    try:
                        self.stack_map[vf] = typ
                    except KeyError:
                        pass

    def handle_call(self, node: IRCall, nodes: tuple = (), idx: int = 0,
                    resolved_target: str = "") -> int:
        """Update type state from a call. Returns resolved cleanup bytes."""
        target = node.ops[0].symbol if node.ops and node.ops[0].symbol else ""
        effective_target = target or resolved_target
        cleanup = node.cleanup

        # Extract target RVA for exact overload resolution
        target_rva = 0
        if node.ops:
            op0 = node.ops[0]
            if op0.kind == "imm" and op0.imm > 0:
                target_rva = op0.imm - self.provider.image_base
            elif op0.kind == "mem" and op0.mem_base and op0.mem_base in self.reg_types:
                base_type = self.reg_types[op0.mem_base]
                result = self.provider.resolve_offset(base_type, op0.mem_disp)
                if isinstance(result, VtSlotInfo) and result.method_rva > 0:
                    target_rva = result.method_rva

        # Slot resolution from ecx (lea ecx, [esp+disp] + call)
        if self.ecx_op and self.ecx_op.kind == "mem" and self.ecx_op.mem_base in ("esp", "ebp"):
            vf = self.machine.vframe(self.ecx_op.mem_disp)
            is_dtor = _is_dtor(effective_target)
            cls = _extract_class(effective_target)
            if cls and is_dtor:
                if vf in self.stack_map:
                    try:
                        del self.stack_map[vf]
                    except KeyError:
                        pass
            elif cls:
                if vf in self.stack_map:
                    try:
                        del self.stack_map[vf]
                    except KeyError:
                        pass
                typ = self.provider.make_type(cls)
                if typ.size > 0:
                    try:
                        self.stack_map[vf] = typ
                    except KeyError:
                        pass
            elif effective_target and vf not in self.stack_map:
                ret = self.provider.resolve_return_type(
                    effective_target, rva=target_rva)
                if ret:
                    try:
                        self.stack_map[vf] = ret.deref()
                    except KeyError:
                        pass

        # Resolve indirect call cleanup via type info
        if cleanup == 0 and node.ops and node.ops[0].kind == "mem":
            op = node.ops[0]
            if op.mem_base and op.mem_base in self.reg_types:
                base_type = self.reg_types[op.mem_base]
                resolved = self.provider.resolve_cleanup(
                    base_type, op.mem_disp)
                if resolved > 0:
                    cleanup = resolved
            elif not op.mem_base and op.mem_disp:
                iat_rva = (op.mem_disp - self.provider.image_base
                           if op.mem_disp > self.provider.image_base
                           else op.mem_disp)
                if iat_rva > 0:
                    resolved = self.provider.resolve_iat_cleanup(iat_rva)
                    if resolved > 0:
                        cleanup = resolved

        # Clobber + retval type
        self.clobber_caller_saved()

        if effective_target:
            # Detect ctor: Class::Class → returns Class*
            ret_type = _detect_ctor_return(effective_target, self.provider)
            if ret_type is None:
                ret_type = self.provider.resolve_return_type(
                    effective_target, rva=target_rva)
            if ret_type:
                self.set_reg_type("eax", ret_type)

        self.ecx_op = None
        return cleanup

    def handle_arith(self, node: IRArith) -> str | None:
        """Update register type on arithmetic.

        add/sub reg, imm on a typed pointer → resolve field at new offset.
        Everything else → clear type.

        Returns resolved field name if one was found (for op annotation).
        """
        if len(node.ops) < 1 or node.ops[0].kind != "reg":
            return None
        reg = node.ops[0].reg

        if (node.mn in (Mn.ADD, Mn.SUB) and len(node.ops) >= 2
                and node.ops[1].kind == "imm" and reg in self.reg_types):
            base_type = self.reg_types[reg]
            owner = base_type.deref()
            disp = node.ops[1].imm
            if node.mn == Mn.SUB:
                disp = -disp
            result = self.provider.resolve_offset(owner, disp)
            if isinstance(result, FieldInfo):
                self.set_reg_type(reg, result.type)
                return result.name
            else:
                self.reg_types.pop(reg, None)
                return None
        else:
            self.reg_types.pop(reg, None)
            return None


# ── Ctor return type detection ──────────────────────────────────────

def _detect_ctor_return(func_name: str, provider: TypeProvider) -> Type | None:
    """Detect constructor: 'ns::Class::Class' → Class*."""
    if "::" not in func_name:
        return None
    parts = func_name.rsplit("::", 1)
    cls, method = parts[0], parts[1]
    short = cls.rsplit("::", 1)[-1]
    if method == short:
        typ = provider.make_type(cls)
        if typ.size > 0:
            return provider.ptr_of(typ)
    return None


# ── Inline dtor detection ────────────────────────────────────────────

def _match_inline_dtor(nodes: tuple, idx: int, vf_slot: int,
                       pattern: DtorPattern, machine: Machine) -> int | None:
    """Check if nodes[idx:] is an inline dtor for the slot at vf_slot.

    Returns the index past the last dtor node, or None.
    """
    n = len(nodes)
    window = min(idx + 8, n)

    lea_idx = None
    for j in range(idx + 1, window):
        nd = nodes[j]
        if (isinstance(nd, IRMov) and nd.mn == Mn.LEA
                and len(nd.ops) >= 2
                and nd.ops[1].kind == "mem"
                and nd.ops[1].mem_base in ("esp", "ebp")):
            lea_vf = machine.vframe(nd.ops[1].mem_disp)
            if lea_vf == vf_slot + pattern.zero_offset:
                lea_idx = j
                break

    if lea_idx is None:
        return None

    for j in range(lea_idx + 1, min(lea_idx + 4, n)):
        if isinstance(nodes[j], IRCmp):
            if (j + 1 < n and isinstance(nodes[j + 1], IRJcc)
                    and nodes[j + 1].mn == Mn.JE
                    and nodes[j + 1].target_rva > 0):
                skip_rva = nodes[j + 1].target_rva
                end = j + 2
                while end < n and nodes[end].rva < skip_rva:
                    end += 1
                return end

    return None


# ── Main pass ────────────────────────────────────────────────────────

def enrich(provider: TypeProvider, stream: IRStream,
           this_class: str = "",
           known: Stack | None = None) -> IRStream:
    """Type flow enrichment pass.

    Walks the stream, tracks types via Machine, emits a new stream
    with the same shape but types resolved on every operand.
    """
    # Pre-load dtor patterns for all known slot types
    dtor_patterns: dict[str, DtorPattern] = {}
    if known:
        for ref in known:
            type_name = ref.type.name
            if type_name and type_name not in dtor_patterns:
                pat = provider.get_dtor_pattern(ref.type)
                if pat:
                    dtor_patterns[type_name] = pat

    ctx = _TypeCtx(provider, this_class, known)
    out: list[IR] = []

    # Build jump map for scoping
    jm = build_jumpmap(stream)

    nodes = stream._nodes
    inline_until = -1
    inline_dead_slot: int | None = None

    for i, node in enumerate(nodes):
        # Deferred slot deletion: after inline block ends, kill the slot
        if inline_dead_slot is not None and i >= inline_until:
            try:
                del ctx.stack_map[inline_dead_slot]
            except KeyError:
                pass
            inline_dead_slot = None

        # Scope exit (from jumpmap regions)
        pops = jm.scope_exit.get(node.rva, 0)
        for _ in range(pops):
            ctx.machine.pop_scope()

        # Scope enter (from jumpmap regions)
        if node.rva in jm.scope_enter:
            for _ in jm.scope_enter[node.rva]:
                ctx.machine.push_scope()

        # Prologue/epilogue: feed to machine, skip enrichment
        if node.kind in (Kind.PROLOGUE, Kind.EPILOGUE):
            ctx.machine.eat(node)
            out.append(node)
            continue

        # Check for inline dtor pattern start
        if i >= inline_until and isinstance(node, IRMov) and node.mn == Mn.MOV:
            dtor_end, dead_vf = _try_match_dtor(ctx, nodes, i, dtor_patterns, provider)
            if dtor_end is not None:
                inline_until = dtor_end
                inline_dead_slot = dead_vf

        # Enrich
        resolved_cleanup = None
        resolved_field: str | None = None
        if isinstance(node, IRCall):
            enriched_ops = [ctx.enrich_op(op, nodes, i) for op in node.ops]
            enriched_target = enriched_ops[0].symbol if enriched_ops else ""
            resolved_cleanup = ctx.handle_call(node, nodes, i,
                                               resolved_target=enriched_target)
            _annotate_call_params(ctx, node, enriched_target, out)
        else:
            is_lea = isinstance(node, IRMov) and node.mn == Mn.LEA
            enriched_ops = [ctx.enrich_op(op, nodes, i, is_lea=(is_lea and j == 1))
                            for j, op in enumerate(node.ops)]
            if isinstance(node, IRMov):
                ctx.handle_mov(node, nodes, i)
                # Post-annotate destination register with resolved type
                if enriched_ops and enriched_ops[0].kind == "reg":
                    new_type = ctx.get_reg_type(enriched_ops[0].reg)
                    if new_type:
                        enriched_ops[0] = _copy_op(enriched_ops[0],
                                                   sym=new_type, type=new_type.name)
                    elif enriched_ops[0].type:
                        enriched_ops[0] = _copy_op(enriched_ops[0],
                                                   sym=None, type="")
            elif isinstance(node, IRArith):
                resolved_field = ctx.handle_arith(node)
                # Re-annotate dst with post-arith type + field
                if enriched_ops and enriched_ops[0].kind == "reg":
                    post_type = ctx.get_reg_type(enriched_ops[0].reg)
                    if post_type:
                        overrides: dict = {"sym": post_type, "type": post_type.name}
                        if resolved_field:
                            overrides["field"] = resolved_field
                        enriched_ops[0] = _copy_op(enriched_ops[0], **overrides)
                    elif enriched_ops[0].type:
                        enriched_ops[0] = _copy_op(enriched_ops[0],
                                                   sym=None, type="")

        enriched = _copy_node(node, enriched_ops)
        enriched.esp_delta = ctx.machine.esp_delta

        if resolved_cleanup is not None and isinstance(enriched, IRCall):
            enriched.cleanup = resolved_cleanup

        if i < inline_until:
            enriched.kind = Kind.INLINE

        ctx.machine.eat(enriched)
        out.append(enriched)

    return IRStream(out)


def _try_match_dtor(ctx: _TypeCtx, nodes: tuple, idx: int,
                    dtor_patterns: dict[str, DtorPattern],
                    provider: TypeProvider) -> tuple[int | None, int | None]:
    """Try to match an inline dtor at nodes[idx].

    Returns (end_index, vf_slot_to_delete) or (None, None).
    """
    node = nodes[idx]
    if len(node.ops) < 2 or node.ops[1].kind != "mem":
        return None, None
    src = node.ops[1]
    if src.mem_base not in ("esp", "ebp"):
        return None, None

    vf = ctx.machine.vframe(src.mem_disp)
    if vf not in ctx.stack_map:
        return None, None

    ref = ctx.stack_map[vf]
    if ref.offset != 0:
        return None, None

    type_name = ref.type.name
    pattern = dtor_patterns.get(type_name)
    if pattern is None:
        pat = provider.get_dtor_pattern(ref.type)
        if pat:
            dtor_patterns[type_name] = pat
            pattern = pat
        else:
            return None, None

    end = _match_inline_dtor(nodes, idx, vf, pattern, ctx.machine)
    if end is not None:
        return end, vf
    return None, None


# ── Call param annotation ────────────────────────────────────────────

def _annotate_call_params(ctx: _TypeCtx, node: IRCall,
                          resolved_target: str, out: list) -> None:
    """Walk backwards through out[] and annotate pushes with param names."""
    target = node.ops[0].symbol if node.ops and node.ops[0].symbol else ""
    effective = target or resolved_target
    if not effective:
        return

    target_rva = 0
    if node.ops:
        op0 = node.ops[0]
        if op0.kind == "imm" and op0.imm > 0:
            target_rva = op0.imm - ctx.provider.image_base

    params = ctx.provider.resolve_params(effective, rva=target_rva)
    if not params:
        return

    param_idx = 0
    j = len(out) - 1
    while j >= 0 and param_idx < len(params):
        n = out[j]
        if isinstance(n, IRPush) and n.ops:
            pname, ptype = params[param_idx]
            if pname and not n.ops[0].field:
                op = n.ops[0]
                out[j] = _copy_node(n, [_copy_op(op, field=pname,
                    type=ptype.name if not op.type else op.type)])
            param_idx += 1
        elif isinstance(n, IRCall):
            break
        j -= 1


# ── Build known stack from TypeProvider ─────────────────────────────

def build_known(provider: TypeProvider, func_rva: int) -> Stack:
    """Build PDB known stack from TypeProvider (no DIA/COM)."""
    known = Stack()
    locals_list = provider.build_known_locals(func_rva)
    for vframe_off, name, typ, size, loc_kind, register in locals_list:
        if loc_kind == 3 and typ.size > 0:  # RegRel (vframe-relative)
            try:
                known.set(vframe_off, typ, var_name=name)
            except KeyError:
                pass
    return known


# ── Helpers ──────────────────────────────────────────────────────────

def _extract_class(symbol: str) -> str:
    """'CStr::CStr' → 'CStr', 'operator+' → ''."""
    if "::" not in symbol:
        return ""
    return symbol.rsplit("::", 1)[0]


def _is_dtor(symbol: str) -> bool:
    """'CStr::~CStr' → True, 'CStr::CStr' → False."""
    if "::" not in symbol:
        return False
    method = symbol.rsplit("::", 1)[1]
    return method.startswith("~")


def _copy_op(op: IROp, **overrides) -> IROp:
    """Shallow copy of IROp with field overrides."""
    return IROp(
        reg=overrides.get("reg", op.reg),
        mem_base=overrides.get("mem_base", op.mem_base),
        mem_index=overrides.get("mem_index", op.mem_index),
        mem_scale=overrides.get("mem_scale", op.mem_scale),
        mem_disp=overrides.get("mem_disp", op.mem_disp),
        imm=overrides.get("imm", op.imm),
        size=overrides.get("size", op.size),
        kind=overrides.get("kind", op.kind),
        symbol=overrides.get("symbol", op.symbol),
        field=overrides.get("field", op.field),
        type=overrides.get("type", op.type),
        string=overrides.get("string", op.string),
        vframe=overrides.get("vframe", op.vframe),
        sym=overrides.get("sym", op.sym),
        owner=overrides.get("owner", op.owner),
    )


def _copy_node(node: IR, new_ops: list[IROp]) -> IR:
    """Copy an IR node with new ops, preserving subclass and extra fields."""
    kwargs = dict(mn=node.mn, rva=node.rva, lexical=node.lexical, ops=new_ops, kind=node.kind)
    if isinstance(node, IRCall):
        return IRCall(**kwargs, cleanup=node.cleanup)
    if isinstance(node, IRJcc):
        return IRJcc(**kwargs, target_rva=node.target_rva)
    if isinstance(node, IRJmp):
        return IRJmp(**kwargs, target_rva=node.target_rva)
    if isinstance(node, IRRet):
        return IRRet(**kwargs, cleanup=node.cleanup)
    return type(node)(**kwargs)
