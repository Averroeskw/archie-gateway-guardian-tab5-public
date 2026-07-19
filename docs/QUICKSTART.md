# Quickstart

Get Archie from a blank M5Stack Tab5 to a verified AI link without placing a
secret in source code.

## 1. Flash

### Browser (easiest for a release build)

Open the project's published installer in Chrome or Edge, connect the Tab5 over
USB-C, and choose **Install**. The installer uses Web Serial and can offer
Improv Wi-Fi provisioning after flashing. GitHub Pages can host it after the
repository is made public; it can also be served locally over HTTPS.

To assemble a local installer from a clean release checkout, use
`bash scripts/prepare_web_installer.sh <merged-image> <version>`. The script
refuses to package while an ignored `*.local.h` firmware profile exists, then
serve the generated `site/` directory with `python3 -m http.server --directory
site 8000`. Do not use a development build containing device credentials.

The browser route is the recommended public installer because it works on
macOS, Linux, Windows, and ChromeOS without asking an operator to install the
ESP-IDF toolchain. Chrome or Edge and a USB data cable are required.

### M5Burner (planned public catalog route)

[M5Burner](https://docs.m5stack.com/en/uiflow/m5burner/intro) is M5Stack's
cross-platform firmware catalog and burner. It can export, publish, and share
firmware. Add Archie there only after a signed/tagged release has passed the
display-revision test matrix; do not upload development images containing a
provisioned NVS partition.

### EasyLoader (optional Windows one-click package)

M5Stack's [EasyLoader Packer](https://docs.m5stack.com/en/guide/easyloader/easyloader_packer)
can wrap the merged release binary and esptool in a branded Windows `.exe`.
Use offset `0x0` with `archie-gateway-guardian-tab5-merged.bin`. This is a
convenience mirror, not the canonical artifact; publish the SHA-256 checksum
from the matching GitHub Release beside it.

### Merged binary

```bash
python3 -m pip install esptool
esptool.py --chip esp32p4 -b 460800 write_flash 0x0 \
  archie-gateway-guardian-tab5-merged.bin
```

### Source build

Use ESP-IDF 5.4.2, the version recommended by M5Stack's Tab5 UserDemo:

```bash
cd firmware
idf.py set-target esp32p4
idf.py reconfigure
python3 scripts/patch_usb_component.py
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Adjust the serial port for your OS. The USB dependency patch is idempotent and
is required after dependency resolution on stock IDF 5.4.2.

### If the Tab5 does not appear as a serial port

1. Use a known USB **data** cable and connect directly rather than through a
   charge-only adapter.
2. With USB connected, hold Reset for about two seconds until the internal
   green LED flashes rapidly, then release it. This is M5Stack's documented
   download mode.
3. Re-open the browser installer or list ports again.
4. If the host still cannot see the device, consult M5Stack's
   [USB-driver page](https://docs.m5stack.com/en/download) for that computer.

The official [Tab5 product page](https://docs.m5stack.com/en/core/Tab5) also
links factory-firmware and ESP32-C6 Wi-Fi recovery procedures. Those are the
safe fallback when a user needs to prove that a problem is not in Archie.

## 2. Complete Archie's setup portal

1. **Wi-Fi** — enter a 2.4 GHz SSID and password. If Improv already supplied
   them, the device skips this step automatically.
2. **Link** — select Hermes, OpenClaw, OpenAI, Claude, or Custom.
3. **Credentials** — enter the endpoint, token/key, and model required by the
   selected link. Secret fields are masked.
4. **Voice** — leave disabled, or enable ElevenLabs and enter a dedicated key
   plus voice ID.
5. **Test** — Archie verifies Wi-Fi and the chosen service. A direct-provider
   test sends one short, billable request. When all checks pass, Archie
   condenses from his 3D-derived particle field into the command console.

## 3. Choose one link

### Hermes adapter

```bash
cd gateway
./quickstart.sh
```

Use the printed WebSocket URL and generated token on the Tab5. The adapter
works in echo mode without an LLM key. Add a model key only after the device
link is verified.

### OpenClaw

Enable the Chat Completions compatibility endpoint in the OpenClaw gateway
configuration:

```json5
{
  gateway: {
    http: {
      endpoints: {
        chatCompletions: { enabled: true }
      }
    }
  }
}
```

Restart the OpenClaw gateway, then enter:

```text
Link       OpenClaw
Base URL   http://<trusted-openclaw-host>:18789/v1
Key        <gateway bearer token>
Model      openclaw/default
```

Use a private LAN, VPN, or tailnet. The OpenClaw gateway token has broad
operator authority; do not put it in screenshots, logs, issues, or a public
repo.

### OpenAI

```text
Link       OpenAI
Base URL   https://api.openai.com/v1
Key        <dedicated OpenAI project key>
Model      gpt-5.6-terra
```

### Claude

```text
Link       Claude
Base URL   https://api.anthropic.com
Key        <dedicated Anthropic API key>
Model      claude-sonnet-5
```

### Custom

Enter an HTTPS OpenAI-compatible base URL, key if required, and exact model
name. Plain HTTP should be limited to a trusted local service.

## 4. Operate and reconfigure

Type a prompt and press `Enter`. The deck offers brief/tasks shortcuts,
persona cycling, clear, stop, and settings. Open `SET` → `SETUP WIZARD` to
change Wi-Fi, provider, endpoint, API key, model, or voice later.

## 5. Remove all personal data

NVS survives a normal firmware update. Before handing the device to someone
else, erase the entire flash and reflash a clean image:

```bash
esptool.py --chip esp32p4 erase_flash
esptool.py --chip esp32p4 -b 460800 write_flash 0x0 \
  archie-gateway-guardian-tab5-merged.bin
```

The next boot must return to the Wi-Fi step. That is the practical proof that
the stored network and service credentials were removed.

## Troubleshooting

| Symptom | Check |
|---|---|
| Wi-Fi fails | Tab5 uses a 2.4 GHz ESP32-C6 radio; verify the password and network band. |
| Hermes fails | Verify `/ws/tab5`, matching token, gateway process, and TLS certificate. |
| OpenClaw fails | Enable the Chat Completions endpoint; use `/v1`, `openclaw/default`, and the gateway token. |
| Direct API fails | Check key scope/credit, exact model ID, DNS, date/time, and HTTPS endpoint. |
| ElevenLabs is silent | Enable Voice, provide both key and voice ID, and raise speaker volume in settings. |
| USB component build error | Run `python3 scripts/patch_usb_component.py` after `idf.py reconfigure`. |
| Blank/wrong-color display | Check the rear label for ILI9881C/GT911, ST7123, or ST7121; update the Tab5 BSP before assuming a damaged panel. |
| No serial port | Use a data cable, enter download mode by holding Reset ~2 seconds until the green LED flashes rapidly, then retry. |

See [M5STACK_DEVELOPER_RESOURCES.md](M5STACK_DEVELOPER_RESOURCES.md) for the
complete M5Stack tool and support map.
