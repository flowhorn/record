#include "aac_encoder.h"
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
    IMFSinkWriter* pSinkWriter = NULL;
    IMFMediaType* pMediaTypeOut = NULL;
    IMFMediaType* pMediaTypeIn = NULL;
    
    // Create the sink writer
    // Note: This relies on the file extension to select the container (e.g. .m4a)
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), NULL, NULL, &pSinkWriter);
    
    // Configure output media type (AAC)
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
        hr = pMediaTypeOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_bitrate / 8);
        if (FAILED(hr)) std::cerr << "SetUINT32 Bitrate Out failed: " << hr << std::endl;
    }
    
    if (SUCCEEDED(hr)) {
        hr = pSinkWriter->AddStream(pMediaTypeOut, &m_streamIndex);
        if (FAILED(hr)) std::cerr << "AddStream failed: " << hr << std::endl;
    }
    
    // Configure input media type (PCM)
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

    // PCM requires Block Alignment and Avg Bytes/Sec for strict definition
    if (SUCCEEDED(hr)) {
        UINT32 blockAlign = m_channels * 2; // 16 bits = 2 bytes
        hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
        if (FAILED(hr)) std::cerr << "SetUINT32 BlockAlignment In failed: " << hr << std::endl;
        
        if (SUCCEEDED(hr)) {
            hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_sampleRate * blockAlign);
            if (FAILED(hr)) std::cerr << "SetUINT32 AvgBytesPerSecond In failed: " << hr << std::endl;
        }
    }
    
    // Often required for strict topology building
    if (SUCCEEDED(hr) && m_channels > 0 && m_channels <= 2) {
       DWORD channelMask = (m_channels == 2) ? 3 : 4; // Stereo (FL|FR) or Mono (FC). 3=0x3, 4=0x4 is actually FL. Mono usually 4 (FC) or 3?
       // WAVE_FORMAT_PCM default mask logic:
       // 1 channel: SPEAKER_FRONT_CENTER (0x4)
       // 2 channels: SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT (0x3)
       if (m_channels == 1) channelMask = 0x4; // FC
       if (m_channels == 2) channelMask = 0x3; // FL|FR
       
       // Note: Some encoders ignore this, others require it
       // Let's try setting it if it's unset
       hr = pMediaTypeIn->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, channelMask);
       if (FAILED(hr)) {
           std::cerr << "SetUINT32 ChannelMask In failed (ignoring): " << hr << std::endl;
           hr = S_OK; // Ignore failure here as it might be optional
       }
    }
    
    if (SUCCEEDED(hr)) {
        hr = pSinkWriter->SetInputMediaType(m_streamIndex, pMediaTypeIn, NULL);
        if (FAILED(hr)) std::cerr << "SetInputMediaType failed (Likely format mismatch): " << hr << std::endl;
    }
    
    if (SUCCEEDED(hr)) {
        hr = pSinkWriter->BeginWriting();
        if (FAILED(hr)) std::cerr << "BeginWriting failed: " << hr << std::endl;
    }
    
    if (SUCCEEDED(hr)) {
        m_pSinkWriter = pSinkWriter;
        m_pSinkWriter->AddRef();
    } else {
        std::cerr << "ConfigSinkWriter failed at step " << (pSinkWriter ? "Setup" : "Creation") << ": " << hr << std::endl;
        
        // Detailed error check
        if (!pSinkWriter) std::cerr << "MFCreateSinkWriterFromURL failed" << std::endl;
        else if (!pMediaTypeOut) std::cerr << "MFCreateMediaType (Out) failed" << std::endl;
        else if (!pMediaTypeIn) std::cerr << "MFCreateMediaType (In) failed" << std::endl;
    }
    
    SafeRelease(&pSinkWriter);
    SafeRelease(&pMediaTypeOut);
    SafeRelease(&pMediaTypeIn);
    
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
