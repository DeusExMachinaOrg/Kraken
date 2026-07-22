"""TypeProvider — SQLite-backed type resolution for the enricher.

Reads from TypeDB (same game.persifal.db). No DIA/COM dependency.
All type references are UID-based, cached in memory.

Type resolution follows pointee_uid chains for pointer/ref/array types.
Self-references (Self*, Self&, array<Self>&, Self***) resolve correctly
because pointer/ref UIDs always point to a simpler (lower-indirection) type.

Enricher interface:
    resolve_offset(type, offset) → FieldInfo | VtSlotInfo | None
    resolve_global(name)         → Type | None
    resolve_return_type(name, rva) → Type | None
    resolve_params(name, rva)    → list[(name, Type)] | None
    make_type(name)              → Type
    resolve_cleanup(type, offset) → int
    get_dtor_pattern(type)       → DtorPattern | None
"""

from __future__ import annotations

from dataclasses import dataclass

from ._typedb import TypeDB, uid as _gen_uid
from ._types import (
    Type, ScalarType, PointerType, RefType, UDTType, EnumType,
    VtableType, FuncSigType, ArrayType,
    FieldInfo, FieldStep, VtSlotInfo, CallConv,
    VOID, INT, FLOAT, DOUBLE, CHAR, BOOL,
    make_udt, make_pointer, scalar,
)


# ── Dtor pattern (for inline dtor detection in enricher) ────────────

@dataclass(slots=True)
class DtorPattern:
    """Inline destructor pattern for a type."""
    type_name: str
    zero_offset: int      # offset of the CStr ZERO field within the type
    dtor_rva: int         # RVA of the out-of-line destructor


# ── CallConv mapping ────────────────────────────────────────────────

_CC_MAP = {
    "cdecl": CallConv.CDECL,
    "stdcall": CallConv.STDCALL,
    "thiscall": CallConv.THISCALL,
    "fastcall": CallConv.FASTCALL,
    "unknown": CallConv.UNKNOWN,
}


# ── IAT runtime types ──────────────────────────────────────────────
# Manually annotated return types + cleanup for CRT / WinAPI imports.
# (return_type_name, callconv, cleanup_bytes)
# return_type_name: "" = void, "*" suffix = pointer

_IAT: dict[str, tuple[str, CallConv, int]] = {
    # CRT (cdecl, cleanup=0)
    "sprintf":      ("int",           CallConv.CDECL,   0),
    "sscanf":       ("int",           CallConv.CDECL,   0),
    "printf":       ("int",           CallConv.CDECL,   0),
    "strlen":       ("unsigned int",  CallConv.CDECL,   0),
    "strcmp":       ("int",           CallConv.CDECL,   0),
    "_stricmp":     ("int",           CallConv.CDECL,   0),
    "_strnicmp":    ("int",           CallConv.CDECL,   0),
    "strstr":       ("char*",         CallConv.CDECL,   0),
    "strcpy":       ("char*",         CallConv.CDECL,   0),
    "strncpy":      ("char*",         CallConv.CDECL,   0),
    "strcat":       ("char*",         CallConv.CDECL,   0),
    "atoi":         ("int",           CallConv.CDECL,   0),
    "atof":         ("double",        CallConv.CDECL,   0),
    "strtol":       ("long",          CallConv.CDECL,   0),
    "malloc":       ("void*",         CallConv.CDECL,   0),
    "calloc":       ("void*",         CallConv.CDECL,   0),
    "realloc":      ("void*",         CallConv.CDECL,   0),
    "free":         ("",              CallConv.CDECL,   0),
    "memcpy":       ("void*",         CallConv.CDECL,   0),
    "memset":       ("void*",         CallConv.CDECL,   0),
    "memcmp":       ("int",           CallConv.CDECL,   0),
    "abs":          ("int",           CallConv.CDECL,   0),
    "fabs":         ("double",        CallConv.CDECL,   0),
    # WinAPI (stdcall)
    "CreateWindowExA":  ("HWND__*",        CallConv.STDCALL, 48),
    "GetWindowLongA":   ("long",           CallConv.STDCALL, 8),
    "SetWindowLongA":   ("long",           CallConv.STDCALL, 12),
    "RegisterClassA":   ("unsigned short", CallConv.STDCALL, 4),
    "GetClientRect":    ("int",            CallConv.STDCALL, 8),
    "GetWindowRect":    ("int",            CallConv.STDCALL, 8),
    "SetRect":          ("int",            CallConv.STDCALL, 16),
    "AdjustWindowRect": ("int",            CallConv.STDCALL, 12),
    "SetWindowPos":     ("int",            CallConv.STDCALL, 28),
    "ShowWindow":       ("int",            CallConv.STDCALL, 8),
    "SendMessageA":     ("long",           CallConv.STDCALL, 16),
    "PostMessageA":     ("int",            CallConv.STDCALL, 16),
    "MessageBoxA":      ("int",            CallConv.STDCALL, 16),
    "GetLastError":     ("unsigned long",  CallConv.STDCALL, 0),
    "GetModuleHandleA": ("HINSTANCE__*",   CallConv.STDCALL, 4),
    "LoadIconA":        ("HICON__*",       CallConv.STDCALL, 8),
    "LoadCursorA":      ("HICON__*",       CallConv.STDCALL, 8),
    "GetDC":            ("HDC__*",         CallConv.STDCALL, 4),
    "ReleaseDC":        ("int",            CallConv.STDCALL, 8),
    "GetTickCount":     ("unsigned long",  CallConv.STDCALL, 0),
    "GetSystemMetrics": ("int",            CallConv.STDCALL, 4),
    "GetDesktopWindow": ("HWND__*",        CallConv.STDCALL, 0),
    "FindWindowA":      ("HWND__*",        CallConv.STDCALL, 8),
    "ClipCursor":       ("int",            CallConv.STDCALL, 4),
    "SetCursorPos":     ("int",            CallConv.STDCALL, 8),
    "PeekMessageA":     ("int",            CallConv.STDCALL, 20),
    "DefWindowProcA":   ("long",           CallConv.STDCALL, 16),
    "DestroyWindow":    ("int",            CallConv.STDCALL, 4),
    "PostQuitMessage":  ("",               CallConv.STDCALL, 4),
    "SetCursor":        ("HICON__*",       CallConv.STDCALL, 4),
    "ShellExecuteA":    ("HINSTANCE__*",   CallConv.STDCALL, 24),
    # WINMM
    "timeGetTime":      ("unsigned long",  CallConv.STDCALL, 0),
    "timeSetEvent":     ("unsigned int",   CallConv.STDCALL, 20),
    "timeKillEvent":    ("unsigned int",   CallConv.STDCALL, 4),
    "timeBeginPeriod":  ("unsigned int",   CallConv.STDCALL, 4),
    "timeEndPeriod":    ("unsigned int",   CallConv.STDCALL, 4),
}


# ── FuncAnnotation for runtime overrides ──────────────────────────

@dataclass(slots=True)
class FuncAnnotation:
    """Manually annotated function info (IAT, stubs, overrides)."""
    ret_type: str = ""         # return type name ("int", "char*", "")
    callconv: CallConv = CallConv.CDECL
    cleanup: int = 0


class TypeProvider:
    """SQLite-backed type resolution. Drop-in replacement for SymbolProvider."""

    def __init__(self, db: TypeDB, image_base: int = 0) -> None:
        self.db = db
        self.image_base = image_base
        # In-memory caches: uid → Type
        self._type_cache: dict[int, Type] = {}
        self._name_cache: dict[str, int] = {}  # name → uid
        self._dtor_cache: dict[int, DtorPattern | None] = {}
        self._vft_cache: dict[str, VtableType] = {}  # "name_vft" → VtableType
        self._pending_types: list[tuple] = []        # deferred DB writes
        # Runtime function annotations (IAT, manual overrides)
        self._func_annotations: dict[str, FuncAnnotation] = {}

    def preheat(self) -> int:
        """Bulk-load all types from DB into memory cache. Returns count."""
        rows = self.db.conn.execute(
            "SELECT uid, name, size, kind, is_polymorphic, "
            "pointee_uid, underlying_uid, element_uid, element_count "
            "FROM types"
        ).fetchall()
        # Two passes: first create all types, then wire pointee chains
        for row in rows:
            uid, name = row[0], row[1]
            self._name_cache[name] = uid
        for row in rows:
            self._type_from_uid(row[0])
        return len(rows)

    def flush_new_types(self) -> int:
        """Write pending runtime types to DB. Call after enrichment batch."""
        if not self._pending_types:
            return 0
        for uid, name, size, kind, pointee_uid in self._pending_types:
            self.db.insert_type(uid, name, size, kind, pointee_uid=pointee_uid)
        count = len(self._pending_types)
        self._pending_types.clear()
        return count

    # ── Runtime annotation API ──────────────────────────────────────

    def annotate(self, name: str, ret_type: str = "",
                 callconv: CallConv = CallConv.CDECL,
                 cleanup: int = 0) -> None:
        """Register a manual function annotation (return type, callconv, cleanup)."""
        self._func_annotations[name] = FuncAnnotation(ret_type, callconv, cleanup)

    def annotate_batch(self, entries: dict[str, tuple[str, CallConv, int]]) -> None:
        """Bulk-register annotations: {name: (ret_type, callconv, cleanup)}."""
        for name, (ret, cc, cl) in entries.items():
            self._func_annotations[name] = FuncAnnotation(ret, cc, cl)

    # ── Type construction from DB rows ──────────────────────────────

    def _type_from_uid(self, uid: int) -> Type:
        """Get or create Type from UID. Cached.

        Follows pointee_uid chains for pointer/ref types.
        Self-references resolve correctly: Foo* → pointee Foo (UDT),
        array<Foo>& → pointee array<Foo> (UDT). No cycles possible
        since each indirection level has its own UID.
        """
        if uid == 0:
            return VOID
        if uid in self._type_cache:
            return self._type_cache[uid]

        # Sentinel to break hypothetical cycles (shouldn't happen,
        # but protects against corrupt DB data)
        self._type_cache[uid] = VOID

        row = self.db.type_by_uid(uid)
        if row is None:
            return VOID

        (_, name, size, kind, is_poly,
         pointee_uid, underlying_uid, element_uid, element_count) = row

        if kind == "scalar":
            t = scalar(name, size)
        elif kind == "pointer":
            pointee = self._type_from_uid(pointee_uid)
            t = PointerType(name=name, size=4, pointee=pointee)
        elif kind == "ref":
            pointee = self._type_from_uid(pointee_uid)
            t = RefType(name=name, size=4, pointee=pointee)
        elif kind == "array":
            element = self._type_from_uid(element_uid)
            t = ArrayType(name=name, size=size, element=element,
                          count=element_count)
        elif kind == "enum":
            underlying = (self._type_from_uid(underlying_uid)
                          if underlying_uid else INT)
            t = EnumType(name=name, size=size, underlying=underlying)
        elif kind == "udt":
            t = UDTType(name=name, size=size, is_polymorphic_=bool(is_poly))
        else:
            t = Type(name=name, size=size)

        t.uid = uid
        self._type_cache[uid] = t
        return t

    def _uid_for_name(self, name: str) -> int:
        """Lookup UID by type name. Cached."""
        if name in self._name_cache:
            return self._name_cache[name]
        uid = self.db.type_uid(name)
        self._name_cache[name] = uid
        return uid

    # ── Public interface (enricher API) ─────────────────────────────

    def make_type(self, name: str) -> Type:
        """Create a Type by name. Returns VOID if unknown."""
        uid = self._uid_for_name(name)
        return self._type_from_uid(uid)

    def ptr_of(self, typ: Type) -> PointerType:
        """T → T* with DB UID. In-memory only, flushed by flush_new_types()."""
        name = f"{typ.name}*"
        t = self.make_type(name)
        if isinstance(t, PointerType):
            return t
        new_uid = _gen_uid()
        pt = PointerType(name=name, size=4, pointee=typ)
        pt.uid = new_uid
        self._type_cache[new_uid] = pt
        self._name_cache[name] = new_uid
        self._pending_types.append((new_uid, name, 4, "pointer", typ.uid))
        return pt

    def ref_of(self, typ: Type) -> RefType:
        """T → T& with DB UID. In-memory only, flushed by flush_new_types()."""
        name = f"{typ.name}&"
        t = self.make_type(name)
        if isinstance(t, RefType):
            return t
        new_uid = _gen_uid()
        rt = RefType(name=name, size=4, pointee=typ)
        rt.uid = new_uid
        self._type_cache[new_uid] = rt
        self._name_cache[name] = new_uid
        self._pending_types.append((new_uid, name, 4, "ref", typ.uid))
        return rt

    def resolve_offset(self, typ: Type, offset: int,
                       _depth: int = 0) -> FieldInfo | VtSlotInfo | None:
        """Resolve [type + offset] → field or vtable slot.

        For VtableType: lookup vtable slot.
        For UDT/Pointer: lookup field, recurse into sub-structs.

        Returns FieldInfo with chain: list[FieldStep(name, type)] for
        each access level. Arrow (. vs ->) derived from previous step's
        type.is_pointer at render time.
        """
        if _depth > 10:
            return None

        # Vtable dispatch: type is a vtable → resolve slot
        if isinstance(typ, VtableType):
            return self._resolve_vtslot(typ, offset)

        # UDT field resolution
        base = typ.deref() if typ.is_pointer or typ.is_ref else typ
        if not isinstance(base, UDTType):
            return None

        type_uid = self._uid_for_name(base.name)
        if type_uid == 0:
            return None

        # Offset 0 on polymorphic UDT = vfptr
        if offset == 0 and base.is_polymorphic:
            primary_rva = self.db.get_primary_vt_rva(type_uid)
            vft = self._get_vft(base, primary_rva)
            return FieldInfo(
                offset=0,
                name="__vfptr",
                type=vft,
                chain=[FieldStep("__vfptr", vft)],
            )

        # Exact field match
        row = self.db.field_at(type_uid, offset)
        if row:
            fname, fuid, fsize = row
            ftype = self._type_from_uid(fuid)
            return FieldInfo(offset=offset, name=fname, type=ftype,
                             chain=[FieldStep(fname, ftype)])

        # Nearest field before → recurse into sub-struct
        before = self.db.field_before(type_uid, offset)
        if before:
            foff, fname, fuid, fsize = before
            sub_off = offset - foff
            ftype = self._type_from_uid(fuid)
            if sub_off > 0:
                inner = ftype.deref() if ftype.is_pointer else ftype
                sub = self.resolve_offset(inner, sub_off, _depth + 1)
                if isinstance(sub, FieldInfo):
                    chain = [FieldStep(fname, ftype)] + sub.chain
                    return FieldInfo(offset=offset, name=sub.name,
                                     type=sub.type, chain=chain)
                if isinstance(sub, VtSlotInfo):
                    return sub
            return FieldInfo(offset=offset, name=fname, type=ftype,
                             chain=[FieldStep(fname, ftype)])

        return None

    def _get_vft(self, owner: UDTType, vt_rva: int) -> VtableType:
        """Get or create a VtableType for a UDT. In-memory only."""
        name = f"{owner.name}_vft"
        cached = self._vft_cache.get(name)
        if cached is not None:
            return cached
        new_uid = _gen_uid()
        vft = VtableType(name=name, size=4, owner=owner, vt_rva=vt_rva)
        vft.uid = new_uid
        self._type_cache[new_uid] = vft
        self._name_cache[name] = new_uid
        self._vft_cache[name] = vft
        self._pending_types.append((new_uid, name, 4, "udt", 0))
        return vft

    def _resolve_vtslot(self, vt: VtableType, offset: int) -> VtSlotInfo | None:
        """Resolve vtable slot at byte offset."""
        owner = vt.owner
        if not isinstance(owner, UDTType):
            return None
        type_uid = self._uid_for_name(owner.name)
        if type_uid == 0:
            return None

        row = self.db.get_vtslot(type_uid, offset, vt_rva=vt.vt_rva)
        if row is None:
            return None

        method_name, method_rva, ret_uid, cc_str, cleanup = row
        ret_type = self._type_from_uid(ret_uid) if ret_uid else None
        cc = _CC_MAP.get(cc_str, CallConv.THISCALL)
        return VtSlotInfo(
            offset=offset, method_name=method_name, method_rva=method_rva,
            return_type=ret_type, callconv=cc, cleanup=cleanup,
        )

    def resolve_global(self, name: str) -> Type | None:
        """Resolve a global symbol by name → its type."""
        row = self.db.get_global_by_name(name)
        if row is None:
            return None
        _, _, type_uid = row
        if type_uid == 0:
            return None
        return self._type_from_uid(type_uid)

    def resolve_return_type(self, name: str, rva: int = 0) -> Type | None:
        """Get return type for a function by name or RVA."""
        row = None
        if rva > 0:
            row = self.db.get_function(rva)
        if row is None:
            row = self.db.get_function_by_name(name)
        if row is not None:
            # (rva, name, parent_uid, callconv, ret_uid, length, cleanup)
            ret_uid = row[4]
            if isinstance(ret_uid, int) and ret_uid != 0:
                return self._type_from_uid(ret_uid)

        # Fallback: runtime annotations
        return self._annotated_return_type(name)

    def resolve_params(self, name: str, rva: int = 0) -> list[tuple[str, Type]] | None:
        """Get function parameters as [(name, Type), ...]."""
        func_rva = rva
        if func_rva == 0:
            row = self.db.get_function_by_name(name)
            if row:
                func_rva = row[0]
        if func_rva == 0:
            return None

        rows = self.db.get_params(func_rva)
        if not rows:
            return None

        result = []
        for idx, pname, type_uid in rows:
            ptype = self._type_from_uid(type_uid)
            result.append((pname, ptype))
        return result

    def resolve_cleanup(self, typ: Type, offset: int) -> int:
        """Resolve callee cleanup bytes for indirect call [type+offset].

        Three cases:
          1. Vtable slot: [vtable + offset] → method cleanup from PDB
          2. Function pointer field: [struct_ptr + offset] → field type
             is functype* — cleanup encoded in pointee's size field
          3. Fallback: 0
        """
        if isinstance(typ, VtableType):
            slot = self._resolve_vtslot(typ, offset)
            if slot:
                return slot.cleanup
        # Non-vtable: dereference pointer, resolve field
        owner = typ.deref() if typ.is_pointer else typ
        if owner.is_udt and owner.size > 0:
            result = self.resolve_offset(owner, offset)
            if isinstance(result, FieldInfo) and result.type.is_pointer:
                # Field is a pointer — check if pointee is a functype
                pointee = result.type.deref()
                if pointee.name.startswith("__func_"):
                    # size field = cleanup bytes (set by pdb_import)
                    return pointee.size
        return 0

    def resolve_iat_cleanup(self, iat_rva: int) -> int:
        """Resolve cleanup for an IAT call."""
        row = self.db.get_global(iat_rva)
        if row is None:
            return 0
        _, name, _ = row
        func = self.db.get_function_by_name(name)
        if func:
            cleanup = func[6]
            if isinstance(cleanup, int) and cleanup > 0:
                return cleanup
        # Fallback: runtime annotations
        return self._annotated_cleanup(name)

    # ── Annotation lookup helpers ──────────────────────────────────

    def _annotated_return_type(self, name: str) -> Type | None:
        """Lookup return type from runtime annotations."""
        ann = self._func_annotations.get(name)
        if ann is None or not ann.ret_type:
            return None
        ret = ann.ret_type
        if ret.endswith("*"):
            # Try full pointer name first (e.g. "char*" → DB lookup)
            full = self.make_type(ret)
            if isinstance(full, PointerType):
                return full
            inner = self.make_type(ret[:-1].rstrip())
            if inner.size == 0:
                inner = scalar(ret[:-1].rstrip(), 4)
            return self.ptr_of(inner)
        return scalar(ret, 4)

    def _annotated_cleanup(self, name: str) -> int:
        """Lookup cleanup bytes from runtime annotations."""
        ann = self._func_annotations.get(name)
        return ann.cleanup if ann else 0

    def get_parent_class(self, rva: int) -> str:
        """Get parent class name for a method at RVA."""
        row = self.db.get_function(rva)
        if row is None:
            return ""
        parent_uid = row[2]
        if parent_uid == 0:
            return ""
        t = self._type_from_uid(parent_uid)
        return t.name

    def get_dtor_pattern(self, typ: Type) -> DtorPattern | None:
        """Get inline dtor pattern for a type (CStr-like types)."""
        if not isinstance(typ, UDTType):
            return None
        type_uid = self._uid_for_name(typ.name)
        if type_uid in self._dtor_cache:
            return self._dtor_cache[type_uid]

        # Look for a field named "ZERO" or similar CStr pattern
        fields = self.db.get_fields(type_uid)
        zero_offset = -1
        for foff, fname, fuid, fsize, is_base in fields:
            if fname == "ZERO" or fname == "m_zero":
                zero_offset = foff
                break

        if zero_offset < 0:
            self._dtor_cache[type_uid] = None
            return None

        # Find the destructor in the functions table
        dtor_name = f"{typ.name}::~{typ.short_name}"
        func = self.db.get_function_by_name(dtor_name)
        dtor_rva = func[0] if func else 0

        pat = DtorPattern(type_name=typ.name, zero_offset=zero_offset,
                          dtor_rva=dtor_rva)
        self._dtor_cache[type_uid] = pat
        return pat

    def build_known_locals(self, func_rva: int):
        """Build known locals for a function from DB.

        Returns list of (vframe_off, name, Type, size, loc_kind, register).
        loc_kind: 3=regrel (stack), 5=enregistered, etc.
        """
        rows = self.db.get_locals(func_rva)
        result = []
        for name, type_uid, vframe_off, size, loc_kind, register in rows:
            typ = self._type_from_uid(type_uid)
            result.append((vframe_off, name, typ, size, loc_kind, register))
        return result
