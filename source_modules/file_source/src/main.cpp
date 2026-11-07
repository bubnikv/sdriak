/*
 * Adapted from the SDRPP distribution (https://github.com/qrp73/SDRPP).
 * Copyright (c) 2023 qrp73.
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#define NOMINMAX
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <wavreader.h>
#include <flacreader.h>
#include <core.h>
#include <gui/widgets/file_select.h>
#include <filesystem>
#include <regex>
#include <gui/tuner.h>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <fstream>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "file_source",
    /* Description:     */ "Wav file source module for SDRIAK",
    /* Author:          */ "Ryzerth, qrp73",
    /* Version:         */ 0, 1, 2,
    /* Max instances    */ 1
};

ConfigManager config;

// Converters from one WAV sample to float, all little-endian
// (per-format conversion adopted from qrp73/SDRPP)
typedef float (*SampleConv)(const uint8_t*);

static float convU8(const uint8_t* p) {
    return ((float)p[0] - 127.5f) * (1.0f / 127.5f);
}
static float convI16(const uint8_t* p) {
    int16_t v;
    memcpy(&v, p, sizeof(v));
    return (float)v * (1.0f / 32768.0f);
}
static float convI24(const uint8_t* p) {
    int32_t v = (int32_t)((uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16)) << 8) >> 8;
    return (float)v * (1.0f / 8388608.0f);
}
static float convI32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return (float)((double)v * (1.0 / 2147483648.0));
}
static float convF32(const uint8_t* p) {
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static float convF64(const uint8_t* p) {
    double v;
    memcpy(&v, p, sizeof(v));
    return (float)v;
}

static SampleConv getSampleConv(WAVE_FORMAT format, int bitDepth) {
    if (format == WAVE_FORMAT::PCM) {
        switch (bitDepth) {
            case 8:  return convU8;
            case 16: return convI16;
            case 24: return convI24;
            case 32: return convI32;
        }
    }
    else if (format == WAVE_FORMAT::IEEE_FLOAT) {
        switch (bitDepth) {
            case 32: return convF32;
            case 64: return convF64;
        }
    }
    return NULL;
}

// Pick the reader by the file's magic bytes rather than its extension:
// "RIFF"/"RF64" -> WavReader, "fLaC" -> FlacReader. Returns NULL for anything
// else. The caller owns the returned reader.
static SampleReader* openReader(const std::string& path) {
    uint8_t magic[4] = { 0 };
    std::ifstream(path, std::ios::binary).read((char*)magic, sizeof(magic));
    if (!memcmp(magic, "fLaC", 4)) { return new FlacReader(path); }
    if (!memcmp(magic, "RIFF", 4) || !memcmp(magic, "RF64", 4)) { return new WavReader(path); }
    return NULL;
}

// The worker assumes all of this, so it must hold before starting one
static bool isPlayable(SampleReader* reader) {
    if (!reader) { return false; }
    int channels = reader->getChannelCount();
    int bytesPerChan = reader->getBitDepth() / 8;
    return getSampleConv(reader->getFormat(), reader->getBitDepth()) != NULL
        && channels >= 1
        && reader->getBlockAlign() >= channels * bytesPerChan
        && reader->getSampleCount() >= 1;
}

class FileSourceModule : public ModuleManager::Instance {
public:
    FileSourceModule(std::string name) : fileSelect("", { "IQ Recordings (*.wav *.flac)", "*.wav *.flac", "All Files", "*" }) {
        this->name = name;

        if (core::args["server"].b()) { return; }

        fileSelect.setPath(config.read().value("path", std::string()), true);

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("File", &handler);
    }

    ~FileSourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("File");
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
    static void menuSelected(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", _this->centerFreq);
        sigpath::iqFrontEnd.setBuffering(false);
        gui::waterfall.centerFrequencyLocked = true;
        //gui::freqSelect.minFreq = _this->centerFreq - (_this->sampleRate/2);
        //gui::freqSelect.maxFreq = _this->centerFreq + (_this->sampleRate/2);
        //gui::freqSelect.limitFreq = true;
        flog::info("FileSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        sigpath::iqFrontEnd.setBuffering(true);
        //gui::freqSelect.limitFreq = false;
        gui::waterfall.centerFrequencyLocked = false;
        flog::info("FileSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (_this->running) { return; }
        if (!isPlayable(_this->reader)) { return; }
        _this->running = true;
        _this->workerThread = std::thread(worker, _this);
        flog::info("FileSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        if (!_this->running) { return; }
        if (_this->reader == NULL) { return; }
        _this->stream.stopWriter();
        _this->workerThread.join();
        _this->stream.clearWriteStop();
        _this->running = false;
        _this->reader->rewind();
        flog::info("FileSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        flog::info("FileSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;

        if (_this->running) { style::beginDisabled(); }
        bool fileChanged = _this->fileSelect.render("##file_source_" + _this->name);
        if (_this->running) { style::endDisabled(); }

        if (fileChanged && _this->fileSelect.pathIsValid()) {
            if (_this->reader != NULL) {
                _this->reader->close();
                delete _this->reader;
                _this->reader = NULL;
            }
            try {
                _this->reader = openReader(_this->fileSelect.path);
                if (_this->reader == NULL) {
                    throw std::runtime_error("Unsupported file format (not a WAV or FLAC file)");
                }
                if (!_this->reader->isValid()) {
                    throw std::runtime_error("Invalid or damaged file");
                }
                if (_this->reader->getSampleRate() == 0) {
                    throw std::runtime_error("Sample rate may not be zero");
                }
                if (!getSampleConv(_this->reader->getFormat(), _this->reader->getBitDepth())) {
                    throw std::runtime_error(std::string("Unsupported sample format: ") + _this->reader->getFormatName() + ", " + std::to_string(_this->reader->getBitDepth()) + " bit");
                }
                if (_this->reader->getSampleCount() < 1) {
                    throw std::runtime_error("File contains no samples");
                }
                if (!isPlayable(_this->reader)) {
                    throw std::runtime_error("Invalid channel layout");
                }
                _this->sampleRate = _this->reader->getSampleRate();
                core::setInputSampleRate(_this->sampleRate);
                std::string filename = std::filesystem::path(_this->fileSelect.path).filename().string();
                _this->centerFreq = _this->getFrequency(filename);
                tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", _this->centerFreq);
                //gui::freqSelect.minFreq = _this->centerFreq - (_this->sampleRate/2);
                //gui::freqSelect.maxFreq = _this->centerFreq + (_this->sampleRate/2);
                //gui::freqSelect.limitFreq = true;
            }
            catch (const std::exception& e) {
                flog::error("Error: {}", e.what());
                if (_this->reader != NULL) {
                    _this->reader->close();
                    delete _this->reader;
                    _this->reader = NULL;
                }
            }
            _this->updateFormatStr();
            config.edit().set("path", _this->fileSelect.path);
        }

        if (!_this->formatStr.empty()) {
            ImGui::TextDisabled("%s", _this->formatStr.c_str());
        }
    }

    void updateFormatStr() {
        if (reader == NULL) {
            formatStr = "";
            return;
        }
        uint32_t rate = std::max(reader->getSampleRate(), (uint32_t)1);
        uint64_t seconds = reader->getSampleCount() / rate;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s %u bit, %u ch, %.6g MS/s, %02u:%02u:%02u",
                 reader->getFormatName(), (unsigned)reader->getBitDepth(), (unsigned)reader->getChannelCount(),
                 rate / 1e6, (unsigned)(seconds / 3600), (unsigned)((seconds / 60) % 60), (unsigned)(seconds % 60));
        formatStr = buf;
    }

    static void worker(void* ctx) {
        FileSourceModule* _this = (FileSourceModule*)ctx;
        SampleReader* reader = _this->reader;
        uint32_t sampleRate = std::max(reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::clamp((int)(sampleRate / 200), 1, (int)STREAM_BUFFER_SIZE);
        int channels = reader->getChannelCount();
        int blockAlign = reader->getBlockAlign();
        int bytesPerChan = reader->getBitDepth() / 8;

        // Cap the input buffer at 16 MB: a corrupt header could otherwise
        // declare sizes whose bad_alloc, thrown on this thread, would
        // terminate the app
        blockSize = std::clamp(blockSize, 1, std::max(1, (int)(16777216 / blockAlign)));

        // start() only runs the worker on an isPlayable() reader
        SampleConv conv = getSampleConv(reader->getFormat(), reader->getBitDepth());

        // Stereo float32 is already the stream's format, read it in directly
        bool direct = (conv == convF32 && channels == 2 && blockAlign == sizeof(dsp::complex_t));
        uint8_t* inBuf = direct ? NULL : new uint8_t[(size_t)blockSize * blockAlign];

        while (true) {
            // swap() exchanges writeBuf/readBuf, so re-fetch it every block
            void* readDst = direct ? (void*)_this->stream.writeBuf : (void*)inBuf;
            size_t read = reader->readSamples(readDst, (size_t)blockSize * blockAlign);
            if (read < (size_t)blockAlign) {
                // End of file, loop back to the start
                reader->rewind();
                read = reader->readSamples(readDst, (size_t)blockSize * blockAlign);
                if (read < (size_t)blockAlign) { break; }
            }
            int frames = read / blockAlign;
            if (!direct) {
                const uint8_t* src = inBuf;
                dsp::complex_t* dst = _this->stream.writeBuf;
                for (int i = 0; i < frames; i++) {
                    // Mono plays as I=Q; extra channels beyond 2 are ignored
                    float ival = conv(src);
                    dst[i].re = ival;
                    dst[i].im = (channels >= 2) ? conv(src + bytesPerChan) : ival;
                    src += blockAlign;
                }
            }
            if (!_this->stream.swap(frames)) { break; }
        }

        if (inBuf) { delete[] inBuf; }
    }

    double getFrequency(std::string filename) {
        // Match e.g. "14100000Hz" (SDR++, SDR#), "7100kHz" (HDSDR) or
        // "7.100MHz" (SDR Console)
        std::regex expr("([0-9]+(\\.[0-9]+)?)(G|M|k)?Hz");
        std::smatch matches;
        if (!std::regex_search(filename, matches, expr)) { return 0; }
        double freq = std::atof(matches[1].str().c_str());
        std::string unit = matches[3].str();
        if (unit == "G") { return freq * 1e9; }
        if (unit == "M") { return freq * 1e6; }
        if (unit == "k") { return freq * 1e3; }
        return freq;
    }

    FileSelect fileSelect;
    std::string name;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    SampleReader* reader = NULL;
    bool running = false;
    bool enabled = true;
    float sampleRate = 1000000;
    std::thread workerThread;

    double centerFreq = 100000000;

    std::string formatStr;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["path"] = "";
    config.setPath(core::args["root"].s() + "/file_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    return new FileSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FileSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
