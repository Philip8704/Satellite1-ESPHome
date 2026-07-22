#include "mww_training_capture.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_err.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstring>

namespace esphome {
namespace mww_training_capture {

static const char *const TAG = "mww_training_capture";

void MwwTrainingCapture::setup() {
  if (this->mww_ == nullptr) {
    ESP_LOGE(TAG, "micro_wake_word reference is null; capture disabled");
    this->mark_failed();
    return;
  }
  if (this->mic_source_ == nullptr) {
    ESP_LOGE(TAG, "microphone source is null; capture disabled");
    this->mark_failed();
    return;
  }

  this->sample_rate_ = this->mic_source_->get_audio_stream_info().get_sample_rate();
  if (this->sample_rate_ == 0) {
    this->sample_rate_ = 16000;
  }

  // Allocate the ring buffer so it can hold the full pre-buffer plus the
  // largest post-buffer of either event type plus a small headroom. Total
  // samples fits comfortably in PSRAM on the Sat1 ESP32-S3 (~96 kB at 3 s).
  const uint32_t max_post_buffer_ms = std::max(this->post_buffer_ms_, this->detection_post_buffer_ms_);
  const float total_seconds = this->pre_buffer_seconds_ + (max_post_buffer_ms / 1000.0f) + 0.5f;
  this->ring_capacity_ = static_cast<size_t>(total_seconds * this->sample_rate_);
  if (this->ring_capacity_ < this->sample_rate_) {
    this->ring_capacity_ = this->sample_rate_;  // hard floor of 1 s
  }
  const uint64_t configured_capture_samples =
      (uint64_t)(this->pre_buffer_seconds_ * this->sample_rate_) +
      ((uint64_t) max_post_buffer_ms * this->sample_rate_ / 1000ULL);
  this->capture_buffer_capacity_ = (size_t) std::min<uint64_t>(
      std::max<uint64_t>(configured_capture_samples, 1), (uint64_t) this->ring_capacity_);

  RAMAllocator<int16_t> alloc;
  this->ring_ = alloc.allocate(this->ring_capacity_);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-sample ring buffer", (unsigned) this->ring_capacity_);
    this->mark_failed();
    return;
  }
  std::memset(this->ring_, 0, this->ring_capacity_ * sizeof(int16_t));
  this->capture_buffer_ = alloc.allocate(this->capture_buffer_capacity_);
  if (this->capture_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-sample capture buffer", (unsigned) this->capture_buffer_capacity_);
    this->mark_failed();
    return;
  }
  this->ring_initialised_ = true;

  // Subscribe to the same microphone source as MWW. ESPHome's MicrophoneSource
  // uses a CallbackManager so multiple subscribers get the same audio frames —
  // MWW is the one that starts/stops the underlying mic, our callback simply
  // piggybacks. That gives us the desirable property that we stop capturing
  // automatically whenever MWW pauses (e.g. while voice_assistant is active).
  this->mww_->add_audio_callback([this](const std::vector<uint8_t> &data) { this->on_audio_data_(data); });

  // Subscribe to real detections. The event carries the probabilities as they were when the
  // detection fired — polling the model here would read zeroes, because MWW calls
  // reset_probabilities() on the inference task before this ever reaches the main loop.
  this->mww_->add_detection_callback(
      [this](const micro_wake_word::DetectionEvent &event) { this->on_detection_event_(event); });

  // Pre-register every non-internal model that's exposed by MWW unless the
  // user explicitly listed models in YAML. This keeps the "drop-in capture"
  // case zero-config — registering models via YAML overrides this.
  if (this->models_.empty()) {
    for (auto *model : this->mww_->get_wake_words()) {
      if (model == nullptr || model->get_internal_only())
        continue;
      this->models_.push_back(this->make_entry_(model, this->default_lower_cutoff_));
    }
  }

  ESP_LOGI(TAG, "Training capture ready (ring=%u samples / %.1f s, capture=%u samples, models=%u, enabled=%s)",
           (unsigned) this->ring_capacity_, total_seconds, (unsigned) this->capture_buffer_capacity_,
           (unsigned) this->models_.size(),
           this->enabled_ ? "yes" : "no");
}

MwwTrainingCapture::ModelEntry MwwTrainingCapture::make_entry_(micro_wake_word::WakeWordModel *model,
                                                               uint8_t lower_cutoff) const {
  ModelEntry entry;
  entry.model = model;
  entry.lower_cutoff = lower_cutoff;
  return entry;
}

void MwwTrainingCapture::dump_config() {
  ESP_LOGCONFIG(TAG, "MWW Training Capture:");
  ESP_LOGCONFIG(TAG, "  Pre-buffer: %.2f s", this->pre_buffer_seconds_);
  ESP_LOGCONFIG(TAG, "  Post-buffer: %u ms (close-miss) / %u ms (detection)", (unsigned) this->post_buffer_ms_,
                (unsigned) this->detection_post_buffer_ms_);
  ESP_LOGCONFIG(TAG, "  Cooldown: %u ms (close-miss) / %u ms (detection)", (unsigned) this->cooldown_ms_,
                (unsigned) this->detection_cooldown_ms_);
  ESP_LOGCONFIG(TAG, "  Capture events: close-miss=%s detection=%s", this->capture_close_miss_ ? "yes" : "no",
                this->capture_wake_detected_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Require VAD (close-miss only): %s", this->require_vad_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Upload URL: %s", this->upload_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Audio format: %s", this->audio_format_header_.c_str());
  ESP_LOGCONFIG(TAG, "  Source device: %s", this->device_name_.c_str());
  if (!this->detection_profile_.empty()) {
    ESP_LOGCONFIG(TAG, "  Detection profile: %s", this->detection_profile_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  Default lower cutoff: %.2f", this->default_lower_cutoff_ / 255.0f);
  ESP_LOGCONFIG(TAG, "  Models registered: %u", (unsigned) this->models_.size());
  for (auto &m : this->models_) {
    if (m.model != nullptr) {
      ESP_LOGCONFIG(TAG, "    - %s lower=%.2f upper=%.2f window=%u", m.model->get_wake_word().c_str(),
                    m.lower_cutoff / 255.0f, m.model->get_probability_cutoff() / 255.0f,
                    (unsigned) m.model->get_sliding_window_size());
    }
  }
  ESP_LOGCONFIG(TAG, "  Enabled at boot: %s", this->enabled_ ? "yes" : "no");
}

void MwwTrainingCapture::register_model(micro_wake_word::WakeWordModel *model) {
  if (model == nullptr)
    return;
  for (auto &existing : this->models_) {
    if (existing.model == model) {
      return;  // already registered
    }
  }
  this->models_.push_back(this->make_entry_(model, this->default_lower_cutoff_));
}

void MwwTrainingCapture::add_model_override(micro_wake_word::WakeWordModel *model, uint8_t lower_cutoff) {
  if (model == nullptr)
    return;
  for (auto &existing : this->models_) {
    if (existing.model == model) {
      existing.lower_cutoff = lower_cutoff;
      return;
    }
  }
  this->models_.push_back(this->make_entry_(model, lower_cutoff));
}

void MwwTrainingCapture::enable() {
  this->enabled_ = true;
  ESP_LOGI(TAG, "Training capture enabled");
}

void MwwTrainingCapture::disable() {
  this->enabled_ = false;
  ESP_LOGI(TAG, "Training capture disabled");
}

void MwwTrainingCapture::on_audio_data_(const std::vector<uint8_t> &data) {
  if (!this->ring_initialised_ || data.empty())
    return;

  const int16_t *samples = reinterpret_cast<const int16_t *>(data.data());
  const size_t sample_count = data.size() / sizeof(int16_t);
  if (sample_count == 0)
    return;
  if (this->audio_debug_count_ < 5) {
    ESP_LOGD(TAG, "Audio tap received %u bytes / %u samples", (unsigned) data.size(), (unsigned) sample_count);
    this->audio_debug_count_++;
  }

  // Single producer (audio thread) writes; main loop only reads. The ring
  // head is published with release semantics so the consumer's acquire load
  // sees the freshly written samples.
  size_t head = this->ring_head_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < sample_count; ++i) {
    this->ring_[head] = samples[i];
    head++;
    if (head >= this->ring_capacity_) {
      head = 0;
    }
  }
  this->ring_head_.store(head, std::memory_order_release);
  this->total_samples_written_.fetch_add(sample_count, std::memory_order_acq_rel);
}

void MwwTrainingCapture::loop() {
  if (this->is_failed() || !this->ring_initialised_)
    return;
  this->check_near_misses_();
  if (this->capture_pending_)
    this->finish_pending_capture_();
}

void MwwTrainingCapture::update_probability_history_() {
  for (auto &entry : this->models_) {
    if (entry.model == nullptr || !entry.model->is_enabled())
      continue;

    const uint8_t prob = entry.model->get_last_max_probability();

    // Run-length-suppress zeroes. After a detection MWW forces ~100 slices of zeroed
    // probabilities, and silence produces zeroes indefinitely — without this the 32-slot window
    // degenerates into a wall of "0,0,0,..." and the trainer's probability trace is useless.
    // Keeping one leading zero preserves the visible rise out of silence.
    if (prob == 0 && entry.history_count > 0) {
      const size_t newest_idx = (entry.history_head + PROBABILITY_HISTORY_LEN - 1) % PROBABILITY_HISTORY_LEN;
      if (entry.history[newest_idx] == 0)
        continue;
    }

    entry.history[entry.history_head] = prob;
    entry.history_head = (uint8_t) ((entry.history_head + 1) % PROBABILITY_HISTORY_LEN);
    if (entry.history_count < PROBABILITY_HISTORY_LEN)
      entry.history_count++;
  }
}

std::string MwwTrainingCapture::serialize_history_(const ModelEntry &entry) const {
  std::string out;
  if (entry.history_count == 0)
    return out;
  out.reserve(entry.history_count * 4);

  // Walk oldest -> newest.
  const size_t start = (entry.history_head + PROBABILITY_HISTORY_LEN - entry.history_count) % PROBABILITY_HISTORY_LEN;
  char buf[8];
  for (size_t i = 0; i < entry.history_count; ++i) {
    const uint8_t value = entry.history[(start + i) % PROBABILITY_HISTORY_LEN];
    const int written = snprintf(buf, sizeof(buf), "%u", (unsigned) value);
    if (written <= 0)
      continue;
    if (!out.empty())
      out.push_back(',');
    out.append(buf, (size_t) written);
  }
  return out;
}

uint16_t MwwTrainingCapture::count_active_windows_(const ModelEntry &entry, uint8_t cutoff) const {
  // NOTE: this is a PROXY, not something microWakeWord computes. ESPHome detects via a
  // sliding-window MEAN test (sum > cutoff * window), never a count of windows above threshold.
  // The value is monotone in signal strength, so it is useful for sorting and eyeballing in a
  // review UI, but its absolute magnitude depends on our main-loop poll rate. Do not feed it to
  // anything that makes automatic accept/reject decisions.
  uint16_t count = 0;
  if (entry.history_count == 0)
    return count;
  const size_t start = (entry.history_head + PROBABILITY_HISTORY_LEN - entry.history_count) % PROBABILITY_HISTORY_LEN;
  for (size_t i = 0; i < entry.history_count; ++i) {
    if (entry.history[(start + i) % PROBABILITY_HISTORY_LEN] >= cutoff)
      count++;
  }
  return count;
}

uint8_t MwwTrainingCapture::compute_rise_score_(const ModelEntry &entry) const {
  // Magnitude of the climb leading to the peak: max(history) - min(history[0 .. argmax]).
  // Also an invented metric with no upstream microWakeWord equivalent — display only.
  if (entry.history_count == 0)
    return 0;
  const size_t start = (entry.history_head + PROBABILITY_HISTORY_LEN - entry.history_count) % PROBABILITY_HISTORY_LEN;

  uint8_t peak = 0;
  size_t peak_i = 0;
  for (size_t i = 0; i < entry.history_count; ++i) {
    const uint8_t value = entry.history[(start + i) % PROBABILITY_HISTORY_LEN];
    if (value > peak) {
      peak = value;
      peak_i = i;
    }
  }

  uint8_t trough = peak;
  for (size_t i = 0; i <= peak_i; ++i) {
    const uint8_t value = entry.history[(start + i) % PROBABILITY_HISTORY_LEN];
    if (value < trough)
      trough = value;
  }
  return (uint8_t) (peak - trough);
}

std::string MwwTrainingCapture::build_original_name_(const CaptureMeta &meta) const {
  // Sanitize hard: this ends up in an HTTP header, and the wake word comes from a user-supplied
  // model manifest. A CR/LF or a non-ASCII byte here would corrupt the request.
  std::string wake;
  wake.reserve(meta.wake_word.size());
  for (char c : meta.wake_word) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      wake.push_back(c);
    } else if (c >= 'A' && c <= 'Z') {
      wake.push_back((char) (c - 'A' + 'a'));
    } else {
      wake.push_back('_');
    }
    if (wake.size() >= 32)
      break;
  }

  char buf[160];
  snprintf(buf, sizeof(buf), "%s_%s_%s_%06u.wav", this->device_name_.c_str(), wake.c_str(), event_type_str_(meta.event),
           (unsigned) meta.seq);
  return std::string(buf);
}

const char *MwwTrainingCapture::event_type_str_(CaptureEvent event) {
  return event == CaptureEvent::WAKE_DETECTED ? "wake_detected" : "close_miss";
}

void MwwTrainingCapture::stage_capture_(ModelEntry &entry, CaptureEvent event, uint8_t max_prob, uint8_t avg_prob,
                                        bool blocked_by_vad, uint32_t now) {
  const uint64_t total_written = this->total_samples_written_.load(std::memory_order_acquire);
  const uint32_t post_ms =
      (event == CaptureEvent::WAKE_DETECTED) ? this->detection_post_buffer_ms_ : this->post_buffer_ms_;
  const uint64_t post_samples = (uint64_t) post_ms * this->sample_rate_ / 1000ULL;

  this->capture_pending_ = true;
  this->capture_post_samples_ = post_samples;
  this->capture_target_total_ = total_written + post_samples;
  this->capture_queued_ms_ = now;
  this->capture_max_wait_ms_ = post_ms + 1000;

  CaptureMeta meta;
  meta.event = event;
  meta.wake_word = entry.model->get_wake_word();
  meta.max_prob = max_prob;
  meta.avg_prob = avg_prob;
  meta.prob_cutoff = entry.model->get_probability_cutoff();
  meta.blocked_by_vad = blocked_by_vad;
  meta.active_windows = this->count_active_windows_(entry, meta.prob_cutoff);
  meta.rise_score = this->compute_rise_score_(entry);
  // Serialize on the main loop: the upload task must never read the live history ring.
  meta.prob_history_csv = this->serialize_history_(entry);
  meta.seq = this->capture_sequence_.fetch_add(1, std::memory_order_relaxed);
#ifdef USE_MICRO_WAKE_WORD_VAD
  if (auto *vad = this->mww_->get_vad_model()) {
    meta.vad_max_prob = vad->get_last_max_probability();
    meta.vad_avg_prob = vad->get_last_average_probability();
  }
#endif
  meta.original_name = this->build_original_name_(meta);
  this->capture_meta_ = std::move(meta);

  ESP_LOGD(TAG, "%s queued: %s max=%.2f avg=%.2f (cutoff=%.2f, vad_blocked=%s, samples=%llu target=%llu)",
           event_type_str_(event), this->capture_meta_.wake_word.c_str(), max_prob / 255.0f, avg_prob / 255.0f,
           this->capture_meta_.prob_cutoff / 255.0f, blocked_by_vad ? "yes" : "no",
           (unsigned long long) total_written, (unsigned long long) this->capture_target_total_);
}

void MwwTrainingCapture::on_detection_event_(const micro_wake_word::DetectionEvent &event) {
  if (!this->enabled_ || !this->capture_wake_detected_ || this->mww_ == nullptr)
    return;
  if (event.wake_word == nullptr)
    return;
  // Deliberately NOT gated on require_vad_: a detection either already passed VAD, or was
  // explicitly blocked by it — and VAD-blocked detections are among the most valuable samples,
  // because they are exactly the false negatives you want to retrain away.

  const uint32_t now = millis();
  for (auto &entry : this->models_) {
    if (entry.model == nullptr || entry.model->get_wake_word() != *event.wake_word)
      continue;
    if (entry.last_detection_capture_ms != 0 && (now - entry.last_detection_capture_ms) < this->detection_cooldown_ms_)
      return;

    if (this->capture_pending_ && this->capture_upload_started_) {
      // The slot is genuinely busy — a previous capture is mid-upload. Dropping the positive
      // sample is unfortunate but the alternative is corrupting the in-flight one.
      ESP_LOGW(TAG, "Detection capture dropped: previous upload still in flight");
      this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (this->capture_pending_) {
      // A close-miss was staged moments ago for the same utterance — the probability necessarily
      // climbs through the near-miss band on its way to a real detection, so this is the common
      // case, not the exception. Re-label it in place: the audio slice is identical, only the
      // event type and metadata differ. Without this the positive sample would be both dropped
      // AND mislabeled as a negative, which is the worst outcome for a training pipeline.
      ESP_LOGD(TAG, "Re-labelling pending close-miss as wake_detected for '%s'", event.wake_word->c_str());
    }

    this->stage_capture_(entry, CaptureEvent::WAKE_DETECTED, event.max_probability, event.average_probability,
                         event.blocked_by_vad, now);
    entry.last_detection_capture_ms = now;
    // Suppress a redundant close-miss immediately following this detection.
    entry.last_capture_ms = now;
    return;
  }
}

void MwwTrainingCapture::check_near_misses_() {
  if (!this->enabled_ || this->mww_ == nullptr || !this->mww_->is_running())
    return;

  // Maintain the history ring unconditionally — including while a capture is pending and while
  // VAD is quiet — so the trace covers the run-up to whatever fires next.
  this->update_probability_history_();

  if (!this->capture_close_miss_ || this->capture_pending_)
    return;

#ifdef USE_MICRO_WAKE_WORD_VAD
  const bool vad_active = this->mww_->get_vad_state();
#else
  const bool vad_active = true;
#endif
  if (this->require_vad_ && !vad_active)
    return;

  const uint32_t now = millis();

  for (auto &entry : this->models_) {
    if (entry.model == nullptr)
      continue;
    if (!entry.model->is_enabled())
      continue;
    if (entry.last_capture_ms != 0 && (now - entry.last_capture_ms) < this->cooldown_ms_)
      continue;

    const uint8_t max_prob = entry.model->get_last_max_probability();
    const uint8_t avg_prob = entry.model->get_last_average_probability();
    const uint8_t real_cutoff = entry.model->get_probability_cutoff();
    // Near-miss band: [lower_cutoff, real_cutoff). If VAD is inactive, also
    // keep scores above the real cutoff because MWW itself will not fire.
    if (max_prob < entry.lower_cutoff)
      continue;
    if (max_prob >= real_cutoff && vad_active)
      continue;

    this->stage_capture_(entry, CaptureEvent::CLOSE_MISS, max_prob, avg_prob, /*blocked_by_vad=*/false, now);
    entry.last_capture_ms = now;
    return;  // one capture at a time
  }
}

void MwwTrainingCapture::finish_pending_capture_() {
  if (this->capture_upload_started_) {
    this->finalize_upload_if_finished_();
    return;
  }

  const uint64_t total_written = this->total_samples_written_.load(std::memory_order_acquire);
  if (total_written < this->capture_target_total_) {
    const uint32_t now = millis();
    // Stop waiting the moment MWW stops. voice_assistant stops micro_wake_word as soon as the
    // pipeline starts, and MWW returns from its mic callback without fanning out audio while
    // stopped — so the post-buffer samples will NEVER arrive after a real detection. Without this
    // every wake_detected capture would burn the full timeout before uploading. The pre-roll
    // already contains the wake word, so finishing short is correct rather than degraded.
    if (this->mww_ != nullptr && !this->mww_->is_running()) {
      ESP_LOGD(TAG, "MWW stopped mid-capture; finishing with %llu of %llu samples",
               (unsigned long long) total_written, (unsigned long long) this->capture_target_total_);
      this->capture_target_total_ = total_written;
    } else if (now - this->capture_queued_ms_ < this->capture_max_wait_ms_) {
      if (now - this->last_debug_log_ms_ > 1000) {
        ESP_LOGD(TAG, "Waiting for capture post-buffer: samples=%llu target=%llu",
                 (unsigned long long) total_written, (unsigned long long) this->capture_target_total_);
        this->last_debug_log_ms_ = now;
      }
      return;
    } else {
      ESP_LOGW(TAG, "Capture post-buffer timed out: samples=%llu target=%llu; finishing with available audio",
               (unsigned long long) total_written, (unsigned long long) this->capture_target_total_);
      this->capture_target_total_ = total_written;
    }
  }

  if (this->capture_target_total_ == 0) {
    ESP_LOGW(TAG, "Capture had no audio samples; dropping");
    this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
    this->capture_pending_ = false;
    return;
  }

  // Compute slice we want: [target - (pre+post) samples, target). We copy the
  // slice into a persistent PSRAM buffer before upload so Wi-Fi latency cannot
  // race the live audio ring and corrupt the beginning of the capture.
  const uint64_t pre_samples = (uint64_t)(this->pre_buffer_seconds_ * this->sample_rate_);
  // Use the post-roll staged with THIS capture, not the close-miss default — the two event types
  // have different post buffers and recomputing here would over-reach into the pre-roll.
  const uint64_t post_samples = this->capture_post_samples_;
  const uint64_t total_slice = pre_samples + post_samples;
  const uint64_t slice_len = std::min<uint64_t>(std::min<uint64_t>(total_slice, (uint64_t) this->ring_capacity_),
                                               std::min<uint64_t>((uint64_t) this->capture_buffer_capacity_,
                                                                  this->capture_target_total_));
  const uint64_t start_sample = this->capture_target_total_ - slice_len;
  if (start_sample > this->capture_target_total_) {
    // Underflow guard
    this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
    this->capture_pending_ = false;
    return;
  }

  // Find the ring index that corresponds to ``start_sample``. ring_head is
  // where the next write will go, so the most-recent sample lives at
  // (ring_head - 1) % capacity. Walk back ``total_written - start_sample``
  // positions from there.
  const size_t head = this->ring_head_.load(std::memory_order_acquire);
  const uint64_t back_off = total_written - start_sample;
  if (back_off > (uint64_t) this->ring_capacity_) {
    // We waited too long — start point already overwritten.
    ESP_LOGW(TAG, "Capture slice was overrun before assembly; dropping");
    this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
    this->capture_pending_ = false;
    return;
  }
  size_t start_idx = (head + this->ring_capacity_ - (size_t) back_off) % this->ring_capacity_;

  for (size_t i = 0; i < (size_t) slice_len; ++i) {
    this->capture_buffer_[i] = this->ring_[(start_idx + i) % this->ring_capacity_];
  }

  this->start_upload_task_((size_t) slice_len);
}

void MwwTrainingCapture::finalize_upload_if_finished_() {
  if (!this->capture_upload_finished_.load(std::memory_order_acquire)) {
    return;
  }

  const bool uploaded = this->capture_upload_success_.load(std::memory_order_relaxed);
  const size_t sample_count = this->capture_upload_sample_count_;
  this->last_meta_ = this->capture_meta_;
  if (uploaded) {
    this->capture_count_.fetch_add(1, std::memory_order_relaxed);
    if (this->last_meta_.event == CaptureEvent::WAKE_DETECTED) {
      this->detection_capture_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
      this->close_miss_capture_count_.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
  }

  const float max_f = this->last_meta_.max_prob / 255.0f;
  const float avg_f = this->last_meta_.avg_prob / 255.0f;
  ESP_LOGI(TAG, "%s %.2f s %s for '%s' (max=%.2f, avg=%.2f)", uploaded ? "Uploaded" : "Failed to upload",
           (float) sample_count / (float) this->sample_rate_, event_type_str_(this->last_meta_.event),
           this->last_meta_.wake_word.c_str(), max_f, avg_f);

  if (uploaded) {
    this->capture_uploaded_trigger_.trigger(this->last_meta_.wake_word, event_type_str_(this->last_meta_.event), max_f,
                                            avg_f);
    // Historical contract: on_near_miss_detected fires only for close-misses.
    if (this->last_meta_.event == CaptureEvent::CLOSE_MISS) {
      this->near_miss_trigger_.trigger(this->last_meta_.wake_word, max_f, avg_f);
    }
  }

  this->capture_upload_started_ = false;
  this->capture_upload_finished_.store(false, std::memory_order_relaxed);
  this->capture_upload_success_.store(false, std::memory_order_relaxed);
  this->capture_upload_task_ = nullptr;
  this->capture_upload_sample_count_ = 0;
  this->capture_upload_started_ms_ = 0;
  this->capture_pending_ = false;
}

void MwwTrainingCapture::start_upload_task_(size_t sample_count) {
  this->capture_upload_sample_count_ = sample_count;
  this->capture_upload_success_.store(false, std::memory_order_relaxed);
  this->capture_upload_finished_.store(false, std::memory_order_relaxed);
  this->capture_upload_started_ = true;
  this->capture_upload_started_ms_ = millis();

  const BaseType_t result = xTaskCreate(
      MwwTrainingCapture::upload_task_, "mww_capture_upload", 8192, this, 1, &this->capture_upload_task_);
  if (result != pdPASS) {
    ESP_LOGW(TAG, "Failed to start capture upload task; dropping capture");
    this->capture_upload_success_.store(false, std::memory_order_relaxed);
    this->capture_upload_finished_.store(true, std::memory_order_release);
    return;
  }

  ESP_LOGD(TAG, "Capture upload task started (%u samples)", (unsigned) sample_count);
}

void MwwTrainingCapture::upload_task_(void *arg) {
  auto *self = static_cast<MwwTrainingCapture *>(arg);
  const bool uploaded = self->upload_wav_(self->capture_buffer_, self->capture_upload_sample_count_);
  self->capture_upload_success_.store(uploaded, std::memory_order_relaxed);
  self->capture_upload_finished_.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool MwwTrainingCapture::upload_wav_(const int16_t *samples, size_t sample_count) {
  if (this->upload_url_.empty()) {
    ESP_LOGW(TAG, "No upload_url configured; dropping capture");
    return false;
  }
  if (samples == nullptr || sample_count == 0) {
    ESP_LOGW(TAG, "Capture has no audio samples; dropping upload");
    return false;
  }

  const uint32_t data_bytes = (uint32_t)(sample_count * sizeof(int16_t));
  const uint32_t total_bytes = 44 + data_bytes;
  const uint32_t riff_size = 36 + data_bytes;
  const uint16_t channels = 1;
  const uint16_t bits_per_sample = 16;
  const uint16_t block_align = channels * (bits_per_sample / 8);
  const uint32_t byte_rate = this->sample_rate_ * block_align;

  esp_http_client_config_t config = {};
  config.url = this->upload_url_.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 4500;
  config.buffer_size = 512;
  // esp_http_client renders the request line AND every header into the tx buffer inside
  // esp_http_client_open(). The full metadata set lands near 1 kB (X-Probability-History alone
  // can be ~150 bytes), so 512 silently fails the open and every upload returns false — which
  // looks exactly like an unreachable endpoint. Do not lower this without recounting the headers.
  config.buffer_size_tx = 2048;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Failed to initialise HTTP client for capture upload");
    return false;
  }

  const CaptureMeta &meta = this->capture_meta_;

  char max_probability[12];
  char avg_probability[12];
  char prob_cutoff[12];
  char rise_score[12];
  char active_windows[12];
  char sample_rate[12];
  snprintf(max_probability, sizeof(max_probability), "%.3f", meta.max_prob / 255.0f);
  snprintf(avg_probability, sizeof(avg_probability), "%.3f", meta.avg_prob / 255.0f);
  snprintf(prob_cutoff, sizeof(prob_cutoff), "%.3f", meta.prob_cutoff / 255.0f);
  snprintf(rise_score, sizeof(rise_score), "%.3f", meta.rise_score / 255.0f);
  snprintf(active_windows, sizeof(active_windows), "%u", (unsigned) meta.active_windows);
  snprintf(sample_rate, sizeof(sample_rate), "%u", (unsigned) this->sample_rate_);

  // Wake word goes into an HTTP header and originates from a user-supplied model manifest —
  // clamp it to a safe character set so a stray CR/LF can't corrupt the request.
  std::string safe_wake_word;
  safe_wake_word.reserve(meta.wake_word.size());
  for (char c : meta.wake_word) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                    c == '_' || c == '-';
    safe_wake_word.push_back(ok ? c : '_');
    if (safe_wake_word.size() >= 48)
      break;
  }

  esp_http_client_set_header(client, "Content-Type", "audio/wav");
  // Tells the trainer the body is a complete WAV rather than headerless PCM.
  esp_http_client_set_header(client, "X-Audio-Format", this->audio_format_header_.c_str());
  esp_http_client_set_header(client, "X-Original-Name", meta.original_name.c_str());
  esp_http_client_set_header(client, "X-Source-Device", this->device_name_.c_str());
  esp_http_client_set_header(client, "X-Wake-Word", safe_wake_word.c_str());
  esp_http_client_set_header(client, "X-Event-Type", event_type_str_(meta.event));
  esp_http_client_set_header(client, "X-Blocked-By-Vad", meta.blocked_by_vad ? "true" : "false");
  esp_http_client_set_header(client, "X-Max-Probability", max_probability);
  esp_http_client_set_header(client, "X-Average-Probability", avg_probability);
  esp_http_client_set_header(client, "X-Probability-Cutoff", prob_cutoff);
  esp_http_client_set_header(client, "X-Sample-Rate", sample_rate);
  // Display-only proxies — see count_active_windows_() / compute_rise_score_(). Deliberately no
  // X-Min-Active-Windows: that would let a reviewer auto-gate on a metric ESPHome doesn't compute.
  esp_http_client_set_header(client, "X-Active-Windows", active_windows);
  esp_http_client_set_header(client, "X-Rise-Score", rise_score);
  if (!meta.prob_history_csv.empty()) {
    esp_http_client_set_header(client, "X-Probability-History", meta.prob_history_csv.c_str());
  }
  if (!this->detection_profile_.empty()) {
    esp_http_client_set_header(client, "X-Detection-Profile", this->detection_profile_.c_str());
  }
  if (!this->notes_.empty()) {
    esp_http_client_set_header(client, "X-Notes", this->notes_.c_str());
  }
#ifdef USE_MICRO_WAKE_WORD_VAD
  char vad_max[12];
  char vad_avg[12];
  snprintf(vad_max, sizeof(vad_max), "%.3f", meta.vad_max_prob / 255.0f);
  snprintf(vad_avg, sizeof(vad_avg), "%.3f", meta.vad_avg_prob / 255.0f);
  esp_http_client_set_header(client, "X-Vad-Max-Probability", vad_max);
  esp_http_client_set_header(client, "X-Vad-Average-Probability", vad_avg);
#endif

  esp_err_t err = esp_http_client_open(client, total_bytes);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to open capture upload to %s: %s", this->upload_url_.c_str(), esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }

  auto write_all = [client](const uint8_t *data, int len) -> bool {
    int offset = 0;
    while (offset < len) {
      int written = esp_http_client_write(client, reinterpret_cast<const char *>(data + offset), len - offset);
      if (written <= 0) {
        return false;
      }
      offset += written;
    }
    return true;
  };

  uint8_t wav_header[44];
  size_t header_pos = 0;
  auto push32 = [&wav_header, &header_pos](uint32_t v) {
    wav_header[header_pos++] = (uint8_t)(v & 0xFF);
    wav_header[header_pos++] = (uint8_t)((v >> 8) & 0xFF);
    wav_header[header_pos++] = (uint8_t)((v >> 16) & 0xFF);
    wav_header[header_pos++] = (uint8_t)((v >> 24) & 0xFF);
  };
  auto push16 = [&wav_header, &header_pos](uint16_t v) {
    wav_header[header_pos++] = (uint8_t)(v & 0xFF);
    wav_header[header_pos++] = (uint8_t)((v >> 8) & 0xFF);
  };
  auto push_str = [&wav_header, &header_pos](const char *s) {
    for (; *s != '\0'; ++s) {
      wav_header[header_pos++] = (uint8_t) *s;
    }
  };

  push_str("RIFF");
  push32(riff_size);
  push_str("WAVE");
  push_str("fmt ");
  push32(16);                  // fmt chunk size
  push16(1);                   // PCM format
  push16(channels);
  push32(this->sample_rate_);
  push32(byte_rate);
  push16(block_align);
  push16(bits_per_sample);
  push_str("data");
  push32(data_bytes);

  bool ok = write_all(wav_header, sizeof(wav_header));
  uint8_t chunk[512];
  size_t sample_offset = 0;
  while (ok && sample_offset < sample_count) {
    const size_t samples_this_chunk =
        std::min<size_t>(sizeof(chunk) / sizeof(int16_t), sample_count - sample_offset);
    for (size_t i = 0; i < samples_this_chunk; ++i) {
      const int16_t sample = samples[sample_offset + i];
      chunk[i * 2] = (uint8_t)(((uint16_t) sample) & 0xFF);
      chunk[i * 2 + 1] = (uint8_t)((((uint16_t) sample) >> 8) & 0xFF);
    }
    ok = write_all(chunk, (int) (samples_this_chunk * sizeof(int16_t)));
    sample_offset += samples_this_chunk;
  }

  int status = 0;
  if (ok) {
    int header_length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (header_length < 0) {
      ESP_LOGW(TAG, "Capture upload failed while reading response headers");
      ok = false;
    }
  } else {
    ESP_LOGW(TAG, "Capture upload failed while writing request body");
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (!ok) {
    return false;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "Capture upload returned HTTP %d", status);
    return false;
  }
  ESP_LOGD(TAG, "Capture upload returned HTTP %d (%u bytes)", status, (unsigned) total_bytes);
  return true;
}

std::string MwwTrainingCapture::get_last_wake_word() { return this->last_meta_.wake_word; }
std::string MwwTrainingCapture::get_last_event_type() {
  // Empty until the first successful capture, so the HA entity reads blank rather than
  // claiming a close_miss that never happened.
  if (this->capture_count_.load(std::memory_order_relaxed) == 0)
    return std::string();
  return std::string(event_type_str_(this->last_meta_.event));
}
float MwwTrainingCapture::get_last_max_probability() { return this->last_meta_.max_prob / 255.0f; }
float MwwTrainingCapture::get_last_average_probability() { return this->last_meta_.avg_prob / 255.0f; }

}  // namespace mww_training_capture
}  // namespace esphome

#endif  // USE_ESP32
