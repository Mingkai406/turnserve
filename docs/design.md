# Design notes

## Scope

TurnServe v0.1 is an embeddable single-owner runtime and a replay executable. It is not a network server. A llama.cpp adapter performs actual inference; a synthetic adapter makes lifecycle tests deterministic. The tested model is a dense decoder-only model. Recurrent, hybrid, diffusion and encoder architectures are explicitly rejected. Other GGUF models remain unverified, including their chat templates and context requirements.

## Ownership

`Backend` outlives `Runtime`. `submit()` and `cancel()` are safe for concurrent producers. `tick()`, `idle()`, and `history()` belong to one owner thread. An event sink executes synchronously on that owner; it must not throw or recursively call `tick()`. It can enqueue another command. The demo uses this to replace a request at a defined output-token boundary.

The model and context have RAII owners. A shared model context contains independent sequence IDs. The runtime releases a sequence before returning its slot to the free pool. The backend uses a transient batch buffer and greedy sampling. It does not instantiate a model per user.

## Request contract

```text
submitted → accepted → admitted → prefill → decode → completed
           ↘ rejected        ↘ cancelled / failed
```

`submitted` records the producer's enqueue time. `accepted` follows validation and tokenization. The request can still wait for a sequence slot. A final prefill batch can produce the first output token directly, with no preceding decode event. Every successfully enqueued submission eventually has one terminal event if the runtime is drained without a fatal process error.

The command queue returning `nullopt` means the submission was not enqueued; no request lifecycle or terminal event is created. A cancellation returning `true` means the command was queued, not that cancellation has already happened.

Session generation increments on accepted submissions. Only a completed request that still owns its session generation commits history. Cancellation and failure do not commit partial answers or their prompts. Length-limited generation is a completed turn, with terminal reason `length`. Consumers may choose a stricter application policy later.

Submitting to a busy session requires `replace=true`. Replacement is validated against committed history before cancelling the old request. Unknown or terminal request IDs are harmless cancellation no-ops. In-flight compute is not interrupted inside llama.cpp; pending commands are handled at the next tick. One last token from the current batch can therefore arrive before acknowledgement. No token is emitted after the `cancelled` event.

## Bounds and overload

Defaults: 4 active slots, 32 admitted/queued requests, 64 queued commands, 128 sessions, 8 committed turns retained per session, 2048 context tokens per session, 128 tokens per batch, 64 tokens per prefill chunk. Prompts are limited to 1 MiB and session IDs to 128 bytes during admission. This is not yet an untrusted network boundary; callers can allocate a large string before admission checks.

Every prompt is rendered with the model's chat template and checked together with its requested output budget against the context limit. Old history is limited by turn count; if retained history still exceeds the token budget, the request is rejected rather than silently changing its context. Sessions currently remain registered until runtime destruction; reaching the session cap rejects new session IDs. A future explicit close-session API will reclaim these entries.

There is no prefix-cache reuse across turns. Rebuilding from committed messages makes cancellation recovery explicit but repeats prefill. A context is allocated for the configured slot count; admission bounds do not imply a byte-accurate memory guarantee for arbitrary models.

## Scheduling

Active sequence slots are assigned in request-ID order. FIFO executes one oldest admitted request until termination. Round-robin rotates active order and caps prefill work per request. Interleave stably places decoding requests first, then uses rotated prefill requests for the remaining budget. Since the token budget must exceed the number of active slots, ongoing decodes cannot consume the entire batch budget. Finite admitted prefills rotate to receive work. There is no time-based SLA, adaptive chunk sizing, aging-based admission, or formal starvation proof under unbounded arrivals.

The unit of scheduling is a token, not a microsecond. A prefill-heavy batch can delay outputs even if decode tokens are included in it. Measuring this effect is part of the next benchmark phase.

## Failures

A backend decode exception fails all active requests and releases their sequences because the shared context may be partly mutated. Pending requests can be admitted on the next tick. A sequence-release failure is fatal: reuse of potentially stale state is unsafe. Allocation failures, sink exceptions and failures during token rendering are also process-level failures in this prototype, not yet recoverable request errors.

## Trace format

JSONL events include `type`, `request`, `session`, `generation`, `ms`, `text`, and `count`. Times are monotonic milliseconds relative to runtime construction; model loading is excluded. `count` means input tokens for `accepted`, batch tokens for `prefill`/`decode`, cumulative output tokens for `token`, and total output tokens for terminal events.

`text` is a byte-preserving string: bytes above 127 are encoded as `\u00XX`. Token pieces may split UTF-8 characters, so concatenate their Latin-1-decoded bytes and then decode the aggregate as UTF-8. `inspect_trace.py` does this. The same escaping applies to session IDs. This is an internal trace format, not an OpenAI streaming API.

`prefill` and `decode` mark dispatch, not completion or kernel duration. Do not infer device utilization from these timestamps. Event order preserves owner-thread processing; a producer timestamp can be earlier than preceding emitted events. Consumers should not assume file order is globally timestamp-sorted.

## Future application integration

A network adapter can translate streaming requests and cancellations into the runtime API. For a virtual-patient application, the application retains case state, assessment rules and authorization; TurnServe owns only inference lifecycle and its bounded chat history. Audio interruption also requires the application to stop playback. Fine-tuned weights require compatible architecture, conversion, template and quality checks. Hosted provider models with inaccessible weights cannot use this backend's token-level scheduling.
