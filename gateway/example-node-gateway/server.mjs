// Hermes Gateway (Node reference) — WebSocket bridge between an Archie Tab5
// panel and any LLM.
//
// No code editing required: set an API key in the environment (or a .env file)
// and it streams from any OpenAI-compatible provider. With no key it falls back
// to a demo echo persona. Same envelope protocol as the Python example
// (see docs/GATEWAY_API.md).
//
//   GET  /health        liveness probe (open)
//   WS   /ws/tab5        envelope protocol
//   POST /v1/announce    push a log line to connected panels (X-Hermes-Token)
//
// Env (see .env.example):
//   HERMES_GATEWAY_TOKEN  shared secret the device presents in its hello
//   HERMES_GATEWAY_PORT   port (default 8787)
//   LLM_PROVIDER          openai|openrouter|groq|together|deepseek|mistral|
//                         ollama|llamacpp|custom
//   LLM_API_KEY           provider key (not needed for local ollama/llamacpp)
//   LLM_MODEL / LLM_BASE_URL / LLM_SYSTEM_PROMPT  overrides
//
// Requires Node 20+ (built-in fetch) and the `ws` package (npm install).

import fs from "node:fs";
import http from "node:http";
import { WebSocketServer } from "ws";

// ---- optional .env loading (no dependency) --------------------------------
try {
  for (const line of fs.readFileSync(".env", "utf-8").split("\n")) {
    const t = line.trim();
    if (!t || t.startsWith("#") || !t.includes("=")) continue;
    const i = t.indexOf("=");
    const k = t.slice(0, i).trim();
    const v = t.slice(i + 1).trim().replace(/^["']|["']$/g, "");
    if (!(k in process.env)) process.env[k] = v;
  }
} catch {
  /* no .env file — fine */
}

const TOKEN = process.env.HERMES_GATEWAY_TOKEN || "";
const PORT = parseInt(process.env.HERMES_GATEWAY_PORT || "8787", 10);
const VALID_PERSONAS = new Set(["hermes", "archie", "mira", "custom"]);

// ---- LLM provider config (any OpenAI-compatible API) ----------------------
const PROVIDER_PRESETS = {
  openai: ["https://api.openai.com/v1", "gpt-4o-mini"],
  openrouter: ["https://openrouter.ai/api/v1", "openai/gpt-4o-mini"],
  groq: ["https://api.groq.com/openai/v1", "llama-3.3-70b-versatile"],
  together: ["https://api.together.xyz/v1", "meta-llama/Llama-3.3-70B-Instruct-Turbo"],
  deepseek: ["https://api.deepseek.com/v1", "deepseek-chat"],
  mistral: ["https://api.mistral.ai/v1", "mistral-small-latest"],
  ollama: ["http://localhost:11434/v1", "llama3.2"],
  llamacpp: ["http://localhost:8080/v1", "local-model"],
};
const LLM_PROVIDER = (process.env.LLM_PROVIDER || "openai").toLowerCase();
const LLM_API_KEY = process.env.LLM_API_KEY || "";
const [PRESET_BASE, PRESET_MODEL] = PROVIDER_PRESETS[LLM_PROVIDER] || [
  "https://api.openai.com/v1",
  "gpt-4o-mini",
];
const LLM_BASE_URL = (process.env.LLM_BASE_URL || PRESET_BASE).replace(/\/$/, "");
const LLM_MODEL = process.env.LLM_MODEL || PRESET_MODEL;
const LLM_SYSTEM_PROMPT =
  process.env.LLM_SYSTEM_PROMPT ||
  "You are {persona}, a helpful assistant answering on a small handheld " +
    "console. Keep replies concise and plain-text.";
const LLM_ENABLED = Boolean(LLM_API_KEY) || ["ollama", "llamacpp"].includes(LLM_PROVIDER);
// Conversation memory: past messages resent per request (per persona, per
// connection). Reset by the `clear` envelope.
const LLM_HISTORY_MAX = parseInt(process.env.LLM_HISTORY_MAX || "12", 10);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const envelope = (type, fields = {}) => JSON.stringify({ type, ...fields });

// ---------------------------------------------------------------------------
// THE BRAIN — streams from any OpenAI-compatible chat/completions endpoint.
// Async generator yielding reply text chunks. For a non-OpenAI backend or a
// local CLI, replace the body of this function.
// ---------------------------------------------------------------------------
async function* handleChat(persona, text, history = []) {
  if (!LLM_ENABLED) {
    const reply =
      `[${persona}] Gateway link confirmed. You said: ${JSON.stringify(text)}. ` +
      "Set LLM_API_KEY (see .env.example) to stream from a real model.";
    for (const word of reply.split(" ")) {
      yield word + " ";
      await sleep(40);
    }
    return;
  }

  const headers = { "Content-Type": "application/json" };
  if (LLM_API_KEY) headers["Authorization"] = `Bearer ${LLM_API_KEY}`;
  let resp;
  try {
    resp = await fetch(`${LLM_BASE_URL}/chat/completions`, {
      method: "POST",
      headers,
      body: JSON.stringify({
        model: LLM_MODEL,
        stream: true,
        messages: [
          { role: "system", content: LLM_SYSTEM_PROMPT.replace("{persona}", persona) },
          ...history,
          { role: "user", content: text },
        ],
      }),
    });
  } catch (err) {
    yield `[gateway] LLM request failed: ${err}`;
    return;
  }
  if (!resp.ok) {
    await resp.arrayBuffer();
    console.warn(`LLM HTTP status=${resp.status}`);
    yield `[gateway] LLM error ${resp.status}. Check LLM_* env vars.`;
    return;
  }

  // Parse the SSE stream: lines of "data: {json}" / "data: [DONE]".
  const decoder = new TextDecoder();
  let buffer = "";
  for await (const chunk of resp.body) {
    buffer += decoder.decode(chunk, { stream: true });
    const lines = buffer.split("\n");
    buffer = lines.pop() ?? "";
    for (const line of lines) {
      const t = line.trim();
      if (!t.startsWith("data:")) continue;
      const data = t.slice(5).trim();
      if (data === "[DONE]") return;
      try {
        const delta = JSON.parse(data).choices?.[0]?.delta?.content;
        if (delta) yield delta;
      } catch {
        /* keepalive or partial line */
      }
    }
  }
}

const sessions = new Set();

class PanelSession {
  constructor(ws) {
    this.ws = ws;
    this.authed = !TOKEN; // no token configured -> open (private-LAN only)
    this.busy = false;
    this.persona = "hermes";
    // Per-persona conversation memory; reset by the `clear` envelope.
    this.history = new Map();
  }

  send(payload) {
    if (this.ws.readyState === this.ws.OPEN) this.ws.send(payload);
  }

  sendStatus(state, tokensIn = 0, tokensOut = 0) {
    this.send(envelope("status", {
      persona: this.persona, state, tokens_in: tokensIn, tokens_out: tokensOut,
    }));
  }

  async runChat(persona, text) {
    this.persona = VALID_PERSONAS.has(persona) ? persona : "hermes";
    this.sendStatus("thinking");
    if (!this.history.has(this.persona)) this.history.set(this.persona, []);
    const history = this.history.get(this.persona);
    let tokensOut = 0;
    const replyParts = [];
    try {
      for await (const chunk of handleChat(this.persona, text, history)) {
        tokensOut += Math.max(1, Math.floor(chunk.length / 4));
        replyParts.push(chunk);
        this.send(envelope("chat_delta", { persona: this.persona, text: chunk }));
      }
    } catch (err) {
      console.error("handleChat failed:", err);
      this.send(envelope("error", { text: "chat failed on the gateway" }));
      this.sendStatus("error");
      return;
    }
    this.send(envelope("chat_done", { persona: this.persona, text: "" }));
    // Remember the exchange (capped) so follow-ups have context.
    history.push({ role: "user", content: text });
    history.push({ role: "assistant", content: replyParts.join("") });
    if (history.length > LLM_HISTORY_MAX) history.splice(0, history.length - LLM_HISTORY_MAX);
    this.sendStatus("idle", Math.max(1, Math.floor(text.length / 4)), tokensOut);
  }

  runPtt() {
    // EXTENSION POINT — speech-to-text: decode payload.audio_base64 (mono
    // s16le PCM at payload.sample_rate), transcribe, then runChat().
    this.send(envelope("log", {
      text: "ptt received — STT not configured, plug your speech-to-text into runPtt()",
    }));
    this.sendStatus("idle");
  }

  runVision(payload) {
    // EXTENSION POINT — vision: decode payload.jpeg_base64, send to a
    // multimodal model, reply with vision_result.
    const bytes = (payload.jpeg_base64 || "").length;
    this.send(envelope("vision_result", {
      summary: `Received a frame (~${Math.floor(bytes / 1024)} KB b64). ` +
               "Plug a vision model into runVision() to describe it.",
      confidence: 0.0,
    }));
    this.sendStatus("idle");
  }

  async handle(payload) {
    const kind = payload.type;
    if (kind === "hello") {
      if (TOKEN && payload.token !== TOKEN) {
        this.send(envelope("error", { text: "auth failed" }));
        this.ws.close();
        return;
      }
      this.authed = true;
      console.log(`panel hello: client=${payload.client} version=${payload.version}`);
      this.send(envelope("log", { text: "Hermes Gateway: link established" }));
      this.sendStatus("idle");
      if (!LLM_ENABLED) {
        // Demo persona: seed the panel's TASKS tile so the UI is alive.
        // Real deployments push this envelope from their own task source.
        this.send(envelope("tasks", { items: [
          { label: "Add your LLM key to .env", done: false },
          { label: "Flash the panel", done: true },
          { label: "Read GATEWAY_API.md", done: false },
        ]}));
      }
      return;
    }
    if (!this.authed) {
      this.send(envelope("error", { text: "hello with token required" }));
      return;
    }
    if (kind === "stop") {
      this.send(envelope("log", { text: "stop received" }));
      return;
    }
    if (kind === "clear") {
      // Panel CLEAR: wipe this persona's conversation memory too.
      const persona = payload.persona || this.persona;
      this.history.delete(persona);
      this.send(envelope("log", { text: `context cleared (${persona})` }));
      return;
    }
    if (this.busy) {
      this.send(envelope("log", { text: "busy: previous request still running" }));
      return;
    }
    this.busy = true;
    try {
      if (kind === "chat") {
        await this.runChat(payload.persona || this.persona, payload.text || "");
      } else if (kind === "ptt_audio") {
        this.runPtt(payload);
      } else if (kind === "camera_frame") {
        this.runVision(payload);
      } else {
        this.send(envelope("error", { text: `unknown envelope '${kind}'` }));
      }
    } catch (err) {
      console.error(`handler for ${kind} failed:`, err);
      this.send(envelope("error", { text: `${kind} failed on the gateway` }));
    } finally {
      this.busy = false;
    }
  }
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({
      status: "ok",
      auth: Boolean(TOKEN),
      panels: sessions.size,
      llm: LLM_ENABLED ? LLM_PROVIDER : "demo-echo",
      model: LLM_ENABLED ? LLM_MODEL : null,
    }));
    return;
  }
  if (req.method === "POST" && req.url === "/v1/announce") {
    if (TOKEN && req.headers["x-hermes-token"] !== TOKEN) {
      res.writeHead(401, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: "bad token" }));
      return;
    }
    let body = "";
    req.on("data", (c) => (body += c));
    req.on("end", () => {
      let parsed;
      try {
        parsed = JSON.parse(body || "{}");
      } catch {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: "bad json" }));
        return;
      }
      const text = (parsed.text || "").trim();
      let persona = parsed.persona || "hermes";
      if (!VALID_PERSONAS.has(persona)) persona = "hermes";
      if (!text) {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: "text required" }));
        return;
      }
      let delivered = 0;
      for (const s of sessions) {
        if (s.authed) {
          s.send(envelope("log", { text: `announce [${persona}]: ${text}` }));
          delivered++;
        }
      }
      const status = delivered ? 200 : 503;
      res.writeHead(status, { "content-type": "application/json" });
      res.end(JSON.stringify({ delivered }));
    });
    return;
  }
  res.writeHead(404).end();
});

const wss = new WebSocketServer({ server, path: "/ws/tab5", maxPayload: 8 * 1024 * 1024 });
wss.on("connection", (ws) => {
  const session = new PanelSession(ws);
  sessions.add(session);
  console.log("panel connected");
  ws.on("message", async (data) => {
    let payload;
    try {
      payload = JSON.parse(data.toString());
    } catch {
      session.send(envelope("error", { text: "bad json" }));
      return;
    }
    await session.handle(payload);
  });
  ws.on("close", () => {
    sessions.delete(session);
    console.log("panel disconnected");
  });
  ws.on("error", () => sessions.delete(session));
});

if (!TOKEN) {
  console.warn("=".repeat(64));
  console.warn("HERMES_GATEWAY_TOKEN is NOT set — the gateway is OPEN.");
  console.warn("Anyone who can reach this port can drive your agent.");
  console.warn("Set a token and serve over TLS (wss://) before exposing it.");
  console.warn("=".repeat(64));
}
if (LLM_ENABLED) {
  console.log(`LLM: ${LLM_PROVIDER} · model=${LLM_MODEL} · ${LLM_BASE_URL}`);
} else {
  console.warn(
    "No LLM_API_KEY set — using the demo echo persona. " +
      "Set LLM_PROVIDER + LLM_API_KEY (see .env.example) for real replies.",
  );
}

server.listen(PORT, () => {
  console.log(`Hermes Gateway (Node) on :${PORT} (auth=${Boolean(TOKEN)})`);
});
