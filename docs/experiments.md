# Experiments

## What is recorded now

`results/smoke-cpu` contains one run of the scripted replay for each policy. Its job is to verify real token generation, one explicit replacement, sequence cleanup, terminal accounting, and readable traces. The new question arrives after the old question emits three tokens, so its wall-clock arrival depends on the policy. The long document is deliberately repetitive and the small model is not a quality benchmark.

There are no speedup claims. Individual TTFT values are observations, not estimates of population p95. Model load and context setup precede the runtime clock. CPU model and memory capacity were not recorded, and thermal/load conditions were not controlled. These limits make the checked-in artifacts unsuitable for cross-machine performance comparison.

## Controlled study to implement next

1. Accept versioned workload files instead of hard-coded prompts. Record tokenizer-derived input lengths and requested output caps separately from actual output lengths.
2. Replay fixed open-loop arrivals for load tests; use completion plus think-time for normal multi-turn conversations. Explicitly distinguish interruption events from normal next turns.
3. Compare short-only, mixed-length, burst, and interrupt-heavy workloads. Tune on separate pilot workloads and freeze parameters before measuring.
4. Use identical model weights, CPU/GPU backend, thread count, context capacity, sampling settings and request trace for internal policy comparisons. Add pinned native llama-server as a reference with its settings documented.
5. Warm up and run repeated trials. Keep raw per-request events and outcome counts. Report queue-inclusive TTFT, per-request token gaps, completion latency, throughput, maximum wait, cancellation latency and peak memory.
6. Include cancelled, failed and rejected requests in accounting. Do not improve apparent throughput by quietly discarding work, truncating output differently, or omitting slow requests.

Before publication, record source revision, model hash, dependency revision, compiler, hardware model, RAM, OS, backend flags, threads, workload hash and run order. Attribute differences to evidence, and report regressions as well as improvements. A stronger benchmark may favor the upstream server; that is a useful result.
