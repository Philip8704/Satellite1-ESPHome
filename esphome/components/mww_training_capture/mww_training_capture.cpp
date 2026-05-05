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
  // post-buffer plus a small headroom. Total samples fits comfortably in
  // PSRAM on the Sat1 ESP32-S3 (~96 kB at 3 s).
  const float total_seconds = this->pre_buffer_seconds_ + (this->post_buffer_ms_ / 1000.0f) + 0.5f;
  this->ring_capacity_ = static_cast<size_t>(total_seconds * this->sample_rate_);
  if (this->ring_capacity_ < this->sample_rate_) {
    this->ring_capacity_ = this->sample_rate_;  // hard floor of 1 s
  }
  const uint64_t configured_capture_samples =
      (uint64_t)(this->pre_buffer_seconds_ * this->sample_rate_) +
      ((uint64_t) this->post_buffer_ms_ * this->sample_rate_ / 1000ULL);
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

  ESP_LOGI(TAG, "Training capture ready (ring=%u samples / %.1f s, capture=%u samples, models=%u, enabled=%s)",
           (unsigned) this->ring_capacity_, total_seconds, (unsigned) this->capture_buffer_capacity_,
           (unsigned) this->models_.size(),
           this->enabled_ ? "yes" : "no");
}

void MwwTrainingCapture::dump_config() {
  ESP_LOGCONFIG(TAG, "MWW Training Capture:");
  ESP_LOGCONFIG(TAG, "  Pre-buffer: %.2f s", this->pre_buffer_seconds_);
  ESP_LOGCONFIG(TAG, "  Post-buffer: %u ms", (unsigned) this->post_buffer_ms_);
  ESP_LOGCONFIG(TAG, "  Cooldown: %u ms", (unsigned) this->cooldown_ms_);
  ESP_LOGCONFIG(TAG, "  Require VAD: %s", this->require_vad_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Upload URL: %s", this->upload_url_.c_str());
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
    // Near-miss band: [lower_cutoff, real_cutoff). If VAD is inactive, also
    // keep scores above the real cutoff because MWW itself will not fire.
    if (max_prob < entry.lower_cutoff)
      continue;
    if (max_prob >= real_cutoff && vad_active)
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
  if (this->capture_upload_started_) {
    this->finalize_upload_if_finished_();
    return;
  }

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
    this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
    this->capture_pending_ = false;
    return;
  }

  // Compute slice we want: [target - (pre+post) samples, target). We copy the
  // slice into a persistent PSRAM buffer before upload so Wi-Fi latency cannot
  // race the live audio ring and corrupt the beginning of the capture.
  const uint64_t pre_samples = (uint64_t)(this->pre_buffer_seconds_ * this->sample_rate_);
  const uint64_t post_samples = (uint64_t) this->post_buffer_ms_ * this->sample_rate_ / 1000ULL;
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
  this->last_wake_word_ = this->capture_wake_word_;
  this->last_max_prob_ = this->capture_max_prob_;
  this->last_avg_prob_ = this->capture_avg_prob_;
  if (uploaded) {
    this->capture_count_.fetch_add(1, std::memory_order_relaxed);
  } else {
    this->capture_drop_count_.fetch_add(1, std::memory_order_relaxed);
  }

  ESP_LOGI(TAG, "%s %.2f s near-miss for '%s' (max=%.2f, avg=%.2f)",
           uploaded ? "Uploaded" : "Failed to upload",
           (float) sample_count / (float) this->sample_rate_, this->last_wake_word_.c_str(),
           this->last_max_prob_ / 255.0f, this->last_avg_prob_ / 255.0f);

  if (uploaded) {
    this->near_miss_trigger_.trigger(this->last_wake_word_, this->last_max_prob_ / 255.0f,
                                     this->last_avg_prob_ / 255.0f);
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
  config.buffer_size_tx = 512;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Failed to initialise HTTP client for capture upload");
    return false;
  }

  char max_probability[12];
  char avg_probability[12];
  char sample_rate[12];
  snprintf(max_probability, sizeof(max_probability), "%.3f", this->capture_max_prob_ / 255.0f);
  snprintf(avg_probability, sizeof(avg_probability), "%.3f", this->capture_avg_prob_ / 255.0f);
  snprintf(sample_rate, sizeof(sample_rate), "%u", (unsigned) this->sample_rate_);

  esp_http_client_set_header(client, "Content-Type", "audio/wav");
  esp_http_client_set_header(client, "X-Wake-Word", this->capture_wake_word_.c_str());
  esp_http_client_set_header(client, "X-Max-Probability", max_probability);
  esp_http_client_set_header(client, "X-Avg-Probability", avg_probability);
  esp_http_client_set_header(client, "X-Device-Name", this->device_name_.c_str());
  esp_http_client_set_header(client, "X-Sample-Rate", sample_rate);

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

std::string MwwTrainingCapture::get_last_wake_word() { return this->last_wake_word_; }
float MwwTrainingCapture::get_last_max_probability() { return this->last_max_prob_ / 255.0f; }
float MwwTrainingCapture::get_last_average_probability() { return this->last_avg_prob_ / 255.0f; }

}  // namespace mww_training_capture
}  // namespace esphome

#endif  // USE_ESP32
