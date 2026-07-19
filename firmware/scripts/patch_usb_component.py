#!/usr/bin/env python3
"""Make espressif/usb 1.3.0 compile on stock ESP-IDF v5.4.2.

The component's hcd_dwc.c calls usb_dwc_hal_fifo_config_is_valid() /
usb_dwc_hal_set_fifo_config(), which only exist in newer IDF HALs; v5.4.2
ships usb_dwc_hal_set_fifo_bias() instead. Every published 1.x release calls
the new API, so the dependency cannot be solved by pinning. This script
rewrites the single affected block in _port_cmd_reset() to the legacy bias
call. We never use USB host at runtime (the keyboard is I2C); the BSP merely
links the component.

Idempotent: run after `idf.py reconfigure` (locally and in CI), exits 0 if the
patch is already applied, exits 1 loudly if the expected code is missing.
"""
import pathlib
import sys

SRC = pathlib.Path(__file__).resolve().parents[1] / "managed_components/espressif__usb/src/hcd_dwc.c"

NEW_API_BLOCK = """    // Reinitialize port registers
    if (!usb_dwc_hal_fifo_config_is_valid(port->hal, &port->fifo_config)) {
        HCD_EXIT_CRITICAL();
        ret = ESP_ERR_INVALID_SIZE;
        ESP_LOGE(HCD_DWC_TAG, "Invalid FIFO config");
        HCD_ENTER_CRITICAL();
        goto bailout;
    }
    usb_dwc_hal_set_fifo_config(port->hal, &port->fifo_config);// Apply FIFO settings
"""

LEGACY_BLOCK = """    // Reinitialize port registers
    // [tab5 patch] usb_dwc_hal_fifo_config_is_valid()/usb_dwc_hal_set_fifo_config()
    // helpers only exist in IDF >= 5.5; v5.4.2 configures FIFOs via the bias
    // preset instead. USB host is unused by this firmware at runtime.
    usb_dwc_hal_set_fifo_bias(port->hal, USB_HAL_FIFO_BIAS_DEFAULT);
"""


def main() -> int:
    if not SRC.exists():
        print(f"patch_usb_component: {SRC} not found (run idf.py reconfigure first)", file=sys.stderr)
        return 1
    text = SRC.read_text(encoding="utf-8")
    if LEGACY_BLOCK in text:
        print("patch_usb_component: already applied")
        return 0
    if NEW_API_BLOCK not in text:
        print("patch_usb_component: expected block not found - component changed; review needed", file=sys.stderr)
        return 1
    SRC.write_text(text.replace(NEW_API_BLOCK, LEGACY_BLOCK, 1), encoding="utf-8")
    print("patch_usb_component: applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
