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

static int  hdmiArcVolumeLevel = 0;
static bool hdmiArcMuteStatus = false;

namespace WPEFramework {
namespace Plugin {

    static Exchange::IAudioOutput::AudioModes DsAudioModeToSoundMode(
        Exchange::IDeviceSettingsAudio::StereoMode smode);
    SERVICE_REGISTRATION(AudioOutputImplementation, 1, 0);

    // -------------------------------------------------------------------------
    // Constructor / Destructor
    // -------------------------------------------------------------------------

    AudioOutputImplementation::AudioOutputImplementation()
        : _dsAudioNotification(*this)
        , _dsHdminNotification(*this)
        , _hdmiCecSinkNotification(this)
    {
        LOGINFO("AudioOutputImplementation Constructor");
    }

    AudioOutputImplementation::~AudioOutputImplementation()
    {
        LOGINFO("AudioOutputImplementation Destructor");

        if (_hdmiCecSink != nullptr) {
            _hdmiCecSink->Unregister(&_hdmiCecSinkNotification);
            _hdmiCecSink->Release();
            _hdmiCecSink = nullptr;
        }

        // Unregister the DS notifications before the COM-RPC link is closed.
        auto* hdmiIn = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsHDMIIn>();
        if (hdmiIn != nullptr) {
            hdmiIn->Unregister(&_dsHdminNotification);
            hdmiIn->Release();
        }

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

        _hdmiCecSink = service->QueryInterfaceByCallsign<Exchange::IHdmiCecSink>("org.rdk.HdmiCecSink");
        if (_hdmiCecSink != nullptr) {
            _hdmiCecSink->Register(&_hdmiCecSinkNotification);
            LOGINFO("Configure: registered for HdmiCecSink ReportAudioDevicePowerStatus notification");
        } else {
            LOGWARN("Configure: HdmiCecSink not available, ReportAudioDevicePowerStatus will not be received");
        }

        return Core::ERROR_NONE;
    }

    void AudioOutputImplementation::OnHdmiInEventHotPlug(int port, bool isConnected)
    {
        LOGINFO("Received OnHdmiInEventHotPlug callback, port: %d, isConnected: %s", port, isConnected ? "true" : "false");
        dispatchEvent(EV_REEVALUATE_ATMOS, ParamsType{ 0, 0 });
    }

    void AudioOutputImplementation::ReportAudioDevicePowerStatus(const int powerStatus)
    {
        // Only the first report after boot is acted on; this event arrives frequently.
        if (!_externalArcEnabled) {
            LOGINFO("AudioOutputImplementation::ReportAudioDevicePowerStatus: powerStatus=%d", powerStatus);
            dispatchEvent(EV_REEVALUATE_ATMOS, ParamsType{ 0, 0 });
	    _externalArcEnabled = true;
        }
    }

    void AudioOutputImplementation::OnArcAudioStatusChanged(int muteStatus, int volumeLevel) 
    {
        LOGINFO("ARC audio status changed: mute=%d, volume=%d", muteStatus, volumeLevel);
        if ( volumeLevel < 0 || volumeLevel > 100 ) {
            LOGWARN("Invalid volumeLevel in ARC Audio Status Event");
            return;
        }
        if ( volumeLevel != hdmiArcVolumeLevel) {
            hdmiArcVolumeLevel = volumeLevel;
            dispatchEvent(EV_ONVOLUMELEVEL_CHANGED, ParamsType{ volumeLevel, 0, "HDMI_ARC0" });
        }

        if ( muteStatus < 0 || muteStatus > 1 ) {
            LOGWARN("Invalid muteStatus in ARC Audio Status Event");
            return;
        }
        bool bMuteStatus = (muteStatus != 0);
        if (bMuteStatus != hdmiArcMuteStatus) {
            hdmiArcMuteStatus = bMuteStatus;
            dispatchEvent(EV_ONMUTESTATUS_CHANGED, ParamsType{ bMuteStatus ? 1 : 0, 0, "HDMI_ARC0" });
        }
    } 

    void AudioOutputImplementation::OnAudioDeviceConnectedStatusEventHandler(string status, string audioDeviceConnected)
    {
        LOGINFO("ARC audio device connection status changed: status=%s, audioDeviceConnected=%s", status.c_str(), audioDeviceConnected.c_str());
        //Reset to initial value incase of disconnection
        hdmiArcVolumeLevel = 0;
        hdmiArcMuteStatus = false;
    }

    // -------------------------------------------------------------------------
    // DSHelper lifecycle hooks
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::OnDeviceSettingsActivated()
    {
        LOGINFO("AudioOutputImplementation: OnDeviceSettingsActivated — registering DS audio notification");
        _dsActivated = true;

        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio != nullptr) {
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

        auto* hdmiIn = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsHDMIIn>();
        if (hdmiIn != nullptr) {
            const uint32_t hdmiInResult = hdmiIn->Register("AudioOutput", &_dsHdminNotification);
            if (hdmiInResult != Core::ERROR_NONE) {
                LOGERR("OnDeviceSettingsActivated: hdmiIn->Register() FAILED with error %u", hdmiInResult);
            }
            hdmiIn->Release();
        } else {
            LOGWARN("OnDeviceSettingsActivated: IDeviceSettingsHDMIIn not available, HDMI-In hot plug events will not be received");
        }

    }

    void AudioOutputImplementation::dispatchEvent(Event ev, const ParamsType& params)
    {
        Core::IWorkerPool::Instance().Submit(DispatchJob::Create(this, ev, params));
    }

    void AudioOutputImplementation::Dispatch(Event ev, const ParamsType& params)
    {
        if (ev == EV_AUDIOMODE_CHANGE) {
            onAudioModeChanged(static_cast<Exchange::IDeviceSettingsAudio::AudioPortType>(params.first),
                               static_cast<Exchange::IDeviceSettingsAudio::StereoMode>(params.second));
        } else if (ev == EV_ATMOS_CAPABILITY_CHANGE) {
            onAtmosCapabilitiesChanged(static_cast<Exchange::IDeviceSettingsAudio::DolbyAtmosCapability>(params.first),
                                       params.second != 0);
        } else if (ev == EV_REEVALUATE_ATMOS) {
            ReevaluateAtmosExperience();
        } else if (ev == EV_ATMOS_EXP_CHANGED) {
            SendNotify(params.first != 0);
        } else if (ev == EV_ONVOLUMELEVEL_CHANGED) {
            SendVolumeLevelNotify(params.audioPort, static_cast<uint32_t>(params.first));
        } else if (ev == EV_ONMUTESTATUS_CHANGED) {
            SendMuteStatusNotify(params.audioPort, params.first != 0);
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

    void AudioOutputImplementation::OnDeviceSettingsDeactivated()
    {
        LOGINFO("AudioOutputImplementation: OnDeviceSettingsDeactivated");
        _dsActivated = false;
    }

    // -------------------------------------------------------------------------
    // IAudioOutput::DolbyAtmosExperience
    // -------------------------------------------------------------------------
    Core::hresult AudioOutputImplementation::DolbyAtmosExperience(bool& enabled) const
    {
            bool cap = false;

            if (!_dsActivated) {
                LOGERR("DolbyAtmosExperience: failed dsmgr not activated yet");
                return Core::ERROR_GENERAL;
            }

            bool atmosErr = (AtmosMetadata(cap) != Core::ERROR_NONE);

            Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
            bool soundErr = (SoundMode(mode) != Core::ERROR_NONE);

            if (atmosErr || soundErr) {
                LOGERR("DolbyAtmosExperience: failed (atmosMetadata=%s, soundMode=%s)",
                       atmosErr ? "failed" : "ok", soundErr ? "failed" : "ok");
                return Core::ERROR_GENERAL;
            }
	    const bool newValue = EvaluateCurrentAtmosExperience(mode, cap);
            enabled = newValue;
            LOGINFO("AudioOutputImplementation: current dolbyAtmosExperience value: %s", newValue ? "true" : "false");
           
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
    // IAudioOutput::SetVolumeLevel
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::SetVolumeLevel(const string& audioPort, const uint32_t& volumeLevel, bool& success)
    {
	if (volumeLevel > 100) {
            LOGWARN("volumeLevel invalid: %u (expected range [0, 100])", volumeLevel);
       	    success = false;
            return Core::ERROR_INVALID_PARAMETER; 
    	}
	int level = volumeLevel;
        success = true;
        string audioport = (! audioPort.empty()) ? audioPort : "HDMI0";
	{
            const int32_t audioHandle = DSHelper::getCachedAudioPortHandle(audioport);
            if (INVALID_DS_HANDLE != audioHandle) {
                auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
                if (audio != nullptr) {
                    // DS_IARM: reads current_volumelevel = aPort.getLevel() before set
                    float currentLevel = -1.0f;
                    Core::hresult comResult = audio->GetAudioLevel(audioHandle, currentLevel);
                    if (comResult == Core::ERROR_NONE) {
                        comResult = audio->SetAudioLevel(audioHandle, static_cast<float>(volumeLevel));
                        if (comResult == Core::ERROR_NONE) {
                            if (static_cast<int>(currentLevel) != level) {
                                dispatchEvent(EV_ONVOLUMELEVEL_CHANGED, ParamsType{ level, 0, audioport });
                                LOGINFO("SetAudioLevel success");
                            }
                        } else {
                            LOGERR("SetAudioLevel failed for audioPort='%s', Error=%d", audioport.c_str(), static_cast<int>(comResult));
                            success = false;
                        }
                    } else {
                        LOGERR("GetAudioLevel failed for audioPort='%s', Error=%d", audioport.c_str(), static_cast<int>(comResult));
                        success = false;
                    }
                    audio->Release();
                } else {
                    LOGERR("IDeviceSettingsAudio not available");
                    success = false;
		    return Core::ERROR_UNAVAILABLE;
                }
            } else {
                LOGERR("Failed to get audio port handle for audioPort='%s'", audioport.c_str());
                success = false;
            }
        }
       	return (success ? Core::ERROR_NONE : Core::ERROR_GENERAL);
    }
    
    // -------------------------------------------------------------------------
    // IAudioOutput::SetMuted
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::SetMuted(const string& audioPort, const bool muted, bool& success)
    {
   
        static bool cache_muted = false;
        success = true;
        string audioport = (! audioPort.empty()) ? audioPort : "HDMI0";
        LOGWARN("AudioOutputImplementation::SetMuted called Audio Port :%s muted:%d\n", audioport.c_str(), muted);
	{
            const int32_t audioHandle = DSHelper::getCachedAudioPortHandle(audioport);
            if (INVALID_DS_HANDLE != audioHandle) {
                auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
                if (audio != nullptr) {
                    Core::hresult comResult = audio->SetAudioMute(audioHandle, muted);
                    if (comResult == Core::ERROR_NONE) {
                        if (cache_muted != muted) {
                            cache_muted = muted;
                            dispatchEvent(EV_ONMUTESTATUS_CHANGED, ParamsType{ muted ? 1 : 0, 0, audioport });
                        }
                    } else {
                        LOGERR("SetAudioMute failed for audioPort='%s', Error=%d", audioport.c_str(), static_cast<int>(comResult));
                        success = false;
                    }
                    audio->Release();
                } else {
                    LOGERR("IDeviceSettingsAudio not available for audioPort='%s'", audioport.c_str());
                    success = false;
                }
            } else {
                LOGERR("Failed to get audio port handle for audioPort='%s'", audioport.c_str());
                success = false;
            }
        }
        return (success ? Core::ERROR_NONE : Core::ERROR_GENERAL);
    }
    
   // -------------------------------------------------------------------------
    // IAudioOutput::GetVolumeLevel
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::GetVolumeLevel(const string& audioPort, string& affectedAudioPort, int& volumeLevel, bool& success)
    {
	success = true;
        float level = 0;
	string audioport = (! audioPort.empty()) ? audioPort : "HDMI0";
        // DS_IARM: HDMI_ARC0 uses cached hdmiArcVolumeLevel (updated via CEC onArcAudioStatusEventHandler)
        if (audioport == "HDMI_ARC0") {
            level = hdmiArcVolumeLevel;
            volumeLevel = level;
	        affectedAudioPort = audioport;
	        return Core::ERROR_NONE;
        }
 	{
         const int32_t audioHandle = DSHelper::getCachedAudioPortHandle(audioport);
          if (INVALID_DS_HANDLE != audioHandle) {
               auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
               if (audio != nullptr) {
                    float vol = 0;
                    Core::hresult comResult = audio->GetAudioLevel(audioHandle, vol);
                    if (comResult == Core::ERROR_NONE) {
                        level = vol;
                        volumeLevel = level;
			affectedAudioPort = audioport;
                    } else {
                        LOGERR("GetAudioLevel failed for audioPort='%s', Error=%d", audioport.c_str(), static_cast<int>(comResult));
                        success = false;
                    }
                    audio->Release();
                } else {
                    LOGERR("IDeviceSettingsAudio not available");
                    success = false;
                }
            } else {
                LOGERR("audio port handle not found for audioPort='%s'", audioport.c_str());
                success = false;
            }
        }
	return (success ? Core::ERROR_NONE : Core::ERROR_GENERAL);
    }
    
    // -------------------------------------------------------------------------
    // IAudioOutput::GetMuted
    // -------------------------------------------------------------------------

    Core::hresult AudioOutputImplementation::GetMuted(const string& audioPort, string& affectedAudioPort, bool& muted, bool& success)
    {
	
        success = true;
        bool muteStatus = false;
        string audioport = (! audioPort.empty()) ? audioPort : "HDMI0";
        {
          const int32_t audioHandle = DSHelper::getCachedAudioPortHandle(audioport);
          if (INVALID_DS_HANDLE != audioHandle) {
                auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
                if (audio != nullptr) {
                    Core::hresult comResult = audio->IsAudioMuted(audioHandle, muteStatus);
                    if (comResult == Core::ERROR_NONE) {
                        muted = muteStatus;
			affectedAudioPort = audioport;
                    } else {
                        LOGERR("IsAudioMuted failed for audioPort='%s', Error=%d", audioport.c_str(), static_cast<int>(comResult));
                        success = false;
                    }
                    audio->Release();
                } else {
                    LOGERR("IDeviceSettingsAudio not available");
                    success = false;
                }
            } else {
                LOGERR("audio port handle not found for audioPort='%s'", audioport.c_str());
                success = false;
            }
        }
        return (success ? Core::ERROR_NONE : Core::ERROR_GENERAL);
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
        bool isAudioModeChanged = false;
        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        std::vector<AudioPortEntry> entries;
        if (audio != nullptr && DSHelper::getAudioPortEntries(entries)) {
            for (const AudioPortEntry& e : entries) {
                int32_t portHandle = INVALID_DS_HANDLE;
                if (DSHelper::isAudioOutputPortConnected(audio, e.name, portHandle)) {
                    LOGINFO("Audio Port : [%s] is connected", e.name.c_str());
                    if (e.type == audioPortType) {
                        isAudioModeChanged = true;
                        break;
                    } else {
                        // Ignore invalid event when Audiomode set for hdmi_arc0 while no external devices connected(speaker0)
                        TRACE(Trace::Warning, (_T("Audio Mode not changed for connected port %s"), e.name.c_str()));
                    }
                }
            }
        }
        if (audio != nullptr) {
            audio->Release();
        }
        if (isAudioModeChanged) {
            Exchange::IAudioOutput::AudioModes mode = DsAudioModeToSoundMode(audioMode);
            bool cap = false;
            if (AtmosMetadata(cap) != Core::ERROR_NONE) {
                LOGERR("onAudioModeChanged: AtmosMetadata() query failed");
                return;
            }
            NotifyIfAtmosExperienceChanged(mode, cap);
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

        const bool cap =
            (atmosCapability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
        Exchange::IAudioOutput::AudioModes mode = Exchange::IAudioOutput::UNKNOWN;
        if (SoundMode(mode) != Core::ERROR_NONE) {
            LOGERR("onAtmosCapabilitiesChanged: SoundMode() query failed");
            return;
        }
        NotifyIfAtmosExperienceChanged(mode, cap);
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
    // Private: SendVolumeLevelNotify
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::SendVolumeLevelNotify(const string& audioPort, uint32_t volumeLevel)
    {
        std::list<Exchange::IAudioOutput::INotification*> index;

        _adminLock.Lock();
        index = _observers;
        for (auto* obs : index) {
            obs->AddRef();
        }
        _adminLock.Unlock();

        for (auto* obs : index) {
            obs->OnVolumeLevelChanged(audioPort, volumeLevel);
            obs->Release();
        }
    }

     // -------------------------------------------------------------------------
    // Private: SendMuteStatusNotify
    // -------------------------------------------------------------------------

    void AudioOutputImplementation::SendMuteStatusNotify(const string& audioPort, bool muted)
    {
        std::list<Exchange::IAudioOutput::INotification*> index;

        _adminLock.Lock();
        index = _observers;
        for (auto* obs : index) {
            obs->AddRef();
        }
        _adminLock.Unlock();

        for (auto* obs : index) {
            obs->OnMuteStatusChanged(audioPort, muted);
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
        LOGINFO("AtmosMetadata: arcHandle=%d, hdmiHandle=%d", arcHandle, hdmiHandle);

        if (TV == searchRdkProfile()) {
            // TV platform: use persisted user-intent enable state — HAL state is unreliable on some platforms
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
            Exchange::IDeviceSettingsAudio::DolbyAtmosCapability capability = Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;
            if (arcEnabled && arcHandle != INVALID_DS_HANDLE) {
                // ARC is enabled — query HDMI_ARC0 for ATMOS capability
                LOGINFO("AtmosMetadata: ARC enabled, querying HDMI_ARC0 for ATMOS capability");
                audio->GetAudioSinkDeviceAtmosCapability(arcHandle, capability);
            } else {
                // ARC not enabled — query the TV panel sink itself, matching legacy's
                // device::Host::getSinkDeviceAtmosCapability() which passes a NULL (no port) handle.
                LOGINFO("AtmosMetadata: ARC not enabled, querying TV panel ATMOS capability");
                audio->GetAudioSinkDeviceAtmosCapability(0, capability);
            }
            supported = (capability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
            LOGINFO("AtmosMetadata: capability=%d, supported=%s",
                    static_cast<int>(capability), supported ? "true" : "false");
        } else {
            // STB platform: audio goes through HDMI0 only, matching legacy's
            // audioPort="HDMI0" + aPort.isConnected() / Host fallback (no ARC, no SPEAKER on STB).
            LOGINFO("AtmosMetadata: STB platform");
            Exchange::IDeviceSettingsAudio::DolbyAtmosCapability capability = Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_NOT_SUPPORTED;
            auto* self = const_cast<AudioOutputImplementation*>(this);
            if (self->isAudioOutputPortConnected(audio, "HDMI0", hdmiHandle)) {
                audio->GetAudioSinkDeviceAtmosCapability(hdmiHandle, capability);
            } else {
                LOGWARN("AtmosMetadata: HDMI0 not connected, using host getSinkDeviceAtmosCapability");
                audio->GetAudioSinkDeviceAtmosCapability(0, capability);
            }
            supported = (capability == Exchange::IDeviceSettingsAudio::AUDIO_DOLBY_ATMOS_METADATA);
            LOGINFO("AtmosMetadata: capability=%d, supported=%s",
                    static_cast<int>(capability), supported ? "true" : "false");
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
