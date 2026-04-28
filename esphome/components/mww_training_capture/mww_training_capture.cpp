#include "mww_training_capture.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace mww_training_capture {

static const char *const TAG = "mww_training_capture";

// Maximum sliding-window quantised probability we keep watching for. Anything
// above this is the real wake-word — we *want* MWW to trigger on those, not
// us, so capping the band keeps us out of MWW's way.
static constexpr uint8_t NEAR_MISS_UPPER_HEADROOM = 4;  // ~0.016 in float

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
  // post-buffer plus a small headroom. Total samples fits comfortably in
  // PSRAM on the Sat1 ESP32-S3 (~96 kB at 3 s).
  const float total_seconds = this->pre_buffer_seconds_ + (this->post_buffer_ms_ / 1000.0f) + 0.5f;
  this->ring_capacity_ = static_cast<size_t>(total_seconds * this->sample_rate_);
  if (this->ring_capacity_ < this->sample_rate_) {
    this->ring_capacity_ = this->sample_rate_;  // hard floor of 1 s
  }
  RAMAllocator<int16_t> alloc;
  this->ring_ = alloc.allocate(this->ring_capacity_);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-sample ring buffer", (unsigned) this->ring_capacity_);
    this->mark_failed();
    return;
  }
  std::memset(this->ring_, 0, this->ring_capacity_ * sizeof(int16_t));
  this->ring_initialised_ = true;

  // Subscribe to the same microphone source as MWW. ESPHome's MicrophoneSource
  // uses a CallbackManager so multiple subscribers get the same audio frames —
  // MWW is the one that starts/stops the underlying mic, our callback simply
  // piggybacks. That gives us the desirable property that we stop capturing
  // automatically whenever MWW pauses (e.g. while voice_assistant is active).
  this->mww_->add_audio_callback([this](const std::vector<uint8_t> &data) { this->on_audio_data_(data); });

  // Pre-register every non-internal model that's exposed by MWW unless the
  // user explicitly listed models in YAML. This keeps the "drop-in capture"
  // case zero-config — registering models via YAML overrides this.
  if (this->models_.empty()) {
    for (auto *model : this->mww_->get_wake_words()) {
      if (model == nullptr || model->get_internal_only())
        continue;
      this->models_.push_back({model, this->default_lower_cutoff_, 0});
    }
  }

  ESP_LOGI(TAG, "Training capture ready (ring=%u samples / %.1f s, models=%u, enabled=%s)",
           (unsigned) this->ring_capacity_, total_seconds, (unsigned) this->models_.size(),
           this->enabled_ ? "yes" : "no");
}

void MwwTrainingCapture::dump_config() {
  ESP_LOGCONFIG(TAG, "MWW Training Capture:");
  ESP_LOGCONFIG(TAG, "  Pre-buffer: %.2f s", this->pre_buffer_seconds_);
  ESP_LOGCONFIG(TAG, "  Post-buffer: %u ms", (unsigned) this->post_buffer_ms_);
  ESP_LOGCONFIG(TAG, "  Cooldown: %u ms", (unsigned) this->cooldown_ms_);
  ESP_LOGCONFIG(TAG, "  Require VAD: %s", this->require_vad_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Default lower cutoff: %.2f", this->default_lower_cutoff_ / 255.0f);
  ESP_LOGCONFIG(TAG, "  Models registered: %u", (unsigned) this->models_.size());
  for (auto &m : this->models_) {
    if (m.model != nullptr) {
      ESP_LOGCONFIG(TAG, "    - %s lower=%.2f upper=%.2f", m.model->get_wake_word().c_str(),
                    m.lower_cutoff / 255.0f, m.model->get_probability_cutoff() / 255.0f);
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
  this->models_.push_back({model, this->default_lower_cutoff_, 0});
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
  this->models_.push_back({model, lower_cutoff, 0});
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

void MwwTrainingCapture::check_near_misses_() {
  if (!this->enabled_ || this->capture_pending_ || this->mww_ == nullptr) {
    return;
  }
  if (!this->mww_->is_running()) {
    return;
  }

#ifdef USE_MICRO_WAKE_WORD_VAD
  const bool vad_active = this->mww_->get_vad_state();
#else
  const bool vad_active = true;
#endif
  if (this->require_vad_ && !vad_active)
    return;

  const uint32_t now = millis();
  const uint64_t total_written = this->total_samples_written_.load(std::memory_order_acquire);

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
    const uint8_t upper_bound =
        real_cutoff > NEAR_MISS_UPPER_HEADROOM ? (uint8_t)(real_cutoff - NEAR_MISS_UPPER_HEADROOM) : real_cutoff;

    // Near-miss band: [lower_cutoff, real_cutoff). Both checks defensive — if
    // the user accidentally set lower_cutoff >= real_cutoff we silently no-op.
    if (max_prob < entry.lower_cutoff)
      continue;
    if (max_prob >= upper_bound && vad_active)
      continue;

    // Stage a capture. We pin the "end" sample index to where the producer
    // currently is — once the audio thread has written ``post_buffer_ms``
    // additional samples, finish_pending_capture_() will read the slice and
    // fire the trigger.
    const uint64_t post_samples = (uint64_t) this->post_buffer_ms_ * this->sample_rate_ / 1000ULL;
    this->capture_pending_ = true;
    this->capture_target_total_ = total_written + post_samples;
    this->capture_queued_ms_ = now;
    this->capture_wake_word_ = entry.model->get_wake_word();
    this->capture_max_prob_ = max_prob;
    this->capture_avg_prob_ = avg_prob;
    entry.last_capture_ms = now;

    ESP_LOGD(TAG, "Near-miss queued: %s max=%.2f avg=%.2f (real cutoff=%.2f, vad=%s, samples=%llu target=%llu)",
             this->capture_wake_word_.c_str(), max_prob / 255.0f, avg_prob / 255.0f, real_cutoff / 255.0f,
             vad_active ? "yes" : "no", (unsigned long long) total_written,
             (unsigned long long) this->capture_target_total_);
    return;  // one capture at a time
  }
}

void MwwTrainingCapture::finish_pending_capture_() {
  const uint64_t total_written = this->total_samples_written_.load(std::memory_order_acquire);
  if (total_written < this->capture_target_total_) {
    const uint32_t now = millis();
    const uint32_t max_wait_ms = this->post_buffer_ms_ + 1000;
    if (now - this->capture_queued_ms_ < max_wait_ms) {
      if (now - this->last_debug_log_ms_ > 1000) {
        ESP_LOGD(TAG, "Waiting for capture post-buffer: samples=%llu target=%llu",
                 (unsigned long long) total_written, (unsigned long long) this->capture_target_total_);
        this->last_debug_log_ms_ = now;
      }
      return;
    }
    ESP_LOGW(TAG, "Capture post-buffer timed out: samples=%llu target=%llu; finishing with available audio",
             (unsigned long long) total_written, (unsigned long long) this->capture_target_total_);
    this->capture_target_total_ = total_written;
  }

  if (this->capture_target_total_ == 0) {
    ESP_LOGW(TAG, "Capture had no audio samples; dropping");
    this->capture_pending_ = false;
    return;
  }

  // Compute slice we want: [target - (pre+post) samples, target). Keep this
  // conservative; building a multi-second WAV in std::vector can exhaust
  // internal heap on ESP32 builds where exceptions are disabled.
  const uint64_t requested_pre_samples = (uint64_t)(this->pre_buffer_seconds_ * this->sample_rate_);
  const uint64_t max_pre_samples = this->sample_rate_;  // debug-safe 1 s cap
  const uint64_t pre_samples = std::min<uint64_t>(requested_pre_samples, max_pre_samples);
  const uint64_t post_samples = (uint64_t) this->post_buffer_ms_ * this->sample_rate_ / 1000ULL;
  const uint64_t total_slice = pre_samples + post_samples;
  const uint64_t slice_len = std::min<uint64_t>(std::min<uint64_t>(total_slice, (uint64_t) this->ring_capacity_),
                                               this->capture_target_total_);
  const uint64_t start_sample = this->capture_target_total_ - slice_len;
  if (start_sample > this->capture_target_total_) {
    // Underflow guard
    this->capture_pending_ = false;
    return;
  }

  std::vector<int16_t> samples;
  samples.reserve((size_t) slice_len);
  samples.resize((size_t) slice_len, 0);

  // Find the ring index that corresponds to ``start_sample``. ring_head is
  // where the next write will go, so the most-recent sample lives at
  // (ring_head - 1) % capacity. Walk back ``total_written - start_sample``
  // positions from there.
  const size_t head = this->ring_head_.load(std::memory_order_acquire);
  const uint64_t back_off = total_written - start_sample;
  if (back_off > (uint64_t) this->ring_capacity_) {
    // We waited too long — start point already overwritten.
    ESP_LOGW(TAG, "Capture slice was overrun before assembly; dropping");
    this->capture_pending_ = false;
    return;
  }
  size_t start_idx = (head + this->ring_capacity_ - (size_t) back_off) % this->ring_capacity_;
  for (size_t i = 0; i < (size_t) slice_len; ++i) {
    samples[i] = this->ring_[(start_idx + i) % this->ring_capacity_];
  }

  // Build the WAV blob.
  this->build_wav_(samples, this->last_capture_wav_);
  this->last_wake_word_ = this->capture_wake_word_;
  this->last_max_prob_ = this->capture_max_prob_;
  this->last_avg_prob_ = this->capture_avg_prob_;
  this->capture_count_.fetch_add(1, std::memory_order_relaxed);

  ESP_LOGI(TAG, "Captured %.2f s near-miss for '%s' (max=%.2f, avg=%.2f, %u bytes)",
           (float) slice_len / (float) this->sample_rate_, this->last_wake_word_.c_str(),
           this->last_max_prob_ / 255.0f, this->last_avg_prob_ / 255.0f,
           (unsigned) this->last_capture_wav_.size());

  // Fire the trigger. YAML automation can now grab the WAV via
  // get_last_capture_wav_string() and POST it to the companion service.
  this->near_miss_trigger_.trigger(this->last_wake_word_, this->last_max_prob_ / 255.0f,
                                   this->last_avg_prob_ / 255.0f);

  this->capture_pending_ = false;
}

void MwwTrainingCapture::build_wav_(const std::vector<int16_t> &samples, std::vector<uint8_t> &wav_out) {
  const uint32_t data_bytes = (uint32_t)(samples.size() * sizeof(int16_t));
  const uint32_t riff_size = 36 + data_bytes;
  const uint16_t channels = 1;
  const uint16_t bits_per_sample = 16;
  const uint16_t block_align = channels * (bits_per_sample / 8);
  const uint32_t byte_rate = this->sample_rate_ * block_align;

  wav_out.clear();
  wav_out.reserve(44 + data_bytes);

  auto push32 = [&wav_out](uint32_t v) {
    wav_out.push_back((uint8_t)(v & 0xFF));
    wav_out.push_back((uint8_t)((v >> 8) & 0xFF));
    wav_out.push_back((uint8_t)((v >> 16) & 0xFF));
    wav_out.push_back((uint8_t)((v >> 24) & 0xFF));
  };
  auto push16 = [&wav_out](uint16_t v) {
    wav_out.push_back((uint8_t)(v & 0xFF));
    wav_out.push_back((uint8_t)((v >> 8) & 0xFF));
  };
  auto push_str = [&wav_out](const char *s) {
    for (; *s != '\0'; ++s) {
      wav_out.push_back((uint8_t) *s);
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

  // Append PCM samples little-endian.
  const uint8_t *raw = reinterpret_cast<const uint8_t *>(samples.data());
  wav_out.insert(wav_out.end(), raw, raw + data_bytes);
}

std::string MwwTrainingCapture::get_last_capture_wav_string() {
  return std::string(reinterpret_cast<const char *>(this->last_capture_wav_.data()),
                     this->last_capture_wav_.size());
}
size_t MwwTrainingCapture::get_last_capture_size() { return this->last_capture_wav_.size(); }
std::string MwwTrainingCapture::get_last_wake_word() { return this->last_wake_word_; }
float MwwTrainingCapture::get_last_max_probability() { return this->last_max_prob_ / 255.0f; }
float MwwTrainingCapture::get_last_average_probability() { return this->last_avg_prob_ / 255.0f; }

}  // namespace mww_training_capture
}  // namespace esphome

#endif  // USE_ESP32
