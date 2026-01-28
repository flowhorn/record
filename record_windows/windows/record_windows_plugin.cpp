#include "record_windows_plugin.h"
#include "record_config.h"
#include <flutter/event_stream_handler_functions.h>
#include <mutex>

// Include miniaudio for device enumeration
#include "miniaudio.h"

using namespace flutter;

namespace record_windows {
    static void ErrorFromHR(HRESULT hr, MethodResult<EncodableValue>& result)
    {
        _com_error err(hr);
        std::string errorText = Utf8FromUtf16(err.ErrorMessage());

        result.Error("Record", "", EncodableValue(errorText));
    }

    static HWND GetRootWindow(flutter::FlutterView* view) {
        return ::GetAncestor(view->GetNativeWindow(), GA_ROOT);
    }

    // static, Register the plugin
    void RecordWindowsPlugin::RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar) {
        auto plugin = std::make_unique<RecordWindowsPlugin>(
            [registrar](auto delegate) {
                return registrar->RegisterTopLevelWindowProcDelegate(delegate);
            },
            [registrar](auto proc_id) {
                registrar->UnregisterTopLevelWindowProcDelegate(proc_id);
            },
            [registrar] { return GetRootWindow(registrar->GetView()); }
        );

        m_binaryMessenger = registrar->messenger();

        auto methodChannel = std::make_unique<MethodChannel<EncodableValue>>(
            m_binaryMessenger, "com.llfbandit.record/messages",
            &StandardMethodCodec::GetInstance());

        methodChannel->SetMethodCallHandler(
            [plugin_pointer = plugin.get()](const auto& call, auto result)
            {
                plugin_pointer->HandleMethodCall(call, std::move(result));
            });

        registrar->AddPlugin(std::move(plugin));
    }

    // static
    std::queue<std::function<void()>> RecordWindowsPlugin::callbacks{};

    // static
    std::mutex RecordWindowsPlugin::callbacks_mutex{};

    // static
    FlutterRootWindowProvider RecordWindowsPlugin::get_root_window{};

    // static
    void RecordWindowsPlugin::RunOnMainThread(std::function<void()> callback) {
        // Lock only while pushing the callback, then release before posting the message.
        {
            std::lock_guard<std::mutex> lock(callbacks_mutex);
            callbacks.push(callback);
        }
        PostMessage(get_root_window(), WM_RUN_DELEGATE, 0, 0);
    }

    RecordWindowsPlugin::RecordWindowsPlugin(
        WindowProcDelegateRegistrator registrator,
        WindowProcDelegateUnregistrator unregistrator,
        FlutterRootWindowProvider window_provider
    ):	m_win_proc_delegate_registrator(registrator),
        m_win_proc_delegate_unregistrator(unregistrator) {

        get_root_window = std::move(window_provider);

        m_window_proc_id = m_win_proc_delegate_registrator(
            [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
                return HandleWindowProc(hwnd, message, wparam, lparam);
            }
        );
    }

    RecordWindowsPlugin::~RecordWindowsPlugin() {
        for (const auto& [recorderId, recorder] : m_recorders)
        {
            recorder->Dispose();
        }

        m_win_proc_delegate_unregistrator(m_window_proc_id);
    }

    std::optional<LRESULT> RecordWindowsPlugin::HandleWindowProc(HWND hwnd,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) {
        std::optional<LRESULT> result;
        switch (message) {
        case WM_RUN_DELEGATE:
            {
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> lock(callbacks_mutex);
                    if (!callbacks.empty()) {
                        cb = std::move(callbacks.front());
                        callbacks.pop();
                    }
                }
                if (cb) cb();
                result = 0;
            }
            break;
        }
        return result;
    }

    // Called when a method is called on this plugin's channel from Dart.
    void RecordWindowsPlugin::RecordWindowsPlugin::HandleMethodCall(
        const MethodCall<EncodableValue>& method_call,
        std::unique_ptr<MethodResult<EncodableValue>> result
    ) {
        const auto args = method_call.arguments();
        const auto* mapArgs = std::get_if<EncodableMap>(args);
        if (!mapArgs) {
            result->Error("Record", "Call missing parameters");
            return;
        }

        std::string recorderId;
        GetValueFromEncodableMap(mapArgs, "recorderId", recorderId);
        if (recorderId.empty()) {
            result->Error("Record", "Call missing mandatory parameter recorderId");
            return;
        }

        if (method_call.method_name().compare("create") == 0) {
            HRESULT hr = CreateRecorder(recorderId);

            if (SUCCEEDED(hr)) {
                result->Success(EncodableValue(NULL));
            }
            else {
                ErrorFromHR(hr, *result);
            }
            return;
        }

        auto recorder = GetRecorder(recorderId);
        if (!recorder) {
            result->Error(
                "Record",
                "Recorder has not yet been created or has already been disposed."
            );
            return;
        }

        if (method_call.method_name().compare("hasPermission") == 0)
        {
            result->Success(EncodableValue(true));
        }
        else if (method_call.method_name().compare("isPaused") == 0)
        {
            result->Success(EncodableValue(recorder->IsPaused()));
        }
        else if (method_call.method_name().compare("isRecording") == 0)
        {
            result->Success(EncodableValue(recorder->IsRecording()));
        }
        else if (method_call.method_name().compare("pause") == 0)
        {
            HRESULT hr = recorder->Pause();

            if (SUCCEEDED(hr)) { result->Success(EncodableValue()); }
            else { ErrorFromHR(hr, *result); }
        }
        else if (method_call.method_name().compare("resume") == 0)
        {
            HRESULT hr = recorder->Resume();

            if (SUCCEEDED(hr)) { result->Success(EncodableValue()); }
            else { ErrorFromHR(hr, *result); }
        }
        else if (method_call.method_name().compare("start") == 0)
        {
            auto config = InitRecordConfig(mapArgs);

            std::string path;
            GetValueFromEncodableMap(mapArgs, "path", path);

            HRESULT hr = recorder->Start(std::move(config), Utf16FromUtf8(path));

            if (SUCCEEDED(hr)) { result->Success(EncodableValue()); }
            else { ErrorFromHR(hr, *result); }
        }
        else if (method_call.method_name().compare("startStream") == 0)
        {
            auto config = InitRecordConfig(mapArgs);

            HRESULT hr = recorder->StartStream(std::move(config));

            if (SUCCEEDED(hr)) { result->Success(EncodableValue()); }
            else { ErrorFromHR(hr, *result); }
        }
        else if (method_call.method_name().compare("stop") == 0)
        {
            auto recordingPath = recorder->GetRecordingPath();
            HRESULT hr = recorder->Stop();

            if (SUCCEEDED(hr))
            {
                result->Success(recordingPath.empty() ? EncodableValue() : EncodableValue(Utf8FromUtf16(recordingPath)));
            }
            else {
                ErrorFromHR(hr, *result);
            }
        }
        else if (method_call.method_name().compare("cancel") == 0)
        {
            HRESULT hr = recorder->Cancel();

            if (SUCCEEDED(hr))
            {
                result->Success(EncodableValue());
            }
            else
            {
                ErrorFromHR(hr, *result);
            }
        }
        else if (method_call.method_name().compare("dispose") == 0)
        {
            // Dispose recorder and schedule removal on the main thread so any
            // callbacks already queued to run on the main thread (e.g. UpdateState)
            // can run safely and observe the disposed state before the object is
            // destroyed. Immediate erase would destroy the Recorder while lambdas
            // referencing it may still be pending, causing access violations.
            recorder->Dispose();
            RecordWindowsPlugin::RunOnMainThread([this, recorderId]() -> void {
                m_recorders.erase(recorderId);
                m_state_event_channels.erase(recorderId);
                m_record_event_channels.erase(recorderId);
            });

            result->Success(EncodableValue());
        }
        else if (method_call.method_name().compare("getAmplitude") == 0)
        {
            auto amp = recorder->GetAmplitude();

            result->Success(EncodableValue(
                EncodableMap({
                    {EncodableValue("current"), EncodableValue(amp["current"])},
                    {EncodableValue("max"), EncodableValue(amp["max"])}
                    }
                ))
            );
        }
        else if (method_call.method_name().compare("isEncoderSupported") == 0)
        {
            std::string encoderName;
            if (!GetValueFromEncodableMap(mapArgs, "encoder", encoderName))
            {
                result->Error("Bad arguments", "Expected encoder name.");
                return;
            }

            bool supported = false;
            HRESULT hr = recorder->isEncoderSupported(encoderName, &supported);

            if (SUCCEEDED(hr))
            {
                result->Success(EncodableValue(supported));
            }
            else
            {
                ErrorFromHR(hr, *result);
            }
        }
        else if (method_call.method_name().compare("listInputDevices") == 0)
        {
            ListInputDevices(*result);
        }
        else if (method_call.method_name().compare("enableContinuousCapture") == 0)
        {
            auto config = InitRecordConfig(mapArgs);
            HRESULT hr = recorder->EnableContinuousCapture(std::move(config));

            if (SUCCEEDED(hr)) { result->Success(EncodableValue()); }
            else { ErrorFromHR(hr, *result); }
        }
        else if (method_call.method_name().compare("disableContinuousCapture") == 0)
        {
            HRESULT hr = recorder->DisableContinuousCapture();

            if (SUCCEEDED(hr)) { result->Success(EncodableValue()); }
            else { ErrorFromHR(hr, *result); }
        }
        else if (method_call.method_name().compare("isContinuousCaptureEnabled") == 0)
        {
            result->Success(EncodableValue(recorder->IsContinuousCaptureEnabled()));
        }
    }

    std::unique_ptr<RecordConfig> RecordWindowsPlugin::InitRecordConfig(const EncodableMap* args)
    {
        std::string path;
        GetValueFromEncodableMap(args, "path", path);
        std::string encoderName;
        GetValueFromEncodableMap(args, "encoder", encoderName);
        int bitRate;
        GetValueFromEncodableMap(args, "bitRate", bitRate);
        int sampleRate;
        GetValueFromEncodableMap(args, "sampleRate", sampleRate);
        int numChannels;
        GetValueFromEncodableMap(args, "numChannels", numChannels);
        EncodableMap device;
        std::string deviceId;
        if (GetValueFromEncodableMap(args, "device", device))
        {
            GetValueFromEncodableMap(&device, "id", deviceId);
        }
        bool autoGain;
        GetValueFromEncodableMap(args, "autoGain", autoGain);
        bool echoCancel;
        GetValueFromEncodableMap(args, "echoCancel", echoCancel);
        bool noiseSuppress;
        GetValueFromEncodableMap(args, "noiseSuppress", noiseSuppress);

        auto config = std::make_unique<RecordConfig>(
            encoderName,
            deviceId,
            bitRate,
            sampleRate,
            numChannels,
            autoGain,
            echoCancel,
            noiseSuppress
        );

        return config;
    }

    HRESULT RecordWindowsPlugin::CreateRecorder(std::string recorderId)
    {
        // State event channel
        auto eventChannel = std::make_unique<EventChannel<EncodableValue>>(
            m_binaryMessenger, "com.llfbandit.record/events/" + recorderId,
            &StandardMethodCodec::GetInstance());

        auto eventHandler = new EventStreamHandler<>();
        std::unique_ptr<StreamHandler<EncodableValue>> pStateEventHandler{static_cast<StreamHandler<EncodableValue>*>(eventHandler)};
        eventChannel->SetStreamHandler(std::move(pStateEventHandler));

        // Record stream event channel
        auto eventRecordChannel = std::make_unique<EventChannel<EncodableValue>>(
            m_binaryMessenger, "com.llfbandit.record/eventsRecord/" + recorderId,
            &StandardMethodCodec::GetInstance());

        auto eventRecordHandler = new EventStreamHandler<>();
        std::unique_ptr<StreamHandler<EncodableValue>> pRecordEventHandler{static_cast<StreamHandler<EncodableValue>*>(eventRecordHandler)};
        eventRecordChannel->SetStreamHandler(std::move(pRecordEventHandler));

        // Keep channels alive for the recorder lifetime and also keep shared
        // ownership of handlers so Recorder's weak_ptr captures remain valid
        m_state_event_channels.insert(std::make_pair(recorderId, std::move(eventChannel)));
        m_record_event_channels.insert(std::make_pair(recorderId, std::move(eventRecordChannel)));

        Recorder* pRecorder = NULL;

        HRESULT hr = Recorder::CreateInstance(eventHandler, eventRecordHandler, &pRecorder);
        if (SUCCEEDED(hr))
        {
            m_recorders.insert(std::make_pair(recorderId, std::move(pRecorder)));
        }

        return hr;
    }

    Recorder* RecordWindowsPlugin::GetRecorder(std::string recorderId)
    {
        auto searchedRecorder = m_recorders.find(recorderId);
        if (searchedRecorder == m_recorders.end()) {
            return nullptr;
        }
        return searchedRecorder->second.get();
    }

    HRESULT RecordWindowsPlugin::ListInputDevices(MethodResult<EncodableValue>& result)
    {
        EncodableList devices;

        // Use miniaudio for device enumeration
        ma_context context;
        ma_context_config contextConfig = ma_context_config_init();
        
        if (ma_context_init(NULL, 0, &contextConfig, &context) != MA_SUCCESS) {
            result.Error("Record", "Failed to initialize audio context");
            return E_FAIL;
        }

        ma_device_info* pCaptureDevices;
        ma_uint32 captureDeviceCount;
        ma_device_info* pPlaybackDevices;
        ma_uint32 playbackDeviceCount;

        ma_result maResult = ma_context_get_devices(&context, 
                                                     &pPlaybackDevices, &playbackDeviceCount,
                                                     &pCaptureDevices, &captureDeviceCount);

        if (maResult == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < captureDeviceCount; i++) {
                // Convert device ID to string (use the index as a simple ID for now)
                std::string deviceId = std::to_string(i);
                std::string deviceName = pCaptureDevices[i].name;

                devices.push_back(EncodableMap({
                    {EncodableValue("id"), EncodableValue(deviceId)},
                    {EncodableValue("label"), EncodableValue(deviceName)}
                }));
            }
        }

        ma_context_uninit(&context);

        if (maResult == MA_SUCCESS) {
            result.Success(std::move(EncodableValue(devices)));
        } else {
            result.Error("Record", "Failed to enumerate devices");
            return E_FAIL;
        }

        return S_OK;
    }
}  // namespace record_windows
