#pragma once

#include <aacenc_lib.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>

namespace record_windows {

class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();

    bool Initialize(const std::wstring& path, int sampleRate, int channels, int bitrate);
    bool EncodeFrame(const int16_t* pcm, int frameSize);
    void Finalize();
    
private:
    HANDLE_AACENCODER m_encoder = nullptr;
    std::ofstream m_outputFile;
    std::vector<uint8_t> m_outputBuffer;
    
    int m_sampleRate = 44100;
    int m_channels = 2;
    int m_bitrate = 128000;
    int m_frameSize = 1024;
    
    bool m_initialized = false;
    std::mutex m_mutex;

    bool ConfigureEncoder();
    bool EncodeInternal(const INT_PCM* pcm, int numInSamples);
    void FlushEncoder();
};

} // namespace record_windows
