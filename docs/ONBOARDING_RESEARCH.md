# Onboarding research and decision record

The device offers two deliberately different paths: gateway mode for full
agents, and direct API mode for the lowest-friction text experience.

## Recommended flow

1. **Flash in a browser** with ESP Web Tools. It supports Web Serial, Improv
   Wi-Fi, and ESP32-P4 manifests, so a new operator does not need ESP-IDF.
2. **Provision Wi-Fi** in the browser or on the device.
3. **Choose a service on the Tab5** and enter its credential locally.
4. **Run one live test** before entering the console.
5. **Return through Settings** to swap gateways/providers without reflashing.

This keeps the distributable image generic and makes the same binary useful to
Hermes, OpenClaw, or model-only users.

## Service findings

### Hermes

Hermes-Agent is open source, but its native agent environment is richer than a
small embedded display protocol. The included adapter is therefore the easiest
stable boundary: the Tab5 speaks streamed JSON over one authenticated
WebSocket, while the host owns tools, memory, provider credentials, and policy.
The reference adapter also has an offline echo mode for onboarding tests.

- Project: https://github.com/NousResearch/hermes-agent

### OpenClaw

OpenClaw's native WebSocket protocol begins with a challenge and can require
device identity, scopes, and pairing. Implementing that security model on the
panel would increase onboarding and maintenance risk. Its official
OpenAI-compatible Chat Completions endpoint is the proven simpler device
boundary: enable it, use `/v1`, authenticate with the gateway token, and select
`openclaw/default`.

The token should be treated as owner/operator-level authority. Keep the
gateway private and prefer a tailnet or reverse proxy with additional controls.

- HTTP endpoint: https://docs.openclaw.ai/gateway/openai-http-api
- Gateway protocol: https://docs.openclaw.ai/gateway/protocol
- Configuration: https://docs.openclaw.ai/gateway/configuration

### OpenAI

The official API uses a bearer project key and an HTTPS endpoint. That makes
direct device setup simple, but it puts a billable credential in NVS and does
not give the Tab5 an agent tool runtime. Use a dedicated project key with
restricted permissions and budget limits.

- Quickstart: https://developers.openai.com/api/docs/quickstart
- Models: https://developers.openai.com/api/docs/models

### Claude

Anthropic's Messages API uses `x-api-key`, `anthropic-version`, and a model ID.
The firmware implements this separately instead of pretending it is
OpenAI-compatible. A dedicated limited key is still recommended.

- API overview: https://platform.claude.com/docs/en/api/overview
- Model IDs: https://platform.claude.com/docs/en/about-claude/models/model-ids-and-versions

### ElevenLabs

Voice is optional and independent of the selected chat transport. The
text-to-speech endpoint can return 24 kHz raw PCM, avoiding an MP3 decoder on
the ESP32-P4. Workspace-restricted keys and quotas reduce exposure.

- TTS quickstart: https://elevenlabs.io/docs/eleven-api/quickstart/
- API keys: https://elevenlabs.io/docs/overview/administration/workspaces/api-keys
- Convert endpoint: https://elevenlabs.io/docs/api-reference/text-to-speech/convert

### Browser flashing

ESP Web Tools is the simplest public distribution layer because it handles
firmware manifests, Web Serial, ESP32-P4, and Improv provisioning in one flow.

- https://esphome.github.io/esp-web-tools/

## Why credentials are not embedded

Compiling a shared key into a binary makes every clone and every physical unit
share the same compromise. Runtime onboarding allows per-device revocation and
keeps public source reproducible. The tradeoff is that NVS can be extracted by
an attacker with physical access, so high-authority gateways should issue a
dedicated revocable device token rather than reuse a human admin secret.
