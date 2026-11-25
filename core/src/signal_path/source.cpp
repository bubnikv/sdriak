#include <server.h>
#include <signal_path/source.h>
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/gui.h>
#ifdef __ANDROID__
#include <android_backend.h>
#endif

SourceManager::SourceManager() {
}

void SourceManager::registerSource(std::string name, SourceHandler* handler) {
    if (sources.find(name) != sources.end()) {
        flog::error("Tried to register new source with existing name: {0}", name);
        return;
    }
    sources[name] = handler;
    onSourceRegistered.emit(name);
}

void SourceManager::unregisterSource(std::string name) {
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to unregister non existent source: {0}", name);
        return;
    }
    onSourceUnregister.emit(name);
    if (name == selectedName) {
        if (selectedHandler != NULL) {
            sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
        }
        sigpath::iqFrontEnd.setInput(&nullSource);
        selectedHandler = NULL;
    }
    sources.erase(name);
    onSourceUnregistered.emit(name);
}

std::vector<std::string> SourceManager::getSourceNames() {
    std::vector<std::string> names;
    for (auto const& [name, src] : sources) { names.push_back(name); }
    return names;
}

void SourceManager::selectSource(std::string name) {
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to select non existent source: {0}", name);
        return;
    }
    if (selectedHandler != NULL) {
        sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
    }
    selectedHandler = sources[name];
    selectedHandler->selectHandler(selectedHandler->ctx);
    selectedName = name;
    if (core::args["server"].b()) {
        server::setInput(selectedHandler->stream);
    }
    else {
        sigpath::iqFrontEnd.setInput(selectedHandler->stream);
    }
    // Set server input here
}

void SourceManager::showSelectedMenu() {
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->menuHandler(selectedHandler->ctx);
}

void SourceManager::start() {
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->startHandler(selectedHandler->ctx);
#ifdef __ANDROID__
    backend::startSleepTimer();
#endif
}

void SourceManager::stop() {
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->stopHandler(selectedHandler->ctx);
#ifdef __ANDROID__
    backend::stopSleepTimer();
#endif
}

void SourceManager::tune(double freq) {
    if (selectedHandler == NULL) {
        return;
    }
    // TODO: No need to always retune the hardware in Panadapter mode
    selectedHandler->tuneHandler(abs(((tuneMode == TuningMode::NORMAL) ? (freq + tuneOffset) : ifFreq)), selectedHandler->ctx);
    onRetune.emit(freq + tuneOffset);
    currentFreq = freq;
}

void SourceManager::uiTick() {
    if (selectedHandler == NULL || selectedHandler->uiTickHandler == nullptr) {
        return;
    }
    selectedHandler->uiTickHandler(selectedHandler->ctx);
}

void SourceManager::setTuningOffset(double offset) {
    tuneOffset = offset;
    // The offset shifts the display<->hardware mapping, so the selector limit
    // must be recomputed against the new offset.
    applyTuningLimits();
    tune(currentFreq);
}

void SourceManager::setTuningLimits(double minFreq, double maxFreq) {
    tuningLimitEnabled = true;
    tuningLimitMin = minFreq;
    tuningLimitMax = maxFreq;
    applyTuningLimits();
}

void SourceManager::clearTuningLimits() {
    tuningLimitEnabled = false;
    applyTuningLimits();
}

void SourceManager::applyTuningLimits() {
    if (core::args["server"].b()) {
        // Headless server: there is no local frequency selector. Forward the
        // native-domain limit to the connected remote client, whose own
        // SourceManager applies its local tuning offset before constraining
        // its freqSelect.
        server::setTuningLimits(tuningLimitEnabled, tuningLimitMin, tuningLimitMax);
        return;
    }
    if (!tuningLimitEnabled) {
        gui::freqSelect.limitFreq = false;
        return;
    }
    // gui::freqSelect constrains the DISPLAY frequency, but sources express
    // their NATIVE hardware range. tune() computes hardware = display +
    // tuneOffset, so display = hardware - tuneOffset. Shift the native range
    // into the display domain. Floor at 0 because the display frequency can't
    // be negative (an upconverter offset would push the low end below zero).
    double dispMin = tuningLimitMin - tuneOffset;
    double dispMax = tuningLimitMax - tuneOffset;
    if (dispMin < 0.0) { dispMin = 0.0; }
    if (dispMax < 0.0) { dispMax = 0.0; }
    gui::freqSelect.minFreq = (uint64_t)dispMin;
    gui::freqSelect.maxFreq = (uint64_t)dispMax;
    gui::freqSelect.limitFreq = true;
}

void SourceManager::setTuningMode(TuningMode mode) {
    tuneMode = mode;
    tune(currentFreq);
}

void SourceManager::setPanadapterIF(double freq) {
    ifFreq = freq;
    tune(currentFreq);
}