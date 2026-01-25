#pragma once

#include <string>

namespace record_windows
{

	struct AudioEncoder
	{
		static constexpr const char* aacLc = "aacLc";
		static constexpr const char* aacEld = "aacEld";
		static constexpr const char* aacHe = "aacHe";
		static constexpr const char* amrNb = "amrNb";
		static constexpr const char* amrWb = "amrWb";
		static constexpr const char* opus = "opus";
		static constexpr const char* flac = "flac";
		static constexpr const char* pcm16bits = "pcm16bits";
		static constexpr const char* wav = "wav";
	};

	struct RecordConfig
	{
		std::string encoderName = AudioEncoder::aacLc;
		std::string deviceId;
		int bitRate = 128000;
		int sampleRate = 16000;  // Changed default to 16kHz for better voice quality
		int numChannels = 1;     // Changed default to mono for voice
		bool autoGain = false;
		bool echoCancel = false;
		bool noiseSuppress = false;

		RecordConfig(
			const std::string& encoderName,
			const std::string& deviceId,
			int bitRate,
			int sampleRate,
			int numChannels,
			bool autoGain,
			bool echoCancel,
			bool noiseSuppress)
			: encoderName(encoderName),
			deviceId(deviceId),
			bitRate(bitRate),
			sampleRate(sampleRate),
			numChannels(numChannels),
			autoGain(autoGain),
			echoCancel(echoCancel),
			noiseSuppress(noiseSuppress)
		{
		}
	};
}