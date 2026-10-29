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
	unsubscribeFromCecSink();
	if (m_shellService) 
	{
            m_shellService->Release();
            m_shellService = nullptr;
      	}
    }

    // -------------------------------------------------------------------------
    // Exchange::IConfiguration::Configure
    // -------------------------------------------------------------------------

    uint32_t AudioOutputImplementation::Configure(PluginHost::IShell* service)
    {
        ASSERT(service != nullptr);
 	m_shellService = service;
        m_shellService->AddRef();		
        // Open the COM-RPC link to the DeviceSettings plugin. If DeviceSettings is
        // already active, OnDeviceSettingsActivated() fires synchronously here.
	LOGINFO("Configure: Opening DeviceSettings COM-RPC link for AudioOutput plugin");
        const uint32_t result = DSHelper::Open(service, "AudioOutput");
        if (result != Core::ERROR_NONE) {
            LOGERR("Configure: DSHelper::Open() failed with result=%u — audio events will NOT work", result);
        } else {
            LOGINFO("Configure: DSHelper::Open() succeeded");
        }
	subscribeToCecSink(); 
        return Core::ERROR_NONE;
    }
    
    void AudioOutputImplementation::subscribeToCecSink()
    {
    	if (m_shellService == nullptr) return;

    	_cecSink = m_shellService->QueryInterfaceByCallsign<Exchange::IHdmiCecSink>("org.rdk.HdmiCecSink");
    	if (_cecSink == nullptr) {
        	LOGERR("HdmiCecSink not active yet; ARC audio status events unavailable");
        	return;                    // unavailable case — see note below
    	}
    	_cecSink->Register(&_cecSinkNotification);
    	LOGINFO("Subscribed to HdmiCecSink ReportAudioStatusEvent");
    }

    void AudioOutputImplementation::unsubscribeFromCecSink()
    {
	if (_cecSink != nullptr) {
            _cecSink->Unregister(&_cecSinkNotification);
            _cecSink->Release();
            _cecSink = nullptr;
        }
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
				JsonObject params;
				params["audioPort"] = audioport;
           			params["volumeLevel"] =	level;
				dispatchEvent(AUDIOOUTPUT_EVT_ONVOLUMELEVELCHANGED, params);
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
		    //return Core::ERROR_UNAVAILABLE;
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
    //    LOGINFOMETHOD();
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
                            JsonObject params;
			    params["muted"] = muted;
		            params["audioPort"] = audioport;
		            dispatchEvent(AUDIOOUTPUT_EVT_ONMUTESTATUSCHANGED, params);
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
	//LOGINFOMETHOD();
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
    // IAudioOutput::SetAudioConfig / GetAudioConfig / GetSupportedAudioConfigs (COM-RPC)
    // Delegates to IDeviceSettingsAudio::SetApplicationAudioConfig / GetApplicationAudioConfig /
    // GetApplicationAudioConfigList. The handle is unused by the underlying HAL call (mirrors
    // the original libds Host::setApplicationAudioConfig(NULL, ...) semantics), so 0 is passed.
    // -------------------------------------------------------------------------

   /* Core::hresult AudioOutputImplementation::SetAudioConfig(const std::string& audioConfig, const bool enable)
    {
        LOGINFO("Set %s audio configuration to enable = %s", audioConfig.c_str(), enable ? "true" : "false");

        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("SetAudioConfig: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

     //   const Core::hresult result = audio->SetApplicationAudioConfig(0, audioConfig, enable);
        const Core::hresult result = Core::ERROR_NONE;//Dummy created for build breakage
        audio->Release();
        if (result != Core::ERROR_NONE) {
            LOGERR("SetAudioConfig: SetApplicationAudioConfig failed: %u", result);
        }
        return result;
    }

  //  Core::hresult AudioOutputImplementation::GetAudioConfig(const std::string& audioConfig, bool& enable) const
    {
        enable = false;
        LOGINFO("Get %s audio configuration", audioConfig.c_str());
        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("GetAudioConfig: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

//        const Core::hresult result = audio->GetApplicationAudioConfig(0, audioConfig, enable);
	const Core::hresult result = Core::ERROR_NONE;//Dummy created for build breakage
        audio->Release();
        if (result != Core::ERROR_NONE) {
            LOGERR("GetAudioConfig: GetApplicationAudioConfig failed: %u", result);
        } else {
            LOGINFO("%s audio config enabled = %s", audioConfig.c_str(), enable ? "true" : "false");
        }
        return result;
    }

    Core::hresult AudioOutputImplementation::GetSupportedAudioConfigs(Exchange::IAudioOutput::IAudioConfigListIterator*&  audioConfigs) const
    {
        audioConfigs = nullptr;

		auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("GetSupportedAudioConfigs: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        Exchange::IDeviceSettingsAudio::IDeviceSettingsAudioApplicationConfigIterator* dsConfigs = nullptr;
//        const Core::hresult result = audio->GetApplicationAudioConfigList(0, dsConfigs);
	const Core::hresult result = Core::ERROR_NONE;//Dummy created for build breakage
        audio->Release();
        if (result != Core::ERROR_NONE) {
            LOGERR("GetSupportedAudioConfigs: GetApplicationAudioConfigList failed: %u", result);
            return result;
        }
		
        std::vector<std::string> configList;
        if (dsConfigs != nullptr) {
            Exchange::IDeviceSettingsAudio::ApplicationAudioConfig entry;
            while (dsConfigs->Next(entry)) {
                LOGINFO("audio config = %s", entry.configName.c_str());
                configList.push_back(entry.configName);
            }
            dsConfigs->Release();
        }

        audioConfigs = (Core::Service<RPC::IteratorType<Exchange::IAudioOutput::IAudioConfigListIterator>>::Create<Exchange::IAudioOutput::IAudioConfigListIterator>(configList));
        return Core::ERROR_NONE;
    }
*/
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

    void AudioOutputImplementation::dispatchEvent(Event event, const JsonObject &params)
    { 
       Core::IWorkerPool::Instance().Submit(Job::Create(this, event, params));
    }

    void AudioOutputImplementation::Dispatch(Event event, const JsonObject &params)
    {
      _adminLock.Lock();
      std::list<Exchange::IAudioOutput::INotification*>::const_iterator index (_observers.begin());
      switch(event)
      {
	case AUDIOOUTPUT_EVT_ONVOLUMELEVELCHANGED:
	{
	   string audioPort = params["audioPort"];
	   uint32_t volumeLevel = params["volumeLevel"].Number();
	   while (index != _observers.end())
	   {
		   (*index)->OnVolumeLevelChanged(audioPort, volumeLevel);
		   ++index;
	   }
	   break;
	}
	case AUDIOOUTPUT_EVT_ONMUTESTATUSCHANGED:
	{
	    string audioPort = params["audioPort"];
	    bool muted = params["muted"].Boolean();
	    while (index != _observers.end())
            {
	  	(*index)->OnMuteStatusChanged(audioPort, muted);
		++index;
	    }
	    break;
	}
	default:
               LOGWARN("Event[%u] not handled", event);
               break;
      }	       
      _adminLock.Unlock();
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

        auto* audio = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsAudio>();
        if (audio == nullptr) {
            LOGERR("AtmosMetadata: IDeviceSettingsAudio unavailable");
            return Core::ERROR_UNAVAILABLE;
        }

        // Cached handles populated by DSHelper on DeviceSettings activation.
        const int32_t arcHandle  = DSHelper::getCachedAudioPortHandle("HDMI_ARC0");
        const int32_t hdmiHandle = DSHelper::getCachedAudioPortHandle("HDMI0");
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
                                                   : DSHelper::getCachedAudioPortHandle("SPEAKER0");
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

  void AudioOutputImplementation::OnArcAudioStatusChanged(int muteStatus, int volumeLevel) 
  {
    LOGINFO("ARC audio status changed: mute=%d, volume=%d", muteStatus, volumeLevel);
    if ( volumeLevel < 0 || volumeLevel > 100 ) {
         LOGWARN("Invalid volumeLevel in ARC Audio Status Event");
         return;
     }
     if ( volumeLevel != hdmiArcVolumeLevel) {
          hdmiArcVolumeLevel = volumeLevel;
          JsonObject volParams;
          volParams["volumeLevel"] = (int)hdmiArcVolumeLevel;
	      volParams["audioPort"] = "HDMI_ARC0";
          dispatchEvent(AUDIOOUTPUT_EVT_ONVOLUMELEVELCHANGED, volParams);
     }

     if ( muteStatus < 0 || muteStatus > 1 ) {
         LOGWARN("Invalid muteStatus in ARC Audio Status Event");
         return;
      }
      bool bMuteStatus = (muteStatus != 0);
      if (bMuteStatus != hdmiArcMuteStatus) {
           hdmiArcMuteStatus = bMuteStatus;
           JsonObject params;
           params["muted"] = hdmiArcMuteStatus;
	       params["audioPort"] = "HDMI_ARC0";
	       dispatchEvent(AUDIOOUTPUT_EVT_ONMUTESTATUSCHANGED, params);
       }
    } 

    void AudioOutputImplementation::onAudioDeviceConnectedStatusEventHandler(string status, string audioDeviceConnected)
    {
        LOGINFO("ARC audio device connection status changed: status=%s, audioDeviceConnected=%s", status.c_str(), audioDeviceConnected.c_str());
        //Reset to initial value incase of disconnection
        hdmiArcVolumeLevel = 0;
        hdmiArcMuteStatus = false;
    }

} // namespace Plugin
} // namespace WPEFramework
