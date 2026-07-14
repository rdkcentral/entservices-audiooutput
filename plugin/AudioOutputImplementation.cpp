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

    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(const device::AudioStereoMode& smode);
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
            registerDsEventHandlers();
        } catch (const device::Exception& err) {
            LOGWARN("device::Manager::Initialize failed : {%s}", err.what());
        }
    }

    AudioOutputImplementation::~AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Destructor");

        try {
            if (_registeredDsEventHandlers) {
                unregisterDsEventHandlers();
            }
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

        bool cap = false;
        if (AtmosMetadata(cap) != Core::ERROR_NONE) {
            LOGERR("UpdateCache: failed to get atmos metadata from HAL");
        }

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        if (SoundMode(mode) != Core::ERROR_NONE) {
            LOGERR("UpdateCache: failed to get sound mode from HAL");
        }

        _adminLock.Lock();
        _atmosMetaData = cap;
        _soundMode = mode;
        _adminLock.Unlock();
        UpdateCache();
        LOGINFO("AudioOutputImplementation::Configure: initial dolbyAtmosExperience=%s",
                _dolbyAtmosExperience ? "true" : "false");

        return Core::ERROR_NONE;
    }

    void AudioOutputImplementation::registerDsEventHandlers()
    {
        if (!_registeredDsEventHandlers) {
            _registeredDsEventHandlers = true;
            device::Host::getInstance().Register(&_dsAudioPortNotification, "WPE[AudioOutput]");
            LOGINFO("Registered for IAudioOutputPortEvents");
        }
    }

    void AudioOutputImplementation::unregisterDsEventHandlers()
    {
        if (_registeredDsEventHandlers) {
            _registeredDsEventHandlers = false;
            device::Host::getInstance().UnRegister(&_dsAudioPortNotification);
            LOGINFO("Unregistered from IAudioOutputPortEvents");
        }
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
    // DS HAL callback for OnAudioModeEvent (IAudioOutputPortEvents)
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAudioModeChanged(dsAudioPortType_t portType, dsAudioStereoMode_t smode)
    {
        LOGINFO("AudioOutputImplementation::onAudioModeChanged: portType=%d, smode=%d",
                static_cast<int>(portType), static_cast<int>(smode));

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
       
        try {
            device::List<device::AudioOutputPort> aPorts = device::Host::getInstance().getAudioOutputPorts();
            for (size_t i = 0; i < aPorts.size(); i++) {
                device::AudioOutputPort &aPortObj = aPorts.at(i);
                dsAudioPortType_t typeId = static_cast<dsAudioPortType_t>(aPortObj.getType().getId());
                if ((typeId == portType) &&
                    (typeId == dsAUDIOPORT_TYPE_HDMI_ARC ||
                     typeId == dsAUDIOPORT_TYPE_SPDIF ||
                     typeId == dsAUDIOPORT_TYPE_HDMI) && aPortObj.getStereoAuto()) {
                    mode = Exchange::IAudioOutput::SOUNDMODE_AUTO;
                    break;
                } else if (typeId == portType) {
                    mode = DsAudioModeToSoundMode(device::AudioStereoMode(smode));
                    break;
                }
            }
        } catch (const device::Exception& err) {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }

        _adminLock.Lock();
        _soundMode = mode;
        _adminLock.Unlock();
        UpdateCache();
    }

    // -------------------------------------------------------------------------
    // onAtmosCapabilitiesChanged
    // DS HAL callback for OnDolbyAtmosCapabilitiesChanged
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAtmosCapabilitiesChanged(dsATMOSCapability_t atmosCapability, bool status)
    {
        LOGINFO("AudioOutputImplementation::onAtmosCapabilitiesChanged: atmosCapability=%d, status=%d",
                atmosCapability, static_cast<int>(status));
                
     	_adminLock.Lock();
        _atmosMetaData = (atmosCapability == dsAUDIO_ATMOS_ATMOSMETADATA);
        _adminLock.Unlock();

        UpdateCache();
    }

    // -------------------------------------------------------------------------
    // Private: SendNotify
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::SendNotify(bool dolbyAtmosExperience)
    {
        _adminLock.Lock();
        std::list<Exchange::IAudioOutput::INotification*> index(_observers);

        LOGINFO("AudioOutputImplementation: SendNotify: notifying %zu observers of dolbyAtmosExperience=%s",
                index.size(), dolbyAtmosExperience ? "true" : "false");
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
        case Exchange::IAudioOutput::PASSTHRU:
        case Exchange::IAudioOutput::DOLBYDIGITALPLUS:
	    case Exchange::IAudioOutput::SOUNDMODE_AUTO:
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

    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(
        const device::AudioStereoMode& smode)
    {
        if (smode == device::AudioStereoMode::kMono)     return Exchange::IAudioOutput::MONO;
        if (smode == device::AudioStereoMode::kStereo)   return Exchange::IAudioOutput::STEREO;
        if (smode == device::AudioStereoMode::kSurround) return Exchange::IAudioOutput::SURROUND;
        if (smode == device::AudioStereoMode::kPassThru) return Exchange::IAudioOutput::PASSTHRU;
        if (smode == device::AudioStereoMode::kDD)       return Exchange::IAudioOutput::DOLBYDIGITAL;
        if (smode == device::AudioStereoMode::kDDPlus)   return Exchange::IAudioOutput::DOLBYDIGITALPLUS;
        LOGWARN("Unknown AudioStereoMode encountered, returning UNKNOWN");
        return Exchange::IAudioOutput::UNKNOWN;
    }

    uint32_t AudioOutputImplementation::SoundMode(Exchange::IAudioOutput::AudioModes& mode) const
    {
	    mode = Exchange::IAudioOutput::UNKNOWN;
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

                    if ((aPort.getType().getId() == device::AudioOutputPortType::kARC ||
                         aPort.getType().getId() == device::AudioOutputPortType::kSPDIF ||
                         aPort.getType().getId() == device::AudioOutputPortType::kHDMI)
                            && aPort.getStereoAuto()) {
                        mode = Exchange::IAudioOutput::SOUNDMODE_AUTO;
                        LOGINFO("setting audio mode as auto");
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
