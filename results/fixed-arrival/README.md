# Fixed-arrival CPU reference

A dedicated producer submits the same 12 mixed-length prompts at fixed 30 ms intervals.
It does not wait for model completions. Every offered request receives an outcome, including
command-queue rejection, admission rejection, failure and cancellation. Timing starts at
**planned arrival**, so producer dispatch lag is visible instead of disappearing from latency.

Recorded 2026-10-08, SmolLM2-135M-Instruct Q4_K_M, pinned llama.cpp CPU backend, 2 threads,
4 slots, 2,048 context tokens per slot, 128-token batch budget and 64-token prefill chunks.
Environment: macOS-26.4.1-arm64-arm-64bit-Mach-O. Policies rotate order across three repetitions;
each run uses a fresh process. Model loading is excluded; first inference is cold.
No other project benchmark ran concurrently during these reference measurements.

| Policy | Completion | Completed within 1 s | p95 TTFT ms | p95 completion ms | p95 dispatch lag ms |
|---|---:|---:|---:|---:|---:|
| fifo | 100% | 41.7% | 1981.0 | 2006.7 | 1.46 |
| round-robin | 100% | 33.3% | 2238.9 | 2495.1 | 1.28 |
| interleave | 100% | 33.3% | 2265.6 | 2526.0 | 1.41 |

Values are medians of three per-run metrics, not pooled requests or confidence intervals.
There are only 12 requests per run: nearest-rank p95 is consequently the maximum in this
small workload. The 1 s deadline is an experiment parameter, not an established service SLO.
All three policies completed all requests; this load does not establish saturation capacity.

**FIFO performs better on this particular small CPU workload.** This is not evidence of a
universal policy ranking. Long/short prompt composition, slot occupancy, backend batching and
hardware all matter. The result motivates measurements over a broader load matrix before
claiming an interleaving speedup. No comparison against upstream llama-server is claimed.

## Reproduce

```sh
python3 scripts/fetch_model.py
cmake -S . -B build-llama -DTURNSERVE_LLAMA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-llama --parallel 2
python3 scripts/load_benchmark.py --binary build-llama/turnserve-load \
  --model models/SmolLM2-135M-Instruct-Q4_K_M.gguf \
  --workload workloads/mixed.tsv --threads 2 --repeats 3 --out runs/fixed-arrival
```

`summary.json` fingerprints the workload, model and binary and retains per-request outcomes
and per-run metrics. Adjacent JSONL files contain the actual events. The binary hash identifies
the recorded build, not a promise of bit-identical compilation on another platform.

Workload rows are `arrival_ms TAB unique_session TAB max_output_tokens TAB prompt`.
Times must be sorted, sessions unique, and workload bounds are checked before execution.
The executable has a bounded drain deadline; an incomplete run fails instead of publishing
partial success metrics. Removing `--model` runs a synthetic control, explicitly labeled as
such. Failed and rejected requests stay in the deadline-success denominator; latency and TTFT
percentiles are labeled as completion-only. Dispatch lag includes every offered request.
