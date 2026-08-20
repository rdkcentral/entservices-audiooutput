## Why

The `DolbyAtmosExperience` evaluation logic excluded `SURROUND` mode, causing atmos experience to be reported as `false` when the device is in surround mode even if atmos metadata is present. Additionally, the `onAudioModeChanged` HAL callback fires for `HDMI_ARC0` even when only the internal speaker (speaker0) is connected; processing this event in that scenario causes incorrect sound-mode updates for the internal speaker.

## What Changes

- **Atmos experience**: Expand `EvaluateCurrentAtmosExperience()` to return `true` for `SURROUND` mode in addition to `DOLBYDIGITALPLUS`, `PASSTHRU`, and `SOUNDMODE_AUTO`.
- **AudioModeChanged event filtering**: In `onAudioModeChanged`, ignore the event when the port type resolves only to `speaker0` (internal speaker) and no external device is connected — the event originates from `HDMI_ARC0` and is not applicable to the internal speaker.
- Update the inline comment in `EvaluateCurrentAtmosExperience()` to reflect the updated sound-mode list.

## Capabilities

### New Capabilities
<!-- none introduced; changes are corrections to existing behaviour -->

### Modified Capabilities
- `audiooutput-plugin`: Atmos experience evaluation now includes `SURROUND` mode; `onAudioModeChanged` event is silently ignored for internal speaker (speaker0) when no external device is connected and the event source is `HDMI_ARC0`.

## Impact

- `plugin/AudioOutputImplementation.cpp`: `EvaluateCurrentAtmosExperience()` switch-case and `onAudioModeChanged()` filtering logic.
- No API surface changes — `DolbyAtmosExperience` and `SetSoundMode` interfaces are unchanged.
- No new dependencies.
