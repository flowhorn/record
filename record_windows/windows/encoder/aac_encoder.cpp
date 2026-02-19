#include "aac_encoder.h"
#include "aac_container_negotiation.h"
#include <iostream>

template <class T> void SafeRelease(T **ppT)
{
    if (*ppT)
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

namespace record_windows {

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
    const auto bitrateCandidates = BuildAacBitrateCandidates(m_bitrate, m_sampleRate, m_channels);
    const std::vector<UINT32> profileLevelCandidates = {0x29, 0};
    HRESULT lastHr = E_FAIL;

    auto tryConfigure = [&](bool useDefaultProfile, UINT32 profileLevel, bool useDefaultBitrate, UINT32 avgBitrate) -> HRESULT {
        IMFSinkWriter* pSinkWriter = NULL;
        IMFMediaType* pMediaTypeOut = NULL;
        IMFMediaType* pMediaTypeIn = NULL;
        DWORD streamIndex = 0;

        HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), NULL, NULL, &pSinkWriter);

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
            hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
            if (FAILED(hr)) std::cerr << "SetUINT32 Channels Out failed: " << hr << std::endl;
        }

        if (SUCCEEDED(hr)) {
            hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
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
            m_streamIndex = streamIndex;
            m_pSinkWriter = pSinkWriter;
            m_pSinkWriter->AddRef();
        }

        SafeRelease(&pSinkWriter);
        SafeRelease(&pMediaTypeOut);
        SafeRelease(&pMediaTypeIn);

        return hr;
    };

    for (UINT32 profileLevel : profileLevelCandidates) {
        const bool useDefaultProfile = (profileLevel == 0);
        for (uint32_t candidateBitrate : bitrateCandidates) {
            const UINT32 avgBitrate = static_cast<UINT32>(candidateBitrate);
            HRESULT hr = tryConfigure(useDefaultProfile, profileLevel, false, avgBitrate);
            if (SUCCEEDED(hr)) {
                if (useDefaultProfile) {
                    std::cout << "Record: AAC profile-level selected by Media Foundation defaults." << std::endl;
                } else {
                    std::cout << "Record: AAC profile-level set to 0x" << std::hex
                              << profileLevel << std::dec << "." << std::endl;
                }
                std::cout << "Record: AAC bitrate selected " << avgBitrate << " bps." << std::endl;
                return hr;
            }

            lastHr = hr;
            std::cerr << "Record: AAC attempt failed for profile "
                      << (useDefaultProfile ? "default" : "explicit")
                      << " and bitrate " << avgBitrate
                      << " bps: " << hr << std::endl;
        }
    }

    HRESULT hr = tryConfigure(true, 0, true, 0);
    if (SUCCEEDED(hr)) {
        std::cout << "Record: AAC profile-level and bitrate selected by Media Foundation defaults." << std::endl;
        return hr;
    }

    lastHr = hr;
    std::cerr << "ConfigSinkWriter failed at step Setup: " << lastHr << std::endl;
    return lastHr;
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
