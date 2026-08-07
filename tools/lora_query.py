#!/usr/bin/env python3
"""
Thin CLI driver for the `lora` MCP toolset (tools/lora), used without a live
MCP client connection - calls FastMCP.call_tool() directly in-process.

Usage: pass a JSON array of {"tool": name, "args": {...}} steps on argv[1],
executed in order in a single Python process (so a session loaded via the
"load" tool stays alive for later steps in the same invocation). Prints each
step's result as JSON, one per line.

argv[1] may instead be the PATH of a file containing that JSON - useful when the
shell's quoting rules make a long inline argument painful, which on Windows they
usually do. A path is told from JSON by looking at the first character: valid
step JSON always starts with '[', and no path does.

Use FORWARD SLASHES in exe_path/pdb_path. The backslash form has to survive both
the shell and the JSON decoder, and getting the doubling wrong yields a
JSONDecodeError rather than anything informative.

Example:
  python tools/lora_query.py '[
    {"tool": "load", "args": {"exe_path": "C:/Users/etozh/code/HTA_Kraken/hta.exe", "pdb_path": "C:/Users/etozh/code/HTA_Kraken/game.pdb"}},
    {"tool": "find_functions", "args": {"pattern": "*NearCallback*"}}
  ]'
"""
import asyncio
import json
import sys

sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

import lora  # noqa: E402


def _to_jsonable(result):
    if isinstance(result, (list, tuple)):
        out = []
        for item in result:
            text = getattr(item, "text", None)
            out.append(text if text is not None else str(item))
        return out
    return result


async def run_steps(steps):
    for step in steps:
        name = step["tool"]
        args = step.get("args", {})
        try:
            result = await lora.mcp.call_tool(name, args)
            print(json.dumps({"tool": name, "ok": True, "result": _to_jsonable(result)}, ensure_ascii=False))
        except Exception as e:
            print(json.dumps({"tool": name, "ok": False, "error": str(e)}, ensure_ascii=False))


def main():
    arg = sys.argv[1]
    if arg.lstrip()[:1] == "[":
        steps = json.loads(arg)
    else:
        with open(arg, "r", encoding="utf-8") as f:
            steps = json.load(f)
    asyncio.run(run_steps(steps))


if __name__ == "__main__":
    main()
