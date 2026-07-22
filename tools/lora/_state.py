"""Unified session manager for PE + PDB targets."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any

import pefile
from pydia2 import CreateObject, dia

from . import _pe


@dataclass
class Session:
    """Holds PE + PDB state for one loaded target."""
    alias: str
    exe_path: str
    pdb_path: str
    pe: pefile.PE
    session: Any          # IDiaSession
    global_scope: Any     # IDiaSymbol
    image_base: int
    is_64: bool
    iat_map: dict[int, dict] = field(default_factory=dict)
    exec_ranges: list[tuple[int, int]] = field(default_factory=list)

    # Caches
    sym_cache: dict[int, str | None] = field(default_factory=dict)
    vftable_cache: dict[int, list[dict]] = field(default_factory=dict)
    proto_cache_by_rva: dict[int, dict] = field(default_factory=dict)
    proto_cache_by_name: dict[str, dict] = field(default_factory=dict)


class SessionManager:
    """Manages named PE+PDB sessions. Default = first loaded."""

    def __init__(self):
        self._sessions: dict[str, Session] = {}

    def load(self, exe_path: str, pdb_path: str, alias: str | None = None) -> Session:
        exe_path = os.path.abspath(exe_path)
        pdb_path = os.path.abspath(pdb_path)

        if alias is None:
            alias = os.path.splitext(os.path.basename(exe_path))[0]

        pe = _pe.load_pe(exe_path)
        image_base = pe.OPTIONAL_HEADER.ImageBase
        is_64 = _pe.is_64bit(pe)
        iat_map = _pe.build_iat_map(pe, image_base)
        exec_ranges = _pe.build_exec_ranges(pe)

        src = CreateObject(dia.DiaSource, dia.IDiaDataSource)
        src.loadDataFromPdb(pdb_path)
        dia_session = src.openSession()
        global_scope = dia_session.globalScope

        sess = Session(
            alias=alias,
            exe_path=exe_path,
            pdb_path=pdb_path,
            pe=pe,
            session=dia_session,
            global_scope=global_scope,
            image_base=image_base,
            is_64=is_64,
            iat_map=iat_map,
            exec_ranges=exec_ranges,
        )
        self._sessions[alias] = sess
        return sess

    def get(self, alias: str | None = None) -> Session:
        if not self._sessions:
            raise ValueError("No target loaded. Call load() first.")
        if alias is None:
            return next(iter(self._sessions.values()))
        if alias not in self._sessions:
            raise ValueError(f"Unknown alias '{alias}'. Loaded: {list(self._sessions.keys())}")
        return self._sessions[alias]

    def list_aliases(self) -> list[str]:
        return list(self._sessions.keys())

    @property
    def loaded(self) -> bool:
        return len(self._sessions) > 0


# Module-level singleton
manager = SessionManager()
