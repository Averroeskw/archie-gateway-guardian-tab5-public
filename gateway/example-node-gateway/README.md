# Archie Hermes adapter — Node reference

The same Archie Hermes adapter protocol (see
[`../../docs/GATEWAY_API.md`](../../docs/GATEWAY_API.md)) in a single
well-commented `server.mjs`. **Set an API key and it just works** — it streams
from any OpenAI-compatible provider using Node's built-in `fetch`. No key → demo
echo persona.

## Run it

```bash
cd gateway/example-node-gateway
npm install
cp .env.example .env      # then edit .env (token + LLM key)
npm start
```

Point the Tab5 wizard at this host (type just the hostname — the panel adds
`wss://` and `/ws/tab5`) and paste the token. Health / status:
`curl http://<host>:8787/health` shows the active provider + model.

## Choose a model — no code, just env vars

Same knobs as the Python gateway — see
[`.env.example`](.env.example): `LLM_PROVIDER` (`openai`, `openrouter`, `groq`,
`together`, `deepseek`, `mistral`, `ollama`, `llamacpp`, `custom`), `LLM_API_KEY`,
and optional `LLM_MODEL` / `LLM_BASE_URL` / `LLM_SYSTEM_PROMPT`.

## Deeper customization

To use a non-OpenAI-shaped backend, replace the body of `handleChat(persona,
text)` in `server.mjs` — an async generator that `yield`s reply chunks. STT
(`runPtt`) and vision (`runVision`) are marked as extension points.

## Security

Same rules as the Python example: **always set a token**, and put `wss://`
TLS termination (Caddy/nginx) in front for anything beyond your LAN. Never
expose the plain `ws://` port to the internet.
