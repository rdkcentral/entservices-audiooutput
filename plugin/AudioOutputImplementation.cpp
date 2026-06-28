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

#include <core/core.h>
#include "UtilsLogging.h"

#include "host.hpp"
#include "audioOutputPort.hpp"
#include "audioOutputPortType.hpp"
#include "audioStereoMode.hpp"
#include "manager.hpp"
#include "exception.hpp"
#include "dsAudio.h"

namespace WPEFramework {
namespace Plugin {

    SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0);

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    AudioOutputImplementation::AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Constructor");

        try {
            device::Manager::Initialize();
            LOGINFO("device::Manager::Initialize success");
        } catch (const std::exception& e) {
            LOGERR("device::Manager::Initialize failed, Exception: {%s}", e.what());
        }
    }

    AudioOutputImplementation::~AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Destructor");

        try {
            device::Manager::DeInitialize();
            LOGINFO("device::Manager::DeInitialize success");
        } catch (const std::exception& e) {
            LOGERR("device::Manager::DeInitialize failed, Exception: {%s}", e.what());
        }
    }

    // -------------------------------------------------------------------------
    // Initialize / Deinitialize
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::Initialize(PluginHost::IShell* service)
    {
        ASSERT(service != nullptr);

        _service = service;
        _service->AddRef();

        InitializePlayerInfo();
        InitializeDisplaySettings();
        UpdateCache();

        LOGINFO("AudioOutputImplementation::Initialize: initial dolbyAtmosExperience=%s",
                _dolbyAtmosExperience ? "true" : "false");

        return Core::ERROR_NONE;
    }

    void AudioOutputImplementation::InitializePlayerInfo()
    {
        LOGINFO("Connect the COM-RPC socket for PlayerInfo");

        _playerInfo = _service->QueryInterfaceByCallsign<Exchange::Dolby::IOutput>(PLAYERINFO_CALLSIGN);
        if (_playerInfo != nullptr) {
            _playerInfo->Register(this);
        }
    }

    void AudioOutputImplementation::InitializeDisplaySettings()
    {
        LOGINFO("Connect the JSON-RPC socket for DisplaySettings");

        _displaySettingsClient = new WPEFramework::JSONRPC::LinkType<Core::JSON::IElement>(
            _T(DISPLAYSETTINGS_CALLSIGN_VER), _T("AudioOutput"), false, string{});
        if (nullptr == _displaySettingsClient) {
            LOGERR("JSONRPC: %s: initialization failed", DISPLAYSETTINGS_CALLSIGN_VER);
        } else {
            _displaySettingsClient->Subscribe<JsonObject>(1000, _T("onAtmosCapabilityChanged"),
                &AudioOutputImplementation::onAtmosCapabilityChanged, this);
        }
    }

    void AudioOutputImplementation::UpdateCache()
    {
        bool cap = false;
        if (AtmosMetadata(cap) == Core::ERROR_NONE) {
            _adminLock.Lock();
            _atmosMetaData = cap;
            _adminLock.Unlock();
        }

        Exchange::Dolby::IOutput::SoundModes mode = Exchange::Dolby::IOutput::UNKNOWN;
        if (SoundMode(mode) == Core::ERROR_NONE) {
            _adminLock.Lock();
            _soundMode = mode;
            _adminLock.Unlock();
        }

        _adminLock.Lock();
        _dolbyAtmosExperience = EvaluateCurrentAtmosExperience();
        _adminLock.Unlock();
    }

    uint32_t AudioOutputImplementation::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);

        if (_playerInfo != nullptr) {
            _playerInfo->Unregister(this);
            _playerInfo->Release();
            _playerInfo = nullptr;
        }

        if (_displaySettingsClient != nullptr) {
            _displaySettingsClient->Unsubscribe(1000, _T("onAtmosCapabilityChanged"));
            delete _displaySettingsClient;
            _displaySettingsClient = nullptr;
        }

        if (_service != nullptr) {
            _service->Release();
            _service = nullptr;
        }

        return Core::ERROR_NONE;
    }

    // -------------------------------------------------------------------------
    // IAudioOutput::DolbyAtmosExperience
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::DolbyAtmosExperience(bool& enabled) const
    {
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
    // Dolby::IOutput::INotification::AudioModeChanged
    // Received when PlayerInfo detects an audio mode change
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::AudioModeChanged(const Exchange::Dolby::IOutput::SoundModes mode, const bool enable)
    {
        LOGINFO("AudioOutputImplementation::AudioModeChanged: mode=%d", static_cast<int>(mode));

        _adminLock.Lock();
        _soundMode = mode;
        bool newValue = EvaluateCurrentAtmosExperience();
        bool changed = (newValue != _dolbyAtmosExperience);
        _dolbyAtmosExperience = newValue;
        _adminLock.Unlock();

        if (changed) {
            LOGINFO("AudioOutputImplementation: dolbyAtmosExperience changed to %s",
                    newValue ? "true" : "false");
            SendNotify(newValue);
        }
    }

    // -------------------------------------------------------------------------
    // onAtmosCapabilityChangedHandler
    // JSON-RPC event handler for DisplaySettings onAtmosCapabilityChanged
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAtmosCapabilityChanged(const JsonObject& parameters)
    {
        if (!parameters.HasLabel("currentAtmosCapability")) {
            LOGERR("AudioOutputImplementation: onAtmosCapabilityChanged: missing currentAtmosCapability");
            return;
        }

        const string& capStr = parameters["currentAtmosCapability"].String();
        if (capStr != "ATMOS_SUPPORTED" && capStr != "ATMOS_NOT_SUPPORTED") {
            LOGINFO("AudioOutputImplementation: unknown currentAtmosCapability value '%s', ignoring", capStr.c_str());
            return;
        }
        const bool cap = (capStr == "ATMOS_SUPPORTED");

        _adminLock.Lock();
        _atmosMetaData = cap;
        bool newValue = EvaluateCurrentAtmosExperience();
        bool changed = (newValue != _dolbyAtmosExperience);
        _dolbyAtmosExperience = newValue;
        _adminLock.Unlock();

        if (changed) {
            LOGINFO("AudioOutputImplementation: dolbyAtmosExperience changed to %s",
                    newValue ? "true" : "false");
            SendNotify(newValue);
        }
    }

    // -------------------------------------------------------------------------
    // Private: SendNotify
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::SendNotify(bool dolbyAtmosExperience)
    {
        _adminLock.Lock();
        std::list<Exchange::IAudioOutput::INotification*> index(_observers);

        for (auto* itr : index) {
            itr->OnDolbyAtmosExperienceChanged(dolbyAtmosExperience);
        }

        _adminLock.Unlock();
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
        case Exchange::Dolby::IOutput::PASSTHRU:
        case Exchange::Dolby::IOutput::DOLBYDIGITALPLUS:
        case Exchange::Dolby::IOutput::SOUNDMODE_AUTO:
            return true;
        default:
            return false;
        }
    }

    uint32_t AudioOutputImplementation::AtmosMetadata(bool& supported) const
    {
        dsATMOSCapability_t atmosCapability = dsAUDIO_ATMOS_NOTSUPPORTED;
        supported = false;
        string audioPort = "HDMI0"; //default to HDMI
        try
        {
            /*  Check if the device has an HDMI_ARC out. If ARC is connected, then SPEAKERS and SPDIF are disabled.
                So, check the atmos capability of the HDMI_ARC first*/
            device::List<device::AudioOutputPort> aPorts = device::Host::getInstance().getAudioOutputPorts();
            for (size_t i = 0; i < aPorts.size(); i++)
            {
                device::AudioOutputPort &aPort = aPorts.at(i);
                if(aPort.getName().find("HDMI_ARC") != std::string::npos)
                {
                    //the platform supports HDMI_ARC. Get the sound mode of the ARC port
                    audioPort = "HDMI_ARC0";
                    break;
                }
            }
            device::AudioOutputPort aPort = device::Host::getInstance().getAudioOutputPort(audioPort);
            if (aPort.isConnected())
            {
                aPort.getSinkDeviceAtmosCapability(atmosCapability);
            }
            else
            {
                TRACE(Trace::Error, (_T("getSinkAtmosCapability failure: neither HDMI0 nor HDMI_ARC connected!\n")));
                device::Host::getInstance().getSinkDeviceAtmosCapability(atmosCapability); //gets host device-sink's atmos caps (For TV panel, device Sink is itself)
            }
        }
        catch(const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }

        if(atmosCapability == dsAUDIO_ATMOS_ATMOSMETADATA) supported = true;
        return (Core::ERROR_NONE);
    }

    // -------------------------------------------------------------------------
    // DsAudioModeToSoundMode
    // Copied from entservices-playerinfo/plugin/DeviceSettings/PlatformImplementation.cpp
    // -------------------------------------------------------------------------

    static Exchange::Dolby::IOutput::SoundModes DsAudioModeToSoundMode(
        const device::AudioStereoMode& smode)
    {
        if (smode == device::AudioStereoMode::kMono)     return Exchange::Dolby::IOutput::MONO;
        if (smode == device::AudioStereoMode::kStereo)   return Exchange::Dolby::IOutput::STEREO;
        if (smode == device::AudioStereoMode::kSurround) return Exchange::Dolby::IOutput::SURROUND;
        if (smode == device::AudioStereoMode::kPassThru) return Exchange::Dolby::IOutput::PASSTHRU;
        if (smode == device::AudioStereoMode::kDD)       return Exchange::Dolby::IOutput::DOLBYDIGITAL;
        if (smode == device::AudioStereoMode::kDDPlus)   return Exchange::Dolby::IOutput::DOLBYDIGITALPLUS;
        LOGWARN("Unknown AudioStereoMode encountered, returning UNKNOWN");
        return Exchange::Dolby::IOutput::UNKNOWN;
    }

    uint32_t AudioOutputImplementation::SoundMode(Exchange::Dolby::IOutput::SoundModes& mode) const
    {
        mode = Exchange::Dolby::IOutput::UNKNOWN;
        std::vector<std::string> hdmiArcPorts, hdmiPorts, speakerPorts, spdifPorts, headphonePorts;

        try {
            device::List<device::AudioOutputPort> aPorts = device::Host::getInstance().getAudioOutputPorts();
            for (size_t i = 0; i < aPorts.size(); i++) {
                device::AudioOutputPort &aPort = aPorts.at(i);
                if (aPort.isEnabled() && aPort.isConnected()) {
                    auto typeId = aPort.getType().getId();
                    if (typeId == device::AudioOutputPortType::kARC)
                        hdmiArcPorts.push_back(aPort.getName());
                    else if (typeId == device::AudioOutputPortType::kHDMI)
                        hdmiPorts.push_back(aPort.getName());
                    else if (typeId == device::AudioOutputPortType::kSPEAKER)
                        speakerPorts.push_back(aPort.getName());
                    else if (typeId == device::AudioOutputPortType::kSPDIF)
                        spdifPorts.push_back(aPort.getName());
                    else if (typeId == device::AudioOutputPortType::kHEADPHONE)
                        headphonePorts.push_back(aPort.getName());
                }
            }

            // Strict precedence: HDMI_ARC > HDMI > SPEAKER > SPDIF > HEADPHONE
            // first enumerated port is intentionally selected if multiple exist.
            std::string selectedPort;
            if (!hdmiArcPorts.empty()) {
                selectedPort = hdmiArcPorts.front();
            } else if (!hdmiPorts.empty()) {
                selectedPort = hdmiPorts.front();
            } else if (!speakerPorts.empty()) {
                selectedPort = speakerPorts.front();
            } else if (!spdifPorts.empty()) {
                selectedPort = spdifPorts.front();
            } else if (!headphonePorts.empty()) {
                selectedPort = headphonePorts.front();
            }

            if (!selectedPort.empty()) {
                device::AudioOutputPort aPort = device::Host::getInstance().getAudioOutputPort(selectedPort);
                if (aPort.isConnected()) {
                    device::AudioStereoMode soundmode = aPort.getStereoMode();
                    mode = DsAudioModeToSoundMode(soundmode);
                    // Auto mode for HDMI ARC and SPDIF
                    if ((aPort.getType().getId() == device::AudioOutputPortType::kARC || aPort.getType().getId() == device::AudioOutputPortType::kSPDIF)
                            && aPort.getStereoAuto()) {
                        mode = Exchange::Dolby::IOutput::SOUNDMODE_AUTO;
                    }
                    LOGINFO("Audio port %s has sound mode %d", selectedPort.c_str(), mode);
                } else {
                    LOGWARN("Selected audio port %s is no longer connected.", selectedPort.c_str());
                }
            } else {
                LOGWARN("No enabled and connected audio port found matching precedence.");
            }
        } catch (const device::Exception& err) {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }

        return Core::ERROR_NONE;
    }

} // namespace Plugin
} // namespace WPEFramework
