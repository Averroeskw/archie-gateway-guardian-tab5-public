# M5Stack developer resources for Archie

The [M5Stack developer portal](https://m5stack.com/developers) is useful in
four different phases: firmware engineering, installation, recovery, and
public release. This map keeps each resource in the role where it helps.

## Use now

| Resource | How it helps Archie |
|---|---|
| [Tab5 product documentation](https://docs.m5stack.com/en/core/Tab5) | Source of truth for schematics, download mode, factory recovery, pinout, power, peripherals, and production display changes. |
| [Tab5 ESP-IDF UserDemo guide](https://docs.m5stack.com/en/esp_idf/m5tab5/userdemo) | Confirms ESP-IDF 5.4.2 and gives operators an official control build when diagnosing hardware. |
| [M5Tab5-UserDemo source](https://github.com/m5stack/M5Tab5-UserDemo) | Reference implementation for display buffers, rotation, panel detection, touch, Wi-Fi companion, audio, and other BSP behavior. |
| [ESP-IDF Tab5 BSP](https://components.espressif.com/components/espressif/m5stack_tab5) | Production hardware abstraction used by Archie instead of copying board pins and display drivers into the app. |
| [Hardware design files](https://github.com/m5stack/M5_Hardware) | Electrical reference for future battery, microphone, camera, keyboard, RS-485, and expansion work. |
| [I2C address table](https://docs.m5stack.com/en/product_i2c_addr) | Prevents address conflicts when adding sensors or M5 units to the shared buses. |
| [USB drivers](https://docs.m5stack.com/en/download) | Host troubleshooting when a device or serial adapter does not enumerate. |

The official demo is a comparison target, not application code to paste into
Archie. We keep the M5Stack BSP and known-good display pipeline, while the
Archie UI, provisioning, gateway protocol, and branding remain project-owned.

## Installation and recovery ladder

Use the least demanding route first:

1. **Archie browser installer** — primary public path; merged image at offset
   `0x0`, Web Serial, and Improv Wi-Fi provisioning.
2. **GitHub Release + esptool** — universal developer/fallback path with a
   published SHA-256 checksum.
3. **M5Burner** — future M5Stack-native discovery and cross-platform burning
   after public-release approval.
4. **EasyLoader** — optional branded Windows `.exe` generated from the exact
   same merged release image.
5. **ESP-IDF source build** — contributor path and reproducibility proof.
6. **Factory firmware / C6 Wi-Fi restore** — official hardware-control test if
   flashing or networking remains broken.

To force download mode, connect USB, hold Reset for about two seconds until
the internal green LED flashes rapidly, then release. Before transferring a
device, erase the whole flash; reflashing an app alone does not erase NVS
credentials.

## Use at public launch

| Resource | Launch action |
|---|---|
| [M5Burner](https://docs.m5stack.com/en/uiflow/m5burner/intro) | Publish a generic, unprovisioned release after the three-panel test matrix passes. |
| [EasyLoader Packer](https://docs.m5stack.com/en/guide/easyloader/easyloader_packer) | Produce an optional Windows installer from the checksummed merged binary. |
| [M5Stack Project Hub](https://m5stack.com/project-hub?category_id=1&page=1) | Publish the build story, photos/video, setup link, supported panel list, and source license. |
| [M5Stack GitHub](https://github.com/m5stack), [Discord](https://discord.gg/m5stack), and [forum](https://community.m5stack.com/) | Find revision testers and route hardware-specific reports to the right community. |
| [M5Stack release history](https://docs.m5stack.com/en/history) | Watch for Tab5 and keyboard hardware changes that require a BSP or compatibility retest. |

Release packages must contain firmware binaries, checksums, license/source
links, and instructions only. Never package a local `sdkconfig`, `.env`, NVS
partition, Wi-Fi profile, API key, certificate private key, or device token.

## Useful later, not the production firmware path

- **UIFlow2** is useful for teaching, quick peripheral experiments, and a
  support reproduction. The full-screen LVGL gateway firmware stays on
  ESP-IDF for predictable rendering, networking, memory, and release builds.
- **VLW Font Creator** targets M5GFX font assets. Archie currently uses LVGL
  compiled fonts, so switching formats would add complexity; font subsetting
  is still worth revisiting if flash size becomes tight.
- **StackFlow AI and AI accelerator hardware** could become another gateway or
  local inference target later. They are not required for Hermes, OpenClaw,
  OpenAI, Claude, or ElevenLabs onboarding.
- **Product certifications and hardware drawings** become important for a
  commercial enclosure or bundled product, but do not change the open-source
  firmware workflow today.

## Release compatibility gate

Before calling a build generally available:

- boot, touch, landscape rotation, full-screen transitions, Wi-Fi, and one
  gateway request pass on ILI9881C/GT911, ST7123, and ST7121 hardware;
- factory recovery and download-mode instructions are exercised once;
- the release binary is built by CI, checksummed, and secret-scanned;
- a clean-flash onboarding run reaches the console without local files or
  preloaded credentials;
- M5Burner/EasyLoader packages, if offered, are derived from that same release
  binary rather than a developer machine's flash dump.
