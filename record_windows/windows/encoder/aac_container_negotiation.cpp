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

std::vector<uint32_t> BuildAacSampleRateCandidates(int requestedSampleRate) {
    // Practical AAC-LC rates for voice/music pipelines on Windows MF.
    const std::vector<uint32_t> supported = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
    };

    uint32_t requested = static_cast<uint32_t>(requestedSampleRate <= 0 ? 16000 : requestedSampleRate);
    if (requested < supported.front()) requested = supported.front();
    if (requested > supported.back()) requested = supported.back();

    std::vector<uint32_t> candidates;
    candidates.reserve(supported.size() + 1);
    candidates.push_back(requested);
    candidates.insert(candidates.end(), supported.begin(), supported.end());

    std::vector<uint32_t> uniqueCandidates;
    uniqueCandidates.reserve(candidates.size());
    for (uint32_t v : candidates) {
        if (std::find(uniqueCandidates.begin(), uniqueCandidates.end(), v) == uniqueCandidates.end()) {
            uniqueCandidates.push_back(v);
        }
    }

    // Nearest rate first. For ties, prefer lower rate to limit bandwidth.
    std::stable_sort(uniqueCandidates.begin(), uniqueCandidates.end(), [requested](uint32_t a, uint32_t b) {
        const int da = std::abs((int)a - (int)requested);
        const int db = std::abs((int)b - (int)requested);
        if (da == db) return a < b;
        return da < db;
    });

    return uniqueCandidates;
}

std::vector<uint32_t> BuildAacChannelCandidates(int requestedChannels) {
    if (requestedChannels <= 1) {
        return {1, 2};
    }
    return {2, 1};
}

} // namespace record_windows
