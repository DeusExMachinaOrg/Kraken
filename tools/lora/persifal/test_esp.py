"""Debug: track esp delta per instruction in a lexical group."""

import sys, os

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora.persifal._decode import decode
from provider.lora.persifal._ir import (
    IR, IROp, Mn,
    IRMov, IRPush, IRPop, IRCall, IRCmp, IRJcc, IRJmp, IRRet, IRArith,
)


def op_str(op: IROp) -> str:
    if op.kind == "reg":
        return op.reg
    elif op.kind == "imm":
        return hex(op.imm)
    elif op.kind == "mem":
        parts = []
        if op.mem_base: parts.append(op.mem_base)
        if op.mem_index: parts.append(f"{op.mem_index}*{op.mem_scale}")
        if op.mem_disp: parts.append(hex(op.mem_disp))
        return f"[{'+'.join(parts) or '0'}]"
    return "?"


def main():
    target_lex = int(sys.argv[1]) if len(sys.argv) > 1 else 1498

    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    func_rva, func_len, func_name = resolve_function(sess, "m3d::Application::init")
    print(f"{func_name} — L{target_lex}\n")

    stream = decode(sess, func_rva, func_len)
    group = [n for n in stream if n.lexical == target_lex]

    esp_delta = 0  # relative to function entry

    print(f"{'rva':>10s}  {'esp_d':>6s}  {'mn':8s}  operands")
    print(f"{'---':>10s}  {'---':>6s}  {'---':8s}  ---")

    for node in group:
        mn = node.mn.value if node.mn else "???"
        ops = ", ".join(op_str(o) for o in node.ops)

        # Show esp-relative accesses with vframe offset
        vframe_note = ""
        for op in node.ops:
            if op.kind == "mem" and op.mem_base == "esp":
                vf = op.mem_disp + esp_delta
                vframe_note += f"  [esp+{hex(op.mem_disp)}] = vf[{vf:+d}]"

        print(f"  {hex(node.rva):10s}  {esp_delta:+6d}  {mn:8s}  {ops}{vframe_note}")

        # Track esp changes
        if isinstance(node, IRPush):
            esp_delta -= 4
        elif isinstance(node, IRPop):
            esp_delta += 4
        elif isinstance(node, IRArith) and len(node.ops) >= 2:
            if node.ops[0].kind == "reg" and node.ops[0].reg == "esp" and node.ops[1].kind == "imm":
                if node.mn == Mn.SUB:
                    esp_delta -= node.ops[1].imm
                elif node.mn == Mn.ADD:
                    esp_delta += node.ops[1].imm
        elif isinstance(node, IRCall):
            # call pushes return address, callee pops it on ret
            # net effect depends on calling convention
            # for now just note it
            pass


if __name__ == "__main__":
    main()
