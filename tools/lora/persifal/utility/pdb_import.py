"""PDB import — walk DIA once, dump everything to TypeDB SQLite.

Usage:
    python provider/lora/persifal/utility/pdb_import.py target/hta.exe target/game.pdb [db_path]

All type references stored as integer UIDs, not strings.

Import order:
  1. import_scalars — well-known scalar types
  2. import_type_names — assign UIDs to ALL UDTs + enums (names only)
  3. import_types — fields for each UDT (all pointee UIDs now resolvable)
  4. import_enums — enum values
  5. import_functions, import_globals, import_lexlines
  6. import_vtables — PE-based vtable slot resolution
  7. flush_derived_types — pointer/ref/array entries created during field resolution

This two-pass approach (names first, fields second) handles self-references:
  - Foo has field Foo* → Foo UID exists from pass 1, Foo* created in pass 2
  - array<Foo>& → array<Foo> UID from pass 1, ref wrapper created in pass 2

UID persistence: on re-import, existing name→uid mappings from DB are loaded
into _UIDMap so types keep the same UIDs across runs.
"""

from __future__ import annotations

import struct
import sys
import time
from pathlib import Path

from pydia2 import cvconst


# ── DIA helpers ─────────────────────────────────────────────────────

CC_MAP = {
    0: "unknown", 1: "cdecl", 2: "unknown", 3: "unknown",
    4: "thiscall", 7: "stdcall", 8: "fastcall", 11: "thiscall",
}

BASE_TYPE_NAMES = {
    0: "void", 1: "void", 2: "char", 3: "wchar_t",
    6: "int", 7: "unsigned int", 8: "float",
    9: "BCD", 10: "bool", 13: "long", 14: "unsigned long",
    15: "__int8", 16: "unsigned __int8",
    17: "__int16", 18: "unsigned __int16",
    19: "__int32", 20: "unsigned __int32",
    21: "__int64", 22: "unsigned __int64",
    25: "HRESULT", 30: "char16_t", 31: "char32_t",
}


def _base_type_name(sym) -> str:
    """Get base type name, disambiguated by size."""
    bt = int(sym.baseType)
    size = int(sym.length)
    name = BASE_TYPE_NAMES.get(bt, f"base_{bt}")
    if bt == 6 and size == 2: name = "short"
    elif bt == 7 and size == 2: name = "unsigned short"
    elif bt == 7 and size == 1: name = "unsigned char"
    elif bt == 6 and size == 1: name = "signed char"
    elif bt == 8 and size == 8: name = "double"
    return name


def _resolve_dia_type(sym, uids: _UIDMap) -> int:
    """Walk DIA type symbol → UID. Creates pointer/ref/array entries in uids.

    Each pointer/ref level gets its own UID and type entry.
    Returns the final (possibly pointer/ref) UID.

    Self-references are safe: UDT/Enum fall through to uids.get(name)
    which returns the UID assigned in the name-registration pass.
    """
    try:
        tag = int(sym.symTag)
    except Exception:
        return 0

    if tag == int(cvconst.SymTag.PointerType):
        try:
            inner_uid = _resolve_dia_type(sym.type, uids)
            inner_name = uids.name_of(inner_uid) if inner_uid else "void"
            is_ref = bool(sym.reference)
            if is_ref:
                ref_name = f"{inner_name}&"
                return uids.get_or_create(ref_name, "ref", 4, pointee=inner_uid)
            else:
                ptr_name = f"{inner_name}*"
                return uids.get_or_create(ptr_name, "pointer", 4, pointee=inner_uid)
        except Exception:
            return uids.get_or_create("void*", "pointer", 4,
                                       pointee=uids.get("void"))

    if tag == int(cvconst.SymTag.FunctionType):
        # Function pointer target: extract calling convention + param count
        # to compute callee stack cleanup (ret N).
        try:
            cc = int(sym.callingConvention)
            # Count explicit parameters (FunctionArgType children).
            # DIA FunctionArgType does NOT include implicit 'this'.
            args = sym.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
            n_explicit = args.count if args else 0
            # Compute callee stack cleanup (= ret N) from convention.
            # thiscall: this in ecx (implicit), all explicit → stack
            # fastcall: first 2 explicit in ecx/edx, rest → stack
            # stdcall:  all explicit → stack
            # cdecl:    caller cleanup → 0
            _CC_NAMES = {0: "cdecl", 4: "fastcall", 7: "stdcall", 11: "thiscall"}
            cc_name = _CC_NAMES.get(cc, f"cc{cc}")
            if cc == 0:    # cdecl — caller cleanup
                cleanup = 0
            elif cc == 4:  # fastcall — first 2 in regs
                cleanup = max(0, n_explicit - 2) * 4
            elif cc == 7:  # stdcall — all on stack
                cleanup = n_explicit * 4
            elif cc == 11: # thiscall — this in ecx, rest on stack
                cleanup = n_explicit * 4
            else:
                cleanup = 0
            # Encode as synthetic type: __func_{cc}_{cleanup}
            func_name = f"__func_{cc_name}_{cleanup}"
            return uids.get_or_create(func_name, "functype", cleanup)
        except Exception:
            return 0

    if tag == int(cvconst.SymTag.BaseType):
        name = _base_type_name(sym)
        size = int(sym.length) if sym.length else 0
        return uids.get_or_create(name, "scalar", size)

    if tag == int(cvconst.SymTag.ArrayType):
        try:
            inner_uid = _resolve_dia_type(sym.type, uids)
            inner_name = uids.name_of(inner_uid) if inner_uid else "void"
            count = int(sym.count)
            arr_name = f"{inner_name}[{count}]"
            return uids.get_or_create(arr_name, "array", int(sym.length),
                                       element_uid=inner_uid, element_count=count)
        except Exception:
            return 0

    # UDT, Enum, Typedef — use name directly
    try:
        name = sym.name or ""
        if name:
            return uids.get(name)
    except Exception:
        pass
    return 0


def _type_size(sym) -> int:
    try:
        return int(sym.length)
    except Exception:
        return 0


# ── UID map: ensures every type name gets exactly one UID ──────────

class _TypeEntry:
    """Metadata for one type in the UID map."""
    __slots__ = ("uid", "name", "kind", "size", "pointee", "element_uid", "element_count")

    def __init__(self, uid: int, name: str, kind: str = "", size: int = 0,
                 pointee: int = 0, element_uid: int = 0, element_count: int = 0):
        self.uid = uid
        self.name = name
        self.kind = kind
        self.size = size
        self.pointee = pointee
        self.element_uid = element_uid
        self.element_count = element_count


class _UIDMap:
    """Maps type names → UIDs + metadata. Tracks pointer/ref variants.

    Accepts existing name→uid pairs for UID persistence across re-imports.
    """

    def __init__(self, uid_gen, existing_uids: dict[str, int] | None = None):
        self._map: dict[str, _TypeEntry] = {}
        self._uid_to_name: dict[int, str] = {}
        self._gen = uid_gen
        # Preload existing UIDs from DB for persistence
        if existing_uids:
            for name, ex_uid in existing_uids.items():
                entry = _TypeEntry(ex_uid, name)
                self._map[name] = entry
                self._uid_to_name[ex_uid] = name

    def get(self, name: str) -> int:
        """Get or create UID for a base type name. Returns 0 for empty."""
        if not name:
            return 0
        if name not in self._map:
            u = self._gen.next()
            self._map[name] = _TypeEntry(u, name)
            self._uid_to_name[u] = name
        return self._map[name].uid

    def get_or_create(self, name: str, kind: str, size: int,
                      pointee: int = 0, element_uid: int = 0,
                      element_count: int = 0) -> int:
        """Get or create with full metadata (for pointer/ref/array/scalar)."""
        if not name:
            return 0
        if name not in self._map:
            u = self._gen.next()
            self._map[name] = _TypeEntry(u, name, kind, size, pointee,
                                          element_uid, element_count)
            self._uid_to_name[u] = name
        else:
            # Update metadata if it was a bare placeholder
            entry = self._map[name]
            if not entry.kind and kind:
                entry.kind = kind
                entry.size = size
                entry.pointee = pointee
                entry.element_uid = element_uid
                entry.element_count = element_count
        return self._map[name].uid

    def name_of(self, uid: int) -> str:
        """Reverse lookup: UID → name."""
        return self._uid_to_name.get(uid, "")

    def entries(self) -> list[_TypeEntry]:
        return list(self._map.values())

    def __len__(self) -> int:
        return len(self._map)


# ── Import passes ───────────────────────────────────────────────────

def import_type_names(sess, uids: _UIDMap, progress=None):
    """Pass 1: Assign UIDs to ALL UDT + enum names without resolving fields.

    This ensures that when pass 2 (import_types) resolves field types,
    self-referencing types like Foo* inside Foo already have a UID.
    """
    gs = sess.global_scope

    # UDTs
    udts = gs.findChildren(cvconst.SymTag.UDT, None, 0)
    udt_count = 0
    for i in range(udts.count):
        try:
            name = udts.Item(i).name or ""
            if name:
                uids.get(name)
                udt_count += 1
        except Exception:
            continue
        if progress and (i + 1) % 2000 == 0:
            progress("type_names_udt", i + 1, udts.count)

    # Enums
    enums = gs.findChildren(cvconst.SymTag.Enum, None, 0)
    enum_count = 0
    for i in range(enums.count):
        try:
            name = enums.Item(i).name or ""
            if name:
                uids.get(name)
                enum_count += 1
        except Exception:
            continue

    return udt_count + enum_count


def import_types(sess, db, uids: _UIDMap, progress=None):
    """Pass 2: Import all UDTs → types + fields tables.

    All type names already have UIDs from import_type_names,
    so self-references (Self*, array<Self>&, etc.) resolve correctly.
    """
    gs = sess.global_scope
    enum = gs.findChildren(cvconst.SymTag.UDT, None, 0)
    total = enum.count
    type_batch = []
    field_batch = []

    for i in range(total):
        try:
            udt = enum.Item(i)
            name = udt.name or ""
            if not name:
                continue
            size = int(udt.length)
            type_uid = uids.get(name)

            # Check polymorphic
            is_poly = 0
            try:
                methods = udt.findChildren(cvconst.SymTag.Function, None, 0)
                for mi in range(min(methods.count, 500)):
                    try:
                        if bool(methods.Item(mi).virtual):
                            is_poly = 1
                            break
                    except Exception:
                        pass
            except Exception:
                pass

            type_batch.append((type_uid, name, size, "udt", is_poly, 0, 0, 0, 0))

            # Fields (recursive with base classes)
            _collect_fields(udt, type_uid, 0, set(), field_batch, uids)

            # Also ensure the UIDMap entry has full metadata
            uids._map[name].kind = "udt"
            uids._map[name].size = size

        except Exception:
            continue

        if progress and (i + 1) % 1000 == 0:
            progress("types", i + 1, total)

    conn = db.conn
    conn.executemany(
        "INSERT OR REPLACE INTO types "
        "(uid, name, size, kind, is_polymorphic, pointee_uid, underlying_uid, element_uid, element_count) "
        "VALUES (?,?,?,?,?,?,?,?,?)", type_batch)
    conn.executemany(
        "INSERT OR REPLACE INTO fields "
        "(type_uid, offset, name, field_uid, field_size, is_base) "
        "VALUES (?,?,?,?,?,?)", field_batch)
    db.commit()
    return len(type_batch)


def _collect_fields(udt, type_uid: int, base_off: int,
                    visited: set, out: list, uids: _UIDMap):
    """Walk UDT: Data → fields, BaseClass → recurse."""
    udt_name = udt.name or ""
    if udt_name in visited:
        return
    visited.add(udt_name)

    children = udt.findChildren(cvconst.SymTag.Data, None, 0)
    for i in range(min(children.count, 2000)):
        try:
            f = children.Item(i)
            fname = f.name or ""
            if not fname:
                continue
            foffset = base_off + int(f.offset)
            fsize = _type_size(f.type) if f.type else 0
            field_uid = _resolve_dia_type(f.type, uids) if f.type else 0
            out.append((type_uid, foffset, fname, field_uid, fsize, 0))
        except Exception:
            continue

    bases = udt.findChildren(cvconst.SymTag.BaseClass, None, 0)
    for i in range(bases.count):
        try:
            b = bases.Item(i)
            boff = base_off + int(b.offset)
            btype = b.type
            if btype:
                bname = btype.name or ""
                bsize = int(btype.length)
                base_uid = uids.get(bname)
                out.append((type_uid, boff, f"__base_{bname}", base_uid, bsize, 1))
                _collect_fields(btype, type_uid, boff, visited, out, uids)
        except Exception:
            continue


def import_enums(sess, db, uids: _UIDMap, progress=None):
    """Import all enums → types + enums tables."""
    gs = sess.global_scope
    enum = gs.findChildren(cvconst.SymTag.Enum, None, 0)
    total = enum.count
    type_batch = []
    value_batch = []

    for i in range(total):
        try:
            sym = enum.Item(i)
            name = sym.name or ""
            if not name:
                continue
            size = int(sym.length)
            type_uid = uids.get(name)

            underlying_uid = 0
            try:
                if sym.type:
                    underlying_uid = _resolve_dia_type(sym.type, uids)
            except Exception:
                pass

            type_batch.append((type_uid, name, size, "enum", 0, 0, underlying_uid, 0, 0))

            children = sym.findChildren(cvconst.SymTag.Null, None, 0)
            for j in range(min(children.count, 5000)):
                try:
                    child = children.Item(j)
                    mname = child.name or ""
                    mval = int(child.value)
                    if mname:
                        value_batch.append((type_uid, mname, mval))
                except Exception:
                    continue
        except Exception:
            continue

        if progress and (i + 1) % 500 == 0:
            progress("enums", i + 1, total)

    conn = db.conn
    conn.executemany(
        "INSERT OR REPLACE INTO types "
        "(uid, name, size, kind, is_polymorphic, pointee_uid, underlying_uid, element_uid, element_count) "
        "VALUES (?,?,?,?,?,?,?,?,?)", type_batch)
    conn.executemany(
        "INSERT OR REPLACE INTO enums (type_uid, member_name, value) "
        "VALUES (?,?,?)", value_batch)
    db.commit()
    return len(type_batch)


def import_functions(sess, db, uids: _UIDMap, progress=None):
    """Import all functions → functions + params + locals tables."""
    gs = sess.global_scope
    enum = gs.findChildren(cvconst.SymTag.Function, None, 0)
    total = enum.count
    func_batch = []
    param_batch = []
    local_batch = []

    for i in range(total):
        try:
            func = enum.Item(i)
            rva = int(func.relativeVirtualAddress)
            length = int(func.length)
            name = func.name or ""
            if rva <= 0 or length <= 0 or not name:
                continue

            # Parent class
            parent_uid = 0
            try:
                cp = func.classParent
                if cp and cp.name:
                    parent_uid = uids.get(cp.name)
            except Exception:
                pass

            # Calling convention + return type
            cc = "unknown"
            ret_uid = 0
            try:
                ft = func.type
                if ft:
                    cc_raw = int(ft.callingConvention)
                    cc = CC_MAP.get(cc_raw, "unknown")
                    if ft.type:
                        ret_uid = _resolve_dia_type(ft.type, uids)

                    # Params from function type
                    args = ft.findChildren(cvconst.SymTag.FunctionArgType, None, 0)
                    for pi in range(args.count):
                        try:
                            arg = args.Item(pi)
                            if arg.type:
                                auid = _resolve_dia_type(arg.type, uids)
                                param_batch.append((rva, pi, f"a{pi}", auid))
                        except Exception:
                            continue
            except Exception:
                pass

            # Locals from function body (all location types)
            try:
                data = func.findChildren(cvconst.SymTag.Data, None, 0)
                for di in range(min(data.count, 200)):
                    try:
                        child = data.Item(di)
                        cname = child.name or ""
                        if not cname:
                            continue
                        loc_kind = int(child.locationType)
                        coffset = int(child.offset)
                        csize = _type_size(child.type) if child.type else 0
                        reg_name = ""
                        try:
                            if loc_kind == 5:  # enregistered
                                reg_name = child.registerName or ""
                        except Exception:
                            pass
                        luid = _resolve_dia_type(child.type, uids) if child.type else 0
                        local_batch.append((rva, cname, luid,
                                            coffset, csize, loc_kind, reg_name))
                    except Exception:
                        continue
            except Exception:
                pass

            func_batch.append((rva, name, parent_uid, cc, ret_uid, length, 0))
        except Exception:
            continue

        if progress and (i + 1) % 2000 == 0:
            progress("functions", i + 1, total)

    conn = db.conn
    conn.executemany(
        "INSERT OR REPLACE INTO functions "
        "(rva, name, parent_uid, callconv, ret_uid, length, cleanup) "
        "VALUES (?,?,?,?,?,?,?)", func_batch)
    conn.executemany(
        "INSERT OR REPLACE INTO params (func_rva, idx, name, type_uid) "
        "VALUES (?,?,?,?)", param_batch)
    conn.executemany(
        "INSERT OR REPLACE INTO locals "
        "(func_rva, name, type_uid, vframe_off, size, loc_kind, register) "
        "VALUES (?,?,?,?,?,?,?)", local_batch)
    db.commit()
    return len(func_batch)


def import_globals(sess, db, uids: _UIDMap, progress=None):
    """Import global/static data + public symbols → globals table.

    Data symbols have type info; public symbols (like ??_7 vtables)
    have only RVA+name but are needed for vtable resolution.
    """
    gs = sess.global_scope
    batch = []

    # Data symbols (with type info)
    data_enum = gs.findChildren(cvconst.SymTag.Data, None, 0)
    total_data = data_enum.count
    for i in range(total_data):
        try:
            sym = data_enum.Item(i)
            rva = int(sym.relativeVirtualAddress)
            name = sym.name or ""
            if rva <= 0 or not name:
                continue
            guid = _resolve_dia_type(sym.type, uids) if sym.type else 0
            batch.append((rva, name, guid))
        except Exception:
            continue

        if progress and (i + 1) % 5000 == 0:
            progress("globals_data", i + 1, total_data)

    # Public symbols (vtable ??_7, RTTI, etc. — no type info)
    pub_enum = gs.findChildren(cvconst.SymTag.PublicSymbol, None, 0)
    total_pub = pub_enum.count
    pub_count = 0
    for i in range(total_pub):
        try:
            sym = pub_enum.Item(i)
            rva = int(sym.relativeVirtualAddress)
            name = sym.name or ""
            if rva <= 0 or not name:
                continue
            # Only import public symbols not already covered by data
            batch.append((rva, name, 0))
            pub_count += 1
        except Exception:
            continue

        if progress and (i + 1) % 10000 == 0:
            progress("globals_pub", i + 1, total_pub)

    db.conn.executemany(
        "INSERT OR IGNORE INTO globals (rva, name, type_uid) "
        "VALUES (?,?,?)", batch)
    db.commit()
    return len(batch)


def import_lexlines(sess, db, progress=None):
    """Import source line info for every function → lexlines table."""
    conn = db.conn

    # Get all function RVAs + lengths
    rows = conn.execute("SELECT rva, length FROM functions").fetchall()
    total = len(rows)
    batch = []

    for idx, (func_rva, func_len) in enumerate(rows):
        try:
            lines = sess.session.findLinesByRVA(func_rva, func_len)
            for i in range(lines.count):
                ln = lines.Item(i)
                line_rva = int(ln.relativeVirtualAddress)
                line_num = int(ln.lineNumber)
                batch.append((func_rva, line_rva, line_num))
        except Exception:
            continue

        if len(batch) >= 50000:
            conn.executemany(
                "INSERT OR REPLACE INTO lexlines (func_rva, rva, line) "
                "VALUES (?,?,?)", batch)
            batch.clear()

        if progress and (idx + 1) % 2000 == 0:
            progress("lexlines", idx + 1, total)

    if batch:
        conn.executemany(
            "INSERT OR REPLACE INTO lexlines (func_rva, rva, line) "
            "VALUES (?,?,?)", batch)
    db.commit()
    return total


def import_vtables(sess, db, uids: _UIDMap, progress=None):
    """Import vtable slots: PE ground truth + PDB declarations for purecall gaps.

    For each polymorphic UDT:
    1. Read PE vtable slots — concrete methods get exact positions.
    2. Collect PDB virtual method declarations (hierarchy walk, root→derived).
    3. Count-based dedup: PE-placed methods consume from PDB list.
    4. Remaining PDB methods fill _purecall slots in declaration order.

    Result: fully resolved slot→method map with no purecall entries.
    """
    conn = db.conn
    pe = sess.pe
    image_base = sess.image_base

    # All polymorphic types
    rows = conn.execute(
        "SELECT uid, name FROM types WHERE is_polymorphic=1"
    ).fetchall()
    total = len(rows)
    batch = []

    # Pre-build vtable lookup: mangled class key → [(rva, mangled_name), ...]
    vt_globals = conn.execute(
        "SELECT rva, name FROM globals WHERE name LIKE '??_7%@@6B%'"
    ).fetchall()

    vt_by_class: dict[str, list[tuple[int, str]]] = {}
    for rva, name in vt_globals:
        start = 4  # len("??_7")
        end = name.find("@@6B")
        if end <= start:
            continue
        cls_key = name[start:end]
        vt_by_class.setdefault(cls_key, []).append((rva, name))

    # Map type_uid → mangled class key (once)
    uid_to_key: dict[int, str] = {}
    for type_uid, type_name in rows:
        parts = type_name.split("::")
        uid_to_key[type_uid] = "@".join(reversed(parts))

    # Pre-cache functions table for fast RVA lookup
    func_map: dict[int, tuple] = {}
    for frva, fname, fcc, fret, fcleanup in conn.execute(
        "SELECT rva, name, callconv, ret_uid, cleanup FROM functions"
    ).fetchall():
        func_map[frva] = (fname, fcc, fret, fcleanup)

    for idx, (type_uid, type_name) in enumerate(rows):
        key = uid_to_key[type_uid]
        vt_rows = vt_by_class.get(key)

        # Collect PDB virtual declarations for purecall filling
        pdb_virtuals = _collect_pdb_virtuals(sess, type_name, uids)

        if not vt_rows:
            # No PE vtable (pure DLL interface like IRenderer/ISound).
            # Use PDB declarations only. Slot 0 = dtor (skip).
            for si, (owner, mname, ret_uid, cc) in enumerate(pdb_virtuals):
                batch.append((type_uid, 0, (si + 1) * 4, f"{owner}::{mname}",
                              0, ret_uid, cc, 0))
            if progress and (idx + 1) % 200 == 0:
                progress("vtables", idx + 1, total)
            continue

        # Walk ALL vtables for this type (MI: one per base with virtuals)
        for vt_rva, _vt_name in vt_rows:
            # Phase 1: read PE slots
            pe_slots: dict[int, tuple] = {}  # slot_off → (name, rva, ret_uid, cc, cleanup)
            pure_slots: list[int] = []
            placed_counts: dict[str, int] = {}

            slot_off = 0
            while True:
                try:
                    data = pe.get_data(vt_rva + slot_off, 4)
                    if len(data) < 4:
                        break
                    target_va = struct.unpack("<I", data)[0]
                    target_rva = target_va - image_base
                    if target_rva <= 0 or target_rva > 0x1000000:
                        break
                    if not _is_executable(sess, target_rva):
                        break

                    func = func_map.get(target_rva)
                    if func:
                        mname, cc, r_uid, cleanup = func
                    else:
                        mname = f"sub_{target_rva:x}"
                        r_uid, cc, cleanup = 0, "thiscall", 0

                    if "_purecall" in mname:
                        pure_slots.append(slot_off)
                    else:
                        pe_slots[slot_off] = (mname, target_rva, r_uid, cc, cleanup)
                        placed_counts[mname] = placed_counts.get(mname, 0) + 1

                    slot_off += 4
                except Exception:
                    break

            # Phase 2: emit PE-placed slots
            for soff, (mname, target_rva, r_uid, cc, cleanup) in pe_slots.items():
                batch.append((type_uid, vt_rva, soff, mname,
                              target_rva, r_uid, cc, cleanup))

            # Phase 3: fill purecall slots with PDB declarations
            # Filter out methods already placed by PE (count-based dedup)
            remaining = []
            for owner, mname, ret_uid, cc in pdb_virtuals:
                qname = f"{owner}::{mname}"
                cnt = placed_counts.get(qname, 0)
                if cnt > 0:
                    placed_counts[qname] = cnt - 1
                else:
                    remaining.append((owner, mname, ret_uid, cc))

            for soff, (owner, mname, ret_uid, cc) in zip(pure_slots, remaining):
                batch.append((type_uid, vt_rva, soff, f"{owner}::{mname}",
                              0, ret_uid, cc, 0))

        if progress and (idx + 1) % 200 == 0:
            progress("vtables", idx + 1, total)

    conn.executemany(
        "INSERT OR REPLACE INTO vtables "
        "(type_uid, vt_rva, slot_offset, method_name, method_rva, "
        "ret_uid, callconv, cleanup) "
        "VALUES (?,?,?,?,?,?,?,?)", batch)
    db.commit()
    return len(batch)


def _collect_pdb_virtuals(sess, type_name: str, uids: _UIDMap
                          ) -> list[tuple[str, str, int, str]]:
    """Collect virtual methods from full hierarchy in declaration order.

    Returns [(owner_type, method_name, ret_uid, callconv), ...]
    Walks primary base chain to root, then collects root→derived.
    Overrides replace the base slot (matched by undecoratedName).
    """
    skip = {"__local_vftable_ctor_closure", "__vecDelDtor"}

    # Walk primary base chain to root
    chain: list[str] = []
    cur = type_name
    visited: set[str] = set()
    while cur and cur not in visited:
        visited.add(cur)
        chain.append(cur)
        try:
            udts = sess.global_scope.findChildren(cvconst.SymTag.UDT, cur, 0)
            if udts.count == 0:
                break
            bases = udts.Item(0).findChildren(cvconst.SymTag.BaseClass, None, 0)
            found = False
            for i in range(bases.count):
                b = bases.Item(i)
                try:
                    if int(b.offset) == 0:
                        cur = b.type.name if b.type else b.name
                        found = True
                        break
                except Exception:
                    continue
            if not found:
                break
        except Exception:
            break

    chain.reverse()  # root → derived

    # (owner, name, ret_uid, callconv) — keyed by signature for override detection
    vtable: list[tuple[str, str, int, str, str]] = []  # +sig for matching

    for udt_name in chain:
        try:
            udts = sess.global_scope.findChildren(cvconst.SymTag.UDT, udt_name, 0)
            if udts.count == 0:
                continue
            udt = udts.Item(0)
            short = udt_name.split("::")[-1]
            funcs = udt.findChildren(cvconst.SymTag.Function, None, 0)
            for i in range(funcs.count):
                f = funcs.Item(i)
                try:
                    fname = f.name or ""
                    if fname in skip or fname == short or fname == f"~{short}":
                        continue
                    if not f.virtual:
                        continue
                    sig = ""
                    try:
                        sig = f.undecoratedName or ""
                    except Exception:
                        pass
                    # Return type UID
                    ret_uid = 0
                    cc = "thiscall"
                    try:
                        ft = f.type
                        if ft:
                            cc_raw = int(ft.callingConvention)
                            cc = CC_MAP.get(cc_raw, "thiscall")
                            if ft.type:
                                ret_uid = _resolve_dia_type(ft.type, uids)
                    except Exception:
                        pass
                    # Override: same signature in base → replace
                    overridden = False
                    for si, (owner, mname, _, _, msig) in enumerate(vtable):
                        if sig and msig == sig:
                            vtable[si] = (udt_name, fname, ret_uid, cc, sig)
                            overridden = True
                            break
                    if not overridden:
                        vtable.append((udt_name, fname, ret_uid, cc, sig))
                except Exception:
                    continue
        except Exception:
            continue

    # Strip sig from result
    return [(owner, name, ret_uid, cc) for owner, name, ret_uid, cc, _ in vtable]


def import_scalars(db, uids: _UIDMap):
    """Ensure well-known scalar types exist in the types table."""
    scalars = [
        ("void", 0), ("bool", 1), ("char", 1), ("unsigned char", 1),
        ("signed char", 1), ("wchar_t", 2),
        ("short", 2), ("unsigned short", 2),
        ("int", 4), ("unsigned int", 4),
        ("long", 4), ("unsigned long", 4),
        ("__int64", 8), ("unsigned __int64", 8),
        ("float", 4), ("double", 8),
        ("HRESULT", 4),
    ]
    batch = []
    for name, size in scalars:
        suid = uids.get_or_create(name, "scalar", size)
        batch.append((suid, name, size, "scalar", 0, 0, 0, 0, 0))

    db.conn.executemany(
        "INSERT OR REPLACE INTO types "
        "(uid, name, size, kind, is_polymorphic, pointee_uid, underlying_uid, element_uid, element_count) "
        "VALUES (?,?,?,?,?,?,?,?,?)", batch)
    db.commit()
    return len(batch)


def flush_derived_types(db, uids: _UIDMap):
    """Flush pointer/ref/array/scalar entries from UIDMap into types table.

    These are created during field/param/return type resolution but not
    explicitly imported. UDTs and enums are already written by their own
    import passes — this catches everything else.
    """
    batch = []
    for e in uids.entries():
        if not e.kind:
            # Bare name placeholder (UDT seen via pointer before import_types).
            # Will be filled by import_types — skip here, or write as placeholder.
            continue
        # Only write derived types (pointer, ref, array, scalar).
        # UDTs/enums are already in the table from their import passes.
        if e.kind in ("pointer", "ref", "array", "scalar", "functype"):
            batch.append((e.uid, e.name, e.size, e.kind, 0,
                          e.pointee, 0, e.element_uid, e.element_count))

    if batch:
        db.conn.executemany(
            "INSERT OR REPLACE INTO types "
            "(uid, name, size, kind, is_polymorphic, pointee_uid, "
            "underlying_uid, element_uid, element_count) "
            "VALUES (?,?,?,?,?,?,?,?,?)", batch)
        db.commit()
    return len(batch)


def import_compilands(sess, db, progress=None):
    """Import compilands (translation units) + their source files + func mapping.

    Walks DIA compiland symbols to get:
    - compiland name (object file path)
    - source files referenced by each compiland (#includes)
    - which functions belong to which compiland (via section contributions)
    """
    gs = sess.global_scope
    comp_enum = gs.findChildren(cvconst.SymTag.Compiland, None, 0)
    total = comp_enum.count
    conn = db.conn

    src_batch = []
    func_comp_batch = []

    for i in range(total):
        try:
            comp = comp_enum.Item(i)
            name = comp.name or ""
            if not name:
                continue

            # Extract short name (basename)
            short = name.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]

            # Insert compiland and get ID
            conn.execute(
                "INSERT OR IGNORE INTO compilands (name, short_name) VALUES (?,?)",
                (name, short))
            row = conn.execute(
                "SELECT id FROM compilands WHERE name=?", (name,)
            ).fetchone()
            if not row:
                continue
            comp_id = row[0]

            # Source files for this compiland
            try:
                src_files = sess.session.findFile(comp, None, 0)
                for si in range(src_files.count):
                    try:
                        sf = src_files.Item(si)
                        fpath = sf.fileName or ""
                        if fpath:
                            src_batch.append((comp_id, fpath))
                    except Exception:
                        continue
            except Exception:
                pass

            # Functions in this compiland
            try:
                funcs = comp.findChildren(cvconst.SymTag.Function, None, 0)
                for fi in range(funcs.count):
                    try:
                        func = funcs.Item(fi)
                        frva = int(func.relativeVirtualAddress)
                        if frva > 0:
                            func_comp_batch.append((frva, comp_id))
                    except Exception:
                        continue
            except Exception:
                pass

        except Exception:
            continue

        if progress and (i + 1) % 200 == 0:
            progress("compilands", i + 1, total)

    conn.executemany(
        "INSERT OR IGNORE INTO source_files (compiland_id, file_path) "
        "VALUES (?,?)", src_batch)
    conn.executemany(
        "INSERT OR REPLACE INTO func_compilands (func_rva, compiland_id) "
        "VALUES (?,?)", func_comp_batch)
    db.commit()

    comp_count = conn.execute("SELECT COUNT(*) FROM compilands").fetchone()[0]
    return comp_count


def _load_existing_uids(db) -> dict[str, int]:
    """Load existing name→uid from DB for UID persistence across re-imports."""
    try:
        rows = db.conn.execute("SELECT name, uid FROM types").fetchall()
        return {name: uid for name, uid in rows}
    except Exception:
        return {}


def _mangle_vtable(type_name: str) -> str:
    """'ns::Class' → '??_7Class@ns@@6B@'."""
    parts = type_name.split("::")
    if not parts:
        return ""
    return "??_7" + "@".join(reversed(parts)) + "@@6B@"


def _is_executable(sess, rva: int) -> bool:
    for start, end in sess.exec_ranges:
        if start <= rva < end:
            return True
    return False


# ── Main ────────────────────────────────────────────────────────────

def import_all(sess, db_path: str | Path, progress=None) -> dict:
    """Full PDB import into TypeDB.

    Two-pass type import: names first (UIDs assigned), then fields.
    Existing UIDs from previous runs are preserved.
    """
    # Support both relative and direct import
    try:
        from .._typedb import TypeDB, UIDGen
    except ImportError:
        from provider.lora.persifal._typedb import TypeDB, UIDGen

    db = TypeDB(db_path)
    db.open()

    gen = UIDGen()

    # Load existing UIDs for persistence
    existing = _load_existing_uids(db)
    uids = _UIDMap(gen, existing_uids=existing if existing else None)

    stats = {}
    t0 = time.time()

    def prog(phase, i, total, rate=0):
        elapsed = time.time() - t0
        if not rate:
            rate = i / elapsed if elapsed > 0 else 0
        if progress:
            progress(phase, i, total, rate)
        sys.stdout.write(f"\r  {phase}: [{i}/{total}] {rate:.0f}/s")
        sys.stdout.flush()

    print("Importing scalars...")
    stats["scalars"] = import_scalars(db, uids)

    # Pass 1: Assign UIDs to all type names
    print("Registering type names...")
    stats["type_names"] = import_type_names(sess, uids, prog)
    print(f"\n  {stats['type_names']} type names registered")

    # Pass 2: Import type fields (all pointee UIDs now resolvable)
    print("Importing types + fields...")
    stats["types"] = import_types(sess, db, uids, prog)

    print("\nImporting enums...")
    stats["enums"] = import_enums(sess, db, uids, prog)

    print("\nImporting functions + params + locals...")
    stats["functions"] = import_functions(sess, db, uids, prog)

    print("\nImporting globals...")
    stats["globals"] = import_globals(sess, db, uids, prog)

    print("\nImporting lexlines...")
    stats["lexlines"] = import_lexlines(sess, db, prog)

    print("\nImporting compilands + source files...")
    stats["compilands"] = import_compilands(sess, db, prog)

    print("\nImporting vtables from PE...")
    stats["vtable_slots"] = import_vtables(sess, db, uids, prog)

    # Flush derived types (pointer/ref/array created during resolution)
    print("\nFlushing derived types...")
    stats["derived"] = flush_derived_types(db, uids)

    # Store UID count
    db.conn.execute(
        "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)",
        ("uid_count", str(len(uids))))
    db.commit()

    elapsed = time.time() - t0
    print(f"\n\nDone in {elapsed:.1f}s  ({len(uids)} unique types)")
    full_stats = db.stats()
    for k, v in full_stats.items():
        print(f"  {k}: {v}")

    if existing:
        print(f"  persisted UIDs: {len(existing)} (reused from previous run)")

    db.close()
    return stats


# ── CLI ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python pdb_import.py <exe> <pdb> [db_path]")
        sys.exit(1)

    import os
    root = os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
    sys.path.insert(0, root)
    os.chdir(root)

    from provider.lora._state import SessionManager

    exe_path = sys.argv[1]
    pdb_path = sys.argv[2]
    # Same db as analyzer: game.pdb → game.persifal.db
    db_path = sys.argv[3] if len(sys.argv) > 3 else Path(pdb_path).with_suffix(".persifal.db")

    mgr = SessionManager()
    sess = mgr.load(exe_path, pdb_path)
    import_all(sess, db_path)
