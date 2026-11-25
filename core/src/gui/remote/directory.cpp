#include "directory.h"
#include "providers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <core.h>
#include <utils/flog.h>
#include <utils/proto/curl_http.h>

namespace remote {

    int64_t nowUnix() {
        return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    HttpDocument fetchDocument(const std::string& url, const FetchRequest& request, size_t maxBody) {
        net::http::CurlRequestOptions options;
        options.connectTimeoutMs = 8000;
        options.timeoutMs = 60000;
        options.maxBody = maxBody;
        options.shouldCancel = request.shouldCancel;
        if (!request.etag.empty()) { options.headers["If-None-Match"] = request.etag; }
        if (!request.lastModified.empty()) { options.headers["If-Modified-Since"] = request.lastModified; }
        auto response = net::http::curlGet(url, options);
        flog::debug("Remote directory {}: HTTP {}", response.effectiveUrl, response.status);
        HttpDocument doc;
        if (response.status == 304) {
            doc.notModified = true;
            doc.etag = request.etag;
            doc.lastModified = request.lastModified;
            return doc;
        }
        if (response.status != 200) {
            throw std::runtime_error("HTTP status " + std::to_string(response.status));
        }
        doc.body = std::move(response.body);
        doc.etag = response.etag;
        doc.lastModified = response.lastModified;
        return doc;
    }

    namespace {
        std::mutex g_instanceMtx;
        DirectoryService* g_instance = nullptr;
        bool g_shutDown = false;

        std::vector<std::unique_ptr<DirectoryProvider>> builtInProviders() {
            std::vector<std::unique_ptr<DirectoryProvider>> out;
            out.push_back(makeKiwiSDRProvider());
            out.push_back(makeUberSDRProvider());
            out.push_back(makeSpyServerProvider());
            out.push_back(makeReceiverbookProvider());
            return out;
        }
    }

    DirectoryService& DirectoryService::instance() {
        std::lock_guard lck(g_instanceMtx);
        if (!g_instance) {
            // Leaked on purpose after shutdown(): a view drawn during exit
            // still finds a valid, stopped service.
            std::string root = core::args["root"].s();
            g_instance = new DirectoryService(root + "/remote_cache", builtInProviders());
            if (g_shutDown) { g_instance->m_stopping = true; }
        }
        return *g_instance;
    }

    void DirectoryService::shutdown() {
        std::lock_guard lck(g_instanceMtx);
        g_shutDown = true;
        if (!g_instance) { return; }
        {
            std::lock_guard l(g_instance->m_mtx);
            g_instance->m_stopping = true;
        }
        g_instance->m_cv.notify_all();
        if (g_instance->m_worker.joinable()) { g_instance->m_worker.join(); }
    }

    DirectoryService::DirectoryService(std::string cacheDir, std::vector<std::unique_ptr<DirectoryProvider>> providers)
        : m_cacheDir(std::move(cacheDir)) {
        for (auto& p : providers) {
            Slot slot;
            slot.provider = std::move(p);
            m_slots.push_back(std::move(slot));
        }
    }

    DirectoryService::~DirectoryService() {
        {
            std::lock_guard l(m_mtx);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) { m_worker.join(); }
    }

    void DirectoryService::ensureStarted() {
        std::lock_guard l(m_mtx);
        if (m_started || m_stopping) { return; }
        m_started = true;
        m_requested = true;
        m_requestForce = false;
        m_worker = std::thread(&DirectoryService::workerFn, this);
    }

    void DirectoryService::refresh(bool force) {
        {
            std::lock_guard l(m_mtx);
            if (m_stopping) { return; }
            m_requested = true;
            m_requestForce = m_requestForce || force;
            if (!m_started) {
                m_started = true;
                m_worker = std::thread(&DirectoryService::workerFn, this);
            }
        }
        m_cv.notify_all();
    }

    std::vector<RemoteEntry> DirectoryService::snapshot() const {
        std::lock_guard l(m_mtx);
        std::vector<RemoteEntry> out;
        for (const auto& s : m_slots) { out.insert(out.end(), s.entries.begin(), s.entries.end()); }
        return out;
    }

    std::vector<DirectoryService::ProviderStatus> DirectoryService::status() const {
        std::lock_guard l(m_mtx);
        std::vector<ProviderStatus> out;
        for (const auto& s : m_slots) {
            ProviderStatus st;
            st.id = s.provider->id();
            st.name = s.provider->name();
            st.loading = s.loading;
            st.error = s.error;
            st.fetchedUnix = s.fetchedUnix;
            st.count = s.entries.size();
            out.push_back(std::move(st));
        }
        return out;
    }

    void DirectoryService::workerFn() {
        while (true) {
            bool force = false;
            {
                std::unique_lock l(m_mtx);
                m_cv.wait(l, [this] { return m_requested || m_stopping; });
                if (m_stopping) { return; }
                force = m_requestForce;
                m_requested = false;
                m_requestForce = false;
            }
            runPass(force);
        }
    }

    void DirectoryService::refreshNow(bool force) {
        runPass(force);
    }

    std::string DirectoryService::cachePath(const Slot& slot) const {
        return m_cacheDir + "/" + slot.provider->id() + ".json";
    }

    void DirectoryService::loadCache(Slot& slot) {
        // Worker thread; the slot's provider is fixed, entries under m_mtx.
        std::vector<RemoteEntry> entries;
        std::string etag, lastModified;
        int64_t fetched = 0;
        try {
            std::ifstream in(cachePath(slot), std::ios::binary);
            if (in) {
                std::stringstream ss;
                ss << in.rdbuf();
                const auto doc = nlohmann::json::parse(ss.str());
                fetched = doc.value("fetched", (int64_t)0);
                etag = doc.value("etag", std::string());
                lastModified = doc.value("lastModified", std::string());
                if (doc.contains("entries") && doc["entries"].is_array()) {
                    for (const auto& j : doc["entries"]) {
                        if (auto e = entryFromJson(j)) { entries.push_back(std::move(*e)); }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            flog::warn("Remote directory cache {} unreadable: {}", cachePath(slot), e.what());
            entries.clear();
            fetched = 0;
        }
        std::lock_guard l(m_mtx);
        slot.cacheLoaded = true;
        if (!entries.empty()) {
            slot.entries = std::move(entries);
            slot.etag = etag;
            slot.lastModified = lastModified;
            slot.fetchedUnix = fetched;
            m_generation++;
        }
    }

    void DirectoryService::saveCache(const Slot& slot) {
        nlohmann::json doc;
        nlohmann::json arr = nlohmann::json::array();
        {
            std::lock_guard l(m_mtx);
            doc["fetched"] = slot.fetchedUnix;
            doc["etag"] = slot.etag;
            doc["lastModified"] = slot.lastModified;
            for (const auto& e : slot.entries) { arr.push_back(entryToJson(e)); }
        }
        doc["entries"] = std::move(arr);
        try {
            std::filesystem::create_directories(m_cacheDir);
            const std::string path = cachePath(slot);
            const std::string tmp = path + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << doc.dump();
                if (!out) { throw std::runtime_error("write failed"); }
            }
            std::filesystem::rename(tmp, path);
        }
        catch (const std::exception& e) {
            flog::warn("Remote directory cache {} not saved: {}", cachePath(slot), e.what());
        }
    }

    void DirectoryService::runPass(bool force) {
        for (auto& slot : m_slots) {
            if (m_stopping) { return; }
            if (!slot.cacheLoaded) { loadCache(slot); }
            FetchRequest request;
            {
                std::lock_guard l(m_mtx);
                const bool fresh = slot.fetchedUnix > 0 && nowUnix() - slot.fetchedUnix < kMaxAgeS;
                if (fresh && !force) { continue; }
                slot.loading = true;
                request.etag = slot.entries.empty() ? std::string() : slot.etag;
                request.lastModified = slot.entries.empty() ? std::string() : slot.lastModified;
                m_generation++;
            }
            request.shouldCancel = [this] { return m_stopping.load(); };

            std::string failure;
            FetchResult result;
            try {
                result = slot.provider->fetch(request);
            }
            catch (const std::exception& e) {
                failure = e.what();
            }

            bool save = false;
            {
                std::lock_guard l(m_mtx);
                slot.loading = false;
                if (!failure.empty()) {
                    if (!m_stopping) { flog::warn("Remote directory {}: {}", slot.provider->name(), failure); }
                    slot.error = failure;
                }
                else {
                    slot.error.clear();
                    slot.fetchedUnix = nowUnix();
                    slot.etag = result.etag;
                    slot.lastModified = result.lastModified;
                    if (!result.notModified) {
                        flog::info("Remote directory {}: {} receivers", slot.provider->name(), result.entries.size());
                        slot.entries = std::move(result.entries);
                    }
                    save = true;
                }
                m_generation++;
            }
            if (save) { saveCache(slot); }
        }
    }

}
