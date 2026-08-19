# Enrolment Mode

Adds an **Enrolment Mode** switch used to build speaker profiles for the voice stack, so
the assistant can tell household members apart.

Estate addition, on `develop-belo`. It does not touch wake word capture:
`mww_training_capture` keeps posting clips to the trainer exactly as before.

---

## Why

Speaker profiles built from a phone recording do not transfer to the room. Measured here:
a phone take scored **0.777** and **0.804** against its own profile, while real turns
through this satellite peaked at **0.629** — so every turn was reported as `Unknown` and
the assistant could never personalise. A profile has to be built from the microphone that
will later be scored, which is this one.

Reading a three minute enrolment script one wake word at a time is miserable, so the
switch keeps the pipeline listening. Each utterance still runs through Assist exactly as
normal and is captured downstream by the voice stack's speech-to-text.

**With the switch off, nothing changes.**

## What it adds

| | |
|---|---|
| `switch.enrolment_mode` | keeps the voice pipeline re-arming while on |
| `web_server` (port 80) | lets the trainer flip that switch remotely |

```
POST http://<satellite>/switch/enrolment_mode/turn_on
POST http://<satellite>/switch/enrolment_mode/turn_off
```

The trainer proxies those calls server-side, so no browser talks to the device and there
is no cross-origin problem.

## Using it

From the wake word trainer's **Voice Enrolment** tab: add the satellite's address, press
**Record**, read the script, press **Finish**. The tab arms and disarms the switch for
you. The switch is also exposed to Home Assistant, so it can be toggled by hand.

## Design notes

- **Written as a package.** This repo already extends `voice_assistant` through inline
  packages, and ESPHome appends trigger lists from packages — so the `on_end` here joins
  the existing one rather than replacing it. Nothing in `voice_assistant.yaml` is edited.
- **`ALWAYS_OFF` on boot.** A satellite that reboots mid-enrolment comes back as a normal
  voice satellite, never one stuck listening.
- **Both paths back off.** The re-arm waits 500 ms, and the error path 3 s, so a failing
  pipeline cannot spin against the server.

## Security

`web_server` is an **unauthenticated control endpoint on the LAN**: anything that can
reach the satellite can toggle its exposed entities. That is normal for ESPHome and it is
how the trainer reaches the switch, but it is a new exposure on a device that previously
only talked outbound.

`config/common/enrollment_mode.yaml` carries a commented-out `auth:` block:

```yaml
web_server:
  auth:
    username: !secret satellite_web_user
    password: !secret satellite_web_password
```

If you enable it, the trainer's relay needs the same credentials.

## Related

- [Voice stack](https://github.com/Philip8704/HA-AI-Framework/tree/master/voice_stack) —
  captures the audio and holds the profiles
- [Wake word trainer](https://github.com/Philip8704/microWakeWord-Trainer-Nvidia-Docker) —
  the Voice Enrolment tab
