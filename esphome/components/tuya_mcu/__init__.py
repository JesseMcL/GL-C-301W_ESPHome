"""tuya_mcu - extras for ESPHome's `tuya:` component.

  output: platform: tuya_mcu   float output backed by an integer datapoint,
                               paced so light transitions don't flood the MCU

The MCU firmware update entity lives in its own repo:
https://github.com/JesseMcL/esphome_tuya_mcu_dfu
"""
import esphome.codegen as cg

CODEOWNERS = ["@JesseMcL"]
DEPENDENCIES = ["tuya"]

tuya_mcu_ns = cg.esphome_ns.namespace("tuya_mcu")
