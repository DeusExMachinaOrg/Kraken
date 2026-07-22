"""Inlinee detector: identifies compiler-inlined function bodies via field ownership.

C++ rule:  methods access their own object's fields.
Compiler:  can inline code but can't change field ownership.
Therefore: foreign fields = foreign code = inline.

Iterative inside-out detection peels one foreign-owner layer at a time:
  Pass 0: find runs where the most-frequent foreign owner dominates
  Pass 1: mask those nodes, expose the next foreign layer
  Repeat until stable.

Neutral nodes (no field access) extend a run but don't start one.
Masked nodes (from prior passes) are treated as neutral.

Function identification uses Fingerprints built from Analyzer cache:
  - Strip prologue/epilogue → core instruction stream
  - Extract call targets (symbol names) + field set + mnemonic sequence
  - After ownership identifies the class, match against that class's fingerprints
"""

from __future__ import annotations

from typing import NamedTuple
from dataclasses import dataclass, field as dc_field
from collections import Counter
from bisect import bisect_left, bisect_right

from ._ir import IR, IRCall, IRJcc, Kind, Mn
from ._stream import IRStream
from ._lexical import LexicalMap


# ── Output ────────────────────────────────────────────────────────────

class InlineMark(NamedTuple):
    """Detected inline function region."""
    begin: int          # RVA of first instruction
    end: int            # RVA past last instruction (exclusive)
    owner_class: str    # dominant owner class name (always set)
    target: str         # matched function name (empty if unidentified)
    pass_idx: int       # detection pass (0 = most-frequent foreign owner, outermost)


# ── Fingerprint ───────────────────────────────────────────────────────

@dataclass(frozen=True)
class Fingerprint:
    """Signature of a cached function, stripped of prologue/epilogue."""
    name: str                                  # full PDB name (e.g. "m3d::CVar::Set")
    owner_class: str                           # parent class (e.g. "m3d::CVar")
    call_rvas: frozenset[int]                  # RVAs of called functions (direct calls)
    field_uids: frozenset[int]                 # owner UIDs of fields accessed
    core_size: int                             # byte span of core (no pro/epi)
    core_mnemonics: tuple[int, ...]            # mnemonic enum hashes of core nodes


def _owner_class(func_name: str) -> str:
    """Extract class name from a fully qualified function name.

    "m3d::CVar::Set" → "m3d::CVar"
    "m3d::CStr::~CStr" → "m3d::CStr"
    "free_function" → ""
    """
    idx = func_name.rfind("::")
    if idx < 0:
        return ""
    return func_name[:idx]


def _collect_call_rvas(nodes: list[IR]) -> frozenset[int]:
    """Extract call target RVAs from IR nodes (direct calls only)."""
    rvas: set[int] = set()
    for nd in nodes:
        if isinstance(nd, IRCall) and nd.ops:
            op = nd.ops[0]
            if op.kind == "imm" and op.imm:
                rvas.add(op.imm)
    return frozenset(rvas)


def _collect_field_uids(nodes: list[IR]) -> frozenset[int]:
    """Extract owner UIDs of all field accesses in nodes."""
    uids: set[int] = set()
    for nd in nodes:
        for op in nd.ops:
            if op.owner > 0:
                uids.add(op.owner)
    return frozenset(uids)


def _collect_udt_basenames(nodes: list[IR], provider=None) -> set[str]:
    """Collect class names of all non-zero owner UIDs in nodes.

    Returns class names for preheating — which classes' methods to
    fingerprint. Requires provider to resolve UIDs to names.
    """
    uids: set[int] = set()
    for nd in nodes:
        for op in nd.ops:
            if op.owner > 0:
                uids.add(op.owner)
    if not provider:
        return set()
    names: set[str] = set()
    for uid in uids:
        t = provider._type_from_uid(uid)
        if t.name and t.name != "void":
            names.add(t.name)
    return names


def _core_nodes(nodes: list[IR]) -> list[IR]:
    """Strip prologue and epilogue nodes, returning the core payload."""
    return [nd for nd in nodes if nd.kind == Kind.PAYLOAD]


def build_fingerprints(funcs, provider=None) -> list[Fingerprint]:
    """Build fingerprints from cached FuncInfo objects.

    Args:
        funcs: iterable of FuncInfo (from Analyzer.funcs.values())
        provider: TypeProvider for resolving owner UIDs to class names
    Returns:
        list of Fingerprint objects
    """
    fps: list[Fingerprint] = []
    for info in funcs:
        if not info.name or not info.stream:
            continue

        owner = _owner_class(info.name)
        if not owner:
            continue  # free functions can't be inlined as class methods

        core = _core_nodes(info.stream._nodes)
        if len(core) < 3:
            continue  # too trivial to fingerprint

        call_rvas = _collect_call_rvas(core)
        field_uids = _collect_field_uids(core)
        mnemonics: list[int] = []
        for nd in core:
            if nd.mn:
                mnemonics.append(hash(nd.mn))

        # Core byte size: first to last RVA
        core_size = 0
        if len(core) >= 2:
            core_size = core[-1].rva - core[0].rva
        elif core:
            core_size = 4  # single instruction estimate

        fps.append(Fingerprint(
            name=info.name,
            owner_class=owner,
            call_rvas=call_rvas,
            field_uids=field_uids,
            core_size=core_size,
            core_mnemonics=tuple(mnemonics),
        ))

    return fps


def _index_by_owner(fps: list[Fingerprint]) -> dict[str, list[Fingerprint]]:
    """Build owner_class → fingerprints index."""
    idx: dict[str, list[Fingerprint]] = {}
    for fp in fps:
        idx.setdefault(fp.owner_class, []).append(fp)
    return idx


# ── Fingerprint matching ─────────────────────────────────────────────

def _match_fingerprint(nodes: list[IR], start: int, end: int,
                       class_name: str,
                       fp_index: dict[str, list[Fingerprint]]) -> str:
    """Match a detected inline region against fingerprints for class_name.

    Extracts call RVAs + field UIDs from the span,
    then scores each candidate fingerprint.
    Returns best match name, or empty string if no confident match.
    """
    # Collect span's signals
    core: list[IR] = [nd for nd in nodes[start:end]
                      if nd.kind == Kind.PAYLOAD]
    if len(core) < 3:
        return ""

    span_calls = _collect_call_rvas(core)
    span_uids = _collect_field_uids(core)
    span_mn: list[int] = [hash(nd.mn) for nd in core if nd.mn]

    # Byte size of span
    span_size = core[-1].rva - core[0].rva if len(core) >= 2 else 4

    # Lookup by class
    candidates = fp_index.get(class_name)
    if not candidates:
        return ""

    # Score each candidate
    best_name = ""
    best_score = -1.0

    for fp in candidates:
        score = 0.0

        # 1. Call RVA overlap (strongest signal)
        if fp.call_rvas and span_calls:
            match = span_calls & fp.call_rvas
            call_ratio = len(match) / max(len(fp.call_rvas), len(span_calls))
            score += call_ratio * 4.0
            foreign = span_calls - fp.call_rvas
            if foreign:
                score -= len(foreign) * 0.5
        elif not fp.call_rvas and not span_calls:
            score += 0.5
        elif fp.call_rvas and not span_calls:
            score -= 1.0  # fingerprint calls but span doesn't — mismatch
        elif not fp.call_rvas and span_calls:
            score -= 1.0  # span calls but fingerprint doesn't — mismatch

        # 2. Field UID overlap
        if fp.field_uids and span_uids:
            match = span_uids & fp.field_uids
            uid_ratio = len(match) / max(len(fp.field_uids), len(span_uids))
            score += uid_ratio * 2.0
        elif not fp.field_uids and not span_uids:
            score += 0.3

        # 3. Size ratio
        if fp.core_size > 0 and span_size > 0:
            ratio = min(span_size, fp.core_size) / max(span_size, fp.core_size)
            score += ratio * 1.0 if ratio > 0.3 else -0.5

        # 4. Mnemonic sequence similarity (length ratio + head match)
        if fp.core_mnemonics and span_mn:
            len_ratio = min(len(span_mn), len(fp.core_mnemonics)) / \
                        max(len(span_mn), len(fp.core_mnemonics))
            score += len_ratio * 0.5
            head = min(5, len(span_mn), len(fp.core_mnemonics))
            head_match = sum(1 for a, b in zip(span_mn[:head],
                                                fp.core_mnemonics[:head])
                            if a == b)
            score += (head_match / head) * 0.5 if head > 0 else 0

        if score > best_score:
            best_score = score
            best_name = fp.name

    if best_score < 1.5:
        return ""

    return best_name


# ── Type hierarchy ────────────────────────────────────────────────────

def build_hierarchy(provider, this_class: str) -> frozenset[int]:
    """Build the set of UDT UIDs in this function's type hierarchy.

    Includes this_class + all transitive base classes.
    """
    if not this_class:
        return frozenset()

    uids: set[int] = set()
    todo = [this_class]
    visited: set[str] = set()

    while todo:
        name = todo.pop()
        if name in visited:
            continue
        visited.add(name)
        typ = provider.make_type(name)
        if typ.uid > 0 and typ.is_udt:
            uids.add(typ.uid)
            fields = provider.db.get_fields(typ.uid)
            for _off, _fname, fuid, _fsz, is_base in fields:
                if is_base and fuid:
                    base_type = provider._type_from_uid(fuid)
                    if base_type.is_udt:
                        todo.append(base_type.name)

    return frozenset(uids)


def collect_global_uids(stream: IRStream, provider) -> frozenset[int]:
    """Collect UDT UIDs of types accessed via global singletons.

    Global loads (mov reg, [abs_addr] with resolved symbol) access
    service locators — not inlined code.  Their UIDs should be
    treated as 'own' to avoid false positive inline marks.
    """
    uids: set[int] = set()
    for nd in stream._nodes:
        for op in nd.ops:
            if (op.symbol and op.kind == "mem"
                    and not op.mem_base and op.mem_disp):
                t = provider.resolve_global(op.symbol)
                if t and t.is_pointer and t.deref().is_udt:
                    uids.add(t.deref().uid)
    return frozenset(uids)


# ── Classification ────────────────────────────────────────────────────

_MIN_NODES = 3
_MIN_FOREIGN = 2    # minimum foreign-field nodes to form a mark


def _classify_node(nd: IR, hierarchy: frozenset[int],
                   target_uid: int,
                   skip: set[int], idx: int) -> str:
    """Classify a node for a specific target owner UID.

    skip: node indices consumed by prior passes — neutral.
    own: any op owned by hierarchy (includes globals).
    foreign: any op owned by target_uid.
    neutral: everything else (other foreign types, no fields, skipped).
    """
    if idx in skip:
        return "neutral"

    has_target = False
    for op in nd.ops:
        if op.owner > 0:
            if op.owner in hierarchy:
                return "own"
            if op.owner == target_uid:
                has_target = True
    return "foreign" if has_target else "neutral"


def _dominant_foreign(nodes: list[IR], hierarchy: frozenset[int],
                      masked_uids: frozenset[int],
                      skip: set[int]) -> int:
    """Find the most-frequent foreign owner UID (not in hierarchy, not masked)."""
    counts: Counter[int] = Counter()
    for i, nd in enumerate(nodes):
        if i in skip or nd.kind in (Kind.PROLOGUE, Kind.EPILOGUE):
            continue
        for op in nd.ops:
            if (op.owner > 0
                    and op.owner not in hierarchy
                    and op.owner not in masked_uids):
                counts[op.owner] += 1
    return counts.most_common(1)[0][0] if counts else 0


# ── Single pass ───────────────────────────────────────────────────────

def _identify_function(nodes: list[IR], start: int, end: int,
                       class_name: str,
                       fp_index: dict[str, list[Fingerprint]] | None = None) -> str:
    """Identify the inlined function via fingerprint matching.

    Uses Fingerprint-based matching (call RVAs + field UIDs + size + mnemonics).
    Falls back to heuristics for ctor/dtor patterns.
    """
    if fp_index:
        result = _match_fingerprint(nodes, start, end, class_name, fp_index)
        if result:
            return result

    # Fallback: simple heuristics
    fields: set[str] = set()
    n_fields = 0
    for nd in nodes[start:end]:
        for op in nd.ops:
            if op.field:
                fields.add(op.field)
                n_fields += 1

    short = class_name.rsplit("::", 1)[-1]

    # Dtor field pattern: ZERO + m_charPtr + small region
    if {"m_charPtr", "ZERO"} <= fields and n_fields <= 8:
        return class_name + "::~" + short

    # Ctor: look for direct class method call
    for nd in nodes[start:end]:
        if isinstance(nd, IRCall) and nd.ops and nd.ops[0].symbol:
            sym = nd.ops[0].symbol
            if sym.endswith("::" + short) and "::~" not in sym:
                return sym

    return ""


def _one_pass(nodes: list[IR], hierarchy: frozenset[int],
              target_uid: int, skip: set[int],
              pass_idx: int, provider=None,
              fp_index: dict[str, list[Fingerprint]] | None = None,
              unmatched: list | None = None) -> list[InlineMark]:
    """Detect runs of nodes owned by target_uid (+ neutral fillers).

    unmatched: if provided, collects InlineMarks with empty target
    (regions detected but not identified — diagnostic only).
    """
    marks: list[InlineMark] = []
    run_start = -1
    run_foreign = 0
    target_name = ""
    if provider:
        t = provider._type_from_uid(target_uid)
        target_name = t.name if t.name != "void" else ""

    def _flush(end_idx: int) -> None:
        nonlocal run_start, run_foreign
        if run_start >= 0 and run_foreign >= _MIN_FOREIGN and (end_idx - run_start) >= _MIN_NODES:
            end_rva = (nodes[end_idx].rva if end_idx < len(nodes)
                       else nodes[-1].rva + 1)
            func = _identify_function(nodes, run_start, end_idx,
                                      target_name, fp_index=fp_index)
            mark = InlineMark(
                nodes[run_start].rva, end_rva,
                target_name, func, pass_idx)
            if func:
                marks.append(mark)
            elif unmatched is not None:
                unmatched.append(mark)
        run_start = -1
        run_foreign = 0

    for i, nd in enumerate(nodes):
        if nd.kind in (Kind.PROLOGUE, Kind.EPILOGUE):
            _flush(i)
            continue

        cls = _classify_node(nd, hierarchy, target_uid, skip, i)

        if cls == "own":
            _flush(i)
        elif cls == "foreign":
            if run_start < 0:
                run_start = i
            run_foreign += 1
        # neutral: extends but doesn't start

    _flush(len(nodes))

    # Post-pass: extend marks through guard jumps.
    # Dtor pattern: foreign fields → cmp → je skip_free → FreeMem block → skip_free.
    # The run gets flushed at g_Kernel (own) inside the FreeMem block, but the
    # je target tells us the whole block belongs to this dtor.
    # Apply to both matched AND unmatched — then merge overlapping.
    rva_to_idx_local: dict[int, int] = {nd.rva: i for i, nd in enumerate(nodes)}

    def _extend_guard(m: InlineMark) -> InlineMark:
        begin_idx = rva_to_idx_local.get(m.begin, -1)
        end_idx = rva_to_idx_local.get(m.end, len(nodes))
        scan_start = max(begin_idx, end_idx - 6)
        best_target = m.end
        for j in range(scan_start, end_idx):
            nd = nodes[j]
            if isinstance(nd, IRJcc) and nd.target_rva > m.end:
                best_target = max(best_target, nd.target_rva)
        if best_target > m.end:
            ext_end_idx = rva_to_idx_local.get(best_target, end_idx)
            func = _identify_function(nodes, begin_idx, ext_end_idx,
                                      target_name, fp_index=fp_index)
            return m._replace(end=best_target, target=func or m.target)
        return m

    # Extend matched marks
    extended = [_extend_guard(m) for m in marks]

    # Also extend unmatched — they may now overlap with matched marks
    if unmatched is not None:
        extended_unmatched = [_extend_guard(m) for m in unmatched]
        # Promote: unmatched that now overlap a matched mark get absorbed;
        # unmatched that gained identification get promoted to matched.
        for um in extended_unmatched:
            if um.target:
                extended.append(um)

    return extended


# ── Main: iterative inside-out ────────────────────────────────────────

_MAX_DEPTH = 5


def detect_inlinees(stream: IRStream, lexmap: LexicalMap,
                    hierarchy: frozenset[int] | None = None,
                    this_class: str = "",
                    provider=None,
                    fps: list[Fingerprint] | None = None,
                    unmatched: list | None = None) -> list[InlineMark]:
    """Detect inlined function bodies by field ownership.

    Iterates inside-out, peeling one foreign-owner layer per pass:
      Pass 0: mask most-frequent foreign owner, find runs
      Pass 1: mask next foreign owner, find deeper runs
      ...until no new marks.

    fps: fingerprints from build_fingerprints() — used for function identification.
    unmatched: if provided, collects regions detected but not identified (diagnostic).
    """
    if not hierarchy:
        return []

    # Merge global singleton UIDs into hierarchy — accessing globals
    # is normal code, not inlined foreign methods.
    if provider:
        globals_uids = collect_global_uids(stream, provider)
        if globals_uids:
            hierarchy = hierarchy | globals_uids

    # Build fingerprint index once
    fp_index = _index_by_owner(fps) if fps else None

    nodes = stream._nodes
    # RVA → node index for fast skip population
    rva_to_idx: dict[int, int] = {nd.rva: i for i, nd in enumerate(nodes)}
    sorted_rvas = sorted(rva_to_idx.keys())

    all_marks: list[InlineMark] = []
    skip: set[int] = set()
    peeled: set[int] = set()  # owner UIDs already processed

    for pass_idx in range(_MAX_DEPTH):
        # Find the dominant foreign owner not yet peeled
        dom = _dominant_foreign(nodes, hierarchy, frozenset(peeled), skip)
        if dom == 0:
            break
        peeled.add(dom)

        # Detect runs for this specific owner UID
        new_marks = _one_pass(nodes, hierarchy, dom,
                              skip, pass_idx, provider, fp_index=fp_index,
                              unmatched=unmatched)
        if not new_marks:
            continue

        all_marks.extend(new_marks)

        # Skip marked nodes in subsequent passes (bisect for O(n log n))
        for m in new_marks:
            lo = bisect_left(sorted_rvas, m.begin)
            hi = bisect_right(sorted_rvas, m.end - 1)
            for j in range(lo, hi):
                skip.add(rva_to_idx[sorted_rvas[j]])

    # Post-filter: bigger mark absorbs smaller nested marks
    return _remove_nested(all_marks)


def _remove_nested(marks: list[InlineMark]) -> list[InlineMark]:
    """Merge marks: bigger absorbs overlapping or same-class adjacent smaller marks."""
    if len(marks) <= 1:
        return marks
    # Sort by size descending — bigger marks take priority
    by_size = sorted(marks, key=lambda m: m.end - m.begin, reverse=True)
    kept: list[InlineMark] = []
    for m in by_size:
        merged = False
        m_size = m.end - m.begin
        for ki, k in enumerate(kept):
            overlap_start = max(m.begin, k.begin)
            overlap_end = min(m.end, k.end)
            if overlap_end > overlap_start:
                overlap = overlap_end - overlap_start
                # Same owner class: any overlap merges (adjacent dtor fragments)
                # Different owner: need >50% overlap (nested inlinees)
                threshold = 0 if m.owner_class == k.owner_class else m_size * 0.5
                if overlap >= threshold:
                    kept[ki] = k._replace(
                        begin=min(k.begin, m.begin),
                        end=max(k.end, m.end),
                    )
                    merged = True
                    break
        if not merged:
            kept.append(m)
    # Return in RVA order
    return sorted(kept, key=lambda m: m.begin)
