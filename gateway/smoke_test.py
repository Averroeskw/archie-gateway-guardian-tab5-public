#!/usr/bin/env python3
"""Small protocol smoke test for either reference Hermes gateway."""

import argparse
import asyncio
import json
from urllib.parse import urlparse

import aiohttp


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://127.0.0.1:8787")
    parser.add_argument("--token", default="ci-smoke-token")
    args = parser.parse_args()
    base = args.url.rstrip("/")
    parsed = urlparse(base)
    if parsed.scheme not in ("http", "https"):
        raise RuntimeError("--url must use http:// or https://")
    if parsed.scheme == "http" and parsed.hostname not in ("127.0.0.1", "localhost", "::1"):
        raise RuntimeError("plaintext smoke tests are limited to loopback")

    def require(condition, message):
        if not condition:
            raise RuntimeError(message)

    async with aiohttp.ClientSession() as session:
        async with session.get(f"{base}/health", timeout=5) as response:
            require(response.status == 200, f"health returned {response.status}")
        ws_url = base.replace("http://", "ws://", 1).replace("https://", "wss://", 1)
        async with session.ws_connect(f"{ws_url}/ws/tab5", timeout=5) as ws:
            await ws.send_json({"type": "hello", "client": "ci-smoke", "version": "test", "token": args.token})
            seen = set()
            for _ in range(8):
                message = await ws.receive(timeout=5)
                require(message.type == aiohttp.WSMsgType.TEXT, f"unexpected hello frame: {message.type}")
                payload = json.loads(message.data)
                seen.add(payload.get("type"))
                if payload.get("type") == "status" and payload.get("state") == "idle":
                    break
            require({"log", "status"} <= seen, f"missing hello envelopes: {seen}")
            await ws.send_json({"type": "chat", "persona": "hermes", "text": "smoke test"})
            chat_seen = set()
            chat_reached_idle = False
            for _ in range(80):
                message = await ws.receive(timeout=5)
                require(message.type == aiohttp.WSMsgType.TEXT, f"unexpected chat frame: {message.type}")
                payload = json.loads(message.data)
                chat_seen.add(payload.get("type"))
                if payload.get("type") == "status" and payload.get("state") == "idle":
                    chat_reached_idle = True
                    break
            require(chat_reached_idle and {"chat_delta", "chat_done"} <= chat_seen,
                    f"missing chat envelopes: {chat_seen}")


if __name__ == "__main__":
    asyncio.run(main())
