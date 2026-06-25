## Context

The `entservices-audiooutput` repository is a new, empty Thunder plugin repository. The goal is to introduce the first plugin: `AudioOutput`, registered under callsign `org.rdk.AudioOutput`.

The plugin's primary purpose is to determine and expose the **Dolby Atmos Experience** state — a boolean value computed from two underlying device queries that currently live in `entservices-playerinfo`:

1. `AtmosMetadata` — queries the sink device's ATMOS capability via the DeviceSettings HAL (`dshal`).
2. `SoundMode` — queries the current stereo mode of the active audio output port via the DeviceSettings HAL.

Both of these HAL queries are copied as private implementation methods into `AudioOutputImplementation`. They are **not** re-exposed as individual public APIs; they serve only the internal computation of `dolbyAtmosExperience`.

The plugin follows the established two-library pattern used in `entservices-migration`: a thin plugin shell (`AudioOutput.so`) that handles lifecycle and JSON-RPC registration, and a separate out-of-process implementation library (`AudioOutputImplementation.so`) that contains the business logic.

**Reference implementations used:**
- Plugin shell pattern: `entservices-migration/plugin/Migration.cpp`
- Business logic pattern: `entservices-migration/plugin/MigrationImplementation.cpp`
- HAL queries: `entservices-playerinfo/plugin/DeviceSettings/PlatformImplementation.cpp` (`AtmosMetadata`, `SoundMode`)

## Goals / Non-Goals

**Goals:**
- Introduce the `org.rdk.AudioOutput` Thunder plugin in this repository.
- Expose `dolbyAtmosExperience` as a single JSON-RPC method and COM-RPC interface method.
- Fire `onDolbyAtmosExperienceChanged` notifications to all subscribers when the state changes.
- Copy `AtmosMetadata` and `SoundMode` HAL query logic verbatim from `entservices-playerinfo` as private methods.
- Subscribe to `AtmosCapabilityChanged` (DisplaySettings) and `audioModeChanged` (PlayerInfo) COM-RPC events to maintain cache and trigger notifications.
- Run out-of-process (mode `LOCAL`) by default for isolation.

**Non-Goals:**
- Do NOT remove or modify `PlayerInfo.AtmosMetadata` or `PlayerInfo.soundMode`.
- Do NOT expose `AtmosMetadata` or `SoundMode` as individual public APIs on `IAudioOutput`.
- Do NOT use JSON-RPC or `LinkType` for inter-plugin communication.
- Do NOT introduce additional audio output APIs in this change (future scope).

## Decisions

### Decision 1: Copy HAL queries rather than delegate via COM-RPC to PlayerInfo

**Choice:** Copy `AtmosMetadata` and `SoundMode` logic from `PlatformImplementation.cpp` directly into `AudioOutputImplementation` as private methods.

**Rationale:** The spec explicitly states "Do not call these APIs directly from this new API." Delegating to PlayerInfo via COM-RPC would create a runtime dependency on PlayerInfo being active, introduce unnecessary IPC latency for a simple HAL call, and couple two plugins that should be independent. Direct HAL access via DeviceSettings is the correct pattern for implementation libraries in this architecture.

**Alternative considered:** Query `Exchange::Dolby::IOutput` via COM-RPC on PlayerInfo. Rejected because it introduces IPC overhead and a hard runtime dependency on PlayerInfo's in-process state.

---

### Decision 2: Use Exchange::Dolby::IOutput::INotification for change events

**Choice:** Subscribe to `Exchange::Dolby::IOutput::INotification::AudioModeChanged` (from PlayerInfo) and `Exchange::IDisplaySettings` (or DisplaySettings equivalent) for `AtmosCapabilityChanged` via COM-RPC event registration.

**Rationale:** The `AudioModeChanged` event is already emitted by `PlayerInfoImplementation` whenever the audio mode changes (see `audiomodeChanged()` in `PlatformImplementation.cpp`). Subscribing via the COM-RPC notification interface is the correct, low-latency, in-process-friendly approach.

**For `AtmosCapabilityChanged`:** Subscribing to the DisplaySettings plugin's notification interface via COM-RPC follows the same pattern.

---

### Decision 3: Out-of-process plugin (LOCAL mode)

**Choice:** Default plugin mode is `LOCAL` (out-of-process).

**Rationale:** Consistent with other entservices plugins. Provides process isolation — a crash in the implementation does not bring down the Thunder framework. The plugin shell handles `RPC::IRemoteConnection::INotification` for crash recovery.

---

### Decision 4: Internal cache for both values

**Choice:** `AudioOutputImplementation` maintains `_atmosCapability` and `_soundMode` as cached member variables, initialised at startup and updated on every event.

**Rationale:** Avoids repeated HAL calls on every `dolbyAtmosExperience` query. The HAL calls in `AtmosMetadata` and `SoundMode` iterate over all audio ports — caching is essential for performance. When an event arrives, only the changed value updates; the other is read from cache to compute the new combined result.

---

### Decision 5: Interface — IAudioOutput in ThunderInterfaces

**Choice:** Create `Exchange::IAudioOutput` in ThunderInterfaces with `DolbyAtmosExperience(bool& enabled)`, `Register(INotification*)`, `Unregister(INotification*)`, and nested `INotification` with `OnDolbyAtmosExperienceChanged(bool)`.

**Rationale:** Follows the established Thunder interface pattern. Autogenerated JSON-RPC stubs (`JAudioOutput.h`) are then used in the plugin shell to avoid manual JSON-RPC handler registration.

## Risks / Trade-offs

| Risk | Mitigation |
|------|-----------|
| DisplaySettings `AtmosCapabilityChanged` event interface not yet defined in `Exchange::IDisplaySettings` | Verify interface availability; if absent, raise a ThunderInterfaces PR to add it before implementation starts (tracked as OQ-02 in spec) |
| PlayerInfo or DisplaySettings not yet activated when AudioOutput initialises | Use `PluginHost::IShell::INotification` (`StateChange`) to detect activation and register late; initialise cache to `false` until both values are available |
| HAL query copy drift — if PlayerInfo's `AtmosMetadata` or `SoundMode` logic is updated, AudioOutput's copy becomes stale | Acceptable risk; the spec notes these APIs may be deprecated in future. Document the source reference clearly in the code |
| `ID_AUDIO_OUTPUT` not yet allocated in `interfaces/Ids.h` | Must be allocated in ThunderInterfaces before the interface header can be compiled; tracked as OQ-03 in spec |
| Thread safety — event callbacks arrive on a different thread from client JSON-RPC calls | Protect all cache reads and writes with `Core::CriticalSection _adminLock` |

## Open Questions

- **OQ-02**: What is the exact COM-RPC event/method name on `Exchange::IDisplaySettings` for `AtmosCapabilityChanged`? Must be confirmed before implementing the DisplaySettings subscription.
- **OQ-03**: What are the next available IDs in `interfaces/Ids.h` for `ID_AUDIO_OUTPUT` and `ID_AUDIO_OUTPUT_NOTIFICATION`?
- **OQ-04**: Does the `AtmosCapabilityChanged` event payload carry the new capability value, or must we re-query after receiving it?
