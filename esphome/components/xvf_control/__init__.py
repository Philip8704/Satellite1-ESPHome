"""xvf_control — Satellite1 XMOS DSP runtime control plane.

Talks to the audio DSP servicer at RESID 0xE0 over the existing Satellite1
SPI link. The servicer is implemented in a Phase 3 fork of the
Satellite1-XMOS firmware (see docs/xmos_dsp_control_protocol.md). Stock
FPH builds do not implement it; this component probes for the servicer at
boot and self-disables when CAPABILITY_FLAGS returns BAD_RESOURCE, so it's
safe to include in a build that may run against either firmware.

Example YAML
------------

xvf_control:
  id: xvf
  satellite1_id: satellite1_id
  doa_poll_interval: 500ms
  on_capability_probed:
    - logger.log:
        format: "XVF capability flags=0x%08X"
        args: [ caps ]
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

# The satellite1 component's __init__.py imports its satellite1.py module as
# `sat`, but doesn't re-export the symbols. We reach into the submodule
# directly so the names match the satellite1.py source.
from esphome.components.satellite1.satellite1 import (
    CONF_SATELLITE1,
    Satellite1,
    Satellite1SPIService,
    namespace as satellite1_ns,
)

CODEOWNERS = ["@futureproofhomes"]
DEPENDENCIES = ["satellite1"]

CONF_DOA_POLL_INTERVAL = "doa_poll_interval"
CONF_ON_CAPABILITY_PROBED = "on_capability_probed"

xvf_control_ns = cg.esphome_ns.namespace("xvf_control")
XvfControl = xvf_control_ns.class_(
    "XvfControl", cg.Component, Satellite1SPIService
)

CapabilityProbedTrigger = xvf_control_ns.class_(
    "CapabilityProbedTrigger", automation.Trigger.template(cg.uint32)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(XvfControl),
        cv.Required(CONF_SATELLITE1): cv.use_id(Satellite1),
        # 0 disables DOA polling entirely (useful when the servicer is known
        # not to support DOA, or to reduce SPI traffic).
        cv.Optional(CONF_DOA_POLL_INTERVAL, default="500ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(milliseconds=60000)),
        ),
        cv.Optional(CONF_ON_CAPABILITY_PROBED): automation.validate_automation(
            {cv.GenerateID(): cv.declare_id(CapabilityProbedTrigger)},
            single=True,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    sat1 = await cg.get_variable(config[CONF_SATELLITE1])
    cg.add(var.set_parent(sat1))

    cg.add(var.set_doa_poll_interval_ms(int(config[CONF_DOA_POLL_INTERVAL].total_milliseconds)))

    if conf := config.get(CONF_ON_CAPABILITY_PROBED):
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.uint32, "caps")],
            conf,
        )
        cg.add(var.set_capability_probed_trigger(trigger))
