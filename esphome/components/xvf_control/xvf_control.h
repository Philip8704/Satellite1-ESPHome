#pragma once

#include "esphome/components/satellite1/satellite1.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include <cstdint>

namespace esphome {
namespace xvf_control {

// Probes the audio DSP servicer at RESID 0xE0 on the XMOS at boot, caches
// the capability bitmask, and exposes typed read/write methods for each
// command in the Audio Servicer protocol (docs/xmos_dsp_control_protocol.md).
//
// If the probe fails (BAD_RESOURCE or anything other than CMD_SUCCESS), the
// component marks itself unsupported and every public setter is a no-op
// returning false. Lambdas in the xvf_control.yaml package can gate visible
// HA entities on is_servicer_present() so a stock-FPH build cleanly hides
// non-functional knobs.
class XvfControl : public Component, public satellite1::Satellite1SPIService {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Probe after the XMOS link is up but before user-facing automations run.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_doa_poll_interval_ms(uint32_t ms) { this->doa_poll_interval_ms_ = ms; }
  void set_capability_probed_trigger(Trigger<uint32_t> *trigger) {
    this->capability_probed_trigger_ = trigger;
  }

  // ----- Probe state ------------------------------------------------------
  bool is_servicer_present() const { return this->servicer_present_; }
  uint32_t get_capability_flags() const { return this->capability_flags_; }
  bool has_capability(uint32_t bit) const { return (this->capability_flags_ & bit) != 0; }
  // Returns 0xFFFFFFFF until the probe runs, then either 0 (stock firmware)
  // or the bitmask. Lambdas can use the sentinel to render "unknown" UI.
  bool probe_complete() const { return this->probe_complete_; }

  // ----- Writes (all return true on CMD_SUCCESS from XMOS, false otherwise)
  // Each write is rejected client-side if servicer isn't present or the
  // matching capability bit isn't set — saves SPI traffic for non-supported
  // commands.
  bool set_beam_mode(satellite1::BeamMode mode);
  bool set_beam_angle_deg(int16_t angle_deg);
  bool set_aec_mode(satellite1::AecMode mode);
  bool set_aec_ref_gain_db(int8_t gain_db);
  bool set_agc_enable(bool enable);
  bool set_agc_target_dbfs(int8_t target_dbfs);
  bool set_ns_enable(bool enable);
  bool set_ns_level(uint8_t level);
  bool set_mic_gain_l_db(int8_t gain_db);
  bool set_mic_gain_r_db(int8_t gain_db);

  // ----- Cached reads ----------------------------------------------------
  // Refreshed by the loop poll at doa_poll_interval. Reading is cheap; the
  // XvfControl object holds the last value, so HA template sensors don't
  // each trigger SPI traffic.
  int16_t get_doa_angle_deg() const { return this->cached_doa_angle_deg_; }
  uint8_t get_doa_confidence() const { return this->cached_doa_confidence_; }
  uint16_t get_dsp_load_q8_8() const { return this->cached_dsp_load_q8_8_; }
  uint16_t get_mic_l_clip_count() const { return this->cached_mic_l_clip_count_; }
  uint16_t get_mic_r_clip_count() const { return this->cached_mic_r_clip_count_; }
  uint8_t get_mic_count() const { return this->cached_mic_count_; }

 protected:
  // Low-level SPI helpers. `transfer_command_` is the single chokepoint
  // every command goes through — keeps logging / error handling consistent.
  bool transfer_command_(uint8_t cmd, uint8_t *payload, uint8_t len, bool is_read);
  bool write_cmd_(uint8_t cmd, const uint8_t *payload, uint8_t len);
  bool read_cmd_(uint8_t cmd, uint8_t *payload_out, uint8_t len);

  void probe_capabilities_();
  void poll_doa_();
  void poll_pipeline_stats_();

  uint32_t doa_poll_interval_ms_{500};
  uint32_t last_poll_ms_{0};
  // 0xFFFFFFFF until probe completes; then either 0 (servicer responds but
  // claims no features) or the real bitmask. Separate `probe_complete_`
  // distinguishes "not probed yet" from "probed = 0".
  uint32_t capability_flags_{0};
  bool servicer_present_{false};
  bool probe_complete_{false};
  uint8_t fw_feature_version_[4]{0, 0, 0, 0};

  // Cached read state — refreshed by loop poll.
  int16_t cached_doa_angle_deg_{0};
  uint8_t cached_doa_confidence_{0};
  uint16_t cached_dsp_load_q8_8_{0};
  uint16_t cached_mic_l_clip_count_{0};
  uint16_t cached_mic_r_clip_count_{0};
  uint8_t cached_mic_count_{0};

  Trigger<uint32_t> *capability_probed_trigger_{nullptr};
};

// ------------------------- Automation trigger ----------------------------
// Wired up by xvf_control/__init__.py: the Python side instantiates this
// trigger with the parent XvfControl pointer, then calls
// set_capability_probed_trigger() so probe_capabilities_() can fire it.
class CapabilityProbedTrigger : public Trigger<uint32_t> {
 public:
  explicit CapabilityProbedTrigger(XvfControl * /*parent*/) {}
};

}  // namespace xvf_control
}  // namespace esphome
