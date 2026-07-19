# Hardware — M5Stack Tab5

The firmware targets the **M5Stack Tab5**, built around the **ESP32-P4** with
an **ESP32-C6** companion radio.

## At a glance

| Block | Part / detail |
|---|---|
| SoC | ESP32-P4 (dual RISC-V, no built-in Wi-Fi) |
| Wi-Fi/BT radio | ESP32-C6 over SDIO (`esp_wifi_remote` / esp-hosted), 2.4 GHz |
| Display/touch | 5" 1280×720 MIPI-DSI; ILI9881C + GT911, ST7123, or ST7121 by production revision |
| Keyboard | A164-style I2C keyboard module |
| Audio | ES8388 codec + ES7210 echo-cancel mic array, speaker |
| Camera | SC2356-family 2MP MIPI-CSI sensor (optional; unused by this build) |
| Storage | microSD (FAT), 16 MB flash with OTA A/B + coredump |
| Power | INA226-style fuel gauge on I2C (battery %/volts) |

The board support package is the public
[`espressif/m5stack_tab5`](https://components.espressif.com/components/espressif/m5stack_tab5)
component (≥1.2.0), pulled automatically by the build.

## Display revisions and compatibility

M5Stack has shipped three Tab5 display/touch combinations. Check the rear
product sticker before reporting a display failure.

| Production period | Display/touch | Project status |
|---|---|---|
| Original units | ILI9881C + GT911 | **Physically verified** on the development Tab5 |
| From 2025-10-14 | integrated ST7123 | Detected by current BSP; physical verification requested |
| From 2026-04-28 | integrated ST7121 | Detected by current BSP; physical verification requested |

The firmware does not hard-code one panel. The current Tab5 BSP probes the
touch/display controller and selects ILI9881C/GT911, ST7123, or ST7121. A
successful CI build therefore proves source compatibility, but it does not
replace a boot/touch/rotation test on each physical revision. Do not label a
release compatible with all Tab5 units until that three-device matrix passes.

M5Stack warns that older firmware may fail on newer panels; its
[Tab5 product documentation](https://docs.m5stack.com/en/core/Tab5) and the
current [`M5Tab5-UserDemo`](https://github.com/m5stack/M5Tab5-UserDemo) are the
reference points when a new hardware batch appears.

## Wi-Fi: the ESP32-C6 over SDIO

The P4 has no radio; Wi-Fi runs on the C6 over a 4-bit SDIO link. The generic
ESP-hosted P4 pin defaults are **wrong** for the Tab5, so `sdkconfig.defaults`
pins them explicitly:

```
CMD=13  CLK=12  D0=11  D1=10  D2=9  D3=8  reset=15   (SDIO slot 1, 4-bit)
```

`bsp_feature_enable(BSP_FEATURE_WIFI, true)` powers the radio; give it ~500 ms
to settle before the first STA connect.

## Display rendering — the hard-won bits

The firmware honors a few constraints the panel taught us (preserved verbatim
from the original mission-console work):

- **Full-frame double buffering in PSRAM.** This matches the proven
  `fave-build-2` Archie display path for the exact Tab5 panel. It eliminates
  visible 20-line strip updates while native LVGL objects and the bounded
  Archie canvas keep routine dirty regions small.
  ```c
  bsp_display_cfg_t cfg = {
      .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
      .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
      .double_buffer = true,
      .flags = { .buff_dma = true, .buff_spiram = true, .sw_rotate = true },
  };
  ```
- **PPA stays disabled on IDF 5.4.2.** This release predates Espressif's
  ESP32-P4 PPA SRM workaround for DIG-734. Enabling the port/backend on this
  toolchain can hang rotation jobs; the checked-in defaults therefore use
  software rotation until the project upgrades to a fixed IDF release:
  ```text
  # CONFIG_LVGL_PORT_ENABLE_PPA is not set
  # CONFIG_LV_USE_PPA is not set
  ```
- **Animate bounded properties, not the full screen.** The loader moves native
  orbit arcs, tiny stars, streak positions, and a progress width. Large
  container fades and full-screen alpha overlays are intentionally avoided;
  the 3D-derived Archie canvas stays bounded to the console hero tile.
- **One owner of the display, always under the lock.** Every LVGL mutation runs
  while holding `bsp_display_lock(0)` (wait-forever). Producers on other tasks
  (the WebSocket client, the telemetry sampler) marshal through that lock
  before touching widgets. A timed-out lock that silently skips work corrupts
  LVGL state; blocking a producer task is harmless.
- **Touch up front before asserting panel behavior** — the GT911 is on I2C and
  comes up with the BSP.

This pipeline is cross-checked against M5Stack's official
[`M5Tab5-UserDemo`](https://github.com/m5stack/M5Tab5-UserDemo), which uses the
same full-frame/double-buffer/PSRAM/software-rotation flags for RGB565. Keep
PPA opt-in until the project deliberately upgrades and revalidates it; the
Espressif BSP tracker documents current 90°/270° rotation and PPA stability
history in [`esp-bsp#665`](https://github.com/espressif/esp-bsp/issues/665).

## USB component patch

The Tab5 BSP transitively depends on `espressif/usb`, whose 1.x releases call
`usb_dwc_hal_fifo_config_*()` helpers that only exist in IDF ≥ 5.5. On stock
**IDF 5.4.2** the build fails until you run, after `idf.py reconfigure`:

```bash
python3 scripts/patch_usb_component.py
```

It rewrites the single affected block to the legacy `usb_dwc_hal_set_fifo_bias`
call. USB host is never used at runtime (the keyboard is I2C), so this is safe.
The CI workflow runs it automatically.

## Partitions

16 MB flash, OTA A/B (two 4 MB app slots), 2 MB FAT storage, 64 KB coredump —
see [`firmware/partitions.csv`](../firmware/partitions.csv).
