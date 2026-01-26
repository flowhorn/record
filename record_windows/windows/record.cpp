#include "record.h"
#include "record_windows_plugin.h"

// Miniaudio implementation (macros defined in CMakeLists.txt)
#include "miniaudio.h"

#include <iostream>

namespace record_windows {

// Ring buffer size: 100ms of audio at 48kHz mono (16-bit samples)
static const size_t RING_BUFFER_SIZE = 48000 * 2 * 1;  // 100ms @ 48kHz, 2 bytes/sample, mono

// static
HRESULT Recorder::CreateInstance(EventStreamHandler<>* stateEventHandler, 
                                  EventStreamHandler<>* recordEventHandler, 
                                  Recorder** ppRecorder) {
    auto pRecorder = new (std::nothrow) Recorder(stateEventHandler, recordEventHandler);
    if (pRecorder == NULL) {
        return E_OUTOFMEMORY;
    }
    *ppRecorder = pRecorder;
    return S_OK;
}

Recorder::Recorder(EventStreamHandler<>* stateEventHandler, EventStreamHandler<>* recordEventHandler)
    : m_stateEventHandler(stateEventHandler),
      m_recordEventHandler(recordEventHandler) {
}

Recorder::~Recorder() {
    Dispose();
}

// static
void Recorder::AudioDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pOutput;  // Unused for capture
    
    Recorder* recorder = static_cast<Recorder*>(pDevice->pUserData);
    if (recorder) {
        recorder->OnAudioData(pInput, frameCount);
    }
}

void Recorder::OnAudioData(const void* pInput, ma_uint32 frameCount) {
    if (m_isPaused.load(std::memory_order_relaxed)) {
        return;
    }

    const int16_t* samples = static_cast<const int16_t*>(pInput);
    size_t byteCount = frameCount * sizeof(int16_t) * m_pConfig->numChannels;

    // Calculate amplitude
    CalculateAmplitude(samples, frameCount * m_pConfig->numChannels);

    // Write to ring buffer (encoder thread will read from it)
    if (m_ringBuffer) {
        m_ringBuffer->Write(reinterpret_cast<const uint8_t*>(pInput), byteCount);
    }
}

void Recorder::EncoderThreadFunc() {
    const int frameSize = m_pConfig->sampleRate * 20 / 1000;  // 20ms frame
    const size_t bytesPerFrame = frameSize * sizeof(int16_t) * m_pConfig->numChannels;
    std::vector<int16_t> frameBuffer(frameSize * m_pConfig->numChannels);

    while (m_encoderRunning.load(std::memory_order_relaxed)) {
        // Wait for enough data
        size_t available = m_ringBuffer->Available();
        if (available < bytesPerFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Read from ring buffer
        size_t bytesRead = m_ringBuffer->Read(reinterpret_cast<uint8_t*>(frameBuffer.data()), bytesPerFrame);
        if (bytesRead < bytesPerFrame) {
            continue;
        }

        // Handle different output modes
        if (m_opusEncoder && m_opusEncoder->IsInitialized()) {
            // Opus file output
            m_opusEncoder->EncodeFrame(frameBuffer.data(), frameSize);
            m_dataWritten += bytesRead;
        }
        else if (m_aacEncoder) {
            // AAC file output
            m_aacEncoder->EncodeFrame(frameBuffer.data(), frameSize);
            m_dataWritten += bytesRead;
        }
        else if (m_isWavOutput && m_wavFile.is_open()) {
            // WAV file output (raw PCM)
            m_wavFile.write(reinterpret_cast<char*>(frameBuffer.data()), bytesRead);
            m_dataWritten += bytesRead;
        }
        else if (m_recordEventHandler) {
            // PCM stream to Flutter
            std::vector<uint8_t> bytes(reinterpret_cast<uint8_t*>(frameBuffer.data()),
                                       reinterpret_cast<uint8_t*>(frameBuffer.data()) + bytesRead);
            
            RecordWindowsPlugin::RunOnMainThread([this, bytes]() -> void {
                if (m_recordEventHandler) {
                    m_recordEventHandler->Success(std::make_unique<flutter::EncodableValue>(bytes));
                }
            });
            m_dataWritten += bytesRead;
        }
    }
}

HRESULT Recorder::Start(std::unique_ptr<RecordConfig> config, std::wstring path) {
    bool supported = false;
    HRESULT hr = isEncoderSupported(config->encoderName, &supported);

    if (FAILED(hr) || !supported) {
        return E_NOTIMPL;
    }

    m_recordingPath = path;
    hr = InitRecording(std::move(config));

    if (SUCCEEDED(hr)) {
        // Set up output based on encoder
        if (m_pConfig->encoderName == AudioEncoder().opus) {
            // Initialize Opus encoder
            m_opusEncoder = std::make_unique<OpusAudioEncoder>();
            if (!m_opusEncoder->Initialize(path, m_pConfig->sampleRate, 
                                            m_pConfig->numChannels, m_pConfig->bitRate)) {
                EndRecording();
                return E_FAIL;
            }
        }
        else if (m_pConfig->encoderName == AudioEncoder().aacLc) {
            // Initialize AAC encoder
            m_aacEncoder = std::make_unique<AacEncoder>();
            if (!m_aacEncoder->Initialize(path, m_pConfig->sampleRate, 
                                           m_pConfig->numChannels, m_pConfig->bitRate)) {
                EndRecording();
                return E_FAIL;
            }
        }
        else if (m_pConfig->encoderName == AudioEncoder().wav || 
                 m_pConfig->encoderName == AudioEncoder().pcm16bits) {
            // Open WAV file
            m_wavFile.open(path, std::ios::binary);
            if (!m_wavFile.is_open()) {
                EndRecording();
                return E_FAIL;
            }
            m_isWavOutput = true;

            // Write WAV header placeholder (will be filled in on stop)
            char header[44] = {0};
            m_wavFile.write(header, sizeof(header));
        }
    }

    if (SUCCEEDED(hr)) {
        // Start miniaudio device
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            EndRecording();
            return E_FAIL;
        }

        // Start encoder thread
        m_encoderRunning = true;
        m_encoderThread = std::thread(&Recorder::EncoderThreadFunc, this);

        UpdateState(RecordState::record);
    }

    return hr;
}

HRESULT Recorder::StartStream(std::unique_ptr<RecordConfig> config) {
    if (config->encoderName != AudioEncoder().pcm16bits) {
        return E_NOTIMPL;
    }

    HRESULT hr = InitRecording(std::move(config));

    if (SUCCEEDED(hr)) {
        // Start miniaudio device
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            EndRecording();
            return E_FAIL;
        }

        // Start encoder thread (will stream PCM to Flutter)
        m_encoderRunning = true;
        m_encoderThread = std::thread(&Recorder::EncoderThreadFunc, this);

        UpdateState(RecordState::record);
    }

    return hr;
}

HRESULT Recorder::InitRecording(std::unique_ptr<RecordConfig> config) {
    EndRecording();

    m_pConfig = std::move(config);
    m_dataWritten = 0;
    m_amplitude = -160.0;
    m_maxAmplitude = -160.0;

    // Initialize miniaudio context if not already done
    if (!m_contextInitialized) {
        ma_context_config contextConfig = ma_context_config_init();
        if (ma_context_init(NULL, 0, &contextConfig, &m_context) != MA_SUCCESS) {
            return E_FAIL;
        }
        m_contextInitialized = true;
    }

    // Configure capture device
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_capture);
    deviceConfig.capture.format = ma_format_s16;
    deviceConfig.capture.channels = m_pConfig->numChannels;
    deviceConfig.sampleRate = m_pConfig->sampleRate;
    deviceConfig.dataCallback = AudioDataCallback;
    deviceConfig.pUserData = this;
    
    // Low latency settings
    deviceConfig.performanceProfile = ma_performance_profile_low_latency;
    deviceConfig.periodSizeInFrames = 0; // Let WASAPI decide optimal buffer size
    deviceConfig.wasapi.noHardwareOffloading = MA_TRUE; 
    
    // We still calculate our ring buffer based on ~100ms, but native buffer will be smaller
    // deviceConfig.periodSizeInFrames = m_pConfig->sampleRate * 20 / 1000;  // REMOVED fixed 20ms buffer

    // Set specific device if requested
    if (!m_pConfig->deviceId.empty()) {
        try {
            int deviceIndex = std::stoi(m_pConfig->deviceId);
            
            ma_device_info* pPlaybackDeviceInfos;
            ma_uint32 playbackDeviceCount;
            ma_device_info* pCaptureDeviceInfos;
            ma_uint32 captureDeviceCount;
            
            if (ma_context_get_devices(&m_context, &pPlaybackDeviceInfos, &playbackDeviceCount, &pCaptureDeviceInfos, &captureDeviceCount) == MA_SUCCESS) {
                if (deviceIndex >= 0 && deviceIndex < (int)captureDeviceCount) {
                    deviceConfig.capture.pDeviceID = &pCaptureDeviceInfos[deviceIndex].id;
                    std::cout << "Record: Selected device index " << deviceIndex << ": " << pCaptureDeviceInfos[deviceIndex].name << std::endl;
                } else {
                    std::cerr << "Record: Device index " << deviceIndex << " out of range (count=" << captureDeviceCount << ")" << std::endl;
                }
            } else {
                std::cerr << "Record: Failed to list devices" << std::endl;
            }
        } catch (...) {
            std::cerr << "Record: Exception parsing deviceId" << std::endl;
        }
    }

    ma_result initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
    
    // If strict low latency fails (likely due to format mismatch), retry with default profile
    if (initResult == MA_FORMAT_NOT_SUPPORTED) {
        std::cerr << "Record: Strict low latency failed (Format not supported). Retrying with default profile and native sample rate." << std::endl;
        
        // Reset config to defaults but keep callback/data
        deviceConfig.performanceProfile = ma_performance_profile_conservative;
        deviceConfig.periodSizeInFrames = 0;
        deviceConfig.sampleRate = 0; // Let backend choose valid rate (we'll resample if needed or just use what we get)
        deviceConfig.wasapi.shareMode = ma_wasapi_share_mode_shared; // Explicitly request shared mode

        
        // If the user *really* wanted a specific rate, miniaudio converter *should* kick in if we don't disable it.
        // But for safety, let's try 0 sample rate and just use what the device gives us. 
        // Note: usage of ring buffer assumes we push data in format we agreed on.
        // Actually, if we set sampleRate=0, miniaudio picks device native. 
        // But our Encoder expects `m_pConfig->sampleRate`. 
        // We really want miniaudio to CONVERT for us.
        
        // Try again with default profile, requesting the SAME sample rate (hoping converter works in default mode)
        deviceConfig.sampleRate = m_pConfig->sampleRate;
        initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
    }

    if (initResult != MA_SUCCESS) {
        std::cerr << "Record: Failed to initialize device. Result=" << initResult << " (" << ma_result_description(initResult) << ")" << std::endl;
        return E_FAIL;
    }
    m_deviceInitialized = true;

    // Create ring buffer
    m_ringBuffer = std::make_unique<RingBuffer>(RING_BUFFER_SIZE);

    return S_OK;
}

HRESULT Recorder::Pause() {
    m_isPaused = true;
    UpdateState(RecordState::pause);
    return S_OK;
}

HRESULT Recorder::Resume() {
    m_isPaused = false;
    UpdateState(RecordState::record);
    return S_OK;
}

HRESULT Recorder::Stop() {
    if (m_dataWritten == 0) {
        return Cancel();
    }

    HRESULT hr = EndRecording();

    if (SUCCEEDED(hr)) {
        UpdateState(RecordState::stop);
    }

    return hr;
}

HRESULT Recorder::Cancel() {
    auto recordingPath = GetRecordingPath();
    HRESULT hr = EndRecording();

    if (SUCCEEDED(hr)) {
        UpdateState(RecordState::stop);

        if (!recordingPath.empty()) {
            DeleteFile(recordingPath.c_str());
        }
    }

    return hr;
}

bool Recorder::IsPaused() {
    return m_recordState == RecordState::pause;
}

bool Recorder::IsRecording() {
    return m_recordState == RecordState::record;
}

HRESULT Recorder::EndRecording() {
    AutoLock lock(m_critsec);

    // Stop encoder thread
    if (m_encoderRunning) {
        m_encoderRunning = false;
        if (m_encoderThread.joinable()) {
            m_encoderThread.join();
        }
    }

    // Stop and uninit miniaudio device
    if (m_deviceInitialized) {
        ma_device_stop(&m_device);
        ma_device_uninit(&m_device);
        m_deviceInitialized = false;
    }

    // Finalize Opus encoder
    if (m_opusEncoder) {
        m_opusEncoder->Finalize();
        m_opusEncoder.reset();
    }
    
    if (m_aacEncoder) {
        m_aacEncoder->Finalize();
        m_aacEncoder.reset();
    }

    // Finalize WAV file
    if (m_isWavOutput && m_wavFile.is_open()) {
        // Write WAV header
        m_wavFile.seekp(0);
        
        // RIFF header
        m_wavFile.write("RIFF", 4);
        uint32_t fileSize = static_cast<uint32_t>(m_dataWritten + 36);
        m_wavFile.write(reinterpret_cast<char*>(&fileSize), 4);
        m_wavFile.write("WAVE", 4);

        // fmt subchunk
        m_wavFile.write("fmt ", 4);
        uint32_t fmtSize = 16;
        m_wavFile.write(reinterpret_cast<char*>(&fmtSize), 4);
        uint16_t audioFormat = 1;  // PCM
        m_wavFile.write(reinterpret_cast<char*>(&audioFormat), 2);
        uint16_t numChannels = static_cast<uint16_t>(m_pConfig ? m_pConfig->numChannels : 1);
        m_wavFile.write(reinterpret_cast<char*>(&numChannels), 2);
        uint32_t sampleRate = m_pConfig ? m_pConfig->sampleRate : 48000;
        m_wavFile.write(reinterpret_cast<char*>(&sampleRate), 4);
        uint32_t byteRate = sampleRate * numChannels * 2;
        m_wavFile.write(reinterpret_cast<char*>(&byteRate), 4);
        uint16_t blockAlign = numChannels * 2;
        m_wavFile.write(reinterpret_cast<char*>(&blockAlign), 2);
        uint16_t bitsPerSample = 16;
        m_wavFile.write(reinterpret_cast<char*>(&bitsPerSample), 2);

        // data subchunk
        m_wavFile.write("data", 4);
        uint32_t dataSize = static_cast<uint32_t>(m_dataWritten);
        m_wavFile.write(reinterpret_cast<char*>(&dataSize), 4);

        m_wavFile.close();
        m_isWavOutput = false;
    }

    // Reset ring buffer
    m_ringBuffer.reset();

    // Reset state
    m_amplitude = -160.0;
    m_maxAmplitude = -160.0;
    m_isPaused = false;
    m_pConfig.reset();
    m_recordingPath.clear();

    return S_OK;
}

HRESULT Recorder::Dispose() {
    HRESULT hr = EndRecording();

    // Uninit context
    if (m_contextInitialized) {
        ma_context_uninit(&m_context);
        m_contextInitialized = false;
    }

    m_stateEventHandler = nullptr;
    m_recordEventHandler = nullptr;

    return hr;
}

void Recorder::UpdateState(RecordState state) {
    m_recordState = state;

    if (m_stateEventHandler) {
        EventStreamHandler<>* handlerPtr = m_stateEventHandler;
        RecordWindowsPlugin::RunOnMainThread([handlerPtr, state]() -> void {
            if (handlerPtr) {
                handlerPtr->Success(std::make_unique<flutter::EncodableValue>(state));
            }
        });
    }
}

std::map<std::string, double> Recorder::GetAmplitude() {
    return {
        {"current", m_amplitude.load()},
        {"max", m_maxAmplitude.load()},
    };
}

void Recorder::CalculateAmplitude(const int16_t* samples, size_t count) {
    int maxSample = 0;

    for (size_t i = 0; i < count; i++) {
        int curSample = std::abs(samples[i]);
        if (curSample > maxSample) {
            maxSample = curSample;
        }
    }

    double amplitude = 20.0 * std::log10(static_cast<double>(maxSample) / 32767.0);
    if (amplitude < -160.0) amplitude = -160.0;

    m_amplitude = amplitude;
    if (amplitude > m_maxAmplitude.load()) {
        m_maxAmplitude = amplitude;
    }
}

std::wstring Recorder::GetRecordingPath() {
    return m_recordingPath;
}

HRESULT Recorder::isEncoderSupported(const std::string encoderName, bool* supported) {
    // Only support opus, aacLc, pcm16bits, and wav with the new implementation
    if (encoderName == AudioEncoder().opus ||
        encoderName == AudioEncoder().aacLc ||
        encoderName == AudioEncoder().pcm16bits ||
        encoderName == AudioEncoder().wav) {
        *supported = true;
    } else {
        *supported = false;
    }
    return S_OK;
}

} // namespace record_windows
