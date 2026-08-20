## Context

`AudioOutputImplementation` maintains a cached `_soundMode` and `_atmosMetaData` flag, recomputed on every DS HAL event. Two bugs exist in the current logic:

1. **Atmos + Surround**: `EvaluateCurrentAtmosExperience()` originally excluded `SURROUND` from the set of modes that return `true`, even though a device in surround mode with atmos metadata present should report atmos experience as enabled. The fix — adding `SURROUND` to the switch-case — has already landed in `AudioOutputImplementation.cpp`.

2. **Spurious AudioModeChanged events for speaker0**: The DS HAL fires `OnAudioModeEvent` targeting `HDMI_ARC0` when the audio mode is changed from the UI, even if no external device is connected (only the internal speaker, `speaker0`, is active). The existing `onAudioModeChanged` handler iterates all audio output ports and processes the event for any enabled+connected port whose type matches the incoming `portType`. When `speaker0` is the only connected port and no external device is attached, this causes the internal speaker's sound mode to be updated based on an event that was generated for `HDMI_ARC0`, which is incorrect.

## Goals / Non-Goals

**Goals:**
- Ensure `dolbyAtmosExperience` returns `true` for `SURROUND` + atmos metadata.
- Suppress `OnAudioModeEvent` processing for internal speaker (speaker0) when the event originates from `HDMI_ARC0` and no external device is actually connected.
- Keep changes isolated to `AudioOutputImplementation.cpp`; no interface or build changes.

**Non-Goals:**
- Changing how atmos capability is detected from the HAL.
- Modifying behaviour for any other port type (HDMI0, SPDIF, etc.).
- Refactoring the broader event-handling architecture.

## Decisions

### D1: Include SURROUND in EvaluateCurrentAtmosExperience

**Decision**: Add `SURROUND` to the switch-case in `EvaluateCurrentAtmosExperience()` alongside `PASSTHRU`, `DOLBYDIGITALPLUS`, and `SOUNDMODE_AUTO`.

**Rationale**: A device in surround mode that has atmos metadata present is capable of delivering an atmos experience. The prior exclusion was an oversight; the updated spec (REQ-02) now includes `SURROUND`.

**Alternative considered**: Evaluate atmos experience purely from the DS HAL atmos capability flag without consulting sound mode — rejected because sound mode reflects the actual output format selected by the user/platform.

---

### D2: Filter spurious OnAudioModeEvent for speaker0 with no external device

**Decision**: Inside `onAudioModeChanged`, after iterating ports and finding that the only connected port is `speaker0` (internal speaker), skip updating the sound mode if the incoming `portType` is `dsAUDIOPORT_TYPE_HDMI_ARC`. Log a warning for observability.

**Rationale**: The DS HAL emits `OnAudioModeEvent` with `portType = dsAUDIOPORT_TYPE_HDMI_ARC` whenever the audio mode is changed in the UI, regardless of whether an ARC device is actually connected. When speaker0 is the only active output, this event is not meaningful for the internal speaker and should be ignored. The existing code already has a guard that logs a warning when the mode doesn't change for a connected port — this decision extends that intent to cover the speaker0 + no-external-device case explicitly.

**Alternative considered**: Check `aPort.isConnected()` more granularly per port — already done, but `speaker0` still shows as `connected` (it's always present); the key discriminant is the mismatch between the event's port type (`HDMI_ARC`) and the actual active port (`speaker0`).

## Risks / Trade-offs

- **Risk**: Future platforms where speaker0 *should* respond to HDMI_ARC mode-change events.  
  → **Mitigation**: The filter is conditioned on `portType == dsAUDIOPORT_TYPE_HDMI_ARC` AND the matched port being of speaker type; it is narrow and platform-specific behaviour can override via DS HAL connection state.

- **Risk**: Adding `SURROUND` to atmos evaluation could falsely report atmos experience on devices that report surround mode but lack a true atmos path.  
  → **Mitigation**: Step 1 still requires `AtmosCapability == ATMOS_METADATA`; surround alone is insufficient.
