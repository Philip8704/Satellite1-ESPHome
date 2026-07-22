#include "xvf_control.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstring>

namespace esphome {
namespace xvf_control {

using satellite1::AUDIO_SERVICER_RESID;
using satellite1::CONTROL_CMD_READ_BIT;

static const char *const TAG = "xvf_control";

void XvfControl::setup() {
  // Probe deferred to loop() — XMOS may not have finished its own boot by
  // the time setup() runs at AFTER_CONNECTION priority. Loop polls the
  // parent's state every tick until it sees SAT_XMOS_CONNECTED_STATE.
  ESP_LOGCONFIG(TAG, "xvf_control queued; awaiting XMOS connection before probing");
}

void XvfControl::dump_config() {
  ESP_LOGCONFIG(TAG, "XVF Control:");
  ESP_LOGCONFIG(TAG, "  Audio servicer RESID: 0x%02X", AUDIO_SERVICER_RESID);
  ESP_LOGCONFIG(TAG, "  DOA poll interval: %u ms", (unsigned) this->doa_poll_interval_ms_);
  if (this->probe_complete_) {
    ESP_LOGCONFIG(TAG, "  Probe: complete (servicer %s)", this->servicer_present_ ? "PRESENT" : "ABSENT");
    if (this->servicer_present_) {
      ESP_LOGCONFIG(TAG, "  Capability flags: 0x%08X", (unsigned) this->capability_flags_);
      ESP_LOGCONFIG(TAG, "  Feature firmware version: %u.%u.%u", this->fw_feature_version_[0],
                    this->fw_feature_version_[1], this->fw_feature_version_[2]);
    }
  } else {
    ESP_LOGCONFIG(TAG, "  Probe: pending");
  }
}

void XvfControl::loop() {
  if (this->parent_ == nullptr) {
    return;
  }

  // Probe once, after the XMOS connects. State transitions back to
  // DETACHED on XMOS reset / re-flash — we don't re-probe automatically
  // because most users either run stock firmware (probe = absent forever)
  // or Phase 3 firmware (probe = present forever); flipping mid-session
  // would require an ESP32 reboot anyway.
  if (!this->probe_complete_) {
    if (this->parent_->state == satellite1::SAT_XMOS_CONNECTED_STATE) {
      this->probe_capabilities_();
    }
    return;
  }

  if (!this->servicer_present_)
    return;
  if (this->doa_poll_interval_ms_ == 0)
    return;

  const uint32_t now = millis();
  if (now - this->last_poll_ms_ < this->doa_poll_interval_ms_) {
    return;
  }
  this->last_poll_ms_ = now;

  // Pipeline stats roll up DOA, confidence, DSP load, and clip counters
  // into a single 16-byte read — preferred when available because it
  // halves the SPI traffic vs. two separate reads. Falls back to the
  // discrete DOA/CONFIDENCE commands when the firmware supports DOA
  // reporting but not the stats roll-up.
  if (this->has_capability(satellite1::audio_capability::PIPELINE_STATS_AVAILABLE)) {
    this->poll_pipeline_stats_();
  } else if (this->has_capability(satellite1::audio_capability::DOA_REPORTING)) {
    this->poll_doa_();
  }
}

bool XvfControl::transfer_command_(uint8_t cmd, uint8_t *payload, uint8_t len, bool is_read) {
  if (this->parent_ == nullptr) {
    return false;
  }
  const uint8_t cmd_byte = is_read ? (cmd | CONTROL_CMD_READ_BIT) : (cmd & ~CONTROL_CMD_READ_BIT);
  const bool ok = this->parent_->transfer(AUDIO_SERVICER_RESID, cmd_byte, payload, len);
  if (!ok) {
    ESP_LOGV(TAG, "transfer cmd=0x%02X (%s, len=%u) failed", cmd_byte, is_read ? "read" : "write", len);
  }
  return ok;
}

bool XvfControl::write_cmd_(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  // The Satellite1::transfer() signature takes a non-const buffer because
  // reads write back into it. For writes we copy into a local stack buffer
  // so callers can pass const data without const_cast.
  uint8_t buf[16] = {0};
  if (len > sizeof(buf)) {
    ESP_LOGE(TAG, "write payload (%u bytes) exceeds stack buffer", len);
    return false;
  }
  std::memcpy(buf, payload, len);
  return this->transfer_command_(cmd, buf, len, /*is_read=*/false);
}

bool XvfControl::read_cmd_(uint8_t cmd, uint8_t *payload_out, uint8_t len) {
  return this->transfer_command_(cmd, payload_out, len, /*is_read=*/true);
}

void XvfControl::probe_capabilities_() {
  this->probe_complete_ = true;

  uint8_t fw_version[4] = {0};
  uint8_t capability_buf[4] = {0};

  // Read feature version first — gives a more reliable "servicer present"
  // signal than the capability flags (which might legitimately be zero on
  // a Phase 3 build that compiled out every feature). Any non-zero byte in
  // either response means at least the audio servicer dispatcher exists.
  this->read_cmd_(satellite1::audio_cmd::FW_FEATURE_VERSION, fw_version, sizeof(fw_version));
  this->read_cmd_(satellite1::audio_cmd::CAPABILITY_FLAGS, capability_buf, sizeof(capability_buf));

  std::memcpy(this->fw_feature_version_, fw_version, sizeof(fw_version));

  this->capability_flags_ = static_cast<uint32_t>(capability_buf[0]) | (static_cast<uint32_t>(capability_buf[1]) << 8) |
                            (static_cast<uint32_t>(capability_buf[2]) << 16) |
                            (static_cast<uint32_t>(capability_buf[3]) << 24);

  const bool any_version = (fw_version[0] | fw_version[1] | fw_version[2] | fw_version[3]) != 0;
  this->servicer_present_ = any_version || this->capability_flags_ != 0;

  // One-shot mic_count probe. Cached for the lifetime of the boot —
  // it's a compile-time property of the running XMOS firmware.
  if (this->servicer_present_) {
    uint8_t mc = 0;
    if (this->read_cmd_(satellite1::audio_cmd::MIC_COUNT, &mc, 1) && mc >= 2 && mc <= 8) {
      this->cached_mic_count_ = mc;
    } else {
      // Servicer present but mic_count cmd not implemented — assume 2
      // (matches the pre-v2 firmware behavior).
      this->cached_mic_count_ = 2;
    }
  }

  if (this->servicer_present_) {
    ESP_LOGI(TAG, "XVF audio servicer present: caps=0x%08X fw=v%u.%u.%u", (unsigned) this->capability_flags_,
             fw_version[0], fw_version[1], fw_version[2]);
  } else {
    ESP_LOGI(TAG, "XVF audio servicer absent (stock FPH firmware) — entities will stay disabled");
  }

  if (this->capability_probed_trigger_ != nullptr) {
    this->capability_probed_trigger_->trigger(this->capability_flags_);
  }
}

void XvfControl::poll_doa_() {
  uint8_t angle_buf[2] = {0};
  if (this->read_cmd_(satellite1::audio_cmd::DOA_ANGLE, angle_buf, sizeof(angle_buf))) {
    // Little-endian signed 16-bit.
    this->cached_doa_angle_deg_ =
        static_cast<int16_t>(static_cast<uint16_t>(angle_buf[0]) | (static_cast<uint16_t>(angle_buf[1]) << 8));
  }

  uint8_t confidence = 0;
  if (this->read_cmd_(satellite1::audio_cmd::DOA_CONFIDENCE, &confidence, 1)) {
    this->cached_doa_confidence_ = confidence;
  }
}

void XvfControl::poll_pipeline_stats_() {
  // 16-byte packed struct per docs/xmos_dsp_control_protocol.md. Parsed
  // byte-by-byte rather than via a struct cast so endianness and padding
  // can't bite us if the host compiler ever pads differently than the
  // XMOS firmware's __attribute__((packed)) layout.
  uint8_t buf[16] = {0};
  if (!this->read_cmd_(satellite1::audio_cmd::PIPELINE_STATS, buf, sizeof(buf))) {
    return;
  }

  // Skip frames_since_boot (bytes 0..3) — useful for debugging but not a
  // first-class HA entity yet.
  this->cached_dsp_load_q8_8_ = static_cast<uint16_t>(buf[4]) | (static_cast<uint16_t>(buf[5]) << 8);
  this->cached_mic_l_clip_count_ = static_cast<uint16_t>(buf[6]) | (static_cast<uint16_t>(buf[7]) << 8);
  this->cached_mic_r_clip_count_ = static_cast<uint16_t>(buf[8]) | (static_cast<uint16_t>(buf[9]) << 8);
  // AEC ref clip counter at buf[10..11] is captured here for future use.
  this->cached_doa_angle_deg_ =
      static_cast<int16_t>(static_cast<uint16_t>(buf[12]) | (static_cast<uint16_t>(buf[13]) << 8));
  this->cached_doa_confidence_ = buf[14];
  // buf[15] is reserved pad.
}

// ---------------------------- Setters ------------------------------------

bool XvfControl::set_beam_mode(satellite1::BeamMode mode) {
  if (!this->servicer_present_)
    return false;
  // Mode-specific capability gate — refuses to send an adaptive write to
  // firmware that only supports fixed beamforming.
  const uint32_t needed = (mode == satellite1::BeamMode::FIXED)      ? satellite1::audio_capability::BEAM_FIXED
                          : (mode == satellite1::BeamMode::ADAPTIVE) ? satellite1::audio_capability::BEAM_ADAPTIVE
                                                                     : satellite1::audio_capability::BEAM_TRACKING;
  if (!this->has_capability(needed)) {
    ESP_LOGW(TAG, "set_beam_mode(%u) rejected — capability bit 0x%02X not present", (unsigned) mode, (unsigned) needed);
    return false;
  }
  const uint8_t v = static_cast<uint8_t>(mode);
  return this->write_cmd_(satellite1::audio_cmd::BEAM_MODE, &v, 1);
}

bool XvfControl::set_beam_angle_deg(int16_t angle_deg) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::BEAM_FIXED) &&
      !this->has_capability(satellite1::audio_capability::BEAM_ADAPTIVE)) {
    return false;
  }
  if (angle_deg > 180)
    angle_deg = 180;
  if (angle_deg < -180)
    angle_deg = -180;
  uint8_t buf[2] = {static_cast<uint8_t>(angle_deg & 0xFF), static_cast<uint8_t>((angle_deg >> 8) & 0xFF)};
  return this->write_cmd_(satellite1::audio_cmd::BEAM_ANGLE, buf, sizeof(buf));
}

bool XvfControl::set_aec_mode(satellite1::AecMode mode) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::AEC_TUNING)) {
    return false;
  }
  const uint8_t v = static_cast<uint8_t>(mode);
  return this->write_cmd_(satellite1::audio_cmd::AEC_MODE, &v, 1);
}

bool XvfControl::set_aec_ref_gain_db(int8_t gain_db) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::AEC_TUNING)) {
    return false;
  }
  if (gain_db > 12)
    gain_db = 12;
  if (gain_db < -24)
    gain_db = -24;
  const uint8_t v = static_cast<uint8_t>(gain_db);
  return this->write_cmd_(satellite1::audio_cmd::AEC_REF_GAIN_DB, &v, 1);
}

bool XvfControl::set_agc_enable(bool enable) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::AGC_TUNING)) {
    return false;
  }
  const uint8_t v = enable ? 1 : 0;
  return this->write_cmd_(satellite1::audio_cmd::AGC_ENABLE, &v, 1);
}

bool XvfControl::set_agc_target_dbfs(int8_t target_dbfs) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::AGC_TUNING)) {
    return false;
  }
  if (target_dbfs > 0)
    target_dbfs = 0;
  if (target_dbfs < -60)
    target_dbfs = -60;
  const uint8_t v = static_cast<uint8_t>(target_dbfs);
  return this->write_cmd_(satellite1::audio_cmd::AGC_TARGET_DBFS, &v, 1);
}

bool XvfControl::set_ns_enable(bool enable) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::NS_TUNING)) {
    return false;
  }
  const uint8_t v = enable ? 1 : 0;
  return this->write_cmd_(satellite1::audio_cmd::NS_ENABLE, &v, 1);
}

bool XvfControl::set_ns_level(uint8_t level) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::NS_TUNING)) {
    return false;
  }
  if (level > 3)
    level = 3;
  return this->write_cmd_(satellite1::audio_cmd::NS_LEVEL, &level, 1);
}

bool XvfControl::set_mic_gain_l_db(int8_t gain_db) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::MIC_GAIN_RUNTIME)) {
    return false;
  }
  if (gain_db > 24)
    gain_db = 24;
  if (gain_db < -12)
    gain_db = -12;
  const uint8_t v = static_cast<uint8_t>(gain_db);
  return this->write_cmd_(satellite1::audio_cmd::MIC_GAIN_L, &v, 1);
}

bool XvfControl::set_mic_gain_r_db(int8_t gain_db) {
  if (!this->servicer_present_)
    return false;
  if (!this->has_capability(satellite1::audio_capability::MIC_GAIN_RUNTIME)) {
    return false;
  }
  if (gain_db > 24)
    gain_db = 24;
  if (gain_db < -12)
    gain_db = -12;
  const uint8_t v = static_cast<uint8_t>(gain_db);
  return this->write_cmd_(satellite1::audio_cmd::MIC_GAIN_R, &v, 1);
}

}  // namespace xvf_control
}  // namespace esphome
