## 1. ThunderInterfaces — IAudioOutput Interface

- [x] 1.1 Allocate `ID_AUDIO_OUTPUT` and `ID_AUDIO_OUTPUT_NOTIFICATION` in `interfaces/Ids.h` in ThunderInterfaces
- [x] 1.2 Create `interfaces/IAudioOutput.h` defining `Exchange::IAudioOutput` with `DolbyAtmosExperience(bool& enabled)`, `Register(INotification*)`, `Unregister(INotification*)`, and nested `INotification::OnDolbyAtmosExperienceChanged(bool)`
- [x] 1.3 Run the Thunder JSON-RPC code generator on `IAudioOutput.h` to produce `interfaces/json/JAudioOutput.h` and `interfaces/json/JsonData_AudioOutput.h`
- [x] 1.4 Verify the generated stubs compile cleanly against the interface

## 2. Repository Scaffolding

- [x] 2.1 Create `plugin/` directory under the repo root
- [x] 2.2 Create `plugin/Module.h` — define `MODULE_NAME` as `Plugin_AudioOutput`, include `<plugins/plugins.h>` and `<tracing/tracing.h>`
- [x] 2.3 Create `plugin/Module.cpp` — include `Module.h` and call `MODULE_NAME_DECLARATION(BUILD_REFERENCE)`
- [x] 2.4 Create top-level `CMakeLists.txt` with `PLUGIN_AUDIOOUTPUT` CMake option and `add_subdirectory(plugin)` guard

## 3. Plugin Shell — AudioOutput.h / AudioOutput.cpp

- [x] 3.1 Create `plugin/AudioOutput.h` — declare `class AudioOutput` inheriting `PluginHost::IPlugin` and `PluginHost::JSONRPC`; add `BEGIN_INTERFACE_MAP` with `INTERFACE_ENTRY(IPlugin)`, `INTERFACE_ENTRY(IDispatcher)`, `INTERFACE_AGGREGATE(Exchange::IAudioOutput, _audioOutput)`; declare `Initialize()`, `Deinitialize()`, `Information()`, and `Deactivated(RPC::IRemoteConnection*)`
- [x] 3.2 Create `plugin/AudioOutput.cpp` — implement `SERVICE_REGISTRATION` macro and plugin metadata (`Plugin::Metadata<Plugin::AudioOutput>`)
- [x] 3.3 Implement `AudioOutput::Initialize()` — assert preconditions, call `_service->Root<Exchange::IAudioOutput>()` to acquire implementation, call `Exchange::JAudioOutput::Register(*this, _audioOutput)` on success
- [x] 3.4 Implement `AudioOutput::Deinitialize()` — call `Exchange::JAudioOutput::Unregister(*this)`, release `_audioOutput`, handle `_connectionId` and `_service` cleanup
- [x] 3.5 Implement `AudioOutput::Deactivated()` — detect out-of-process crash, call `_service->Submit()` to trigger plugin deactivation

## 4. Implementation — AudioOutputImplementation.h / .cpp

- [x] 4.1 Create `plugin/AudioOutputImplementation.h` — declare `class AudioOutputImplementation` inheriting `Exchange::IAudioOutput`, `PluginHost::IPlugin::INotification`, `Exchange::Dolby::IOutput::INotification`; declare private members `_atmosCapability`, `_soundMode`, `_dolbyAtmosExperience`, `_adminLock`, `_observers`, `_playerInfo`, `_displaySettings`, `_service`
- [x] 4.2 Create `plugin/AudioOutputImplementation.cpp` — implement `SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0)`
- [x] 4.3 Copy `AtmosMetadata(bool& supported)` logic verbatim from `entservices-playerinfo/plugin/DeviceSettings/PlatformImplementation.cpp` as a **private** method `GetAtmosCapability(bool& supported)` — includes HDMI_ARC precedence logic, `getSinkDeviceAtmosCapability` fallback
- [x] 4.4 Copy `SoundMode(Exchange::Dolby::IOutput::SoundModes& mode)` logic verbatim from `entservices-playerinfo/plugin/DeviceSettings/PlatformImplementation.cpp` as a **private** method `GetSoundMode(Exchange::Dolby::IOutput::SoundModes& mode)` — includes port precedence (HDMI_ARC → HDMI → SPEAKER → SPDIF → HEADPHONE), SOUNDMODE_AUTO detection
- [x] 4.5 Copy `dsAudioModeToSoundMode()` helper as a private static method
- [x] 4.6 Implement private `ComputeDolbyAtmosExperience(bool atmosCapability, Exchange::Dolby::IOutput::SoundModes soundMode) -> bool` — applies decision logic: `ATMOS_METADATA + {PASSTHRU|DOLBYDIGITALPLUS|SOUNDMODE_AUTO} → true`, else `false`
- [x] 4.7 Implement `AudioOutputImplementation::Initialize(PluginHost::IShell* service)` — store `_service`, call `_service->Register(this)`, check if `org.rdk.PlayerInfo` is already `ACTIVATED` (acquire `Exchange::Dolby::IOutput` and register listener, read initial values), check if `org.rdk.DisplaySettings` is already `ACTIVATED` (acquire interface and register listener), compute initial `_dolbyAtmosExperience` from cache
- [x] 4.8 Implement `AudioOutputImplementation::Deinitialize()` — unregister listeners, release interface pointers, call `_service->Unregister(this)`, release `_service`
- [x] 4.9 Implement `StateChange(PluginHost::IShell* plugin)` — on `ACTIVATED`: if callsign is `org.rdk.PlayerInfo` acquire `Exchange::Dolby::IOutput`, register listener, read initial values; if `org.rdk.DisplaySettings` acquire and register; on `DEACTIVATED`: unregister and release the corresponding pointer
- [x] 4.10 Implement `Exchange::Dolby::IOutput::INotification::AudioModeChanged(mode, enable)` — acquire lock, update `_soundMode`, call `ComputeDolbyAtmosExperience`, if result differs from `_dolbyAtmosExperience` update cache and fire `OnDolbyAtmosExperienceChanged` to all observers
- [x] 4.11 Implement `AtmosCapabilityChanged` notification handler (DisplaySettings event) — acquire lock, call private `GetAtmosCapability()` to refresh `_atmosCapability`, call `ComputeDolbyAtmosExperience`, if changed update cache and fire notification
- [x] 4.12 Implement public `DolbyAtmosExperience(bool& enabled)` — acquire lock, copy `_dolbyAtmosExperience` to `enabled`, return `Core::ERROR_NONE`
- [x] 4.13 Implement `Register(IAudioOutput::INotification*)` and `Unregister(IAudioOutput::INotification*)` — manage `_observers` list with `AddRef()`/`Release()`
- [x] 4.14 Verify all cache accesses are wrapped in `_adminLock.Lock()` / `_adminLock.Unlock()`

## 5. Build System

- [x] 5.1 Create `plugin/CMakeLists.txt` — set `PLUGIN_NAME`, `MODULE_NAME`, `PLUGIN_IMPLEMENTATION`; add `find_package` for `${NAMESPACE}Plugins`, `${NAMESPACE}Definitions`, `CompileSettingsDebug`
- [x] 5.2 Add `add_library(${MODULE_NAME} SHARED AudioOutput.cpp Module.cpp)` with `CXX_STANDARD 11` and `target_link_libraries` using `${NAMESPACE}` variables
- [x] 5.3 Add `add_library(${PLUGIN_IMPLEMENTATION} SHARED AudioOutputImplementation.cpp Module.cpp)` with DeviceSettings / dshal link dependencies
- [x] 5.4 Add `include_directories(../helpers)` for shared utilities
- [x] 5.5 Add `install()` for both targets to `${CMAKE_INSTALL_PREFIX}/lib/${STORAGE_DIRECTORY}/plugins`
- [x] 5.6 Add CMake cache options: `PLUGIN_AUDIOOUTPUT_MODE` (default `LOCAL`), `PLUGIN_AUDIOOUTPUT_AUTOSTART` (default `false`), `PLUGIN_AUDIOOUTPUT_STARTUPORDER`

## 6. Configuration Files

- [x] 6.1 Create `plugin/AudioOutput.conf.in` — set `precondition = ["Platform"]`, `callsign = "org.rdk.AudioOutput"`, `autostart`, `startuporder`, `mode`, `locator` using CMake variable substitution
- [x] 6.2 Create `plugin/AudioOutput.config` — CMake-time config with `set(callsign "org.rdk.AudioOutput")`, `set(preconditions Platform)`, `map()` block for `root` with `mode` and `locator`

## 7. CI / Coverity Integration

- [x] 7.1 Add `PLUGIN_AUDIOOUTPUT=ON` flag to `.github/workflows/L1-tests.yml` build step
- [x] 7.2 Add `PLUGIN_AUDIOOUTPUT=ON` flag to `.github/workflows/L2-tests.yml` build step
- [x] 7.3 Add `PLUGIN_AUDIOOUTPUT=ON` flag to `.github/workflows/L2-tests-oop.yml` build step
- [x] 7.4 Add `PLUGIN_AUDIOOUTPUT=ON` flag to `cov_build.sh` CMake invocation

## 8. L1 Unit Tests

- [x] 8.1 Create `Tests/L1Tests/` directory and test CMakeLists
- [x] 8.2 Write L1 test: `dolbyAtmosExperience` returns `false` for `NOT_SUPPORTED` capability (all sound modes)
- [x] 8.3 Write L1 test: `dolbyAtmosExperience` returns `false` for `DDPLUS_STREAM` capability (all sound modes)
- [x] 8.4 Write L1 test: `dolbyAtmosExperience` returns `true` for `ATMOS_METADATA` + `PASSTHRU`
- [x] 8.5 Write L1 test: `dolbyAtmosExperience` returns `true` for `ATMOS_METADATA` + `DOLBYDIGITALPLUS`
- [x] 8.6 Write L1 test: `dolbyAtmosExperience` returns `true` for `ATMOS_METADATA` + `SOUNDMODE_AUTO`
- [x] 8.7 Write L1 test: `dolbyAtmosExperience` returns `false` for `ATMOS_METADATA` + `MONO`, `STEREO`, `SURROUND`, `DOLBYDIGITAL`, `UNKNOWN`
- [x] 8.8 Write L1 test: notification fires on `true→false` transition, does NOT fire when value unchanged

## 9. L2 Integration Tests

- [x] 9.1 Create `Tests/L2Tests/` directory and test CMakeLists
- [x] 9.2 Write L2 test: JSON-RPC `dolbyAtmosExperience` call returns correct boolean with mocked HAL
- [x] 9.3 Write L2 test: `onDolbyAtmosExperienceChanged` notification delivered to subscriber when `AudioModeChanged` event fires with mode change that flips result
- [x] 9.4 Write L2 test: `onDolbyAtmosExperienceChanged` notification delivered when `AtmosCapabilityChanged` event fires with capability change that flips result
- [x] 9.5 Write L2 test: no notification sent when event fires but computed value is unchanged
- [x] 9.6 Create out-of-process variants of L2 tests for `L2-tests-oop.yml`
