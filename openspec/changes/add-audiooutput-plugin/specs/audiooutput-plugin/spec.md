## ADDED Requirements

### Requirement: Plugin registers under callsign org.rdk.AudioOutput
The `AudioOutput` Thunder plugin SHALL register under the callsign `org.rdk.AudioOutput` and SHALL be activatable by the Thunder framework.

#### Scenario: Plugin activation
- **WHEN** the Thunder framework activates `org.rdk.AudioOutput`
- **THEN** the plugin SHALL initialise successfully and return an empty string from `Initialize()`

#### Scenario: Plugin activation failure
- **WHEN** the `AudioOutputImplementation` object cannot be acquired during `Initialize()`
- **THEN** `Initialize()` SHALL return a non-empty error message string

---

### Requirement: dolbyAtmosExperience method returns correct boolean
The plugin SHALL expose a JSON-RPC method `org.rdk.AudioOutput.dolbyAtmosExperience` that returns a boolean indicating whether Dolby Atmos Experience is currently enabled.

#### Scenario: Atmos fully enabled
- **WHEN** the device's ATMOS capability is `ATMOS_METADATA (2)` AND the current sound mode is one of `PASSTHRU`, `DOLBYDIGITALPLUS`, or `SOUNDMODE_AUTO`
- **THEN** `dolbyAtmosExperience` SHALL return `true`

#### Scenario: Atmos not supported
- **WHEN** the device's ATMOS capability is `NOT_SUPPORTED (0)`
- **THEN** `dolbyAtmosExperience` SHALL return `false` regardless of the current sound mode

#### Scenario: DDPLUS stream only
- **WHEN** the device's ATMOS capability is `DDPLUS_STREAM (1)`
- **THEN** `dolbyAtmosExperience` SHALL return `false` regardless of the current sound mode

#### Scenario: Atmos capable but unsupported sound mode
- **WHEN** the device's ATMOS capability is `ATMOS_METADATA (2)` AND the current sound mode is one of `MONO`, `STEREO`, `SURROUND`, `DOLBYDIGITAL`, or `UNKNOWN`
- **THEN** `dolbyAtmosExperience` SHALL return `false`

#### Scenario: JSON-RPC response format
- **WHEN** a client calls `org.rdk.AudioOutput.dolbyAtmosExperience` via JSON-RPC
- **THEN** the response SHALL be `{"jsonrpc":"2.0","id":<id>,"result":true}` or `{"jsonrpc":"2.0","id":<id>,"result":false}`

#### Scenario: Error propagation
- **WHEN** the underlying HAL query fails
- **THEN** `dolbyAtmosExperience` SHALL return `Core::ERROR_GENERAL`, propagated as a Firebolt error

---

### Requirement: AtmosMetadata HAL query is a private internal method
The implementation SHALL contain a private method equivalent to `AtmosMetadata(bool& supported)` copied from `entservices-playerinfo/plugin/DeviceSettings/PlatformImplementation.cpp`. This method SHALL NOT be exposed via `IAudioOutput` or any public interface.

#### Scenario: HDMI_ARC port present and connected
- **WHEN** the device has an HDMI_ARC audio output port and it is connected
- **THEN** the `AtmosMetadata` private method SHALL query the atmos capability of the HDMI_ARC port

#### Scenario: HDMI_ARC not present, HDMI0 connected
- **WHEN** no HDMI_ARC port is present and HDMI0 is connected
- **THEN** the `AtmosMetadata` private method SHALL query the atmos capability of HDMI0

#### Scenario: No port connected
- **WHEN** no connected audio output port is found
- **THEN** the `AtmosMetadata` private method SHALL fall back to `getSinkDeviceAtmosCapability()` on the host

#### Scenario: ATMOS_METADATA capability
- **WHEN** `getSinkDeviceAtmosCapability` returns `dsAUDIO_ATMOS_ATMOSMETADATA`
- **THEN** the private method SHALL set `supported = true`

#### Scenario: Other capability values
- **WHEN** `getSinkDeviceAtmosCapability` returns any value other than `dsAUDIO_ATMOS_ATMOSMETADATA`
- **THEN** the private method SHALL set `supported = false`

---

### Requirement: SoundMode HAL query is a private internal method
The implementation SHALL contain a private method equivalent to `SoundMode(Exchange::Dolby::IOutput::SoundModes& mode)` copied from `entservices-playerinfo/plugin/DeviceSettings/PlatformImplementation.cpp`. This method SHALL NOT be exposed via `IAudioOutput` or any public interface.

#### Scenario: HDMI_ARC port takes highest precedence
- **WHEN** an enabled and connected HDMI_ARC port exists
- **THEN** the `SoundMode` private method SHALL use that port to determine the sound mode

#### Scenario: HDMI port fallback
- **WHEN** no HDMI_ARC port is connected but an HDMI port is connected
- **THEN** the `SoundMode` private method SHALL use the HDMI port

#### Scenario: Speaker/SPDIF/Headphone fallback
- **WHEN** neither HDMI_ARC nor HDMI ports are connected
- **THEN** the `SoundMode` private method SHALL fall through SPEAKER, SPDIF, and HEADPHONE in precedence order

#### Scenario: Auto mode for ARC/SPDIF
- **WHEN** the selected port is HDMI_ARC or SPDIF and `getStereoAuto()` returns true
- **THEN** the `SoundMode` private method SHALL return `SOUNDMODE_AUTO`

#### Scenario: No connected port
- **WHEN** no enabled and connected audio output port is found
- **THEN** the `SoundMode` private method SHALL return `UNKNOWN` and `dolbyAtmosExperience` SHALL return `false`

---

### Requirement: onDolbyAtmosExperienceChanged notification is sent when state changes
The plugin SHALL send an `onDolbyAtmosExperienceChanged` JSON-RPC notification to all registered subscribers whenever the computed `dolbyAtmosExperience` value transitions between `true` and `false`.

#### Scenario: Notification on true-to-false transition
- **WHEN** the cached `dolbyAtmosExperience` was `true` and a new event causes it to be recomputed as `false`
- **THEN** the plugin SHALL send `{"jsonrpc":"2.0","id":0,"method":"org.rdk.AudioOutput.onDolbyAtmosExperienceChanged","params":{"dolbyAtmosExperience":false}}` to all subscribers

#### Scenario: Notification on false-to-true transition
- **WHEN** the cached `dolbyAtmosExperience` was `false` and a new event causes it to be recomputed as `true`
- **THEN** the plugin SHALL send `{"jsonrpc":"2.0","id":0,"method":"org.rdk.AudioOutput.onDolbyAtmosExperienceChanged","params":{"dolbyAtmosExperience":true}}` to all subscribers

#### Scenario: No notification when value is unchanged
- **WHEN** an event is received but the recomputed `dolbyAtmosExperience` value is the same as the cached value
- **THEN** the plugin SHALL NOT send a notification

---

### Requirement: Subscribe to AtmosCapabilityChanged event from DisplaySettings
The implementation SHALL register as a notification listener on `DisplaySettings` (via COM-RPC) to receive the `AtmosCapabilityChanged` event and update the internal `_atmosCapability` cache.

#### Scenario: DisplaySettings active at startup
- **WHEN** `org.rdk.DisplaySettings` is already in `ACTIVATED` state when `AudioOutputImplementation` initialises
- **THEN** the implementation SHALL immediately acquire the `IDisplaySettings` interface and register its notification listener

#### Scenario: DisplaySettings activates after startup
- **WHEN** `org.rdk.DisplaySettings` transitions to `ACTIVATED` after `AudioOutputImplementation` has already initialised
- **THEN** the implementation SHALL detect this via `StateChange()` and register its notification listener at that point

#### Scenario: AtmosCapabilityChanged event received
- **WHEN** the `AtmosCapabilityChanged` notification fires
- **THEN** the implementation SHALL update `_atmosCapability` cache and recompute `dolbyAtmosExperience` using the cached `_soundMode` value

#### Scenario: DisplaySettings deactivated
- **WHEN** `org.rdk.DisplaySettings` transitions to `DEACTIVATED`
- **THEN** the implementation SHALL unregister its notification listener and release the `IDisplaySettings` interface pointer

---

### Requirement: Subscribe to audioModeChanged event from PlayerInfo
The implementation SHALL register as a notification listener on `PlayerInfo` (via COM-RPC) to receive the `AudioModeChanged` event and update the internal `_soundMode` cache.

#### Scenario: PlayerInfo active at startup
- **WHEN** `org.rdk.PlayerInfo` is already in `ACTIVATED` state when `AudioOutputImplementation` initialises
- **THEN** the implementation SHALL immediately acquire the `Exchange::Dolby::IOutput` interface and register its notification listener

#### Scenario: PlayerInfo activates after startup
- **WHEN** `org.rdk.PlayerInfo` transitions to `ACTIVATED` after `AudioOutputImplementation` has already initialised
- **THEN** the implementation SHALL detect this via `StateChange()` and register its notification listener at that point

#### Scenario: AudioModeChanged event received
- **WHEN** the `AudioModeChanged` notification fires with a new sound mode
- **THEN** the implementation SHALL update `_soundMode` cache and recompute `dolbyAtmosExperience` using the cached `_atmosCapability` value

#### Scenario: PlayerInfo deactivated
- **WHEN** `org.rdk.PlayerInfo` transitions to `DEACTIVATED`
- **THEN** the implementation SHALL unregister its notification listener and release the interface pointer

---

### Requirement: Internal cache initialised at startup
The implementation SHALL fetch the initial `AtmosCapability` and `SoundMode` values at startup and populate the internal cache before processing any client requests.

#### Scenario: Both plugins available at startup
- **WHEN** both `PlayerInfo` and `DisplaySettings` are `ACTIVATED` when `AudioOutputImplementation` initialises
- **THEN** the implementation SHALL call the private `AtmosMetadata()` and `SoundMode()` methods to populate `_atmosCapability` and `_soundMode` before returning from `Initialize()`

#### Scenario: One or both plugins unavailable at startup
- **WHEN** one or both of `PlayerInfo` or `DisplaySettings` are not yet `ACTIVATED` at initialisation time
- **THEN** the implementation SHALL initialise the cache to safe default values (`_atmosCapability = false`, `_soundMode = UNKNOWN`) and populate correct values when the plugins activate via `StateChange()`

---

### Requirement: Thread safety for cache access
The implementation SHALL be thread-safe. All reads and writes to the internal cache (`_atmosCapability`, `_soundMode`, `_dolbyAtmosExperience`) SHALL be protected by a `Core::CriticalSection`.

#### Scenario: Concurrent event and query
- **WHEN** an event callback updates the cache concurrently with a client's `dolbyAtmosExperience` JSON-RPC call
- **THEN** the result SHALL be consistent and no data race SHALL occur

---

### Requirement: COM-RPC only for inter-plugin communication
The implementation SHALL use COM-RPC (`QueryInterfaceByCallsign`) exclusively for all inter-plugin communication. JSON-RPC calls and `LinkType` usage for inter-plugin calls are PROHIBITED.

#### Scenario: Acquiring PlayerInfo interface
- **WHEN** the implementation needs to access PlayerInfo
- **THEN** it SHALL call `_service->QueryInterfaceByCallsign<Exchange::Dolby::IOutput>("org.rdk.PlayerInfo")`

#### Scenario: Acquiring DisplaySettings interface
- **WHEN** the implementation needs to access DisplaySettings
- **THEN** it SHALL call `_service->QueryInterfaceByCallsign` with the appropriate `IDisplaySettings` interface type

---

### Requirement: Plugin runs out-of-process by default
The plugin SHALL default to `LOCAL` (out-of-process) execution mode.

#### Scenario: Default mode in config
- **WHEN** no override is provided at build time
- **THEN** `PLUGIN_AUDIOOUTPUT_MODE` SHALL default to `LOCAL` and the `.conf.in` file SHALL reflect this

#### Scenario: Out-of-process crash recovery
- **WHEN** the `AudioOutputImplementation` process crashes
- **THEN** the plugin shell SHALL detect the disconnection via `RPC::IRemoteConnection::INotification::Activated` and deactivate cleanly

---

### Requirement: PlayerInfo APIs are not removed
`PlayerInfo.AtmosMetadata` and `PlayerInfo.soundMode` SHALL continue to exist in the `entservices-playerinfo` repository and SHALL NOT be removed or deprecated as part of this change.

#### Scenario: PlayerInfo APIs still accessible
- **WHEN** a client calls `org.rdk.PlayerInfo.AtmosMetadata` or `org.rdk.PlayerInfo.soundMode`
- **THEN** those methods SHALL continue to function exactly as before
