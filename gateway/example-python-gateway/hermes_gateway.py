#!/usr/bin/env python3
"""Hermes Gateway (Python reference) — WebSocket bridge between an Archie
Gateway Guardian panel (M5Stack Tab5 firmware in this repo) and any LLM.

No code editing required: set an API key in the environment (or a .env file)
and the gateway streams replies from any OpenAI-compatible provider. With no
key it falls back to a demo echo persona so you can still validate the device.

Endpoints:
  GET /health                 liveness probe (open)
  WS  /ws/tab5                envelope protocol (docs/GATEWAY_API.md)
  GET /audio/<clip>.wav       TTS clips fetched by the device
  POST /v1/announce           push a spoken/log line to connected panels

Config via environment (see .env.example):
  HERMES_GATEWAY_TOKEN  shared secret the device must present in its hello.
                        ALWAYS set this; the gateway warns loudly when open.
  HERMES_GATEWAY_BIND   bind address (default 0.0.0.0)
  HERMES_GATEWAY_PORT   port (default 8787)
  HERMES_GATEWAY_AUDIO  directory for generated WAV clips

  LLM_PROVIDER   openai | openrouter | groq | together | deepseek | mistral |
                 ollama | llamacpp | custom  (picks a base URL + default model)
  LLM_API_KEY    your provider API key (not needed for local ollama/llamacpp)
  LLM_MODEL      model id (overrides the provider default)
  LLM_BASE_URL   override the base URL (for `custom` or self-hosted)
  LLM_SYSTEM_PROMPT  system prompt; "{persona}" is substituted
"""

import asyncio
import base64
import binascii
import json
import logging
import os
import struct
from pathlib import Path

from aiohttp import ClientSession, ClientTimeout, web

log = logging.getLogger("hermes-gateway")

# ---- optional .env loading (no dependency; a tiny parser) ------------------
def _load_dotenv(path=".env"):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                key = key.strip()
                value = value.strip().strip('"').strip("'")
                os.environ.setdefault(key, value)
    except FileNotFoundError:
        pass


_load_dotenv()

TOKEN = os.environ.get("HERMES_GATEWAY_TOKEN", "")
BIND = os.environ.get("HERMES_GATEWAY_BIND", "0.0.0.0")
PORT = int(os.environ.get("HERMES_GATEWAY_PORT", "8787"))
AUDIO_DIR = Path(os.environ.get("HERMES_GATEWAY_AUDIO", "/tmp/hermes-gateway-audio"))

MAX_FRAME_BYTES = 8 * 1024 * 1024
VALID_PERSONAS = ("hermes", "archie", "mira", "custom")

# ---- LLM provider config (any OpenAI-compatible API) ----------------------
# (base_url, default_model). Anthropic models are reachable via openrouter.
PROVIDER_PRESETS = {
    "openai": ("https://api.openai.com/v1", "gpt-4o-mini"),
    "openrouter": ("https://openrouter.ai/api/v1", "openai/gpt-4o-mini"),
    "groq": ("https://api.groq.com/openai/v1", "llama-3.3-70b-versatile"),
    "together": ("https://api.together.xyz/v1", "meta-llama/Llama-3.3-70B-Instruct-Turbo"),
    "deepseek": ("https://api.deepseek.com/v1", "deepseek-chat"),
    "mistral": ("https://api.mistral.ai/v1", "mistral-small-latest"),
    "ollama": ("http://localhost:11434/v1", "llama3.2"),
    "llamacpp": ("http://localhost:8080/v1", "local-model"),
}

LLM_PROVIDER = os.environ.get("LLM_PROVIDER", "openai").lower()
LLM_API_KEY = os.environ.get("LLM_API_KEY", "")
_preset_base, _preset_model = PROVIDER_PRESETS.get(LLM_PROVIDER, ("https://api.openai.com/v1", "gpt-4o-mini"))
LLM_BASE_URL = os.environ.get("LLM_BASE_URL", _preset_base).rstrip("/")
LLM_MODEL = os.environ.get("LLM_MODEL", _preset_model)
LLM_SYSTEM_PROMPT = os.environ.get(
    "LLM_SYSTEM_PROMPT",
    "You are {persona}, a helpful assistant answering on a small handheld "
    "console. Keep replies concise and plain-text.",
)
# Local providers don't need a key; hosted ones do.
LLM_ENABLED = bool(LLM_API_KEY) or LLM_PROVIDER in ("ollama", "llamacpp")
# Conversation memory: how many past messages (user+assistant) to resend per
# request. Per persona, per connection; the panel's CLEAR button resets it.
LLM_HISTORY_MAX = int(os.environ.get("LLM_HISTORY_MAX", "12"))


# --------------------------------------------------------------------------
# THE BRAIN — streams from any OpenAI-compatible chat/completions endpoint.
#
# Contract: async generator yielding reply text chunks. The session layer
# turns chunks into chat_delta envelopes and closes with chat_done.
#
# To use a provider the presets don't cover, set LLM_PROVIDER=custom and
# LLM_BASE_URL / LLM_MODEL. To swap in a non-OpenAI-shaped backend or a local
# CLI agent, replace the body of this function.
# --------------------------------------------------------------------------
async def handle_chat(persona: str, text: str, history=None):
    if not LLM_ENABLED:
        # Demo echo persona (no API key configured).
        reply = (
            f"[{persona}] Gateway link confirmed. You said: {text!r}. "
            "Set LLM_API_KEY (see .env.example) to stream from a real model."
        )
        for word in reply.split(" "):
            yield word + " "
            await asyncio.sleep(0.04)
        return

    system = LLM_SYSTEM_PROMPT.format(persona=persona)
    payload = {
        "model": LLM_MODEL,
        "stream": True,
        "messages": [
            {"role": "system", "content": system},
            *(history or []),
            {"role": "user", "content": text},
        ],
    }
    headers = {"Content-Type": "application/json"}
    if LLM_API_KEY:
        headers["Authorization"] = f"Bearer {LLM_API_KEY}"
    url = f"{LLM_BASE_URL}/chat/completions"
    try:
        async with ClientSession(timeout=ClientTimeout(total=120)) as http:
            async with http.post(url, json=payload, headers=headers) as resp:
                if resp.status != 200:
                    resp.release()
                    log.warning("LLM HTTP status=%s", resp.status)
                    yield f"[gateway] LLM error {resp.status}. Check LLM_* env vars."
                    return
                async for raw in resp.content:
                    line = raw.decode("utf-8", "ignore").strip()
                    if not line.startswith("data:"):
                        continue
                    data = line[len("data:"):].strip()
                    if data == "[DONE]":
                        break
                    try:
                        delta = json.loads(data)["choices"][0]["delta"].get("content")
                    except (json.JSONDecodeError, KeyError, IndexError):
                        continue
                    if delta:
                        yield delta
    except asyncio.TimeoutError:
        yield "[gateway] LLM request timed out."
    except Exception as exc:  # network / DNS / provider down
        log.warning("LLM request failed: %s", exc)
        yield f"[gateway] LLM request failed: {exc}"


def envelope(type_, **fields):
    fields["type"] = type_
    return json.dumps(fields)


# Connected panels, so /v1/announce can push to whatever is online.
SESSIONS = set()


class PanelSession:
    """One connected panel. Serializes agent work: the device is single-user,
    so one chat/ptt/vision request runs at a time; extras get a 'busy' log."""

    def __init__(self, ws):
        self.ws = ws
        self.authed = not TOKEN  # no token configured -> open (private-LAN only)
        self.busy = asyncio.Lock()
        self.current_task = None
        self.persona = "hermes"
        # Per-persona conversation memory, so switching personas keeps each
        # thread intact. Reset by the `clear` envelope.
        self.history = {}

    async def send(self, payload: str):
        if not self.ws.closed:
            await self.ws.send_str(payload)

    async def send_status(self, state, tokens_in=0, tokens_out=0):
        await self.send(
            envelope("status", persona=self.persona, state=state,
                     tokens_in=tokens_in, tokens_out=tokens_out)
        )

    # ---- chat -----------------------------------------------------------

    async def run_chat(self, persona, text):
        self.persona = persona if persona in VALID_PERSONAS else "hermes"
        await self.send_status("thinking")
        history = self.history.setdefault(self.persona, [])
        tokens_out = 0
        reply_parts = []
        try:
            async for chunk in handle_chat(self.persona, text, history):
                tokens_out += max(1, len(chunk) // 4)
                reply_parts.append(chunk)
                await self.send(envelope("chat_delta", persona=self.persona, text=chunk))
        except Exception:
            log.exception("handle_chat failed")
            await self.send(envelope("error", text="chat failed on the gateway"))
            await self.send_status("error")
            return
        await self.send(envelope("chat_done", persona=self.persona, text=""))
        # Remember the exchange (capped) so follow-ups have context.
        history.append({"role": "user", "content": text})
        history.append({"role": "assistant", "content": "".join(reply_parts)})
        if len(history) > LLM_HISTORY_MAX:
            del history[: len(history) - LLM_HISTORY_MAX]
        await self.send_status("idle", tokens_in=max(1, len(text) // 4), tokens_out=tokens_out)

    # ---- voice in (push-to-talk) -----------------------------------------

    async def run_ptt(self, payload):
        persona = payload.get("persona", self.persona)
        try:
            pcm = base64.b64decode(payload.get("audio_base64", ""), validate=True)
        except (binascii.Error, ValueError):
            await self.send(envelope("error", text="ptt: bad audio payload"))
            return
        if not pcm:
            return
        await self.send(envelope(
            "log",
            text=f"ptt: {len(pcm) // 1024} KB received — STT not configured, "
                 "plug your speech-to-text into run_ptt()",
        ))
        # EXTENSION POINT — speech-to-text:
        #   wav = pcm_to_wav(pcm, int(payload.get("sample_rate", 16000)))
        #   transcript = await your_stt(wav)   # Whisper API, local whisper.cpp, ...
        #   await self.run_chat(persona, transcript)
        await self.send_status("idle")

    # ---- vision ----------------------------------------------------------

    async def run_vision(self, payload):
        try:
            jpeg = base64.b64decode(payload.get("jpeg_base64", ""), validate=True)
        except (binascii.Error, ValueError):
            await self.send(envelope("error", text="vision: bad frame payload"))
            return
        if not jpeg:
            return
        await self.send_status("thinking")
        # EXTENSION POINT — vision: send the JPEG to a multimodal model and
        # reply with its description. The demo just acknowledges the frame.
        await self.send(envelope(
            "vision_result",
            summary=f"Received a {len(jpeg) // 1024} KB frame. "
                    "Plug a vision model into run_vision() to describe it.",
            confidence=0.0,
        ))
        await self.send_status("idle")

    # ---- dispatch ----------------------------------------------------------

    async def handle(self, payload):
        kind = payload.get("type")
        if kind == "hello":
            if TOKEN and payload.get("token") != TOKEN:
                await self.send(envelope("error", text="auth failed"))
                await self.ws.close()
                return
            self.authed = True
            log.info("panel hello: client=%s version=%s",
                     payload.get("client"), payload.get("version"))
            await self.send(envelope("log", text="Hermes Gateway: link established"))
            await self.send_status("idle")
            if not LLM_ENABLED:
                # Demo persona: seed the panel's TASKS tile so the UI is alive.
                # Real deployments push this envelope from their own task source.
                await self.send(envelope("tasks", items=[
                    {"label": "Add your LLM key to .env", "done": False},
                    {"label": "Flash the panel", "done": True},
                    {"label": "Read GATEWAY_API.md", "done": False},
                ]))
            return
        if not self.authed:
            await self.send(envelope("error", text="hello with token required"))
            return
        if kind == "stop":
            if self.current_task and not self.current_task.done():
                self.current_task.cancel()
                await self.send(envelope("log", text="generation stopped"))
            return
        if kind == "clear":
            # Panel CLEAR: wipe this persona's conversation memory too.
            persona = payload.get("persona", self.persona)
            self.history.pop(persona, None)
            await self.send(envelope("log", text=f"context cleared ({persona})"))
            return
        runner = {
            "chat": lambda: self.run_chat(payload.get("persona", self.persona),
                                          payload.get("text", "")),
            "ptt_audio": lambda: self.run_ptt(payload),
            "camera_frame": lambda: self.run_vision(payload),
        }.get(kind)
        if runner is None:
            await self.send(envelope("error", text=f"unknown envelope '{kind}'"))
            return
        if self.busy.locked():
            await self.send(envelope("log", text="busy: previous request still running"))
            return

        async def guarded():
            async with self.busy:
                try:
                    await runner()
                except asyncio.CancelledError:
                    await self.send_status("idle")
                except Exception:
                    log.exception("handler for %s failed", kind)
                    await self.send(envelope("error", text=f"{kind} failed on the gateway"))

        self.current_task = asyncio.create_task(guarded())


def pcm_to_wav(pcm: bytes, rate: int) -> bytes:
    """Wrap raw mono s16le PCM in a WAV header (for your STT plug-in)."""
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(pcm), b"WAVE", b"fmt ", 16,
        1, 1, rate, rate * 2, 2, 16, b"data", len(pcm),
    )
    return header + pcm


async def ws_handler(request):
    ws = web.WebSocketResponse(max_msg_size=MAX_FRAME_BYTES, heartbeat=30)
    await ws.prepare(request)
    session = PanelSession(ws)
    SESSIONS.add(session)
    peer = request.remote
    log.info("panel connected from %s", peer)
    try:
        async for msg in ws:
            if msg.type == web.WSMsgType.TEXT:
                try:
                    payload = json.loads(msg.data)
                except json.JSONDecodeError:
                    await session.send(envelope("error", text="bad json"))
                    continue
                await session.handle(payload)
            elif msg.type == web.WSMsgType.ERROR:
                break
    finally:
        SESSIONS.discard(session)
        if session.current_task and not session.current_task.done():
            session.current_task.cancel()
        log.info("panel disconnected (%s)", peer)
    return ws


async def announce(request):
    """Push a log line to every connected panel: POST {"text": "...", "persona": "hermes"}.
    Authenticated with the shared token (X-Hermes-Token header). Pair this with
    a TTS plug-in to make the panel speak."""
    if TOKEN and request.headers.get("X-Hermes-Token") != TOKEN:
        return web.json_response({"error": "bad token"}, status=401)
    try:
        body = await request.json()
    except json.JSONDecodeError:
        return web.json_response({"error": "bad json"}, status=400)
    text = (body.get("text") or "").strip()
    persona = body.get("persona", "hermes")
    if persona not in VALID_PERSONAS:
        persona = "hermes"
    if not text:
        return web.json_response({"error": "text required"}, status=400)
    live = [s for s in SESSIONS if s.authed and not s.ws.closed]
    if not live:
        return web.json_response({"delivered": 0, "note": "no panel connected"}, status=503)
    for session in live:
        await session.send(envelope("log", text=f"announce [{persona}]: {text}"))
        # EXTENSION POINT — TTS: synthesize `text` to a 16/24 kHz mono s16le
        # WAV in AUDIO_DIR and send envelope("tts", url=f"/audio/{clip}.wav").
    return web.json_response({"delivered": len(live)})


async def health(_request):
    return web.json_response({
        "status": "ok",
        "auth": bool(TOKEN),
        "panels": len(SESSIONS),
        "llm": LLM_PROVIDER if LLM_ENABLED else "demo-echo",
        "model": LLM_MODEL if LLM_ENABLED else None,
    })


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if not TOKEN:
        log.warning("=" * 64)
        log.warning("HERMES_GATEWAY_TOKEN is NOT set — the gateway is OPEN.")
        log.warning("Anyone who can reach this port can drive your agent.")
        log.warning("Set a token and serve over TLS (wss://) before exposing it.")
        log.warning("=" * 64)
    if LLM_ENABLED:
        log.info("LLM: %s · model=%s · %s", LLM_PROVIDER, LLM_MODEL, LLM_BASE_URL)
    else:
        log.warning("No LLM_API_KEY set — using the demo echo persona. "
                    "Set LLM_PROVIDER + LLM_API_KEY (see .env.example) for real replies.")
    AUDIO_DIR.mkdir(parents=True, exist_ok=True)
    app = web.Application()
    app.router.add_get("/health", health)
    app.router.add_get("/ws/tab5", ws_handler)
    app.router.add_post("/v1/announce", announce)
    app.router.add_static("/audio/", str(AUDIO_DIR))
    log.info("Hermes Gateway on %s:%s (auth=%s)", BIND, PORT, bool(TOKEN))
    web.run_app(app, host=BIND, port=PORT)


if __name__ == "__main__":
    main()
