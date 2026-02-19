#include "aac_encoder.h"
#include "aac_container_negotiation.h"
#include <iostream>
#include <sstream>

template <class T> void SafeRelease(T **ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

namespace record_windows {

struct NegotiatedAacOutputInfo {
    UINT32 sampleRate = 0;
    UINT32 channels = 0;
    bool hasProfileLevel = false;
    UINT32 profileLevel = 0;
    bool hasAvgBitrate = false;
    UINT32 avgBitrate = 0;
};

static HRESULT GetNegotiatedAacOutputInfo(
    IMFSinkWriter* pSinkWriter,
    DWORD streamIndex,
    NegotiatedAacOutputInfo* outputInfo) {
    if (!pSinkWriter || !outputInfo) {
        return E_POINTER;
    }

    IMFMediaSink* pMediaSink = NULL;
    IMFStreamSink* pStreamSink = NULL;
    IMFMediaTypeHandler* pTypeHandler = NULL;
    IMFMediaType* pCurrentType = NULL;

    HRESULT hr = pSinkWriter->GetServiceForStream(
        static_cast<DWORD>(MF_SINK_WRITER_MEDIASINK),
        GUID_NULL,
        IID_PPV_ARGS(&pMediaSink));

    if (SUCCEEDED(hr)) {
        hr = pMediaSink->GetStreamSinkByIndex(streamIndex, &pStreamSink);
    }
    if (SUCCEEDED(hr)) {
        hr = pStreamSink->GetMediaTypeHandler(&pTypeHandler);
    }
    if (SUCCEEDED(hr)) {
        hr = pTypeHandler->GetCurrentMediaType(&pCurrentType);
    }
    if (SUCCEEDED(hr)) {
        hr = pCurrentType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &outputInfo->sampleRate);
    }
    if (SUCCEEDED(hr)) {
        hr = pCurrentType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outputInfo->channels);
    }
    if (SUCCEEDED(hr)) {
        UINT32 v = 0;
        if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, &v))) {
            outputInfo->hasProfileLevel = true;
            outputInfo->profileLevel = v;
        }
    }
    if (SUCCEEDED(hr)) {
        UINT32 v = 0;
        if (SUCCEEDED(pCurrentType->GetUINT32(MF_MT_AVG_BITRATE, &v))) {
            outputInfo->hasAvgBitrate = true;
            outputInfo->avgBitrate = v;
        }
    }

    SafeRelease(&pCurrentType);
    SafeRelease(&pTypeHandler);
    SafeRelease(&pStreamSink);
    SafeRelease(&pMediaSink);

    return hr;
}

AacEncoder::AacEncoder() {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "MFStartup failed: " << hr << std::endl;
    }
}

AacEncoder::~AacEncoder() {
    Finalize();
    MFShutdown();
}

bool AacEncoder::Initialize(const std::wstring& path, int sampleRate, int channels, int bitrate) {
    if (m_initialized) {
        Finalize();
    }
    
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrate = bitrate;
    m_duration = 0;
    
    HRESULT hr = ConfigSinkWriter(path);
    if (SUCCEEDED(hr)) {
        m_initialized = true;
        return true;
    }
    
    return false;
}

HRESULT AacEncoder::ConfigSinkWriter(const std::wstring& path) {
    const std::vector<UINT32> profileLevelCandidates = {0x29, 0};
    const auto outputRateCandidates = BuildAacSampleRateCandidates(m_sampleRate);
    const auto outputChannelCandidates = BuildAacChannelCandidates(m_channels);
    HRESULT lastHr = E_FAIL;
    NegotiatedAacOutputInfo negotiatedOutputInfo;

    auto formatUInt32List = [](const std::vector<UINT32>& values) -> std::string {
        std::ostringstream oss;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << values[i];
        }
        return oss.str();
    };

    auto computeAutoBitrateTarget = [](UINT32 sampleRate, UINT32 channels) -> UINT32 {
        if (channels <= 1) {
            if (sampleRate <= 12000) return 24000;
            if (sampleRate <= 16000) return 32000;
            if (sampleRate <= 24000) return 40000;
            if (sampleRate <= 32000) return 48000;
            return 64000;
        }
        if (sampleRate <= 16000) return 32000;
        if (sampleRate <= 24000) return 48000;
        if (sampleRate <= 32000) return 64000;
        return 96000;
    };

    auto computeAutoBitrateFloor = [](UINT32 sampleRate, UINT32 channels) -> UINT32 {
        if (channels <= 1) {
            if (sampleRate <= 12000) return 12000;
            if (sampleRate <= 16000) return 16000;
            if (sampleRate <= 24000) return 24000;
            if (sampleRate <= 32000) return 32000;
            return 48000;
        }
        if (sampleRate <= 16000) return 24000;
        if (sampleRate <= 24000) return 32000;
        if (sampleRate <= 32000) return 48000;
        return 64000;
    };

    struct BitrateAttempt {
        bool useDefaultBitrate;
        UINT32 avgBitrate;
    };

    struct OutputFormatAttempt {
        UINT32 sampleRate;
        UINT32 channels;
        bool exactRequested;
    };

    std::vector<OutputFormatAttempt> outputFormatAttempts;
    outputFormatAttempts.reserve(outputRateCandidates.size() * outputChannelCandidates.size() + 1);
    outputFormatAttempts.push_back({
        static_cast<UINT32>(m_sampleRate),
        static_cast<UINT32>(m_channels),
        true
    });
    for (UINT32 channels : outputChannelCandidates) {
        for (UINT32 sampleRate : outputRateCandidates) {
            if (channels == static_cast<UINT32>(m_channels) &&
                sampleRate == static_cast<UINT32>(m_sampleRate)) {
                continue;
            }
            outputFormatAttempts.push_back({sampleRate, channels, false});
        }
    }

    std::cout << "Record: AAC negotiation request " << m_sampleRate << " Hz, "
              << m_channels << " channel(s), bitrate "
              << (m_bitrate > 0 ? std::to_string(m_bitrate) : std::string("auto"))
              << "." << std::endl;
    std::cout << "Record: AAC output rate candidates: "
              << formatUInt32List(outputRateCandidates) << std::endl;
    std::cout << "Record: AAC output channel candidates: "
              << formatUInt32List(outputChannelCandidates) << std::endl;

    auto buildBitrateAttempts = [&](UINT32 outputSampleRate, UINT32 outputChannels) {
        std::vector<BitrateAttempt> attempts;
        int requestedBitrate = m_bitrate;
        if (requestedBitrate <= 0) {
            requestedBitrate = static_cast<int>(
                computeAutoBitrateTarget(outputSampleRate, outputChannels));
        }
        const auto bitrateCandidates = BuildAacBitrateCandidates(
            requestedBitrate, (int)outputSampleRate, (int)outputChannels);

        attempts.reserve(bitrateCandidates.size() + 1);
        for (uint32_t bitrate : bitrateCandidates) {
            attempts.push_back({false, static_cast<UINT32>(bitrate)});
        }
        // Always keep encoder default as final escape hatch.
        attempts.push_back({true, 0});

        return attempts;
    };

    auto tryConfigure = [&](UINT32 outputSampleRate,
                            UINT32 outputChannels,
                            bool useDefaultProfile,
                            UINT32 profileLevel,
                            bool useDefaultBitrate,
                            UINT32 avgBitrate,
                            bool disableConverters,
                            bool requireExactOutput,
                            bool requireRequestedChannels,
                            bool allowVeryLowAutoBitrate) -> HRESULT {
        IMFSinkWriter* pSinkWriter = NULL;
        IMFAttributes* pSinkWriterAttributes = NULL;
        IMFMediaType* pMediaTypeOut = NULL;
        IMFMediaType* pMediaTypeIn = NULL;
        DWORD streamIndex = 0;

        HRESULT hr = MFCreateAttributes(&pSinkWriterAttributes, 4);
        if (SUCCEEDED(hr)) {
            HRESULT attrHr = pSinkWriterAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            if (FAILED(attrHr)) {
                std::cerr << "SetUINT32 MF_LOW_LATENCY failed (ignoring): " << attrHr << std::endl;
            }
            attrHr = pSinkWriterAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            if (FAILED(attrHr)) {
                std::cerr << "SetUINT32 MF_SINK_WRITER_DISABLE_THROTTLING failed (ignoring): " << attrHr << std::endl;
            }
            if (disableConverters) {
                attrHr = pSinkWriterAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
                if (FAILED(attrHr)) {
                    std::cerr << "SetUINT32 MF_READWRITE_DISABLE_CONVERTERS failed (ignoring): " << attrHr << std::endl;
                }
            }
        }

        if (SUCCEEDED(hr)) {
            hr = MFCreateSinkWriterFromURL(path.c_str(), NULL, pSinkWriterAttributes, &pSinkWriter);
        }
        if (FAILED(hr)) {
            hr = MFCreateSinkWriterFromURL(path.c_str(), NULL, NULL, &pSinkWriter);
        }

        if (SUCCEEDED(hr)) {
            hr = MFCreateMediaType(&pMediaTypeOut);
            if (FAILED(hr)) std::cerr << "MFCreateMediaType Out failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (FAILED(hr)) std::cerr << "SetGUID MF_MT_MAJOR_TYPE Out failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            if (FAILED(hr)) std::cerr << "SetGUID MFAudioFormat_AAC failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, outputChannels);
            if (FAILED(hr)) std::cerr << "SetUINT32 Channels Out failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, outputSampleRate);
            if (FAILED(hr)) std::cerr << "SetUINT32 SampleRate Out failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (FAILED(hr)) std::cerr << "SetUINT32 BitsPerSample Out failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            if (FAILED(hr)) std::cerr << "SetUINT32 PayloadType Out failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            if (useDefaultProfile) {
                pMediaTypeOut->DeleteItem(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION);
            } else {
                hr = pMediaTypeOut->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, profileLevel);
                if (FAILED(hr)) std::cerr << "SetUINT32 AACProfile Out failed: " << hr << std::endl;
            }
        }
        if (SUCCEEDED(hr)) {
            if (useDefaultBitrate) {
                pMediaTypeOut->DeleteItem(MF_MT_AUDIO_AVG_BYTES_PER_SECOND);
                pMediaTypeOut->DeleteItem(MF_MT_AVG_BITRATE);
            } else {
                const UINT32 bytesPerSecond = avgBitrate / 8;
                hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
                if (FAILED(hr)) std::cerr << "SetUINT32 Bitrate Out failed: " << hr << std::endl;
                if (SUCCEEDED(hr)) {
                    hr = pMediaTypeOut->SetUINT32(MF_MT_AVG_BITRATE, avgBitrate);
                    if (FAILED(hr)) std::cerr << "SetUINT32 AvgBitrate Out failed: " << hr << std::endl;
                }
            }
        }
        if (SUCCEEDED(hr)) {
            hr = pSinkWriter->AddStream(pMediaTypeOut, &streamIndex);
            if (FAILED(hr)) std::cerr << "AddStream failed: " << hr << std::endl;
        }

        // Input PCM is always the recorder target format; MF may convert if allowed.
        if (SUCCEEDED(hr)) {
            hr = MFCreateMediaType(&pMediaTypeIn);
            if (FAILED(hr)) std::cerr << "MFCreateMediaType In failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (FAILED(hr)) std::cerr << "SetGUID MF_MT_MAJOR_TYPE In failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            if (FAILED(hr)) std::cerr << "SetGUID MFAudioFormat_PCM failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (FAILED(hr)) std::cerr << "SetUINT32 BitsPerSample In failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
            if (FAILED(hr)) std::cerr << "SetUINT32 SampleRate In failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
            if (FAILED(hr)) std::cerr << "SetUINT32 Channels In failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            if (FAILED(hr)) std::cerr << "SetUINT32 AllSamplesIndependent In failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            UINT32 blockAlign = m_channels * 2;
            hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
            if (FAILED(hr)) std::cerr << "SetUINT32 BlockAlignment In failed: " << hr << std::endl;
            if (SUCCEEDED(hr)) {
                hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_sampleRate * blockAlign);
                if (FAILED(hr)) std::cerr << "SetUINT32 AvgBytesPerSecond In failed: " << hr << std::endl;
            }
        }
        if (SUCCEEDED(hr) && m_channels > 0 && m_channels <= 2) {
            DWORD channelMask = (m_channels == 1) ? 0x4 : 0x3;
            HRESULT channelMaskHr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, channelMask);
            if (FAILED(channelMaskHr)) {
                std::cerr << "SetUINT32 ChannelMask In failed (ignoring): " << channelMaskHr << std::endl;
            }
        }
        if (SUCCEEDED(hr)) {
            hr = pSinkWriter->SetInputMediaType(streamIndex, pMediaTypeIn, NULL);
            if (FAILED(hr)) std::cerr << "SetInputMediaType failed (Likely format mismatch): " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            hr = pSinkWriter->BeginWriting();
            if (FAILED(hr)) std::cerr << "BeginWriting failed: " << hr << std::endl;
        }
        if (SUCCEEDED(hr)) {
            NegotiatedAacOutputInfo info;
            HRESULT negotiatedHr = GetNegotiatedAacOutputInfo(pSinkWriter, streamIndex, &info);
            if (SUCCEEDED(negotiatedHr)) {
                negotiatedOutputInfo = info;
                if (requireExactOutput &&
                    (info.sampleRate != static_cast<UINT32>(m_sampleRate) ||
                     info.channels != static_cast<UINT32>(m_channels))) {
                    std::cerr << "Record: AAC negotiated output " << info.sampleRate
                              << " Hz, " << info.channels
                              << " channel(s) differs from requested "
                              << m_sampleRate << " Hz, " << m_channels
                              << " channel(s). Retrying exact mode." << std::endl;
                    hr = MF_E_INVALIDMEDIATYPE;
                } else if (!requireExactOutput &&
                           requireRequestedChannels &&
                           info.channels != static_cast<UINT32>(m_channels)) {
                    std::cerr << "Record: AAC negotiated output channels " << info.channels
                              << " differs from requested " << m_channels
                              << " in channel-preserving mode. Retrying." << std::endl;
                    hr = MF_E_INVALIDMEDIATYPE;
                } else if (m_bitrate <= 0 &&
                           info.hasAvgBitrate &&
                           !allowVeryLowAutoBitrate) {
                    const UINT32 minAutoBitrate = computeAutoBitrateFloor(
                        info.sampleRate, info.channels);
                    if (info.avgBitrate < minAutoBitrate) {
                        std::cerr << "Record: AAC negotiated auto bitrate " << info.avgBitrate
                                  << " bps is below preferred minimum " << minAutoBitrate
                                  << " bps for " << info.sampleRate << " Hz/"
                                  << info.channels << "ch. Retrying higher-quality options."
                                  << std::endl;
                        hr = MF_E_INVALIDMEDIATYPE;
                    }
                }
            } else {
                std::cerr << "Record: Failed to query negotiated AAC output type: "
                          << negotiatedHr << std::endl;
                if (requireExactOutput || requireRequestedChannels) {
                    std::cerr << "Record: Rejecting attempt because negotiated output "
                              << "cannot be verified in strict/channel-preserving mode."
                              << std::endl;
                    hr = MF_E_INVALIDMEDIATYPE;
                }
            }
        }
        if (SUCCEEDED(hr)) {
            m_streamIndex = streamIndex;
            m_pSinkWriter = pSinkWriter;
            m_pSinkWriter->AddRef();
        }

        SafeRelease(&pSinkWriter);
        SafeRelease(&pSinkWriterAttributes);
        SafeRelease(&pMediaTypeOut);
        SafeRelease(&pMediaTypeIn);
        return hr;
    };

    auto runAttempts = [&](const char* passName,
                           bool disableConverters,
                           bool requireExactOutput,
                           bool requireRequestedChannels,
                           bool allowVeryLowAutoBitrate) -> HRESULT {
        std::cout << "Record: AAC negotiation pass '" << passName << "' started." << std::endl;
        for (const auto& formatAttempt : outputFormatAttempts) {
            if (requireExactOutput && !formatAttempt.exactRequested) {
                continue;
            }

            const auto bitrateAttempts = buildBitrateAttempts(
                formatAttempt.sampleRate, formatAttempt.channels);
            if (m_bitrate <= 0) {
                const UINT32 autoTarget = computeAutoBitrateTarget(
                    formatAttempt.sampleRate, formatAttempt.channels);
                const UINT32 autoFloor = computeAutoBitrateFloor(
                    formatAttempt.sampleRate, formatAttempt.channels);
                std::ostringstream bitrateOss;
                for (size_t i = 0; i < bitrateAttempts.size(); ++i) {
                    if (i > 0) bitrateOss << ", ";
                    if (bitrateAttempts[i].useDefaultBitrate) {
                        bitrateOss << "default";
                    } else {
                        bitrateOss << bitrateAttempts[i].avgBitrate;
                    }
                }
                std::cout << "Record: AAC auto bitrate for output "
                          << formatAttempt.sampleRate << " Hz/" << formatAttempt.channels
                          << "ch -> target " << autoTarget
                          << " bps, floor " << autoFloor
                          << " bps, candidates [" << bitrateOss.str() << "]."
                          << std::endl;
            }

            for (UINT32 profileLevel : profileLevelCandidates) {
                const bool useDefaultProfile = (profileLevel == 0);
                for (const auto& bitrateAttempt : bitrateAttempts) {
                    HRESULT hr = tryConfigure(
                        formatAttempt.sampleRate,
                        formatAttempt.channels,
                        useDefaultProfile,
                        profileLevel,
                        bitrateAttempt.useDefaultBitrate,
                        bitrateAttempt.avgBitrate,
                        disableConverters,
                        requireExactOutput,
                        requireRequestedChannels,
                        allowVeryLowAutoBitrate);

                    if (SUCCEEDED(hr)) {
                        if (useDefaultProfile) {
                            std::cout << "Record: AAC profile-level selected by Media Foundation defaults." << std::endl;
                        } else {
                            std::cout << "Record: AAC profile-level set to 0x"
                                      << std::hex << profileLevel << std::dec << "." << std::endl;
                        }

                        if (bitrateAttempt.useDefaultBitrate) {
                            std::cout << "Record: AAC bitrate selected by Media Foundation defaults." << std::endl;
                        } else {
                            std::cout << "Record: AAC bitrate selected " << bitrateAttempt.avgBitrate << " bps." << std::endl;
                        }

                        std::cout << "Record: AAC output request " << formatAttempt.sampleRate
                                  << " Hz, " << formatAttempt.channels
                                  << " channel(s); negotiated " << negotiatedOutputInfo.sampleRate
                                  << " Hz, " << negotiatedOutputInfo.channels << " channel(s)." << std::endl;
                        if (negotiatedOutputInfo.hasProfileLevel) {
                            std::cout << "Record: AAC negotiated profile-level 0x"
                                      << std::hex << negotiatedOutputInfo.profileLevel << std::dec << "." << std::endl;
                        } else {
                            std::cout << "Record: AAC negotiated profile-level not provided by encoder." << std::endl;
                        }
                        if (negotiatedOutputInfo.hasAvgBitrate) {
                            std::cout << "Record: AAC negotiated bitrate " << negotiatedOutputInfo.avgBitrate << " bps." << std::endl;
                        }
                        return hr;
                    }

                    lastHr = hr;
                    std::cerr << "Record: AAC attempt failed for output "
                              << formatAttempt.sampleRate << " Hz/" << formatAttempt.channels
                              << "ch, profile " << (useDefaultProfile ? "default" : "explicit")
                              << ", bitrate "
                              << (bitrateAttempt.useDefaultBitrate ? std::string("default")
                                                                   : std::to_string(bitrateAttempt.avgBitrate) + " bps")
                              << ": " << hr << std::endl;
                }
            }
        }

        return lastHr;
    };

    HRESULT hr = runAttempts("exact", true, true, true, false);
    if (SUCCEEDED(hr)) {
        return hr;
    }

    std::cerr << "Record: Exact AAC output not supported by this encoder/device combo. "
              << "Falling back to channel-preserving AAC negotiation." << std::endl;
    hr = runAttempts("channel-preserving", true, false, true, false);
    if (SUCCEEDED(hr)) {
        return hr;
    }

    std::cerr << "Record: Channel-preserving AAC negotiation not supported. "
              << "Falling back to fully compatible AAC negotiation." << std::endl;
    hr = runAttempts("compatible", true, false, false, false);
    if (SUCCEEDED(hr)) {
        return hr;
    }

    std::cerr << "Record: Explicit AAC output types unavailable. "
              << "Trying converter-enabled compatibility fallback." << std::endl;
    hr = runAttempts("compatible-converters", false, false, false, false);
    if (SUCCEEDED(hr)) {
        return hr;
    }

    if (m_bitrate <= 0) {
        std::cerr << "Record: Preferred auto-bitrate floor could not be met. "
                  << "Allowing very low auto bitrate as last resort." << std::endl;
        hr = runAttempts("compatible-low-bitrate", false, false, false, true);
        if (SUCCEEDED(hr)) {
            return hr;
        }
    }

    std::cerr << "ConfigSinkWriter failed at step Setup: " << hr << std::endl;
    return hr;
}

bool AacEncoder::EncodeFrame(const int16_t* pcm, int frameSize) {
    if (!m_initialized || !m_pSinkWriter) return false;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    IMFSample* pSample = NULL;
    IMFMediaBuffer* pBuffer = NULL;
    
    const DWORD cbBuffer = frameSize * m_channels * sizeof(int16_t);
    BYTE* pData = NULL;
    
    // Create a new memory buffer
    HRESULT hr = MFCreateMemoryBuffer(cbBuffer, &pBuffer);
    
    // Lock the buffer and copy the data
    if (SUCCEEDED(hr)) {
        hr = pBuffer->Lock(&pData, NULL, NULL);
    }
    
    if (SUCCEEDED(hr)) {
        memcpy(pData, pcm, cbBuffer);
        hr = pBuffer->Unlock();
    }
    
    if (SUCCEEDED(hr)) {
        hr = pBuffer->SetCurrentLength(cbBuffer);
    }
    
    // Create a new sample
    if (SUCCEEDED(hr)) {
        hr = MFCreateSample(&pSample);
    }
    
    if (SUCCEEDED(hr)) {
        hr = pSample->AddBuffer(pBuffer);
    }
    
    // Set timestamp and duration
    if (SUCCEEDED(hr)) {
        // duration in 100-nanoseconds units
        // 1 second = 10,000,000 units
        // duration = frameSize / sampleRate * 10,000,000
        LONGLONG duration = (LONGLONG)frameSize * 10000000 / m_sampleRate;
        
        hr = pSample->SetSampleTime(m_duration);
        if (SUCCEEDED(hr)) {
            hr = pSample->SetSampleDuration(duration);
        }
        
        m_duration += duration;
    }
    
    if (SUCCEEDED(hr)) {
        hr = m_pSinkWriter->WriteSample(m_streamIndex, pSample);
    } else {
        std::cerr << "EncodeFrame setup failed: " << hr << std::endl;
    }
    
    if (FAILED(hr)) {
        std::cerr << "WriteSample failed: " << hr << std::endl;
    }
    
    SafeRelease(&pSample);
    SafeRelease(&pBuffer);
    
    return SUCCEEDED(hr);
}

void AacEncoder::Finalize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_pSinkWriter) {
        m_pSinkWriter->Finalize();
        SafeRelease(&m_pSinkWriter);
    }
    m_initialized = false;
}

} // namespace record_windows
