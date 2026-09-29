#!/usr/bin/env python3
"""Check lifecycle invariants and summarize one trace. No statistical performance claims."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

TERMINAL = {"completed", "cancelled", "failed", "rejected"}

def inspect(path):
    events = [json.loads(line) for line in Path(path).read_text().splitlines() if line]
    by_id = defaultdict(list)
    for event in events:
        if event["request"]:
            by_id[event["request"]].append(event)
    summary = []
    for request, items in sorted(by_id.items()):
        submits = [e for e in items if e["type"] == "submitted"]
        if not submits:
            continue  # A cancel command may refer to an unknown request.
        terminal = [e for e in items if e["type"] in TERMINAL]
        if len(terminal) != 1:
            raise ValueError(f"request {request}: expected exactly one terminal event")
        end = terminal[0]
        end_index = items.index(end)
        if any(e["type"] in {"token", "committed"} for e in items[end_index+1:]):
            raise ValueError(f"request {request}: token or commit after terminal")
        commits = [e for e in items if e["type"] == "committed"]
        if len(commits) != (1 if end["type"] == "completed" else 0):
            raise ValueError(f"request {request}: incorrect commit count")
        tokens = [e for e in items if e["type"] == "token"]
        if [e["count"] for e in tokens] != list(range(1, len(tokens)+1)):
            raise ValueError(f"request {request}: token sequence has gaps or duplicates")
        if len(tokens) != end["count"]:
            raise ValueError(f"request {request}: terminal output count mismatch")
        accepted = [e for e in items if e["type"] == "accepted"]
        output = b"".join(e["text"].encode("latin-1") for e in tokens).decode("utf-8", errors="replace")
        summary.append({
            "request": request, "session": submits[0]["session"], "status": end["type"],
            "input_tokens": accepted[0]["count"] if accepted else 0,
            "output_tokens": len(tokens),
            "ttft_ms": round(tokens[0]["ms"]-submits[0]["ms"], 3) if tokens else None,
            "duration_ms": round(end["ms"]-submits[0]["ms"], 3), "output": output,
        })
    return {"source": Path(path).name, "outcomes": dict(Counter(r["status"] for r in summary)), "requests": summary}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    args = parser.parse_args()
    print(json.dumps(inspect(args.trace), indent=2, ensure_ascii=False))
