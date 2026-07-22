"""Test inlinee detector — emit with fold rule: chain=ignore, no-call=emit."""

import sys, os, time

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora.persifal._typedb import TypeDB
from provider.lora.persifal._typeprovider import TypeProvider, _IAT
from provider.lora.persifal._analyzer import Analyzer
from provider.lora.persifal._ir import IROp, IRCall, Kind
from provider.lora.persifal._inlinee import (
    build_fingerprints, detect_inlinees, InlineMark, Fingerprint,
    _collect_udt_basenames, _index_by_owner, _owner_class,
    _collect_call_rvas,
)


def op_str(op: IROp) -> str:
    if op.kind == "reg":
        s = op.reg
    elif op.kind == "imm":
        s = hex(op.imm)
    elif op.kind == "mem":
        parts = []
        if op.mem_base: parts.append(op.mem_base)
        if op.mem_index: parts.append(f"{op.mem_index}*{op.mem_scale}")
        if op.mem_disp: parts.append(hex(op.mem_disp))
        s = f"[{'+'.join(parts) or '0'}]"
    else:
        s = "?"
    if op.symbol: s += f" «{op.symbol}»"
    if op.string: s += f' "{op.string[:30]}"'
    if op.field: s += f" .{op.field}"
    if op.type: s += f" ({op.type})"
    return s


def main():
    func_name = sys.argv[1] if len(sys.argv) > 1 else "m3d::Application::init"

    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")

    db = TypeDB("target/game.persifal.db")
    db.preheat()
    provider = TypeProvider(db, image_base=sess.pe.OPTIONAL_HEADER.ImageBase)
    provider.annotate_batch(_IAT)
    provider.preheat()

    az = Analyzer(sess, provider=provider)

    # Resolve target
    func_rva, func_len, resolved = resolve_function(sess, func_name)
    if func_rva == 0:
        print(f"Not found: {func_name}")
        return

    info = az.get(func_rva)
    if info is None:
        info = az.process(func_rva, func_len, name=resolved)
        az.cache.commit()

    this_class = provider.get_parent_class(func_rva)

    # Preheat stem-related functions
    target_basenames = _collect_udt_basenames(info.stream._nodes, provider)
    target_stems = target_basenames - {this_class} if this_class else target_basenames

    all_funcs = db.all_functions()
    cached_rvas = az.cache.all_rvas()
    stem_funcs = [(r, l, n) for r, l, n in all_funcs
                  if _owner_class(n) in target_stems and r not in cached_rvas]
    if stem_funcs:
        print(f"Processing {len(stem_funcs)} stem-related functions...")
        for r, l, n in stem_funcs:
            try: az.process(r, l, name=n)
            except: pass
        az.cache.commit()

    az.load_cache()
    all_fps = build_fingerprints(az.funcs.values(), provider=provider)

    from provider.lora.persifal._inlinee import build_hierarchy
    hierarchy = build_hierarchy(provider, this_class)

    # Detect (collect_global_uids runs internally)
    marks = detect_inlinees(info.stream, info.lexical, hierarchy,
                            this_class=this_class, provider=provider, fps=all_fps)

    print(f"{resolved} @ {hex(func_rva)}, {func_len} bytes")
    print(f"Stream: {len(info.stream._nodes)} nodes, Detected: {len(marks)} marks")

    # For each mark, check if span has direct calls → chain vs inline
    nodes = info.stream._nodes
    mark_begin = {m.begin: m for m in marks}
    mark_ends = {m.end for m in marks}

    # Precompute: for each mark, collect span's inner calls and compare
    # against the fingerprint's call_targets.
    fp_by_name: dict[str, Fingerprint] = {}
    for fp in all_fps:
        fp_by_name[fp.name] = fp

    mark_info: dict[int, tuple[list[str], bool, frozenset[str]]] = {}
    for m in marks:
        span_nodes = [nd for nd in nodes if m.begin <= nd.rva < m.end]
        span_calls = _collect_call_rvas(span_nodes)
        # Use target (matched function) if available, else owner_class
        lookup_name = m.target or m.owner_class
        fp = fp_by_name.get(lookup_name)
        fp_calls = fp.call_rvas if fp else frozenset()
        foreign = span_calls - fp_calls
        calls_self = lookup_name in span_calls
        mark_info[m.begin] = (sorted(span_calls), calls_self, foreign)

    # Emit
    lines: list[str] = []
    last_lex = -1
    skip_until = 0
    emitted_ice = 0
    ignored_chain = 0
    suppressed = 0

    for node in info.stream._nodes:
        if node.rva < skip_until:
            suppressed += 1
            continue

        if node.rva in mark_begin:
            m = mark_begin[node.rva]
            span_calls, calls_self, foreign = mark_info[m.begin]

            if node.lexical != last_lex and node.lexical >= 0:
                lines.append(f"  // L{node.lexical}")
                last_lex = node.lexical

            # Call match ratio: what fraction of span calls does the fingerprint explain?
            lookup_name = m.target or m.owner_class
            fp = fp_by_name.get(lookup_name)
            fp_calls = fp.call_rvas if fp else frozenset()
            span_set = frozenset(span_calls)
            explained = span_set & fp_calls
            call_ratio = len(explained) / len(span_set) if span_set else 1.0

            display = m.target if m.target else f"?{m.owner_class}"

            if calls_self:
                ignored_chain += 1
                lines.append(f"    // CALLSITE [{m.owner_class}] {display} d={m.depth}")
            elif span_calls and call_ratio == 0.0:
                ignored_chain += 1
                lines.append(f"    // FOREIGN [{m.owner_class}] {display} d={m.depth} "
                             f"(0/{len(span_calls)} calls match, unknown: {', '.join(sorted(foreign)[:3])})")
            else:
                skip_until = m.end
                emitted_ice += 1
                ratio_str = f" calls={len(explained)}/{len(span_calls)}" if span_calls else ""
                foreign_str = f" foreign={','.join(sorted(foreign)[:2])}" if foreign else ""
                lines.append(f"    {hex(m.begin)}  → {display}()  "
                             f"// ICE {m.owner_class} {m.end - m.begin}B d={m.depth}{ratio_str}{foreign_str}")
                continue
        else:
            if node.lexical != last_lex and node.lexical >= 0:
                if last_lex >= 0:
                    lines.append("")
                lines.append(f"  // L{node.lexical}")
                last_lex = node.lexical

        mn = node.mn.value if node.mn else "???"
        ops = ", ".join(op_str(o) for o in node.ops)
        tag = "" if node.kind == Kind.PAYLOAD else f" [{node.kind.value}]"
        lines.append(f"    {hex(node.rva)}  {mn:8s}  {ops}{tag}")

    total = len(info.stream._nodes)
    print(f"Result: {total - suppressed} emitted, {suppressed} suppressed")
    print(f"  ICE emitted: {emitted_ice}, CHAIN ignored: {ignored_chain}")

    out = "\n".join(lines)
    dump_path = os.path.join(root, "temp_inlinee_output.txt")
    with open(dump_path, "w", encoding="utf-8") as f:
        f.write(f"{resolved} @ {hex(func_rva)}, {func_len} bytes\n")
        f.write(f"ICE emitted: {emitted_ice}, CHAIN ignored: {ignored_chain}\n\n")
        f.write(out)
    print(f"Dumped to {dump_path}")


if __name__ == "__main__":
    main()
