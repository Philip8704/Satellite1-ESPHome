#pragma once

#ifdef USE_ESP32

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#include "esphome/components/micro_wake_word/streaming_model.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace mww_training_capture {

// Captures short audio snippets whenever a microWakeWord model produces a
// "near-miss": its sliding-window probability climbed into a configurable band
// that's high enough to suggest the user said something close to the wake
// word, but never crossed the real probability_cutoff_ that would have
// triggered the assistant.
//
// Operationally:
//   * a second subscription on the microphone source feeds a circular PCM
//     buffer (pre_buffer_seconds long).
//   * the main loop polls each registered WakeWordModel for its latest
//     sliding-window max probability.
//   * when (max >= near_miss_lower_cutoff) AND (max < probability_cutoff)
//     AND (VAD active, if required) we mark a pending capture, wait
//     post_buffer_ms more samples, then assemble a 16-bit mono 16 kHz WAV
//     and fire the on_near_miss_detected trigger.
//   * the YAML automation is responsible for transporting the WAV — typically
//     an http_request.post to the companion FastAPI service.
class MwwTrainingCapture : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // ------------------------- Configuration setters ------------------------
  void set_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->mww_ = mww; }
  void set_microphone_source(microphone::MicrophoneSource *src) { this->mic_source_ = src; }
  void set_default_lower_cutoff(uint8_t cutoff) { this->default_lower_cutoff_ = cutoff; }
  void set_pre_buffer_seconds(float seconds) { this->pre_buffer_seconds_ = seconds; }
  void set_post_buffer_ms(uint32_t ms) { this->post_buffer_ms_ = ms; }
  void set_cooldown_ms(uint32_t ms) { this->cooldown_ms_ = ms; }
  void set_require_vad(bool require) { this->require_vad_ = require; }
  void set_initial_enabled(bool enabled) { this->enabled_ = enabled; }

  // Register a wake-word model that should participate in near-miss capture.
  // Without an explicit override it inherits ``default_lower_cutoff_``.
  void register_model(micro_wake_word::WakeWordModel *model);
  void add_model_override(micro_wake_word::WakeWordModel *model, uint8_t lower_cutoff);

  // ------------------------- Runtime control ------------------------------
  void enable();
  void disable();
  bool is_enabled() const { return this->enabled_; }

  // ------------------------- YAML accessors -------------------------------
  // Returns the most recently captured WAV blob as a std::string (binary
  // safe). Empty when no capture has been emitted yet. Designed to be passed
  // straight into ``http_request.post.body``.
  std::string get_last_capture_wav_string();
  size_t get_last_capture_size();
  std::string get_last_wake_word();
  float get_last_max_probability();
  float get_last_average_probability();
  uint32_t get_capture_count() const { return this->capture_count_.load(std::memory_order_relaxed); }

  Trigger<std::string, float, float> *get_near_miss_trigger() { return &this->near_miss_trigger_; }

 protected:
  // ------------------------- Audio path -----------------------------------
  void on_audio_data_(const std::vector<uint8_t> &data);
  // ------------------------- Polling logic --------------------------------
  void check_near_misses_();
  void finish_pending_capture_();
  void build_wav_(const std::vector<int16_t> &samples, std::vector<uint8_t> &wav_out);

  // ------------------------- Configuration --------------------------------
  micro_wake_word::MicroWakeWord *mww_{nullptr};
  microphone::MicrophoneSource *mic_source_{nullptr};

  bool enabled_{false};
  bool require_vad_{true};
  float pre_buffer_seconds_{2.0f};
  uint32_t post_buffer_ms_{500};
  uint32_t cooldown_ms_{4000};
  uint8_t default_lower_cutoff_{102};  // ~0.40 quantized

  struct ModelEntry {
    micro_wake_word::WakeWordModel *model;
    uint8_t lower_cutoff;
    uint32_t last_capture_ms;
  };
  std::vector<ModelEntry> models_;

  // ------------------------- Ring buffer ----------------------------------
  // Allocated via RAMAllocator so PSRAM is preferred on the ESP32-S3.
  // Layout: int16 samples, length = pre_buffer_seconds * sample_rate +
  // post_buffer_ms * sample_rate / 1000 + headroom.
  int16_t *ring_{nullptr};
  size_t ring_capacity_{0};
  // Producer state — written by the audio callback only.
  std::atomic<uint64_t> total_samples_written_{0};
  std::atomic<size_t> ring_head_{0};
  uint32_t sample_rate_{16000};
  bool ring_initialised_{false};
  uint32_t last_debug_log_ms_{0};

  // ------------------------- Pending capture state ------------------------
  bool capture_pending_{false};
  uint64_t capture_target_total_{0};
  uint32_t capture_queued_ms_{0};
  std::string capture_wake_word_;
  uint8_t capture_max_prob_{0};
  uint8_t capture_avg_prob_{0};

  // ------------------------- Last completed capture -----------------------
  std::vector<uint8_t> last_capture_wav_;
  std::string last_wake_word_;
  uint8_t last_max_prob_{0};
  uint8_t last_avg_prob_{0};
  std::atomic<uint32_t> capture_count_{0};

  Trigger<std::string, float, float> near_miss_trigger_;
};

// ------------------------- Automation glue --------------------------------
template<typename... Ts> class EnableAction : public Action<Ts...>, public Parented<MwwTrainingCapture> {
 public:
  void play(const Ts &...x) override { this->parent_->enable(); }
};
template<typename... Ts> class DisableAction : public Action<Ts...>, public Parented<MwwTrainingCapture> {
 public:
  void play(const Ts &...x) override { this->parent_->disable(); }
};
template<typename... Ts> class IsEnabledCondition : public Condition<Ts...>, public Parented<MwwTrainingCapture> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_enabled(); }
};

}  // namespace mww_training_capture
}  // namespace esphome

#endif  // USE_ESP32
