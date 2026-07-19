# Archie ↔ Hermes Gateway API

One WebSocket, JSON text envelopes both ways, plus a couple of HTTP endpoints.
The Archie device (M5Stack Tab5 firmware) is the client; your **Hermes Gateway** is
the server. Reference implementations live in [`../gateway/`](../gateway/).

```
Tab5 firmware  ──WebSocket (JSON envelopes)──►  Hermes Gateway  ──►  your agent
               ◄─────────────────────────────                  ◄──
```

- **Endpoint:** `GET /ws/tab5` (WebSocket upgrade). Use `wss://` in production.
- **Framing:** one JSON object per text message. Each has a `"type"` field.
- **Auth:** the device sends `hello` with a shared token first; the gateway
  **closes the socket on a bad token**.

## Device → Gateway

### `hello` (required first)
```json
{"type": "hello", "client": "archie-gateway-guardian-tab5", "version": "0.1.0", "token": "<shared-secret>", "backend": "hermes"}
```
Reject by closing the connection if `token` is wrong. On success, reply with a
`log` line and an initial `status`.

### `chat`
```json
{"type": "chat", "persona": "hermes", "text": "what's my battery?"}
```
`persona` is one of `hermes` / `archie` / `mira` / a custom name.

### `ptt_audio` — a push-to-talk clip
```json
{"type": "ptt_audio", "format": "pcm16", "sample_rate": 16000,
 "persona": "hermes", "wake": false, "audio_base64": "..."}
```
Mono `s16le` PCM, base64-encoded. Transcribe, then treat the text as `chat`.
`wake: true` marks a hands-free clip the gateway should ignore unless it begins
with a wake word. (The shipped firmware build is text-first; this envelope is
defined for builds that enable the mic.)

### `camera_frame` — a vision snapshot
```json
{"type": "camera_frame", "source": "tab5_cam", "mode": "snapshot",
 "prompt": "What am I looking at?", "jpeg_base64": "..."}
```
`mode` is `snapshot` (user-initiated) or `sentry` (motion event).

### `stop`
```json
{"type": "stop"}
```
Cancel the in-flight request.

### `clear`
```json
{"type": "clear", "persona": "hermes"}
```
Sent when the user taps CLEAR: drop the persona's server-side conversation
memory. The panel wipes its own screen; the gateway should forget the thread
(the reference gateways keep per-persona history per connection).

## Gateway → Device

### `chat_delta` / `chat_done` — streamed reply
```json
{"type": "chat_delta", "persona": "hermes", "text": "chunk of words "}
{"type": "chat_done",  "persona": "hermes", "text": ""}
```
Stream the reply as `chat_delta`s; `chat_done` marks the terminal turn complete
(its `text` may be empty). The firmware owns the final terminal rendering.

### `status` — agent state + token counters
```json
{"type": "status", "persona": "hermes", "state": "idle|thinking|speaking|error",
 "tokens_in": 0, "tokens_out": 123}
```
Drives Archie's particle-core state and the token gauge.

### `log` / `error` — session-log lines
```json
{"type": "log", "text": "link established"}
{"type": "error", "text": "vision: inference timed out"}
```

### `tasks` — TASKS panel snapshot (optional)
```json
{"type": "tasks", "items": [
  {"label": "Review print order", "done": true},
  {"label": "Call supplier 16:00", "done": false}
]}
```
Replaces the panel's task list (up to 6 items shown; labels truncated at 47
chars). Send whenever your task source changes — the reference gateways send
a demo list on hello when no LLM key is configured.

### `vision_result`
```json
{"type": "vision_result", "summary": "A desk with a soldering iron…", "confidence": 0.0}
```

### `tts` — voice reply ready (optional)
```json
{"type": "tts", "url": "/audio/ab12cd34.wav"}
```
A relative URL resolves against the gateway host. Ship 16/24 kHz mono `s16le`
WAV so the MCU needs no decoder. The shipped firmware's default voice path
synthesizes completed replies locally with ElevenLabs; this route is optional
and only needed by audio-enabled gateway builds.

## HTTP

### `GET /health` (open)
```json
{"status": "ok", "auth": true, "panels": 1}
```

### `GET /audio/<clip>.wav`
Serves TTS clips referenced by `tts` envelopes.

### `POST /v1/announce` — push a line to connected panels
Header `X-Hermes-Token: <token>`, body `{"text": "...", "persona": "hermes"}`.
Returns `{"delivered": N}` (503 when no panel is connected). The reverse path:
make the panel speak/log from anything that can reach the gateway.

## Connection behavior

- The client opens the socket, sends `hello`, and **auto-reconnects** if it
  drops. A `hello` always precedes other envelopes.
- The gateway should **serialize work per connection** (one chat/ptt/vision at
  a time) and answer extra requests with a `log` "busy" line rather than
  queueing.
- Keepalive: the reference gateways set a WS heartbeat (~30 s). Idle sockets
  stay open.

## Writing your own gateway — checklist

1. Accept the WS upgrade at `/ws/tab5`.
2. Require `hello`; validate the token; close on mismatch.
3. On `chat`, stream `chat_delta`s and finish with `chat_done`; bracket the
   turn with `status` (`thinking` → `idle`) and token counts.
4. Emit `log`/`error` for anything the operator should see.
5. Serve `GET /health`.
6. (Optional) implement `ptt_audio`/`camera_frame`/`tts` and `/audio/<clip>`.
7. Put TLS (`wss://`) in front and **always require a token**.
