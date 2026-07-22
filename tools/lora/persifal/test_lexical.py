"""Debug: extract one lexical group, fold it, show slot type resolution."""

import sys, os

root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
sys.path.insert(0, root)
os.chdir(root)

from provider.lora._state import SessionManager
from provider.lora._pdb import resolve_function
from provider.lora import _pdb as pdb_mod
from provider.lora.persifal._decode import decode
from provider.lora.persifal._ir import (
    IR, IROp, Mn,
    IRMov, IRPush, IRPop, IRCall, IRCmp, IRJcc, IRJmp, IRRet, IRArith, IRNop,
)
from provider.lora.persifal._stream import IRStream
from provider.lora.persifal._vm import Machine, Reg, to_reg, Stack, Symbol, Scope


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
    return s


_JCC_OP = {
    Mn.JE: "==", Mn.JNE: "!=",
    Mn.JG: ">", Mn.JGE: ">=", Mn.JL: "<", Mn.JLE: "<=",
    Mn.JA: ">", Mn.JAE: ">=", Mn.JB: "<", Mn.JBE: "<=",
}


def cpu_dump(machine: Machine) -> str:
    parts = []
    for r in (Reg.EAX, Reg.ECX, Reg.EDX, Reg.EBX, Reg.ESI, Reg.EDI):
        val = machine.cpu[r]
        if val is not None:
            parts.append(f"{r.name}={val.type.name}@{hex(val.base)}+{val.offset}")
    return " ".join(parts) if parts else "(empty)"


def build_known(sess, func_rva: int) -> tuple[Stack, dict[int, tuple[str, str]]]:
    """Build PDB known stack locals."""
    from pydia2 import cvconst
    known = Stack()
    raw: dict[int, tuple[str, str]] = {}
    try:
        sym = sess.session.findSymbolByRVA(func_rva, cvconst.SymTag.Function)
        if not sym:
            return known, raw
        data = sym.findChildren(cvconst.SymTag.Data, None, 0)
        for i in range(min(data.count, 200)):
            child = data.Item(i)
            try:
                loc = int(child.locationType)
                if loc == 2:
                    name = child.name or ""
                    typ = pdb_mod.type_name(child.type) if child.type else ""
                    off = int(child.offset)
                    size = int(child.type.length) if child.type else 4
                    if name and name != "this":
                        raw[off] = (name, typ)
                        try:
                            known[off] = Symbol(name=f"{typ} {name}", size=max(size, 1))
                        except KeyError:
                            pass
            except Exception:
                continue
    except Exception:
        pass
    return known, raw


def extract_class_from_call(symbol: str) -> str:
    """Extract class name from a call symbol like 'CStr::CStr' or 'CStr::operator='."""
    if "::" not in symbol:
        return ""
    parts = symbol.rsplit("::", 1)
    return parts[0]


# Global symbol type cache
_global_type_cache: dict[str, str] = {}

def resolve_global_type(sess, symbol: str) -> str:
    """Resolve a global symbol's PDB type (e.g. 'm3d::g_Kernel' → 'm3d::Kernel*')."""
    if symbol in _global_type_cache:
        return _global_type_cache[symbol]
    from pydia2 import cvconst
    try:
        enum = sess.global_scope.findChildren(cvconst.SymTag.Data, symbol, 0)
        if enum.count > 0:
            tname = pdb_mod.type_name(enum.Item(0).type)
            if tname:
                _global_type_cache[symbol] = tname
                return tname
    except Exception:
        pass
    _global_type_cache[symbol] = ""
    return ""


# Field resolution cache
_field_cache: dict[tuple[str, int], tuple[str, str]] = {}

def resolve_field(sess, class_name: str, offset: int) -> tuple[str, str]:
    """Resolve [class + offset] → (field_name, field_type) via PDB."""
    key = (class_name, offset)
    if key in _field_cache:
        return _field_cache[key]

    from pydia2 import cvconst

    # Strip pointer
    base = class_name.rstrip("* ")
    if not base:
        _field_cache[key] = ("", "")
        return ("", "")

    def _collect(udt_name, base_off, visited):
        if udt_name in visited:
            return []
        visited.add(udt_name)
        enum_res = sess.global_scope.findChildren(cvconst.SymTag.UDT, udt_name, 0)
        if enum_res.count == 0:
            return []
        udt = enum_res.Item(0)
        result = []
        # Own fields
        children = udt.findChildren(cvconst.SymTag.Data, None, 0)
        for i in range(min(children.count, 2000)):
            f = children.Item(i)
            try:
                o = int(f.offset)
                n = f.name or ""
                t = pdb_mod.type_name(f.type) if f.type else "?"
                result.append((base_off + o, n, t or "?"))
            except Exception:
                pass
        # Base classes
        bases = udt.findChildren(cvconst.SymTag.BaseClass, None, 0)
        for i in range(bases.count):
            b = bases.Item(i)
            try:
                bname = b.type.name if b.type else b.name
                boff = int(b.offset)
                result.extend(_collect(bname, base_off + boff, visited))
            except Exception:
                pass
        return result

    fields = _collect(base, 0, set())
    fields.sort(key=lambda x: x[0])

    # Exact match
    for o, n, t in fields:
        if o == offset:
            _field_cache[key] = (n, t)
            return (n, t)

    # Nearest before
    before = [(o, n, t) for o, n, t in fields if o < offset]
    if before:
        o, n, t = before[-1]
        delta = offset - o
        _field_cache[key] = (f"{n}+{hex(delta)}", t)
        return (f"{n}+{hex(delta)}", t)

    _field_cache[key] = (f"field_{hex(offset)}", "")
    return (f"field_{hex(offset)}", "")


def main():
    target_lex = int(sys.argv[1]) if len(sys.argv) > 1 else 1498

    mgr = SessionManager()
    sess = mgr.load("target/hta.exe", "target/game.pdb")
    func_rva, func_len, func_name = resolve_function(sess, "m3d::Application::init")
    print(f"{func_name} — lexical group L{target_lex}\n")

    # PDB known locals
    known, raw_locals = build_known(sess, func_rva)
    if raw_locals:
        print(f"PDB known stack locals ({len(raw_locals)}):")
        for off in sorted(raw_locals.keys()):
            name, typ = raw_locals[off]
            print(f"  [{off:+5d}]  {typ:30s}  {name}")
        print()
    else:
        print("PDB known stack locals: (none)\n")

    stream = decode(sess, func_rva, func_len)
    group = [n for n in stream if n.lexical == target_lex]
    print(f"Raw IR: {len(group)} nodes\n")

    for i, node in enumerate(group):
        mn = node.mn.value if node.mn else "???"
        ops = ", ".join(op_str(o) for o in node.ops)
        print(f"  {i:3d}  {hex(node.rva):10s}  {type(node).__name__:10s}  {mn:8s}  {ops}")

    # ── Fold with slot tracking ──────────────────────────────────
    print(f"\nFolded (with slot tracking):\n")

    machine = Machine()
    push_stack: list[IROp] = []
    ecx_op: IROp | None = None

    # Interval-based stack map: disp → Symbol(type, size)
    # Ctor calls and known locals populate this.
    # Any access to a disp within range returns RelativeAddr(base, offset_into_type, type).
    from provider.lora.persifal._vm import Stack as VMStack
    stack_map = VMStack()

    # Seed this class from calling convention
    this_class = ""
    idx = func_name.rfind("::")
    if idx > 0:
        this_class = func_name[:idx]

    # Register types: reg → type_name (lightweight type tracking)
    reg_types: dict[str, str] = {}
    if this_class:
        reg_types["ecx"] = f"{this_class}*"
        print(f"  Seeded: ecx = {this_class}* (this)\n")

    def resolve_op(op: IROp) -> str:
        s = op_str(op)
        if op.kind == "reg":
            if op.reg in reg_types:
                s += f"  → {reg_types[op.reg]}"
        if op.kind == "mem" and op.mem_base in ("ebp", "esp"):
            disp = op.mem_disp
            if disp in stack_map:
                ref = stack_map[disp]
                if ref.offset == 0:
                    s += f"  → {ref.type.name}"
                else:
                    # Sub-field: ask PDB for field at offset within the type
                    fname, ftype = resolve_field(sess, ref.type.name, ref.offset)
                    if fname:
                        s += f"  → {ref.type.name}.{fname} ({ftype})"
                    else:
                        s += f"  → {ref.type.name}+{hex(ref.offset)}"
            elif disp in known:
                ref = known[disp]
                s += f"  → {ref.type.name}"
        # mem with typed base reg: [esi+0x30c] where esi = Application*
        if op.kind == "mem" and op.mem_base and op.mem_base in reg_types:
            base_type = reg_types[op.mem_base]
            fname, ftype = resolve_field(sess, base_type, op.mem_disp)
            if fname:
                s += f"  → .{fname} ({ftype})"
            else:
                s += f"  ({base_type}+{hex(op.mem_disp)})"
        return s

    def try_resolve_slot(disp: int, call_symbol: str) -> None:
        """First access to a slot via call — resolve type from call signature."""
        if disp in stack_map:
            return  # already known
        cls = extract_class_from_call(call_symbol)
        if cls:
            # Get PDB size for the type
            from pydia2 import cvconst
            size = 4
            try:
                udts = sess.global_scope.findChildren(cvconst.SymTag.UDT, cls, 0)
                if udts.count > 0:
                    size = int(udts.Item(0).length)
            except Exception:
                pass
            try:
                stack_map[disp] = Symbol(name=cls, size=max(size, 1))
                print(f"        *** SLOT [{disp:+d}] = {cls} ({size}B)  (from {call_symbol})")
            except KeyError:
                print(f"        *** SLOT [{disp:+d}] overlap! {cls}")

    def try_resolve_slot_from_store(disp: int) -> None:
        """First mov to a slot — check known."""
        if disp in stack_map:
            return
        if disp in known:
            ref = known[disp]
            try:
                stack_map[disp] = ref.type
                print(f"        *** SLOT [{disp:+d}] = {ref.type.name}  (from PDB)")
            except KeyError:
                pass

    gs = IRStream(group)
    while gs.has_next():
        node = gs.peek()

        if isinstance(node, IRPush):
            gs.shift()
            if node.ops:
                push_stack.append(node.ops[0])
            machine.eat(node)
            continue

        if isinstance(node, IRCall):
            gs.shift()
            target = ""
            if node.ops and node.ops[0].symbol:
                target = node.ops[0].symbol
            elif node.ops and node.ops[0].kind == "imm":
                target = hex(node.ops[0].imm)
            elif node.ops:
                target = f"indirect {op_str(node.ops[0])}"
            args = list(reversed(push_stack))
            ecx_s = resolve_op(ecx_op) if ecx_op else ""
            args_s = ", ".join(resolve_op(a) for a in args)
            print(f"  CALL  {target}({args_s})"
                  f"{'  this=' + ecx_s if ecx_s else ''}")

            # Slot resolution: if ecx was lea [esp+disp], this is a ctor/method on that slot
            if ecx_op and ecx_op.kind == "mem" and ecx_op.mem_base in ("esp", "ebp"):
                try_resolve_slot(ecx_op.mem_disp, target)

            push_stack.clear()
            # Call clobbers eax/ecx/edx types
            for r in ("eax", "ecx", "edx"):
                reg_types.pop(r, None)
            # eax gets return type (class name from call if method)
            ret_cls = extract_class_from_call(target)
            if ret_cls:
                reg_types["eax"] = f"{ret_cls}*"
            machine.eat(node)
            nxt = gs.peek()
            if isinstance(nxt, IRArith) and nxt.mn == Mn.ADD and nxt.ops and nxt.ops[0].reg == "esp":
                gs.shift()
                machine.eat(nxt)
            continue

        if isinstance(node, IRCmp):
            m = gs.check(IRCmp, IRJcc)
            if m:
                cmp_n, jcc_n = m
                gs.advance(2)
                lhs = resolve_op(cmp_n.ops[0]) if cmp_n.ops else "?"
                rhs = resolve_op(cmp_n.ops[1]) if len(cmp_n.ops) > 1 else "?"
                op = _JCC_OP.get(jcc_n.mn, "?")
                if cmp_n.mn == Mn.TEST and cmp_n.ops[0].reg == cmp_n.ops[1].reg:
                    rhs = "0"
                print(f"  COND  {lhs} {op} {rhs}  → {hex(jcc_n.target_rva)}")
                machine.eat(cmp_n)
                machine.eat(jcc_n)
                continue
            gs.shift()
            machine.eat(node)
            continue

        if isinstance(node, IRJcc):
            gs.shift()
            print(f"  JCC   {node.mn.value if node.mn else '?'}  → {hex(node.target_rva)}")
            machine.eat(node)
            continue

        if isinstance(node, IRMov):
            gs.shift()
            if len(node.ops) >= 2:
                dst, src = node.ops[0], node.ops[1]
                if dst.kind == "mem":
                    print(f"  STORE {resolve_op(dst)} = {resolve_op(src)}")
                    # First store to stack slot → check known
                    if dst.mem_base in ("esp", "ebp"):
                        try_resolve_slot_from_store(dst.mem_disp)
                elif dst.kind == "reg" and src.kind in ("mem", "imm"):
                    print(f"  LOAD  {dst.reg} = {resolve_op(src)}")
                    # Propagate type from stack → register
                    if src.kind == "mem" and src.mem_base in ("esp", "ebp") and src.mem_disp in stack_map:
                        ref = stack_map[src.mem_disp]
                        if ref.offset == 0:
                            reg_types[dst.reg] = ref.type.name
                        else:
                            # Sub-field load: resolve field type
                            _, ftype = resolve_field(sess, ref.type.name, ref.offset)
                            if ftype and ftype != "?":
                                reg_types[dst.reg] = ftype
                            else:
                                reg_types.pop(dst.reg, None)
                    elif src.kind == "mem" and src.mem_base and src.mem_base in reg_types:
                        # Field access: resolve field type → register type
                        _, ftype = resolve_field(sess, reg_types[src.mem_base], src.mem_disp)
                        if ftype and ftype != "?":
                            reg_types[dst.reg] = ftype
                        else:
                            reg_types.pop(dst.reg, None)
                    elif src.symbol:
                        # Global symbol: resolve its PDB type
                        gtype = resolve_global_type(sess, src.symbol)
                        if gtype:
                            reg_types[dst.reg] = gtype
                        else:
                            reg_types.pop(dst.reg, None)
                    else:
                        reg_types.pop(dst.reg, None)
                elif dst.kind == "reg" and src.kind == "reg":
                    print(f"  MOV   {dst.reg} = {resolve_op(src)}")
                    # Propagate type reg → reg
                    if src.reg in reg_types:
                        reg_types[dst.reg] = reg_types[src.reg]
                    else:
                        reg_types.pop(dst.reg, None)
                if dst.kind == "reg" and dst.reg == "ecx":
                    ecx_op = src
            machine.eat(node)
            continue

        if isinstance(node, IRArith) and node.mn == Mn.XOR and len(node.ops) >= 2:
            a, b = node.ops[0], node.ops[1]
            if a.kind == "reg" and b.kind == "reg" and a.reg == b.reg:
                gs.shift()
                print(f"  ZERO  {a.reg}")
                reg_types.pop(a.reg, None)
                machine.eat(node)
                continue

        if isinstance(node, IRArith) and node.mn == Mn.ADD and node.ops and node.ops[0].reg == "esp":
            gs.shift()
            machine.eat(node)
            continue

        if isinstance(node, IRArith) and node.mn == Mn.SUB and node.ops and node.ops[0].reg == "esp":
            gs.shift()
            print(f"  FRAME alloc {op_str(node.ops[1])}")
            machine.eat(node)
            continue

        if isinstance(node, IRJmp):
            gs.shift()
            print(f"  JMP   → {hex(node.target_rva)}")
            machine.eat(node)
            continue

        if isinstance(node, IRRet):
            gs.shift()
            print(f"  RET")
            machine.eat(node)
            continue

        gs.shift()
        mn = node.mn.value if node.mn else "???"
        print(f"  ???   {mn}  {', '.join(op_str(o) for o in node.ops)}")
        machine.eat(node)

    # Summary
    all_slots = list(stack_map)
    if all_slots:
        print(f"\nStack map:")
        for ref in all_slots:
            print(f"  [{ref.base:+5d}]  {ref.type.name:20s}  ({ref.type.size}B)")


if __name__ == "__main__":
    main()
