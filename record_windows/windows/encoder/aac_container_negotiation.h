#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace record_windows {

bool IsAacContainerPathCompatible(const std::wstring& path);

std::vector<uint32_t> BuildAacBitrateCandidates(
    int requestedBitrate,
    int sampleRate,
    int channels);

std::vector<uint32_t> BuildAacSampleRateCandidates(int requestedSampleRate);

std::vector<uint32_t> BuildAacChannelCandidates(int requestedChannels);

} // namespace record_windows
