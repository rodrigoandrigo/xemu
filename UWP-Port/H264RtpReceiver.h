#pragma once

#include <deque>
#include <mutex>
#include <vector>

namespace UWP_Port
{
    ref class H264RtpReceiver sealed
    {
    internal:
        H264RtpReceiver();
        void Start(Platform::String^ bindAddress, unsigned short port,
                   unsigned int width, unsigned int height,
                   Windows::UI::Xaml::Controls::MediaElement^ output);
        void Stop();
        property Platform::String^ Status { Platform::String^ get(); }

    private:
        struct AccessUnit {
            std::vector<uint8_t> bytes;
            int64_t timestamp;
            bool keyframe;
        };

        void OnPacket(Windows::Networking::Sockets::DatagramSocket^ sender,
                      Windows::Networking::Sockets::DatagramSocketMessageReceivedEventArgs^ args);
        void OnStarting(Windows::Media::Core::MediaStreamSource^ sender,
                        Windows::Media::Core::MediaStreamSourceStartingEventArgs^ args);
        void OnSampleRequested(Windows::Media::Core::MediaStreamSource^ sender,
                               Windows::Media::Core::MediaStreamSourceSampleRequestedEventArgs^ args);
        void ParseRtp(const uint8_t* packet, size_t size);
        bool AppendNal(const uint8_t* data, size_t size);
        void FinishAccessUnit(uint32_t timestamp);
        Windows::Media::Core::MediaStreamSample^ TakeSample();
        Windows::Media::Core::MediaStreamSample^ CreateSample(AccessUnit&& unit);
        void CompletePendingRequest();
        void SetStatus(const wchar_t* status);

        Windows::Networking::Sockets::DatagramSocket^ m_socket;
        Windows::Media::Core::MediaStreamSource^ m_source;
        Windows::UI::Xaml::Controls::MediaElement^ m_output;
        Windows::Foundation::EventRegistrationToken m_messageToken;
        Windows::Foundation::EventRegistrationToken m_startingToken;
        Windows::Foundation::EventRegistrationToken m_sampleToken;
        Windows::Media::Core::MediaStreamSourceSampleRequest^ m_pendingRequest;
        Windows::Media::Core::MediaStreamSourceSampleRequestDeferral^ m_pendingDeferral;
        std::mutex m_mutex;
        std::deque<AccessUnit> m_ready;
        std::vector<uint8_t> m_current;
        uint32_t m_currentTimestamp;
        uint32_t m_baseTimestamp;
        uint16_t m_expectedSequence;
        bool m_haveTimestamp;
        bool m_haveSequence;
        bool m_fuActive;
        bool m_keyframe;
        Platform::String^ m_status;
    };
}
