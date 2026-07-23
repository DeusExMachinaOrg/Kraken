#!/usr/bin/env python3
"""
Delegate a well-scoped subtask to a locally-hosted Ollama model (qwen3-coder),
giving it read (and optionally write) access to a single project directory
via tool-calling, so Claude doesn't have to spend its own tokens reading and
generating the mechanical parts of the task itself.

Usage:
    python tools/local_agent.py [--root PATH] [--allow-write] [--model NAME]
                                 [--url URL] [--max-iters N] "<task description>"

Only use this for bounded, easily-verifiable tasks (mechanical search/summarize,
boilerplate generation, doc formatting). Always review its output/diffs before
trusting them for anything involving actual logic correctness.
"""
import argparse
import asyncio
import functools
import json
import os
import re
import sys
import urllib.error
import urllib.request

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")

DEFAULT_URL = "http://192.168.2.118:11434/api/chat"
DEFAULT_MODEL = "qwen3-coder:30b-a3b-q4_K_M"
MAX_TOOL_RESULT_CHARS = 20000


def resolve_path(root, rel):
    root_abs = os.path.abspath(root)
    p_abs = os.path.abspath(os.path.join(root_abs, rel))
    if p_abs != root_abs and not p_abs.startswith(root_abs + os.sep):
        raise ValueError("path escapes project root: %r" % rel)
    return p_abs


def tool_read_file(root, path, offset=1, limit=2000, **_):
    p = resolve_path(root, path)
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    start = max(1, int(offset)) - 1
    end = start + int(limit)
    chunk = lines[start:end]
    return "".join("%6d\t%s" % (start + i + 1, line) for i, line in enumerate(chunk))


def tool_list_dir(root, path=".", **_):
    p = resolve_path(root, path)
    entries = sorted(os.listdir(p))
    out = []
    for e in entries:
        full = os.path.join(p, e)
        out.append(("d " if os.path.isdir(full) else "f ") + e)
    return "\n".join(out)


def tool_search_text(root, pattern, path=".", glob=None, max_matches=200, **_):
    p = resolve_path(root, path)
    rx = re.compile(pattern)
    glob_rx = None
    if glob:
        glob_rx = re.compile(re.escape(glob).replace(r"\*", ".*").replace(r"\?", ".") + "$")
    results = []
    for dirpath, dirnames, filenames in os.walk(p):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "node_modules")]
        for fn in filenames:
            if glob_rx and not glob_rx.match(fn):
                continue
            full = os.path.join(dirpath, fn)
            try:
                with open(full, "r", encoding="utf-8", errors="replace") as f:
                    for i, line in enumerate(f, 1):
                        if rx.search(line):
                            rel = os.path.relpath(full, root)
                            results.append("%s:%d:%s" % (rel, i, line.rstrip("\n")))
                            if len(results) >= max_matches:
                                raise StopIteration
            except (UnicodeDecodeError, OSError):
                continue
    if not results:
        return "(no matches)"
    return "\n".join(results)


def tool_write_file(root, path, content, **_):
    p = resolve_path(root, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(content)
    return "wrote %d bytes to %s" % (len(content.encode("utf-8")), path)


def tool_edit_file(root, path, old_string, new_string, **_):
    p = resolve_path(root, path)
    with open(p, "r", encoding="utf-8") as f:
        text = f.read()
    count = text.count(old_string)
    if count == 0:
        return "ERROR: old_string not found in %s" % path
    if count > 1:
        return "ERROR: old_string is not unique in %s (%d occurrences)" % (path, count)
    text = text.replace(old_string, new_string, 1)
    with open(p, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return "edited %s" % path


READ_ONLY_TOOLS = {
    "read_file": tool_read_file,
    "list_dir": tool_list_dir,
    "search_text": tool_search_text,
}
WRITE_TOOLS = {
    "write_file": tool_write_file,
    "edit_file": tool_edit_file,
}

TOOL_SCHEMAS = {
    "read_file": {
        "type": "function",
        "function": {
            "name": "read_file",
            "description": "Read a text file's contents (with 1-indexed line numbers).",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Path relative to project root"},
                    "offset": {"type": "integer", "description": "1-indexed starting line, default 1"},
                    "limit": {"type": "integer", "description": "Max lines to read, default 2000"},
                },
                "required": ["path"],
            },
        },
    },
    "list_dir": {
        "type": "function",
        "function": {
            "name": "list_dir",
            "description": "List files ('f ' prefix) and subdirectories ('d ' prefix) in a directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Directory path relative to root, '.' for root"},
                },
                "required": [],
            },
        },
    },
    "search_text": {
        "type": "function",
        "function": {
            "name": "search_text",
            "description": "Regex search across text files under a directory. Returns file:line:text per match.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pattern": {"type": "string", "description": "Python regex"},
                    "path": {"type": "string", "description": "Directory to search under, relative to root, default '.'"},
                    "glob": {"type": "string", "description": "Optional filename glob, e.g. '*.cpp'"},
                },
                "required": ["pattern"],
            },
        },
    },
    "write_file": {
        "type": "function",
        "function": {
            "name": "write_file",
            "description": "Create or overwrite a text file with given content.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "content": {"type": "string"},
                },
                "required": ["path", "content"],
            },
        },
    },
    "edit_file": {
        "type": "function",
        "function": {
            "name": "edit_file",
            "description": "Replace one exact, unique occurrence of old_string with new_string in a file.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "old_string": {"type": "string"},
                    "new_string": {"type": "string"},
                },
                "required": ["path", "old_string", "new_string"],
            },
        },
    },
}

FUNCTION_CALL_RE = re.compile(
    r"<function=(?P<name>[a-zA-Z_][a-zA-Z0-9_]*)>(?P<body>.*?)</function>",
    re.DOTALL,
)
PARAM_RE = re.compile(
    r"<parameter=(?P<key>[a-zA-Z_][a-zA-Z0-9_]*)>\s*(?P<value>.*?)\s*</parameter>",
    re.DOTALL,
)


def parse_fallback_tool_calls(content):
    """This model/Ollama build emits tool calls as inline pseudo-XML in
    `content` instead of populating the structured `tool_calls` field, e.g.
    <function=search_text><parameter=pattern>foo</parameter></function>.
    Parse that format as a fallback so the agent loop still works."""
    calls = []
    for m in FUNCTION_CALL_RE.finditer(content or ""):
        args = {pm.group("key"): pm.group("value") for pm in PARAM_RE.finditer(m.group("body"))}
        calls.append({"function": {"name": m.group("name"), "arguments": args}})
    return calls


SYSTEM_PROMPT = (
    "You are a careful local coding assistant with tool access to a single project "
    "directory. Use tools to gather all context you need before answering - never "
    "assume file contents, read them. Prefer search_text to locate things before "
    "read_file on a specific file. When you have enough information, reply with a "
    "final plain-text answer and do NOT call any more tools. Be concise and concrete "
    "in your final answer; include file:line references where relevant."
)


def call_ollama(url, model, messages, tools, timeout=480):
    body = json.dumps({
        "model": model,
        "messages": messages,
        "tools": tools,
        "stream": False,
    }).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.URLError as e:
        raise RuntimeError("Ollama request failed: %s" % e)


async def run_agent(impls, schemas, system_prompt, task, model, url, max_iters, verbose):
    """Generic tool-calling loop against Ollama. `impls` maps tool name -> callable
    taking **kwargs and returning a result (str or awaitable of str); `schemas` is
    the matching list of Ollama tool-schema dicts. Reusable across different tool
    sets (plain file tools here, or e.g. lora's MCP tools in lora_agent.py)."""
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": task},
    ]

    for i in range(max_iters):
        resp = call_ollama(url, model, messages, schemas)
        msg = resp.get("message", {})
        messages.append(msg)
        tool_calls = msg.get("tool_calls")
        if not tool_calls:
            tool_calls = parse_fallback_tool_calls(msg.get("content", ""))
        if not tool_calls:
            return msg.get("content", ""), messages

        if verbose:
            print("--- iteration %d: %d tool call(s) ---" % (i + 1, len(tool_calls)), file=sys.stderr)

        for tc in tool_calls:
            fn = tc.get("function", {})
            name = fn.get("name")
            args = fn.get("arguments", {})
            if isinstance(args, str):
                try:
                    args = json.loads(args)
                except json.JSONDecodeError:
                    args = {}
            if verbose:
                print("  %s(%s)" % (name, json.dumps(args)[:200]), file=sys.stderr)
            impl = impls.get(name)
            if impl is None:
                result = "ERROR: unknown or disallowed tool %r" % name
            else:
                try:
                    result = impl(**args)
                    if asyncio.iscoroutine(result):
                        result = await result
                except Exception as e:
                    result = "ERROR: %s" % e
            result = str(result)
            if len(result) > MAX_TOOL_RESULT_CHARS:
                result = result[:MAX_TOOL_RESULT_CHARS] + "\n...(truncated)"
            messages.append({"role": "tool", "name": name, "content": result})

    return "ERROR: exceeded max iterations (%d) without a final answer" % max_iters, messages


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("task", help="Task description for the local agent")
    ap.add_argument("--root", default=os.getcwd(), help="Project root the agent may read/write within")
    ap.add_argument("--allow-write", action="store_true", help="Grant write_file/edit_file tools")
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--max-iters", type=int, default=15)
    ap.add_argument("--quiet", action="store_true", help="Suppress progress lines on stderr")
    args = ap.parse_args()

    impls = {name: functools.partial(fn, args.root) for name, fn in READ_ONLY_TOOLS.items()}
    schemas = [TOOL_SCHEMAS[n] for n in READ_ONLY_TOOLS]
    if args.allow_write:
        impls.update({name: functools.partial(fn, args.root) for name, fn in WRITE_TOOLS.items()})
        schemas += [TOOL_SCHEMAS[n] for n in WRITE_TOOLS]

    answer, _ = asyncio.run(run_agent(
        impls=impls,
        schemas=schemas,
        system_prompt=SYSTEM_PROMPT,
        task=args.task,
        model=args.model,
        url=args.url,
        max_iters=args.max_iters,
        verbose=not args.quiet,
    ))
    print(answer)


if __name__ == "__main__":
    main()
