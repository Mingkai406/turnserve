# Streaming service

The C++ runtime can now serve real loopback HTTP requests. Each connection gets one
independent generation: it does not share conversation history with another client.
The model backend, scheduler, and cancellation contracts are the same as the replay tool.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/turnserve-server --port 8080
curl -N http://127.0.0.1:8080/generate \
  -H 'Content-Type: text/plain' -H 'X-Max-Tokens: 16' --data 'Explain a database index.'
curl http://127.0.0.1:8080/metrics
```

The default backend emits synthetic `x` tokens for transport testing; it is **not an LLM**.
For real inference, use the existing `TURNSERVE_LLAMA=ON` build and pass `--model model.gguf`.
The server prints one readiness JSON line with its actual port and backend (`--port 0`
selects a free port), then final metrics on shutdown.

## Protocol

- `POST /generate`: plain prompt bytes with one `Content-Length` (1–65,536 bytes).
  Optional `X-Max-Tokens: 1..1024`, default 64. Prompt tokenization must still fit the
  2,048-token runtime context. The runtime can reject an oversized tokenized prompt.
- HTTP 200 opens a close-delimited SSE stream. `data` contains the existing event JSON.
  Read through `completed`, `cancelled`, `failed`, or `rejected`; **HTTP 200 alone does
  not mean generation succeeded**. A disconnect can leave the client without a terminal event.
- Admission overload returns HTTP 503 **before** starting SSE. At the connection cap,
  a new socket is closed immediately, without a guaranteed HTTP response.
- `GET /health` and `GET /metrics`: health text and bounded aggregate JSON counters.
- No chunked request bodies, `Expect`, keep-alive, pipelining, chat/session API, or OpenAI
  API compatibility. This is a deliberately limited HTTP/1.1 endpoint.
- Token text uses the runtime's byte-preserving JSON encoding: convert decoded characters
  back to Latin-1 bytes and concatenate bytes before decoding UTF-8 for display.

## Bounds and lifecycle

| Control | Default | Behavior |
|---|---:|---|
| `--max-clients` | 64 | Counts all open sockets, including incomplete requests |
| `--max-inflight` | 16 | Includes queued and decoding generations |
| `--buffer-bytes` | 65536 | Per-client unsent application bytes; overflow disconnects and cancels |
| `--timeout-ms` | 5000 | Absolute request-read deadline; stalled-output deadline after admission |
| `--grace-ms` | 5000 | SIGTERM/SIGINT drain window, then close connections and cancel work |

Socket send buffers are requested at 8 KiB; the OS may adjust this. Application-buffer
limits do not bound kernel or model memory. Header input is capped at 8 KiB, body input
at 64 KiB. Completed/cancelled sessions are explicitly reclaimed, so the runtime's session
cap does not become a lifetime request limit. Disconnected clients queue cancellation
before the next runtime tick, releasing the model slot after acknowledgement.

**Single-owner tradeoff:** networking and decoding use one thread. A backend decode or
prompt encode already in progress cannot be preempted. Deadlines, disconnect detection,
and shutdown take effect when it returns; these are not hard real-time guarantees.
Metrics report generated terminal outcomes, not proof that a remote consumer read every byte.
Bind is fixed to `127.0.0.1`: there is no authentication, TLS, public deployment, or production
hardening claim. Put an appropriate authenticated gateway in front before any external use.

## Reproduce checks and load samples

```sh
ctest --test-dir build --output-on-failure
python3 scripts/http_load.py --binary build/turnserve-server --output /tmp/http-synthetic.json
python3 scripts/http_load.py --binary build-llama/turnserve-server \
  --model models/SmolLM2-135M-Instruct-Q4_K_M.gguf --tokens 16 --output /tmp/http-real.json
TURNSERVE_TEST_MODEL="$PWD/models/SmolLM2-135M-Instruct-Q4_K_M.gguf" \
  python3 tests/http_contracts.py build-llama/turnserve-server
```

The load generator schedules fixed arrivals with one client worker per request. Raw rows
include launch lag, status, terminal outcome, tokens, TTFT and completion time from the
**planned** arrival. Rejections and incomplete streams remain in the denominator for
completed-within-TTFT-deadline fractions. Percentiles explicitly cover completed requests
only. CPU time includes model loading; peak RSS uses the child process resource record.
Committed samples are small local observations, not throughput or scalability claims.
The `--synthetic-tick-delay-ms` server option is test-only pacing and is refused with a model.

### Saved local sample

[Raw real-model sample](../examples/http-service/real-model.json), SmolLM2-135M Q4_K_M,
CPU on the recorded macOS host: 24 arrivals at 5 ms intervals, 16-token output budget,
and 8 admitted requests at a time. **12 completed and 12 received HTTP 503**. Completed
requests had a 213.85 ms p95 TTFT and 289.41 ms p95 completion time; only 12/24 both
completed and received their first token within 500 ms. The server reached about 365 MiB
peak RSS including weights. This one small run validates accounting and overload behavior;
it is not evidence of a production latency target. The [synthetic transport control](../examples/http-service/synthetic.json)
completed 24/24, but its timing says nothing about model inference performance.
