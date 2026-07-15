# AudioOutput Plugin Specification

## Overview

A new Thunder plugin `AudioOutput` is introduced to expose the Dolby Atmos Experience capability of the device. It combines the logic of `PlayerInfo.AtmosMetadata` and `PlayerInfo.soundMode` into a single boolean API (`dolbyAtmosExperience`) and notifies all subscribers when the experience state changes via `onDolbyAtmosExperienceChanged`.

> **Note:** `PlayerInfo.AtmosMetadata` and `PlayerInfo.soundMode` must continue to exist and must NOT be removed.

---

## Description

The `AudioOutput` plugin is a WPEFramework (Thunder) service registered under the callsign `org.rdk.AudioOutput`. It is the canonical place for audio output related capabilities exposed to Firebolt clients.

The initial capability is **Dolby Atmos Experience** determination. Rather than requiring clients to call two separate PlayerInfo APIs and combine the results themselves, `AudioOutput` exposes a single boolean method `dolbyAtmosExperience` that encapsulates the combination logic internally. The plugin maintains an internal cache of the two underlying values (`AtmosCapability` and `soundMode`) and recomputes the combined result whenever either value changes via DS HAL event callbacks registered through `device::Host::IAudioOutputPortEvents`.

### Plugin Structure

Following the pattern established by `entservices-migration`:

```
plugin/
├── AudioOutput.h                  ← Plugin shell header
├── AudioOutput.cpp                ← Plugin shell: lifecycle + JSONRPC registration
├── AudioOutputImplementation.h    ← Implementation header
├── AudioOutputImplementation.cpp  ← Business logic, caching, event handling
├── Module.h                       ← MODULE_NAME definition (Plugin_AudioOutput)
├── Module.cpp                     ← MODULE_NAME_DECLARATION
├── CMakeLists.txt                 ← Build: two targets (plugin + implementation)
├── AudioOutput.conf.in            ← Runtime config (callsign, mode, locator)
└── AudioOutput.config             ← CMake-time config (callsign, preconditions)
```

### dolbyAtmosExperience Decision Logic

The result is computed by combining two values: **AtmosCapability** (from PlayerInfo) and **soundMode** (from PlayerInfo).

```
Step 1: Check AtmosCapability
───────────────────────────────────────────────────────────
  AtmosCapability == NOT_SUPPORTED (0)  →  return false
  AtmosCapability == DDPLUS_STREAM  (1) →  return false
  AtmosCapability == ATMOS_METADATA (2) →  proceed to Step 2

Step 2: Check soundMode
───────────────────────────────────────────────────────────
  soundMode ∈ { MONO, STEREO, SURROUND, DOLBYDIGITAL, UNKNOWN }
                                        →  return false

  soundMode ∈ { PASSTHRU, DOLBYDIGITALPLUS, SOUNDMODE_AUTO }
                                        →  return true
```

Both values are obtained by direct HAL queries via `device::Host` (DeviceSettings) at startup, then kept in cache and refreshed on every relevant DS HAL event.

### Cache and Event Flow

```
                ┌──────────────────────────────┐
                │  AudioOutputImplementation   │
                │                              │
                │  Cache:                      │
                │    _atmosMetaData            │
                │    _soundMode                │
                │    _dolbyAtmosExperience     │
                └──────────────┬───────────────┘
                               │
          ┌────────────────────┼─────────────────────┐
          │                                           │
          ▼                                           ▼
 DS HAL: OnDolbyAtmosCapabilitiesChanged    DS HAL: OnAudioModeEvent
 (device::Host::IAudioOutputPortEvents)    (device::Host::IAudioOutputPortEvents)
          │                                           │
          ▼                                           ▼
  Update _atmosMetaData                    Update _soundMode
  Recompute using cached _soundMode        Recompute using cached _atmosMetaData
          │                                           │
          └────────────────┬──────────────────────────┘
                           │
                           ▼
              Did _dolbyAtmosExperience change?
                 YES → fire OnDolbyAtmosExperienceChanged
                 NO  → do nothing
```

---

## Requirements

### Functional Requirements

- **REQ-01**: The plugin MUST expose a method `dolbyAtmosExperience` under callsign `org.rdk.AudioOutput` that returns a boolean.
- **REQ-02**: `dolbyAtmosExperience` MUST return `true` only when `AtmosCapability == ATMOS_METADATA (2)` AND `soundMode` is one of `{ PASSTHRU, DOLBYDIGITALPLUS, SOUNDMODE_AUTO }`.
- **REQ-03**: `dolbyAtmosExperience` MUST return `false` when `AtmosCapability` is `NOT_SUPPORTED (0)` or `DDPLUS_STREAM (1)`, regardless of soundMode.
- **REQ-04**: `dolbyAtmosExperience` MUST return `false` when `AtmosCapability == ATMOS_METADATA` but `soundMode` is one of `{ MONO, STEREO, SURROUND, DOLBYDIGITAL, UNKNOWN }`.
- **REQ-05**: The plugin MUST send an `onDolbyAtmosExperienceChanged` notification to all subscribers whenever the computed `dolbyAtmosExperience` value changes.
- **REQ-06**: The notification payload MUST include a `dolbyAtmosExperience` boolean field.
- **REQ-07**: The plugin MUST listen to the `OnDolbyAtmosCapabilitiesChanged(dsATMOSCapability_t, bool)` DS HAL event via `device::Host::IAudioOutputPortEvents` to detect changes in AtmosCapability.
- **REQ-08**: The plugin MUST listen to the `OnAudioModeEvent(dsAudioPortType_t, dsAudioStereoMode_t)` DS HAL event via `device::Host::IAudioOutputPortEvents` to detect changes in soundMode.
- **REQ-09**: The plugin MUST call `device::Manager::Initialize()` at construction and query initial `AtmosCapability` and `soundMode` values directly from the DS HAL via `device::Host` before processing any client requests.
- **REQ-10**: The plugin MUST NOT remove or modify `PlayerInfo.AtmosMetadata` or `PlayerInfo.soundMode`.
- **REQ-11**: The plugin MUST return `Core::ERROR_NONE` on success and propagate errors appropriately.
- **REQ-12**: The plugin MUST be thread-safe; a `Core::CriticalSection` MUST protect all cache reads and writes.
- **REQ-13**: The plugin MUST run in-process (mode `Off`) by default.

### Interface Requirements

- **REQ-14**: A new Thunder interface `Exchange::IAudioOutput` MUST be created in ThunderInterfaces.
- **REQ-15**: `IAudioOutput` MUST expose `DolbyAtmosExperience(bool& enabled)`, `Register(INotification*)`, and `Unregister(INotification*)`.
- **REQ-16**: `IAudioOutput::INotification` MUST expose `OnDolbyAtmosExperienceChanged(bool dolbyAtmosExperience)`.
- **REQ-17**: JSON-RPC autogenerated stubs (`JAudioOutput.h`, `JsonData_AudioOutput.h`) MUST be generated from `IAudioOutput.h` via the Thunder code generator.
- **REQ-18**: `ID_AUDIO_OUTPUT` and `ID_AUDIO_OUTPUT_NOTIFICATION` MUST be allocated in `interfaces/Ids.h` in ThunderInterfaces.

### Build Requirements

- **REQ-19**: The build system MUST produce two shared libraries: `lib${NAMESPACE}AudioOutput.so` (plugin shell) and `lib${NAMESPACE}AudioOutputImplementation.so` (business logic).
- **REQ-20**: All CMake targets MUST use `${NAMESPACE}` instead of hardcoded framework names.
- **REQ-21**: `MODULE_NAME` MUST be defined as `Plugin_AudioOutput` in `Module.h`.

---

## Architecture / Design

### Component Structure

```
┌──────────────────────────────────────────────────────────────────┐
│                  Thunder Framework (in-process)                  │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │        AudioOutput Plugin Shell (JSONRPC)                  │  │
│  │  ┌──────────────────────────────────────────────────────┐  │  │
│  │  │   AudioOutput.cpp/.h                                 │  │  │
│  │  │   - PluginHost::IPlugin, PluginHost::JSONRPC         │  │  │
│  │  │   - Exchange::JAudioOutput::Register/Unregister      │  │  │
│  │  │   - Exchange::IConfiguration::Configure              │  │  │
│  │  └──────────────────────────────────────────────────────┘  │  │
│  │                    │ direct call (in-process)               │  │
│  │                    ▼                                        │  │
│  │  ┌──────────────────────────────────────────────────────┐  │  │
│  │  │  AudioOutputImplementation.cpp/.h                    │  │  │
│  │  │  - Exchange::IAudioOutput                            │  │  │
│  │  │  - Exchange::IConfiguration                          │  │  │
│  │  │  - DsAudioPortNotification (inner class)             │  │  │
│  │  │      implements IAudioOutputPortEvents               │  │  │
│  │  │  - Internal cache + decision logic                   │  │  │
│  │  └──────────────────────────────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────┘  │
│                    │ DS HAL callbacks                             │
│                    ▼                                             │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  DeviceSettings HAL (dshal / device::Host)                 │  │
│  │  - OnAudioModeEvent                                        │  │
│  │  - OnDolbyAtmosCapabilitiesChanged                        │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

### Plugin Shell: AudioOutput.h / AudioOutput.cpp

- Inherits from `PluginHost::IPlugin` and `PluginHost::JSONRPC`.
- Interface map: `INTERFACE_ENTRY(IPlugin)`, `INTERFACE_ENTRY(IDispatcher)`, `INTERFACE_AGGREGATE(Exchange::IAudioOutput, _audioOutput)`.
- `Initialize()`: Acquire `AudioOutputImplementation` via `_service->Root<Exchange::IAudioOutput>()` (in-process), call `Exchange::IConfiguration::Configure()`, then register JSON-RPC stubs via `Exchange::JAudioOutput::Register(*this, _audioOutput)`.
- `Deinitialize()`: `Exchange::JAudioOutput::Unregister(*this)`, release `_audioOutput`, `_configure`, `_service`.

### Implementation: AudioOutputImplementation.h / .cpp

- Inherits `Exchange::IAudioOutput` and `Exchange::IConfiguration`.
- Contains inner class `DsAudioPortNotification` implementing `device::Host::IAudioOutputPortEvents`, registered via `device::Host::getInstance().Register()` at construction.
- Relevant HAL callbacks implemented: `OnAudioModeEvent(dsAudioPortType_t, dsAudioStereoMode_t)` and `OnDolbyAtmosCapabilitiesChanged(dsATMOSCapability_t, bool)`; all other `IAudioOutputPortEvents` callbacks are stubs.
- `SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0)` macro required.
- Thread-safe: use `Core::CriticalSection _adminLock` to protect cache reads/writes.

### Initialisation Sequence

1. Constructor: call `device::Manager::Initialize()`; call `device::Host::getInstance().Register(&_dsAudioPortNotification, ...)` to subscribe to DS HAL events.
2. `Configure(IShell*)` (called by plugin shell after `Root<>()`): call `UpdateCache()` which queries `AtmosMetadata()` and `SoundMode()` directly from the HAL and populates `_atmosMetaData`, `_soundMode`, and `_dolbyAtmosExperience`.
3. Destructor: unregister from DS HAL via `device::Host::getInstance().UnRegister()`, call `device::Manager::DeInitialize()`.

### Module Files

**Module.h:**
```cpp
#ifndef MODULE_NAME
#define MODULE_NAME Plugin_AudioOutput
#endif
#include <plugins/plugins.h>
#include <tracing/tracing.h>
#undef EXTERNAL
#define EXTERNAL
```

**Module.cpp:**
```cpp
#include "Module.h"
MODULE_NAME_DECLARATION(BUILD_REFERENCE)
```

### Build: CMakeLists.txt

Two CMake targets:
1. `${NAMESPACE}AudioOutput` — plugin shell (`.so`)
2. `${NAMESPACE}AudioOutputImplementation` — business logic (`.so`)

`find_package` dependencies: `${NAMESPACE}Plugins`, `${NAMESPACE}Definitions`, `CompileSettingsDebug`

`CXX_STANDARD: 11`

Both `.so` files installed to `${CMAKE_INSTALL_PREFIX}/lib/${STORAGE_DIRECTORY}/plugins`.

CMake cache options:
```cmake
set(PLUGIN_AUDIOOUTPUT_MODE "Off" CACHE STRING "AudioOutput execution mode (Off=in-process, LOCAL=out-of-process)")
set(PLUGIN_AUDIOOUTPUT_AUTOSTART "True" CACHE STRING "Automatically start AudioOutput plugin")
set(PLUGIN_AUDIOOUTPUT_STARTUPORDER "" CACHE STRING "Startup order for AudioOutput plugin")
```

---

## External Interfaces

### Thunder Interface: IAudioOutput

A new Thunder interface `Exchange::IAudioOutput` must be created in ThunderInterfaces.

**File:** `interfaces/IAudioOutput.h`

```cpp
namespace WPEFramework {
namespace Exchange {

    struct EXTERNAL IAudioOutput : virtual public Core::IUnknown {
        enum { ID = ID_AUDIO_OUTPUT };

        struct EXTERNAL INotification : virtual public Core::IUnknown {
            enum { ID = ID_AUDIO_OUTPUT_NOTIFICATION };

            // Fired when the Dolby Atmos Experience state changes
            virtual void OnDolbyAtmosExperienceChanged(const bool dolbyAtmosExperience) = 0;
        };

        virtual void Register(IAudioOutput::INotification* notification) = 0;
        virtual void Unregister(const IAudioOutput::INotification* notification) = 0;

        // Returns true if Dolby Atmos Experience is enabled, false otherwise.
        // Combines AtmosMetadata (ATMOS_METADATA) and soundMode (PASSTHRU / DOLBYDIGITALPLUS / SOUNDMODE_AUTO).
        virtual Core::hresult DolbyAtmosExperience(bool& enabled /* @out */) const = 0;
    };

} // namespace Exchange
} // namespace WPEFramework
```

**Autogenerated stubs required** (generated from `IAudioOutput.h` by the Thunder JSON-RPC code generator):
- `interfaces/json/JAudioOutput.h` — JSON-RPC stub (`Exchange::JAudioOutput::Register / Unregister`)
- `interfaces/json/JsonData_AudioOutput.h` — JSON data types

### JSON-RPC API

#### Method: `org.rdk.AudioOutput.dolbyAtmosExperience`

| Property    | Value |
|------------|-------|
| Callsign   | `org.rdk.AudioOutput` |
| Direction  | Client → Plugin (request) |
| Params     | None |
| Result type | `boolean` |
| Returns    | `true` if Dolby Atmos Experience is enabled, `false` otherwise |
| Error      | `Core::ERROR_GENERAL` on failure |

**Request:**
```json
{ "jsonrpc": "2.0", "id": 42, "method": "org.rdk.AudioOutput.dolbyAtmosExperience" }
```

**Response (success):**
```json
{ "jsonrpc": "2.0", "id": 42, "result": true }
```

#### Notification: `org.rdk.AudioOutput.onDolbyAtmosExperienceChanged`

| Property   | Value |
|-----------|-------|
| Direction | Plugin → Subscriber (push) |
| Trigger   | When the computed Dolby Atmos Experience value changes |
| Params    | `dolbyAtmosExperience` (boolean) |

**Event payload:**
```json
{
  "jsonrpc": "2.0",
  "id": 0,
  "method": "org.rdk.AudioOutput.onDolbyAtmosExperienceChanged",
  "params": { "dolbyAtmosExperience": true }
}
```

### DS HAL Event Sources

| HAL Callback | DS HAL Interface | Trigger |
|---|---|---|
| `OnAudioModeEvent(dsAudioPortType_t, dsAudioStereoMode_t)` | `device::Host::IAudioOutputPortEvents` | Audio stereo mode changes on any port |
| `OnDolbyAtmosCapabilitiesChanged(dsATMOSCapability_t, bool)` | `device::Host::IAudioOutputPortEvents` | Sink device ATMOS capability changes |

- Registered via `device::Host::getInstance().Register(&_dsAudioPortNotification, "WPE[AudioOutput]")`.
- No inter-plugin COM-RPC dependencies. The plugin is self-contained against the DS HAL.

### Error Codes

| Condition                             | Return Code           |
|--------------------------------------|-----------------------|
| Success                               | `Core::ERROR_NONE`   |
| PlayerInfo unavailable / IPC failure  | `Core::ERROR_GENERAL` |
| Invalid state                         | `Core::ERROR_GENERAL` |

All errors propagate as Firebolt errors to the caller.

### Plugin Configuration

**AudioOutput.conf.in:**
```
precondition = ["Platform"]
callsign = "org.rdk.AudioOutput"
autostart = "@PLUGIN_AUDIOOUTPUT_AUTOSTART@"
startuporder = "@PLUGIN_AUDIOOUTPUT_STARTUPORDER@"

configuration = JSON()
rootobject = JSON()

rootobject.add("mode", "@PLUGIN_AUDIOOUTPUT_MODE@")
rootobject.add("locator", "lib@PLUGIN_IMPLEMENTATION@.so")

configuration.add("root", rootobject)
```

**AudioOutput.config:**
```cmake
set(autostart false)
set(preconditions Platform)
set(callsign "org.rdk.AudioOutput")

if(PLUGIN_AUDIOOUTPUT_STARTUPORDER)
  set(startuporder ${PLUGIN_AUDIOOUTPUT_STARTUPORDER})
endif()

map()
  key(root)
  map()
    kv(mode ${PLUGIN_AUDIOOUTPUT_MODE})
    kv(locator lib${PLUGIN_IMPLEMENTATION}.so)
  end()
end()
ans(configuration)
```

---

## Performance

_Not applicable — this plugin performs only lightweight boolean computation and cache lookups. No performance-critical paths are introduced._

---

## Security

_Not applicable — the plugin does not handle user credentials, sensitive data, or network-facing input. Access control is managed by the Thunder framework's standard plugin security model._

---

## Versioning & Compatibility

- Plugin version: `1.0.0` (MAJOR=1, MINOR=0, PATCH=0), registered via `SERVICE_REGISTRATION` macro.
- `PlayerInfo.AtmosMetadata` and `PlayerInfo.soundMode` MUST remain in their existing plugin and must NOT be deprecated or removed as part of this change.
- The new `Exchange::IAudioOutput` interface is additive; it does not modify any existing interfaces.

---

## Conformance Testing & Validation

- **L1 tests**: Unit tests for `AudioOutputImplementation` covering all combinations of `AtmosCapability` × `soundMode` values, including boundary conditions (ATMOS_METADATA + each soundMode variant).
- **L2 tests (in-process)**: Integration tests verifying JSON-RPC method response and notification delivery with mocked DS HAL events.
- All new tests MUST be added to the CI workflows (`L1-tests.yml`, `L2-tests.yml`, `L2-tests-oop.yml`) and the Coverity build script (`cov_build.sh`).

---

## Covered Code

- plugin/AudioOutput.h:
    - `AudioOutput` (class)
    - `AudioOutput::Notification` (inner class)
    - `AudioOutput::Notification::Initialize`
    - `AudioOutput::Notification::Deinitialize`
    - `AudioOutput::Notification::OnDolbyAtmosExperienceChanged`
- plugin/AudioOutput.cpp:
    - `AudioOutput::AudioOutput`
    - `AudioOutput::~AudioOutput`
    - `AudioOutput::Initialize`
    - `AudioOutput::Deinitialize`
    - `AudioOutput::Information`
    - `AudioOutput::Deactivated`
- plugin/AudioOutputImplementation.h:
    - `AudioOutputImplementation` (class)
    - `AudioOutputImplementation::DsAudioPortNotification` (inner class)
    - `AudioOutputImplementation::DsAudioPortNotification::OnDolbyAtmosCapabilitiesChanged`
    - `AudioOutputImplementation::DsAudioPortNotification::OnAudioModeEvent`
- plugin/AudioOutputImplementation.cpp:
    - `AudioOutputImplementation::AudioOutputImplementation`
    - `AudioOutputImplementation::~AudioOutputImplementation`
    - `AudioOutputImplementation::Configure`
    - `AudioOutputImplementation::registerDsEventHandlers`
    - `AudioOutputImplementation::unregisterDsEventHandlers`
    - `AudioOutputImplementation::UpdateCache`
    - `AudioOutputImplementation::DolbyAtmosExperience`
    - `AudioOutputImplementation::Register`
    - `AudioOutputImplementation::Unregister`
    - `AudioOutputImplementation::onAudioModeChanged`
    - `AudioOutputImplementation::onAtmosCapabilitiesChanged`
    - `AudioOutputImplementation::SendNotify`
    - `AudioOutputImplementation::EvaluateCurrentAtmosExperience`
    - `AudioOutputImplementation::AtmosMetadata`
    - `AudioOutputImplementation::SoundMode`
    - `DsAudioModeToSoundMode` (static helper)
- plugin/Module.h:
    - `MODULE_NAME` definition (`Plugin_AudioOutput`)
- plugin/Module.cpp:
    - `MODULE_NAME_DECLARATION`
- plugin/CMakeLists.txt:
    - `WPEFrameworkAudioOutput` target
    - `WPEFrameworkAudioOutputImplementation` target

---

## Open Queries

- **OQ-03**: `ID_AUDIO_OUTPUT` and `ID_AUDIO_OUTPUT_NOTIFICATION` need to be allocated in `interfaces/Ids.h` in ThunderInterfaces. Confirm the next available ID values.

---

## References

- `entservices-migration` plugin — reference implementation pattern: `plugin/Migration.cpp`, `plugin/MigrationImplementation.cpp`
- Thunder plugin instructions: `.github/instructions/Plugin.instructions.md`, `Pluginimplementation.instructions.md`, `Plugincmake.instructions.md`, `Pluginconfig.instructions.md`, `Pluginmodule.instructions.md`
- Original AudioOutput feature requirements (provided in explore session, 2026-06-22)

---

## Change History

- [2026-06-22] - openspec-explore - Initial spec created from feature requirements and reference codebase analysis.
- [2026-06-23] - openspec-templater - Restructured to match spec template.
- [2026-07-08] - openspec-templater - Updated to match actual implementation: in-process mode, DS HAL direct events, renumbered requirements, generated Covered Code section.
