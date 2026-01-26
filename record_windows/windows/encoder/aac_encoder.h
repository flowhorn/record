#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <string>
#include <vector>
#include <mutex>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace record_windows {

class AacEncoder {
public:
    AacEncoder();
    ~AacEncoder();

    bool Initialize(const std::wstring& path, int sampleRate, int channels, int bitrate);
    bool EncodeFrame(const int16_t* pcm, int frameSize);
    void Finalize();
    
private:
    IMFSinkWriter* m_pSinkWriter = NULL;
    DWORD m_streamIndex = 0;
    
    int m_sampleRate = 44100;
    int m_channels = 2;
    int m_bitrate = 128000;
    
    long long m_duration = 0;
    
    bool m_initialized = false;
    std::mutex m_mutex;
    
    HRESULT ConfigSinkWriter(const std::wstring& path);
    HRESULT WriteSample(IMFSample* pSample);
};

} // namespace record_windows
