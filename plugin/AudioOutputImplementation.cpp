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

// libds is retained ONLY for the application-audio-config trio
// (setApplicationAudioConfig / getApplicationAudioConfig / getApplicationAudioConfigList),
// which has no DeviceSettings COM-RPC equivalent. All Dolby Atmos capability and
// sound-mode logic is served over COM-RPC via DSHelper.
#include "host.hpp"
#include "manager.hpp"
#include "exception.hpp"

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

        // libds init is required only for the application-audio-config path.
        try {
            device::Manager::Initialize();
            LOGINFO("device::Manager::Initialize success");
        } catch (const device::Exception& err) {
            LOGWARN("device::Manager::Initialize failed : {%s}", err.what());
        }
    }

    AudioOutputImplementation::~AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Destructor");

        // Unregister the DS audio notification before the COM-RPC link is closed.
        auto* audio = AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio != nullptr) {
            audio->Unregister(&_dsAudioNotification);
            audio->Release();
        }
        DSHelper::Close();

        try {
            device::Manager::DeInitialize();
            LOGINFO("device::Manager::DeInitialize success");
        } catch (const device::Exception& err) {
            LOGWARN("device::Manager::DeInitialize failed: {%s}", err.what());
        }
    }

    // -------------------------------------------------------------------------
    // Exchange::IConfiguration::Configure
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::Configure(PluginHost::IShell* service)
    {
        ASSERT(service != nullptr);

        // Open the COM-RPC link to the DeviceSettings plugin. If DeviceSettings is
        // already active, OnDeviceSettingsActivated() fires synchronously here.
        const uint32_t result = DSHelper::Open(service, "AudioOutput");
        if (result != Core::ERROR_NONE) {
            LOGERR("Configure: failed to open DeviceSettings link (result=%u)", result);
        }
        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // DSHelper lifecycle hooks
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::OnDeviceSettingsActivated()
    {
        LOGINFO("AudioOutputImplementation: OnDeviceSettingsActivated — registering DS audio notification");

        auto* audio = AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio != nullptr) {
            audio->Register("AudioOutput", &_dsAudioNotification);
            audio->Release();
        } else {
            LOGWARN("OnDeviceSettingsActivated: IDeviceSettingsAudio unavailable for Register");
        }

        // Prime the cache now that DeviceSettings is guaranteed active.
        bool cap = false;
        _atmosMetadataInitFailed = (AtmosMetadata(cap) != Core::ERROR_NONE);

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        _soundModeInitFailed = (SoundMode(mode) != Core::ERROR_NONE);

        _adminLock.Lock();
        _atmosMetaData = cap;
        _soundMode = mode;
        _adminLock.Unlock();

        UpdateCache();
        LOGINFO("AudioOutputImplementation::OnDeviceSettingsActivated: dolbyAtmosExperience=%s",
                _dolbyAtmosExperience ? "true" : "false");
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

    Core::hresult AudioOutputImplementation::SetAudioConfig(const std::string& audioConfig, const bool enable)
    {
        LOGINFO("Set %s audio configuration to enable = %s", audioConfig.c_str(), enable ? "true" : "false");
        try
        {
            device::Host::getInstance().setApplicationAudioConfig(audioConfig, enable);
        }
        catch (const device::Exception& err)
        {
            LOGERR("Exception during DeviceSetting library call. code = %d message = %s", err.getCode(), err.what());
            return Core::ERROR_GENERAL;
        }
        return (Core::ERROR_NONE);

    }

    Core::hresult AudioOutputImplementation::GetAudioConfig(const std::string& audioConfig, bool& enable /* @out */) const
    {
        enable = false;
        LOGINFO("Get %s audio configuration", audioConfig.c_str());
        try
        {
            device::Host::getInstance().getApplicationAudioConfig(audioConfig, &enable);
            LOGINFO("%s audio config enabled = %s", audioConfig.c_str(), enable ? "true" : "false");
        }
        catch (const device::Exception& err)
        {
            LOGERR("Exception during DeviceSetting library call. code = %d message = %s", err.getCode(), err.what());
           return Core::ERROR_GENERAL;
        }
        return (Core::ERROR_NONE);

    }

    Core::hresult AudioOutputImplementation::GetSupportedAudioConfigs(Exchange::IAudioOutput::IAudioConfigListIterator*&  audioConfigs) const
    {
        audioConfigs = nullptr;
        std::vector<std::string> configList;
        try
        {
           device::Host::getInstance().getApplicationAudioConfigList(configList);
           for (const auto& config : configList) {
               LOGINFO("audio config = %s", config.c_str());
           }
        }
        catch (const device::Exception& err)
        {
             LOGERR("Exception during DeviceSetting library call. code = %d message = %s", err.getCode(), err.what());
             return Core::ERROR_GENERAL;
        }
        audioConfigs = (Core::Service<RPC::IteratorType<Exchange::IAudioOutput::IAudioConfigListIterator>>::Create<Exchange::IAudioOutput::IAudioConfigListIterator>(configList));
        return (Core::ERROR_NONE);
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

        // Recompute the effective sound mode over COM-RPC — SoundMode() applies the
        // full port precedence + connectivity rules rather than trusting a single
        // per-port event, mirroring the DS_IARM behaviour.
        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        if (SoundMode(mode) == Core::ERROR_NONE) {
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

    uint32_t AudioOutputImplementation::AtmosMetadata(bool& supported) const
    {
        using DolbyAtmosCapability = Exchange::IDeviceSettingsAudio::DolbyAtmosCapability;

        supported = false;

        auto* audio = const_cast<AudioOutputImplementation*>(this)
                          ->AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("AtmosMetadata: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        // Cached handles populated by DSHelper on DeviceSettings activation.
        const int32_t arcHandle  = getCachedAudioPortHandle("HDMI_ARC0");
        const int32_t hdmiHandle = getCachedAudioPortHandle("HDMI0");
        DolbyAtmosCapability capability = DolbyAtmosCapability::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;

        if (TV == searchRdkProfile()) {
            // TV platform: use the persisted user-intent HDMI_ARC0 enable flag — HAL
            // connection state is unreliable on some panels.
            bool arcEnabled = false;
            if (arcHandle != INVALID_DS_HANDLE) {
                string portName = "HDMI_ARC0";
                audio->GetAudioEnablePersist(arcHandle, arcEnabled, portName);
                LOGINFO("AtmosMetadata: GetAudioEnablePersist(HDMI_ARC0) = %s", arcEnabled ? "true" : "false");
            }

            if (arcEnabled && arcHandle != INVALID_DS_HANDLE) {
                LOGINFO("AtmosMetadata: ARC enabled, querying HDMI_ARC0 for ATMOS capability");
                audio->GetAudioSinkDeviceAtmosCapability(arcHandle, capability);
            } else {
                LOGINFO("AtmosMetadata: ARC not enabled, querying TV panel ATMOS capability");
                const int32_t selectedHandle = (hdmiHandle != INVALID_DS_HANDLE)
                                                   ? hdmiHandle
                                                   : getCachedAudioPortHandle("SPEAKER0");
                if (selectedHandle != INVALID_DS_HANDLE) {
                    audio->GetAudioSinkDeviceAtmosCapability(selectedHandle, capability);
                } else {
                    LOGWARN("AtmosMetadata: no HDMI_ARC (enabled), HDMI, or SPEAKER port found");
                }
            }
        } else {
            // STB platform: audio goes through HDMI0.
            if (hdmiHandle != INVALID_DS_HANDLE) {
                LOGINFO("AtmosMetadata: STB platform, querying HDMI0 for ATMOS capability");
                audio->GetAudioSinkDeviceAtmosCapability(hdmiHandle, capability);
            } else {
                LOGWARN("AtmosMetadata: HDMI0 handle unavailable");
            }
        }

        audio->Release();

        supported = (capability == DolbyAtmosCapability::AUDIO_DOLBY_ATMOS_METADATA);
        LOGINFO("AtmosMetadata: capability=%d, supported=%s",
                static_cast<int>(capability), supported ? "true" : "false");
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
        auto* audio = self->AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("SoundMode: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        std::vector<AudioPortEntry> entries;
        getAudioPortEntries(entries);
        std::vector<int32_t> handles(entries.size(), INVALID_DS_HANDLE);
        for (size_t i = 0; i < entries.size(); ++i) {
            handles[i] = getCachedAudioPortHandle(entries[i].name);
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
