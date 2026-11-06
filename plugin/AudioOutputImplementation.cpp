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

#include "host.hpp"
#include "audioOutputPort.hpp"
#include "audioOutputPortType.hpp"
#include "audioStereoMode.hpp"
#include "manager.hpp"
#include "exception.hpp"
#include "dsAudio.h"

namespace WPEFramework {
namespace Plugin {

    using JsonObject = Core::JSON::VariantContainer;

    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(const device::AudioStereoMode& smode);
    SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0);

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    AudioOutputImplementation::AudioOutputImplementation()
        : _hdmiCecSinkNotification(this)
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

        if (_hdmiCecSink != nullptr) {
            _hdmiCecSink->Unregister(&_hdmiCecSinkNotification);
            _hdmiCecSink->Release();
            _hdmiCecSink = nullptr;
        }

        try {
            unregisterDsEventHandlers();
            device::Manager::DeInitialize();
            LOGINFO("device::Manager::DeInitialize success");
        } catch (const device::Exception& err) {
            LOGWARN("device::Manager::DeInitialize failed: {%s}", err.what());
        }
    }

    uint32_t AudioOutputImplementation::Configure(PluginHost::IShell* service)
    {
        ASSERT(service != nullptr);

        _hdmiCecSink = service->QueryInterfaceByCallsign<Exchange::IHdmiCecSink>("org.rdk.HdmiCecSink");
        if (_hdmiCecSink != nullptr) {
            _hdmiCecSink->Register(&_hdmiCecSinkNotification);
            LOGINFO("Configure: registered for HdmiCecSink ReportAudioDevicePowerStatus notification");
        } else {
            LOGWARN("Configure: HdmiCecSink not available, ReportAudioDevicePowerStatus will not be received");
        }

        return Core::ERROR_NONE;
    }

    void AudioOutputImplementation::registerDsEventHandlers()
    {
        if (!_registeredDsEventHandlers) {
            device::Host::getInstance().Register(&_dsAudioPortNotification, "WPE[AudioOutput]");
            device::Host::getInstance().Register(&_dsHdmiInNotification, "WPE[AudioOutput]");
            _registeredDsEventHandlers = true;
            LOGINFO("Registered for IAudioOutputPortEvents and IHdmiInEvents");
        }
    }

    void AudioOutputImplementation::unregisterDsEventHandlers()
    {
        if (_registeredDsEventHandlers) {
            device::Host::getInstance().UnRegister(&_dsAudioPortNotification);
            device::Host::getInstance().UnRegister(&_dsHdmiInNotification);
            _registeredDsEventHandlers = false;
            LOGINFO("Unregistered from IAudioOutputPortEvents and IHdmiInEvents");
        }
    }

    void AudioOutputImplementation::OnHdmiInEventHotPlug(int port, bool isConnected)
    {
        LOGINFO("Received OnHdmiInEventHotPlug callback, port: %d, isConnected: %s", port, isConnected ? "true" : "false");
        dispatchEvent(EV_REEVALUATE_ATMOS, ParamsType{ 0, 0 });
    }

    void AudioOutputImplementation::ReportAudioDevicePowerStatus(const int powerStatus)
    {
        if (!_externalArcEnabled.exchange(true)) {
            LOGINFO("AudioOutputImplementation::ReportAudioDevicePowerStatus: powerStatus=%d", powerStatus);
            dispatchEvent(EV_REEVALUATE_ATMOS, ParamsType{ 0, 0 });
        }
    }

    // Queries both HAL values and notifies if the derived Atmos experience changed.
    void AudioOutputImplementation::ReevaluateAtmosExperience()
    {
        bool cap = false;
        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;

        const bool atmosErr = (AtmosMetadata(cap) != Core::ERROR_NONE);
        const bool soundErr = (SoundMode(mode) != Core::ERROR_NONE);
        if (atmosErr || soundErr) {
            LOGERR("ReevaluateAtmosExperience: failed (atmosMetadata=%s, soundMode=%s)",
                   atmosErr ? "failed" : "ok", soundErr ? "failed" : "ok");
            return;
        }
        NotifyIfAtmosExperienceChanged(mode, cap);
    }

    void AudioOutputImplementation::dispatchEvent(Event ev, const ParamsType& params)
    {
        Core::IWorkerPool::Instance().Submit(DispatchJob::Create(this, ev, params));
    }

    void AudioOutputImplementation::Dispatch(Event ev, const ParamsType& params)
    {
        if (ev == EV_AUDIOMODE_CHANGE) {
            onAudioModeChanged(static_cast<dsAudioPortType_t>(params.first),
                               static_cast<dsAudioStereoMode_t>(params.second));
        } else if (ev == EV_ATMOS_CAPABILITY_CHANGE) {
            onAtmosCapabilitiesChanged(static_cast<dsATMOSCapability_t>(params.first), params.second != 0);
        } else if (ev == EV_REEVALUATE_ATMOS) {
            ReevaluateAtmosExperience();
        } else if (ev == EV_ATMOS_EXP_CHANGED) {
            SendNotify(params.first != 0);
        }
    }

    void AudioOutputImplementation::NotifyIfAtmosExperienceChanged(Exchange::IAudioOutput::AudioModes mode, bool cap)
    {
        const bool newValue = EvaluateCurrentAtmosExperience(mode, cap);

        _adminLock.Lock();
        const bool changed = (_dolbyAtmosExperience != newValue);
        _dolbyAtmosExperience = newValue;
        _adminLock.Unlock();

        if (changed) {
            LOGINFO("AudioOutputImplementation: dolbyAtmosExperience changed to %s",
                    newValue ? "true" : "false");
            dispatchEvent(EV_ATMOS_EXP_CHANGED, ParamsType{ static_cast<int32_t>(newValue), 0 });
        }
    }

    // -------------------------------------------------------------------------
    // IAudioOutput::DolbyAtmosExperience
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::DolbyAtmosExperience(bool& enabled) const
    {
        bool cap = false;
        const bool atmosErr = (AtmosMetadata(cap) != Core::ERROR_NONE);

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        const bool soundErr = (SoundMode(mode) != Core::ERROR_NONE);

        if (atmosErr || soundErr) {
            LOGERR("DolbyAtmosExperience: failed (atmosMetadata=%s, soundMode=%s)",
                   atmosErr ? "failed" : "ok", soundErr ? "failed" : "ok");
            return Core::ERROR_GENERAL;
        }

        enabled = EvaluateCurrentAtmosExperience(mode, cap);
        LOGERR("DolbyAtmosExperience: enabled=%s", enabled ? "true" : "false");
        
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
    // DS HAL callback for OnAudioModeEvent (IAudioOutputPortEvents)
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAudioModeChanged(dsAudioPortType_t portType, dsAudioStereoMode_t smode)
    {
        LOGINFO("AudioOutputImplementation::onAudioModeChanged: portType=%d, smode=%d",
                static_cast<int>(portType), static_cast<int>(smode));

        bool isAudioModeChanged = false;

        try {
            device::List<device::AudioOutputPort> aPorts = device::Host::getInstance().getAudioOutputPorts();
            for (size_t i = 0; i < aPorts.size(); i++) {
                device::AudioOutputPort &aPort = aPorts.at(i);
                if (aPort.isEnabled() && aPort.isConnected()) {
                    auto typeId = aPort.getType().getId();
                    if (typeId == portType) {
                        isAudioModeChanged = true;
                        break;
                    } else {
                        // Ignore invalid event when Audiomode set for hdmi_arc0 while no external devices connected(speaker0)
                        TRACE(Trace::Warning, (_T("Audio Mode not changed for connected port %s"), aPort.getName().c_str()));
                    }
                }
            }
        } catch (const device::Exception& err) {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
        }

        if (isAudioModeChanged) {
            bool cap = false;
            if (AtmosMetadata(cap) != Core::ERROR_NONE) {
                LOGERR("onAudioModeChanged: AtmosMetadata() query failed");
                return;
            }
            NotifyIfAtmosExperienceChanged(DsAudioModeToSoundMode(device::AudioStereoMode(smode)), cap);
        }
    }

    // -------------------------------------------------------------------------
    // onAtmosCapabilitiesChanged
    // DS HAL callback for OnDolbyAtmosCapabilitiesChanged
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::onAtmosCapabilitiesChanged(dsATMOSCapability_t atmosCapability, bool status)
    {
        LOGINFO("AudioOutputImplementation::onAtmosCapabilitiesChanged: atmosCapability=%d, status=%d",
                atmosCapability, static_cast<int>(status));

        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        if (SoundMode(mode) != Core::ERROR_NONE) {
            LOGERR("onAtmosCapabilitiesChanged: SoundMode() query failed");
            return;
        }
        NotifyIfAtmosExperienceChanged(mode, atmosCapability == dsAUDIO_ATMOS_ATMOSMETADATA);
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

    bool AudioOutputImplementation::EvaluateCurrentAtmosExperience(Exchange::IAudioOutput::AudioModes mode, bool cap) const
    {
        if (!cap) {
            return false;
        }

        switch (mode) {
        case Exchange::IAudioOutput::PASSTHRU:
        case Exchange::IAudioOutput::DOLBYDIGITALPLUS:
        case Exchange::IAudioOutput::SOUNDMODE_AUTO:
        case Exchange::IAudioOutput::SURROUND:
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
            if (TV == searchRdkProfile())
            {
                // TV platform: query DisplaySettings for the persisted user-intent HDMI_ARC0
                // enabled flag. This is authoritative — do NOT rely on isConnected() (HAL
                // returns unreliable state on some platforms).
                bool arcEnabled = false;
                Core::SystemInfo::SetEnvironment(_T("THUNDER_ACCESS"), _T("127.0.0.1:9998"));
                WPEFramework::JSONRPC::LinkType<Core::JSON::IElement> dsClient(
                    _T("org.rdk.DisplaySettings.1"), _T("org.rdk.DisplaySettings.1"), false, _T(""));
                JsonObject params;
                JsonObject result;
                params["audioPort"] = "HDMI_ARC0";
                if (dsClient.Invoke<JsonObject, JsonObject>(2000, "getEnableAudioPort", params, result) == Core::ERROR_NONE) {
                    arcEnabled = result["enable"].Boolean();
                    LOGINFO("AtmosMetadata: getEnableAudioPort(HDMI_ARC0) = %s", arcEnabled ? "true" : "false");
                } else {
                    LOGWARN("AtmosMetadata: getEnableAudioPort JSON-RPC failed");
                }
                if (arcEnabled)
                {
                    // ARC is enabled — query HDMI_ARC0 port directly, bypassing isConnected()
                    LOGINFO("AtmosMetadata: ARC enabled, querying HDMI_ARC0 for ATMOS capability");
                    device::AudioOutputPort aPort = device::Host::getInstance().getAudioOutputPort("HDMI_ARC0");
                    aPort.getSinkDeviceAtmosCapability(atmosCapability);
                }
                else
                {
                    // ARC not enabled or JSON-RPC failed — query TV panel itself
                    LOGINFO("AtmosMetadata: ARC not enabled, querying TV panel ATMOS capability");
                    device::Host::getInstance().getSinkDeviceAtmosCapability(atmosCapability);
                }
            }
            else
            {
                // STB platform: audio goes through HDMI0
                LOGINFO("AtmosMetadata: STB platform, audioPort = %s", audioPort.c_str());
                device::AudioOutputPort aPort = device::Host::getInstance().getAudioOutputPort(audioPort);
                if (aPort.isConnected())
                {
                    aPort.getSinkDeviceAtmosCapability(atmosCapability);
                }
                else{
                    LOGWARN("AtmosMetadata: HDMI0 not connected, using host getSinkDeviceAtmosCapability");
                    device::Host::getInstance().getSinkDeviceAtmosCapability(atmosCapability);
                }
            }
        }
        catch(const device::Exception& err)
        {
            TRACE(Trace::Error, (_T("Exception during DeviceSetting library call. code = %d message = %s"), err.getCode(), err.what()));
	        return Core::ERROR_GENERAL;
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
                         aPort.getType().getId() == device::AudioOutputPortType::kSPDIF)
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
            return Core::ERROR_GENERAL;
        }

	return Core::ERROR_NONE;

    }

} // namespace Plugin
} // namespace WPEFramework
