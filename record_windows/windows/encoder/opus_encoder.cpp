#include "opus_encoder.h"
#include <cstring>
#include <random>
#include <algorithm>

namespace record_windows {

// Ogg Opus specification constants
static const uint8_t OPUS_HEAD_MAGIC[] = "OpusHead";
static const uint8_t OPUS_TAGS_MAGIC[] = "OpusTags";

OpusAudioEncoder::OpusAudioEncoder()
    : m_encodeBuffer(4000) {  // Max Opus frame size
}

OpusAudioEncoder::~OpusAudioEncoder() {
    Finalize();
}

bool OpusAudioEncoder::Initialize(const std::wstring& path, int sampleRate, int channels, int bitrate) {
    if (m_initialized) {
        Finalize();
    }

    m_sampleRate = sampleRate;
    m_channels = channels;
    m_bitrate = bitrate;
    
    // Calculate frame size for 20ms
    m_frameSize = sampleRate * 20 / 1000;

    // Create Opus encoder
    int error = 0;
    m_encoder = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !m_encoder) {
        return false;
    }

    // Configure encoder for low-latency voice
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));  // Enable VBR
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR_CONSTRAINT(0));  // Unconstrained VBR
    opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));  // Medium complexity
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));  // Disable DTX for continuous stream
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));  // Enable FEC for robustness

    // Get encoder lookahead for correct pre-skip
    int lookahead = 0;
    if (opus_encoder_ctl(m_encoder, OPUS_GET_LOOKAHEAD(&lookahead)) == OPUS_OK) {
        m_preSkip = lookahead;
    } else {
        m_preSkip = 0;
    }

    // Open output file
    m_file.open(path, std::ios::binary);
    if (!m_file.is_open()) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
        return false;
    }

    // Initialize Ogg stream with random serial number
    std::random_device rd;
    int serialNo = static_cast<int>(rd());
    if (ogg_stream_init(&m_oggStream, serialNo) != 0) {
        m_file.close();
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
        return false;
    }

    m_granulePos = 0;
    m_packetNo = 0;
    m_headerWritten = false;
    m_initialized = true;

    // Write Ogg Opus headers
    if (!WriteOggHeader() || !WriteOggTags()) {
        Finalize();
        return false;
    }

    m_headerWritten = true;
    return true;
}

bool OpusAudioEncoder::WriteOggHeader() {
    // OpusHead packet (RFC 7845)
    std::vector<uint8_t> header(19);
    size_t pos = 0;

    // Magic signature "OpusHead"
    memcpy(header.data() + pos, OPUS_HEAD_MAGIC, 8);
    pos += 8;

    // Version (1)
    header[pos++] = 1;

    // Channel count
    header[pos++] = static_cast<uint8_t>(m_channels);

    // Pre-skip (samples) - use encoder lookahead
    int preskip = m_preSkip;
    if (preskip < 0) preskip = 0;
    if (preskip > 0xFFFF) preskip = 0xFFFF;
    uint16_t preskip16 = static_cast<uint16_t>(preskip);
    header[pos++] = preskip16 & 0xFF;
    header[pos++] = (preskip16 >> 8) & 0xFF;

    // Input sample rate (informational only, stored as little-endian)
    header[pos++] = m_sampleRate & 0xFF;
    header[pos++] = (m_sampleRate >> 8) & 0xFF;
    header[pos++] = (m_sampleRate >> 16) & 0xFF;
    header[pos++] = (m_sampleRate >> 24) & 0xFF;

    // Output gain (0 dB)
    header[pos++] = 0;
    header[pos++] = 0;

    // Channel mapping family (0 = mono/stereo, no mapping table needed)
    header[pos++] = 0;

    ogg_packet op;
    op.packet = header.data();
    op.bytes = static_cast<long>(header.size());
    op.b_o_s = 1;  // Beginning of stream
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = m_packetNo++;

    if (ogg_stream_packetin(&m_oggStream, &op) != 0) {
        return false;
    }

    return FlushOggPage(true);
}

bool OpusAudioEncoder::WriteOggTags() {
    // OpusTags packet (RFC 7845)
    const char* vendor = "record_windows";
    size_t vendorLen = strlen(vendor);

    std::vector<uint8_t> tags(8 + 4 + vendorLen + 4);
    size_t pos = 0;

    // Magic signature "OpusTags"
    memcpy(tags.data() + pos, OPUS_TAGS_MAGIC, 8);
    pos += 8;

    // Vendor string length (little-endian)
    tags[pos++] = vendorLen & 0xFF;
    tags[pos++] = (vendorLen >> 8) & 0xFF;
    tags[pos++] = (vendorLen >> 16) & 0xFF;
    tags[pos++] = (vendorLen >> 24) & 0xFF;

    // Vendor string
    memcpy(tags.data() + pos, vendor, vendorLen);
    pos += vendorLen;

    // User comment list length (0 comments)
    tags[pos++] = 0;
    tags[pos++] = 0;
    tags[pos++] = 0;
    tags[pos++] = 0;

    ogg_packet op;
    op.packet = tags.data();
    op.bytes = static_cast<long>(tags.size());
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = m_packetNo++;

    if (ogg_stream_packetin(&m_oggStream, &op) != 0) {
        return false;
    }

    return FlushOggPage(true);
}

bool OpusAudioEncoder::EncodeFrame(const int16_t* pcm, int frameSize) {
    if (!m_initialized || !m_headerWritten) {
        return false;
    }

    // Encode PCM to Opus
    int encodedBytes = opus_encode(m_encoder, pcm, frameSize, 
                                    m_encodeBuffer.data(), 
                                    static_cast<opus_int32>(m_encodeBuffer.size()));
    
    if (encodedBytes < 0) {
        return false;
    }

    // Update granule position
    m_granulePos += frameSize;

    // Create Ogg packet
    ogg_packet op;
    op.packet = m_encodeBuffer.data();
    op.bytes = encodedBytes;
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.granulepos = m_granulePos;
    op.packetno = m_packetNo++;

    if (ogg_stream_packetin(&m_oggStream, &op) != 0) {
        return false;
    }

    return FlushOggPage(false);
}

bool OpusAudioEncoder::FlushOggPage(bool force) {
    ogg_page og;
    int result;

    while (true) {
        if (force) {
            result = ogg_stream_flush(&m_oggStream, &og);
        } else {
            result = ogg_stream_pageout(&m_oggStream, &og);
        }

        if (result == 0) {
            break;
        }

        m_file.write(reinterpret_cast<char*>(og.header), og.header_len);
        m_file.write(reinterpret_cast<char*>(og.body), og.body_len);

        if (!m_file.good()) {
            return false;
        }
    }

    return true;
}

void OpusAudioEncoder::Finalize() {
    if (!m_initialized) {
        return;
    }

    // Write end-of-stream packet if we have written the header
    if (m_headerWritten && m_file.is_open()) {
        // Create empty EOS packet
        uint8_t emptyPacket[1] = {0};
        ogg_packet op;
        op.packet = emptyPacket;
        op.bytes = 0;
        op.b_o_s = 0;
        op.e_o_s = 1;  // End of stream
        op.granulepos = m_granulePos;
        op.packetno = m_packetNo++;

        ogg_stream_packetin(&m_oggStream, &op);
        FlushOggPage(true);
    }

    // Clean up
    ogg_stream_clear(&m_oggStream);

    if (m_file.is_open()) {
        m_file.close();
    }

    if (m_encoder) {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }

    m_initialized = false;
    m_headerWritten = false;
}

} // namespace record_windows
