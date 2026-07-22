"""mww_training_capture.

A sibling to micro_wake_word that records short audio snippets whenever the
microWakeWord inference engine produces a "near-miss": the wake-word
probability climbs into a configurable band that's *high enough* to mean the
user probably said something close to the wake word but *low enough* that the
real wake word never triggered.

Captured WAV data is streamed directly to a TaterTotterson microWakeWord
trainer (``/api/upload_captured_audio_raw``) and used as additional training
data for the microWakeWord training notebooks.

Example YAML
------------

mww_training_capture:
  id: mww_capture
  micro_wake_word_id: mww
  microphone:
    microphone: sat1_mics
    channels: 1
    gain_factor: 6
  default_lower_cutoff: 0.40
  pre_buffer_seconds: 2.0
  post_buffer_ms: 500
  cooldown_ms: 4000
  upload_url: http://trainer.local:8789/api/upload_captured_audio_raw
  device_name: satellite1
  models:
    - wake_word_model: hey_jarvis
      lower_cutoff: 0.55
    - wake_word_model: okay_nabu
      lower_cutoff: 0.40
  on_near_miss_detected:
    - logger.log:
        format: "Near miss for %s (max=%.2f avg=%.2f)"
        args: [ wake_word.c_str(), max_prob, avg_prob ]
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE

CODEOWNERS = ["@futureproofhomes"]
DEPENDENCIES = ["micro_wake_word", "microphone", "http_request"]

CONF_MICRO_WAKE_WORD_ID = "micro_wake_word_id"
CONF_DEFAULT_LOWER_CUTOFF = "default_lower_cutoff"
CONF_PRE_BUFFER_SECONDS = "pre_buffer_seconds"
CONF_POST_BUFFER_MS = "post_buffer_ms"
CONF_COOLDOWN_MS = "cooldown_ms"
CONF_REQUIRE_VAD = "require_vad"
CONF_ENABLED_BY_DEFAULT = "enabled_by_default"
CONF_UPLOAD_URL = "upload_url"
CONF_DEVICE_NAME = "device_name"
CONF_MODELS = "models"
CONF_WAKE_WORD_MODEL = "wake_word_model"
CONF_LOWER_CUTOFF = "lower_cutoff"
CONF_ON_NEAR_MISS_DETECTED = "on_near_miss_detected"
CONF_AUDIO_FORMAT = "audio_format"
CONF_DETECTION_PROFILE = "detection_profile"
CONF_NOTES = "notes"
CONF_CAPTURE_WAKE_DETECTED = "capture_wake_detected"
CONF_CAPTURE_CLOSE_MISS = "capture_close_miss"
CONF_DETECTION_COOLDOWN_MS = "detection_cooldown_ms"
CONF_DETECTION_POST_BUFFER_MS = "detection_post_buffer_ms"
CONF_ON_CAPTURE_UPLOADED = "on_capture_uploaded"

mww_training_capture_ns = cg.esphome_ns.namespace("mww_training_capture")
MwwTrainingCapture = mww_training_capture_ns.class_(
    "MwwTrainingCapture", cg.Component
)

EnableAction = mww_training_capture_ns.class_(
    "EnableAction", automation.Action
)
DisableAction = mww_training_capture_ns.class_(
    "DisableAction", automation.Action
)
IsEnabledCondition = mww_training_capture_ns.class_(
    "IsEnabledCondition", automation.Condition
)

# Forward declared in micro_wake_word/__init__.py; pulling it in by hand here
# keeps us decoupled from the load order while still letting ``cv.use_id`` find
# the right cg type.
micro_wake_word_ns = cg.esphome_ns.namespace("micro_wake_word")
MicroWakeWord = micro_wake_word_ns.class_("MicroWakeWord")
WakeWordModel = micro_wake_word_ns.class_("WakeWordModel")


def _validate_cutoff(value):
    """A near-miss cutoff is on a [0.01, 0.99] band. Outside that range it
    means the user wired things wrong (cutoff > probability_cutoff bypasses
    real detections, cutoff = 0 captures every scrap of audio)."""
    value = cv.percentage(value)
    if value <= 0.0 or value >= 1.0:
        raise cv.Invalid("lower_cutoff must be strictly between 0.0 and 1.0")
    return value


def _validate_audio_format(value):
    """The component only knows how to emit a full WAV. ``pcm_s16le`` is a valid
    value on the receiving end (Tater synthesizes the header itself), but shipping
    the key as accepted would make the firmware advertise a body format it doesn't
    actually produce."""
    value = cv.string_strict(value).strip().lower()
    if value == "wav":
        return value
    if value == "pcm_s16le":
        raise cv.Invalid(
            "only 'wav' is currently implemented; the component always emits a "
            "complete WAV (44-byte RIFF header + PCM)"
        )
    raise cv.Invalid(f"unsupported audio_format '{value}' (expected 'wav')")


MODEL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_WAKE_WORD_MODEL): cv.use_id(WakeWordModel),
        cv.Optional(CONF_LOWER_CUTOFF): _validate_cutoff,
    }
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MwwTrainingCapture),
        cv.Required(CONF_MICRO_WAKE_WORD_ID): cv.use_id(MicroWakeWord),
        cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
            min_bits_per_sample=16,
            max_bits_per_sample=16,
            min_channels=1,
            max_channels=1,
        ),
        cv.Optional(CONF_DEFAULT_LOWER_CUTOFF, default=0.40): _validate_cutoff,
        cv.Optional(CONF_PRE_BUFFER_SECONDS, default=2.0): cv.float_range(
            min=0.25, max=5.0
        ),
        cv.Optional(CONF_POST_BUFFER_MS, default=500): cv.int_range(
            min=0, max=2000
        ),
        cv.Optional(CONF_COOLDOWN_MS, default=4000): cv.int_range(
            min=500, max=60000
        ),
        cv.Optional(CONF_REQUIRE_VAD, default=True): cv.boolean,
        cv.Optional(CONF_ENABLED_BY_DEFAULT, default=False): cv.boolean,
        cv.Required(CONF_UPLOAD_URL): cv.string_strict,
        cv.Optional(CONF_DEVICE_NAME, default="unknown_device"): cv.string_strict,
        cv.Optional(CONF_MODELS, default=[]): cv.ensure_list(MODEL_SCHEMA),
        # --- Capture event selection -------------------------------------------
        # wake_detected clips are candidate POSITIVE training samples; close_miss
        # clips are candidate NEGATIVES. Both default on — a trainer needs both.
        cv.Optional(CONF_CAPTURE_WAKE_DETECTED, default=True): cv.boolean,
        cv.Optional(CONF_CAPTURE_CLOSE_MISS, default=True): cv.boolean,
        # Real detections are user-initiated and rare, so they get a shorter
        # cooldown than ambient near-misses (which can flood).
        cv.Optional(CONF_DETECTION_COOLDOWN_MS, default=1500): cv.int_range(
            min=0, max=60000
        ),
        # Short by design: voice_assistant stops micro_wake_word the moment the
        # pipeline starts, so the audio tap goes silent and a long post-roll only
        # adds latency without capturing anything.
        cv.Optional(CONF_DETECTION_POST_BUFFER_MS, default=250): cv.int_range(
            min=0, max=2000
        ),
        # --- Upload metadata ----------------------------------------------------
        cv.Optional(CONF_AUDIO_FORMAT, default="wav"): _validate_audio_format,
        cv.Optional(CONF_DETECTION_PROFILE, default=""): cv.string_strict,
        cv.Optional(CONF_NOTES, default=""): cv.string_strict,
        cv.Optional(CONF_ON_NEAR_MISS_DETECTED): automation.validate_automation(
            single=True
        ),
        cv.Optional(CONF_ON_CAPTURE_UPLOADED): automation.validate_automation(
            single=True
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
            "mww_training_capture", sample_rate=16000
        ),
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mww = await cg.get_variable(config[CONF_MICRO_WAKE_WORD_ID])
    cg.add(var.set_micro_wake_word(mww))

    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))

    cg.add(var.set_default_lower_cutoff(int(config[CONF_DEFAULT_LOWER_CUTOFF] * 255)))
    cg.add(var.set_pre_buffer_seconds(config[CONF_PRE_BUFFER_SECONDS]))
    cg.add(var.set_post_buffer_ms(config[CONF_POST_BUFFER_MS]))
    cg.add(var.set_cooldown_ms(config[CONF_COOLDOWN_MS]))
    cg.add(var.set_require_vad(config[CONF_REQUIRE_VAD]))
    cg.add(var.set_initial_enabled(config[CONF_ENABLED_BY_DEFAULT]))
    cg.add(var.set_upload_url(config[CONF_UPLOAD_URL]))
    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_capture_wake_detected(config[CONF_CAPTURE_WAKE_DETECTED]))
    cg.add(var.set_capture_close_miss(config[CONF_CAPTURE_CLOSE_MISS]))
    cg.add(var.set_detection_cooldown_ms(config[CONF_DETECTION_COOLDOWN_MS]))
    cg.add(var.set_detection_post_buffer_ms(config[CONF_DETECTION_POST_BUFFER_MS]))
    cg.add(var.set_audio_format_header(config[CONF_AUDIO_FORMAT]))
    cg.add(var.set_detection_profile(config[CONF_DETECTION_PROFILE]))
    cg.add(var.set_notes(config[CONF_NOTES]))

    for model_conf in config[CONF_MODELS]:
        wake_word_model = await cg.get_variable(model_conf[CONF_WAKE_WORD_MODEL])
        if CONF_LOWER_CUTOFF in model_conf:
            cutoff = int(model_conf[CONF_LOWER_CUTOFF] * 255)
            cg.add(var.add_model_override(wake_word_model, cutoff))
        else:
            cg.add(var.register_model(wake_word_model))

    if conf := config.get(CONF_ON_NEAR_MISS_DETECTED):
        await automation.build_automation(
            var.get_near_miss_trigger(),
            [
                (cg.std_string, "wake_word"),
                (cg.float_, "max_prob"),
                (cg.float_, "avg_prob"),
            ],
            conf,
        )

    if conf := config.get(CONF_ON_CAPTURE_UPLOADED):
        await automation.build_automation(
            var.get_capture_uploaded_trigger(),
            [
                (cg.std_string, "wake_word"),
                (cg.std_string, "event_type"),
                (cg.float_, "max_prob"),
                (cg.float_, "avg_prob"),
            ],
            conf,
        )


CAPTURE_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(MwwTrainingCapture)}
)


@automation.register_action(
    "mww_training_capture.enable", EnableAction, CAPTURE_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "mww_training_capture.disable", DisableAction, CAPTURE_ACTION_SCHEMA, synchronous=True
)
@automation.register_condition(
    "mww_training_capture.is_enabled", IsEnabledCondition, CAPTURE_ACTION_SCHEMA
)
async def capture_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
