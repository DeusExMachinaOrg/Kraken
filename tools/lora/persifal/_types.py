"""Type system for Persifal decompiler.

Replaces the old Symbol class with proper subclasses.
All types are pure data — no PDB/DIA references.
TypeProvider resolves from SQLite, not COM.

Hierarchy:
    Type
    ├── ScalarType    — int, float, bool, char, void, unsigned int
    ├── PointerType   — T*
    ├── RefType        — T&
    ├── UDTType        — class/struct (fields + vtable resolved via provider)
    ├── EnumType       — named enum
    ├── ArrayType      — T[N]
    ├── FuncSigType    — function signature (return, params, callconv)
    └── VtableType     — vfptr (owning UDT)
"""

from __future__ import annotations

import enum
import typing
from dataclasses import dataclass, field


# ── Calling convention ──────────────────────────────────────────────

class CallConv(enum.Enum):
    CDECL    = "cdecl"
    STDCALL  = "stdcall"
    THISCALL = "thiscall"
    FASTCALL = "fastcall"
    UNKNOWN  = "unknown"


# ── Base ────────────────────────────────────────────────────────────

@dataclass(slots=True)
class Type:
    """Base type node. All types are immutable pure data."""
    name: str
    size: int       # sizeof in bytes
    uid: int = 0    # TypeDB snowflake UID (0 = unregistered/builtin)

    @property
    def base_name(self) -> str:
        """Root type name: 'CStr*' → 'CStr', 'CStr' → 'CStr'."""
        return self.name

    @property
    def short_name(self) -> str:
        """Unqualified: 'm3d::Kernel' → 'Kernel'."""
        return self.base_name.rsplit("::", 1)[-1]

    @property
    def is_pointer(self) -> bool:
        return False

    @property
    def is_ref(self) -> bool:
        return False

    @property
    def is_udt(self) -> bool:
        return False

    @property
    def is_void(self) -> bool:
        return self.name == "void" and self.size == 0

    def deref(self) -> Type:
        """Unwrap pointer/ref → pointee. Non-pointer returns self."""
        return self

    def ptr(self) -> PointerType:
        """Wrap: T → T*."""
        return PointerType(name=f"{self.name}*", size=4, pointee=self)

    def ref(self) -> RefType:
        """Wrap: T → T&."""
        return RefType(name=f"{self.name}&", size=4, pointee=self)

    def __repr__(self) -> str:
        return f"{type(self).__name__}({self.name}, {self.size}B)"


# ── Scalar ──────────────────────────────────────────────────────────

@dataclass(slots=True)
class ScalarType(Type):
    """Builtin: int, float, bool, char, void, unsigned int, double, etc."""
    pass


# Well-known scalars (singletons)
VOID   = ScalarType("void", 0)
BOOL   = ScalarType("bool", 1)
CHAR   = ScalarType("char", 1)
UCHAR  = ScalarType("unsigned char", 1)
SHORT  = ScalarType("short", 2)
USHORT = ScalarType("unsigned short", 2)
INT    = ScalarType("int", 4)
UINT   = ScalarType("unsigned int", 4)
LONG   = ScalarType("long", 4)
ULONG  = ScalarType("unsigned long", 4)
FLOAT  = ScalarType("float", 4)
DOUBLE = ScalarType("double", 8)

_SCALAR_MAP: dict[str, ScalarType] = {t.name: t for t in [
    VOID, BOOL, CHAR, UCHAR, SHORT, USHORT, INT, UINT,
    LONG, ULONG, FLOAT, DOUBLE,
]}


def scalar(name: str, size: int = 0) -> ScalarType:
    """Get or create a scalar type by name."""
    if name in _SCALAR_MAP:
        return _SCALAR_MAP[name]
    return ScalarType(name, size)


# ── Pointer / Ref ───────────────────────────────────────────────────

@dataclass(slots=True)
class PointerType(Type):
    """T* — size always 4 on x86."""
    pointee: Type = field(default_factory=lambda: VOID)

    @property
    def base_name(self) -> str:
        return self.pointee.base_name

    @property
    def is_pointer(self) -> bool:
        return True

    def deref(self) -> Type:
        return self.pointee


@dataclass(slots=True)
class RefType(Type):
    """T& — size always 4 on x86."""
    pointee: Type = field(default_factory=lambda: VOID)

    @property
    def base_name(self) -> str:
        return self.pointee.base_name

    @property
    def is_ref(self) -> bool:
        return True

    def deref(self) -> Type:
        return self.pointee


# ── UDT (class/struct) ─────────────────────────────────────────────

@dataclass(slots=True)
class UDTType(Type):
    """Class or struct. Fields and vtable resolved via TypeProvider."""
    is_polymorphic_: bool = False

    @property
    def is_udt(self) -> bool:
        return True

    @property
    def is_polymorphic(self) -> bool:
        return self.is_polymorphic_

    def vtable(self) -> VtableType:
        """Create a vtable type for this UDT."""
        return VtableType(name=f"{self.name}_vft", size=4, owner=self)


# ── Vtable ──────────────────────────────────────────────────────────

@dataclass(slots=True)
class VtableType(Type):
    """Virtual function table pointer. Owner is the UDT it belongs to."""
    owner: UDTType = field(default_factory=lambda: UDTType("", 0))
    vt_rva: int = 0   # PE vtable RVA (distinguishes MI bases, 0 = primary)


# ── Enum ────────────────────────────────────────────────────────────

@dataclass(slots=True)
class EnumType(Type):
    """Named enum with value map (loaded on demand from provider)."""
    underlying: Type = field(default_factory=lambda: INT)


# ── Array ───────────────────────────────────────────────────────────

@dataclass(slots=True)
class ArrayType(Type):
    """Fixed-size array: T[N]."""
    element: Type = field(default_factory=lambda: VOID)
    count: int = 0


# ── Function signature ──────────────────────────────────────────────

@dataclass
class ParamInfo:
    """One function parameter."""
    name: str
    type: Type
    index: int = 0


@dataclass(slots=True)
class FuncSigType(Type):
    """Function signature — for IAT entries, indirect calls, function pointers."""
    return_type: Type = field(default_factory=lambda: VOID)
    params: list[ParamInfo] = field(default_factory=list)
    callconv: CallConv = CallConv.UNKNOWN
    cleanup: int = 0       # callee stack cleanup bytes


# ── Field chain (structured access path) ──────────────────────────────

class FieldStep(typing.NamedTuple):
    """One step in a field access chain.

    name: field name at this level
    type: type OF this field

    Arrow (. vs ->) is derived: if previous step's type is_pointer → '->'.
    DB layer stores (name, uid). Runtime resolves to (name, Type).
    """
    name: str
    type: Type


# ── Field / vtable slot descriptors (query results) ─────────────────

@dataclass(slots=True)
class FieldInfo:
    """Result of resolving a field at an offset within a UDT.

    chain: structured access path with type at each step.
    Arrow derived: chain[i-1].type.is_pointer → '->', else '.'.
    First step's arrow comes from the base object type (caller knows).
    """
    offset: int
    name: str                                        # leaf field name
    type: Type                                       # leaf field type
    chain: list[FieldStep] = field(default_factory=list)


@dataclass(slots=True)
class VtSlotInfo:
    """Result of resolving a vtable slot."""
    offset: int
    method_name: str
    method_rva: int
    return_type: Type | None = None
    callconv: CallConv = CallConv.THISCALL
    cleanup: int = 0


# ── Convenience ─────────────────────────────────────────────────────

def make_pointer(inner: Type) -> PointerType:
    """Shorthand: wrap any type in a pointer."""
    return PointerType(name=f"{inner.name}*", size=4, pointee=inner)


def make_udt(name: str, size: int, polymorphic: bool = False) -> UDTType:
    """Shorthand: create a UDT type."""
    return UDTType(name=name, size=size, is_polymorphic_=polymorphic)
