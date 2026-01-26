#pragma once

#include <windows.h>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <fstream>

#include "miniaudio.h"
#include "utils.h"
#include "record_config.h"
#include "event_stream_handler.h"
#include "ring_buffer.h"
#include "encoder/opus_encoder.h"
#include "encoder/aac_encoder.h"

using namespace flutter;

namespace record_windows {

    enum RecordState {
        pause, record, stop
    };

    class Recorder {
    public:
        static HRESULT CreateInstance(EventStreamHandler<>* stateEventHandler, 
                                       EventStreamHandler<>* recordEventHandler, 
                                       Recorder** recorder);

        Recorder(EventStreamHandler<>* stateEventHandler, EventStreamHandler<>* recordEventHandler);
        virtual ~Recorder();

        HRESULT Start(std::unique_ptr<RecordConfig> config, std::wstring path);
        HRESULT StartStream(std::unique_ptr<RecordConfig> config);
        HRESULT Pause();
        HRESULT Resume();
        HRESULT Stop();
        HRESULT Cancel();
        bool IsPaused();
        bool IsRecording();
        HRESULT Dispose();
        std::map<std::string, double> GetAmplitude();
        std::wstring GetRecordingPath();
        HRESULT isEncoderSupported(std::string encoderName, bool* supported);

    private:
        // Miniaudio callback - called from audio thread
        static void AudioDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);
        void OnAudioData(const void* pInput, ma_uint32 frameCount);

        // Encoder thread function
        void EncoderThreadFunc();

        HRESULT InitRecording(std::unique_ptr<RecordConfig> config);
        HRESULT EndRecording();
        void UpdateState(RecordState state);
        void CalculateAmplitude(const int16_t* samples, size_t count);

        // Thread synchronization
        CritSec m_critsec;

        // Miniaudio
        ma_context m_context;
        ma_device m_device;
        bool m_contextInitialized = false;
        bool m_deviceInitialized = false;

        // Ring buffer for audio data
        std::unique_ptr<RingBuffer> m_ringBuffer;

        // Encoder
        std::unique_ptr<OpusAudioEncoder> m_opusEncoder;
        std::unique_ptr<AacEncoder> m_aacEncoder;
        std::thread m_encoderThread;
        std::atomic<bool> m_encoderRunning{false};

        // WAV file output (for pcm16bits/wav encoder)
        std::ofstream m_wavFile;
        bool m_isWavOutput = false;

        // Recording state
        std::wstring m_recordingPath;
        std::unique_ptr<RecordConfig> m_pConfig;
        RecordState m_recordState = RecordState::stop;
        std::atomic<bool> m_isPaused{false};

        // Amplitude tracking
        std::atomic<double> m_amplitude{-160.0};
        std::atomic<double> m_maxAmplitude{-160.0};
        size_t m_dataWritten = 0;

        // Event handlers
        EventStreamHandler<>* m_stateEventHandler;
        EventStreamHandler<>* m_recordEventHandler;
    };

} // namespace record_windows