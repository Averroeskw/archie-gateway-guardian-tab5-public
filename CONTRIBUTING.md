# Contributing

Thanks for helping Archie guard more gateways.

## Build

```bash
cd firmware
idf.py set-target esp32p4
idf.py reconfigure
python3 scripts/patch_usb_component.py
idf.py build
```

The supported baseline is ESP-IDF 5.4.2 and the ESP32-P4 target.

## Before a pull request

1. Run the firmware build when C/C++, assets, partitions, or configuration
   changes.
2. Run Python, Node, and shell syntax checks for gateway changes.
3. Update both the implementation and relevant documentation.
4. Inspect `git diff --staged` and run the secret checks in `SECURITY.md`.
5. Never commit `.env`, SDK output, network identifiers, API keys, tokens,
   certificates, device profiles, coredumps, or personal paths.

Secrets belong in device NVS or an ignored local `.env`. Test values in docs
must use angle-bracket placeholders such as `<provider-key>`.

Every LVGL mutation must run under the BSP display lock. Keep animation work
bounded: the display driver uses a small strip buffer so the LVGL task stays
responsive while software rotation is active.

Protocol changes must update [docs/GATEWAY_API.md](docs/GATEWAY_API.md) and
both reference gateways in the same pull request.
