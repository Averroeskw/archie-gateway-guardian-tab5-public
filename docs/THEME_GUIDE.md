# Visual system

Archie is a cosmic terminal made for the Tab5's 1280×720 landscape display:
near-black space, warm metallic text, Nexus amber, sparse shooting stars, and
a guardian rendered as live particles rather than a bitmap.

## Palette

| Constant | Hex | Role |
|---|---|---|
| `COL_BG` | `#070605` | deep-space background |
| `COL_PANEL` | `#12100C` | dark glass panel |
| `COL_PANEL_EDGE` | `#3A3022` | metallic edge |
| `COL_TEXT` | `#F2EFE9` | starlight text/particles |
| `COL_DIM` | `#8C8478` | secondary telemetry |
| `COL_SUN` | `#F28A48` | impact and warm nebula |
| `COL_YELLOW` | `#F2C84B` | healthy live state |
| `COL_ALERT` | `#FF5A2A` | fault/stop |
| `COL_AMBER` | `#FFB000` | Nexus primary accent |
| `COL_CYAN` | `#4FD8EB` | alternate persona |

## Particle guardian

[`archie_pointcloud.cpp`](../firmware/main/archie_pointcloud.cpp) renders a
PSRAM-backed RGB565 canvas inside LVGL. Its code-native vertex data lives in
`firmware/main/assets/archie_pointcloud_data.hpp` and is regenerated with:

```bash
cd tools
npm ci
npm run pointcloud -- /private/path/to/archie.glb \
  ../firmware/main/assets/archie_pointcloud_data.hpp \
  --points 5200 --preview /tmp/archie-preview.svg
```

The generator decodes Draco, samples the GLB's real triangles by surface area,
and quantizes only position/depth/glow values. It does not copy the private
model, materials, or textures into the repository. Sampling is deterministic
from the source file's SHA-256 digest, so the same private GLB produces the
same header. At runtime the point set supports scatter-to-condensation, idle
parallax/rotation, colored core pulses, and connection/error states.

## Space layer

The boot background uses tiny native LVGL objects over the complete 1280×720
surface: sparse stars, animated streaks, three orbit arcs, a pulsing Nexus core,
and a narrow progress rail. No full-screen animated canvas is uploaded or
software-rotated. Inside the console, Archie's bounded RGB565 canvas supplies a
denser projected star field while leaving the conversation surface readable.

## UI building blocks

- `make_panel()` creates the dark-glass, metallic-edged panels.
- `make_deck_button()` creates the Nexus command controls.
- `add_scanlines()` remains available for static diagnostic surfaces, but is
  intentionally absent from animated and scrolling screens to avoid a
  full-screen alpha composite on every dirty update.
- `persona_accent()` maps Archie, Hermes, OpenClaw, and custom identities to
  an accent without changing the particle geometry.
- `font_title()`, `font_body()`, `font_mono()`, and `font_small()` keep type
  consistent throughout boot, setup, console, and settings.

## Brand lockup

Primary device lockup:

```text
ARCHIE • NVS-SERIES • GATEWAY GUARDIAN
CREATED BY @AVERROESKW
```

Do not place secrets, SSIDs, IP addresses, or account identifiers in captures
used for documentation. Prefer the fresh-device setup screen or demo values.
