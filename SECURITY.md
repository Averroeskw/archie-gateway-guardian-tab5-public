# Security policy

## Secret-free source

This repository must remain safe to make public without a cleanup commit.
Never commit:

- Wi-Fi SSIDs or passwords;
- API keys, gateway bearer tokens, cookies, or authorization headers;
- private hostnames, public IP addresses, VPN addresses, MAC addresses, or
  account/device identifiers;
- `.env` files, certificates/private keys, NVS dumps, coredumps, build output,
  serial logs, or screenshots containing setup values.

Use placeholders such as `<gateway-token>` and `<provider-key>`. Local `.env`
files and common key/certificate formats are denied by `.gitignore`.

## Device storage

The setup wizard writes credentials to the ESP32's NVS partition. Secret
fields are masked, and the firmware avoids printing their values or the Wi-Fi
SSID. NVS is persistent storage, not a hardware secure element. Anyone with
physical debugging access should be considered capable of extracting it.

Use dedicated, restricted, revocable, low-spend provider keys. Rotate a key
immediately if a device is lost. Before transfer or public demonstration,
erase all flash and install a clean image:

```bash
esptool.py --chip esp32p4 erase_flash
```

## Network exposure

- Use HTTPS/WSS for any route that leaves a trusted private network.
- Keep OpenClaw's gateway on loopback, a LAN, VPN, or tailnet; never expose its
  owner-level bearer token or port directly to the internet.
- Require a strong unique token on the included Hermes adapters.
- Terminate public TLS at a maintained reverse proxy and add rate limits.

## Pre-release audit

Review tracked content and history, not just the working tree:

Run `bash scripts/check_secrets.sh` and `bash scripts/check_secrets.sh --history`,
then inspect `git status --short` before every release.

Also inspect release artifacts, GitHub Actions logs, serial logs, screenshots,
and issue attachments. Automated scans complement human review; they do not
prove a repository contains no secret.

## Reporting

Use GitHub's private vulnerability reporting link in the issue template. Never
open a public issue containing a credential or an unredacted device log.
