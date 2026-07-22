"""Test enrichment pass on real function."""

import sys, os

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora.persifal._decode import decode
from provider.lora.persifal._enrich import enrich, build_known
from provider.lora.persifal._ir import IR, IROp, Mn, Kind


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
    target_lex = int(sys.argv[1]) if len(sys.argv) > 1 else -1  # -1 = all

    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    func_rva, func_len, func_name = resolve_function(sess, "m3d::Application::init")

    this_class = ""
    idx = func_name.rfind("::")
    if idx > 0:
        this_class = func_name[:idx]

    print(f"{func_name} — this={this_class}\n")

    raw = decode(sess, func_rva, func_len)
    known = build_known(sess, func_rva)
    enriched = enrich(sess, raw, this_class=this_class, known=known)

    # Filter to target lexical group or dump all
    enriched.reset()
    count = 0
    last_lex = -1
    for node in enriched:
        if target_lex >= 0 and node.lexical != target_lex:
            continue
        # Print lexical boundary
        if node.lexical != last_lex and node.lexical >= 0:
            if last_lex >= 0:
                print()
            print(f"  // L{node.lexical}")
            last_lex = node.lexical
        mn = node.mn.value if node.mn else "???"
        ops = ", ".join(op_str(o) for o in node.ops)
        tag = "" if node.kind == Kind.PAYLOAD else f" [{node.kind.value}]"
        print(f"  {hex(node.rva)} [{node.esp_delta:+5d}]  {mn:8s}  {ops}{tag}")
        count += 1

    print(f"\n{count} nodes total")


if __name__ == "__main__":
    main()
