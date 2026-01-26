#include "record.h"
#include "record_windows_plugin.h"

// Miniaudio implementation (macros defined in CMakeLists.txt)
#include "miniaudio.h"

#include <iostream>
#include <algorithm>
#include <cmath>

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

    if (!pInput || !m_pConfig) {
        return;
    }

    const int16_t* samples = nullptr;
    ma_uint64 outFrames = 0;

    if (m_dataConverterInitialized) {
        ma_uint64 inFrames = frameCount;
        if (ma_data_converter_get_expected_output_frame_count(&m_dataConverter, inFrames, &outFrames) != MA_SUCCESS || outFrames == 0) {
            return;
        }

        m_convertBuffer.resize(static_cast<size_t>(outFrames * m_targetChannels));
        ma_uint64 outCapacity = outFrames;
        if (ma_data_converter_process_pcm_frames(&m_dataConverter, pInput, &inFrames, m_convertBuffer.data(), &outCapacity) != MA_SUCCESS) {
            return;
        }
        outFrames = outCapacity;
        if (outFrames == 0) {
            return;
        }
        samples = m_convertBuffer.data();
    } else {
        const size_t sampleCount = frameCount * m_pConfig->numChannels;
        if (m_captureFormat == ma_format_s16) {
            samples = static_cast<const int16_t*>(pInput);
            outFrames = frameCount;
        } else {
            m_convertBuffer.resize(sampleCount);
            if (m_captureFormat == ma_format_f32) {
                const float* input = static_cast<const float*>(pInput);
                for (size_t i = 0; i < sampleCount; ++i) {
                    float clamped = std::max(-1.0f, std::min(1.0f, input[i]));
                    m_convertBuffer[i] = static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
                }
            } else if (m_captureFormat == ma_format_s32) {
                const int32_t* input = static_cast<const int32_t*>(pInput);
                for (size_t i = 0; i < sampleCount; ++i) {
                    int32_t v = input[i];
                    float scaled = static_cast<float>(v) / 2147483647.0f;
                    scaled = std::max(-1.0f, std::min(1.0f, scaled));
                    m_convertBuffer[i] = static_cast<int16_t>(std::lrintf(scaled * 32767.0f));
                }
            } else if (m_captureFormat == ma_format_u8) {
                const uint8_t* input = static_cast<const uint8_t*>(pInput);
                for (size_t i = 0; i < sampleCount; ++i) {
                    m_convertBuffer[i] = static_cast<int16_t>((static_cast<int>(input[i]) - 128) << 8);
                }
            } else {
                return;
            }
            samples = m_convertBuffer.data();
            outFrames = frameCount;
        }
    }

    size_t byteCount = static_cast<size_t>(outFrames * m_pConfig->numChannels * sizeof(int16_t));

    // Calculate amplitude
    CalculateAmplitude(samples, static_cast<size_t>(outFrames * m_pConfig->numChannels));

    // Write to ring buffer (encoder thread will read from it)
    if (m_ringBuffer) {
        m_ringBuffer->Write(reinterpret_cast<const uint8_t*>(samples), byteCount);
    }
}

void Recorder::EncoderThreadFunc() {
    const int frameSize = (m_pConfig && m_pConfig->encoderName == AudioEncoder().aacLc)
        ? 1024
        : (m_pConfig->sampleRate * 20 / 1000);  // 20ms frame for non-AAC
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
            UninitDevice();
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
            UninitDevice();
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

    if (m_pConfig && m_pConfig->encoderName == AudioEncoder().aacLc) {
        if (m_pConfig->sampleRate != 44100 && m_pConfig->sampleRate != 48000) {
            std::cout << "Record: AAC requires 44.1k or 48k. Overriding sample rate to 48000." << std::endl;
            m_pConfig->sampleRate = 48000;
        }
        if (m_pConfig->numChannels < 1) {
            m_pConfig->numChannels = 1;
        }
        if (m_pConfig->numChannels > 2) {
            m_pConfig->numChannels = 2;
        }
    }

    // Initialize miniaudio context if not already done
    if (!m_contextInitialized) {
        ma_context_config contextConfig = ma_context_config_init();
        if (ma_context_init(NULL, 0, &contextConfig, &m_context) != MA_SUCCESS) {
            return E_FAIL;
        }
        m_contextInitialized = true;
    }

    // Check if we can reuse the existing initialized device
    bool reuseDevice = false;
    if (m_deviceInitialized) {
        if (m_lastDeviceId == m_pConfig->deviceId &&
            m_lastNumChannels == m_pConfig->numChannels &&
            m_lastSampleRate == m_pConfig->sampleRate) {
            reuseDevice = true;
        }
    }

    if (!reuseDevice) {
        UninitDevice();
    }

    // Capture Device ID negotiation
    ma_device_id selectedDeviceID;
    bool hasSelectedDevice = false;
    
    if (!reuseDevice) {
        // Resolve device ID if requested
        if (!m_pConfig->deviceId.empty()) {
            try {
                int deviceIndex = std::stoi(m_pConfig->deviceId);
                
                ma_device_info* pPlaybackDeviceInfos;
                ma_uint32 playbackDeviceCount;
                ma_device_info* pCaptureDeviceInfos;
                ma_uint32 captureDeviceCount;
                
                if (ma_context_get_devices(&m_context, &pPlaybackDeviceInfos, &playbackDeviceCount, &pCaptureDeviceInfos, &captureDeviceCount) == MA_SUCCESS) {
                    if (deviceIndex >= 0 && deviceIndex < (int)captureDeviceCount) {
                        selectedDeviceID = pCaptureDeviceInfos[deviceIndex].id;
                        hasSelectedDevice = true;
                        std::cout << "Record: Resolved device index " << deviceIndex << " to ID." << std::endl;
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
    }

    bool targetRateSupported = true;
    bool targetChannelsSupported = true;

    if (hasSelectedDevice) {
        ma_device_info deviceInfo;
        if (ma_context_get_device_info(&m_context, ma_device_type_capture, &selectedDeviceID, &deviceInfo) == MA_SUCCESS) {
            if (deviceInfo.nativeDataFormatCount == 0) {
                std::cerr << "Record: Selected device has no native format info; will try requested format." << std::endl;
            } else {
                std::cout << "Record: Selected device reports " << deviceInfo.nativeDataFormatCount << " native formats." << std::endl;
            }

            if (m_pConfig && m_pConfig->encoderName == AudioEncoder().aacLc) {
                auto supportsRate = [&](ma_uint32 rate) -> bool {
                    for (ma_uint32 i = 0; i < deviceInfo.nativeDataFormatCount; ++i) {
                        auto fmt = deviceInfo.nativeDataFormats[i];
                        if (fmt.format != ma_format_s16 && fmt.format != ma_format_f32 && fmt.format != ma_format_s32 && fmt.format != ma_format_u8) {
                            continue;
                        }
                        if (fmt.sampleRate == 0 || fmt.sampleRate == rate) {
                            return true;
                        }
                    }
                    return false;
                };

                targetRateSupported = supportsRate(static_cast<ma_uint32>(m_pConfig->sampleRate));

                if (!supportsRate(48000) && supportsRate(44100)) {
                    std::cout << "Record: Selected device does not advertise 48k. Falling back to 44.1k for AAC." << std::endl;
                    m_pConfig->sampleRate = 44100;
                    targetRateSupported = true;
                } else if (supportsRate(48000)) {
                    m_pConfig->sampleRate = 48000;
                    targetRateSupported = true;
                } else if (!supportsRate(static_cast<ma_uint32>(m_pConfig->sampleRate))) {
                    targetRateSupported = false;
                }

                auto supportsChannels = [&](ma_uint32 channels) -> bool {
                    for (ma_uint32 i = 0; i < deviceInfo.nativeDataFormatCount; ++i) {
                        auto fmt = deviceInfo.nativeDataFormats[i];
                        if (fmt.format != ma_format_s16 && fmt.format != ma_format_f32 && fmt.format != ma_format_s32 && fmt.format != ma_format_u8) {
                            continue;
                        }
                        if (fmt.channels == 0 || fmt.channels == channels) {
                            return true;
                        }
                    }
                    return false;
                };

                targetChannelsSupported = supportsChannels((ma_uint32)m_pConfig->numChannels);

                if (!supportsChannels((ma_uint32)m_pConfig->numChannels)) {
                    if (supportsChannels(1)) {
                        std::cout << "Record: Selected device does not advertise " << m_pConfig->numChannels << " channels. Falling back to mono for AAC." << std::endl;
                        m_pConfig->numChannels = 1;
                        targetChannelsSupported = true;
                    } else if (supportsChannels(2)) {
                        std::cout << "Record: Selected device does not advertise " << m_pConfig->numChannels << " channels. Falling back to stereo for AAC." << std::endl;
                        m_pConfig->numChannels = 2;
                        targetChannelsSupported = true;
                    } else {
                        targetChannelsSupported = false;
                    }
                }
            }
        } else {
            std::cerr << "Record: Failed to query selected device native formats." << std::endl;
        }
    }

    // Attempt 1: Strict Low Latency
    if (!reuseDevice) {
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_capture);
        deviceConfig.capture.format = ma_format_s16;
        deviceConfig.capture.channels = targetChannelsSupported ? m_pConfig->numChannels : 0;
        deviceConfig.sampleRate = targetRateSupported ? m_pConfig->sampleRate : 0;
        deviceConfig.dataCallback = AudioDataCallback;
        deviceConfig.pUserData = this;
        deviceConfig.performanceProfile = ma_performance_profile_low_latency;
        deviceConfig.periodSizeInFrames = 0; 
        deviceConfig.wasapi.noHardwareOffloading = MA_TRUE; 
        
        if (hasSelectedDevice) {
            deviceConfig.capture.pDeviceID = &selectedDeviceID;
        }

        ma_result initResult = ma_device_init(&m_context, &deviceConfig, &m_device);

        if (initResult != MA_SUCCESS && hasSelectedDevice) {
            std::cerr << "Record: Strict init failed for selected device (" << initResult << "). Retrying with default device." << std::endl;
            deviceConfig.capture.pDeviceID = NULL;
            initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
        }

        if (initResult != MA_SUCCESS) {
            deviceConfig.capture.format = ma_format_f32;
            if (hasSelectedDevice) {
                deviceConfig.capture.pDeviceID = &selectedDeviceID;
            }
            initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
            if (initResult != MA_SUCCESS && hasSelectedDevice) {
                std::cerr << "Record: Strict f32 init failed for selected device (" << initResult << "). Retrying with default device." << std::endl;
                deviceConfig.capture.pDeviceID = NULL;
                initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
            }
        }
        
        // Attempt 2: Compatibility Fallback
        if (initResult != MA_SUCCESS) {
            std::cerr << "Record: Strict init failed (" << initResult << "). Retrying with compatibility defaults." << std::endl;
            
            // Clean reset of config
            deviceConfig = ma_device_config_init(ma_device_type_capture);
            deviceConfig.capture.format = ma_format_s16; // We need S16 for our callback logic
            deviceConfig.capture.channels = 0; // Allow native channels (will update config later)
            if (m_pConfig && m_pConfig->encoderName == AudioEncoder().aacLc) {
                deviceConfig.sampleRate = m_pConfig->sampleRate;
            } else {
                deviceConfig.sampleRate = 0;   // Allow native rate (will update config later)
            }
            deviceConfig.dataCallback = AudioDataCallback;
            deviceConfig.pUserData = this;
            deviceConfig.performanceProfile = ma_performance_profile_conservative;
            
            if (hasSelectedDevice) {
                deviceConfig.capture.pDeviceID = &selectedDeviceID;
            }

            initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
            if (initResult != MA_SUCCESS && hasSelectedDevice) {
                std::cerr << "Record: Compatibility init failed for selected device (" << initResult << "). Retrying with default device." << std::endl;
                deviceConfig.capture.pDeviceID = NULL;
                initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
            }

            if (initResult != MA_SUCCESS) {
                deviceConfig.capture.format = ma_format_f32;
                if (hasSelectedDevice) {
                    deviceConfig.capture.pDeviceID = &selectedDeviceID;
                }
                initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
                if (initResult != MA_SUCCESS && hasSelectedDevice) {
                    std::cerr << "Record: Compatibility f32 init failed for selected device (" << initResult << "). Retrying with default device." << std::endl;
                    deviceConfig.capture.pDeviceID = NULL;
                    initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
                }
            }
        }

        if (initResult != MA_SUCCESS && hasSelectedDevice) {
            ma_device_info deviceInfo;
            if (ma_context_get_device_info(&m_context, ma_device_type_capture, &selectedDeviceID, &deviceInfo) == MA_SUCCESS) {
                std::cerr << "Record: Probing supported formats for selected device..." << std::endl;
                for (ma_uint32 i = 0; i < deviceInfo.nativeDataFormatCount; ++i) {
                    auto fmt = deviceInfo.nativeDataFormats[i];
                    if (fmt.format != ma_format_s16 && fmt.format != ma_format_f32 && fmt.format != ma_format_s32 && fmt.format != ma_format_u8) {
                        continue;
                    }

                    deviceConfig = ma_device_config_init(ma_device_type_capture);
                    deviceConfig.capture.format = fmt.format;
                    deviceConfig.capture.channels = (fmt.channels > 0) ? fmt.channels : 0;
                    deviceConfig.sampleRate = (fmt.sampleRate > 0) ? fmt.sampleRate : 0;
                    deviceConfig.dataCallback = AudioDataCallback;
                    deviceConfig.pUserData = this;
                    deviceConfig.performanceProfile = ma_performance_profile_conservative;
                    deviceConfig.capture.pDeviceID = &selectedDeviceID;

                    initResult = ma_device_init(&m_context, &deviceConfig, &m_device);
                    if (initResult == MA_SUCCESS) {
                        break;
                    }
                }
            }
        }

        if (initResult != MA_SUCCESS) {
            std::cerr << "Record: Failed to initialize device. Result=" << initResult << " (" << ma_result_description(initResult) << ")" << std::endl;
            return E_FAIL;
        }
        m_captureFormat = deviceConfig.capture.format;
        m_deviceInitialized = true;
    }
    
    // Update config with actual negotiated parameters
    // This ensures encoders are initialized with the correct format (e.g. 48k vs 44.1k)
    bool configChanged = false;
    
    if ((ma_uint32)m_pConfig->sampleRate != m_device.sampleRate) {
        if (!reuseDevice) std::cout << "Record: Device sample rate " << m_device.sampleRate << " (target " << m_pConfig->sampleRate << ")" << std::endl;
        if (m_pConfig->encoderName != AudioEncoder().aacLc) {
            m_pConfig->sampleRate = (int)m_device.sampleRate;
            configChanged = true;
        }
    }
    
    if ((ma_uint32)m_pConfig->numChannels != m_device.capture.channels) {
        if (!reuseDevice) std::cout << "Record: Device channels " << m_device.capture.channels << " (target " << m_pConfig->numChannels << ")" << std::endl;
        if (m_pConfig->encoderName != AudioEncoder().aacLc) {
            m_pConfig->numChannels = (int)m_device.capture.channels;
            configChanged = true;
        }
    }
    
    if (configChanged && !reuseDevice) {
        std::cout << "Record: Final Config -> Rate: " << m_pConfig->sampleRate << ", Channels: " << m_pConfig->numChannels << std::endl;
    }

    if (!reuseDevice) {
        m_lastDeviceId = m_pConfig->deviceId;
        m_lastSampleRate = m_pConfig->sampleRate;
        m_lastNumChannels = m_pConfig->numChannels;
    }

    // Configure data converter if needed (e.g., device native rate differs from target)
    m_inputFormat = m_captureFormat;
    m_inputSampleRate = (int)m_device.sampleRate;
    m_inputChannels = (int)m_device.capture.channels;
    m_targetSampleRate = m_pConfig->sampleRate;
    m_targetChannels = m_pConfig->numChannels;

    if (m_dataConverterInitialized) {
        ma_data_converter_uninit(&m_dataConverter, NULL);
        m_dataConverterInitialized = false;
    }

    if (m_inputFormat != ma_format_s16 ||
        m_inputSampleRate != m_targetSampleRate ||
        m_inputChannels != m_targetChannels) {
        ma_data_converter_config dcConfig = ma_data_converter_config_init(
            m_inputFormat,
            ma_format_s16,
            (ma_uint32)m_inputChannels,
            (ma_uint32)m_targetChannels,
            (ma_uint32)m_inputSampleRate,
            (ma_uint32)m_targetSampleRate);
        dcConfig.resampling.algorithm = ma_resample_algorithm_linear;
        dcConfig.resampling.linear.lpfOrder = 4;

        if (ma_data_converter_init(&dcConfig, NULL, &m_dataConverter) == MA_SUCCESS) {
            m_dataConverterInitialized = true;
            std::cout << "Record: Data converter enabled (" << m_inputSampleRate << "->" << m_targetSampleRate << ", ch " << m_inputChannels << "->" << m_targetChannels << ")." << std::endl;
        } else {
            std::cerr << "Record: Failed to initialize data converter." << std::endl;
        }
    }

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

    // Stop miniaudio device (but don't uninit yet, to allow reuse)
    if (m_deviceInitialized) {
        ma_device_stop(&m_device);
        // ma_device_uninit(&m_device); -> Moved to UninitDevice()
        // m_deviceInitialized = false;
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

    if (m_dataConverterInitialized) {
        ma_data_converter_uninit(&m_dataConverter, NULL);
        m_dataConverterInitialized = false;
    }

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

    UninitDevice();

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


void Recorder::UninitDevice() {
    if (m_deviceInitialized) {
        ma_device_uninit(&m_device);
        m_deviceInitialized = false;
    }
}

} // namespace record_windows
