/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2025 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "AudioOutputImplementation.h"

#include <algorithm>
#include <vector>
#include <core/core.h>
#include "UtilsLogging.h"
#include "UtilsSearchRDKProfile.h"

namespace WPEFramework {
namespace Plugin {

    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(
        Exchange::IDeviceSettingsAudio::StereoMode smode);
    SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0);

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    AudioOutputImplementation::AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Constructor");
    }

    AudioOutputImplementation::~AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Destructor");

        // Unregister the DS audio notification before the COM-RPC link is closed.
        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio != nullptr) {
            const uint32_t unregResult = audio->Unregister(&_dsAudioNotification);
            LOGINFO("~AudioOutputImplementation: audio->Unregister() returned %u", unregResult);
            if (unregResult != Core::ERROR_NONE) {
                LOGWARN("~AudioOutputImplementation: audio->Unregister() returned non-zero: %u", unregResult);
            }
            audio->Release();
        } else {
            LOGWARN("~AudioOutputImplementation: Cannot unregister — IDeviceSettingsAudio unavailable");
        }
        DSHelper::Close();
    }

    // -------------------------------------------------------------------------
    // Exchange::IConfiguration::Configure
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::Configure(PluginHost::IShell* service)
    {
        ASSERT(service != nullptr);

        // Open the COM-RPC link to the DeviceSettings plugin. If DeviceSettings is
        // already active, OnDeviceSettingsActivated() fires synchronously here.
		LOGINFO("Configure: Opening DeviceSettings COM-RPC link for AudioOutput plugin");
        const uint32_t result = DSHelper::Open(service, "AudioOutput");
        if (result != Core::ERROR_NONE) {
            LOGERR("Configure: DSHelper::Open() failed with result=%u — audio events will NOT work", result);
        } else {
            LOGINFO("Configure: DSHelper::Open() succeeded");
        }
        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // DSHelper lifecycle hooks
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::OnDeviceSettingsActivated()
    {
        LOGINFO("AudioOutputImplementation: OnDeviceSettingsActivated — registering DS audio notification");

        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio != nullptr) {
            // Register the notification delegate for audio events (OnAudioModeEvent, OnDolbyAtmosCapabilitiesChanged, etc.)
            const uint32_t regResult = audio->Register("AudioOutput", &_dsAudioNotification);
            LOGINFO("OnDeviceSettingsActivated: audio->Register() returned %u (ERROR_NONE=%u)",
                    regResult, Core::ERROR_NONE);
            if (regResult != Core::ERROR_NONE) {
                LOGERR("OnDeviceSettingsActivated: audio->Register() FAILED with error %u — events will NOT be delivered",
                       regResult);
            }
            audio->Release();
        } else {
            LOGERR("OnDeviceSettingsActivated: IDeviceSettingsAudio unavailable for Register — NO EVENTS will be received");
        }

        // Prime the cache now that DeviceSettings is guaranteed active.
        bool cap = false;
        _atmosMetadataInitFailed = (AtmosMetadata(cap) != Core::ERROR_NONE);
		if (_atmosMetadataInitFailed) {
            LOGWARN("OnDeviceSettingsActivated: AtmosMetadata() query failed at init");
        }

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        _soundModeInitFailed = (SoundMode(mode) != Core::ERROR_NONE);
		if (_soundModeInitFailed) {
            LOGWARN("OnDeviceSettingsActivated: SoundMode() query failed at init");
        }

        _adminLock.Lock();
        _atmosMetaData = cap;
        _soundMode = mode;
        _adminLock.Unlock();

        UpdateCache();
        LOGINFO("AudioOutputImplementation::OnDeviceSettingsActivated COMPLETE: "
                "dolbyAtmosExperience=%s, soundMode=%d (atmosFailed=%s, soundModeFailed=%s)",
                _dolbyAtmosExperience ? "true" : "false", mode,
                _atmosMetadataInitFailed ? "yes" : "no",
                _soundModeInitFailed ? "yes" : "no");
    }

    void AudioOutputImplementation::OnDeviceSettingsDeactivated()
    {
        LOGINFO("AudioOutputImplementation: OnDeviceSettingsDeactivated");
        // The COM-RPC link is down; cached values are retained until DS reactivates.
    }

    void AudioOutputImplementation::UpdateCache()
    {
        _adminLock.Lock();
        bool isAtmosExpChanged = false;

        bool newValue = EvaluateCurrentAtmosExperience();
        if (newValue != _dolbyAtmosExperience) {
            isAtmosExpChanged = true;
            _dolbyAtmosExperience = newValue;
        } else {
            
            LOGINFO("AudioOutputImplementation: dolbyAtmosExperience unchanged (%s)",
                    _dolbyAtmosExperience ? "true" : "false");
        }
        _adminLock.Unlock();

        if (isAtmosExpChanged) {
            LOGINFO("AudioOutputImplementation: dolbyAtmosExperience changed to %s",
                    newValue ? "true" : "false");
            SendNotify(newValue);
        }
    }

    // -------------------------------------------------------------------------
    // IAudioOutput::DolbyAtmosExperience
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::DolbyAtmosExperience(bool& enabled) const
    {
        if (_atmosMetadataInitFailed || _soundModeInitFailed) {
            LOGINFO("DolbyAtmosExperience: prior init failure detected, retrying HAL queries");

            bool cap = false;
            bool atmosErr = (AtmosMetadata(cap) != Core::ERROR_NONE);

            Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
            bool soundErr = (SoundMode(mode) != Core::ERROR_NONE);

            if (atmosErr || soundErr) {
                LOGERR("DolbyAtmosExperience: retry failed (atmosMetadata=%s, soundMode=%s)",
                       atmosErr ? "failed" : "ok", soundErr ? "failed" : "ok");
                return Core::ERROR_GENERAL;
            }

            _atmosMetadataInitFailed = false;
            _soundModeInitFailed = false;

            _adminLock.Lock();
            _atmosMetaData = cap;
            _soundMode = mode;
            _adminLock.Unlock();

            const_cast<AudioOutputImplementation*>(this)->UpdateCache();

            LOGINFO("DolbyAtmosExperience: retry succeeded (atmosMetadata=%s, soundMode=%d)",
                    cap ? "true" : "false", mode);
        }

        _adminLock.Lock();
        enabled = _dolbyAtmosExperience;
        _adminLock.Unlock();
        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // IAudioOutput::Register / Unregister
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::Register(Exchange::IAudioOutput::INotification* notification)
    {
        ASSERT(nullptr != notification);

        _adminLock.Lock();

        if (std::find(_observers.begin(), _observers.end(), notification) == _observers.end()) {
            _observers.push_back(notification);
            notification->AddRef();
        } else {
            LOGERR("same notification is registered already");
        }

        _adminLock.Unlock();

        return Core::ERROR_NONE;
    }

    Core::hresult AudioOutputImplementation::Unregister(const Exchange::IAudioOutput::INotification* notification)
    {
        ASSERT(nullptr != notification);

        _adminLock.Lock();

        auto itr = std::find(_observers.begin(), _observers.end(), const_cast<Exchange::IAudioOutput::INotification*>(notification));
        if (itr != _observers.end()) {
            (*itr)->Release();
            _observers.erase(itr);
        } else {
            LOGERR("notification not found");
        }

        _adminLock.Unlock();

        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // onAudioModeChanged
    // DeviceSettings COM-RPC callback for IDeviceSettingsAudio::OnAudioModeEvent
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAudioModeChanged(
        Exchange::IDeviceSettingsAudio::AudioPortType audioPortType,
        Exchange::IDeviceSettingsAudio::StereoMode audioMode)
    {
        LOGINFO("AudioOutputImplementation::onAudioModeChanged: portType=%d, audioMode=%d",
                static_cast<int>(audioPortType), static_cast<int>(audioMode));

        Exchange::IAudioOutput::AudioModes mode = DsAudioModeToSoundMode(audioMode);
        if (mode != _soundMode) {
            _adminLock.Lock();
            _soundMode = mode;
            _adminLock.Unlock();
            UpdateCache();
        } else {
            LOGERR("onAudioModeChanged: SoundMode query failed");
        }
    }

    // -------------------------------------------------------------------------
    // onAtmosCapabilitiesChanged
    // DeviceSettings COM-RPC callback for IDeviceSettingsAudio::OnDolbyAtmosCapabilitiesChanged
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAtmosCapabilitiesChanged(
        Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability, bool status)
    {
        LOGINFO("AudioOutputImplementation::onAtmosCapabilitiesChanged: atmosCapability=%d, status=%d",
                static_cast<int>(atmosCapability), static_cast<int>(status));

        _adminLock.Lock();
        _atmosMetaData =
            (atmosCapability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
        _adminLock.Unlock();

        UpdateCache();
    }

    // -------------------------------------------------------------------------
    // Private: SendNotify
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::SendNotify(bool dolbyAtmosExperience)
    {
        std::list<Exchange::IAudioOutput::INotification*> index;

        _adminLock.Lock();
        index = _observers;
        for (auto* obs : index) {
            obs->AddRef();
        }
        _adminLock.Unlock();

        LOGINFO("AudioOutputImplementation: SendNotify: notifying %zu observers of dolbyAtmosExperience=%s",
                index.size(), dolbyAtmosExperience ? "true" : "false");

        for (auto* obs : index) {
            obs->OnDolbyAtmosExperienceChanged(dolbyAtmosExperience);
            obs->Release();
        }
    }

    // -------------------------------------------------------------------------
    // Private: EvaluateAtmosExperience
    // Step 1: AtmosCapability must be ATMOS_METADATA (true from AtmosMetadata)
    // Step 2: soundMode must be PASSTHRU, DOLBYDIGITALPLUS, or SOUNDMODE_AUTO
    // -------------------------------------------------------------------------

    bool AudioOutputImplementation::EvaluateCurrentAtmosExperience() const
    {
        if (!_atmosMetaData) {
            return false;
        }
				
        switch (_soundMode) {
        case Exchange::IAudioOutput::PASSTHRU:
        case Exchange::IAudioOutput::DOLBYDIGITALPLUS:
	case Exchange::IAudioOutput::SOUNDMODE_AUTO:
	case  Exchange::IAudioOutput::SURROUND:
            return true;
        default:
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // AtmosMetadata (COM-RPC)
    // Derived from entservices-playerinfo DeviceSettings/PlatformImplementation.cpp.
    // Queries the DeviceSettings plugin for the connected sink's Dolby Atmos
    // capability using cached audio-port handles.
    // -------------------------------------------------------------------------
    uint32_t AudioOutputImplementation::AtmosMetadata(bool& supported /* @out */) const
    {
        supported = false;

        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("AtmosMetadata: IDeviceSettingsAudio unavailable");
            return Core::ERROR_NONE;
        }

        // Use cached handles populated by DSHelper::LoadAllConfigs()
        int32_t arcHandle  = DSHelper::getCachedAudioPortHandle("HDMI_ARC0");
        int32_t hdmiHandle = DSHelper::getCachedAudioPortHandle("HDMI0");

        if (TV == searchRdkProfile()) {
            // TV platform: use persisted user-intent enable state — HAL state is unreliable on some platforms
            bool arcEnabled = false;
            if (arcHandle != INVALID_DS_HANDLE) {
                string portName = "HDMI_ARC0";
                audio->GetAudioEnablePersist(arcHandle, arcEnabled, portName);
                LOGINFO("AtmosMetadata: GetAudioEnablePersist(HDMI_ARC0) = %s", arcEnabled ? "true" : "false");
            }

            Exchange::IDeviceSettingsAudio::DolbyAtmosCapability capability = Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;
            if (arcEnabled && arcHandle != INVALID_DS_HANDLE) {
                // ARC is enabled — query HDMI_ARC0 for ATMOS capability
                LOGINFO("AtmosMetadata: ARC enabled, querying HDMI_ARC0 for ATMOS capability");
                audio->GetAudioSinkDeviceAtmosCapability(arcHandle, capability);
            } else {
                // ARC not enabled — query HDMI0 (TV panel itself)
                LOGINFO("AtmosMetadata: ARC not enabled, querying HDMI0 for ATMOS capability");
                int32_t selectedHandle = (hdmiHandle != INVALID_DS_HANDLE) ? hdmiHandle
                                       : DSHelper::getCachedAudioPortHandle("SPEAKER0");
                if (selectedHandle != INVALID_DS_HANDLE) {
                    audio->GetAudioSinkDeviceAtmosCapability(selectedHandle, capability);
                } else {
                    LOGWARN("AtmosMetadata: no HDMI_ARC (enabled), HDMI, or SPEAKER port found");
                }
            }
            supported = (capability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
            LOGINFO("AtmosMetadata: capability=%d, supported=%s",
                    static_cast<int>(capability), supported ? "true" : "false");
        } else {
            // STB platform: keep port-priority logic (HDMI_ARC > HDMI > SPEAKER)
            int32_t selectedHandle = INVALID_DS_HANDLE;
            bool    arcConnected   = false;

            if (arcHandle != INVALID_DS_HANDLE) {
                bool arcEnabled = false;
                audio->IsAudioPortEnabled(arcHandle, arcEnabled);
                if (arcEnabled) {
                    selectedHandle = arcHandle;
                    arcConnected   = true;
                }
            }
			if (!arcConnected && hdmiHandle != INVALID_DS_HANDLE) {
                bool hdmiEnabled = false;
                audio->IsAudioPortEnabled(hdmiHandle, hdmiEnabled);
                if (hdmiEnabled) {
                    selectedHandle = hdmiHandle;
                }
            }

            if (selectedHandle != INVALID_DS_HANDLE) {
                Exchange::IDeviceSettingsAudio::DolbyAtmosCapability capability = Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;
                audio->GetAudioSinkDeviceAtmosCapability(selectedHandle, capability);
                supported = (capability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
                LOGINFO("AtmosMetadata: capability=%d, supported=%s",
                        static_cast<int>(capability), supported ? "true" : "false");
            } else {
                int32_t spkHandle = DSHelper::getCachedAudioPortHandle("SPEAKER0");
                if (spkHandle != INVALID_DS_HANDLE) {
                    Exchange::IDeviceSettingsAudio::DolbyAtmosCapability capability = Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;
                    audio->GetAudioSinkDeviceAtmosCapability(spkHandle, capability);
                    supported = (capability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
                    LOGINFO("AtmosMetadata (SPEAKER fallback): capability=%d, supported=%s",
                            static_cast<int>(capability), supported ? "true" : "false");
				} else {
                    LOGWARN("AtmosMetadata: no connected HDMI_ARC, HDMI, or SPEAKER port found");
                }
            }
        }

        audio->Release();
        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // DsAudioModeToSoundMode
    // Maps a DeviceSettings COM-RPC StereoMode to IAudioOutput::AudioModes.
    // -------------------------------------------------------------------------

    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(
        Exchange::IDeviceSettingsAudio::StereoMode smode)
    {
        using StereoMode = Exchange::IDeviceSettingsAudio::StereoMode;
        switch (smode) {
        case StereoMode::AUDIO_STEREO_MONO:        return Exchange::IAudioOutput::MONO;
        case StereoMode::AUDIO_STEREO_STEREO:      return Exchange::IAudioOutput::STEREO;
        case StereoMode::AUDIO_STEREO_SURROUND:    return Exchange::IAudioOutput::SURROUND;
        case StereoMode::AUDIO_STEREO_PASSTHROUGH: return Exchange::IAudioOutput::PASSTHRU;
        case StereoMode::AUDIO_STEREO_DD:          return Exchange::IAudioOutput::DOLBYDIGITAL;
        case StereoMode::AUDIO_STEREO_DDPLUS:      return Exchange::IAudioOutput::DOLBYDIGITALPLUS;
        default:
            LOGWARN("Unknown StereoMode %d encountered, returning UNKNOWN", static_cast<int>(smode));
            return Exchange::IAudioOutput::UNKNOWN;
        }
    }

    // -------------------------------------------------------------------------
    // SoundMode (COM-RPC)
    // Derived from entservices-playerinfo DeviceSettings/PlatformImplementation.cpp.
    // Applies port precedence (HDMI_ARC > HDMI > SPEAKER > SPDIF > HEADPHONE) and
    // connectivity checks over COM-RPC, then reads the effective stereo mode.
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::SoundMode(Exchange::IAudioOutput::AudioModes& mode) const
    {
        using AudioPortType = Exchange::IDeviceSettingsAudio::AudioPortType;
        using StereoMode    = Exchange::IDeviceSettingsAudio::StereoMode;

        mode = Exchange::IAudioOutput::UNKNOWN;

        static const AudioPortType kPriority[] = {
            AudioPortType::AUDIO_PORT_TYPE_HDMIARC,
            AudioPortType::AUDIO_PORT_TYPE_HDMI,
            AudioPortType::AUDIO_PORT_TYPE_SPEAKER,
            AudioPortType::AUDIO_PORT_TYPE_SPDIF,
            AudioPortType::AUDIO_PORT_TYPE_HEADPHONE
        };
        static const size_t kPriorityCount = sizeof(kPriority) / sizeof(kPriority[0]);

        auto* self  = const_cast<AudioOutputImplementation*>(this);
        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("SoundMode: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        std::vector<AudioPortEntry> entries;
        DSHelper::getAudioPortEntries(entries);
        std::vector<int32_t> handles(entries.size(), INVALID_DS_HANDLE);
        for (size_t i = 0; i < entries.size(); ++i) {
            handles[i] = DSHelper::getCachedAudioPortHandle(entries[i].name);
        }

        bool found = false;
        for (size_t pi = 0; pi < kPriorityCount && !found; ++pi) {
            const AudioPortType targetType = kPriority[pi];

            for (size_t ei = 0; ei < entries.size() && !found; ++ei) {
                if (entries[ei].type != targetType) continue;

                const int32_t handle = handles[ei];
                if (handle == INVALID_DS_HANDLE) continue;

                // isEnabled(): skip only when the HAL explicitly reports the port disabled.
                bool enabled = false;
                if (audio->IsAudioPortEnabled(handle, enabled) == Core::ERROR_NONE && !enabled) continue;

                // isConnected(): HDMI→display connected, ARC→HDMI-In connected, others→always true.
                int32_t connHandle = INVALID_DS_HANDLE;
                if (!self->isAudioOutputPortConnected(audio, entries[ei].name, connHandle)) continue;

                StereoMode stereoMode = StereoMode::AUDIO_STEREO_UNKNOWN;
                if (audio->GetStereoMode(handle, stereoMode, false) == Core::ERROR_NONE) {
                    mode = DsAudioModeToSoundMode(stereoMode);

                    // Pass-through auto detection for HDMI_ARC and SPDIF.
                    if (targetType == AudioPortType::AUDIO_PORT_TYPE_HDMIARC
                        || targetType == AudioPortType::AUDIO_PORT_TYPE_SPDIF) {
                        int32_t autoMode = 0;
                        if (audio->GetStereoAuto(handle, autoMode) == Core::ERROR_NONE && autoMode) {
                            mode = Exchange::IAudioOutput::SOUNDMODE_AUTO;
                            LOGINFO("SoundMode: setting audio mode as auto");
                        }
                    }

                    LOGINFO("SoundMode: port type=%d index=%d -> mode=%d",
                            static_cast<int>(targetType), entries[ei].index, static_cast<int>(mode));
                    found = true;
                }
            }
        }

        if (!found) {
            LOGWARN("SoundMode: no enabled and connected audio port found matching precedence.");
        }

        audio->Release();
        return Core::ERROR_NONE;
    }

} // namespace Plugin
} // namespace WPEFramework
