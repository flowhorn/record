#include "aac_encoder.h"

#include <filesystem>
#include <iostream>

namespace record_windows {

namespace {

CHANNEL_MODE ChannelModeFromCount(int channels) {
    switch (channels) {
    case 1:
        return MODE_1;
    case 2:
        return MODE_2;
    default:
        return MODE_INVALID;
    }
}

int DefaultBitrateFor(int channels) {
    return channels == 1 ? 64000 : 128000;
}

} // namespace

AacEncoder::AacEncoder() = default;

AacEncoder::~AacEncoder() {
    Finalize();
}

bool AacEncoder::Initialize(const std::wstring& path, int sampleRate, int channels, int bitrate) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
        FlushEncoder();
        if (m_encoder != nullptr) {
            aacEncClose(&m_encoder);
        }
        if (m_outputFile.is_open()) {
            m_outputFile.close();
        }
        m_outputBuffer.clear();
        m_initialized = false;
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrate = bitrate > 0 ? bitrate : DefaultBitrateFor(channels);
    m_frameSize = 1024;

    const CHANNEL_MODE channelMode = ChannelModeFromCount(m_channels);
    if (channelMode == MODE_INVALID) {
        std::cerr << "AacEncoder: Unsupported channel count: " << m_channels << std::endl;
        return false;
    }

    m_outputFile.open(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!m_outputFile.is_open()) {
        std::cerr << "AacEncoder: Failed to open output file." << std::endl;
        return false;
    }

    if (!ConfigureEncoder()) {
        if (m_outputFile.is_open()) {
            m_outputFile.close();
        }
        return false;
    }

    m_initialized = true;
    return true;
}

bool AacEncoder::ConfigureEncoder() {
    AACENC_ERROR err = aacEncOpen(&m_encoder, 0, static_cast<UINT>(m_channels));
    if (err != AACENC_OK) {
        std::cerr << "AacEncoder: aacEncOpen failed: " << err << std::endl;
        return false;
    }

    const CHANNEL_MODE channelMode = ChannelModeFromCount(m_channels);
    const struct {
        AACENC_PARAM param;
        UINT value;
        const char* name;
    } params[] = {
        {AACENC_AOT, static_cast<UINT>(AOT_AAC_LC), "AACENC_AOT"},
        {AACENC_SAMPLERATE, static_cast<UINT>(m_sampleRate), "AACENC_SAMPLERATE"},
        {AACENC_CHANNELMODE, static_cast<UINT>(channelMode), "AACENC_CHANNELMODE"},
        {AACENC_CHANNELORDER, 1u, "AACENC_CHANNELORDER"},
        {AACENC_BITRATE, static_cast<UINT>(m_bitrate), "AACENC_BITRATE"},
        {AACENC_TRANSMUX, static_cast<UINT>(TT_MP4_ADTS), "AACENC_TRANSMUX"},
        {AACENC_AFTERBURNER, 1u, "AACENC_AFTERBURNER"},
    };

    for (const auto& p : params) {
        err = aacEncoder_SetParam(m_encoder, p.param, p.value);
        if (err != AACENC_OK) {
            std::cerr << "AacEncoder: " << p.name << " failed: " << err << std::endl;
            aacEncClose(&m_encoder);
            return false;
        }
    }

    err = aacEncEncode(m_encoder, nullptr, nullptr, nullptr, nullptr);
    if (err != AACENC_OK) {
        std::cerr << "AacEncoder: Initial aacEncEncode failed: " << err << std::endl;
        aacEncClose(&m_encoder);
        return false;
    }

    AACENC_InfoStruct info = {};
    err = aacEncInfo(m_encoder, &info);
    if (err != AACENC_OK) {
        std::cerr << "AacEncoder: aacEncInfo failed: " << err << std::endl;
        aacEncClose(&m_encoder);
        return false;
    }

    if (info.frameLength > 0) {
        m_frameSize = static_cast<int>(info.frameLength);
    }

    m_outputBuffer.resize(8192);
    return true;
}

bool AacEncoder::EncodeFrame(const int16_t* pcm, int frameSize) {
    if (!pcm || frameSize <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || m_encoder == nullptr || !m_outputFile.is_open()) {
        return false;
    }

    const int numInSamples = frameSize * m_channels;
    return EncodeInternal(reinterpret_cast<const INT_PCM*>(pcm), numInSamples);
}

bool AacEncoder::EncodeInternal(const INT_PCM* pcm, int numInSamples) {
    void* inBuffer = const_cast<INT_PCM*>(pcm);
    INT inIdentifier = IN_AUDIO_DATA;
    INT inElemSize = static_cast<INT>(sizeof(INT_PCM));
    INT inSize = numInSamples * inElemSize;
    AACENC_BufDesc inDesc = {};
    inDesc.numBufs = 1;
    inDesc.bufs = &inBuffer;
    inDesc.bufferIdentifiers = &inIdentifier;
    inDesc.bufSizes = &inSize;
    inDesc.bufElSizes = &inElemSize;

    void* outBuffer = m_outputBuffer.data();
    INT outIdentifier = OUT_BITSTREAM_DATA;
    INT outElemSize = 1;
    INT outSize = static_cast<INT>(m_outputBuffer.size());
    AACENC_BufDesc outDesc = {};
    outDesc.numBufs = 1;
    outDesc.bufs = &outBuffer;
    outDesc.bufferIdentifiers = &outIdentifier;
    outDesc.bufSizes = &outSize;
    outDesc.bufElSizes = &outElemSize;

    AACENC_InArgs inArgs = {};
    inArgs.numInSamples = numInSamples;
    AACENC_OutArgs outArgs = {};

    const AACENC_ERROR err = aacEncEncode(m_encoder, &inDesc, &outDesc, &inArgs, &outArgs);
    if (err != AACENC_OK) {
        std::cerr << "AacEncoder: aacEncEncode failed: " << err << std::endl;
        return false;
    }

    if (outArgs.numOutBytes > 0) {
        m_outputFile.write(reinterpret_cast<const char*>(m_outputBuffer.data()), outArgs.numOutBytes);
        if (!m_outputFile.good()) {
            std::cerr << "AacEncoder: Failed writing encoded data." << std::endl;
            return false;
        }
    }

    return true;
}

void AacEncoder::FlushEncoder() {
    if (m_encoder == nullptr || !m_outputFile.is_open()) {
        return;
    }

    while (true) {
        void* outBuffer = m_outputBuffer.data();
        INT outIdentifier = OUT_BITSTREAM_DATA;
        INT outElemSize = 1;
        INT outSize = static_cast<INT>(m_outputBuffer.size());
        AACENC_BufDesc outDesc = {};
        outDesc.numBufs = 1;
        outDesc.bufs = &outBuffer;
        outDesc.bufferIdentifiers = &outIdentifier;
        outDesc.bufSizes = &outSize;
        outDesc.bufElSizes = &outElemSize;

        AACENC_InArgs inArgs = {};
        inArgs.numInSamples = -1;
        AACENC_OutArgs outArgs = {};

        const AACENC_ERROR err = aacEncEncode(m_encoder, nullptr, &outDesc, &inArgs, &outArgs);
        if (err != AACENC_OK && err != AACENC_ENCODE_EOF) {
            std::cerr << "AacEncoder: Flush failed: " << err << std::endl;
            break;
        }

        if (outArgs.numOutBytes > 0) {
            m_outputFile.write(reinterpret_cast<const char*>(m_outputBuffer.data()), outArgs.numOutBytes);
            if (!m_outputFile.good()) {
                std::cerr << "AacEncoder: Failed writing flushed data." << std::endl;
                break;
            }
        }

        if (err == AACENC_ENCODE_EOF || outArgs.numOutBytes == 0) {
            break;
        }
    }
}

void AacEncoder::Finalize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized) {
        return;
    }

    FlushEncoder();

    if (m_encoder != nullptr) {
        aacEncClose(&m_encoder);
    }

    if (m_outputFile.is_open()) {
        m_outputFile.flush();
        m_outputFile.close();
    }

    m_outputBuffer.clear();
    m_initialized = false;
}

} // namespace record_windows
