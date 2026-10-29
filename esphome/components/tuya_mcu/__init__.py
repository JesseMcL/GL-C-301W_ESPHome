"""tuya_mcu - extras for ESPHome's `tuya:` component.

Platforms:
  output: platform: tuya_mcu   float output backed by an integer datapoint,
                               paced so light transitions don't flood the MCU
  update: platform: tuya_mcu   MCU firmware update over the standard Tuya MCU
                               upgrade protocol (0x0A / 0x0B), with the image
                               bundled from a release manifest at compile time
"""
import esphome.codegen as cg

CODEOWNERS = ["@JesseMcL"]
DEPENDENCIES = ["tuya"]

tuya_mcu_ns = cg.esphome_ns.namespace("tuya_mcu")
