#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <opus/opus.h>
#include <ogg/ogg.h>
#include <fstream>

namespace record_windows {

/**
 * Opus audio encoder with Ogg container output.
 * Configured for voice/VOIP with low latency settings.
 */
class OpusAudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder();

    // Disable copy
    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    /**
     * Initialize the encoder for file output.
     * @param path Output file path
     * @param sampleRate Audio sample rate (should be 48000 for Opus native)
     * @param channels Number of audio channels (1 = mono, 2 = stereo)
     * @param bitrate Target bitrate in bits per second (e.g., 24000)
     * @return true on success
     */
    bool Initialize(const std::wstring& path, int sampleRate, int channels, int bitrate);

    /**
     * Encode a frame of PCM audio.
     * @param pcm Pointer to interleaved 16-bit PCM samples
     * @param frameSize Number of samples per channel (should be 960 for 20ms @ 48kHz)
     * @return true on success
     */
    bool EncodeFrame(const int16_t* pcm, int frameSize);

    /**
     * Finalize encoding and close the file.
     */
    void Finalize();

    /**
     * Check if the encoder is initialized.
     */
    bool IsInitialized() const { return m_initialized; }

    /**
     * Get the recommended frame size for the current sample rate.
     * For 20ms frames: 48000Hz -> 960 samples, 16000Hz -> 320 samples
     */
    int GetFrameSize() const { return m_frameSize; }

private:
    bool WriteOggHeader();
    bool WriteOggTags();
    bool FlushOggPage(bool force = false);

    OpusEncoder* m_encoder = nullptr;
    ogg_stream_state m_oggStream;
    std::ofstream m_file;
    
    int m_sampleRate = 48000;
    int m_channels = 1;
    int m_bitrate = 24000;
    int m_frameSize = 960;  // 20ms at 48kHz
    
    int64_t m_granulePos = 0;
    int m_packetNo = 0;
    bool m_initialized = false;
    bool m_headerWritten = false;
    
    // Buffer for encoded data
    std::vector<uint8_t> m_encodeBuffer;
};

} // namespace record_windows
