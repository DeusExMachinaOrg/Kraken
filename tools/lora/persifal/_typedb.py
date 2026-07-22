"""Type database — SQLite schema + queries.

Single source of truth for all type information. PDB is just the
initial import source; custom overrides go into the same tables.

Type identity: snowflake UID (time_ms << 16 | counter).
Every type — including pointer/ref variants — has its own UID.
All references are a single type_uid INTEGER. No ptr/ref columns.

Pointer chain: CStr(uid=100) ← CStr*(uid=101, pointee=100) ← CStr**(uid=102, pointee=101)
Reference:     CStr(uid=100) ← CStr&(uid=103, pointee=100)
"""

from __future__ import annotations

import sqlite3
import time as _time
from pathlib import Path


# ── UID generator ───────────────────────────────────────────────────

class UIDGen:
    """Snowflake-style UID: (time_ms << 16) | counter."""

    def __init__(self) -> None:
        self._last_ms: int = 0
        self._counter: int = 0

    def next(self) -> int:
        ms = int(_time.time() * 1000)
        if ms == self._last_ms:
            self._counter += 1
        else:
            self._last_ms = ms
            self._counter = 0
        return (ms << 16) | (self._counter & 0xFFFF)


# Global generator
_uid = UIDGen()

def uid() -> int:
    return _uid.next()


# ── Schema ──────────────────────────────────────────────────────────

TYPEDB_SCHEMA = """
-- All types: UDT, enum, scalar, pointer, ref, array, typedef
CREATE TABLE IF NOT EXISTS types (
    uid             INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    size            INTEGER NOT NULL DEFAULT 0,
    kind            TEXT NOT NULL DEFAULT 'udt',
    is_polymorphic  INTEGER NOT NULL DEFAULT 0,
    -- pointer/ref: what this points to (0 = not a pointer)
    pointee_uid     INTEGER NOT NULL DEFAULT 0,
    -- enum: underlying type
    underlying_uid  INTEGER NOT NULL DEFAULT 0,
    -- array: element type + count
    element_uid     INTEGER NOT NULL DEFAULT 0,
    element_count   INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_types_name ON types(name);

-- Struct/class fields
CREATE TABLE IF NOT EXISTS fields (
    type_uid    INTEGER NOT NULL,
    offset      INTEGER NOT NULL,
    name        TEXT NOT NULL,
    field_uid   INTEGER NOT NULL,       -- fully resolved type UID (may be pointer/ref)
    field_size  INTEGER NOT NULL DEFAULT 0,
    is_base     INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (type_uid, offset, name)
);
CREATE INDEX IF NOT EXISTS idx_fields_type ON fields(type_uid);

-- Virtual method table slots
CREATE TABLE IF NOT EXISTS vtables (
    type_uid     INTEGER NOT NULL,
    vt_rva       INTEGER NOT NULL DEFAULT 0,  -- vtable base RVA (distinguishes MI bases)
    slot_offset  INTEGER NOT NULL,
    method_name  TEXT NOT NULL,
    method_rva   INTEGER NOT NULL DEFAULT 0,
    ret_uid      INTEGER NOT NULL DEFAULT 0,
    callconv     TEXT NOT NULL DEFAULT 'thiscall',
    cleanup      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (type_uid, vt_rva, slot_offset)
);
CREATE INDEX IF NOT EXISTS idx_vtables_type ON vtables(type_uid);
CREATE INDEX IF NOT EXISTS idx_vtables_rva ON vtables(vt_rva);

-- Functions
CREATE TABLE IF NOT EXISTS functions (
    rva           INTEGER PRIMARY KEY,
    name          TEXT NOT NULL,
    parent_uid    INTEGER NOT NULL DEFAULT 0,
    callconv      TEXT NOT NULL DEFAULT 'unknown',
    ret_uid       INTEGER NOT NULL DEFAULT 0,
    length        INTEGER NOT NULL DEFAULT 0,
    cleanup       INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_functions_name ON functions(name);
CREATE INDEX IF NOT EXISTS idx_functions_parent ON functions(parent_uid);

-- Function parameters
CREATE TABLE IF NOT EXISTS params (
    func_rva    INTEGER NOT NULL,
    idx         INTEGER NOT NULL,
    name        TEXT NOT NULL,
    type_uid    INTEGER NOT NULL,
    PRIMARY KEY (func_rva, idx)
);

-- Function locals
CREATE TABLE IF NOT EXISTS locals (
    func_rva    INTEGER NOT NULL,
    name        TEXT NOT NULL,
    type_uid    INTEGER NOT NULL,
    vframe_off  INTEGER NOT NULL,
    size        INTEGER NOT NULL DEFAULT 0,
    loc_kind    INTEGER NOT NULL DEFAULT 0,
    register    TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (func_rva, vframe_off, name)
);
CREATE INDEX IF NOT EXISTS idx_locals_func ON locals(func_rva);

-- Source line map
CREATE TABLE IF NOT EXISTS lexlines (
    func_rva    INTEGER NOT NULL,
    rva         INTEGER NOT NULL,
    line        INTEGER NOT NULL,
    PRIMARY KEY (func_rva, rva)
);
CREATE INDEX IF NOT EXISTS idx_lexlines_func ON lexlines(func_rva);

-- Global/static data
CREATE TABLE IF NOT EXISTS globals (
    rva       INTEGER PRIMARY KEY,
    name      TEXT NOT NULL,
    type_uid  INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_globals_name ON globals(name);

-- Enum values
CREATE TABLE IF NOT EXISTS enums (
    type_uid     INTEGER NOT NULL,
    member_name  TEXT NOT NULL,
    value        INTEGER NOT NULL,
    PRIMARY KEY (type_uid, member_name)
);
CREATE INDEX IF NOT EXISTS idx_enums_type ON enums(type_uid);

-- Compilands (object files / translation units)
CREATE TABLE IF NOT EXISTS compilands (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL,       -- full path from PDB (e.g. d:\\src\\engine\\kernel.cpp)
    short_name  TEXT NOT NULL DEFAULT ''  -- basename (kernel.cpp)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_compilands_name ON compilands(name);
CREATE INDEX IF NOT EXISTS idx_compilands_short ON compilands(short_name);

-- Source files referenced by compilands
CREATE TABLE IF NOT EXISTS source_files (
    compiland_id  INTEGER NOT NULL,
    file_path     TEXT NOT NULL,
    PRIMARY KEY (compiland_id, file_path)
);
CREATE INDEX IF NOT EXISTS idx_srcfiles_comp ON source_files(compiland_id);

-- Function → compiland mapping
CREATE TABLE IF NOT EXISTS func_compilands (
    func_rva      INTEGER NOT NULL,
    compiland_id   INTEGER NOT NULL,
    PRIMARY KEY (func_rva)
);
CREATE INDEX IF NOT EXISTS idx_func_comp ON func_compilands(compiland_id);

-- Import metadata
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
"""


class TypeDB:
    """SQLite-backed type database.

    Hot mode: call preheat() after open to load all tables into memory.
    Query methods check in-memory dicts first, fall back to SQLite.
    """

    def __init__(self, db_path: str | Path) -> None:
        self.db_path = Path(db_path)
        self._conn: sqlite3.Connection | None = None
        self._hot = False
        # In-memory mirrors (populated by preheat)
        self._types: dict[int, tuple] = {}          # uid → row
        self._type_names: dict[str, int] = {}        # name → uid
        self._fields: dict[int, list[tuple]] = {}    # type_uid → [(off, name, fuid, fsize, is_base)]
        self._vtslots: dict[tuple, tuple] = {}       # (type_uid, vt_rva, slot_off) → row
        self._vt_rvas: dict[int, list[int]] = {}     # type_uid → [vt_rva, ...]
        self._vt_counts: dict[tuple, int] = {}       # (type_uid, vt_rva) → slot count
        self._primary_vt: dict[int, int] = {}        # type_uid → primary vt_rva (cached)
        self._funcs: dict[int, tuple] = {}           # rva → row
        self._func_names: dict[str, tuple] = {}      # name → row
        self._params: dict[int, list[tuple]] = {}    # func_rva → [(idx, name, type_uid)]
        self._locals: dict[int, list[tuple]] = {}    # func_rva → [(name, type_uid, ...)]
        self._globals: dict[int, tuple] = {}         # rva → (rva, name, type_uid)
        self._global_names: dict[str, tuple] = {}    # name → row

    def open(self) -> sqlite3.Connection:
        if self._conn is None:
            self._conn = sqlite3.connect(str(self.db_path))
            self._conn.execute("PRAGMA journal_mode=WAL")
            self._conn.execute("PRAGMA synchronous=NORMAL")
            self._conn.execute("PRAGMA foreign_keys=OFF")
            self._conn.executescript(TYPEDB_SCHEMA)
        return self._conn

    def preheat(self) -> dict[str, int]:
        """Load all tables into memory. Returns row counts."""
        c = self.conn
        # types
        for row in c.execute(
            "SELECT uid, name, size, kind, is_polymorphic, "
            "pointee_uid, underlying_uid, element_uid, element_count FROM types"
        ):
            self._types[row[0]] = row
            self._type_names[row[1]] = row[0]
        # fields — group by type_uid
        for row in c.execute(
            "SELECT type_uid, offset, name, field_uid, field_size, is_base "
            "FROM fields ORDER BY type_uid, offset"
        ):
            self._fields.setdefault(row[0], []).append(row[1:])
        # vtables — index by (type_uid, vt_rva, slot_offset)
        for row in c.execute(
            "SELECT type_uid, vt_rva, slot_offset, method_name, "
            "method_rva, ret_uid, callconv, cleanup FROM vtables"
        ):
            type_uid, vt_rva, slot_off = row[0], row[1], row[2]
            self._vtslots[(type_uid, vt_rva, slot_off)] = row[3:]
            self._vt_rvas.setdefault(type_uid, [])
            if vt_rva not in self._vt_rvas[type_uid]:
                self._vt_rvas[type_uid].append(vt_rva)
            key = (type_uid, vt_rva)
            self._vt_counts[key] = self._vt_counts.get(key, 0) + 1
        # precompute primary vt_rva per type
        for type_uid, rvas in self._vt_rvas.items():
            pe_rvas = [r for r in rvas if r > 0]
            if pe_rvas:
                self._primary_vt[type_uid] = max(pe_rvas,
                    key=lambda r: self._vt_counts.get((type_uid, r), 0))
            elif 0 in rvas:
                self._primary_vt[type_uid] = 0
            else:
                self._primary_vt[type_uid] = -1
        # functions
        for row in c.execute(
            "SELECT rva, name, parent_uid, callconv, ret_uid, length, cleanup "
            "FROM functions"
        ):
            self._funcs[row[0]] = row
            self._func_names[row[1]] = row
        # params — group by func_rva
        for row in c.execute(
            "SELECT func_rva, idx, name, type_uid FROM params ORDER BY func_rva, idx"
        ):
            self._params.setdefault(row[0], []).append(row[1:])
        # locals — group by func_rva
        for row in c.execute(
            "SELECT func_rva, name, type_uid, vframe_off, size, loc_kind, register "
            "FROM locals"
        ):
            self._locals.setdefault(row[0], []).append(row[1:])
        # globals
        for row in c.execute("SELECT rva, name, type_uid FROM globals"):
            self._globals[row[0]] = row
            self._global_names[row[1]] = row
        self._hot = True
        return {
            "types": len(self._types),
            "fields": sum(len(v) for v in self._fields.values()),
            "vtslots": len(self._vtslots),
            "functions": len(self._funcs),
            "params": sum(len(v) for v in self._params.values()),
            "locals": sum(len(v) for v in self._locals.values()),
            "globals": len(self._globals),
        }

    def close(self) -> None:
        if self._conn:
            self._conn.close()
            self._conn = None

    def commit(self) -> None:
        if self._conn:
            self._conn.commit()

    @property
    def conn(self) -> sqlite3.Connection:
        return self.open()

    # ── Type lookup ─────────────────────────────────────────────────

    def type_by_uid(self, uid: int) -> tuple | None:
        """(uid, name, size, kind, is_polymorphic, pointee_uid, underlying_uid, element_uid, element_count)."""
        if self._hot:
            return self._types.get(uid)
        return self.conn.execute(
            "SELECT uid, name, size, kind, is_polymorphic, pointee_uid, "
            "underlying_uid, element_uid, element_count "
            "FROM types WHERE uid=?", (uid,)
        ).fetchone()

    def type_by_name(self, name: str) -> tuple | None:
        """(uid, name, size, kind, is_polymorphic, pointee_uid, underlying_uid, element_uid, element_count)."""
        if self._hot:
            uid = self._type_names.get(name, 0)
            return self._types.get(uid) if uid else None
        return self.conn.execute(
            "SELECT uid, name, size, kind, is_polymorphic, pointee_uid, "
            "underlying_uid, element_uid, element_count "
            "FROM types WHERE name=?", (name,)
        ).fetchone()

    def type_uid(self, name: str) -> int:
        """Get UID for a type name. Returns 0 if not found."""
        if self._hot:
            return self._type_names.get(name, 0)
        row = self.conn.execute(
            "SELECT uid FROM types WHERE name=?", (name,)
        ).fetchone()
        return row[0] if row else 0

    # ── Type insertion ──────────────────────────────────────────────

    def insert_type(self, new_uid: int, name: str, size: int, kind: str,
                    pointee_uid: int = 0) -> None:
        """Insert a new type row. No-op if name already exists."""
        self.conn.execute(
            "INSERT OR IGNORE INTO types "
            "(uid, name, size, kind, pointee_uid) VALUES (?,?,?,?,?)",
            (new_uid, name, size, kind, pointee_uid),
        )
        if self._hot:
            row = (new_uid, name, size, kind, 0, pointee_uid, 0, 0, 0)
            self._types[new_uid] = row
            self._type_names[name] = new_uid

    # ── Field queries ───────────────────────────────────────────────

    def get_fields(self, type_uid: int) -> list[tuple]:
        """[(offset, name, field_uid, field_size, is_base), ...]."""
        if self._hot:
            return self._fields.get(type_uid, [])
        return self.conn.execute(
            "SELECT offset, name, field_uid, field_size, is_base "
            "FROM fields WHERE type_uid=? ORDER BY offset",
            (type_uid,)
        ).fetchall()

    def field_at(self, type_uid: int, offset: int) -> tuple | None:
        """Exact: (name, field_uid, field_size)."""
        if self._hot:
            for row in self._fields.get(type_uid, ()):
                if row[0] == offset:
                    return (row[1], row[2], row[3])
            return None
        return self.conn.execute(
            "SELECT name, field_uid, field_size FROM fields "
            "WHERE type_uid=? AND offset=?",
            (type_uid, offset)
        ).fetchone()

    def field_before(self, type_uid: int, offset: int) -> tuple | None:
        """Nearest before: (offset, name, field_uid, field_size)."""
        if self._hot:
            best = None
            for row in self._fields.get(type_uid, ()):
                if row[0] < offset:
                    best = (row[0], row[1], row[2], row[3])
                else:
                    break  # sorted by offset
            return best
        return self.conn.execute(
            "SELECT offset, name, field_uid, field_size FROM fields "
            "WHERE type_uid=? AND offset<? ORDER BY offset DESC LIMIT 1",
            (type_uid, offset)
        ).fetchone()

    # ── Vtable queries ──────────────────────────────────────────────

    def get_vtables_for_type(self, type_uid: int) -> list[int]:
        """Get distinct vtable RVAs for a type (MI may have multiple)."""
        if self._hot:
            return self._vt_rvas.get(type_uid, [])
        rows = self.conn.execute(
            "SELECT DISTINCT vt_rva FROM vtables WHERE type_uid=? ORDER BY vt_rva",
            (type_uid,)
        ).fetchall()
        return [r[0] for r in rows]

    def get_vtable(self, type_uid: int, vt_rva: int = 0) -> list[tuple]:
        """[(slot_offset, method_name, method_rva, ret_uid, callconv, cleanup)].

        If vt_rva=0, returns the primary (first) vtable.
        """
        if vt_rva == 0:
            if self._hot:
                rvas = self._vt_rvas.get(type_uid, [])
                if not rvas:
                    return []
                vt_rva = rvas[0]
            else:
                row = self.conn.execute(
                    "SELECT vt_rva FROM vtables WHERE type_uid=? ORDER BY vt_rva LIMIT 1",
                    (type_uid,)
                ).fetchone()
                if not row:
                    return []
                vt_rva = row[0]
        if self._hot:
            count = self._vt_counts.get((type_uid, vt_rva), 0)
            if count == 0:
                return []
            result = []
            for slot_off in range(0, count * 4, 4):
                row = self._vtslots.get((type_uid, vt_rva, slot_off))
                if row:
                    result.append((slot_off,) + row)
            return result
        return self.conn.execute(
            "SELECT slot_offset, method_name, method_rva, ret_uid, callconv, cleanup "
            "FROM vtables WHERE type_uid=? AND vt_rva=? ORDER BY slot_offset",
            (type_uid, vt_rva)
        ).fetchall()

    def get_primary_vt_rva(self, type_uid: int) -> int:
        """Get primary vtable RVA for a type.

        Prefers PE vtable (vt_rva > 0) over pure virtual (vt_rva = 0).
        Among PE vtables, picks the one with most slots (primary MI base).
        Falls back to 0 for pure-virtual-only interfaces (IRenderer etc).
        Returns -1 if no vtable exists at all.
        """
        if self._hot:
            return self._primary_vt.get(type_uid, -1)
        # Try PE vtables first (vt_rva > 0), pick largest
        row = self.conn.execute(
            "SELECT vt_rva FROM vtables WHERE type_uid=? AND vt_rva > 0 "
            "GROUP BY vt_rva ORDER BY COUNT(*) DESC LIMIT 1",
            (type_uid,)
        ).fetchone()
        if row:
            return row[0]
        # Fallback: pure virtual (vt_rva = 0)
        row = self.conn.execute(
            "SELECT 1 FROM vtables WHERE type_uid=? AND vt_rva = 0 LIMIT 1",
            (type_uid,)
        ).fetchone()
        return 0 if row else -1

    def get_vtslot(self, type_uid: int, offset: int, vt_rva: int = -1) -> tuple | None:
        """Single slot: (method_name, method_rva, ret_uid, callconv, cleanup).

        vt_rva=-1 (default): resolve primary vtable, then search.
        vt_rva>=0: exact match (0 = pure virtual, >0 = PE vtable).
        """
        if vt_rva < 0:
            vt_rva = self.get_primary_vt_rva(type_uid)
            if vt_rva < 0:
                return None
        if self._hot:
            return self._vtslots.get((type_uid, vt_rva, offset))
        return self.conn.execute(
            "SELECT method_name, method_rva, ret_uid, callconv, cleanup "
            "FROM vtables WHERE type_uid=? AND vt_rva=? AND slot_offset=?",
            (type_uid, vt_rva, offset)
        ).fetchone()

    # ── Function queries ────────────────────────────────────────────

    def get_function(self, rva: int) -> tuple | None:
        """(rva, name, parent_uid, callconv, ret_uid, length, cleanup)."""
        if self._hot:
            return self._funcs.get(rva)
        return self.conn.execute(
            "SELECT rva, name, parent_uid, callconv, ret_uid, length, cleanup "
            "FROM functions WHERE rva=?", (rva,)
        ).fetchone()

    def get_function_by_name(self, name: str) -> tuple | None:
        if self._hot:
            return self._func_names.get(name)
        return self.conn.execute(
            "SELECT rva, name, parent_uid, callconv, ret_uid, length, cleanup "
            "FROM functions WHERE name=?", (name,)
        ).fetchone()

    def get_params(self, func_rva: int) -> list[tuple]:
        """[(idx, name, type_uid), ...]."""
        if self._hot:
            return self._params.get(func_rva, [])
        return self.conn.execute(
            "SELECT idx, name, type_uid FROM params "
            "WHERE func_rva=? ORDER BY idx", (func_rva,)
        ).fetchall()

    def get_locals(self, func_rva: int) -> list[tuple]:
        """[(name, type_uid, vframe_off, size, loc_kind, register), ...]."""
        if self._hot:
            return self._locals.get(func_rva, [])
        return self.conn.execute(
            "SELECT name, type_uid, vframe_off, size, loc_kind, register "
            "FROM locals WHERE func_rva=?", (func_rva,)
        ).fetchall()

    # ── Global queries ──────────────────────────────────────────────

    def get_global(self, rva: int) -> tuple | None:
        """(rva, name, type_uid)."""
        if self._hot:
            return self._globals.get(rva)
        return self.conn.execute(
            "SELECT rva, name, type_uid FROM globals WHERE rva=?", (rva,)
        ).fetchone()

    def get_global_by_name(self, name: str) -> tuple | None:
        if self._hot:
            return self._global_names.get(name)
        return self.conn.execute(
            "SELECT rva, name, type_uid FROM globals WHERE name=?", (name,)
        ).fetchone()

    def all_functions(self) -> list[tuple[int, int, str]]:
        """[(rva, length, name), ...] — all functions from DB."""
        if self._hot:
            return [(r[0], r[5], r[1]) for r in self._funcs.values() if r[5] > 0]
        return self.conn.execute(
            "SELECT rva, length, name FROM functions WHERE length > 0"
        ).fetchall()

    # ── Enum queries ────────────────────────────────────────────────

    def get_enum_values(self, type_uid: int) -> dict[str, int]:
        rows = self.conn.execute(
            "SELECT member_name, value FROM enums WHERE type_uid=?",
            (type_uid,)
        ).fetchall()
        return {name: val for name, val in rows}

    # ── Lexical queries ─────────────────────────────────────────────

    def get_lexlines(self, func_rva: int) -> list[tuple]:
        """[(rva, line), ...] ordered by rva."""
        return self.conn.execute(
            "SELECT rva, line FROM lexlines WHERE func_rva=? ORDER BY rva",
            (func_rva,)
        ).fetchall()

    # ── Compiland queries ─────────────────────────────────────────

    def get_compiland(self, compiland_id: int) -> tuple | None:
        """(id, name, short_name)."""
        return self.conn.execute(
            "SELECT id, name, short_name FROM compilands WHERE id=?",
            (compiland_id,)
        ).fetchone()

    def find_compilands(self, pattern: str) -> list[tuple]:
        """Find compilands by LIKE pattern on short_name or name."""
        return self.conn.execute(
            "SELECT id, name, short_name FROM compilands "
            "WHERE short_name LIKE ? OR name LIKE ? ORDER BY short_name",
            (pattern, pattern)
        ).fetchall()

    def get_source_files(self, compiland_id: int) -> list[str]:
        """Get source/header files for a compiland."""
        rows = self.conn.execute(
            "SELECT file_path FROM source_files WHERE compiland_id=? ORDER BY file_path",
            (compiland_id,)
        ).fetchall()
        return [r[0] for r in rows]

    def get_func_compiland(self, func_rva: int) -> int:
        """Get compiland ID for a function. Returns 0 if not found."""
        row = self.conn.execute(
            "SELECT compiland_id FROM func_compilands WHERE func_rva=?",
            (func_rva,)
        ).fetchone()
        return row[0] if row else 0

    def get_compiland_functions(self, compiland_id: int) -> list[int]:
        """Get all function RVAs for a compiland."""
        rows = self.conn.execute(
            "SELECT func_rva FROM func_compilands WHERE compiland_id=?",
            (compiland_id,)
        ).fetchall()
        return [r[0] for r in rows]

    # ── Stats ───────────────────────────────────────────────────────

    def stats(self) -> dict:
        c = self.conn
        return {
            "types": c.execute("SELECT COUNT(*) FROM types").fetchone()[0],
            "fields": c.execute("SELECT COUNT(*) FROM fields").fetchone()[0],
            "vtables": c.execute("SELECT COUNT(*) FROM vtables").fetchone()[0],
            "functions": c.execute("SELECT COUNT(*) FROM functions").fetchone()[0],
            "params": c.execute("SELECT COUNT(*) FROM params").fetchone()[0],
            "locals": c.execute("SELECT COUNT(*) FROM locals").fetchone()[0],
            "globals": c.execute("SELECT COUNT(*) FROM globals").fetchone()[0],
            "enums": c.execute("SELECT COUNT(*) FROM enums").fetchone()[0],
            "lexlines": c.execute("SELECT COUNT(*) FROM lexlines").fetchone()[0],
            "compilands": c.execute("SELECT COUNT(*) FROM compilands").fetchone()[0],
            "source_files": c.execute("SELECT COUNT(*) FROM source_files").fetchone()[0],
        }
