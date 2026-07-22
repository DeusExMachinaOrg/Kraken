"""Dump enriched stream from SQLite cache — same format as test_enrich_real.py."""

import sys, os

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora.persifal._analyzer import Analyzer
from provider.lora.persifal._ir import IROp, Kind


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
    func_name = sys.argv[1] if len(sys.argv) > 1 else "ai::Player::SaveToXML"

    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    az = Analyzer(sess)

    func_rva, func_len, resolved = resolve_function(sess, func_name)
    if func_rva == 0:
        print(f"Function not found: {func_name}")
        return

    info = az.get(func_rva)
    if info is None:
        print(f"Not in cache: {func_name} @ {hex(func_rva)}")
        return

    print(f"{info.name} — {info.length} bytes @ {hex(info.rva)}")
    print(f"Nodes: {len(info.stream)}  Edges: {len(info.jumpmap.edges)}  "
          f"Loops: {len(info.jumpmap.loop_headers)}")
    print()

    lines = []
    info.stream.reset()
    last_lex = -1
    for node in info.stream:
        if node.lexical != last_lex and node.lexical >= 0:
            if last_lex >= 0:
                lines.append("")
            lines.append(f"  // L{node.lexical}")
            last_lex = node.lexical
        mn = node.mn.value if node.mn else "???"
        ops = ", ".join(op_str(o) for o in node.ops)
        tag = "" if node.kind == Kind.PAYLOAD else f" [{node.kind.value}]"
        lines.append(f"  {hex(node.rva)} [{node.esp_delta:+5d}]  {mn:8s}  {ops}{tag}")

    out = "\n".join(lines)
    print(out)
    print(f"\n{len(info.stream)} nodes total")

    dump_path = os.path.join(root, "temp_enrich_check.txt")
    with open(dump_path, "w", encoding="utf-8") as f:
        f.write(f"{info.name} — {info.length} bytes @ {hex(info.rva)}\n\n")
        f.write(out)
    print(f"\nDumped to {dump_path}")


if __name__ == "__main__":
    main()
