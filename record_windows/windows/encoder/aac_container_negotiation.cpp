#include "aac_container_negotiation.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

namespace record_windows {

static bool EndsWithCaseInsensitive(const std::wstring& value, const wchar_t* suffix) {
    const size_t suffixLen = wcslen(suffix);
    if (value.length() < suffixLen) {
        return false;
    }

    const size_t offset = value.length() - suffixLen;
    for (size_t i = 0; i < suffixLen; ++i) {
        if (std::towlower(value[offset + i]) != std::towlower(suffix[i])) {
            return false;
        }
    }

    return true;
}

bool IsAacContainerPathCompatible(const std::wstring& path) {
    return EndsWithCaseInsensitive(path, L".m4a") || EndsWithCaseInsensitive(path, L".mp4");
}

std::vector<uint32_t> BuildAacBitrateCandidates(int requestedBitrate, int sampleRate, int channels) {
    // Common AAC-LC target bitrates in bits/sec, sorted low->high.
    const std::vector<uint32_t> common = {
        16000, 24000, 32000, 40000, 48000, 56000, 64000, 80000,
        96000, 112000, 128000, 160000, 192000, 224000, 256000, 320000
    };

    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;

    uint32_t minBitrate = (channels == 1) ? 16000 : 24000;
    uint32_t maxBitrate = (channels == 1) ? 192000 : 320000;

    // Keep low sample rate AAC in ranges that are broadly accepted by MF codecs.
    if (sampleRate <= 16000) {
        maxBitrate = (channels == 1) ? 64000 : 96000;
    } else if (sampleRate <= 24000) {
        maxBitrate = (channels == 1) ? 96000 : 128000;
    } else if (sampleRate <= 32000) {
        maxBitrate = (channels == 1) ? 128000 : 192000;
    }

    uint32_t requested = static_cast<uint32_t>(requestedBitrate <= 0 ? 64000 : requestedBitrate);
    requested = std::max(minBitrate, std::min(maxBitrate, requested));

    std::vector<uint32_t> candidates;
    candidates.reserve(common.size() + 1);
    candidates.push_back(requested);

    for (uint32_t v : common) {
        if (v >= minBitrate && v <= maxBitrate) {
            candidates.push_back(v);
        }
    }

    // Remove duplicates while preserving the first (requested) entry.
    std::vector<uint32_t> uniqueCandidates;
    uniqueCandidates.reserve(candidates.size());
    for (uint32_t v : candidates) {
        if (std::find(uniqueCandidates.begin(), uniqueCandidates.end(), v) == uniqueCandidates.end()) {
            uniqueCandidates.push_back(v);
        }
    }

    // Try nearest bitrate to requested first.
    std::stable_sort(uniqueCandidates.begin(), uniqueCandidates.end(), [requested](uint32_t a, uint32_t b) {
        return std::abs((int)a - (int)requested) < std::abs((int)b - (int)requested);
    });

    return uniqueCandidates;
}

} // namespace record_windows
