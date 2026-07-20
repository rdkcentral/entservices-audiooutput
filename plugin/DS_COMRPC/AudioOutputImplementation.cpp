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

// DS_COMRPC/AudioOutputImplementation.cpp
//
// COM-RPC-based implementation of AudioOutputImplementation.
// Uses DSHelper (same pattern as PlayerInfo DS_COMRPC and DisplaySettings DS_COMRPC).
//
// Key behavioral differences from DS_IARM variant (transport layer only):
//   DS_IARM:   device::Manager::Initialize() in constructor
//   DS_COMRPC: DSHelper::Open(service) in Configure()
//
//   DS_IARM:   device::Host::Register(&_dsAudioPortNotification)
//   DS_COMRPC: audio->Register(&_audioNotification) in OnDeviceSettingsActivated()
//
//   DS_IARM:   device::Host::UnRegister() in destructor
//   DS_COMRPC: audio->Unregister() in OnDeviceSettingsDeactivated() + destructor safety net
//
//   DS_IARM:   device::AudioOutputPort::getSinkDeviceAtmosCapability()
//   DS_COMRPC: IDeviceSettingsAudio::GetAudioSinkDeviceAtmosCapability(handle)
//              using DSHelper::getCachedAudioPortHandle() (same as PlayerInfo)
//
//   DS_IARM:   device::AudioOutputPort::getStereoMode() / getStereoAuto()
//   DS_COMRPC: IDeviceSettingsAudio::GetStereoMode(handle) / GetStereoAuto(handle)
//              using DSHelper::getAudioPortEntries() (same as PlayerInfo)

#include "AudioOutputImplementation.h"

#include <vector>
#include <core/core.h>
#include "UtilsLogging.h"

namespace WPEFramework {
namespace Plugin {

    // -------------------------------------------------------------------------
    // StereoMode -> IAudioOutput::AudioModes mapping
    // -------------------------------------------------------------------------
    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(
        Exchange::IDeviceSettingsAudio::StereoMode mode)
    {
        switch (mode) {
        case Exchange::IDeviceSettingsAudio::AUDIO_STEREO_MONO:        return Exchange::IAudioOutput::MONO;
        case Exchange::IDeviceSettingsAudio::AUDIO_STEREO_STEREO:      return Exchange::IAudioOutput::STEREO;
        case Exchange::IDeviceSettingsAudio::AUDIO_STEREO_SURROUND:    return Exchange::IAudioOutput::SURROUND;
        case Exchange::IDeviceSettingsAudio::AUDIO_STEREO_PASSTHROUGH: return Exchange::IAudioOutput::PASSTHRU;
        case Exchange::IDeviceSettingsAudio::AUDIO_STEREO_DD:          return Exchange::IAudioOutput::DOLBYDIGITAL;
        case Exchange::IDeviceSettingsAudio::AUDIO_STEREO_DDPLUS:      return Exchange::IAudioOutput::DOLBYDIGITALPLUS;
        default:
            LOGWARN("Unknown StereoMode %d, returning UNKNOWN", static_cast<int>(mode));
            return Exchange::IAudioOutput::UNKNOWN;
        }
    }

    SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0);

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    AudioOutputImplementation::AudioOutputImplementation()
        : _audioNotification(*this)
    {
        LOGINFO("AudioOutputImplementation Constructor (DS_COMRPC)");
    }

    AudioOutputImplementation::~AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Destructor (DS_COMRPC)");

        auto* audio = AcquireAudioInterface();
        if (audio != nullptr) {
            audio->Unregister(&_audioNotification);
            audio->Release();
        }

        DSHelper::Close();
    }

    // -------------------------------------------------------------------------
    // IConfiguration::Configure
    // Opens the DSHelper link to DeviceSettings.
    // OnDeviceSettingsActivated() fires once DeviceSettings is confirmed running.
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::Configure(PluginHost::IShell* service)
    {
        ASSERT(service != nullptr);
        LOGINFO("AudioOutputImplementation::Configure (DS_COMRPC)");

        DSHelper::Open(service);
        LOGINFO("AudioOutputImplementation: DSHelper::Open() called — awaiting OnDeviceSettingsActivated()");
        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // AcquireAudioInterface
    // Thin wrapper around DSHelper::AcquireSubInterface<IDeviceSettingsAudio>().
    // Returns an AddRef'd pointer — caller MUST call Release().
    // -------------------------------------------------------------------------

    Exchange::IDeviceSettingsAudio* AudioOutputImplementation::AcquireAudioInterface()
    {
        return DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
    }

    // -------------------------------------------------------------------------
    // OnDeviceSettingsActivated  (DSHelper override)
    // DeviceSettings is active / reconnected.
    // Mirrors DS_IARM: registerDsEventHandlers() + Configure() initial queries.
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::OnDeviceSettingsActivated()
    {
        LOGINFO("AudioOutputImplementation::OnDeviceSettingsActivated");

        // Register our notification delegate with IDeviceSettingsAudio.
        auto* audio = AcquireAudioInterface();
        if (audio != nullptr) {
            audio->Register(&_audioNotification);
            audio->Release();
        } else {
            LOGERR("OnDeviceSettingsActivated: IDeviceSettingsAudio not available");
        }

        // Mirror DS_IARM Configure() logic: query initial state and track failures.
        bool cap = false;
        bool atmosMetadataFailed = false;
        if (AtmosMetadata(cap) != Core::ERROR_NONE) {
            LOGERR("OnDeviceSettingsActivated: failed to get atmos metadata");
            atmosMetadataFailed = true;
        }

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        bool soundModeInitFailed = false;
        if (SoundMode(mode) != Core::ERROR_NONE) {
            LOGERR("OnDeviceSettingsActivated: failed to get sound mode");
            soundModeInitFailed = true;
        }

        _atmosMetadataInitFailed = atmosMetadataFailed;
        _soundModeInitFailed     = soundModeInitFailed;

        _adminLock.Lock();
        _atmosMetaData = cap;
        _soundMode     = mode;
        _adminLock.Unlock();

        UpdateCache();
        LOGINFO("AudioOutputImplementation::OnDeviceSettingsActivated: initial dolbyAtmosExperience=%s",
                _dolbyAtmosExperience ? "true" : "false");
    }

    // -------------------------------------------------------------------------
    // OnDeviceSettingsDeactivated  (DSHelper override)
    // DeviceSettings has deactivated / crashed.
    // Symmetric with OnDeviceSettingsActivated: unregister notification and
    // invalidate cached state.
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::OnDeviceSettingsDeactivated()
    {
        LOGINFO("AudioOutputImplementation::OnDeviceSettingsDeactivated");

        // Symmetric with OnDeviceSettingsActivated: unregister notification.
        // If DeviceSettings crashed the link is already gone and AcquireAudioInterface()
        // returns nullptr — that is safe to skip.
        auto* audio = AcquireAudioInterface();
        if (audio != nullptr) {
            audio->Unregister(&_audioNotification);
            audio->Release();
        }

        // Invalidate cached state.
        _adminLock.Lock();
        _atmosMetaData = false;
        _soundMode     = Exchange::IAudioOutput::UNKNOWN;
        _adminLock.Unlock();
    }

    // -------------------------------------------------------------------------
    // IAudioOutput::DolbyAtmosExperience
    // Same retry logic as DS_IARM variant.
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::DolbyAtmosExperience(bool& enabled) const
    {
        if (_atmosMetadataInitFailed || _soundModeInitFailed) {
            LOGINFO("DolbyAtmosExperience: prior init failure detected, retrying COM-RPC queries");

            bool cap = false;
            bool atmosErr = (const_cast<AudioOutputImplementation*>(this)->AtmosMetadata(cap) != Core::ERROR_NONE);

            Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
            bool soundErr = (const_cast<AudioOutputImplementation*>(this)->SoundMode(mode) != Core::ERROR_NONE);

            if (atmosErr || soundErr) {
                LOGERR("DolbyAtmosExperience: retry failed (atmosMetadata=%s, soundMode=%s)",
                       atmosErr ? "failed" : "ok", soundErr ? "failed" : "ok");
                return Core::ERROR_GENERAL;
            }

            _atmosMetadataInitFailed = false;
            _soundModeInitFailed     = false;

            _adminLock.Lock();
            _atmosMetaData = cap;
            _soundMode     = mode;
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

        auto itr = std::find(_observers.begin(), _observers.end(), notification);
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
    // COM-RPC equivalent of DS_IARM IAudioOutputPortEvents::OnAudioModeEvent.
    // Same if/else-if logic as DS_IARM; only API calls differ.
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAudioModeChanged(
        Exchange::IDeviceSettingsAudio::AudioPortType portType,
        Exchange::IDeviceSettingsAudio::StereoMode    smode)
    {
        LOGINFO("AudioOutputImplementation::onAudioModeChanged: portType=%d, smode=%d",
                static_cast<int>(portType), static_cast<int>(smode));

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;

        auto* audio = AcquireAudioInterface();
        if (audio != nullptr) {
            Exchange::IDeviceSettingsAudio::IAudioTypeConfigIterator* audioTypes = nullptr;
            Exchange::IDeviceSettingsAudio::IAudioPortConfigIterator* audioPorts = nullptr;

            if (audio->GetAudioConfig(audioTypes, audioPorts) == Core::ERROR_NONE) {
                if (audioPorts != nullptr) {
                    Exchange::IDeviceSettingsAudio::AudioPortConfigInfo portInfo{};
                    while (audioPorts->Next(portInfo)) {
                        if (portInfo.audioPortType != portType) {
                            continue;
                        }
                        int32_t handle = -1;
                        if (audio->GetAudioPort(portInfo.audioPortType,
                                                portInfo.audioPortIndex,
                                                handle) != Core::ERROR_NONE) {
                            continue;
                        }
                        int32_t autoVal = 0;
                        audio->GetStereoAuto(handle, autoVal);

                        // Mirror DS_IARM if/else-if:
                        if ((portType == Exchange::IDeviceSettingsAudio::AUDIO_PORT_TYPE_HDMIARC ||
                             portType == Exchange::IDeviceSettingsAudio::AUDIO_PORT_TYPE_SPDIF   ||
                             portType == Exchange::IDeviceSettingsAudio::AUDIO_PORT_TYPE_HDMI    ||
                             portType == Exchange::IDeviceSettingsAudio::AUDIO_PORT_TYPE_SPEAKER) &&
                            autoVal != 0) {
                            mode = Exchange::IAudioOutput::SOUNDMODE_AUTO;
                        } else {
                            mode = DsAudioModeToSoundMode(smode);
                        }
                        break;
                    }
                    audioPorts->Release();
                }
                if (audioTypes != nullptr) {
                    audioTypes->Release();
                }
            } else {
                LOGERR("onAudioModeChanged: GetAudioConfig failed");
            }
            audio->Release();
        }

        _adminLock.Lock();
        _soundMode = mode;
        _adminLock.Unlock();
        UpdateCache();
    }

    // -------------------------------------------------------------------------
    // onAtmosCapabilitiesChanged
    // COM-RPC equivalent of DS_IARM IAudioOutputPortEvents::OnDolbyAtmosCapabilitiesChanged.
    // Logic preserved from DS_IARM variant.
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAtmosCapabilitiesChanged(
        Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability,
        bool status)
    {
        LOGINFO("AudioOutputImplementation::onAtmosCapabilitiesChanged: atmosCapability=%d, status=%d",
                static_cast<int>(atmosCapability), static_cast<int>(status));

        // DS_IARM does not check status — it always updates _atmosMetaData
        // based on the reported atmosCapability value.
        _adminLock.Lock();
        _atmosMetaData = (atmosCapability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
        _adminLock.Unlock();

        UpdateCache();
    }

    // -------------------------------------------------------------------------
    // SendNotify
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

        LOGINFO("AudioOutputImplementation::SendNotify: notifying %zu observers "
                "of dolbyAtmosExperience=%s",
                index.size(), dolbyAtmosExperience ? "true" : "false");

        for (auto* obs : index) {
            obs->OnDolbyAtmosExperienceChanged(dolbyAtmosExperience);
            obs->Release();
        }
    }

    // -------------------------------------------------------------------------
    // UpdateCache
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::UpdateCache()
    {
        _adminLock.Lock();
        bool isAtmosExpChanged = false;

        bool newValue = EvaluateCurrentAtmosExperience();
        if (newValue != _dolbyAtmosExperience) {
            isAtmosExpChanged     = true;
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
    // EvaluateCurrentAtmosExperience
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
            return true;
        default:
            return false;
        }
    }

    // -------------------------------------------------------------------------
    // AtmosMetadata
    // Copied from entservices-playerinfo DS_COMRPC/PlatformImplementation.cpp
    // Adapted: AcquireAudioInterface() instead of DSHelper::AcquireSubInterface<>
    //          DSHelper::getCachedAudioPortHandle() for port handles (same as PlayerInfo)
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::AtmosMetadata(bool& supported)
    {
        supported = false;

        auto* audio = AcquireAudioInterface();
        if (audio == nullptr) {
            LOGERR("AtmosMetadata: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        Exchange::IDeviceSettingsAudio::DolbyAtmosCapability atmosCapability =
            Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;

        // Use cached handles — same as PlayerInfo DS_COMRPC
        int32_t arcHandle  = DSHelper::getCachedAudioPortHandle("HDMI_ARC0");
        int32_t hdmiHandle = DSHelper::getCachedAudioPortHandle("HDMI0");

        int32_t selectedHandle = INVALID_DS_HANDLE;
        bool    arcConnected   = false;

        if (arcHandle != INVALID_DS_HANDLE) {
            bool arcIsConnected = false;
            audio->IsAudioOutputConnected(arcHandle, arcIsConnected);
            if (arcIsConnected) {
                selectedHandle = arcHandle;
                arcConnected   = true;
            }
        }

        if (!arcConnected && hdmiHandle != INVALID_DS_HANDLE) {
            bool hdmiIsConnected = false;
            audio->IsAudioOutputConnected(hdmiHandle, hdmiIsConnected);
            if (hdmiIsConnected) {
                selectedHandle = hdmiHandle;
            }
        }

        if (selectedHandle != INVALID_DS_HANDLE) {
            audio->GetAudioSinkDeviceAtmosCapability(selectedHandle, atmosCapability);
            LOGINFO("AtmosMetadata: capability=%d, supported=%s",
                    static_cast<int>(atmosCapability),
                    (atmosCapability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA) ? "true" : "false");
        } else {
            // Neither HDMI_ARC nor HDMI connected — host/TV-panel fallback.
            // DS_IARM: device::Host::getInstance().getSinkDeviceAtmosCapability()
            //   calls dsGetSinkDeviceAtmosCapability(NULL, ...) — handle=NULL.
            // DS_COMRPC: passing handle=0 maps to NULL at the HAL level (server
            //   casts int32_t to intptr_t before calling the HAL function).
            TRACE(Trace::Error,
                  (_T("getSinkAtmosCapability failure: neither HDMI0 nor HDMI_ARC connected!\n")));
            audio->GetAudioSinkDeviceAtmosCapability(0, atmosCapability);
        }

        audio->Release();
        supported = (atmosCapability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // SoundMode
    // Copied from entservices-playerinfo DS_COMRPC/PlatformImplementation.cpp
    // Adapted: AcquireAudioInterface() instead of DSHelper::AcquireSubInterface<>
    //          DSHelper::getAudioPortEntries() + getCachedAudioPortHandle() (same as PlayerInfo)
    //          Exchange::IAudioOutput::AudioModes instead of Dolby::IOutput::SoundModes
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::SoundMode(Exchange::IAudioOutput::AudioModes& mode)
    {
        mode = Exchange::IAudioOutput::UNKNOWN;

        using AudioPortType = Exchange::IDeviceSettingsAudio::AudioPortType;
        using StereoMode    = Exchange::IDeviceSettingsAudio::StereoMode;

        // Port priority: HDMI_ARC > HDMI > SPEAKER > SPDIF > HEADPHONE
        static const AudioPortType kPriority[] = {
            AudioPortType::AUDIO_PORT_TYPE_HDMIARC,
            AudioPortType::AUDIO_PORT_TYPE_HDMI,
            AudioPortType::AUDIO_PORT_TYPE_SPEAKER,
            AudioPortType::AUDIO_PORT_TYPE_SPDIF,
            AudioPortType::AUDIO_PORT_TYPE_HEADPHONE
        };
        static const size_t kPriorityCount = sizeof(kPriority) / sizeof(kPriority[0]);

        auto* audio = AcquireAudioInterface();
        if (audio == nullptr) {
            LOGERR("SoundMode: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        // Fetch entries via DSHelper — same as PlayerInfo DS_COMRPC
        std::vector<AudioPortEntry> entries;
        DSHelper::getAudioPortEntries(entries);

        bool found = false;
        for (size_t pi = 0; pi < kPriorityCount && !found; ++pi) {
            AudioPortType targetType = kPriority[pi];

            for (size_t ei = 0; ei < entries.size() && !found; ++ei) {
                if (entries[ei].type != targetType) continue;

                int32_t handle = DSHelper::getCachedAudioPortHandle(entries[ei].name);
                if (handle == INVALID_DS_HANDLE) continue;

                bool enabled   = false;
                bool connected = false;
                audio->IsAudioPortEnabled(handle, enabled);
                audio->IsAudioOutputConnected(handle, connected);
                if (!enabled || !connected) continue;

                StereoMode stereoMode = StereoMode::AUDIO_STEREO_UNKNOWN;
                if (audio->GetStereoMode(handle, stereoMode) == Core::ERROR_NONE) {
                    mode = DsAudioModeToSoundMode(stereoMode);

                    // SOUNDMODE_AUTO for HDMI_ARC, SPDIF, HDMI, SPEAKER — mirrors DS_IARM
                    if ((targetType == AudioPortType::AUDIO_PORT_TYPE_HDMIARC ||
                         targetType == AudioPortType::AUDIO_PORT_TYPE_SPDIF   ||
                         targetType == AudioPortType::AUDIO_PORT_TYPE_HDMI    ||
                         targetType == AudioPortType::AUDIO_PORT_TYPE_SPEAKER)) {
                        int32_t autoMode = 0;
                        if (audio->GetStereoAuto(handle, autoMode) == Core::ERROR_NONE && autoMode) {
                            mode = Exchange::IAudioOutput::SOUNDMODE_AUTO;
                            LOGINFO("setting audio mode as auto");
                        }
                    }

                    LOGINFO("SoundMode: port type=%d index=%d, mode=%d",
                            static_cast<int>(targetType), entries[ei].index, static_cast<int>(mode));
                    found = true;
                }
            }
        }

        if (!found) {
            LOGWARN("SoundMode: No enabled audio port found matching precedence.");
        }

        audio->Release();
        return Core::ERROR_NONE;
    }

} // namespace Plugin
} // namespace WPEFramework
