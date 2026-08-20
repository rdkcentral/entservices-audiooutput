## 1. Atmos Experience – Include SURROUND Mode

- [ ] 1.1 Verify `SURROUND` is present in the switch-case of `EvaluateCurrentAtmosExperience()` in `plugin/AudioOutputImplementation.cpp`
- [ ] 1.2 Update the inline comment above `EvaluateCurrentAtmosExperience()` to list `SURROUND` alongside `PASSTHRU`, `DOLBYDIGITALPLUS`, and `SOUNDMODE_AUTO`
- [ ] 1.3 Confirm no other code paths hard-code the set of modes that trigger atmos experience

## 2. OnAudioModeChanged – Ignore Event for Internal Speaker with No External Device

- [ ] 2.1 In `onAudioModeChanged()`, after the port-type match, add a check: if the matched port is speaker0 (internal speaker) and the incoming `portType` is `dsAUDIOPORT_TYPE_HDMI_ARC`, skip processing the event
- [ ] 2.2 Add a `TRACE(Trace::Warning, ...)` log message when the event is discarded (port mismatch: HDMI_ARC event with only speaker0 connected)
- [ ] 2.3 Verify the existing `isAudioModeChanged` flag correctly stays `false` in the skip path so `_soundMode` and `dolbyAtmosExperience` are not updated

## 3. Unit Tests (L1)

- [ ] 3.1 Add test: `DolbyAtmosExperience` returns `true` when atmos metadata is present and sound mode is `SURROUND`
- [ ] 3.2 Add test: `DolbyAtmosExperience` returns `false` when atmos metadata is present and sound mode is `STEREO` (regression)
- [ ] 3.3 Add test: `onAudioModeChanged` with `portType == HDMI_ARC` and only speaker0 connected does NOT update sound mode
- [ ] 3.4 Add test: `onAudioModeChanged` with `portType == HDMI_ARC` and an external device connected DOES update sound mode

## 4. Spec Alignment Verification

- [ ] 4.1 Cross-check `EvaluateCurrentAtmosExperience()` against updated REQ-02 in the delta spec (SURROUND included)
- [ ] 4.2 Cross-check `onAudioModeChanged()` filtering logic against the new `OnAudioModeEvent filtering` requirement in the delta spec
