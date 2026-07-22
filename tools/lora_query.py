#!/usr/bin/env python3
"""
Thin CLI driver for the `lora` MCP toolset (tools/lora), used without a live
MCP client connection - calls FastMCP.call_tool() directly in-process.

Usage: pass a JSON array of {"tool": name, "args": {...}} steps on argv[1],
executed in order in a single Python process (so a session loaded via the
"load" tool stays alive for later steps in the same invocation). Prints each
step's result as JSON, one per line.

Example:
  python tools/lora_query.py '[
    {"tool": "load", "args": {"exe_path": "F:\\\\HTA_Kraken\\\\hta.exe", "pdb_path": "F:\\\\HTA_Kraken\\\\game.pdb"}},
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
    steps = json.loads(sys.argv[1])
    asyncio.run(run_steps(steps))


if __name__ == "__main__":
    main()
