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

} // namespace record_windows
