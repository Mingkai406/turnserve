# CPU inference smoke test

These are actual local model runs, not simulated latency data. Each policy ran once with the same scripted scenario. The purpose is integration validation, not policy ranking.

| Policy | Completed | Cancelled | Failed | Rejected |
|---|---:|---:|---:|---:|
| FIFO | 3 | 1 | 0 | 0 |
| Round-robin | 3 | 1 | 0 | 0 |
| Interleave | 3 | 1 | 0 | 0 |

Every run used the pinned SmolLM2-135M-Instruct Q4_K_M model, CPU inference with four threads, four sequence slots, a 2048-token per-session budget, 128-token batch budget, and 64-token prefill chunks. Sampling was greedy. Model loading is outside the recorded runtime interval.

The interleave run emitted `Here are ten` for the original question before it was replaced. The replacement produced `One planet in the solar system is called Mars.` The separate short question produced `The capital of France is Paris.` The document response repeated the source text and hit its output limit; it is retained exactly as generated, rather than edited into a better demonstration of the small model.

`*.jsonl` are the raw events; `*.summary.json` are derived with `scripts/inspect_trace.py`. That script verifies one terminal per submission, contiguous output-token counts, no token/commit after termination, and commits only on completion. C++ contract tests separately inspect session history to ensure cancelled turns are excluded.

The replacement is triggered after the third emitted token, so its arrival time changes with scheduling. This is a closed-loop interaction demo, not a fixed-arrival A/B study. There was no warmup/repetition protocol, controlled machine load, hardware inventory, peak-memory measurement, or upstream-server comparison. Do not use these three runs to claim a latency improvement or estimate a p95 distribution.

See [manifest.json](manifest.json) for source-file hashes, dependency/model pins, and environment details. The figure is regenerated directly from `interleave.jsonl`:

```sh
python3 scripts/inspect_trace.py results/smoke-cpu/interleave.jsonl
python3 scripts/render_figures.py
```
