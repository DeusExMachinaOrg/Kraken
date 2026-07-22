"""Smoke test for persifal v2 foundation: IR, Stream, VM."""

import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from persifal._ir import (
    IR, IROp, Mn, to_mn, classify,
    IRMov, IRPush, IRPop, IRCall, IRCmp, IRJcc, IRJmp, IRRet, IRArith, IRNop,
)
from persifal._stream import IRStream
from persifal._vm import Reg, to_reg, CPUx86, Symbol, RelativeAddr, Stack, Scope, Machine


# ── IR tests ─────────────────────────────────────────────────────────

def test_mn_enum():
    assert Mn.MOV == "mov"
    assert Mn.CALL == "call"
    assert to_mn("push") is Mn.PUSH
    assert to_mn("garbage") is None

def test_classify():
    cls, mn = classify("mov")
    assert cls is IRMov and mn is Mn.MOV

    cls, mn = classify("je")
    assert cls is IRJcc and mn is Mn.JE

    cls, mn = classify("unknown_insn")
    assert cls is IR and mn is None

def test_ir_node():
    op = IROp(kind="reg", reg="eax", size=4, symbol="g_Kernel")
    node = IRMov(mn=Mn.MOV, rva=0x1000, lexical=42, ops=[op])
    assert node.rva == 0x1000
    assert node.lexical == 42
    assert node.ops[0].symbol == "g_Kernel"

def test_lexical_default():
    node = IR()
    assert node.lexical == -1


# ── Stream tests ─────────────────────────────────────────────────────

def _make_stream():
    return IRStream([
        IRPush(mn=Mn.PUSH, rva=0x100),
        IRPush(mn=Mn.PUSH, rva=0x102),
        IRCall(mn=Mn.CALL, rva=0x104),
        IRArith(mn=Mn.ADD, rva=0x106),
        IRRet(mn=Mn.RET, rva=0x108),
    ])

def test_stream_basics():
    s = _make_stream()
    assert len(s) == 5
    assert s.has_next()
    assert s.remaining() == 5

def test_stream_peek_shift():
    s = _make_stream()
    assert isinstance(s.peek(), IRPush)
    assert isinstance(s.peek(1), IRPush)
    assert isinstance(s.peek(2), IRCall)

    node = s.shift()
    assert isinstance(node, IRPush) and node.rva == 0x100
    assert s.pos == 1
    assert s.remaining() == 4

def test_stream_check_match():
    s = _make_stream()
    m = s.check(IRPush, IRPush, IRCall)
    assert m is not None
    assert len(m) == 3
    assert isinstance(m[0], IRPush)
    assert isinstance(m[2], IRCall)
    # check doesn't consume
    assert s.pos == 0

def test_stream_check_no_match():
    s = _make_stream()
    assert s.check(IRPush, IRCall) is None  # push, push — not push, call
    assert s.check(IRCall) is None

def test_stream_check_past_end():
    s = _make_stream()
    s.advance(4)
    assert s.check(IRRet, IRRet) is None  # only 1 left

def test_stream_advance():
    s = _make_stream()
    s.advance(3)
    assert s.pos == 3
    assert isinstance(s.peek(), IRArith)

def test_stream_frozen():
    s = _make_stream()
    assert not hasattr(s, 'replace')
    assert not hasattr(s, 'insert')
    assert not hasattr(s, 'remove')

def test_stream_iter():
    s = _make_stream()
    s.advance(3)
    remaining = list(s)
    assert len(remaining) == 2

def test_stream_reset():
    s = _make_stream()
    s.advance(3)
    s.reset()
    assert s.pos == 0


# ── VM tests ─────────────────────────────────────────────────────────

def test_reg_enum():
    assert to_reg("eax") is Reg.EAX
    assert to_reg("ecx") is Reg.ECX
    assert to_reg("garbage") is None

def test_cpu_canonical():
    cpu = CPUx86()
    val = RelativeAddr(base=0, offset=0, type=Symbol("int", 4))
    cpu[Reg.EAX] = val
    assert cpu[Reg.EAX] == val
    assert Reg.EAX in cpu

def test_cpu_subreg_invalidates():
    cpu = CPUx86()
    val = RelativeAddr(base=0, offset=0, type=Symbol("int", 4))
    cpu[Reg.EAX] = val
    # writing to AL invalidates EAX
    cpu[Reg.AL] = RelativeAddr(base=1, offset=0, type=Symbol("char", 1))
    assert cpu[Reg.EAX] is None

def test_cpu_delete():
    cpu = CPUx86()
    val = RelativeAddr(base=0, offset=0, type=Symbol("int", 4))
    cpu[Reg.ECX] = val
    del cpu[Reg.ECX]
    assert Reg.ECX not in cpu

def test_stack_interval():
    st = Stack()
    st[100] = Symbol("CStr", 12)
    assert 100 in st
    assert 105 in st  # inside range
    assert 112 not in st  # past end

    r = st[105]
    assert r.base == 100
    assert r.offset == 5
    assert r.type.name == "CStr"

def test_stack_overlap():
    st = Stack()
    st[100] = Symbol("CStr", 12)
    try:
        st[105] = Symbol("int", 4)  # overlaps [100, 112)
        assert False, "should have raised"
    except KeyError:
        pass

def test_scope_chain():
    parent = Scope()
    parent[100] = Symbol("CStr", 12)

    child = parent.enter()
    child[200] = Symbol("int", 4)

    # child sees both
    assert 100 in child
    assert 200 in child
    # parent only sees its own
    assert 100 in parent
    assert 200 not in parent

    # exit returns parent
    back = child.exit()
    assert 200 not in back

def test_machine_eat_mov_reg_reg():
    m = Machine()
    val = RelativeAddr(base=0, offset=0, type=Symbol("int", 4))
    m.cpu[Reg.ESI] = val

    # mov ecx, esi
    node = IRMov(mn=Mn.MOV, rva=0x100, ops=[
        IROp(kind="reg", reg="ecx"),
        IROp(kind="reg", reg="esi"),
    ])
    m.eat(node)
    assert m.cpu[Reg.ECX] == val

def test_machine_eat_call_clobbers():
    m = Machine()
    val = RelativeAddr(base=0, offset=0, type=Symbol("int", 4))
    m.cpu[Reg.EAX] = val
    m.cpu[Reg.ECX] = val
    m.cpu[Reg.EBX] = val  # callee-saved

    node = IRCall(mn=Mn.CALL, rva=0x200)
    m.eat(node)

    assert Reg.EAX not in m.cpu  # clobbered
    assert Reg.ECX not in m.cpu  # clobbered
    assert Reg.EBX in m.cpu      # preserved

def test_machine_eat_xor_zero():
    m = Machine()
    val = RelativeAddr(base=0, offset=0, type=Symbol("int", 4))
    m.cpu[Reg.EAX] = val

    node = IRArith(mn=Mn.XOR, rva=0x300, ops=[
        IROp(kind="reg", reg="eax"),
        IROp(kind="reg", reg="eax"),
    ])
    m.eat(node)
    assert Reg.EAX not in m.cpu


# ── Run all ──────────────────────────────────────────────────────────

if __name__ == "__main__":
    tests = [v for k, v in globals().items() if k.startswith("test_")]
    passed = 0
    failed = 0
    for t in tests:
        try:
            t()
            passed += 1
            print(f"  OK  {t.__name__}")
        except Exception as e:
            failed += 1
            print(f"  FAIL {t.__name__}: {e}")
    print(f"\n{passed} passed, {failed} failed")
