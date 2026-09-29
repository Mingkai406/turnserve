# Roadmap

## v0.1 — execution and cancellation

- [x] C++20 runtime with bounded admission and a single model-state owner
- [x] FIFO, round-robin and decode-first interleaving
- [x] Explicit replacement; cancelled generations cannot commit
- [x] Real llama.cpp CPU adapter and pinned small-model download
- [x] Contract tests, replay traces, and source-generated vector figures
- [x] Linux/macOS CI configuration, including a real-model Linux job

CI configuration is not a claim that every platform has passed; check the linked workflow results.

## Next — controlled measurement

- [ ] Workload-file ingestion, fixed arrival replay, repeated trials
- [ ] Native llama-server reference and reproducible benchmark manifests
- [ ] Batch timing, token-gap distributions and memory measurements
- [ ] Model compatibility tests and additional cancellation/failure stress tests

## Then — application adapter

- [ ] Streaming HTTP interface, request cancellation and backpressure semantics
- [ ] Explicit session close, observability and deployment hardening
- [ ] Compatible fine-tuned model integration with quality regression checks

Multi-GPU scheduling, compiler work and model training are outside the current scope.
