"""Virtual machine state for fold stages.

CPU — register state with sub-register aliasing.
Stack — interval-based stack map with overlap detection.
Scope — scoped local variable tracking with parent chain.
Context — CPU + Stack + Scope, the full machine state.

Fold stages advance the VM by feeding IR nodes, then query it
to resolve what operands refer to (field chains, call targets,
constructor/destructor patterns, etc.).

Type system: uses Type from _types.py (pure data, no PDB/DIA).
"""

from __future__ import annotations

import bisect
import enum
import typing

from ._types import Type, VOID


# ── Registers ────────────────────────────────────────────────────────

class Reg(enum.Enum):
    EAX = enum.auto()
    AX = enum.auto()
    AL = enum.auto()
    AH = enum.auto()
    ECX = enum.auto()
    CX = enum.auto()
    CL = enum.auto()
    CH = enum.auto()
    EDX = enum.auto()
    DX = enum.auto()
    DL = enum.auto()
    DH = enum.auto()
    EBX = enum.auto()
    BX = enum.auto()
    BL = enum.auto()
    BH = enum.auto()
    ESP = enum.auto()
    SP = enum.auto()
    SPL = enum.auto()
    EBP = enum.auto()
    BP = enum.auto()
    BPL = enum.auto()
    ESI = enum.auto()
    SI = enum.auto()
    SIL = enum.auto()
    EDI = enum.auto()
    DI = enum.auto()
    DIL = enum.auto()
    EIP = enum.auto()
    EFLAGS = enum.auto()
    ST0 = enum.auto()
    ST1 = enum.auto()
    ST2 = enum.auto()
    ST3 = enum.auto()
    ST4 = enum.auto()
    ST5 = enum.auto()
    ST6 = enum.auto()
    ST7 = enum.auto()
    MM0 = enum.auto()
    MM1 = enum.auto()
    MM2 = enum.auto()
    MM3 = enum.auto()
    MM4 = enum.auto()
    MM5 = enum.auto()
    MM6 = enum.auto()
    MM7 = enum.auto()
    XMM0 = enum.auto()
    XMM1 = enum.auto()
    XMM2 = enum.auto()
    XMM3 = enum.auto()
    XMM4 = enum.auto()
    XMM5 = enum.auto()
    XMM6 = enum.auto()
    XMM7 = enum.auto()


# Capstone name → Reg lookup
_REG_LOOKUP: dict[str, Reg] = {r.name.lower(): r for r in Reg}

def to_reg(name: str) -> Reg | None:
    """Convert Capstone register string to Reg enum."""
    return _REG_LOOKUP.get(name.lower())


# ── Relative address ─────────────────────────────────────────────────

class RelativeAddr(typing.NamedTuple):
    """Resolved reference: base address + offset into a typed slot."""
    base: int
    offset: int
    type: Type
    var_name: str = ""


# ── CPU state ────────────────────────────────────────────────────────

class CPU(typing.Protocol):
    def __setitem__(self, reg: Reg, value: RelativeAddr) -> None: ...
    def __getitem__(self, reg: Reg) -> RelativeAddr | None: ...
    def __delitem__(self, reg: Reg) -> None: ...
    def __contains__(self, reg: Reg) -> bool: ...


class CPUx86:
    """x86 register state with sub-register aliasing.

    Writing to a sub-register (AL, AH, AX) invalidates the canonical
    register (EAX) — we lose track of the full value.
    Writing to the canonical sets the known value.
    """

    class Descriptor(typing.NamedTuple):
        canonical: Reg
        size: int
        base: int

    _DESCRIPTORS: dict[Reg, Descriptor] = {
        Reg.EAX: Descriptor(Reg.EAX, 4, 0),
        Reg.AX: Descriptor(Reg.EAX, 2, 0),
        Reg.AL: Descriptor(Reg.EAX, 1, 0),
        Reg.AH: Descriptor(Reg.EAX, 1, 1),
        Reg.ECX: Descriptor(Reg.ECX, 4, 0),
        Reg.CX: Descriptor(Reg.ECX, 2, 0),
        Reg.CL: Descriptor(Reg.ECX, 1, 0),
        Reg.CH: Descriptor(Reg.ECX, 1, 1),
        Reg.EDX: Descriptor(Reg.EDX, 4, 0),
        Reg.DX: Descriptor(Reg.EDX, 2, 0),
        Reg.DL: Descriptor(Reg.EDX, 1, 0),
        Reg.DH: Descriptor(Reg.EDX, 1, 1),
        Reg.EBX: Descriptor(Reg.EBX, 4, 0),
        Reg.BX: Descriptor(Reg.EBX, 2, 0),
        Reg.BL: Descriptor(Reg.EBX, 1, 0),
        Reg.BH: Descriptor(Reg.EBX, 1, 1),
        Reg.ESP: Descriptor(Reg.ESP, 4, 0),
        Reg.SP: Descriptor(Reg.ESP, 2, 0),
        Reg.SPL: Descriptor(Reg.ESP, 1, 0),
        Reg.EBP: Descriptor(Reg.EBP, 4, 0),
        Reg.BP: Descriptor(Reg.EBP, 2, 0),
        Reg.BPL: Descriptor(Reg.EBP, 1, 0),
        Reg.ESI: Descriptor(Reg.ESI, 4, 0),
        Reg.SI: Descriptor(Reg.ESI, 2, 0),
        Reg.SIL: Descriptor(Reg.ESI, 1, 0),
        Reg.EDI: Descriptor(Reg.EDI, 4, 0),
        Reg.DI: Descriptor(Reg.EDI, 2, 0),
        Reg.DIL: Descriptor(Reg.EDI, 1, 0),
        Reg.EIP: Descriptor(Reg.EIP, 4, 0),
        Reg.EFLAGS: Descriptor(Reg.EFLAGS, 4, 0),
        Reg.ST0: Descriptor(Reg.ST0, 10, 0),
        Reg.ST1: Descriptor(Reg.ST1, 10, 0),
        Reg.ST2: Descriptor(Reg.ST2, 10, 0),
        Reg.ST3: Descriptor(Reg.ST3, 10, 0),
        Reg.ST4: Descriptor(Reg.ST4, 10, 0),
        Reg.ST5: Descriptor(Reg.ST5, 10, 0),
        Reg.ST6: Descriptor(Reg.ST6, 10, 0),
        Reg.ST7: Descriptor(Reg.ST7, 10, 0),
        Reg.MM0: Descriptor(Reg.MM0, 8, 0),
        Reg.MM1: Descriptor(Reg.MM1, 8, 0),
        Reg.MM2: Descriptor(Reg.MM2, 8, 0),
        Reg.MM3: Descriptor(Reg.MM3, 8, 0),
        Reg.MM4: Descriptor(Reg.MM4, 8, 0),
        Reg.MM5: Descriptor(Reg.MM5, 8, 0),
        Reg.MM6: Descriptor(Reg.MM6, 8, 0),
        Reg.MM7: Descriptor(Reg.MM7, 8, 0),
        Reg.XMM0: Descriptor(Reg.XMM0, 16, 0),
        Reg.XMM1: Descriptor(Reg.XMM1, 16, 0),
        Reg.XMM2: Descriptor(Reg.XMM2, 16, 0),
        Reg.XMM3: Descriptor(Reg.XMM3, 16, 0),
        Reg.XMM4: Descriptor(Reg.XMM4, 16, 0),
        Reg.XMM5: Descriptor(Reg.XMM5, 16, 0),
        Reg.XMM6: Descriptor(Reg.XMM6, 16, 0),
        Reg.XMM7: Descriptor(Reg.XMM7, 16, 0),
    }

    def __init__(self) -> None:
        self._state: dict[Reg, RelativeAddr | None] = {}

    def __setitem__(self, reg: Reg, value: RelativeAddr) -> None:
        desc = self._DESCRIPTORS[reg]
        canon = self._DESCRIPTORS[desc.canonical]
        if desc.base != 0 or desc.size < canon.size:
            # Sub-register write — invalidate canonical
            self._state[desc.canonical] = None
        else:
            self._state[desc.canonical] = value

    def __getitem__(self, reg: Reg) -> RelativeAddr | None:
        desc = self._DESCRIPTORS[reg]
        return self._state.get(desc.canonical)

    def __delitem__(self, reg: Reg) -> None:
        desc = self._DESCRIPTORS[reg]
        self._state.pop(desc.canonical, None)

    def __contains__(self, reg: Reg) -> bool:
        desc = self._DESCRIPTORS[reg]
        return desc.canonical in self._state


# ── Stack (interval map) ─────────────────────────────────────────────

class Stack:
    """Interval-based stack map: addr → Type with overlap detection.

    Supports lookup by any address within a type's range,
    returning RelativeAddr(base, offset_into_type, type, var_name).
    """

    __slots__ = ("_head", "_tail", "_data", "_rva", "_names")

    def __init__(self) -> None:
        self._head: list[int] = []
        self._tail: list[int] = []
        self._data: list[Type] = []
        self._rva: list[int] = []      # instruction RVA that created each slot
        self._names: list[str] = []    # variable name per slot

    def set(self, addr: int, data: Type, rva: int = 0,
            var_name: str = "") -> None:
        """Insert a slot with origin RVA and variable name tracking."""
        self._set_impl(addr, data, rva, var_name)

    def __setitem__(self, addr: int, data: Type) -> None:
        self._set_impl(addr, data, 0, "")

    def _set_impl(self, addr: int, data: Type, rva: int,
                  var_name: str) -> None:
        head = addr
        tail = addr + data.size
        count = len(self._data)
        pos = bisect.bisect_right(self._head, head)

        if pos > 0 and self._tail[pos - 1] > head:
            existing = self._data[pos - 1]
            origin = hex(self._rva[pos - 1]) if self._rva[pos - 1] else "?"
            raise KeyError(
                f"Stack overlap at {addr}: [{self._head[pos-1]},{self._tail[pos-1]}) "
                f"{existing.name}@{origin} vs new [{head},{tail}) {data.name}@{hex(rva)}")
        if pos < count and tail > self._head[pos]:
            existing = self._data[pos]
            origin = hex(self._rva[pos]) if self._rva[pos] else "?"
            raise KeyError(
                f"Stack overlap at {addr}: [{self._head[pos]},{self._tail[pos]}) "
                f"{existing.name}@{origin} vs new [{head},{tail}) {data.name}@{hex(rva)}")

        self._head.insert(pos, head)
        self._tail.insert(pos, tail)
        self._data.insert(pos, data)
        self._rva.insert(pos, rva)
        self._names.insert(pos, var_name)

    def __getitem__(self, addr: int) -> RelativeAddr:
        pos = bisect.bisect_right(self._head, addr) - 1
        if pos < 0 or addr >= self._tail[pos]:
            raise KeyError(f"Stack has no type info for addr: {addr}")
        return RelativeAddr(
            base=self._head[pos],
            offset=addr - self._head[pos],
            type=self._data[pos],
            var_name=self._names[pos],
        )

    def __delitem__(self, addr: int) -> None:
        pos = bisect.bisect_right(self._head, addr) - 1
        if pos < 0 or addr >= self._tail[pos]:
            raise KeyError(f"Stack has no type info for addr: {addr}")
        del self._head[pos]
        del self._tail[pos]
        del self._data[pos]
        del self._rva[pos]
        del self._names[pos]

    def __contains__(self, addr: int) -> bool:
        pos = bisect.bisect_right(self._head, addr) - 1
        return pos >= 0 and addr < self._tail[pos]

    def __iter__(self):
        for head, data, name in zip(self._head, self._data, self._names):
            yield RelativeAddr(head, 0, data, name)

    def __ior__(self, other: Stack) -> Stack:
        for rel in other:
            self[rel.base] = rel.type
        return self


# ── Scope (chained stack locals) ─────────────────────────────────────

class Scope:
    """Scoped local variable tracking with parent chain.

    Each block (if/loop) gets a child Scope. Lookup walks parents.
    Exiting a scope drops its locals.
    """

    __slots__ = ("parent", "locals", "esp_base")

    def __init__(self, parent: Scope | None = None, esp_base: int = 0) -> None:
        self.parent = parent
        self.locals = Stack()
        self.esp_base = esp_base  # esp_delta when this scope was entered

    def __setitem__(self, addr: int, value: Type) -> None:
        self.locals[addr] = value

    def __getitem__(self, addr: int) -> RelativeAddr:
        if addr in self.locals:
            return self.locals[addr]
        if self.parent:
            return self.parent[addr]
        raise KeyError(f"No local at addr: {addr}")

    def __delitem__(self, addr: int) -> None:
        if addr in self.locals:
            del self.locals[addr]
        elif self.parent:
            del self.parent[addr]
        else:
            raise KeyError(f"No local at addr: {addr}")

    def __contains__(self, addr: int) -> bool:
        return addr in self.locals or (
            self.parent is not None and addr in self.parent
        )

    def enter(self, esp_delta: int = 0) -> Scope:
        """Enter a new block scope, remembering esp_delta at entry."""
        return Scope(parent=self, esp_base=esp_delta)

    def exit(self) -> Scope:
        """Exit current scope, return parent."""
        return self.parent or self


# ── Machine (eats IR, tracks runtime state) ─────────────────────────

class Machine:
    """Executes raw IR nodes and maintains runtime state.

    cpu   — register values (RelativeAddr or None)
    scope — runtime scoped locals (ctor → alive, dtor → dead)

    Machine does NOT hold PDB declarations. Fold stages own a separate
    `known: Stack` (PDB-declared locals) and query it themselves when
    they encounter an uninitialized stack slot — "do we know this segment,
    or must we emit a raw stack slot?"
    """

    __slots__ = ("cpu", "scope", "esp_delta", "fpu_cmp", "fpu_in_eax")

    def __init__(self, cpu: CPU | None = None) -> None:
        self.cpu: CPU = cpu or CPUx86()
        self.scope = Scope()
        self.esp_delta: int = 0   # cumulative esp offset from function entry
        # FPU compare state: tracks fcomp→fnstsw→sahf idiom
        self.fpu_cmp: tuple[str, str] | None = None  # (st_lhs, st_rhs) from fcomp/fucomp
        self.fpu_in_eax: bool = False                 # fnstsw ax transferred status to eax

    def vframe(self, esp_disp: int) -> int:
        """Convert [esp+disp] to PDB vframe offset: disp + esp_delta."""
        return esp_disp + self.esp_delta

    def push_scope(self) -> None:
        """Enter a new scope. Saves esp_delta, pushes child scope."""
        self.scope = self.scope.enter(self.esp_delta)

    def pop_scope(self) -> None:
        """Exit scope. Restores esp_delta, drops scope locals."""
        self.esp_delta = self.scope.esp_base
        self.scope = self.scope.exit()

    def eat(self, node) -> None:
        """Feed an IR node, update CPU/scope/esp_delta.

          IRPush           — esp_delta -= 4
          IRPop            — esp_delta += 4
          IRArith(sub esp) — esp_delta -= N
          IRArith(add esp) — esp_delta += N
          IRMov(reg, reg)  — propagate value
          IRMov(reg, mem)  — track if scope knows the source
          IRArith(xor r,r) — zero
          IRArith(dst, ..) — invalidate dst
          IRCall           — clobber caller-saved (eax, ecx, edx)
        """
        from ._ir import (IRMov, IRPush, IRPop, IRCall, IRArith,
                         IRCmp, IRNop, Mn)

        match node:
            case IRPush():
                self.esp_delta -= 4

            case IRPop():
                self.esp_delta += 4

            case IRArith(mn=Mn.SUB, ops=[dst, imm]) if (
                dst.kind == "reg" and dst.reg == "esp" and imm.kind == "imm"
            ):
                self.esp_delta -= imm.imm

            case IRArith(mn=Mn.ADD, ops=[dst, imm]) if (
                dst.kind == "reg" and dst.reg == "esp" and imm.kind == "imm"
            ):
                self.esp_delta += imm.imm

            case IRMov(mn=Mn.MOV, ops=[dst, src]) if dst.kind == "reg" and src.kind == "mem":
                reg = to_reg(dst.reg)
                if reg:
                    addr = self._resolve_mem(src)
                    if addr is not None:
                        self.cpu[reg] = addr
                    else:
                        del self.cpu[reg]

            case IRMov(mn=Mn.MOV, ops=[dst, src]) if dst.kind == "reg" and src.kind == "reg":
                src_reg = to_reg(src.reg)
                dst_reg = to_reg(dst.reg)
                if src_reg and dst_reg:
                    val = self.cpu[src_reg]
                    if val is not None:
                        self.cpu[dst_reg] = val
                    else:
                        del self.cpu[dst_reg]

            case IRMov(mn=Mn.LEA, ops=[dst, _]) if dst.kind == "reg":
                reg = to_reg(dst.reg)
                if reg:
                    del self.cpu[reg]

            case IRMov(ops=[dst, _]) if dst.kind == "reg":
                reg = to_reg(dst.reg)
                if reg:
                    del self.cpu[reg]

            case IRArith(mn=Mn.XOR, ops=[a, b]) if (
                a.kind == "reg" and b.kind == "reg" and a.reg == b.reg
            ):
                reg = to_reg(a.reg)
                if reg:
                    del self.cpu[reg]

            case IRArith(ops=[dst, *_]) if dst.kind == "reg":
                reg = to_reg(dst.reg)
                if reg:
                    del self.cpu[reg]

            case IRCall(cleanup=cleanup):
                for r in (Reg.EAX, Reg.ECX, Reg.EDX):
                    if r in self.cpu:
                        del self.cpu[r]
                if cleanup > 0:
                    self.esp_delta += cleanup
                self.fpu_cmp = None
                self.fpu_in_eax = False

            # ── FPU compare idiom: fcomp → fnstsw ax → sahf/test ah ──
            case IRCmp(mn=mn) if mn in (
                Mn.FCOMP, Mn.FUCOMP, Mn.FCOMPP, Mn.FUCOMPP,
            ):
                ops = node.ops
                lhs = "st(0)"
                rhs = ops[0].reg if ops and ops[0].kind == "reg" else "st(1)"
                self.fpu_cmp = (lhs, rhs)
                self.fpu_in_eax = False

            case IRNop(mn=Mn.FNSTSW) if self.fpu_cmp is not None:
                self.fpu_in_eax = True

            case IRNop(mn=Mn.SAHF) if self.fpu_in_eax:
                pass  # state preserved — chainer reads fpu_cmp + fpu_in_eax

            case _:
                pass

    def _resolve_mem(self, op) -> RelativeAddr | None:
        """Resolve memory operand via scope only (runtime-known state)."""
        disp = op.mem_disp
        base = op.mem_base

        # Stack-relative
        if base in ("ebp", "esp") and disp in self.scope:
            return self.scope[disp]

        # Register-indirect: [reg + disp]
        if base:
            reg = to_reg(base)
            if reg and reg in self.cpu:
                base_addr = self.cpu[reg]
                if base_addr is not None:
                    total = base_addr.base + base_addr.offset + disp
                    if total in self.scope:
                        return self.scope[total]

        return None
