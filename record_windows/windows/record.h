#pragma once

#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <Mferror.h>

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <map>

#include "utils.h"
#include "record_config.h"
#include "event_stream_handler.h"

using namespace flutter;

namespace record_windows
{
	enum RecordState {
		pause, record, stop
	};

	class Recorder
	{
	public:
		static HRESULT CreateInstance(
			EventStreamHandler<>* stateEventHandler,
			EventStreamHandler<>* recordEventHandler,
			Recorder** recorder);

		Recorder(EventStreamHandler<>* stateEventHandler, EventStreamHandler<>* recordEventHandler);
		virtual ~Recorder();

		// Prevent copy/move
		Recorder(const Recorder&) = delete;
		Recorder& operator=(const Recorder&) = delete;
		Recorder(Recorder&&) = delete;
		Recorder& operator=(Recorder&&) = delete;

		// Recording control
		HRESULT Start(std::unique_ptr<RecordConfig> config, std::wstring path);
		HRESULT StartStream(std::unique_ptr<RecordConfig> config);
		HRESULT Pause();
		HRESULT Resume();
		HRESULT Stop();
		HRESULT Cancel();
		bool IsPaused();
		bool IsRecording();
		HRESULT Dispose();

		// Info
		std::map<std::string, double> GetAmplitude();
		std::wstring GetRecordingPath();
		HRESULT isEncoderSupported(std::string encoderName, bool* supported);

	private:
		// WASAPI initialization
		HRESULT InitializeWasapi(const std::string& deviceId);
		HRESULT InitializeEncoder(const std::wstring& path);
		void CleanupWasapi();
		void CleanupEncoder();

		// Capture thread
		void CaptureThreadProc();
		void ProcessCapturedData(BYTE* pData, UINT32 numFrames, DWORD flags);

		// Format conversion - CRITICAL for WASAPI which often uses float
		void ConvertFloatToPcm16(const float* pInput, int16_t* pOutput, UINT32 numSamples);
		void ConvertPcm32ToPcm16(const int32_t* pInput, int16_t* pOutput, UINT32 numSamples);

		// Encoding
		HRESULT WriteToEncoder(const BYTE* pData, UINT32 numBytes, LONGLONG timestamp);
		HRESULT WritePcmToFile(const BYTE* pData, UINT32 numBytes);
		HRESULT FinalizeWavHeader();

		// Amplitude calculation
		void CalculateAmplitude(const BYTE* pData, UINT32 numBytes);

		// State management
		void UpdateState(RecordState state);
		HRESULT EndRecording();

		// WASAPI resources
		IMMDeviceEnumerator* m_pDeviceEnumerator = nullptr;
		IMMDevice* m_pDevice = nullptr;
		IAudioClient* m_pAudioClient = nullptr;
		IAudioCaptureClient* m_pCaptureClient = nullptr;
		WAVEFORMATEX* m_pWaveFormat = nullptr;
		bool m_bIsFloatFormat = false;
		UINT32 m_nBytesPerInputSample = 0;

		// Capture thread
		std::thread m_captureThread;
		HANDLE m_hCaptureEvent = nullptr;
		HANDLE m_hStopEvent = nullptr;  // For clean shutdown
		std::atomic<bool> m_bCapturing{ false };
		std::atomic<bool> m_bPaused{ false };
		std::atomic<RecordState> m_recordState{ RecordState::stop };

		// Media Foundation encoder (for AAC, FLAC, etc.)
		IMFSinkWriter* m_pSinkWriter = nullptr;
		DWORD m_dwStreamIndex = 0;
		bool m_bMfStarted = false;

		// File output for WAV/PCM
		HANDLE m_hOutputFile = INVALID_HANDLE_VALUE;
		std::atomic<DWORD> m_dwDataWritten{ 0 };

		// Timing
		std::atomic<LONGLONG> m_llSampleTime{ 0 };
		std::atomic<bool> m_bFirstSample{ true };

		// Conversion buffer (reusable to avoid allocations)
		std::vector<int16_t> m_conversionBuffer;

		// Configuration (protected by mutex for thread-safe access)
		mutable CritSec m_configMutex;
		std::unique_ptr<RecordConfig> m_pConfig;
		std::wstring m_recordingPath;

		// Amplitude (atomic for thread-safe reads)
		std::atomic<double> m_amplitude{ -160.0 };
		std::atomic<double> m_maxAmplitude{ -160.0 };

		// Event handlers
		EventStreamHandler<>* m_stateEventHandler;
		EventStreamHandler<>* m_recordEventHandler;

		// Main critical section for cleanup synchronization
		CritSec m_critsec;
	};
}