"""Jump analysis: build jump map from raw IR stream.

Pre-fold pass that scans IRJcc/IRJmp nodes and builds:
  - edges: every jump with source, target, direction
  - landings: which RVAs are jump targets (and from where)
  - back-edges: backward jumps (loop candidates)
  - scope_enter / scope_exit: RVA → push/pop scope events

Read-only scan over the frozen stream. Does not consume it.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from ._ir import IR, IRJcc, IRJmp, IRRet, IRCmp, IRArith, Mn
from ._stream import IRStream


@dataclass
class JumpEdge:
    """One jump edge."""
    source_rva: int
    target_rva: int
    mn: Mn | None       # je, jne, jmp, etc.
    lexical: int
    is_conditional: bool
    is_backward: bool   # target < source


class ScopeKind(str, Enum):
    IF_THEN  = "if_then"    # then-arm of if (jcc skips over)
    IF_ELSE  = "if_else"    # else-arm (between jmp and merge)
    LOOP     = "loop"       # backward-edge loop body


@dataclass
class ScopeRegion:
    """A contiguous RVA range that forms a child scope.

    Slots created inside [start, end) should be evicted at end.
    """
    start: int          # first RVA in region
    end: int            # first RVA AFTER region (scope pop point)
    kind: ScopeKind


@dataclass
class SwitchCase:
    """One case in a switch statement."""
    value: int          # case constant (e.g. 1, 2, ...)
    handler_rva: int    # first instruction of this case handler


@dataclass
class SwitchInfo:
    """A switch dispatch detected from jump table."""
    jmp_rva: int                    # RVA of the indirect jmp instruction
    default_rva: int                # RVA of the default handler (ja target)
    cases: list[SwitchCase] = field(default_factory=list)
    # handler_rva → list of case values (multiple values can share a handler)
    handlers: dict[int, list[int]] = field(default_factory=dict)


@dataclass
class JumpMap:
    """Complete jump structure of a function."""
    edges: list[JumpEdge] = field(default_factory=list)
    # target_rva → list of source edges landing here
    landings: dict[int, list[JumpEdge]] = field(default_factory=dict)
    # set of backward jump targets (loop headers)
    loop_headers: set[int] = field(default_factory=set)
    # set of all jump target rvas
    targets: set[int] = field(default_factory=set)

    # Scope tracking
    # RVA → list of scope pushes (enter child scope at this instruction)
    scope_enter: dict[int, list[ScopeRegion]] = field(default_factory=dict)
    # RVA → count of scope pops (exit child scope before this instruction)
    scope_exit: dict[int, int] = field(default_factory=dict)

    # Switch statements
    switches: list[SwitchInfo] = field(default_factory=list)

    def is_landing(self, rva: int) -> bool:
        return rva in self.targets

    def edges_from(self, rva: int) -> list[JumpEdge]:
        return [e for e in self.edges if e.source_rva == rva]

    def edges_to(self, rva: int) -> list[JumpEdge]:
        return self.landings.get(rva, [])


def build_jumpmap(stream: IRStream, sess=None) -> JumpMap:
    """Scan stream, build jump map. Does not consume the stream.

    If sess is provided, also detects switch/case jump tables by reading
    the PE jump table for indirect jmp instructions.
    """
    jm = JumpMap()
    stream.reset()

    # Collect all nodes for two-pass analysis
    all_nodes: list[IR] = []
    while stream.has_next():
        all_nodes.append(stream.shift())
    stream.reset()

    # Pass 1: collect edges
    rva_set: set[int] = {n.rva for n in all_nodes}

    for node in all_nodes:
        target_rva = 0
        is_cond = False

        if isinstance(node, IRJcc) and node.target_rva:
            target_rva = node.target_rva
            is_cond = True
        elif isinstance(node, IRJmp) and node.target_rva:
            target_rva = node.target_rva

        if target_rva == 0:
            continue

        is_back = target_rva < node.rva

        edge = JumpEdge(
            source_rva=node.rva,
            target_rva=target_rva,
            mn=node.mn,
            lexical=node.lexical,
            is_conditional=is_cond,
            is_backward=is_back,
        )

        jm.edges.append(edge)
        jm.targets.add(target_rva)
        jm.landings.setdefault(target_rva, []).append(edge)

        if is_back:
            jm.loop_headers.add(target_rva)

    # Pass 2: identify scope regions
    _build_scopes(jm, all_nodes, rva_set)

    # Pass 3: detect switch/case jump tables
    if sess is not None:
        _detect_switches(jm, all_nodes, rva_set, sess)

    return jm


def _next_rva(nodes: list[IR], idx: int) -> int:
    """RVA of instruction after nodes[idx]."""
    return nodes[idx + 1].rva if idx + 1 < len(nodes) else 0


def _build_scopes(jm: JumpMap, nodes: list[IR], rva_set: set[int]) -> None:
    """Identify scope regions from jump patterns.

    Patterns detected:
    1. if-then: jcc TARGET ... fallthrough → scope [next, TARGET)
    2. if-then-else: jcc ELSE ... jmp MERGE ... ELSE: ... MERGE:
       → then-scope [next, jmp], else-scope [ELSE, MERGE)
    3. loop: backward jcc to HEADER → scope [HEADER, source+1)
    """
    # Build index: rva → node index
    rva_to_idx: dict[int, int] = {n.rva: i for i, n in enumerate(nodes)}

    # Count how many forward JCCs target each label — shared-exit detection
    fwd_jcc_count: dict[int, int] = {}
    for node in nodes:
        if isinstance(node, IRJcc) and node.target_rva > node.rva:
            fwd_jcc_count[node.target_rva] = fwd_jcc_count.get(node.target_rva, 0) + 1

    for i, node in enumerate(nodes):
        if isinstance(node, IRJcc) and node.target_rva > node.rva:
            # Forward conditional jump
            body_start = _next_rva(nodes, i)
            target = node.target_rva

            if target not in rva_set:
                continue

            # Check if the instruction before target is an unconditional jmp
            # (if-then-else pattern)
            target_idx = rva_to_idx.get(target)
            if target_idx is None:
                continue

            # Look for jmp MERGE right before the else landing
            prev_idx = target_idx - 1
            if (prev_idx >= 0
                    and isinstance(nodes[prev_idx], IRJmp)
                    and nodes[prev_idx].target_rva > target
                    and nodes[prev_idx].target_rva in rva_set):
                # if-then-else
                jmp_node = nodes[prev_idx]
                merge = jmp_node.target_rva

                # then-arm: [body_start, jmp_rva)
                then_end = jmp_node.rva
                if body_start < then_end:
                    region = ScopeRegion(body_start, then_end, ScopeKind.IF_THEN)
                    jm.scope_enter.setdefault(body_start, []).append(region)
                    jm.scope_exit[then_end] = jm.scope_exit.get(then_end, 0) + 1

                # else-arm: [target, merge)
                if target < merge:
                    region = ScopeRegion(target, merge, ScopeKind.IF_ELSE)
                    jm.scope_enter.setdefault(target, []).append(region)
                    jm.scope_exit[merge] = jm.scope_exit.get(merge, 0) + 1
            else:
                # Simple if-then: scope [body_start, target)
                if body_start < target:
                    # Guard: skip if multiple forward JCCs target this same
                    # label — that's a shared-exit/guard pattern, not if-then
                    if fwd_jcc_count.get(target, 0) <= 1:
                        region = ScopeRegion(body_start, target, ScopeKind.IF_THEN)
                        jm.scope_enter.setdefault(body_start, []).append(region)
                        jm.scope_exit[target] = jm.scope_exit.get(target, 0) + 1

        elif isinstance(node, IRJcc) and node.target_rva < node.rva:
            # Backward conditional jump (loop)
            header = node.target_rva
            loop_end = _next_rva(nodes, i)
            if header < loop_end and header in rva_set:
                region = ScopeRegion(header, loop_end, ScopeKind.LOOP)
                jm.scope_enter.setdefault(header, []).append(region)
                jm.scope_exit[loop_end] = jm.scope_exit.get(loop_end, 0) + 1


def _detect_switches(jm: JumpMap, nodes: list[IR], rva_set: set[int], sess) -> None:
    """Detect switch/case dispatch patterns and read jump tables from PE.

    Pattern:
      cmp reg, N          (or sub reg, 1; cmp reg, N)
      ja  default_rva     (range check)
      jmp [reg*4 + table] (indirect jump, target_rva==0)

    Reads N+1 dwords from the PE jump table, each is an absolute VA.
    """
    import struct
    image_base = sess.image_base

    for i, node in enumerate(nodes):
        # Look for indirect jmp (target_rva == 0, has memory operand with scale=4)
        if not isinstance(node, IRJmp) or node.target_rva != 0:
            continue
        if not node.ops or node.ops[0].kind != "mem":
            continue
        op = node.ops[0]
        if op.mem_scale != 4 or op.mem_disp == 0:
            continue

        table_rva = op.mem_disp - image_base
        if table_rva <= 0:
            continue

        # Walk backward to find: cmp reg, N; ja default
        # Pattern may have interleaving movs/xors between cmp and ja.
        # May have sub reg, K / add reg, -K before cmp (base_value adjustment).
        default_rva = 0
        num_cases = 0
        base_value = 0

        # Scan backward up to 8 nodes to find ja and cmp
        ja_node = None
        cmp_node = None
        for back in range(1, min(9, i + 1)):
            prev = nodes[i - back]
            if ja_node is None:
                if isinstance(prev, IRJcc) and prev.mn == Mn.JA and prev.target_rva > 0:
                    ja_node = prev
                    default_rva = prev.target_rva
                continue
            # After finding ja, look for cmp
            if cmp_node is None:
                if isinstance(prev, IRCmp) and prev.ops:
                    cmp_node = prev
                    for cop in prev.ops:
                        if cop.kind == "imm":
                            num_cases = cop.imm + 1
                            break
                continue
            # After finding cmp, look for base_value adjustment (sub/add)
            if isinstance(prev, IRArith) and prev.ops and prev.mn in (Mn.SUB, Mn.ADD):
                if prev.mn == Mn.SUB:
                    for aop in prev.ops:
                        if aop.kind == "imm":
                            base_value = aop.imm
                            break
                elif prev.mn == Mn.ADD:
                    for aop in prev.ops:
                        if aop.kind == "imm":
                            # add eax, -1 → base_value = 1
                            base_value = -aop.imm
                            break
                break
            # Skip non-relevant instructions (xor, mov, etc.)

        if num_cases == 0 or default_rva == 0:
            continue

        # Read jump table from PE
        try:
            table_data = sess.pe.get_data(table_rva, num_cases * 4)
        except Exception:
            continue
        if len(table_data) < num_cases * 4:
            continue

        cases: list[SwitchCase] = []
        handlers: dict[int, list[int]] = {}
        for ci in range(num_cases):
            va = struct.unpack_from("<I", table_data, ci * 4)[0]
            handler_rva = va - image_base
            case_value = base_value + ci
            cases.append(SwitchCase(value=case_value, handler_rva=handler_rva))
            handlers.setdefault(handler_rva, []).append(case_value)
            # Register handler as a jump target
            jm.targets.add(handler_rva)

        jm.targets.add(default_rva)

        sw = SwitchInfo(
            jmp_rva=node.rva,
            default_rva=default_rva,
            cases=cases,
            handlers=handlers,
        )
        jm.switches.append(sw)
