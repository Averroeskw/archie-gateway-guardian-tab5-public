#!/usr/bin/env bash
# One-command setup for the Python Hermes Gateway.
#   ./quickstart.sh            # set up + start
#   ./quickstart.sh --no-start # just set up
#
# Creates a venv, installs deps, and scaffolds a .env with a fresh token.
# Then paste your LLM API key into gateway/example-python-gateway/.env.
set -euo pipefail

cd "$(dirname "$0")/example-python-gateway"

echo "▸ Creating virtualenv…"
python3 -m venv .venv
# shellcheck disable=SC1091
. .venv/bin/activate
echo "▸ Installing dependencies…"
pip install --quiet --upgrade pip
pip install --quiet -r requirements.txt

if [ ! -f .env ]; then
  echo "▸ Creating .env with a fresh gateway token…"
  cp .env.example .env
  TOKEN="$(python3 -c 'import secrets; print(secrets.token_hex(24))')"
  # Fill in the token line (portable sed for macOS + Linux).
  if sed --version >/dev/null 2>&1; then
    sed -i "s|^HERMES_GATEWAY_TOKEN=.*|HERMES_GATEWAY_TOKEN=${TOKEN}|" .env
  else
    sed -i '' "s|^HERMES_GATEWAY_TOKEN=.*|HERMES_GATEWAY_TOKEN=${TOKEN}|" .env
  fi
  echo ""
  echo "  Gateway token (enter this in the Tab5 wizard):"
  echo "    ${TOKEN}"
  echo ""
  echo "  Now edit .env and set LLM_PROVIDER + LLM_API_KEY for real replies."
  echo "  (No key? It runs a demo echo persona so you can still test the panel.)"
else
  echo "▸ .env already exists — leaving it as is."
fi

if [ "${1:-}" = "--no-start" ]; then
  echo "▸ Setup done. Start later with:  cd example-python-gateway && . .venv/bin/activate && python hermes_gateway.py"
  exit 0
fi

echo "▸ Starting gateway (Ctrl-C to stop)…"
exec python hermes_gateway.py
