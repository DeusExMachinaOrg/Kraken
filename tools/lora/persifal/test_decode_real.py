"""Smoke test: decode a real function from hta.exe + game.pdb."""

import sys, os

# Direct imports, bypass lora package __init__
root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

# Import lora internals directly
from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora.persifal._decode import decode
from provider.lora.persifal._ir import (
    IRMov, IRPush, IRCall, IRJcc, IRJmp, IRRet, IRArith, IRCmp, IRNop, IR,
)


def main():
    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    print(f"Loaded: image_base={hex(sess.image_base)}")

    func_rva, func_len, func_name = resolve_function(sess, "m3d::Application::init")
    if func_rva == 0:
        print("Function not found!")
        return
    print(f"Function: {func_name} at {hex(func_rva)}, {func_len} bytes")

    stream = decode(sess, func_rva, func_len)
    print(f"Decoded: {len(stream)} IR nodes\n")

    # Stats
    type_counts: dict[str, int] = {}
    enriched = 0
    lexical_tags: set[int] = set()

    for node in stream:
        name = type(node).__name__
        type_counts[name] = type_counts.get(name, 0) + 1
        if node.lexical >= 0:
            lexical_tags.add(node.lexical)
        for op in node.ops:
            if op.symbol or op.string or op.field:
                enriched += 1

    print("Node types:")
    for name, count in sorted(type_counts.items(), key=lambda x: -x[1]):
        print(f"  {name:12s} {count}")
    print(f"\nEnriched operands: {enriched}")
    print(f"Lexical tags: {len(lexical_tags)}")

    # First 30 nodes
    print("\nFirst 30 nodes:")
    stream.reset()
    for i in range(min(30, len(stream))):
        node = stream.shift()
        mn = node.mn.value if node.mn else "???"
        lex = f"L{node.lexical}" if node.lexical >= 0 else ""
        ops_str = []
        for op in node.ops:
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
            # enrichments
            if op.symbol: s += f" «{op.symbol}»"
            if op.string: s += f' "{op.string[:40]}"'
            if op.field: s += f" .{op.field}"
            ops_str.append(s)

        print(f"  {hex(node.rva):10s} {lex:5s} {mn:8s} {', '.join(ops_str)}")


if __name__ == "__main__":
    main()
