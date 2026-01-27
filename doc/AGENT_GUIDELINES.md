# Agent Guidelines for Record Plugin Development

This document contains important guidelines for AI agents working on the record plugin codebase.

## Critical Rules

### Audio Data Integrity

**NEVER discard, skip, or drop any audio samples.**

- Every single audio sample from the first millisecond must be captured and recorded
- Do not implement mechanisms that discard initial audio
- The user requires immediate capture of audio from the moment recording starts
- Even if initial audio levels appear quiet or unstable, the data must be preserved
- Even silence-flagged buffers must be recorded

### Design Goals

1. **Zero audio loss** - Pre-roll buffer captures everything during encoder startup
2. **Low latency** - First audio processed within 20-40ms
3. **Immediate feedback** - Amplitude analysis on first buffer
4. **Non-blocking** - Lock-free architecture prevents capture thread delays
5. **Graceful silence handling** - Even silence-flagged buffers are recorded

### Implementation Strategy

If encoder startup causes delays:
- Use a **pre-roll buffer** that captures audio while encoder initializes
- Once encoder is ready, flush the pre-roll buffer first, then continue with live audio
- This ensures zero audio loss while allowing async encoder initialization

### What NOT to do

- Never discard "warm-up" samples
- Never skip initial audio frames
- Never implement pre-roll that throws away data
- Never truncate the beginning or end of recordings

## Other Guidelines

### Warm-up Strategy

- Use `WarmUpAsync()` to pre-initialize the audio stack without blocking
- Warm-up should happen at plugin initialization, not at recording start
- The goal is to minimize latency when `Start()` is called

### Recording Start/Stop

- Ensure recordings capture audio from the exact moment `Start()` is called
- Ensure recordings include all audio up to the moment `Stop()` is called
- The ring buffer architecture allows audio capture to begin immediately while the encoder thread processes data asynchronously
