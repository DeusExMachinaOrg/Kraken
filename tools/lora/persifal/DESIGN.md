# Persifal v2 — Design

## Pipeline

```
decode → fold_chains → fold_flow → fold_inline → emit_text
```

## Files

```
_ir.py      — Mn enum, IROp (enriched operand), classified IR node types
_stream.py  — frozen read-only cursor over IR list
_vm.py      — Reg, CPUx86, Stack, Scope, Machine
_decode.py  — PE bytes → IRStream (Capstone + static PDB per instruction)
_fold_*.py  — read stream + Machine → emit new stream
_emit.py    — final stream → C text
```

## Key Principles

1. **IR is enriched asm** — one node per instruction, operands carry
   what PDB says statically (symbol, string, source line). No folding.

2. **Stream is frozen** — once emitted, never mutated. Each fold stage
   reads a stream and emits a new list of higher-level nodes.

3. **Machine eats IR, doesn't decide** — fold feeds nodes to Machine
   AFTER making its own decisions. Machine tracks CPU/scope state.
   It never drives fold logic.

4. **Fold loop order**:
   - Check patterns on stream (peek ahead)
   - If match → fold chain, emit high-level node
   - If first stack slot access → check `known` (PDB locals), emit decl
   - If no match → emit raw slot-based node from IR chain
   - Feed node to Machine (update state)

5. **`known` is outside Machine** — PDB-declared stack layout (`CStr item`
   at offset -0x14). Fold stages query it, Machine doesn't trust it.

6. **Everything emits** — pattern match → named node. No match → slot node.
   Output is always complete, never "unknown" or "TODO".

7. **LR(n) matching** — `stream.check(IRPush, IRCall, IRArith)` tries
   pattern. Match → consume + emit reduced node. No match → advance.

8. **Classified nodes** — fold stages `match node:` on type, not string.
   `IRMov`, `IRPush`, `IRCall`, etc.

9. **Lexical tag** — `IR.lexical` is the PDB source line number (-1 if absent).
   Not structure, just a tag. Consecutive nodes with same lexical tag belong
   to the same source expression — fold can consume the run as one chain.

10. **RVA-addressed** — fold operates via RVA. Jumps target RVAs.
    Stream is flat: `rva: IR, rva+n: IR, ...`. No grouping.

11. **Slot type resolution** — first access determines type:
    - First access is a call (lea ecx + call) → type from call signature
    - First access is a mov (store into slot) → check `known` (PDB locals)
    - Neither → raw `slot_0x20`

12. **Jump map** — pre-fold scan of IRJcc/IRJmp builds edge list,
    landing sites, back-edges (loop headers). Fold_flow consumes this.
