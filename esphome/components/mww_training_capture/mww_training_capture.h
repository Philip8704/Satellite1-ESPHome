#pragma once

#ifdef USE_ESP32

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"
#include "esphome/components/micro_wake_word/streaming_model.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace esphome {
namespace mww_training_capture {

// How many recent quantized probabilities are retained per model to build the
// X-Probability-History header. Sampled from the main loop (~16 ms) against a
// model that produces a new probability roughly every 30 ms, so 32 entries is
// ~0.5-1 s of context — comfortably longer than a wake-word utterance.
static constexpr size_t PROBABILITY_HISTORY_LEN = 32;

// Which condition produced a capture. Maps directly onto the trainer's
// X-Event-Type header, where wake_detected clips are candidate positive
// training samples and close_miss clips are candidate negatives.
enum class CaptureEvent : uint8_t {
  CLOSE_MISS = 0,
  WAKE_DETECTED = 1,
};

// Everything the upload task needs to describe one capture. Populated on the
// main loop at staging time and then treated as immutable for the lifetime of
// the upload — the upload runs on a separate FreeRTOS task while the audio
// task keeps mutating the history ring, so anything derived from live state
// (the history CSV, the filename) is pre-serialized here rather than formatted
// inside upload_wav_().
struct CaptureMeta {
  CaptureEvent event{CaptureEvent::CLOSE_MISS};
  std::string wake_word;
  uint8_t max_prob{0};
  uint8_t avg_prob{0};
  uint8_t prob_cutoff{0};
  uint8_t vad_max_prob{0};
  uint8_t vad_avg_prob{0};
  bool blocked_by_vad{false};
  uint16_t active_windows{0};
  uint8_t rise_score{0};
  std::string prob_history_csv;
  std::string original_name;
  uint32_t seq{0};
};

// Captures short audio snippets around microWakeWord activity and streams them
// to a TaterTotterson microWakeWord trainer at /api/upload_captured_audio_raw.
//
// Two capture events:
//   * CLOSE_MISS   — the sliding-window probability climbed into a configurable
//                    band high enough to suggest the user said something close
//                    to the wake word, but never crossed the real
//                    probability_cutoff_ that would have triggered the
//                    assistant. Candidate negative training sample.
//   * WAKE_DETECTED — MWW actually fired. Candidate positive training sample.
//
// Operationally:
//   * a second subscription on the microphone source feeds a circular PCM
//     buffer (pre_buffer_seconds long).
//   * the main loop polls each registered WakeWordModel for its latest
//     sliding-window max probability, both to detect near-misses and to
//     maintain the per-model probability history ring.
//   * real detections arrive via MicroWakeWord::add_detection_callback, which
//     carries the probabilities snapshotted before MWW reset them.
//   * on either event we mark a pending capture, wait for the post-buffer,
//     copy that slice to a stable PSRAM buffer, then stream a 16-bit mono WAV.
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
  void set_upload_url(const std::string &url) { this->upload_url_ = url; }
  void set_device_name(const std::string &device_name) { this->device_name_ = device_name; }
  void set_audio_format_header(const std::string &fmt) { this->audio_format_header_ = fmt; }
  void set_detection_profile(const std::string &profile) { this->detection_profile_ = profile; }
  void set_notes(const std::string &notes) { this->notes_ = notes; }
  void set_capture_wake_detected(bool enabled) { this->capture_wake_detected_ = enabled; }
  void set_capture_close_miss(bool enabled) { this->capture_close_miss_ = enabled; }
  void set_detection_cooldown_ms(uint32_t ms) { this->detection_cooldown_ms_ = ms; }
  void set_detection_post_buffer_ms(uint32_t ms) { this->detection_post_buffer_ms_ = ms; }

  // Register a wake-word model that should participate in capture.
  // Without an explicit override it inherits ``default_lower_cutoff_``.
  void register_model(micro_wake_word::WakeWordModel *model);
  void add_model_override(micro_wake_word::WakeWordModel *model, uint8_t lower_cutoff);

  // ------------------------- Runtime control ------------------------------
  void enable();
  void disable();
  bool is_enabled() const { return this->enabled_; }

  // ------------------------- YAML accessors -------------------------------
  std::string get_last_wake_word();
  std::string get_last_event_type();
  float get_last_max_probability();
  float get_last_average_probability();
  uint32_t get_capture_count() const { return this->capture_count_.load(std::memory_order_relaxed); }
  uint32_t get_dropped_count() const { return this->capture_drop_count_.load(std::memory_order_relaxed); }
  uint32_t get_detection_capture_count() const {
    return this->detection_capture_count_.load(std::memory_order_relaxed);
  }
  uint32_t get_close_miss_capture_count() const {
    return this->close_miss_capture_count_.load(std::memory_order_relaxed);
  }

  // Fires only for CLOSE_MISS captures, preserving the historical contract.
  Trigger<std::string, float, float> *get_near_miss_trigger() { return &this->near_miss_trigger_; }
  // Fires for every successful upload; ``event_type`` is "wake_detected" or "close_miss".
  Trigger<std::string, std::string, float, float> *get_capture_uploaded_trigger() {
    return &this->capture_uploaded_trigger_;
  }

 protected:
  struct ModelEntry {
    micro_wake_word::WakeWordModel *model{nullptr};
    uint8_t lower_cutoff{0};
    uint32_t last_capture_ms{0};            // CLOSE_MISS cooldown
    uint32_t last_detection_capture_ms{0};  // WAKE_DETECTED cooldown (independent)
    std::array<uint8_t, PROBABILITY_HISTORY_LEN> history{};
    uint8_t history_head{0};
    uint8_t history_count{0};
  };

  // ------------------------- Audio path -----------------------------------
  void on_audio_data_(const std::vector<uint8_t> &data);
  // ------------------------- Detection hook -------------------------------
  void on_detection_event_(const micro_wake_word::DetectionEvent &event);
  // ------------------------- Polling logic --------------------------------
  void check_near_misses_();
  void update_probability_history_();
  void stage_capture_(ModelEntry &entry, CaptureEvent event, uint8_t max_prob, uint8_t avg_prob, bool blocked_by_vad,
                      uint32_t now);
  void finish_pending_capture_();
  void finalize_upload_if_finished_();
  void start_upload_task_(size_t sample_count);
  static void upload_task_(void *arg);
  bool upload_wav_(const int16_t *samples, size_t sample_count);

  // ------------------------- Metadata helpers -----------------------------
  ModelEntry make_entry_(micro_wake_word::WakeWordModel *model, uint8_t lower_cutoff) const;
  std::string serialize_history_(const ModelEntry &entry) const;
  uint16_t count_active_windows_(const ModelEntry &entry, uint8_t cutoff) const;
  uint8_t compute_rise_score_(const ModelEntry &entry) const;
  std::string build_original_name_(const CaptureMeta &meta) const;
  static const char *event_type_str_(CaptureEvent event);

  // ------------------------- Configuration --------------------------------
  micro_wake_word::MicroWakeWord *mww_{nullptr};
  microphone::MicrophoneSource *mic_source_{nullptr};

  bool enabled_{false};
  bool require_vad_{true};
  bool capture_wake_detected_{true};
  bool capture_close_miss_{true};
  std::string upload_url_;
  std::string device_name_{"unknown_device"};
  std::string audio_format_header_{"wav"};
  std::string detection_profile_;
  std::string notes_;
  float pre_buffer_seconds_{2.0f};
  uint32_t post_buffer_ms_{500};
  uint32_t detection_post_buffer_ms_{250};
  uint32_t cooldown_ms_{4000};
  uint32_t detection_cooldown_ms_{1500};
  uint8_t default_lower_cutoff_{102};  // ~0.40 quantized

  std::vector<ModelEntry> models_;

  // ------------------------- Ring buffer ----------------------------------
  // Allocated via RAMAllocator so PSRAM is preferred on the ESP32-S3.
  // Layout: int16 samples, length = pre_buffer_seconds * sample_rate +
  // post_buffer_ms * sample_rate / 1000 + headroom.
  int16_t *ring_{nullptr};
  size_t ring_capacity_{0};
  int16_t *capture_buffer_{nullptr};
  size_t capture_buffer_capacity_{0};
  // Producer state — written by the audio callback only.
  std::atomic<uint64_t> total_samples_written_{0};
  std::atomic<size_t> ring_head_{0};
  uint32_t sample_rate_{16000};
  bool ring_initialised_{false};
  uint32_t last_debug_log_ms_{0};
  uint32_t audio_debug_count_{0};

  // ------------------------- Pending capture state ------------------------
  bool capture_pending_{false};
  uint64_t capture_target_total_{0};
  uint64_t capture_post_samples_{0};  // per-event post-roll, staged with the capture
  uint32_t capture_queued_ms_{0};
  uint32_t capture_max_wait_ms_{0};
  // Read by the upload task. Safe only because ``capture_pending_`` blocks new staging until
  // finalize_upload_if_finished_() runs, and the detection path refuses to touch this once
  // ``capture_upload_started_`` is true. Do not relax either guard.
  CaptureMeta capture_meta_;
  bool capture_upload_started_{false};
  size_t capture_upload_sample_count_{0};
  uint32_t capture_upload_started_ms_{0};
  std::atomic<bool> capture_upload_finished_{false};
  std::atomic<bool> capture_upload_success_{false};
  TaskHandle_t capture_upload_task_{nullptr};

  // ------------------------- Last completed capture -----------------------
  CaptureMeta last_meta_;
  std::atomic<uint32_t> capture_count_{0};
  std::atomic<uint32_t> detection_capture_count_{0};
  std::atomic<uint32_t> close_miss_capture_count_{0};
  std::atomic<uint32_t> capture_sequence_{0};
  // Counts captures that were staged but failed to upload (HTTP error, ring
  // overrun, missing endpoint, slot contention, etc.). Surfaced as a HA
  // diagnostic sensor so silent upload failures don't masquerade as
  // "no wake-word activity".
  std::atomic<uint32_t> capture_drop_count_{0};

  Trigger<std::string, float, float> near_miss_trigger_;
  Trigger<std::string, std::string, float, float> capture_uploaded_trigger_;
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
