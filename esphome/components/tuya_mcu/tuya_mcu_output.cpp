#include "tuya_mcu_output.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::tuya_mcu {

static const char *const TAG = "tuya_mcu.output";

std::vector<TuyaMcuOutput *> TuyaMcuOutput::instances_;  // NOLINT
uint32_t TuyaMcuOutput::last_send_ = 0;                  // NOLINT
uint32_t TuyaMcuOutput::min_interval_ = 15;              // NOLINT

void TuyaMcuOutput::setup() {
  instances_.push_back(this);
  // MCU (re)started: it lost its state, push ours again.
  this->parent_->add_on_initialized_callback([this]() {
    if (this->has_written_)
      this->mark_dirty_();
  });
  if (!this->dirty_)
    this->disable_loop();
}

void TuyaMcuOutput::dump_config() {
  ESP_LOGCONFIG(TAG, "Tuya MCU output:");
  ESP_LOGCONFIG(TAG, "  Datapoint: %u", this->datapoint_);
  ESP_LOGCONFIG(TAG, "  Max value: %" PRIu32, this->max_value_);
  LOG_FLOAT_OUTPUT(this);
}

void TuyaMcuOutput::write_state(float state) {
  uint32_t v = (uint32_t) lroundf(state * (float) this->max_value_);
  if (v > this->max_value_)
    v = this->max_value_;
  this->value_ = v;
  this->has_written_ = true;
  if (!this->dirty_ && v == this->sent_value_)
    return;  // lights may re-write unchanged values; don't spend UART time on them
  this->mark_dirty_();
}

void TuyaMcuOutput::mark_dirty_() {
  if (!this->dirty_) {
    this->dirty_ = true;
    this->dirty_since_ = millis();
  }
  this->enable_loop();
}

void TuyaMcuOutput::loop() {
  if (!this->dirty_) {
    this->disable_loop();
    return;
  }
  // tuya: suspended (MCU firmware update) or (re)initialising: hold, keep polling
  if (this->parent_->is_idle() || this->parent_->get_init_state() != tuya::TuyaInitState::INIT_DONE)
    return;
  const uint32_t now = millis();
  if (now - last_send_ < min_interval_)
    return;
  // fairness: the output that has been waiting longest goes first
  for (auto *o : instances_) {
    if (o != this && o->dirty_ && (int32_t) (o->dirty_since_ - this->dirty_since_) < 0)
      return;
  }
  this->dirty_ = false;
  last_send_ = now;
  this->sent_value_ = this->value_;
  this->parent_->force_set_integer_datapoint_value(this->datapoint_, this->value_);
  this->disable_loop();
}

}  // namespace esphome::tuya_mcu
