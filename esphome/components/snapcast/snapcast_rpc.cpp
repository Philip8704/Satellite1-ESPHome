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
#include "snapcast_rpc.h"
#include "esp_transport_tcp.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace snapcast {

static const char *const TAG = "snapcast_rpc";

static const size_t TASK_STACK_SIZE = 4 * 1024;
static const uint8_t RPC_TASK_PRIORITY = 5;

enum RPCTaskBits : uint32_t {
  DISCONNECT_BIT = (1 << 0),
  RECONNECT_BIT = (1 << 1),
};

esp_err_t SnapcastControlSession::connect(const std::string &server, uint32_t port) {
  if (this->task_exiting_ && this->task_handle_) {
    auto st = eTaskGetState(this->task_handle_);
    if (st != eDeleted && st != eInvalid) {
      // task hasn't terminated yet
      return ESP_ERR_INVALID_STATE;
    } else {
      this->task_handle_ = nullptr;
      this->task_exiting_ = false;
    }
  }

  if (!this->config_mutex_) {
    this->config_mutex_ = xSemaphoreCreateMutex();
    if (!this->config_mutex_)
      return ESP_FAIL;
  }

  bool server_has_changed = false;
  {
    if (xSemaphoreTake(this->config_mutex_, 0) != pdTRUE) {
      return ESP_ERR_TIMEOUT;
    }
    server_has_changed = (server != this->server_) || (port != this->port_);
    this->server_ = server;
    this->port_ = port;
    this->task_should_run_ = true;
    xSemaphoreGive(this->config_mutex_);
  }

  if (!this->rpc_queue_) {
    this->rpc_queue_ = xQueueCreate(4, sizeof(RpcRequest *));
  }

  if (this->task_stack_buffer_ == nullptr) {
    RAMAllocator<StackType_t> stack_allocator;
    this->task_stack_buffer_ = stack_allocator.allocate(TASK_STACK_SIZE);
    if (this->task_stack_buffer_ == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (!this->task_handle_) {
    this->task_handle_ = xTaskCreateStatic(
        [](void *param) {
          auto *session = static_cast<SnapcastControlSession *>(param);
          session->notification_loop();
          vTaskDelete(nullptr);
        },
        "snap_rpc_task", TASK_STACK_SIZE, (void *) this, RPC_TASK_PRIORITY, this->task_stack_buffer_,
        &this->task_stack_);

    if (this->task_handle_ == nullptr) {
      return ESP_FAIL;
    }
  } else if (server_has_changed) {
    xTaskNotify(this->task_handle_, RECONNECT_BIT, eSetBits);
  }

  return ESP_OK;
}

esp_err_t SnapcastControlSession::disconnect() {
  this->task_should_run_ = false;
  if (this->task_handle_) {
    xTaskNotify(this->task_handle_, DISCONNECT_BIT, eSetBits);
  }
  return ESP_OK;
}

bool SnapcastControlSession::send_rpc_async(const std::string &method, std::function<void(JsonObject)> fill_params,
                                            RpcResponseCb on_response, uint32_t timeout_ms) {
  if (!this->rpc_queue_)
    return false;

  auto *req = new RpcRequest{
      .method = method,
      .id = next_id_.fetch_add(1, std::memory_order_relaxed),
      .fill_params = std::move(fill_params),
      .on_response = std::move(on_response),
      .timeout_ms = timeout_ms,
  };

  if (xQueueSend(rpc_queue_, &req, 0) != pdTRUE) {
    delete req;
    return false;
  }
  return true;
}

void SnapcastControlSession::drain_rpc_queue_() {
  RpcRequest *req = nullptr;

  while (xQueueReceive(rpc_queue_, &req, 0) == pdTRUE) {
    if (!this->transport_) {  // not connected
      delete req;
      continue;
    }

    // Build JSON-RPC
    JsonDocument doc;
    doc["jsonrpc"] = "2.0";
    doc["id"] = req->id;
    doc["method"] = req->method;
    JsonObject params = doc["params"].to<JsonObject>();
    if (req->fill_params)
      req->fill_params(params);

    // Serialize
    static char buf[768];  // adjust to your message sizes
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n + 1 < sizeof(buf))
      buf[n++] = '\n';

    esp_transport_write(transport_, buf, n, 1000);
    // buf[n] = '\0';
    // printf("sent: %s", buf);

    // Register callback
    if (req->on_response) {
      pending_[req->id] = PendingCb{
          .cb = std::move(req->on_response),
          .expires_ms = millis() + req->timeout_ms,
      };
    }

    delete req;
  }
}

void SnapcastControlSession::expire_pending_() {
  uint32_t now = millis();
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (now >= it->second.expires_ms) {
      // just drop
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void SnapcastControlSession::update_from_server_obj_(const JsonObject &server_obj) {
  ClientState new_state;
  {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    new_state = this->client_state_;
    xSemaphoreGive(state_mutex_);
  }
  bool found = new_state.from_groups_json(server_obj["groups"], this->client_id_);

#if SNAPCAST_DEBUG
  if (found) {
    printf("group_id: %s stream_id: %s\n", new_state.group_id.c_str(), new_state.stream_id.c_str());
    printf("Group members (%zu):\n", new_state.group_members.size());
    for (const auto &id : new_state.group_members) {
      printf("  member: %s\n", id.c_str());
    }
  }
#endif

  JsonArray streams = server_obj["streams"];
  auto &streams_map = new_state.known_streams;
  streams_map.clear();
  for (JsonObject stream_obj : streams) {
    const char *stream_id = stream_obj["id"].as<const char *>();
    if (!stream_id)
      continue;
    StreamInfo &sInfo = streams_map[stream_id];
    sInfo.from_json(stream_obj);
  }

  StreamStatus status = StreamStatus::UNKNOWN;
  std::string stream_id;
  auto it = streams_map.find(new_state.stream_id);
  if (it != streams_map.end()) {
    status = it->second.status;
    stream_id = it->second.id;
  }

  {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    this->client_state_ = std::move(new_state);
    xSemaphoreGive(state_mutex_);
  }

  if (!this->on_stream_update_) {
    return;
  }

  if (status != StreamStatus::UNKNOWN) {
    this->on_stream_update_(status, stream_id);
  }
}

void SnapcastControlSession::notification_loop() {
  this->line_buffer_.reserve(1024);
  while (this->task_should_run_) {
    // Initialize transport
    if (this->transport_ != nullptr) {
      esp_transport_close(this->transport_);
      esp_transport_destroy(this->transport_);
    }
    this->transport_ = esp_transport_tcp_init();

    if (this->transport_ == nullptr) {
      ESP_LOGE(TAG, "Failed to initialize transport");
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }
    esp_transport_keep_alive_t keep_alive_config = {
        .keep_alive_enable = true,
        .keep_alive_idle = 10000,     // 10 seconds
        .keep_alive_interval = 5000,  // 5 seconds
        .keep_alive_count = 5,        // Number of keep-alive probes to send before considering the connection dead
    };
    esp_transport_tcp_set_keep_alive(this->transport_, &keep_alive_config);
    // Try to connect
    error_t err = esp_transport_connect(this->transport_, this->server_.c_str(), this->port_, -1);
    if (err != 0) {
      ESP_LOGE(TAG, "Connection failed with error: %d", errno);
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }
    this->connected_ = true;
    this->request_server_info_();

    {
      xSemaphoreTake(state_mutex_, portMAX_DELAY);
      this->client_state_.clear();
      xSemaphoreGive(state_mutex_);
    }

    this->recv_buffer_.clear();
    this->line_buffer_.clear();
    bool skipping_oversize_ = false;
    size_t dropped_bytes_ = 0;
    static constexpr size_t MAX_LINE = 16 * 1024;
    while (true) {
      uint32_t notify_value = 0;
      if (xTaskNotifyWait(0, RECONNECT_BIT | DISCONNECT_BIT, &notify_value, 0) > 0) {
        break;
      }

      drain_rpc_queue_();
      expire_pending_();

      char chunk[128];  // small read buffer
      int len = esp_transport_read(this->transport_, chunk, sizeof(chunk), 100);
      if (len < 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT) {
          continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        break;
      }
      if (len > 0) {
        if (skipping_oversize_) {
          for (int i = 0; i < len; i++) {
            if (chunk[i] == '\n') {
              skipping_oversize_ = false;
              dropped_bytes_ = 0;
              // resume after newline: anything after it belongs to next message
              if (i + 1 < len) {
                recv_buffer_.append(&chunk[i + 1], len - (i + 1));
              }
              break;
            } else {
              dropped_bytes_++;
            }
          }
        } else {
          // Normal mode: append
          recv_buffer_.append(chunk, len);
        }

        if (!skipping_oversize_ && recv_buffer_.size() > MAX_LINE) {
          ESP_LOGW(TAG, "Oversized JSON line (%u+ bytes). Skipping until newline.", (unsigned) recv_buffer_.size());
          recv_buffer_.clear();
          skipping_oversize_ = true;
          dropped_bytes_ = 0;
          // don't parse anything this iteration
          continue;
        }
        if (!skipping_oversize_) {
          size_t pos;
          while ((pos = this->recv_buffer_.find('\n')) != std::string::npos) {
            this->line_buffer_.assign(this->recv_buffer_, 0, pos);

#if SNAPCAST_DEBUG
            printf("JSON: %s\n", this->line_buffer_.c_str());
#endif
            this->recv_buffer_.erase(0, pos + 1);

            json::parse_json(this->line_buffer_, [this](JsonObject root) -> bool {
              if (root["id"].is<uint32_t>()) {
                uint32_t id = root["id"].as<uint32_t>();
                auto it = pending_.find(id);
                if (it != pending_.end()) {
                  auto cb = std::move(it->second.cb);
                  pending_.erase(it);
                  if (root["result"].is<JsonObject>()) {
                    cb(root);
                  } else if (root["error"].is<JsonObject>()) {
                    JsonObject err = root["error"];
                    int code = err["code"] | 0;
                    const char *msg = err["message"] | "<no message>";
                    ESP_LOGW(TAG, "RPC error id=%u code=%d message=\"%s\"", id, code, msg);
                  }
                }
              } else {
                const char *method = root["method"].as<const char *>();
                if (!method)
                  return false;
                if (strcmp(method, "Server.OnUpdate") == 0) {
                  this->update_from_server_obj_(root["params"]["server"].as<JsonObject>());
                } else if (strcmp(method, "Group.OnStreamChanged") == 0) {
                  JsonObject params = root["params"];
                  const char *group_id = params["id"].as<const char *>();
                  if (!group_id)
                    return false;
                  const char *stream_id = params["stream_id"].as<const char *>();
                  if (!stream_id)
                    return false;

                  StreamStatus status = StreamStatus::UNKNOWN;
                  bool new_stream_unknown = false;
                  {
                    xSemaphoreTake(state_mutex_, portMAX_DELAY);
                    if (strcmp(group_id, this->client_state_.group_id.c_str()) == 0 &&
                        strcmp(stream_id, this->client_state_.stream_id.c_str()) != 0) {
                      auto &streams_map = this->client_state_.known_streams;
                      auto it = streams_map.find(stream_id);
                      if (it != streams_map.end()) {
                        this->client_state_.stream_id = stream_id;
                        status = it->second.status;
                      } else {
                        new_stream_unknown = true;
                      }
                    }
                    xSemaphoreGive(state_mutex_);
                  }
                  if (new_stream_unknown) {
                    request_server_info_();
                  } else if (this->on_stream_update_ && status != StreamStatus::UNKNOWN) {
                    this->on_stream_update_(status, stream_id);
                  }

                } else if (strcmp(method, "Stream.OnUpdate") == 0) {
                  JsonObject params = root["params"];
                  const char *stream_id = params["id"].as<const char *>();
                  if (!stream_id)
                    return false;

                  StreamInfo sInfo;
                  sInfo.from_json(params["stream"]);
                  StreamStatus status = sInfo.status;

                  bool notify = false;
                  {
                    xSemaphoreTake(state_mutex_, portMAX_DELAY);

                    auto &streams_map = this->client_state_.known_streams;
                    streams_map.insert_or_assign(stream_id, std::move(sInfo));

                    if (strcmp(stream_id, this->client_state_.stream_id.c_str()) == 0 &&
                        status != StreamStatus::UNKNOWN) {
                      notify = true;
                    }

                    xSemaphoreGive(state_mutex_);
                  }

                  if (notify && this->on_stream_update_) {
                    this->on_stream_update_(status, stream_id);
                  }
                }
              }
              return true;
            });
          }
        }
      }
    }
  }

  if (this->transport_ != nullptr) {
    esp_transport_close(this->transport_);
    esp_transport_destroy(this->transport_);
    this->transport_ = nullptr;
    this->connected_ = false;
  }
  this->task_exiting_ = true;
}

void SnapcastControlSession::request_server_info_(std::function<void(const ClientState &state)> cb) {
  this->send_rpc_async(
      "Server.GetStatus",
      [](JsonObject params) {
        // no params
      },
      [this, cb = std::move(cb)](JsonObject root) {
        JsonObject server = root["result"]["server"];
        if (!server)
          return;
        this->update_from_server_obj_(root["result"]["server"].as<JsonObject>());
        if (cb) {
          cb(this->client_state_);
        }
      },
      2000);
}

void SnapcastControlSession::set_group_stream_(const std::string &stream_id) {
  std::string gid = this->grp_id_snapshot();
  this->send_rpc_async(
      "Group.SetStream",
      [gid = std::move(gid), sid = stream_id](JsonObject params) {
        params["id"] = gid;
        params["stream_id"] = sid;
      },
      {}, 2000);
}

void SnapcastControlSession::group_request_stop(const ClientState &state) {
  auto it = state.known_streams.find(state.stream_id);
  if (it == state.known_streams.end()) {
    return;
  }
  const StreamInfo &sInfo = it->second;
  if (sInfo.canControl) {
    std::string stream_id = state.stream_id;
    this->send_rpc_async(
        "Stream.Control",
        [sId = std::move(stream_id)](JsonObject params) {
          params["id"] = sId;
          params["command"] = "pause";
          JsonArray cmd_params = params["clients"].to<JsonArray>();
        },
        [this](JsonObject root) { this->request_server_info_({}); }, 2000);
    return;
  }
  // stream doesn't support direct control, set to default stream instead.
  std::string grp_id = state.group_id;
  std::string default_stream = state.default_streamid;
  this->send_rpc_async(
      "Group.SetStream",
      [gid = std::move(grp_id), sid = std::move(default_stream)](JsonObject params) {
        params["id"] = gid;
        params["stream_id"] = sid;
      },
      [this](JsonObject root) mutable { this->request_server_info_({}); }, 2000);
}

void SnapcastControlSession::isolate_client_(std::function<void(const ClientState &state)> cb) {
  auto snap = this->state_snapshot();
  auto gid = snap.group_id;
  auto self_id = this->client_id_;
  auto members = snap.group_members;
  this->send_rpc_async(
      "Group.SetClients",
      [gid, self_id, members = std::move(members)](JsonObject params) {
        params["id"] = gid;
        JsonArray clients = params["clients"].to<JsonArray>();
        for (const auto &cli_id : members) {
          if (cli_id != self_id)
            clients.add(cli_id);
        }
      },
      [this, cb = std::move(cb)](JsonObject root) mutable {
        if (cb)
          this->request_server_info_(std::move(cb));
        else
          this->request_server_info_({});
      },
      2000);
}

void SnapcastControlSession::request_stop() {
  const ClientState state = this->state_snapshot();
  this->group_request_stop(state);
  // if (state.group_members.size() > 1) {
  //   // is member of a group, isolate client first
  //   this->isolate_client_([this](const ClientState &state) { this->group_request_stop(state); });
  // } else {
  //   // is only group memmber
  //   this->group_request_stop(state);
  // }
}

}  // namespace snapcast
}  // namespace esphome
