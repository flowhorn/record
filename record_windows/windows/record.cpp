#include "record.h"
#include "record_windows_plugin.h"

#include <cmath>
#include <algorithm>
#include <limits>

// For WAV header
#pragma warning(disable: 4201)
#include <aviriff.h>
#include <ks.h>
#include <ksmedia.h>

namespace record_windows
{
	// WAV file header structure
	struct WAV_FILE_HEADER
	{
		RIFFCHUNK FileHeader;
		DWORD fccWaveType;    // must be 'WAVE'
		RIFFCHUNK WaveHeader;
		WAVEFORMATEX WaveFormat;
		RIFFCHUNK DataHeader;
	};

	// static
	HRESULT Recorder::CreateInstance(
		EventStreamHandler<>* stateEventHandler,
		EventStreamHandler<>* recordEventHandler,
		Recorder** ppRecorder)
	{
		if (ppRecorder == nullptr)
		{
			return E_POINTER;
		}

		auto pRecorder = new (std::nothrow) Recorder(stateEventHandler, recordEventHandler);
		if (pRecorder == nullptr)
		{
			return E_OUTOFMEMORY;
		}
		*ppRecorder = pRecorder;
		return S_OK;
	}

	Recorder::Recorder(EventStreamHandler<>* stateEventHandler, EventStreamHandler<>* recordEventHandler)
		: m_stateEventHandler(stateEventHandler),
		m_recordEventHandler(recordEventHandler)
	{
		// Pre-allocate conversion buffer for typical audio chunk (10ms at 48kHz stereo)
		m_conversionBuffer.reserve(480 * 2);
	}

	Recorder::~Recorder()
	{
		Dispose();
	}

	HRESULT Recorder::Start(std::unique_ptr<RecordConfig> config, std::wstring path)
	{
		if (!config)
		{
			return E_INVALIDARG;
		}

		bool supported = false;
		HRESULT hr = isEncoderSupported(config->encoderName, &supported);

		if (FAILED(hr) || !supported)
		{
			return E_NOTIMPL;
		}

		// End any existing recording
		hr = EndRecording();
		if (FAILED(hr)) return hr;

		{
			AutoLock lock(m_configMutex);
			m_pConfig = std::move(config);
			m_recordingPath = path;
		}

		// Initialize WASAPI capture
		std::string deviceId;
		{
			AutoLock lock(m_configMutex);
			if (m_pConfig)
			{
				deviceId = m_pConfig->deviceId;
			}
		}

		hr = InitializeWasapi(deviceId);
		if (FAILED(hr))
		{
			EndRecording();
			return hr;
		}

		// Initialize encoder (for non-PCM formats) or file (for WAV/PCM)
		hr = InitializeEncoder(path);
		if (FAILED(hr))
		{
			EndRecording();
			return hr;
		}

		// Create stop event for clean shutdown
		m_hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (m_hStopEvent == nullptr)
		{
			EndRecording();
			return HRESULT_FROM_WIN32(GetLastError());
		}

		// Start capture thread
		m_bCapturing = true;
		m_bPaused = false;
		m_bFirstSample = true;
		m_llSampleTime = 0;

		try
		{
			m_captureThread = std::thread(&Recorder::CaptureThreadProc, this);
		}
		catch (const std::exception&)
		{
			m_bCapturing = false;
			EndRecording();
			return E_FAIL;
		}

		UpdateState(RecordState::record);
		return S_OK;
	}

	HRESULT Recorder::StartStream(std::unique_ptr<RecordConfig> config)
	{
		if (!config)
		{
			return E_INVALIDARG;
		}

		if (config->encoderName != AudioEncoder::pcm16bits)
		{
			return E_NOTIMPL;
		}

		HRESULT hr = EndRecording();
		if (FAILED(hr)) return hr;

		std::string deviceId;
		{
			AutoLock lock(m_configMutex);
			m_pConfig = std::move(config);
			m_recordingPath.clear();
			if (m_pConfig)
			{
				deviceId = m_pConfig->deviceId;
			}
		}

		// Initialize WASAPI capture
		hr = InitializeWasapi(deviceId);
		if (FAILED(hr))
		{
			EndRecording();
			return hr;
		}

		// Create stop event for clean shutdown
		m_hStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (m_hStopEvent == nullptr)
		{
			EndRecording();
			return HRESULT_FROM_WIN32(GetLastError());
		}

		// Start capture thread (no encoder needed - just stream PCM)
		m_bCapturing = true;
		m_bPaused = false;
		m_bFirstSample = true;
		m_llSampleTime = 0;

		try
		{
			m_captureThread = std::thread(&Recorder::CaptureThreadProc, this);
		}
		catch (const std::exception&)
		{
			m_bCapturing = false;
			EndRecording();
			return E_FAIL;
		}

		UpdateState(RecordState::record);
		return S_OK;
	}

	HRESULT Recorder::InitializeWasapi(const std::string& deviceId)
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE)
		{
			return hr;
		}

		// Create device enumerator
		hr = CoCreateInstance(
			__uuidof(MMDeviceEnumerator),
			nullptr,
			CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator),
			(void**)&m_pDeviceEnumerator);
		if (FAILED(hr)) return hr;

		// Get audio device
		if (deviceId.empty())
		{
			// Use default capture device
			hr = m_pDeviceEnumerator->GetDefaultAudioEndpoint(
				eCapture,
				eConsole,
				&m_pDevice);
		}
		else
		{
			// Use specified device
			std::wstring wDeviceId = Utf16FromUtf8(deviceId);
			hr = m_pDeviceEnumerator->GetDevice(wDeviceId.c_str(), &m_pDevice);
		}
		if (FAILED(hr)) return hr;

		// Activate audio client
		hr = m_pDevice->Activate(
			__uuidof(IAudioClient),
			CLSCTX_ALL,
			nullptr,
			(void**)&m_pAudioClient);
		if (FAILED(hr)) return hr;

		// Get device's native format
		hr = m_pAudioClient->GetMixFormat(&m_pWaveFormat);
		if (FAILED(hr)) return hr;

		// Determine if format is float (WASAPI typically uses float in shared mode)
		m_bIsFloatFormat = false;
		m_nBytesPerInputSample = m_pWaveFormat->wBitsPerSample / 8;

		if (m_pWaveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		{
			m_bIsFloatFormat = true;
		}
		else if (m_pWaveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		{
			WAVEFORMATEXTENSIBLE* pWaveFormatEx = (WAVEFORMATEXTENSIBLE*)m_pWaveFormat;
			if (IsEqualGUID(pWaveFormatEx->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
			{
				m_bIsFloatFormat = true;
			}
		}

		// Request low-latency buffer (10ms)
		REFERENCE_TIME hnsRequestedDuration = 100000; // 10ms in 100ns units

		// Create event for capture notifications
		m_hCaptureEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_hCaptureEvent == nullptr)
		{
			return HRESULT_FROM_WIN32(GetLastError());
		}

		// Initialize audio client in shared mode with event-driven buffering
		hr = m_pAudioClient->Initialize(
			AUDCLNT_SHAREMODE_SHARED,
			AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
			hnsRequestedDuration,
			0,
			m_pWaveFormat,
			nullptr);

		// Handle buffer size alignment issues
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			// Get the aligned buffer size
			UINT32 nFrames = 0;
			hr = m_pAudioClient->GetBufferSize(&nFrames);
			if (FAILED(hr)) return hr;

			// Release and re-create
			SafeRelease(m_pAudioClient);

			hr = m_pDevice->Activate(
				__uuidof(IAudioClient),
				CLSCTX_ALL,
				nullptr,
				(void**)&m_pAudioClient);
			if (FAILED(hr)) return hr;

			// Calculate aligned duration
			hnsRequestedDuration = (REFERENCE_TIME)((10000.0 * 1000 / m_pWaveFormat->nSamplesPerSec * nFrames) + 0.5);

			hr = m_pAudioClient->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				hnsRequestedDuration,
				0,
				m_pWaveFormat,
				nullptr);
		}

		if (FAILED(hr)) return hr;

		// Set event handle
		hr = m_pAudioClient->SetEventHandle(m_hCaptureEvent);
		if (FAILED(hr)) return hr;

		// Get capture client
		hr = m_pAudioClient->GetService(
			__uuidof(IAudioCaptureClient),
			(void**)&m_pCaptureClient);
		if (FAILED(hr)) return hr;

		// Start the audio stream
		hr = m_pAudioClient->Start();
		return hr;
	}

	HRESULT Recorder::InitializeEncoder(const std::wstring& path)
	{
		HRESULT hr = S_OK;

		std::string encoderName;
		{
			AutoLock lock(m_configMutex);
			if (m_pConfig)
			{
				encoderName = m_pConfig->encoderName;
			}
		}

		// For PCM16bits or WAV, write directly to file
		if (encoderName == AudioEncoder::pcm16bits ||
			encoderName == AudioEncoder::wav)
		{
			m_hOutputFile = CreateFileW(
				path.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,  // Optimize for sequential writes
				nullptr);

			if (m_hOutputFile == INVALID_HANDLE_VALUE)
			{
				return HRESULT_FROM_WIN32(GetLastError());
			}

			// For WAV, write placeholder header - we'll update it at the end
			if (encoderName == AudioEncoder::wav)
			{
				DWORD bytesWritten;
				WAV_FILE_HEADER header = {};
				if (!WriteFile(m_hOutputFile, &header, sizeof(header), &bytesWritten, nullptr))
				{
					return HRESULT_FROM_WIN32(GetLastError());
				}
			}

			m_dwDataWritten = 0;
			return S_OK;
		}

		// For other formats, use Media Foundation sink writer
		hr = MFStartup(MF_VERSION);
		if (FAILED(hr)) return hr;
		m_bMfStarted = true;

		// Create sink writer with low-latency attributes
		IMFAttributes* pAttributes = nullptr;
		hr = MFCreateAttributes(&pAttributes, 1);
		if (SUCCEEDED(hr))
		{
			// Enable low-latency mode for real-time encoding
			hr = pAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
		}

		if (SUCCEEDED(hr))
		{
			hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, pAttributes, &m_pSinkWriter);
		}
		SafeRelease(&pAttributes);

		if (FAILED(hr)) return hr;

		// Get config values thread-safely
		int sampleRate = 16000;
		int numChannels = 1;
		int bitRate = 128000;
		{
			AutoLock lock(m_configMutex);
			if (m_pConfig)
			{
				sampleRate = m_pConfig->sampleRate;
				numChannels = m_pConfig->numChannels;
				bitRate = m_pConfig->bitRate;
				encoderName = m_pConfig->encoderName;
			}
		}

		// Create output media type
		IMFMediaType* pOutputType = nullptr;
		hr = MFCreateMediaType(&pOutputType);
		if (FAILED(hr)) return hr;

		hr = pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		if (SUCCEEDED(hr))
		{
			if (encoderName == AudioEncoder::aacLc ||
				encoderName == AudioEncoder::aacEld ||
				encoderName == AudioEncoder::aacHe)
			{
				hr = pOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
			}
			else if (encoderName == AudioEncoder::flac)
			{
				hr = pOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_FLAC);
			}
			else if (encoderName == AudioEncoder::opus)
			{
				hr = pOutputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Opus);
			}
			else
			{
				SafeRelease(&pOutputType);
				return E_NOTIMPL;
			}
		}

		if (SUCCEEDED(hr))
		{
			hr = pOutputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		}
		if (SUCCEEDED(hr))
		{
			hr = pOutputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
		}
		if (SUCCEEDED(hr))
		{
			hr = pOutputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, numChannels);
		}
		if (SUCCEEDED(hr))
		{
			hr = pOutputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitRate / 8);
		}

		if (SUCCEEDED(hr))
		{
			hr = m_pSinkWriter->AddStream(pOutputType, &m_dwStreamIndex);
		}

		// Create input media type (PCM - we convert WASAPI float to PCM16)
		IMFMediaType* pInputType = nullptr;
		if (SUCCEEDED(hr))
		{
			hr = MFCreateMediaType(&pInputType);
		}
		if (SUCCEEDED(hr))
		{
			hr = pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		}
		if (SUCCEEDED(hr))
		{
			hr = pInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		}
		if (SUCCEEDED(hr))
		{
			hr = pInputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		}
		if (SUCCEEDED(hr) && m_pWaveFormat)
		{
			hr = pInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_pWaveFormat->nSamplesPerSec);
		}
		if (SUCCEEDED(hr) && m_pWaveFormat)
		{
			hr = pInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_pWaveFormat->nChannels);
		}
		if (SUCCEEDED(hr) && m_pWaveFormat)
		{
			UINT32 blockAlign = m_pWaveFormat->nChannels * 2; // 16 bits = 2 bytes
			hr = pInputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
		}
		if (SUCCEEDED(hr) && m_pWaveFormat)
		{
			UINT32 bytesPerSec = m_pWaveFormat->nSamplesPerSec * m_pWaveFormat->nChannels * 2;
			hr = pInputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
		}

		if (SUCCEEDED(hr))
		{
			hr = m_pSinkWriter->SetInputMediaType(m_dwStreamIndex, pInputType, nullptr);
		}

		if (SUCCEEDED(hr))
		{
			hr = m_pSinkWriter->BeginWriting();
		}

		SafeRelease(&pOutputType);
		SafeRelease(&pInputType);

		return hr;
	}

	void Recorder::CaptureThreadProc()
	{
		// Initialize COM for this thread
		HRESULT hrCoInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		// Raise thread priority for real-time audio
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

		HANDLE waitHandles[2] = { m_hCaptureEvent, m_hStopEvent };
		const DWORD numHandles = m_hStopEvent ? 2 : 1;

		while (m_bCapturing)
		{
			// Wait for capture event, stop event, or timeout
			DWORD waitResult = WaitForMultipleObjects(numHandles, waitHandles, FALSE, 50);

			if (!m_bCapturing) break;

			// Stop event signaled
			if (waitResult == WAIT_OBJECT_0 + 1)
			{
				break;
			}

			// Skip processing if paused (but keep draining buffers)
			bool bPaused = m_bPaused;

			// Process available packets
			if (m_pCaptureClient)
			{
				UINT32 packetLength = 0;
				HRESULT hr = m_pCaptureClient->GetNextPacketSize(&packetLength);

				while (SUCCEEDED(hr) && packetLength > 0 && m_bCapturing)
				{
					BYTE* pData = nullptr;
					UINT32 numFramesAvailable = 0;
					DWORD flags = 0;
					UINT64 devicePosition = 0;
					UINT64 qpcPosition = 0;

					hr = m_pCaptureClient->GetBuffer(
						&pData,
						&numFramesAvailable,
						&flags,
						&devicePosition,
						&qpcPosition);

					if (SUCCEEDED(hr))
					{
						// Always process to keep buffers drained, but only save if not paused
						if (!bPaused && numFramesAvailable > 0)
						{
							ProcessCapturedData(pData, numFramesAvailable, flags);
						}

						hr = m_pCaptureClient->ReleaseBuffer(numFramesAvailable);
					}

					if (SUCCEEDED(hr))
					{
						hr = m_pCaptureClient->GetNextPacketSize(&packetLength);
					}
					else
					{
						// Error occurred, break inner loop
						break;
					}
				}
			}
		}

		if (SUCCEEDED(hrCoInit))
		{
			CoUninitialize();
		}
	}

	void Recorder::ProcessCapturedData(BYTE* pData, UINT32 numFrames, DWORD flags)
	{
		if (m_pWaveFormat == nullptr || pData == nullptr || numFrames == 0)
		{
			return;
		}

		// Handle silent buffers
		if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
		{
			// For silent buffers, write zeros
			UINT32 numSamples = numFrames * m_pWaveFormat->nChannels;
			m_conversionBuffer.resize(numSamples);
			std::fill(m_conversionBuffer.begin(), m_conversionBuffer.end(), 0);
		}
		else
		{
			// Convert to PCM16 if needed
			UINT32 numSamples = numFrames * m_pWaveFormat->nChannels;

			if (m_bIsFloatFormat && m_nBytesPerInputSample == 4)
			{
				// Convert 32-bit float to 16-bit PCM
				m_conversionBuffer.resize(numSamples);
				ConvertFloatToPcm16((const float*)pData, m_conversionBuffer.data(), numSamples);
			}
			else if (!m_bIsFloatFormat && m_nBytesPerInputSample == 4)
			{
				// Convert 32-bit PCM to 16-bit PCM
				m_conversionBuffer.resize(numSamples);
				ConvertPcm32ToPcm16((const int32_t*)pData, m_conversionBuffer.data(), numSamples);
			}
			else if (!m_bIsFloatFormat && m_nBytesPerInputSample == 2)
			{
				// Already 16-bit PCM, just copy
				m_conversionBuffer.resize(numSamples);
				memcpy(m_conversionBuffer.data(), pData, numSamples * 2);
			}
			else
			{
				// Unsupported format, skip
				return;
			}
		}

		// Calculate output size
		UINT32 numBytes = (UINT32)(m_conversionBuffer.size() * sizeof(int16_t));
		const BYTE* pOutputData = (const BYTE*)m_conversionBuffer.data();

		// Calculate timestamp
		LONGLONG timestamp = m_llSampleTime.load();
		if (m_bFirstSample.exchange(false))
		{
			timestamp = 0;
		}

		// Update sample time for next buffer
		double duration = (double)numFrames / m_pWaveFormat->nSamplesPerSec;
		m_llSampleTime += (LONGLONG)(duration * 10000000); // Convert to 100ns units

		// Calculate amplitude
		CalculateAmplitude(pOutputData, numBytes);

		// Get recording path thread-safely
		std::wstring recordingPath;
		std::string encoderName;
		{
			AutoLock lock(m_configMutex);
			recordingPath = m_recordingPath;
			if (m_pConfig)
			{
				encoderName = m_pConfig->encoderName;
			}
		}

		// Write to output (file or stream)
		if (!recordingPath.empty())
		{
			// Writing to file
			if (encoderName == AudioEncoder::pcm16bits ||
				encoderName == AudioEncoder::wav)
			{
				WritePcmToFile(pOutputData, numBytes);
			}
			else if (m_pSinkWriter)
			{
				WriteToEncoder(pOutputData, numBytes, timestamp);
			}
		}
		else if (m_recordEventHandler)
		{
			// Streaming mode - send data to Flutter
			std::vector<uint8_t> bytes(pOutputData, pOutputData + numBytes);

			EventStreamHandler<>* handler = m_recordEventHandler;
			RecordWindowsPlugin::RunOnMainThread([handler, bytes = std::move(bytes)]() -> void {
				if (handler) {
					handler->Success(std::make_unique<flutter::EncodableValue>(bytes));
				}
			});
		}
	}

	void Recorder::ConvertFloatToPcm16(const float* pInput, int16_t* pOutput, UINT32 numSamples)
	{
		for (UINT32 i = 0; i < numSamples; i++)
		{
			// Clamp to [-1.0, 1.0] and convert to 16-bit
			float sample = pInput[i];
			sample = std::max(-1.0f, std::min(1.0f, sample));
			pOutput[i] = static_cast<int16_t>(sample * 32767.0f);
		}
	}

	void Recorder::ConvertPcm32ToPcm16(const int32_t* pInput, int16_t* pOutput, UINT32 numSamples)
	{
		for (UINT32 i = 0; i < numSamples; i++)
		{
			// Shift right by 16 bits (32-bit to 16-bit)
			pOutput[i] = static_cast<int16_t>(pInput[i] >> 16);
		}
	}

	HRESULT Recorder::WritePcmToFile(const BYTE* pData, UINT32 numBytes)
	{
		if (m_hOutputFile == INVALID_HANDLE_VALUE)
		{
			return E_HANDLE;
		}

		DWORD bytesWritten = 0;
		if (!WriteFile(m_hOutputFile, pData, numBytes, &bytesWritten, nullptr))
		{
			return HRESULT_FROM_WIN32(GetLastError());
		}

		m_dwDataWritten += bytesWritten;
		return S_OK;
	}

	HRESULT Recorder::WriteToEncoder(const BYTE* pData, UINT32 numBytes, LONGLONG timestamp)
	{
		if (m_pSinkWriter == nullptr)
		{
			return E_POINTER;
		}

		// Create sample
		IMFSample* pSample = nullptr;
		IMFMediaBuffer* pBuffer = nullptr;
		BYTE* pBufferData = nullptr;

		HRESULT hr = MFCreateMemoryBuffer(numBytes, &pBuffer);
		if (SUCCEEDED(hr))
		{
			hr = pBuffer->Lock(&pBufferData, nullptr, nullptr);
		}
		if (SUCCEEDED(hr))
		{
			memcpy(pBufferData, pData, numBytes);
			pBuffer->Unlock();
			hr = pBuffer->SetCurrentLength(numBytes);
		}
		if (SUCCEEDED(hr))
		{
			hr = MFCreateSample(&pSample);
		}
		if (SUCCEEDED(hr))
		{
			hr = pSample->AddBuffer(pBuffer);
		}
		if (SUCCEEDED(hr))
		{
			hr = pSample->SetSampleTime(timestamp);
		}
		if (SUCCEEDED(hr) && m_pWaveFormat)
		{
			// Calculate duration based on bytes and format
			UINT32 bytesPerSample = m_pWaveFormat->nChannels * 2; // 16-bit PCM output
			UINT32 numFrames = numBytes / bytesPerSample;
			LONGLONG duration = (LONGLONG)numFrames * 10000000 / m_pWaveFormat->nSamplesPerSec;
			hr = pSample->SetSampleDuration(duration);
		}
		if (SUCCEEDED(hr))
		{
			hr = m_pSinkWriter->WriteSample(m_dwStreamIndex, pSample);
		}

		m_dwDataWritten += numBytes;

		SafeRelease(&pBuffer);
		SafeRelease(&pSample);

		return hr;
	}

	HRESULT Recorder::FinalizeWavHeader()
	{
		if (m_hOutputFile == INVALID_HANDLE_VALUE)
		{
			return E_HANDLE;
		}

		DWORD dataWritten = m_dwDataWritten.load();
		if (dataWritten == 0)
		{
			return S_OK;  // Nothing to write
		}

		// Build WAV header
		WAV_FILE_HEADER header = {};
		DWORD cbFileSize = dataWritten + sizeof(WAV_FILE_HEADER) - sizeof(RIFFCHUNK);

		header.FileHeader.fcc = MAKEFOURCC('R', 'I', 'F', 'F');
		header.FileHeader.cb = cbFileSize;
		header.fccWaveType = MAKEFOURCC('W', 'A', 'V', 'E');
		header.WaveHeader.fcc = MAKEFOURCC('f', 'm', 't', ' ');
		header.WaveHeader.cb = sizeof(WAVEFORMATEX);

		// We output 16-bit PCM regardless of input format
		if (m_pWaveFormat)
		{
			header.WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
			header.WaveFormat.nChannels = m_pWaveFormat->nChannels;
			header.WaveFormat.nSamplesPerSec = m_pWaveFormat->nSamplesPerSec;
			header.WaveFormat.wBitsPerSample = 16;
			header.WaveFormat.nBlockAlign = m_pWaveFormat->nChannels * 2;
			header.WaveFormat.nAvgBytesPerSec = m_pWaveFormat->nSamplesPerSec * header.WaveFormat.nBlockAlign;
			header.WaveFormat.cbSize = 0;
		}

		header.DataHeader.fcc = MAKEFOURCC('d', 'a', 't', 'a');
		header.DataHeader.cb = dataWritten;

		// Seek to beginning and write header
		if (SetFilePointer(m_hOutputFile, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
		{
			return HRESULT_FROM_WIN32(GetLastError());
		}

		DWORD bytesWritten = 0;
		if (!WriteFile(m_hOutputFile, &header, sizeof(header), &bytesWritten, nullptr))
		{
			return HRESULT_FROM_WIN32(GetLastError());
		}

		return S_OK;
	}

	void Recorder::CalculateAmplitude(const BYTE* pData, UINT32 numBytes)
	{
		if (pData == nullptr || numBytes == 0)
		{
			return;
		}

		// Data is always 16-bit PCM after conversion
		const int16_t* samples = (const int16_t*)pData;
		UINT32 numSamples = numBytes / 2;

		int32_t maxSample = 0;
		for (UINT32 i = 0; i < numSamples; i++)
		{
			int32_t absSample = std::abs((int32_t)samples[i]);
			if (absSample > maxSample)
			{
				maxSample = absSample;
			}
		}

		// Avoid log(0)
		double newAmplitude = -160.0;
		if (maxSample > 0)
		{
			newAmplitude = 20.0 * std::log10((double)maxSample / 32767.0);
		}

		m_amplitude = newAmplitude;

		double currentMax = m_maxAmplitude.load();
		while (newAmplitude > currentMax)
		{
			if (m_maxAmplitude.compare_exchange_weak(currentMax, newAmplitude))
			{
				break;
			}
		}
	}

	HRESULT Recorder::Pause()
	{
		if (m_recordState == RecordState::record)
		{
			m_bPaused = true;
			UpdateState(RecordState::pause);
		}
		return S_OK;
	}

	HRESULT Recorder::Resume()
	{
		if (m_recordState == RecordState::pause)
		{
			m_bPaused = false;
			UpdateState(RecordState::record);
		}
		return S_OK;
	}

	HRESULT Recorder::Stop()
	{
		if (m_dwDataWritten == 0)
		{
			return Cancel();
		}

		HRESULT hr = EndRecording();
		if (SUCCEEDED(hr))
		{
			UpdateState(RecordState::stop);
		}
		return hr;
	}

	HRESULT Recorder::Cancel()
	{
		std::wstring recordingPath;
		{
			AutoLock lock(m_configMutex);
			recordingPath = m_recordingPath;
		}

		HRESULT hr = EndRecording();

		if (SUCCEEDED(hr))
		{
			UpdateState(RecordState::stop);

			if (!recordingPath.empty())
			{
				DeleteFileW(recordingPath.c_str());
			}
		}
		return hr;
	}

	bool Recorder::IsPaused()
	{
		return m_recordState == RecordState::pause;
	}

	bool Recorder::IsRecording()
	{
		return m_recordState == RecordState::record;
	}

	HRESULT Recorder::EndRecording()
	{
		AutoLock lock(m_critsec);

		// Signal stop event first
		if (m_hStopEvent)
		{
			SetEvent(m_hStopEvent);
		}

		// Stop capture flag
		m_bCapturing = false;

		// Wait for capture thread to finish
		if (m_captureThread.joinable())
		{
			m_captureThread.join();
		}

		// Finalize encoder
		if (m_pSinkWriter)
		{
			m_pSinkWriter->Finalize();
		}

		// Finalize WAV header
		std::string encoderName;
		{
			AutoLock configLock(m_configMutex);
			if (m_pConfig)
			{
				encoderName = m_pConfig->encoderName;
			}
		}

		if (encoderName == AudioEncoder::wav)
		{
			FinalizeWavHeader();
		}

		// Close file
		if (m_hOutputFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_hOutputFile);
			m_hOutputFile = INVALID_HANDLE_VALUE;
		}

		// Close stop event
		if (m_hStopEvent)
		{
			CloseHandle(m_hStopEvent);
			m_hStopEvent = nullptr;
		}

		// Cleanup WASAPI
		CleanupWasapi();

		// Cleanup encoder
		CleanupEncoder();

		// Reset state
		m_bFirstSample = true;
		m_llSampleTime = 0;
		m_dwDataWritten = 0;
		m_amplitude = -160.0;
		m_maxAmplitude = -160.0;
		m_conversionBuffer.clear();

		{
			AutoLock configLock(m_configMutex);
			m_pConfig = nullptr;
			m_recordingPath.clear();
		}

		return S_OK;
	}

	void Recorder::CleanupWasapi()
	{
		if (m_pAudioClient)
		{
			m_pAudioClient->Stop();
		}

		SafeRelease(m_pCaptureClient);
		SafeRelease(m_pAudioClient);
		SafeRelease(m_pDevice);
		SafeRelease(m_pDeviceEnumerator);

		if (m_pWaveFormat)
		{
			CoTaskMemFree(m_pWaveFormat);
			m_pWaveFormat = nullptr;
		}

		if (m_hCaptureEvent)
		{
			CloseHandle(m_hCaptureEvent);
			m_hCaptureEvent = nullptr;
		}

		m_bIsFloatFormat = false;
		m_nBytesPerInputSample = 0;
	}

	void Recorder::CleanupEncoder()
	{
		SafeRelease(m_pSinkWriter);

		if (m_bMfStarted)
		{
			MFShutdown();
			m_bMfStarted = false;
		}

		m_dwStreamIndex = 0;
	}

	HRESULT Recorder::Dispose()
	{
		HRESULT hr = EndRecording();
		m_stateEventHandler = nullptr;
		m_recordEventHandler = nullptr;
		return hr;
	}

	void Recorder::UpdateState(RecordState state)
	{
		m_recordState = state;

		EventStreamHandler<>* handlerPtr = m_stateEventHandler;
		if (handlerPtr)
		{
			RecordWindowsPlugin::RunOnMainThread([handlerPtr, state]() -> void {
				if (handlerPtr) {
					handlerPtr->Success(std::make_unique<flutter::EncodableValue>(static_cast<int>(state)));
				}
			});
		}
	}

	std::map<std::string, double> Recorder::GetAmplitude()
	{
		return {
			{"current", m_amplitude.load()},
			{"max", m_maxAmplitude.load()},
		};
	}

	std::wstring Recorder::GetRecordingPath()
	{
		AutoLock lock(m_configMutex);
		return m_recordingPath;
	}

	HRESULT Recorder::isEncoderSupported(std::string encoderName, bool* supported)
	{
		if (supported == nullptr)
		{
			return E_POINTER;
		}

		*supported = false;

		// Always support PCM and WAV
		if (encoderName == AudioEncoder::pcm16bits || encoderName == AudioEncoder::wav)
		{
			*supported = true;
			return S_OK;
		}

		// Check Media Foundation encoder availability
		MFT_REGISTER_TYPE_INFO typeLookup = {};
		typeLookup.guidMajorType = MFMediaType_Audio;

		if (encoderName == AudioEncoder::aacLc ||
			encoderName == AudioEncoder::aacEld ||
			encoderName == AudioEncoder::aacHe)
		{
			typeLookup.guidSubtype = MFAudioFormat_AAC;
		}
		else if (encoderName == AudioEncoder::flac)
		{
			typeLookup.guidSubtype = MFAudioFormat_FLAC;
		}
		else if (encoderName == AudioEncoder::opus)
		{
			typeLookup.guidSubtype = MFAudioFormat_Opus;
		}
		else if (encoderName == AudioEncoder::amrNb)
		{
			typeLookup.guidSubtype = MFAudioFormat_AMR_NB;
		}
		else if (encoderName == AudioEncoder::amrWb)
		{
			typeLookup.guidSubtype = MFAudioFormat_AMR_WB;
		}
		else
		{
			return S_OK; // Not supported
		}

		HRESULT hr = MFStartup(MF_VERSION);
		if (FAILED(hr)) return hr;

		DWORD dwFlags = (MFT_ENUM_FLAG_ALL & (~MFT_ENUM_FLAG_FIELDOFUSE)) | MFT_ENUM_FLAG_SORTANDFILTER;
		IMFActivate** ppMFTActivate = nullptr;
		UINT32 numMFTActivate = 0;

		hr = MFTEnumEx(
			MFT_CATEGORY_AUDIO_ENCODER,
			dwFlags,
			nullptr,
			&typeLookup,
			&ppMFTActivate,
			&numMFTActivate);

		if (SUCCEEDED(hr))
		{
			*supported = numMFTActivate > 0;
		}

		for (UINT32 i = 0; i < numMFTActivate; i++)
		{
			SafeRelease(ppMFTActivate[i]);
		}
		CoTaskMemFree(ppMFTActivate);

		MFShutdown();
		return hr;
	}
};
