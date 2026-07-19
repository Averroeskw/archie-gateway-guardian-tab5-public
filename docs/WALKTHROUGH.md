# Archie Command Centre walkthrough

Archie's particle guardian is kept in the firmware's code-native point table.
Staged onboarding screens are added only after a clean flash so setup values
cannot be published accidentally.

## Capture checklist

After a clean flash, capture the original 1280×720 Tab5 display at these five
stages:

1. Full-screen NVS Nexus boot and orbit loader.
2. Wi-Fi step (use a neutral network name if a value must be visible).
3. Link and credentials (secret fields masked; endpoints replaced with examples).
4. Test complete (no real host, token, key, SSID, or IP in view).
5. Archie Command Centre with point-cloud guardian, LINK telemetry, terminal,
   and command deck.

Disable desktop notifications, crop out serial monitors, and inspect at 200%
before committing. Save privacy-safe files as `01-boot.png` through
`05-command-centre.png` under `assets/screenshots/`.

## Blank-device flow

```text
clean flash -> NVS Nexus -> Wi-Fi -> Link -> Credentials -> Voice -> Test
                                                        -> Archie Command Centre
```

The device stores entered values in local NVS; the installer and repository do
not receive them. Erase flash and reflash a clean merged image to repeat the
walkthrough. The particle render reference is documented separately from
device screenshots.
