#!/usr/bin/env python3
"""Open-loop replay: include failures in the denominator, summarize runs separately."""

import argparse
import hashlib
import json
import math
import platform
import statistics
import subprocess
from pathlib import Path


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * q) - 1)]


def summarize(events, deadline_ms=1000):
    offered = [e for e in events if e["type"] == "offered"]
    if not offered or len({e["session"] for e in offered}) != len(offered):
        raise ValueError("Expected uniquely named offered sessions")
    rows = []
    for request in offered:
        stream = [e for e in events if e["session"] == request["session"]]
        dispatched = [e for e in stream if e["type"] == "dispatched"]
        terminal = [
            e
            for e in stream
            if e["type"]
            in {"completed", "rejected", "queue_rejected", "failed", "cancelled"}
        ]
        if len(dispatched) != 1 or len(terminal) != 1:
            raise ValueError("Missing or duplicate dispatch/terminal event")
        first = next((e for e in stream if e["type"] == "token"), None)
        rows.append(
            {
                "session": request["session"],
                "status": terminal[0]["type"],
                "dispatch_lag_ms": dispatched[0]["ms"] - request["ms"],
                "ttft_ms": first["ms"] - request["ms"] if first else None,
                "latency_ms": terminal[0]["ms"] - request["ms"],
            }
        )
    successful = [r for r in rows if r["status"] == "completed"]
    return {
        "offered": len(rows),
        "completed": len(successful),
        "outcomes": {
            s: sum(r["status"] == s for r in rows)
            for s in sorted({r["status"] for r in rows})
        },
        "completion_rate": len(successful) / len(rows),
        "deadline_ms": deadline_ms,
        "deadline_success_rate": sum(r["latency_ms"] <= deadline_ms for r in successful)
        / len(rows),
        "completed_latency_p95_ms": percentile(
            [r["latency_ms"] for r in successful], 0.95
        ),
        "completed_ttft_p95_ms": percentile(
            [r["ttft_ms"] for r in successful if r["ttft_ms"] is not None], 0.95
        ),
        "dispatch_lag_p95_ms": percentile([r["dispatch_lag_ms"] for r in rows], 0.95),
        "requests": rows,
    }


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--binary", default="build/turnserve-load")
    p.add_argument("--model")
    p.add_argument("--workload", default="workloads/mixed.tsv")
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--threads", type=int, default=2)
    p.add_argument("--deadline-ms", type=float, default=1000)
    p.add_argument("--out", type=Path, default=Path("runs/load"))
    a = p.parse_args()
    if a.repeats < 1 or not math.isfinite(a.deadline_ms) or a.deadline_ms <= 0:
        p.error("positive repeats and deadline required")
    a.out.mkdir(parents=True, exist_ok=False)
    policies = ["fifo", "round-robin", "interleave"]
    runs = []
    for repeat in range(a.repeats):
        for policy in policies[repeat % 3 :] + policies[: repeat % 3]:
            trace = a.out / f"{policy}-{repeat}.jsonl"
            command = [
                a.binary,
                "--workload",
                a.workload,
                "--trace",
                str(trace),
                "--policy",
                policy,
                "--threads",
                str(a.threads),
            ]
            if a.model:
                command += ["--model", a.model]
            subprocess.run(command, check=True, timeout=200)
            events = [json.loads(s) for s in trace.read_text().splitlines()]
            runs.append(
                {"policy": policy, "repeat": repeat, **summarize(events, a.deadline_ms)}
            )
    report = {
        "measurement": "CPU model inference"
        if a.model
        else "Synthetic contract control; not model performance",
        "environment": platform.platform(),
        "threads": a.threads,
        "workload_sha256": sha(a.workload),
        "binary_sha256": sha(a.binary),
        "model_sha256": sha(a.model) if a.model else None,
        "timing": "From planned arrival; model loading excluded; first inference cold; fresh process per run",
        "aggregation": "Per-run metrics; medians across repeats, no pooled request confidence intervals",
        "runs": runs,
        "policies": {
            policy: {
                key: statistics.median(
                    [
                        r[key]
                        for r in runs
                        if r["policy"] == policy and r[key] is not None
                    ]
                )
                if any(r["policy"] == policy and r[key] is not None for r in runs)
                else None
                for key in (
                    "completion_rate",
                    "deadline_success_rate",
                    "completed_latency_p95_ms",
                    "completed_ttft_p95_ms",
                    "dispatch_lag_p95_ms",
                )
            }
            for policy in policies
        },
    }
    (a.out / "summary.json").write_text(
        json.dumps(report, indent=2, allow_nan=False) + "\n"
    )
    print(json.dumps(report["policies"], indent=2))


if __name__ == "__main__":
    main()
