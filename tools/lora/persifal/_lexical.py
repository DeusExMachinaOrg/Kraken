"""Lexical span tracker: maps RVA ranges to PDB source line groups.

Uses a Stack-like interval structure to track which source line covers
which RVA range.  Query by RVA to get the lexical group and its size.

Large lexical groups (single source line covering many instructions)
indicate inlined function bodies — the call site is one source line
but the compiler expanded the callee inline.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass

from ._ir import IR


@dataclass
class LexicalSpan:
    """One contiguous range of instructions sharing the same source line."""
    lexical: int        # PDB source line number
    start: int          # first RVA in span
    end: int            # last RVA in span (inclusive)
    node_count: int     # number of IR nodes in this span
    first_idx: int      # index of first node in the original node list
    last_idx: int       # index of last node (inclusive)

    @property
    def byte_size(self) -> int:
        return self.end - self.start

    @property
    def inline_cost(self) -> float:
        """Inline probability: byte_size / 96. >1.0 = likely inline."""
        return self.byte_size / 96

    def __repr__(self) -> str:
        cost = f" cost={self.inline_cost:.1f}" if self.inline_cost >= 1.0 else ""
        return (f"LexicalSpan(L{self.lexical} [{self.start:#x}..{self.end:#x}] "
                f"{self.node_count} nodes, {self.byte_size} bytes{cost})")


class LexicalMap:
    """Interval map: RVA → LexicalSpan.

    Built from an IR node list.  Supports O(log n) lookup by RVA.
    Each span carries a mnemonic fingerprint for inline matching.
    """

    __slots__ = ("_starts", "_spans")

    def __init__(self, nodes: list[IR]) -> None:
        self._starts: list[int] = []   # sorted span start RVAs
        self._spans: list[LexicalSpan] = []
        self._build(nodes)

    def _build(self, nodes: list[IR]) -> None:
        """Walk IR nodes, build contiguous lexical spans."""
        if not nodes:
            return

        cur_lex = -1
        span_start_rva = 0
        span_start_idx = 0

        for i, n in enumerate(nodes):
            if n.lexical > 0 and n.lexical != cur_lex:
                if cur_lex > 0 and i > span_start_idx:
                    self._emit(cur_lex, span_start_rva, span_start_idx,
                               nodes[i - 1].rva, i - 1)
                cur_lex = n.lexical
                span_start_rva = n.rva
                span_start_idx = i

        if cur_lex > 0 and len(nodes) > span_start_idx:
            self._emit(cur_lex, span_start_rva, span_start_idx,
                       nodes[-1].rva, len(nodes) - 1)

    def _emit(self, lex: int, start_rva: int, start_idx: int,
              end_rva: int, end_idx: int) -> None:
        span = LexicalSpan(
            lexical=lex, start=start_rva, end=end_rva,
            node_count=end_idx - start_idx + 1,
            first_idx=start_idx, last_idx=end_idx,
        )
        self._starts.append(start_rva)
        self._spans.append(span)

    def at_rva(self, rva: int) -> LexicalSpan | None:
        """Find the lexical span containing this RVA. O(log n)."""
        pos = bisect.bisect_right(self._starts, rva) - 1
        if pos < 0:
            return None
        span = self._spans[pos]
        if rva <= span.end:
            return span
        return None


    def __iter__(self):
        return iter(self._spans)

    def __len__(self) -> int:
        return len(self._spans)

    def __repr__(self) -> str:
        return f"LexicalMap({len(self._spans)} spans)"
