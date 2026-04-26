/*
 * This file is part of Snapcast integration for ESPHome.
 *
 * Copyright (C) 2025 Mischa Siekmann <FutureProofHomes Inc.>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <atomic>
#include <string>
#include "esp_transport.h"

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/component.h"

#include "esphome/components/audio/chunked_ring_buffer.h"
// #include "esphome/components/audio/timed_ring_buffer.h"

#include "messages.h"
#include "time_stats.h"

namespace esphome {
using namespace audio;
namespace snapcast {

enum class StreamState {
  DESTROYED,  // stream_task and transport_task not running
  STARTING,
  DISCONNECTED,  // stream_task is running, not connected yet
  CONNECTING,
  CONNECTED_IDLE,  // Connected but waiting
  STREAMING,       // Receiving data
  RECONNECTING,
  STOPPING,  // Requested shutdown
  ERROR,     // Fatal or recoverable error
};

class SnapcastClient;

class SnapcastStream {
 public:
  esp_err_t init();
  esp_err_t terminate();
  bool finalize_termination();

  /// @brief Establish a connection to the Snapcast server.
  /// @param server The hostname or IP address of the Snapcast server.
  /// @param port The TCP port to connect to on the server.
  /// @return `ESP_OK` on success, or an appropriate error code.
  esp_err_t connect(const std::string &server, uint32_t port);

  /// @brief Disconnect from the Snapcast server.
  /// @return `ESP_OK` on success, or an appropriate error code.
  esp_err_t disconnect();

  /// @brief Start receiving and processing audio/data from the server.
  /// @param ring_buffer The buffer where received audio samples are written.
  /// @param notification_task A FreeRTOS task to be notified on status changes.
  /// @return `ESP_OK` on success, or an appropriate error code.
  esp_err_t start_with_notify(std::weak_ptr<esphome::TimedRingBuffer> ring_buffer, TaskHandle_t notification_task);
  void clear_notification_target() { this->notification_target_ = nullptr; }

  /// @brief Stop the audio/data stream and return to an idle state.
  /// @return `ESP_OK` on success, or an appropriate error code.
  esp_err_t stop_streaming();

  /// @brief Report volume and mute status to the Snapcast server.
  /// @param volume The volume level to report (0–100).
  /// @param muted Whether the stream is muted.
  /// @return `ESP_OK` on success, or an appropriate error code.
  esp_err_t report_volume(uint8_t volume, bool muted);

  bool is_connected() const {
    return this->state_ == StreamState::CONNECTED_IDLE || this->state_ == StreamState::STREAMING;
  }
  bool is_destroyed() const { return this->state_ == StreamState::DESTROYED; }
  bool is_terminating() const { return this->stream_task_exiting_; }

  bool server_is_lost() const { return this->state_ == StreamState::DISCONNECTED && !this->want_connected_; }

  bool error_enforces_termination() const { return this->state_ == StreamState::ERROR && this->stream_task_exiting_; }
  bool is_streaming() const { return this->state_ == StreamState::STREAMING; }

  /// @brief Set a callback to be invoked on stream status updates.
  /// @param cb A function receiving state, volume, and mute information.
  void set_on_status_update_callback(std::function<void(StreamState state, uint8_t volume, bool muted)> cb) {
    this->on_status_update_ = std::move(cb);
  }

  void get_target_snapshot_(std::string &server, uint32_t &port);

 protected:
  friend SnapcastClient;
  void on_server_settings_msg_(const ServerSettingsMessage &msg);
  void on_time_msg_(MessageHeader msg, tv_t time);

  SemaphoreHandle_t config_mutex_{nullptr};
  std::string server_;
  uint32_t port_{0};
  uint32_t server_buffer_size_{0};
  int32_t latency_{0};
  std::atomic<uint8_t> volume_{0};
  std::atomic<bool> muted_{false};

  StreamState state_{StreamState::DESTROYED};
  std::string error_msg_;
  bool want_connected_{false};
  std::atomic<bool> want_streaming_{false};

  tv_t to_local_time_(tv_t server_time) const {
    return server_time - this->est_time_diff_.load(std::memory_order_relaxed) +
           tv_t::from_millis(this->server_buffer_size_ + this->latency_);
  }
  uint32_t last_time_sync_{0};
  TimeStats time_stats_;
  std::atomic<tv_t> est_time_diff_;

  std::weak_ptr<esphome::TimedRingBuffer> write_ring_buffer_;

  TaskHandle_t notification_target_{nullptr};
  std::function<void(StreamState state, uint8_t volume, bool muted)> on_status_update_;

 private:
  StackType_t *task_stack_buffer_{nullptr};

  TaskHandle_t stream_task_handle_{nullptr};
  std::atomic<bool> stream_task_exiting_{false};
  StaticTask_t task_stack_;

  TaskHandle_t transport_task_handle_{nullptr};
  std::atomic<bool> transport_task_exiting_{false};

  QueueHandle_t outgoing_queue_{nullptr};

  void stream_task_();

  void start_streaming_();
  void stop_streaming_();
  void set_state_(StreamState new_state);
  void request_wifi_high_performance_();
  void release_wifi_high_performance_();
  void schedule_wifi_ps_verification_(const char *reason, bool expect_none);
  void poll_wifi_ps_verification_();

  void send_message_(SnapcastMessage *msg);
  void send_hello_();
  void send_report_();
  void send_time_sync_();

  esp_err_t read_and_process_messages_(ChunkedRingBuffer *read_ring_buffer, uint32_t timeout_ms);
  void maybe_log_debug_stats_();
  void reset_sync_wait_watchdog_();

  bool codec_header_sent_{false};
  uint8_t *codec_header_{nullptr};
  size_t codec_header_size_{0};

  bool wifi_high_perf_requested_{false};
#if SNAPCAST_DEBUG
  bool wifi_ps_verify_pending_{false};
  bool wifi_ps_expect_none_{false};
  uint32_t wifi_ps_verify_deadline_ms_{0};
  const char *wifi_ps_verify_reason_{nullptr};
#endif

  uint32_t wire_chunks_seen_{0};
  uint32_t chunks_pushed_{0};
  uint32_t drop_not_ready_{0};
  uint32_t drop_past_{0};

#if SNAPCAST_DEBUG
  uint32_t debug_last_stats_log_ms_{0};
  uint32_t debug_last_wire_chunks_seen_{0};
  uint32_t debug_last_chunks_pushed_{0};
  uint32_t debug_last_drop_not_ready_{0};
  uint32_t debug_last_drop_past_{0};
#endif

  uint32_t sync_wait_started_ms_{0};
  uint32_t sync_wait_wire_start_{0};
  bool sync_wait_active_{false};

  uint32_t sync_bootstrap_started_ms_{0};
  bool sync_ready_logged_{false};

  bool resume_alignment_pending_{false};
  uint32_t resume_alignment_started_ms_{0};
  uint32_t resume_alignment_dropped_{0};
};

}  // namespace snapcast
}  // namespace esphome
