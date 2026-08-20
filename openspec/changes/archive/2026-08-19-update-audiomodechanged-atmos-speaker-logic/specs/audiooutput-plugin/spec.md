## MODIFIED Requirements

### Requirement: dolbyAtmosExperience decision logic
The plugin SHALL compute `dolbyAtmosExperience` as `true` when `AtmosCapability == ATMOS_METADATA (2)` AND `soundMode` is one of `{ PASSTHRU, DOLBYDIGITALPLUS, SOUNDMODE_AUTO, SURROUND }`. The result SHALL be `false` for any other combination.

#### Scenario: Atmos metadata present and sound mode is SURROUND
- **WHEN** `AtmosCapability` is `ATMOS_METADATA` AND `soundMode` is `SURROUND`
- **THEN** `dolbyAtmosExperience` SHALL return `true`

#### Scenario: Atmos metadata present and sound mode is PASSTHRU
- **WHEN** `AtmosCapability` is `ATMOS_METADATA` AND `soundMode` is `PASSTHRU`
- **THEN** `dolbyAtmosExperience` SHALL return `true`

#### Scenario: Atmos metadata present and sound mode is DOLBYDIGITALPLUS
- **WHEN** `AtmosCapability` is `ATMOS_METADATA` AND `soundMode` is `DOLBYDIGITALPLUS`
- **THEN** `dolbyAtmosExperience` SHALL return `true`

#### Scenario: Atmos metadata present and sound mode is SOUNDMODE_AUTO
- **WHEN** `AtmosCapability` is `ATMOS_METADATA` AND `soundMode` is `SOUNDMODE_AUTO`
- **THEN** `dolbyAtmosExperience` SHALL return `true`

#### Scenario: Atmos metadata present but sound mode is STEREO
- **WHEN** `AtmosCapability` is `ATMOS_METADATA` AND `soundMode` is `STEREO`
- **THEN** `dolbyAtmosExperience` SHALL return `false`

#### Scenario: Atmos metadata present but sound mode is MONO
- **WHEN** `AtmosCapability` is `ATMOS_METADATA` AND `soundMode` is `MONO`
- **THEN** `dolbyAtmosExperience` SHALL return `false`

### Requirement: OnAudioModeEvent filtering for internal speaker with no external device
When the `OnAudioModeEvent` DS HAL callback fires with a port type of `HDMI_ARC` but only the internal speaker (speaker0) is connected and no external device is present, the plugin SHALL ignore the event and SHALL NOT update `_soundMode` or recompute `dolbyAtmosExperience`.

#### Scenario: AudioModeChanged event for HDMI_ARC when only speaker0 is connected
- **WHEN** `OnAudioModeEvent` fires with `portType == dsAUDIOPORT_TYPE_HDMI_ARC`
- **AND** the only enabled and connected audio output port is the internal speaker (speaker0)
- **AND** no external device is connected to the HDMI_ARC port
- **THEN** the plugin SHALL discard the event without updating sound mode or firing `onDolbyAtmosExperienceChanged`

#### Scenario: AudioModeChanged event for HDMI_ARC when an external device is connected
- **WHEN** `OnAudioModeEvent` fires with `portType == dsAUDIOPORT_TYPE_HDMI_ARC`
- **AND** an external device connected to HDMI_ARC is enabled and connected
- **THEN** the plugin SHALL update `_soundMode`, recompute `dolbyAtmosExperience`, and fire `onDolbyAtmosExperienceChanged` if the value changed
