"""LoRA — Lossless Reverse-engineering Assistant.

Unified MCP server for PE binary analysis, PDB symbol inspection,
annotated disassembly, class reconstruction, and coverage tracking.

Combines capabilities previously split across asm-inspector, pdb-inspector,
re-inspector, and lora into a single coherent toolset.
"""

from __future__ import annotations

from mcp.server.fastmcp import FastMCP

from ._state import manager

SYSTEM_PROMPT = """\
You have access to **LoRA** (Lossless Reverse-engineering Assistant), an MCP \
toolset for analyzing Win32 PE executables with PDB debug symbols. LoRA is \
designed for reconstructing C++ source code from compiled binaries.

## Quick Start
1. `load(exe_path, pdb_path)` — load a target (PE + PDB pair)
2. `class_overview(name)` — get full class layout, fields, methods, vtable
3. `disasm_compressed(name)` — token-minimal semantic stream for reconstruction
4. `batch_disasm_detailed(rvas)` — batch source-line-grouped disassembly

## Tool Categories

### Loading & Session
- `load` — Load PE + PDB. Supports multiple targets via `alias` param.

### Class Analysis
- `class_overview` — Complete class: fields, bases, methods with prototypes
- `pure_virtual_interface` — Extract DLL interface signatures (IRenderer, etc.)
- `vftable` — Dump full virtual function table at RVA (cached, class-boundary-aware)
- `vtable_method_at` — **Single vtable slot from PE bytes** (accurate, not PDB order). \
Pass vtable RVA + offset from `call [edx + 0x3c]`
- `vft_method` — Resolve vtable slot by offset via PDB (may be inaccurate, prefer vtable_method_at)
- `deps` — All type dependencies with source file paths
- `prototype` / `prototype_at_rva` / `prototypes` — Function signatures

### Disassembly (5 detail levels, low → high density)
- `disasm_raw` — Raw Capstone (PE-only, no PDB). Use for non-PDB targets or raw byte inspection.
- `disasm` — Capstone + symbol/string/import annotation. Quick look with resolved names.
- `disasm_typed` — **Enriched IR**: every instruction annotated with PDB types, field names, \
resolved vtable calls, string literals, stack locals. Full decode→enrich pipeline.
- `disasm_detailed` — Source-line-grouped with prototype, locals, calls, strings. \
Structured JSON output for human-readable analysis. Supports pagination.
- `disasm_compressed` — **Token-minimal semantic stream for LLM reconstruction**. \
Drops register mechanics, emits only semantic actions (calls, field writes, control flow). \
Includes jumpmap (control flow edges) and inlinee candidates (hints). \
~3-5x fewer tokens than raw disasm. **Prefer this for function reconstruction.**
- `batch_disasm_detailed` — Batch version for whole-class reconstruction.

### Enrichment Cache
- `rebuild_cache` — Pre-scan all PDB functions through decode→enrich pipeline. \
SQLite-backed, incremental. Pass `purge=True` to rebuild from scratch. \
Cache enables inlinee fingerprinting in `disasm_compressed`.

### Symbol Resolution
- `resolve` — Batch resolve RVAs to symbol names (cached)
- `demangle` — Batch demangle MSVC decorated names
- `globals_by_pattern` — Find global/static data by pattern
- `symbol_at_rva` / `batch_symbol_at_rva` — Single/batch RVA lookup
- `missing_symbols` — Map linker errors to PDB info

### PDB Queries
- `find_functions` / `find_types` / `find_enums` / `find_data` / \
`find_typedefs` / `find_public_symbols` — Symbol search with DIA wildcards
- `get_type_layout` — Struct/class layout. **Supports pagination**: \
`offset_start/offset_end` for byte-offset range, `field_offset/field_limit` for paging
- `fields_at_offsets` — **Targeted field lookup** by byte-offsets (e.g. from disasm). \
Walks base classes. Pass `['0x304', '0x8b240']` to decode `[esi + offset]` patterns
- `get_enum_values` — All enum member values
- `pdb_function_detail` — PDB-level function info (params, locals, types)
- `get_source_files` / `find_lines_by_rva` — Source file and line queries
- `section_contributions` — Code size per compiland
- `compiland_source_files` — Source files referenced by a compiland

### Compiland Analysis
- `compiland_dump` — Full compiland contents (functions, data, typedefs)
- `list_compilands` / `list_compilands_summary` — Browse compilation units
- `compiland_includes` — Source files for #include generation
- `inline_functions` — Detect header-inlined vs out-of-line functions

### Cross-Reference & Analysis
- `xrefs_to` — Find all callers of a function across .text
- `calls_in_function` — List all call targets from a function
- `strings_in_function` — Extract all string references
- `calling_info` — Detect __usercall patterns (this@<edx>, etc.)
- `scan_usercalls` — Bulk scan for non-standard calling conventions
- `template_recover` — Reconstruct template definitions from instantiations

### PE Inspection
- `pe_sections` / `pe_imports` / `pe_exports` — PE structure
- `read_value` — **Auto-decode data at RVA**: returns float, int, string, pointer→symbol in one call
- `batch_read_value` — Same as read_value but for multiple RVAs at once
- `read_bytes` / `read_string` / `read_wide_string` — Raw memory access
- `resolve_import` / `resolve_imports_batch` — IAT resolution
- `find_ret` — Scan from RVA to find `ret N`, useful for calling convention checks

### Coverage
- `coverage_report` — Compare reconstructed source vs PDB compilands
- `source_map` — Map type names to original source file paths

## Typical Workflow
```
load("target/hta.exe", "target/game.pdb")
list_compilands_summary("FileServer")
class_overview("m3d::fs::FileServer")
disasm_compressed("m3d::fs::FileServer::Open")  # token-efficient reconstruction
# For batch work:
batch_disasm_detailed(["0x1234", "0x5678", ...])
coverage_report("source")
```

## Choosing a Disassembly Tool
- **Reconstructing a function?** → `disasm_compressed` (smallest token cost, semantic stream)
- **Need exact instruction details?** → `disasm_typed` (every node with type annotations)
- **Batch class reconstruction?** → `batch_disasm_detailed` (source-line groups + locals)
- **Quick symbol check?** → `disasm` (fast, annotated)
- **No PDB available?** → `disasm_raw` (raw Capstone)

## disasm_compressed Output Format
```
## jumpmap           — control flow: src→dst mn [back]
## inlinee           — candidate inline regions (begin-end size target owner)
## stream            — semantic actions grouped by @source_line
  @1498 CStr::CStr("version string")
  @1500 m3d::Log::logTex(&v-224, 18)
  @1502 je eax,eax →0x1a8f2c
```
The jumpmap gives control flow structure. Inlinee candidates are hints — the LLM \
decides whether to collapse them. The stream drops register transport and stack \
bookkeeping, preserving only calls, field writes, and control flow with resolved names.

## Notes
- All tools accept optional `alias` param for multi-target sessions
- Symbol caches are per-session and persist across calls
- RVA values are always relative (not VA) unless noted otherwise
- DIA wildcard patterns (* ?) work in all find_* and pattern tools
- `vtable_method_at` reads PE bytes (accurate); `vft_method` uses PDB declaration order (may differ)
- `disasm_compressed` requires TypeDB (auto-created from PDB on first use, ~0.5s preheat)
"""

mcp = FastMCP(
    "lora",
    instructions=SYSTEM_PROMPT,
)


# Register the unified load tool on the mcp instance
@mcp.tool()
def load(exe_path: str, pdb_path: str, alias: str | None = None) -> str:
    """Load PE + PDB pair for analysis. Supports multiple targets via alias.
    Builds IAT map, exec ranges, and initializes caches."""
    sess = manager.load(exe_path, pdb_path, alias)
    arch = "x64" if sess.is_64 else "x86"
    return (
        f"Loaded {sess.exe_path} + {sess.pdb_path} as '{sess.alias}' "
        f"({arch}, base={hex(sess.image_base)})"
    )


# Import and register all tool modules
from . import tools_pe
from . import tools_pdb
from . import tools_resolve
from . import tools_disasm
from . import tools_class
from . import tools_compiland
from . import tools_analysis
from . import tools_coverage
from . import persifal

tools_pe.register(mcp)
tools_pdb.register(mcp)
tools_resolve.register(mcp)
tools_disasm.register(mcp)
tools_class.register(mcp)
tools_compiland.register(mcp)
tools_analysis.register(mcp)
tools_coverage.register(mcp)
persifal.register(mcp)
