#!/usr/bin/env python3
"""
Two local-model roles (Investigator / Validator) that check each other,
built on top of lora_agent's tool wiring, so a research or drafting task
gets a cheap independent second opinion before it reaches Claude at all.

Round structure:
  1. Investigator gets `task` with full lora + file tools, produces a
     concrete answer (must cite RVA/VA or file:line evidence).
  2. Validator gets ONLY that answer text (not the investigator's tool
     trace) plus its OWN fresh set of the same tools, and is told to
     independently re-derive/check every concrete claim rather than trust
     it, then reply with one of CONFIRMED / REFUTED / PARTIAL plus reasons.
  3. If not CONFIRMED, the validator's objections are handed back to a new
     Investigator round (fresh tool-call budget) to address, and we go
     again, up to --max-rounds times.

Final output is (last_answer, last_verdict, transcript_summary). Still spot
-check the final CONFIRMED answer yourself before acting on it for anything
that will inform a real code change - this catches the model fooling
itself twice in the same way, not genuine blind spots in its training.
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
import lora_agent as lag  # noqa: E402

INVESTIGATOR_SYSTEM_EXTRA = """

## Role: Investigator
Answer the task using real tool calls - disassembly, symbol lookup, source
grep - never guess. State your final answer with concrete RVA/VA and/or
file:line evidence for every claim. If a prior attempt's objections are
included in the task, address each one explicitly (confirm, revise, or
explain why it's wrong) rather than ignoring them.
"""

VALIDATOR_SYSTEM_EXTRA = """

## Role: Validator (adversarial)
You will be given another analyst's answer to a reverse-engineering/code
question. Do NOT trust it. Re-derive or spot-check every concrete claim
(RVA/VA, function signature, call site, file:line) using your OWN fresh
tool calls, as if you were trying to catch a mistake. Common failure modes
to check for: wrong RVA/VA, a claim about what a function does that isn't
backed by the disassembly shown, a call site that isn't actually reached
during normal execution, a claim about file:line content that doesn't
match the real file.

End your final answer with exactly one of these verdict lines:
VERDICT: CONFIRMED
VERDICT: REFUTED
VERDICT: PARTIAL
followed by your reasoning and, if not CONFIRMED, the specific objection(s)
the investigator needs to address.
"""


def _extract_verdict(text):
    for line in reversed((text or "").splitlines()):
        line = line.strip().upper()
        if line.startswith("VERDICT:"):
            v = line.split(":", 1)[1].strip()
            if v in ("CONFIRMED", "REFUTED", "PARTIAL"):
                return v
    return "UNCLEAR"


async def _build_impls_schemas(args):
    preload_note = ""
    if not args.no_preload:
        await lora.mcp.call_tool("load", {
            "exe_path": args.exe, "pdb_path": args.pdb, "alias": args.alias,
        })
        preload_note = (
            "\n\n## Already loaded\n'%s' + '%s' is already loaded as alias '%s' - "
            "do NOT call load() again. Just use find_functions/disasm_compressed/etc."
            % (args.exe, args.pdb, args.alias)
        )
    impls, schemas = await lag.build_lora_tools()
    file_impls = {name: functools.partial(fn, args.root) for name, fn in la.READ_ONLY_TOOLS.items()}
    file_schemas = [la.TOOL_SCHEMAS[n] for n in la.READ_ONLY_TOOLS]
    impls.update(file_impls)
    schemas = schemas + file_schemas
    return impls, schemas, preload_note


async def run_dual(args):
    impls, schemas, preload_note = await _build_impls_schemas(args)
    base_system = lora.SYSTEM_PROMPT + lag.SYSTEM_PROMPT_EXTRA + preload_note

    task = args.task
    history = []
    answer = None
    verdict = "UNCLEAR"

    for round_no in range(1, args.max_rounds + 1):
        print("=== round %d: INVESTIGATOR ===" % round_no, file=sys.stderr)
        inv_task = task if round_no == 1 else (
            "%s\n\n## Previous answer\n%s\n\n## Validator objections to address\n%s\n\n"
            "Revise your answer to address these objections. Re-check evidence yourself; "
            "don't just restate the old answer." % (task, answer, verdict_reasoning)
        )
        answer, _ = await la.run_agent(
            impls=impls, schemas=schemas,
            system_prompt=base_system + INVESTIGATOR_SYSTEM_EXTRA,
            task=inv_task, model=args.model, url=args.url,
            max_iters=args.max_iters, verbose=not args.quiet,
        )
        history.append(("investigator", round_no, answer))
        print(answer, file=sys.stderr)

        print("=== round %d: VALIDATOR ===" % round_no, file=sys.stderr)
        val_task = (
            "## Task the investigator was answering\n%s\n\n"
            "## Investigator's answer to verify\n%s" % (task, answer)
        )
        verdict_reasoning, _ = await la.run_agent(
            impls=impls, schemas=schemas,
            system_prompt=base_system + VALIDATOR_SYSTEM_EXTRA,
            task=val_task, model=args.model, url=args.url,
            max_iters=args.max_iters, verbose=not args.quiet,
        )
        history.append(("validator", round_no, verdict_reasoning))
        print(verdict_reasoning, file=sys.stderr)

        verdict = _extract_verdict(verdict_reasoning)
        print("--- verdict: %s ---" % verdict, file=sys.stderr)
        if verdict == "CONFIRMED":
            break

    return answer, verdict, history


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("task", help="Investigation task")
    ap.add_argument("--root", default=r"F:\Kraken")
    ap.add_argument("--exe", default=lag.DEFAULT_EXE)
    ap.add_argument("--pdb", default=lag.DEFAULT_PDB)
    ap.add_argument("--alias", default="hta")
    ap.add_argument("--no-preload", action="store_true")
    ap.add_argument("--model", default=la.DEFAULT_MODEL)
    ap.add_argument("--url", default=la.DEFAULT_URL)
    ap.add_argument("--max-iters", type=int, default=30, help="Per-agent-call tool-call budget")
    ap.add_argument("--max-rounds", type=int, default=3, help="Investigator<->Validator round budget")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    answer, verdict, _ = asyncio.run(run_dual(args))
    print("\n\n===== FINAL (%s) =====\n%s" % (verdict, answer))


if __name__ == "__main__":
    main()
