#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include <algorithm>

namespace record_windows {

/**
 * Lock-free Single-Producer Single-Consumer ring buffer for audio samples.
 * Designed to pass audio data from the capture thread to the encoder thread.
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_buffer(capacity),
          m_capacity(capacity),
          m_writePos(0),
          m_readPos(0) {
    }

    /**
     * Write samples to the buffer. Called from the audio capture thread.
     * @param data Pointer to the samples to write
     * @param count Number of bytes to write
     * @return Number of bytes actually written (may be less if buffer is full)
     */
    size_t Write(const uint8_t* data, size_t count) {
        size_t writePos = m_writePos.load(std::memory_order_relaxed);
        size_t readPos = m_readPos.load(std::memory_order_acquire);

        size_t available = AvailableToWrite(writePos, readPos);
        size_t toWrite = std::min(count, available);

        if (toWrite == 0) {
            return 0;
        }

        // Calculate how much we can write before wrapping
        size_t firstPart = std::min(toWrite, m_capacity - writePos);
        std::copy(data, data + firstPart, m_buffer.begin() + writePos);

        // Write the rest after wrapping (if any)
        if (toWrite > firstPart) {
            std::copy(data + firstPart, data + toWrite, m_buffer.begin());
        }

        // Update write position
        size_t newWritePos = (writePos + toWrite) % m_capacity;
        m_writePos.store(newWritePos, std::memory_order_release);

        return toWrite;
    }

    /**
     * Read samples from the buffer. Called from the encoder thread.
     * @param data Buffer to read into
     * @param count Number of bytes to read
     * @return Number of bytes actually read (may be less if buffer doesn't have enough)
     */
    size_t Read(uint8_t* data, size_t count) {
        size_t readPos = m_readPos.load(std::memory_order_relaxed);
        size_t writePos = m_writePos.load(std::memory_order_acquire);

        size_t available = AvailableToRead(writePos, readPos);
        size_t toRead = std::min(count, available);

        if (toRead == 0) {
            return 0;
        }

        // Calculate how much we can read before wrapping
        size_t firstPart = std::min(toRead, m_capacity - readPos);
        std::copy(m_buffer.begin() + readPos, m_buffer.begin() + readPos + firstPart, data);

        // Read the rest after wrapping (if any)
        if (toRead > firstPart) {
            std::copy(m_buffer.begin(), m_buffer.begin() + (toRead - firstPart), data + firstPart);
        }

        // Update read position
        size_t newReadPos = (readPos + toRead) % m_capacity;
        m_readPos.store(newReadPos, std::memory_order_release);

        return toRead;
    }

    /**
     * Get number of bytes available to read.
     */
    size_t Available() const {
        size_t writePos = m_writePos.load(std::memory_order_acquire);
        size_t readPos = m_readPos.load(std::memory_order_acquire);
        return AvailableToRead(writePos, readPos);
    }

    /**
     * Get remaining capacity for writing.
     */
    size_t FreeSpace() const {
        size_t writePos = m_writePos.load(std::memory_order_acquire);
        size_t readPos = m_readPos.load(std::memory_order_acquire);
        return AvailableToWrite(writePos, readPos);
    }

    /**
     * Reset the buffer (not thread-safe, call only when stopped).
     */
    void Reset() {
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
    }

    size_t Capacity() const { return m_capacity - 1; }

private:
    size_t AvailableToWrite(size_t writePos, size_t readPos) const {
        // Leave one slot empty to distinguish full from empty
        if (writePos >= readPos) {
            return m_capacity - 1 - (writePos - readPos);
        }
        return readPos - writePos - 1;
    }

    size_t AvailableToRead(size_t writePos, size_t readPos) const {
        if (writePos >= readPos) {
            return writePos - readPos;
        }
        return m_capacity - readPos + writePos;
    }

    std::vector<uint8_t> m_buffer;
    size_t m_capacity;
    std::atomic<size_t> m_writePos;
    std::atomic<size_t> m_readPos;
};

} // namespace record_windows
