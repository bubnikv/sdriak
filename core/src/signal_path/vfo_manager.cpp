#include <signal_path/vfo_manager.h>
#include <signal_path/signal_path.h>
#include <gui/gui.h>

VFOManager::VFO::VFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    this->name = name;
    _bandwidth = bandwidth;
    dspVFO = sigpath::iqFrontEnd.addVFO(name, sampleRate, bandwidth, offset);
    wtfVFO = new ImGui::WaterfallVFO;
    wtfVFO->setReference(reference);
    wtfVFO->setBandwidth(bandwidth);
    wtfVFO->setOffset(offset);
    wtfVFO->minBandwidth = minBandwidth;
    wtfVFO->maxBandwidth = maxBandwidth;
    wtfVFO->bandwidthLocked = bandwidthLocked;
    output = &dspVFO->out;
    gui::waterfall.vfos[name] = wtfVFO;
}

VFOManager::VFO::~VFO() {
    dspVFO->stop();
    gui::waterfall.vfos.erase(name);
    if (gui::waterfall.selectedVFO == name) {
        gui::waterfall.selectFirstVFO();
    }
    sigpath::iqFrontEnd.removeVFO(name);
    delete wtfVFO;
}

void VFOManager::VFO::setOffset(double offset) {
    wtfVFO->setOffset(offset);
    m_manager->applyDspOffset(this);
    m_manager->updateOutOfWindow(this);
}

double VFOManager::VFO::getOffset() {
    return wtfVFO->generalOffset;
}

void VFOManager::VFO::setCenterOffset(double offset) {
    wtfVFO->setCenterOffset(offset);
    m_manager->applyDspOffset(this);
    m_manager->updateOutOfWindow(this);
}

void VFOManager::VFO::setBandwidth(double bandwidth, bool updateWaterfall) {
    if (_bandwidth == bandwidth) { return; }
    _bandwidth = bandwidth;
    if (updateWaterfall) { wtfVFO->setBandwidth(bandwidth); }
    dspVFO->setBandwidth(bandwidth);
    m_manager->updateOutOfWindow(this);
}

void VFOManager::VFO::setSampleRate(double sampleRate, double bandwidth) {
    dspVFO->setOutSamplerate(sampleRate, bandwidth);
    wtfVFO->setBandwidth(bandwidth);
    m_manager->updateOutOfWindow(this);
}

void VFOManager::VFO::setReference(int ref) {
    wtfVFO->setReference(ref);
    m_manager->updateOutOfWindow(this);
}

void VFOManager::VFO::setSnapInterval(double interval) {
    wtfVFO->setSnapInterval(interval);
}

void VFOManager::VFO::setBandwidthLimits(double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    wtfVFO->minBandwidth = minBandwidth;
    wtfVFO->maxBandwidth = maxBandwidth;
    wtfVFO->bandwidthLocked = bandwidthLocked;
}

bool VFOManager::VFO::getBandwidthChanged(bool erase) {
    bool val = wtfVFO->bandwidthChanged;
    if (erase) { wtfVFO->bandwidthChanged = false; }
    return val;
}

double VFOManager::VFO::getBandwidth() {
    return wtfVFO->bandwidth;
}

int VFOManager::VFO::getReference() {
    return wtfVFO->reference;
}

void VFOManager::VFO::setColor(ImU32 color) {
    wtfVFO->color = color;
}

std::string VFOManager::VFO::getName() {
    return name;
}

VFOManager::VFOManager() {
}

VFOManager::VFO* VFOManager::createVFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    if (vfos.find(name) != vfos.end() || name == "") {
        return NULL;
    }
    VFOManager::VFO* vfo = new VFO(name, reference, offset, bandwidth, sampleRate, minBandwidth, maxBandwidth, bandwidthLocked);
    vfo->m_manager = this;
    {
        std::lock_guard<std::mutex> lck(m_vfoMtx);
        vfos[name] = vfo;
    }
    applyDspOffset(vfo);
    updateOutOfWindow(vfo);
    onVfoCreated.emit(vfo);
    return vfo;
}

void VFOManager::deleteVFO(VFOManager::VFO* vfo) {
    std::string name = "";
    for (auto const& [_name, _vfo] : vfos) {
        if (_vfo == vfo) {
            name = _name;
            break;
        }
    }
    if (name == "") {
        return;
    }
    onVfoDelete.emit(vfo);
    {
        // Once erased under the lock, no commit can reach the VFO, so the
        // blocking teardown runs unlocked.
        std::lock_guard<std::mutex> lck(m_vfoMtx);
        vfos.erase(name);
    }
    delete vfo;
    onVfoDeleted.emit(name);
}

void VFOManager::setOffset(std::string name, double offset) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setOffset(offset);
}

double VFOManager::getOffset(std::string name) {
    if (vfos.find(name) == vfos.end()) {
        return 0;
    }
    return vfos[name]->getOffset();
}

void VFOManager::setCenterOffset(std::string name, double offset) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setCenterOffset(offset);
}

void VFOManager::setBandwidth(std::string name, double bandwidth, bool updateWaterfall) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setBandwidth(bandwidth, updateWaterfall);
}

void VFOManager::setSampleRate(std::string name, double sampleRate, double bandwidth) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setSampleRate(sampleRate, bandwidth);
}

void VFOManager::setReference(std::string name, int ref) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setReference(ref);
}

void VFOManager::setBandwidthLimits(std::string name, double minBandwidth, double maxBandwidth, bool bandwidthLocked) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    vfos[name]->setBandwidthLimits(minBandwidth, maxBandwidth, bandwidthLocked);
}

bool VFOManager::getBandwidthChanged(std::string name, bool erase) {
    if (vfos.find(name) == vfos.end()) {
        return false;
    }
    return vfos[name]->getBandwidthChanged(erase);
}

double VFOManager::getBandwidth(std::string name) {
    if (vfos.find(name) == vfos.end()) {
        return NAN;
    }
    return vfos[name]->getBandwidth();
}

int VFOManager::getReference(std::string name) {
    if (vfos.find(name) == vfos.end()) {
        return -1;
    }
    return vfos[name]->getReference();
}

void VFOManager::setColor(std::string name, ImU32 color) {
    if (vfos.find(name) == vfos.end()) {
        return;
    }
    return vfos[name]->setColor(color);
}

bool VFOManager::vfoExists(std::string name) {
    return (vfos.find(name) != vfos.end());
}

void VFOManager::updateFromWaterfall(ImGui::WaterFall* wtf) {
    for (auto const& [name, vfo] : vfos) {
        if (vfo->wtfVFO->centerOffsetChanged) {
            vfo->wtfVFO->centerOffsetChanged = false;
            applyDspOffset(vfo);
        }
        // Dragging also changes the passband edges and bandwidth.
        updateOutOfWindow(vfo);
    }
}

void VFOManager::applyDspOffset(VFO* vfo) {
    std::lock_guard<std::mutex> lck(m_vfoMtx);
    vfo->m_dspCentre.store(vfo->wtfVFO->centerOffset, std::memory_order_release);
    const double window = vfo->m_routed.load(std::memory_order_acquire) ? vfo->m_routedOffset.load(std::memory_order_acquire)
                                                                         : m_committedOffset.load(std::memory_order_acquire);
    vfo->dspVFO->setOffset(vfo->wtfVFO->centerOffset - window);
}

void VFOManager::updateOutOfWindow(VFO* vfo) {
    bool out = false;
    if (vfo->m_routed.load(std::memory_order_acquire)) {
        double lo = vfo->m_ownWindowOffset - (vfo->m_ownWindowWidth / 2.0);
        double hi = vfo->m_ownWindowOffset + (vfo->m_ownWindowWidth / 2.0);
        out = vfo->wtfVFO->lowerOffset < lo || vfo->wtfVFO->upperOffset > hi;
    }
    else if (m_windowActive) {
        double lo = m_windowOffset - (m_windowWidth / 2.0);
        double hi = m_windowOffset + (m_windowWidth / 2.0);
        out = vfo->wtfVFO->lowerOffset < lo || vfo->wtfVFO->upperOffset > hi;
    }
    if (out == vfo->wtfVFO->outOfWindow) { return; }
    vfo->wtfVFO->outOfWindow = out;
    vfo->wtfVFO->redrawRequired = true;
    vfo->dspVFO->setMuted(out);
}

void VFOManager::setIQWindow(double offset, double width) {
    m_windowActive = true;
    m_windowOffset = offset;
    m_windowWidth = width;
    for (auto const& [name, vfo] : vfos) { updateOutOfWindow(vfo); }
    sigpath::iqFrontEnd.setNarrowIQ(true, offset, width);
}

void VFOManager::clearIQWindow() {
    m_windowActive = false;
    m_windowOffset = 0.0;
    m_windowWidth = 0.0;
    for (auto const& [name, vfo] : vfos) { updateOutOfWindow(vfo); }
    commitIQWindowOffset(0.0);
    sigpath::iqFrontEnd.setNarrowIQ(false);
}

void VFOManager::commitIQWindowOffset(double offset) {
    std::lock_guard<std::mutex> lck(m_vfoMtx);
    m_committedOffset.store(offset, std::memory_order_release);
    for (auto const& [name, vfo] : vfos) {
        if (vfo->m_routed.load(std::memory_order_acquire)) { continue; }
        vfo->dspVFO->setOffset(vfo->m_dspCentre.load(std::memory_order_acquire) - offset);
    }
}

void VFOManager::muteAllFor(int inputSamples) {
    std::lock_guard<std::mutex> lck(m_vfoMtx);
    for (auto const& [name, vfo] : vfos) {
        if (!vfo->m_routed.load(std::memory_order_acquire)) { vfo->dspVFO->muteFor(inputSamples); }
    }
}

bool VFOManager::isOutOfWindow(const std::string& name) const {
    auto it = vfos.find(name);
    return it != vfos.end() && it->second->wtfVFO->outOfWindow;
}

bool VFOManager::routeVFOInput(const std::string& name, dsp::stream<dsp::complex_t>* input, double sampleRate,
                               double offset, double width) {
    auto it = vfos.find(name);
    if (it == vfos.end()) { return false; }
    VFO* vfo = it->second;
    if (!sigpath::iqFrontEnd.routeVFOInput(name, input, sampleRate)) { return false; }
    {
        std::lock_guard<std::mutex> lck(m_vfoMtx);
        vfo->m_routed.store(input != nullptr, std::memory_order_release);
        vfo->m_routedOffset.store(input ? offset : 0.0, std::memory_order_release);
    }
    vfo->m_ownWindowOffset = input ? offset : 0.0;
    vfo->m_ownWindowWidth = input ? width : 0.0;
    applyDspOffset(vfo);
    updateOutOfWindow(vfo);
    return true;
}

bool VFOManager::isVFORouted(const std::string& name) const {
    auto it = vfos.find(name);
    return it != vfos.end() && it->second->m_routed.load(std::memory_order_acquire);
}

void VFOManager::setVFOWindow(const std::string& name, double offset, double width) {
    auto it = vfos.find(name);
    if (it == vfos.end() || !it->second->m_routed.load(std::memory_order_acquire)) { return; }
    it->second->m_ownWindowOffset = offset;
    it->second->m_ownWindowWidth = width;
    updateOutOfWindow(it->second);
}

void VFOManager::commitVFOWindowOffset(const std::string& name, double offset) {
    std::lock_guard<std::mutex> lck(m_vfoMtx);
    auto it = vfos.find(name);
    if (it == vfos.end() || !it->second->m_routed.load(std::memory_order_acquire)) { return; }
    it->second->m_routedOffset.store(offset, std::memory_order_release);
    it->second->dspVFO->setOffset(it->second->m_dspCentre.load(std::memory_order_acquire) - offset);
}

void VFOManager::muteVFOFor(const std::string& name, int inputSamples) {
    std::lock_guard<std::mutex> lck(m_vfoMtx);
    auto it = vfos.find(name);
    if (it != vfos.end() && it->second->m_routed.load(std::memory_order_acquire)) { it->second->dspVFO->muteFor(inputSamples); }
}
