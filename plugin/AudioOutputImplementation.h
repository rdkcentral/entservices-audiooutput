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

#pragma once

#include "Module.h"
#include <interfaces/Ids.h>
#include <interfaces/IAudioOutput.h>
#include <interfaces/IConfiguration.h>

#include <com/com.h>
#include <core/core.h>
#include <atomic>
#include <list>
#include <string>

#include "host.hpp"
#include "dsAudio.h"

namespace WPEFramework {
namespace Plugin {

    class AudioOutputImplementation
        : public Exchange::IConfiguration
        , public Exchange::IAudioOutput {

    public:
        AudioOutputImplementation(const AudioOutputImplementation&) = delete;
        AudioOutputImplementation& operator=(const AudioOutputImplementation&) = delete;

        AudioOutputImplementation();
        ~AudioOutputImplementation() override;

        BEGIN_INTERFACE_MAP(AudioOutputImplementation)
        INTERFACE_ENTRY(Exchange::IConfiguration)
        INTERFACE_ENTRY(Exchange::IAudioOutput)
        END_INTERFACE_MAP

        // IAudioOutput
        Core::hresult DolbyAtmosExperience(bool& enabled /* @out */) const override;
        Core::hresult Register(Exchange::IAudioOutput::INotification* notification) override;
        Core::hresult Unregister(const Exchange::IAudioOutput::INotification* notification) override;

        // Exchange::IConfiguration
        uint32_t Configure(PluginHost::IShell* service) override;

    private:
        enum Event {
            EV_ATMOS_EXP_CHANGED = 0
        };

        // Event payload: first = new dolbyAtmosExperience value; second unused.
        struct ParamsType {
            int32_t first;
            int32_t second;
        };

        // Worker-pool job: carries (impl*, Event, ParamsType); calls impl->Dispatch() on a worker thread.
        class DispatchJob : public Core::IDispatch {
        protected:
            DispatchJob(AudioOutputImplementation* au, Event ev, const ParamsType& params)
                : _parent(au), _ev(ev), _params(params)
            {
                if (_parent != nullptr) {
                    _parent->AddRef();
                }
            }

        public:
            DispatchJob() = delete;
            DispatchJob(const DispatchJob&) = delete;
            DispatchJob& operator=(const DispatchJob&) = delete;
            ~DispatchJob() override
            {
                if (_parent != nullptr) {
                    _parent->Release();
                }
            }

            static Core::ProxyType<Core::IDispatch> Create(AudioOutputImplementation* au, Event ev, const ParamsType& params)
            {
                return Core::ProxyType<Core::IDispatch>(Core::ProxyType<DispatchJob>::Create(au, ev, params));
            }

            void Dispatch() override { _parent->Dispatch(_ev, _params); }

        private:
            AudioOutputImplementation* _parent;
            Event _ev;
            const ParamsType _params;
        };

    private:
        // HAL query helpers — logic copied from entservices-playerinfo PlatformImplementation.cpp
        uint32_t AtmosMetadata(bool& supported) const;
        uint32_t SoundMode(Exchange::IAudioOutput::AudioModes& mode) const;

        bool EvaluateCurrentAtmosExperience(Exchange::IAudioOutput::AudioModes mode, bool cap) const;

        void SendNotify(bool dolbyAtmosExperience);
        void Dispatch(Event ev, const ParamsType& params);
        void dispatchEvent(Event ev, const ParamsType& params);

    private:
        mutable Core::CriticalSection _adminLock;

        // Last value reported; used only to detect changes for notification
        mutable std::atomic<bool> _dolbyAtmosExperience{false};

        // Observer list
        std::list<Exchange::IAudioOutput::INotification*> _observers;
    };

} // namespace Plugin
} // namespace WPEFramework
