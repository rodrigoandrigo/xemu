#include "pch.h"
#include "H264RtpReceiver.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Media::Core;
using namespace Windows::Media::MediaProperties;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Xaml::Controls;

namespace UWP_Port
{
H264RtpReceiver::H264RtpReceiver()
    : m_socket(nullptr), m_source(nullptr), m_output(nullptr),
      m_pendingRequest(nullptr), m_pendingDeferral(nullptr),
      m_currentTimestamp(0), m_baseTimestamp(0), m_expectedSequence(0),
      m_haveTimestamp(false), m_haveSequence(false), m_fuActive(false),
      m_keyframe(false), m_status("Stopped")
{
}

String^ H264RtpReceiver::Status::get()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void H264RtpReceiver::SetStatus(const wchar_t* status)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = ref new String(status);
}

void H264RtpReceiver::Start(String^ bindAddress, unsigned short port,
                            unsigned int width, unsigned int height,
                            MediaElement^ output)
{
    Stop();
    if (!output || !width || !height || !port) {
        throw ref new InvalidArgumentException("Invalid RTP/H.264 receiver settings");
    }

    auto properties = VideoEncodingProperties::CreateH264();
    properties->Width = width;
    properties->Height = height;
    properties->FrameRate->Numerator = 60;
    properties->FrameRate->Denominator = 1;
    properties->Bitrate = 12000000;
    auto descriptor = ref new VideoStreamDescriptor(properties);
    m_source = ref new MediaStreamSource(descriptor);
    TimeSpan latency{};
    latency.Duration = 1000000; // 100 ms live jitter/decode buffer.
    m_source->BufferTime = latency;
    m_startingToken = m_source->Starting +=
        ref new TypedEventHandler<MediaStreamSource^, MediaStreamSourceStartingEventArgs^>(
            this, &H264RtpReceiver::OnStarting);
    m_sampleToken = m_source->SampleRequested +=
        ref new TypedEventHandler<MediaStreamSource^, MediaStreamSourceSampleRequestedEventArgs^>(
            this, &H264RtpReceiver::OnSampleRequested);

    m_output = output;
    m_output->AutoPlay = true;
    m_output->SetMediaStreamSource(m_source);

    m_socket = ref new DatagramSocket();
    m_socket->Control->InboundBufferSizeInBytes = 4 * 1024 * 1024;
    m_messageToken = m_socket->MessageReceived +=
        ref new TypedEventHandler<DatagramSocket^, DatagramSocketMessageReceivedEventArgs^>(
            this, &H264RtpReceiver::OnPacket);
    auto service = port.ToString();
    auto host = bindAddress && !bindAddress->IsEmpty() ? ref new HostName(bindAddress) : nullptr;
    create_task(host ? m_socket->BindEndpointAsync(host, service) :
                       m_socket->BindServiceNameAsync(service)).then([this](task<void> result) {
        try {
            result.get();
            SetStatus(L"Listening for RTP/H.264");
        } catch (Exception^) {
            SetStatus(L"Failed to bind RTP/H.264 UDP socket");
            Stop();
        }
    });
}

void H264RtpReceiver::Stop()
{
    MediaStreamSourceSampleRequestDeferral^ pendingDeferral = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pendingRequest) {
            m_pendingRequest->Sample = nullptr;
            m_pendingRequest = nullptr;
            pendingDeferral = m_pendingDeferral;
            m_pendingDeferral = nullptr;
        }
    }
    if (pendingDeferral) pendingDeferral->Complete();
    if (m_socket) {
        m_socket->MessageReceived -= m_messageToken;
        delete m_socket;
        m_socket = nullptr;
    }
    if (m_source) {
        m_source->Starting -= m_startingToken;
        m_source->SampleRequested -= m_sampleToken;
        m_source->NotifyError(MediaStreamSourceErrorStatus::Other);
        m_source = nullptr;
    }
    if (m_output) {
        m_output->Stop();
        m_output->Source = nullptr;
        m_output = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready.clear();
    m_current.clear();
    m_haveTimestamp = false;
    m_haveSequence = false;
    m_fuActive = false;
    m_status = "Stopped";
}

void H264RtpReceiver::OnStarting(MediaStreamSource^,
                                 MediaStreamSourceStartingEventArgs^ args)
{
    TimeSpan start{};
    start.Duration = 0;
    args->Request->SetActualStartPosition(start);
}

MediaStreamSample^ H264RtpReceiver::TakeSample()
{
    AccessUnit unit;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ready.empty()) return nullptr;
        unit = std::move(m_ready.front());
        m_ready.pop_front();
    }
    return CreateSample(std::move(unit));
}

MediaStreamSample^ H264RtpReceiver::CreateSample(AccessUnit&& unit)
{
    auto bytes = ref new Array<uint8_t>(static_cast<unsigned int>(unit.bytes.size()));
    memcpy(bytes->Data, unit.bytes.data(), unit.bytes.size());
    IBuffer^ buffer = CryptographicBuffer::CreateFromByteArray(bytes);
    TimeSpan time{};
    time.Duration = unit.timestamp;
    auto sample = MediaStreamSample::CreateFromBuffer(buffer, time);
    sample->KeyFrame = unit.keyframe;
    return sample;
}

void H264RtpReceiver::OnSampleRequested(MediaStreamSource^,
                                        MediaStreamSourceSampleRequestedEventArgs^ args)
{
    auto sample = TakeSample();
    if (sample) {
        args->Request->Sample = sample;
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pendingRequest) {
        m_pendingRequest = args->Request;
        m_pendingDeferral = args->Request->GetDeferral();
    }
}

void H264RtpReceiver::CompletePendingRequest()
{
    MediaStreamSourceSampleRequest^ request = nullptr;
    MediaStreamSourceSampleRequestDeferral^ deferral = nullptr;
    AccessUnit unit;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pendingRequest || m_ready.empty()) return;
        request = m_pendingRequest;
        deferral = m_pendingDeferral;
        m_pendingRequest = nullptr;
        m_pendingDeferral = nullptr;
        unit = std::move(m_ready.front());
        m_ready.pop_front();
    }
    request->Sample = CreateSample(std::move(unit));
    deferral->Complete();
}

void H264RtpReceiver::OnPacket(DatagramSocket^,
                               DatagramSocketMessageReceivedEventArgs^ args)
{
    try {
        auto reader = args->GetDataReader();
        unsigned int size = reader->UnconsumedBufferLength;
        if (size < 12 || size > 65535) return;
        auto data = ref new Array<uint8_t>(size);
        reader->ReadBytes(data);
        ParseRtp(data->Data, data->Length);
    } catch (Exception^) {
        SetStatus(L"Invalid RTP/H.264 packet");
    }
}

bool H264RtpReceiver::AppendNal(const uint8_t* data, size_t size)
{
    if (!data || !size || m_current.size() + size + 4 > 16 * 1024 * 1024) {
        return false;
    }
    static const uint8_t startCode[] = { 0, 0, 0, 1 };
    m_current.insert(m_current.end(), startCode, startCode + 4);
    m_current.insert(m_current.end(), data, data + size);
    uint8_t type = data[0] & 0x1f;
    m_keyframe = m_keyframe || type == 5;
    return true;
}

void H264RtpReceiver::FinishAccessUnit(uint32_t timestamp)
{
    if (m_current.empty()) return;
    if (!m_haveTimestamp) {
        m_baseTimestamp = timestamp;
        m_haveTimestamp = true;
    }
    uint32_t delta = timestamp - m_baseTimestamp;
    AccessUnit unit;
    unit.bytes.swap(m_current);
    unit.timestamp = static_cast<int64_t>(delta) * 10000000 / 90000;
    unit.keyframe = m_keyframe;
    m_keyframe = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (m_ready.size() >= 6) m_ready.pop_front();
        m_ready.push_back(std::move(unit));
        m_status = "Receiving RTP/H.264";
    }
    CompletePendingRequest();
}

void H264RtpReceiver::ParseRtp(const uint8_t* packet, size_t size)
{
    if ((packet[0] >> 6) != 2) return;
    size_t offset = 12 + (packet[0] & 0x0f) * 4;
    if (packet[0] & 0x10) {
        if (offset + 4 > size) return;
        size_t extensionWords = (packet[offset + 2] << 8) | packet[offset + 3];
        offset += 4 + extensionWords * 4;
    }
    size_t end = size;
    if (packet[0] & 0x20) {
        uint8_t padding = packet[size - 1];
        if (!padding || padding > end - offset) return;
        end -= padding;
    }
    if (offset >= end) return;
    uint16_t sequence = (packet[2] << 8) | packet[3];
    uint32_t timestamp = (static_cast<uint32_t>(packet[4]) << 24) |
                         (static_cast<uint32_t>(packet[5]) << 16) |
                         (static_cast<uint32_t>(packet[6]) << 8) | packet[7];
    if (m_haveSequence && sequence != m_expectedSequence) {
        m_current.clear();
        m_fuActive = false;
    }
    m_expectedSequence = sequence + 1;
    m_haveSequence = true;
    if (!m_current.empty() && timestamp != m_currentTimestamp) {
        m_current.clear();
        m_fuActive = false;
        m_keyframe = false;
    }
    m_currentTimestamp = timestamp;

    const uint8_t* payload = packet + offset;
    size_t payloadSize = end - offset;
    uint8_t type = payload[0] & 0x1f;
    if (type >= 1 && type <= 23) {
        AppendNal(payload, payloadSize);
    } else if (type == 24) { // STAP-A
        size_t pos = 1;
        while (pos + 2 <= payloadSize) {
            size_t nalSize = (payload[pos] << 8) | payload[pos + 1];
            pos += 2;
            if (!nalSize || pos + nalSize > payloadSize) {
                m_current.clear();
                return;
            }
            AppendNal(payload + pos, nalSize);
            pos += nalSize;
        }
    } else if (type == 28 && payloadSize >= 2) { // FU-A
        bool start = (payload[1] & 0x80) != 0;
        bool finish = (payload[1] & 0x40) != 0;
        uint8_t nalHeader = (payload[0] & 0xe0) | (payload[1] & 0x1f);
        if (start) {
            m_fuActive = true;
            AppendNal(&nalHeader, 1);
        } else if (!m_fuActive) {
            return;
        }
        if (payloadSize > 2)
            m_current.insert(m_current.end(), payload + 2, payload + payloadSize);
        if (finish) m_fuActive = false;
    } else {
        return;
    }
    if ((packet[1] & 0x80) && !m_fuActive) FinishAccessUnit(timestamp);
}
}
