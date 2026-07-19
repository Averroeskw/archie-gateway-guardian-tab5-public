# Archie Hermes adapter — Python reference

A minimal, dependency-light gateway that speaks Archie's Hermes adapter protocol
(see [`../../docs/GATEWAY_API.md`](../../docs/GATEWAY_API.md)). **Set an API key
and it just works** — it streams from any OpenAI-compatible provider. With no
key it runs a demo echo persona so you can still validate a Tab5 panel.

## Fastest path

```bash
cd gateway
./quickstart.sh          # venv + deps + a generated token in .env, then starts
```

Then edit `example-python-gateway/.env`, set `LLM_PROVIDER` + `LLM_API_KEY`,
and restart.

## Manual

```bash
cd gateway/example-python-gateway
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt

cp .env.example .env      # then edit .env (token + LLM key)
python hermes_gateway.py
```

Point the Tab5 wizard at this host (type just the hostname — the panel adds
`wss://` and `/ws/tab5`) and paste the same token. Health / status:
`curl http://<host>:8787/health` shows the active provider + model.

## Choose a model — no code, just env vars

Set these in `.env` (see [`.env.example`](.env.example)):

| Provider | `LLM_PROVIDER` | Key var | Notes |
|---|---|---|---|
| OpenAI | `openai` | `LLM_API_KEY=<provider-key>` | default `gpt-4o-mini` |
| OpenRouter | `openrouter` | `LLM_API_KEY=<provider-key>` | any supported model via `LLM_MODEL` |
| Groq | `groq` | `LLM_API_KEY=<provider-key>` | fast hosted models |
| Together / DeepSeek / Mistral | `together`/`deepseek`/`mistral` | `LLM_API_KEY=…` | |
| Local Ollama | `ollama` | — (no key) | `ollama serve`, set `LLM_MODEL` |
| llama.cpp server | `llamacpp` | — | OpenAI-compat server on :8080 |
| Anything else | `custom` | maybe | set `LLM_BASE_URL` + `LLM_MODEL` |

`LLM_MODEL`, `LLM_BASE_URL` and `LLM_SYSTEM_PROMPT` override the defaults.

## Deeper customization

To use a non-OpenAI-shaped backend or a local CLI agent, replace the body of
`handle_chat(persona, text)` in `hermes_gateway.py` — it's an async generator
that `yield`s reply chunks. The STT (`run_ptt`) and TTS (`announce`) extension
points are marked in the file.

## Security

- **Always set `HERMES_GATEWAY_TOKEN`.** With no token the gateway is open and
  anyone who can reach the port can drive your agent (it warns loudly on boot).
- **Use TLS for anything off-LAN.** Terminate `wss://` at a reverse proxy
  (Caddy/nginx/Traefik) and point the panel at `wss://your-domain/ws/tab5`.
  Example Caddyfile:

  ```
  your-domain.example.com {
      reverse_proxy 127.0.0.1:8787
  }
  ```

- Run it as a non-root user; the included `hermes-gateway.service` shows a
  hardened-ish systemd setup with an `EnvironmentFile` for the token.
