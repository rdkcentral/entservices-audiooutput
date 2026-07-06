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
#include <interfaces/IAudioOutput.h>
#include <interfaces/IConfiguration.h>
#include <interfaces/json/JAudioOutput.h>
#include <interfaces/json/JsonData_AudioOutput.h>
#include "UtilsLogging.h"
#include "tracing/Logging.h"

namespace WPEFramework {
namespace Plugin {

    class AudioOutput : public PluginHost::IPlugin, public PluginHost::JSONRPC {

    private:
        class Notification : public Exchange::IAudioOutput::INotification {
        public:
            Notification() = delete;
            Notification(const Notification&) = delete;
            Notification& operator=(const Notification&) = delete;

            explicit Notification(AudioOutput* parent)
                : _parent(*parent)
                , _client(nullptr)
            {
                ASSERT(parent != nullptr);
            }
            ~Notification() override = default;

            void Initialize(Exchange::IAudioOutput* client)
            {
                ASSERT(client != nullptr);
                _client = client;
                _client->AddRef();
                _client->Register(this);
            }

            void Deinitialize()
            {
                ASSERT(_client != nullptr);
                if (_client != nullptr) {
                    _client->Unregister(this);
                    _client->Release();
                    _client = nullptr;
                }
            }

            void OnDolbyAtmosExperienceChanged(const bool dolbyAtmosExperience) override
            {
                LOGINFO("AudioOutput::Notification::OnDolbyAtmosExperienceChanged: dolbyAtmosExperience=%s",
                        dolbyAtmosExperience ? "true" : "false");
                Exchange::JAudioOutput::Event::OnDolbyAtmosExperienceChanged(_parent, dolbyAtmosExperience);
            }

            BEGIN_INTERFACE_MAP(Notification)
            INTERFACE_ENTRY(Exchange::IAudioOutput::INotification)
            END_INTERFACE_MAP

        private:
            AudioOutput& _parent;
            Exchange::IAudioOutput* _client;
        };

    public:
        AudioOutput(const AudioOutput&) = delete;
        AudioOutput& operator=(const AudioOutput&) = delete;

        AudioOutput();
        virtual ~AudioOutput();

        BEGIN_INTERFACE_MAP(AudioOutput)
        INTERFACE_ENTRY(PluginHost::IPlugin)
        INTERFACE_ENTRY(PluginHost::IDispatcher)
        INTERFACE_AGGREGATE(Exchange::IAudioOutput, _audioOutput)
        END_INTERFACE_MAP

        // IPlugin methods
        const string Initialize(PluginHost::IShell* service) override;
        void Deinitialize(PluginHost::IShell* service) override;
        string Information() const override;

    private:
        void Deactivated(RPC::IRemoteConnection* connection);

    private:
        PluginHost::IShell* _service{};
        uint32_t _connectionId{};
        Exchange::IAudioOutput* _audioOutput{};
        Exchange::IConfiguration* _configure{};
        Core::Sink<Notification> _notification;
    };

} // namespace Plugin
} // namespace WPEFramework
