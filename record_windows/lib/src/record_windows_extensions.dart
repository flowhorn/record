import 'package:flutter/services.dart';
import 'package:record_platform_interface/record_platform_interface.dart';

/// Windows-specific recorder extensions for continuous capture.
///
/// These methods allow keeping the microphone device continuously open
/// to eliminate startup latency when recording begins.
///
/// Usage:
/// ```dart
/// final recorder = AudioRecorder();
/// await recorder.create();
///
/// // Enable continuous capture with a config
/// await RecordWindowsExtensions.enableContinuousCapture(
///   recorder.recorderId,
///   RecordConfig(sampleRate: 48000, numChannels: 1),
/// );
///
/// // Now start() will have near-instant audio availability
/// await recorder.start(config, path: 'output.aac');
///
/// // When done, disable continuous capture
/// await RecordWindowsExtensions.disableContinuousCapture(recorder.recorderId);
/// ```
class RecordWindowsExtensions {
  static const _methodChannel = MethodChannel('com.llfbandit.record/messages');

  /// Enables continuous capture to keep the microphone device open.
  ///
  /// This starts capturing audio from the specified device, keeping the
  /// Windows audio stack primed and ready. When a recording is started
  /// with [AudioRecorder.start], audio will be available immediately
  /// without any warmup delay.
  ///
  /// If continuous capture is already enabled with the same configuration,
  /// this method returns immediately. If the configuration differs, the
  /// current capture is disabled and a new one is started.
  ///
  /// [recorderId] The ID of the recorder instance.
  /// [config] The recording configuration specifying device, sample rate, etc.
  static Future<void> enableContinuousCapture(
    String recorderId,
    RecordConfig config,
  ) {
    return _methodChannel.invokeMethod('enableContinuousCapture', {
      'recorderId': recorderId,
      ...config.toMap(),
    });
  }

  /// Disables continuous capture and releases the microphone device.
  ///
  /// This stops the continuous capture and releases the audio device.
  /// Call this when continuous capture is no longer needed to free
  /// system resources.
  ///
  /// [recorderId] The ID of the recorder instance.
  static Future<void> disableContinuousCapture(String recorderId) {
    return _methodChannel.invokeMethod('disableContinuousCapture', {
      'recorderId': recorderId,
    });
  }

  /// Checks if continuous capture is currently enabled.
  ///
  /// Returns `true` if continuous capture is active, `false` otherwise.
  ///
  /// [recorderId] The ID of the recorder instance.
  static Future<bool> isContinuousCaptureEnabled(String recorderId) async {
    final result = await _methodChannel.invokeMethod<bool>(
      'isContinuousCaptureEnabled',
      {'recorderId': recorderId},
    );
    return result ?? false;
  }
}
