#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <portaudio.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
#include <utils/flog.h>
#include <core.h>
#include "portaudio_bridge.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_sink",
    /* Description:     */ "Audio sink module for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

class AudioSink : SinkManager::Sink {
public:
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "Audio callback play state must be lock-free");

    struct AudioDevice_t {
        std::string name;
        int index;
        int channels;
        int srId;
        std::vector<double> sampleRates;
        std::string txtSampleRates;
    };

    AudioSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;
        bridge.init(_stream->sinkOut);

        // Initialize PortAudio
        devCount = Pa_GetDeviceCount();
        devId = Pa_GetDefaultOutputDevice();
        const PaDeviceInfo* deviceInfo;
        PaStreamParameters outputParams;
        outputParams.sampleFormat = paFloat32;
        outputParams.hostApiSpecificStreamInfo = NULL;

        // Gather hardware info
        for (int i = 0; i < devCount; i++) {
            deviceInfo = Pa_GetDeviceInfo(i);
            if (deviceInfo->maxOutputChannels < 1) {
                continue;
            }
            AudioDevice_t dev;
            dev.name = deviceInfo->name;
            dev.index = i;
            dev.channels = std::min<int>(deviceInfo->maxOutputChannels, 2);
            dev.sampleRates.clear();
            dev.txtSampleRates = "";
            for (int j = 0; j < 6; j++) {
                outputParams.channelCount = dev.channels;
                outputParams.device = dev.index;
                outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
                PaError err = Pa_IsFormatSupported(NULL, &outputParams, POSSIBLE_SAMP_RATE[j]);
                if (err != paFormatIsSupported) {
                    continue;
                }
                dev.sampleRates.push_back(POSSIBLE_SAMP_RATE[j]);
                dev.txtSampleRates += std::to_string((int)POSSIBLE_SAMP_RATE[j]);
                dev.txtSampleRates += '\0';
            }
            if (dev.sampleRates.size() == 0) {
                continue;
            }
            if (i == devId) {
                devListId = devices.size();
                defaultDev = devListId;
                _stream->setSampleRate(dev.sampleRates[0]);
            }
            dev.srId = 0;

            AudioDevice_t* _dev = new AudioDevice_t;
            *_dev = dev;
            devices.push_back(_dev);

            deviceNames.push_back(deviceInfo->name);
            txtDevList += deviceInfo->name;
            txtDevList += '\0';
        }

        // Load config from file
        playing.store(gui::mainWindow.isPlaying(), std::memory_order_relaxed);
        playStateHandler.handler = playStateChangeHandler;
        playStateHandler.ctx = this;
        gui::mainWindow.onPlayStateChange.bindHandler(&playStateHandler);
    }

    ~AudioSink() {
        gui::mainWindow.onPlayStateChange.unbindHandler(&playStateHandler);
        stop();
        for (auto const& dev : devices) {
            delete dev;
        }
    }

    void start() {
        if (running) {
            return;
        }
        running = doStart();
    }

    void stop() {
        if (!running) {
            return;
        }
        doStop();
        running = false;
    }

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("##_audio_sink_dev_" + _streamName).c_str(), &devListId, txtDevList.c_str())) {
            // TODO: Load SR from config
            if (running) {
                doStop();
                running = doStart();
            }
            // TODO: Save to config
        }

        AudioDevice_t* dev = devices[devListId];

        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::Combo(("##_audio_sink_sr_" + _streamName).c_str(), &dev->srId, dev->txtSampleRates.c_str())) {
            _stream->setSampleRate(dev->sampleRates[dev->srId]);
            if (running) {
                doStop();
                running = doStart();
            }
            // TODO: Save to config
        }
    }

private:
    static void playStateChangeHandler(bool newState, void* ctx) {
        ((AudioSink*)ctx)->playing.store(newState, std::memory_order_relaxed);
    }

    bool doStart() {
        AudioDevice_t* dev = devices[devListId];
        PaStreamParameters outputParams;
        outputParams.channelCount = dev->channels;
        outputParams.sampleFormat = paFloat32;
        outputParams.hostApiSpecificStreamInfo = NULL;
        outputParams.device = dev->index;
        outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
        PaError err;

        float sampleRate = dev->sampleRates[dev->srId];
        const size_t periodFrames = std::max<size_t>(1, (size_t)(sampleRate / 60.0f));
        try {
            bridge.configure(dev->channels, periodFrames);
        }
        catch (const std::exception& e) {
            flog::error("Could not allocate the audio bridge: {0}", e.what());
            return false;
        }

        // Let PortAudio choose the backend's native callback period. The FIFO
        // handles variable frame counts without blocking the realtime thread.
        err = Pa_OpenStream(&stream, NULL, &outputParams, sampleRate,
                            paFramesPerBufferUnspecified, paNoFlag, _audio_cb, this);
        if (err != 0) {
            flog::error("Error while opening audio stream: ({0}) => {1}", err, Pa_GetErrorText(err));
            stream = nullptr;
            return false;
        }

        try {
            // Start the producer before the device so it can prefill the FIFO.
            bridge.start();
        }
        catch (const std::exception& e) {
            flog::error("Error while starting the audio bridge: {0}", e.what());
            Pa_CloseStream(stream);
            stream = nullptr;
            return false;
        }

        err = Pa_StartStream(stream);
        if (err != 0) {
            flog::error("Error while starting audio stream: ({0}) => {1}", err, Pa_GetErrorText(err));
            bridge.stop();
            Pa_CloseStream(stream);
            stream = nullptr;
            return false;
        }
        streamOpen = true;
        flog::info("Audio device open.");
        return true;
    }

    void doStop() {
        // Stop producing first; the callback can safely drain the immutable
        // FIFO or zero-fill while PortAudio shuts down. Do not reset the FIFO
        // until Pa_CloseStream guarantees that no callback is still using it.
        bridge.stop();
        if (streamOpen) {
            PaError err = Pa_StopStream(stream);
            if (err != paNoError && err != paStreamIsStopped) {
                flog::warn("Error while stopping audio stream: ({0}) => {1}", err, Pa_GetErrorText(err));
            }
            err = Pa_CloseStream(stream);
            if (err != paNoError) {
                flog::warn("Error while closing audio stream: ({0}) => {1}", err, Pa_GetErrorText(err));
            }
            streamOpen = false;
            stream = nullptr;
        }
        bridge.reset();
    }

    static int _audio_cb(const void* input, void* output, unsigned long frameCount,
                         const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void* userData) {
        AudioSink* _this = (AudioSink*)userData;
        float* samples = (float*)output;
        const size_t channels = (size_t)_this->bridge.channels();
        if (!_this->playing.load(std::memory_order_relaxed)) {
            _this->bridge.discard();
            std::memset(samples, 0, (size_t)frameCount * channels * sizeof(float));
            return paContinue;
        }

        const size_t copied = _this->bridge.pop(samples, (size_t)frameCount);
        if (copied != (size_t)frameCount) {
            std::memset(samples + copied * channels, 0,
                        ((size_t)frameCount - copied) * channels * sizeof(float));
        }
        return paContinue;
    }

    SinkManager::Stream* _stream;
    PortAudioBridge bridge;
    bool streamOpen = false;
    std::atomic<bool> playing{ false };
    EventHandler<bool> playStateHandler;

    std::string _streamName;
    PaStream* stream = nullptr;

    int srId = 0;
    int devCount;
    int devId = 0;
    int devListId = 0;
    int defaultDev = 0;
    bool running = false;

    const double POSSIBLE_SAMP_RATE[6] = {
        48000.0f,
        44100.0f,
        24000.0f,
        22050.0f,
        12000.0f,
        11025.0f
    };

    std::vector<AudioDevice_t*> devices;
    std::vector<std::string> deviceNames;
    std::string txtDevList;
};

class AudioSinkModule : public ModuleManager::Instance {
public:
    AudioSinkModule(std::string name) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;

        Pa_Initialize();

        sigpath::sinkManager.registerSinkProvider("Audio", provider);
    }

    ~AudioSinkModule() {
        sigpath::sinkManager.unregisterSinkProvider("Audio");
        Pa_Terminate();
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new AudioSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    // Nothing here
    // TODO: Do instancing here (in source modules as well) to prevent multiple loads
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    AudioSinkModule* instance = new AudioSinkModule(name);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_() {
}

MOD_EXPORT void _END_() {
}
