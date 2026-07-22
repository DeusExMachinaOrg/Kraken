"""Analyzer — global state for exe-wide decompilation.

Holds the Session (PE+PDB), TypeProvider, and a per-function
analysis cache (rva → FuncInfo).  SQLite hot cache persists across runs.

Stream is dumped compactly: IR nodes as tuples, Type objects
flattened to name strings.  JumpMap stored as edge tuples.
Indexed columns (call_targets, edge_count, loop_count) enable fast
candidate queries from fold without deserializing blobs.
"""

from __future__ import annotations

import hashlib
import pickle
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from .._state import Session
from ._typeprovider import TypeProvider
from ._stream import IRStream
from ._jumpmap import JumpMap, JumpEdge, ScopeRegion, SwitchInfo, SwitchCase
from ._lexical import LexicalMap, LexicalSpan
from ._vm import Stack
from ._ir import (
    IR, IROp, Mn, Kind,
    IRMov, IRPush, IRPop, IRCall, IRCmp, IRJcc, IRJmp, IRRet, IRArith, IRNop,
)


# ── Per-function analysis bundle ─────────────────────────────────────

@dataclass
class FuncInfo:
    """Cached analysis bundle for one function."""
    rva: int
    length: int
    name: str
    stream: IRStream                     # enrich output (frozen)
    jumpmap: JumpMap                     # always present
    lexical: LexicalMap                  # always present


# ── Compact serialization ────────────────────────────────────────────
# Stream nodes → tuples, Type → name string, JumpMap → edge ints.
# No pickle of live objects — pure ints/strings/tuples.

_IR_TAG = {
    IRMov: 0, IRPush: 1, IRPop: 2, IRCall: 3, IRCmp: 4,
    IRJcc: 5, IRJmp: 6, IRRet: 7, IRArith: 8, IRNop: 9,
}
_TAG_IR = {v: k for k, v in _IR_TAG.items()}


def _pack_op(op: IROp) -> tuple:
    """IROp → compact tuple. sym → UID int."""
    sym_uid = op.sym.uid if op.sym is not None else 0
    return (
        op.kind, op.reg, op.mem_base, op.mem_index, op.mem_scale,
        op.mem_disp, op.imm, op.size,
        op.symbol, op.field, op.type, op.string, op.vframe,
        sym_uid, op.owner,
    )


def _unpack_op(t: tuple) -> IROp:
    """Compact tuple → IROp. sym stored as UID int in t[13], rehydrated later."""
    return IROp(
        kind=t[0], reg=t[1], mem_base=t[2], mem_index=t[3],
        mem_scale=t[4], mem_disp=t[5], imm=t[6], size=t[7],
        symbol=t[8], field=t[9], type=t[10], string=t[11], vframe=t[12],
        sym=t[13],  # UID int — rehydrated to Type by _rehydrate_stream
        owner=t[14] if len(t) > 14 else 0,
    )


def _pack_node(node: IR) -> tuple:
    """IR node → compact tuple."""
    tag = _IR_TAG.get(type(node), -1)
    mn_s = node.mn.value if node.mn else ""
    kind_s = node.kind.value if node.kind else ""
    ops = tuple(_pack_op(op) for op in node.ops)

    # subtype extras
    extra: tuple = ()
    if isinstance(node, IRCall):
        extra = (node.cleanup,)
    elif isinstance(node, IRJcc):
        extra = (node.target_rva,)
    elif isinstance(node, IRJmp):
        extra = (node.target_rva,)
    elif isinstance(node, IRRet):
        extra = (node.cleanup,)

    return (tag, mn_s, node.rva, node.lexical, kind_s, node.esp_delta, ops, extra)


def _unpack_node(t: tuple) -> IR:
    """Compact tuple → IR node."""
    tag, mn_s, rva, lexical, kind_s, esp_delta, ops_t, extra = t
    cls = _TAG_IR.get(tag, IR)
    mn = Mn(mn_s) if mn_s else None
    kind = Kind(kind_s) if kind_s else Kind.PAYLOAD
    ops = [_unpack_op(o) for o in ops_t]

    kwargs = dict(mn=mn, rva=rva, lexical=lexical, ops=ops, kind=kind, esp_delta=esp_delta)
    if cls is IRCall:
        kwargs["cleanup"] = extra[0] if extra else 0
    elif cls is IRJcc:
        kwargs["target_rva"] = extra[0] if extra else 0
    elif cls is IRJmp:
        kwargs["target_rva"] = extra[0] if extra else 0
    elif cls is IRRet:
        kwargs["cleanup"] = extra[0] if extra else 0

    return cls(**kwargs)


def _pack_jumpmap(jm: JumpMap) -> dict:
    """JumpMap → dict of int tuples."""
    edges = [
        (e.source_rva, e.target_rva, e.mn.value if e.mn else "",
         e.lexical, e.is_conditional, e.is_backward)
        for e in jm.edges
    ]
    scope_enter = {
        rva: [(r.start, r.end, r.kind.value) for r in regions]
        for rva, regions in jm.scope_enter.items()
    }
    switches = []
    for sw in jm.switches:
        cases = [(c.value, c.handler_rva) for c in sw.cases]
        switches.append((sw.jmp_rva, sw.default_rva, cases))

    return {
        "edges": edges,
        "loop_headers": list(jm.loop_headers),
        "targets": list(jm.targets),
        "scope_enter": scope_enter,
        "scope_exit": dict(jm.scope_exit),
        "switches": switches,
    }


def _unpack_jumpmap(d: dict) -> JumpMap:
    """Dict → JumpMap."""
    from ._jumpmap import ScopeKind
    jm = JumpMap()
    for e in d["edges"]:
        src, tgt, mn_s, lex, is_cond, is_back = e
        mn = Mn(mn_s) if mn_s else None
        edge = JumpEdge(src, tgt, mn, lex, is_cond, is_back)
        jm.edges.append(edge)
        jm.landings.setdefault(tgt, []).append(edge)
    jm.loop_headers = set(d["loop_headers"])
    jm.targets = set(d["targets"])
    for rva_s, regions in d["scope_enter"].items():
        rva = int(rva_s) if isinstance(rva_s, str) else rva_s
        jm.scope_enter[rva] = [
            ScopeRegion(r[0], r[1], ScopeKind(r[2])) for r in regions
        ]
    jm.scope_exit = {
        (int(k) if isinstance(k, str) else k): v
        for k, v in d["scope_exit"].items()
    }
    for sw_t in d["switches"]:
        jmp_rva, default_rva, cases_t = sw_t
        cases = [SwitchCase(v, h) for v, h in cases_t]
        handlers: dict[int, list[int]] = {}
        for v, h in cases_t:
            handlers.setdefault(h, []).append(v)
        jm.switches.append(SwitchInfo(jmp_rva, default_rva, cases, handlers))
    return jm


def _pack_lexical(lm: LexicalMap) -> list[tuple]:
    """LexicalMap → list of span tuples."""
    return [
        (s.lexical, s.start, s.end, s.node_count, s.first_idx, s.last_idx)
        for s in lm
    ]


def _unpack_lexical(spans: list[tuple]) -> LexicalMap:
    """Span tuples → LexicalMap (bypass __init__ which expects nodes)."""
    lm = object.__new__(LexicalMap)
    lm._starts = []
    lm._spans = []
    for lex, start, end, nc, fi, li in spans:
        lm._starts.append(start)
        lm._spans.append(LexicalSpan(lex, start, end, nc, fi, li))
    return lm


def _extract_call_targets(nodes: list[IR]) -> list[int]:
    """Extract sorted list of call target RVAs from IR nodes."""
    targets = []
    for n in nodes:
        if isinstance(n, IRCall) and n.ops:
            op = n.ops[0]
            if op.symbol:
                # Use imm as RVA for direct calls
                if op.kind == "imm" and op.imm:
                    targets.append(op.imm)
    return sorted(set(targets))


def _rehydrate_stream(stream: IRStream, provider: TypeProvider) -> None:
    """Restore live Type objects from UIDs stored during deserialization."""
    for node in stream._nodes:
        for i, op in enumerate(node.ops):
            uid = op.sym
            if isinstance(uid, int) and uid > 0:
                node.ops[i] = IROp(
                    kind=op.kind, reg=op.reg, mem_base=op.mem_base,
                    mem_index=op.mem_index, mem_scale=op.mem_scale,
                    mem_disp=op.mem_disp, imm=op.imm, size=op.size,
                    symbol=op.symbol, field=op.field, type=op.type,
                    string=op.string, vframe=op.vframe,
                    sym=provider._type_from_uid(uid),
                    owner=op.owner,
                )
            elif isinstance(uid, int):
                node.ops[i] = IROp(
                    kind=op.kind, reg=op.reg, mem_base=op.mem_base,
                    mem_index=op.mem_index, mem_scale=op.mem_scale,
                    mem_disp=op.mem_disp, imm=op.imm, size=op.size,
                    symbol=op.symbol, field=op.field, type=op.type,
                    string=op.string, vframe=op.vframe,
                    sym=None, owner=op.owner,
                )


def _serialize(info: FuncInfo) -> bytes:
    """FuncInfo → compact pickle (sym as UID int)."""
    nodes = [_pack_node(n) for n in info.stream._nodes]
    jm = _pack_jumpmap(info.jumpmap)
    lm = _pack_lexical(info.lexical)
    return pickle.dumps((nodes, jm, lm), protocol=5)


def _deserialize(rva: int, length: int, name: str, data: bytes) -> FuncInfo:
    """Compact pickle → FuncInfo. sym fields hold UID ints until rehydrated."""
    nodes_t, jm_d, lm_t = pickle.loads(data)
    nodes = [_unpack_node(t) for t in nodes_t]
    stream = IRStream(nodes)
    jm = _unpack_jumpmap(jm_d)
    lm = _unpack_lexical(lm_t)
    return FuncInfo(rva=rva, length=length, name=name,
                    stream=stream, jumpmap=jm, lexical=lm)


# ── SQLite hot cache ─────────────────────────────────────────────────

# Bump when enricher output format changes (invalidates cached funcs)
_ENRICHER_VERSION = 7   # v7: owner UID on field ops (inlinee detection)

_SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
CREATE TABLE IF NOT EXISTS funcs (
    rva          INTEGER PRIMARY KEY,
    length       INTEGER NOT NULL,
    name         TEXT NOT NULL,
    edge_count   INTEGER NOT NULL DEFAULT 0,
    loop_count   INTEGER NOT NULL DEFAULT 0,
    call_targets TEXT NOT NULL DEFAULT '',
    data         BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_funcs_length ON funcs(length);
CREATE INDEX IF NOT EXISTS idx_funcs_edge_count ON funcs(edge_count);
"""


def _db_path(pdb_path: str) -> Path:
    """Cache db next to PDB: game.pdb → game.persifal.db."""
    p = Path(pdb_path)
    return p.with_suffix(".persifal.db")


def _pdb_fingerprint(pdb_path: str) -> str:
    """Hash PDB file (first 64KB + size) for cache invalidation."""
    p = Path(pdb_path)
    size = p.stat().st_size
    head = p.read_bytes()[:65536]
    return hashlib.md5(head + size.to_bytes(8, "little")).hexdigest()


class _Cache:
    """SQLite-backed hot cache."""

    def __init__(self, pdb_path: str) -> None:
        self.db_file = _db_path(pdb_path)
        self.fingerprint = _pdb_fingerprint(pdb_path)
        self._conn: sqlite3.Connection | None = None

    def _open(self) -> sqlite3.Connection:
        if self._conn is None:
            self._conn = sqlite3.connect(str(self.db_file))
            self._conn.execute("PRAGMA journal_mode=WAL")
            self._conn.execute("PRAGMA synchronous=NORMAL")
            self._conn.executescript(_SCHEMA)
            row = self._conn.execute(
                "SELECT value FROM meta WHERE key='pdb_fp'"
            ).fetchone()
            ver_row = self._conn.execute(
                "SELECT value FROM meta WHERE key='enricher_ver'"
            ).fetchone()
            cached_ver = ver_row[0] if ver_row else ""
            if (row is None or row[0] != self.fingerprint
                    or cached_ver != str(_ENRICHER_VERSION)):
                self._conn.execute("DELETE FROM funcs")
                self._conn.execute(
                    "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)",
                    ("pdb_fp", self.fingerprint),
                )
                self._conn.execute(
                    "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)",
                    ("enricher_ver", str(_ENRICHER_VERSION)),
                )
                self._conn.commit()
        return self._conn

    def get(self, rva: int) -> tuple[int, str, bytes] | None:
        conn = self._open()
        row = conn.execute(
            "SELECT length, name, data FROM funcs WHERE rva=?", (rva,)
        ).fetchone()
        return row

    def put(self, info: FuncInfo) -> None:
        conn = self._open()
        call_rvas = _extract_call_targets(list(info.stream._nodes))
        call_str = ",".join(str(r) for r in call_rvas)
        conn.execute(
            "INSERT OR REPLACE INTO funcs "
            "(rva, length, name, edge_count, loop_count, call_targets, data) "
            "VALUES (?,?,?,?,?,?,?)",
            (info.rva, info.length, info.name,
             len(info.jumpmap.edges), len(info.jumpmap.loop_headers),
             call_str, _serialize(info)),
        )

    def commit(self) -> None:
        if self._conn:
            self._conn.commit()

    def count(self) -> int:
        conn = self._open()
        row = conn.execute("SELECT COUNT(*) FROM funcs").fetchone()
        return row[0] if row else 0

    def all_rvas(self) -> set[int]:
        conn = self._open()
        return {r[0] for r in conn.execute("SELECT rva FROM funcs")}

    def query_by_size(self, min_len: int, max_len: int,
                      limit: int = 100) -> list[tuple[int, int, str]]:
        """Fast SQL query: (rva, length, name) within size range."""
        conn = self._open()
        rows = conn.execute(
            "SELECT rva, length, name FROM funcs "
            "WHERE length BETWEEN ? AND ? LIMIT ?",
            (min_len, max_len, limit),
        ).fetchall()
        return rows

    def query_by_edges(self, min_e: int, max_e: int,
                       limit: int = 100) -> list[tuple[int, int, str, int]]:
        """Fast SQL query: (rva, length, name, edge_count) within edge range."""
        conn = self._open()
        return conn.execute(
            "SELECT rva, length, name, edge_count FROM funcs "
            "WHERE edge_count BETWEEN ? AND ? LIMIT ?",
            (min_e, max_e, limit),
        ).fetchall()

    def close(self) -> None:
        if self._conn:
            self._conn.close()
            self._conn = None


# ── Analyzer ─────────────────────────────────────────────────────────

class Analyzer:
    """Global decompiler state.

    Usage:
        az = Analyzer(sess)
        az.scan_all()                        # prescan (SQLite-backed)
        info = az.get(rva)                   # lazy load from cache
        info = az.process(rva, len, ...)     # full pipeline
    """

    def __init__(self, sess: Session, provider: TypeProvider | None = None) -> None:
        self.sess = sess
        self.provider = provider  # TypeProvider (set externally, or None)
        self.funcs: dict[int, FuncInfo] = {}   # rva → FuncInfo (in-memory)
        self._cache = _Cache(sess.pdb_path)

    @property
    def cache(self) -> _Cache:
        """Expose cache for direct SQL queries from fold."""
        return self._cache

    # ── single function pipeline ─────────────────────────────────────

    def process(self, rva: int, length: int, name: str = "",
                this_class: str = "", known: Stack | None = None) -> FuncInfo:
        """Run decode → enrich → jumpmap → lexical.

        Auto-detects this_class from name and builds known from PDB
        if not provided.  Caches in memory + SQLite.
        """
        from ._decode import decode
        from ._enrich import enrich, build_known
        from ._jumpmap import build_jumpmap

        if self.provider is None:
            raise RuntimeError("Analyzer requires a TypeProvider")

        # Auto-detect parent class from TypeProvider
        if not this_class:
            this_class = self.provider.get_parent_class(rva)

        # Auto-build known stack locals from TypeProvider
        if known is None:
            known = build_known(self.provider, rva)

        stream = decode(self.sess, rva, length)
        stream = enrich(self.provider, stream, this_class=this_class, known=known)
        jm = build_jumpmap(stream, sess=self.sess)
        lm = LexicalMap(list(stream._nodes))

        info = FuncInfo(rva=rva, length=length, name=name,
                        stream=stream, jumpmap=jm, lexical=lm)

        self.funcs[rva] = info
        self._cache.put(info)
        return info

    def purge_cache(self) -> int:
        """Drop all cached functions from SQLite and memory."""
        self.funcs.clear()
        conn = self._cache._open()
        cursor = conn.execute("SELECT COUNT(*) FROM funcs")
        count = cursor.fetchone()[0]
        conn.execute("DELETE FROM funcs")
        conn.commit()
        return count

    # ── bulk scan ────────────────────────────────────────────────────

    def scan_all(self, progress_cb=None) -> int:
        """Prescan all PDB functions (decode+enrich+jumpmap+lexical).

        SQLite-backed — second run is instant (skips cached).
        Returns total functions available.
        """
        from .. import _pdb as pdb_mod

        all_funcs = pdb_mod.enum_functions(self.sess)
        total = len(all_funcs)
        cached_rvas = self._cache.all_rvas()
        done = len(cached_rvas)
        new = 0

        for i, (rva, length, name) in enumerate(all_funcs):
            if rva in cached_rvas:
                continue
            try:
                self.process(rva, length, name=name)
                done += 1
                new += 1
            except Exception:
                pass
            if new % 200 == 0:
                self._cache.commit()
            if progress_cb and (i + 1) % 500 == 0:
                progress_cb(i + 1, total, done, new)

        self._cache.commit()
        return done

    # ── lookup ───────────────────────────────────────────────────────

    def get(self, rva: int) -> FuncInfo | None:
        """Lookup by RVA — memory first, then SQLite lazy load + rehydrate."""
        info = self.funcs.get(rva)
        if info is not None:
            return info
        row = self._cache.get(rva)
        if row is not None:
            length, name, data = row
            info = _deserialize(rva, length, name, data)
            if self.provider is not None:
                _rehydrate_stream(info.stream, self.provider)
            self.funcs[rva] = info
            return info
        return None

    def _load_class_methods(self, class_names: set[str]) -> int:
        """Load cached methods of specific classes into memory for fingerprinting.

        Only loads functions whose name starts with "ClassName::" for each class.
        Much cheaper than load_cache() for all 45k functions.
        """
        if not class_names:
            return 0
        conn = self._cache._open()
        count = 0
        for cls in class_names:
            prefix = cls + "::"
            cursor = conn.execute(
                "SELECT rva, length, name, data FROM funcs WHERE name LIKE ?",
                (prefix + "%",))
            for rva, length, name, data in cursor:
                if rva in self.funcs:
                    continue
                try:
                    info = _deserialize(rva, length, name, data)
                    if self.provider is not None:
                        _rehydrate_stream(info.stream, self.provider)
                    self.funcs[rva] = info
                    count += 1
                except Exception:
                    pass
        return count

    def load_cache(self) -> int:
        """Bulk load all cached functions into memory + rehydrate."""
        conn = self._cache._open()
        cursor = conn.execute("SELECT rva, length, name, data FROM funcs")
        count = 0
        for rva, length, name, data in cursor:
            if rva not in self.funcs:
                try:
                    info = _deserialize(rva, length, name, data)
                    if self.provider is not None:
                        _rehydrate_stream(info.stream, self.provider)
                    self.funcs[rva] = info
                    count += 1
                except Exception:
                    pass
        return count

    def stats(self) -> dict:
        return {
            "in_memory": len(self.funcs),
            "in_db": self._cache.count(),
        }
