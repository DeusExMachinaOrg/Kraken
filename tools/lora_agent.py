#!/usr/bin/env python3
"""
Local qwen3-coder agent wired up to the `lora` MCP reverse-engineering toolset
(tools/lora: PE/PDB disassembly, symbol resolution, class reconstruction, ...)
plus basic file read/search over the mod's own source tree. Lets the local
model drive the binary-analysis tool-call sequence itself - locate a function,
disassemble it, cross-check it against the mod's C++ hooks - instead of Claude
hand-driving lora_query.py call by call. Reserve Claude's own tokens for
judging/verifying the model's conclusions, not for the mechanical tool-call
sequencing to reach them.

Usage:
    python tools/lora_agent.py [--root F:\\Kraken] [--exe PATH] [--pdb PATH]
                                [--alias hta] [--allow-write] [--model NAME]
                                [--url URL] [--max-iters N] "<investigation task>"

--exe/--pdb default to F:\\HTA_Kraken\\hta.exe / game.pdb and get auto-loaded
as session alias 'hta' before the task starts (skip with --no-preload), so
the model can go straight to find_functions/disasm_compressed/xrefs_to/etc.

Always spot-check its conclusions against a real disasm_detailed/grep before
trusting them for anything that will inform an actual code change - it can
misread a jump condition or a register alias same as any other reverse
engineer, human or not.
"""
import argparse
import asyncio
import functools
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lora  # noqa: E402
import local_agent as la  # noqa: E402

DEFAULT_EXE = r"F:\HTA_Kraken\hta.exe"
DEFAULT_PDB = r"F:\HTA_Kraken\game.pdb"

SYSTEM_PROMPT_EXTRA = """

## Additional file tools (over the mod's own C++ source, not the game binary)
- read_file(path, offset, limit) - read a text source file
- list_dir(path) - list a directory
- search_text(pattern, path, glob) - regex search across source files

Use these to cross-check the mod's own hook code (source/fix/*.cpp) against
what the disassembly shows - e.g. whether an existing function already calls
something you found in the binary, before concluding it's missing.

## Ground rules
- Always resolve names via find_functions/find_types before guessing an RVA.
- Prefer disasm_compressed for a first read of a function, disasm_detailed
  when you need exact instruction-level confirmation of a specific claim
  (e.g. which branch a jump takes, what a comparison actually checks).
- State your conclusion plainly with concrete RVA/VA and file:line references.
  If the evidence is ambiguous or incomplete, say so explicitly rather than
  guessing - a wrong confident answer here is worse than an honest "unclear".
"""


def _format_mcp_result(result):
    if isinstance(result, dict):
        return str(result)
    out = []
    for item in result:
        text = getattr(item, "text", None)
        out.append(text if text is not None else str(item))
    return "\n".join(out)


def make_lora_impl(tool_name):
    async def _impl(**kwargs):
        result = await lora.mcp.call_tool(tool_name, kwargs)
        return _format_mcp_result(result)
    return _impl


async def build_lora_tools():
    tools = await lora.mcp.list_tools()
    impls = {}
    schemas = []
    for t in tools:
        impls[t.name] = make_lora_impl(t.name)
        schemas.append({
            "type": "function",
            "function": {
                "name": t.name,
                "description": (t.description or "").strip(),
                "parameters": t.inputSchema,
            },
        })
    return impls, schemas


async def run(args):
    preload_note = ""
    if not args.no_preload:
        await lora.mcp.call_tool("load", {
            "exe_path": args.exe,
            "pdb_path": args.pdb,
            "alias": args.alias,
        })
        preload_note = (
            "\n\n## Already loaded\n"
            "'%s' + '%s' is already loaded as alias '%s' - do NOT call load() again "
            "unless you specifically need a different exe/pdb pair. Just start using "
            "find_functions/class_overview/disasm_compressed/etc. with alias='%s'."
            % (args.exe, args.pdb, args.alias, args.alias)
        )

    impls, schemas = await build_lora_tools()

    file_impls = {name: functools.partial(fn, args.root) for name, fn in la.READ_ONLY_TOOLS.items()}
    file_schemas = [la.TOOL_SCHEMAS[n] for n in la.READ_ONLY_TOOLS]
    if args.allow_write:
        file_impls.update({name: functools.partial(fn, args.root) for name, fn in la.WRITE_TOOLS.items()})
        file_schemas += [la.TOOL_SCHEMAS[n] for n in la.WRITE_TOOLS]

    impls.update(file_impls)
    schemas += file_schemas

    system_prompt = lora.SYSTEM_PROMPT + SYSTEM_PROMPT_EXTRA + preload_note
    answer, _ = await la.run_agent(
        impls=impls,
        schemas=schemas,
        system_prompt=system_prompt,
        task=args.task,
        model=args.model,
        url=args.url,
        max_iters=args.max_iters,
        verbose=not args.quiet,
    )
    print(answer)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("task", help="Investigation task for the local agent")
    ap.add_argument("--root", default=r"F:\Kraken", help="Mod source root for the file tools")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--pdb", default=DEFAULT_PDB)
    ap.add_argument("--alias", default="hta")
    ap.add_argument("--no-preload", action="store_true", help="Skip auto-loading --exe/--pdb before the task")
    ap.add_argument("--allow-write", action="store_true", help="Also grant write_file/edit_file over --root")
    ap.add_argument("--model", default=la.DEFAULT_MODEL)
    ap.add_argument("--url", default=la.DEFAULT_URL)
    ap.add_argument("--max-iters", type=int, default=30)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    asyncio.run(run(args))


if __name__ == "__main__":
    main()
