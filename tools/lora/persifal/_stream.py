"""IR stream — read-only cursor over a frozen node list.

Immutable after construction. Fold stages read from a stream
and emit a new list of higher-level nodes.
"""

from __future__ import annotations

from ._ir import IR


class IRStream:
    """Read-only cursor over a flat list of IR nodes."""

    __slots__ = ("_nodes", "_pos")

    def __init__(self, nodes: list[IR]):
        self._nodes: tuple[IR, ...] = tuple(nodes)
        self._pos: int = 0

    # ── Basics ───────────────────────────────────────────────────────

    @property
    def pos(self) -> int:
        return self._pos

    def __len__(self) -> int:
        return len(self._nodes)

    def has_next(self) -> bool:
        return self._pos < len(self._nodes)

    def remaining(self) -> int:
        return len(self._nodes) - self._pos

    # ── Read ─────────────────────────────────────────────────────────

    def peek(self, offset: int = 0) -> IR | None:
        """Look ahead without consuming. offset=0 is current."""
        idx = self._pos + offset
        if 0 <= idx < len(self._nodes):
            return self._nodes[idx]
        return None

    def shift(self) -> IR | None:
        """Consume and return current node, advance position."""
        if self._pos >= len(self._nodes):
            return None
        node = self._nodes[self._pos]
        self._pos += 1
        return node

    def advance(self, n: int = 1) -> None:
        """Skip n nodes."""
        self._pos = min(self._pos + n, len(self._nodes))

    # ── Type matching ────────────────────────────────────────────────

    def check(self, *types: type[IR]) -> tuple[IR, ...] | None:
        """Peek ahead: do next N nodes match given types?
        Returns matched nodes as tuple if all match, None otherwise.
        Does NOT consume — call advance(len(types)) after."""
        end = self._pos + len(types)
        if end > len(self._nodes):
            return None
        matched = []
        for i, t in enumerate(types):
            node = self._nodes[self._pos + i]
            if not isinstance(node, t):
                return None
            matched.append(node)
        return tuple(matched)

    # ── Navigation ───────────────────────────────────────────────────

    def reset(self) -> None:
        """Reset cursor to start."""
        self._pos = 0

    def __iter__(self):
        """Iterate all nodes from current position."""
        return iter(self._nodes[self._pos:])
