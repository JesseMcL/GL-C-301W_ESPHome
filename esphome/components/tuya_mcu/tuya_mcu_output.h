#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/tuya/tuya.h"

namespace esphome::tuya_mcu {

/// Float output backed by an integer (value) datapoint of a Tuya MCU.
///
/// Writes are coalesced: only the latest value is sent, at most one write per
/// `min_interval` across all TuyaMcuOutput instances (oldest pending first),
/// so a 5-channel light transition can't overrun the request/response UART.
/// The MCU smooths between updates with its own fade (DP 106 on the GL-C-301W).
class TuyaMcuOutput : public output::FloatOutput, public Component {
 public:
  void set_tuya_parent(tuya::Tuya *parent) { this->parent_ = parent; }
  void set_datapoint(uint8_t dp) { this->datapoint_ = dp; }
  void set_max_value(uint32_t max) { this->max_value_ = max; }
  void set_min_interval(uint32_t ms) { min_interval_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Hold all writes (used while the MCU firmware is being updated).
  static void set_paused(bool paused);

 protected:
  void write_state(float state) override;
  void mark_dirty_();

  tuya::Tuya *parent_{nullptr};
  uint8_t datapoint_{0};
  uint32_t max_value_{65535};
  uint32_t value_{0};
  uint32_t sent_value_{0xFFFFFFFF};
  uint32_t dirty_since_{0};
  bool dirty_{false};
  bool has_written_{false};

  static std::vector<TuyaMcuOutput *> instances_;  // NOLINT
  static uint32_t last_send_;                       // NOLINT
  static uint32_t min_interval_;                    // NOLINT
  static bool paused_;                              // NOLINT
};

}  // namespace esphome::tuya_mcu
