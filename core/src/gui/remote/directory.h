#pragma once
// Public receiver directories and the one service that fetches and caches
// them for every browser view. Plan: doc/todo/remote-receiver-browser.md.

#include "remote_entry.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace remote {

    struct FetchRequest {
        std::string etag;         // from the cache, for a conditional GET
        std::string lastModified;
        std::function<bool()> shouldCancel;
    };

    struct FetchResult {
        bool notModified = false; // 304: keep the cached entries
        std::vector<RemoteEntry> entries;
        std::string etag;
        std::string lastModified;
    };

    // One public directory. fetch() runs on the service's worker thread and
    // makes one request with a timeout, through core's libcurl (proxy and
    // TLS settings included). It throws std::runtime_error on failure.
    // Providers drop owner e-mail addresses while parsing.
    struct DirectoryProvider {
        virtual ~DirectoryProvider() = default;
        virtual std::string id() const = 0;   // cache file name, e.g. "kiwisdr.com"
        virtual std::string name() const = 0; // shown in the status line
        virtual FetchResult fetch(const FetchRequest& request) = 0;
    };

    // Conditional GET of a directory document: sends the validators, returns
    // the body, or sets notModified. Throws on transport errors and on any
    // status but 200 and 304.
    struct HttpDocument {
        bool notModified = false;
        std::string body;
        std::string etag;
        std::string lastModified;
    };
    HttpDocument fetchDocument(const std::string& url, const FetchRequest& request, size_t maxBody);

    class DirectoryService {
    public:
        struct ProviderStatus {
            std::string id;
            std::string name;
            bool loading = false;
            std::string error;       // last fetch failure; stale entries stay
            int64_t fetchedUnix = 0; // 0: never fetched
            size_t count = 0;
        };

        // The process-wide service. Created on first use; stopped by
        // shutdown() (core calls it before modules unload).
        static DirectoryService& instance();
        // Stop the worker, cancelling a transfer in flight. Idempotent.
        static void shutdown();

        // Load the caches and refresh stale providers. Cheap after the first
        // call; the browser calls it every time it opens.
        void ensureStarted();
        // Refetch: force ignores the 15 min cache age.
        void refresh(bool force);

        // Bumped whenever the entry set changes.
        uint64_t generation() const { return m_generation.load(); }
        std::vector<RemoteEntry> snapshot() const;
        std::vector<ProviderStatus> status() const;

        // For tests: a service over the given providers and cache folder.
        DirectoryService(std::string cacheDir, std::vector<std::unique_ptr<DirectoryProvider>> providers);
        ~DirectoryService();
        DirectoryService(const DirectoryService&) = delete;
        DirectoryService& operator=(const DirectoryService&) = delete;

        // Blocking: run one refresh pass on the calling thread (tests).
        void refreshNow(bool force);

        static constexpr int64_t kMaxAgeS = 15 * 60;

    private:
        struct Slot {
            std::unique_ptr<DirectoryProvider> provider;
            std::vector<RemoteEntry> entries;
            std::string etag;
            std::string lastModified;
            int64_t fetchedUnix = 0;
            bool cacheLoaded = false;
            bool loading = false;
            std::string error;
        };

        void workerFn();
        void runPass(bool force);
        void loadCache(Slot& slot);
        void saveCache(const Slot& slot);
        std::string cachePath(const Slot& slot) const;

        const std::string m_cacheDir;
        std::vector<Slot> m_slots;          // guarded by m_mtx (entries/status); provider fixed
        mutable std::mutex m_mtx;
        std::condition_variable m_cv;
        std::thread m_worker;
        bool m_started = false;
        bool m_requested = false;
        bool m_requestForce = false;
        std::atomic<bool> m_stopping{ false };
        std::atomic<uint64_t> m_generation{ 1 };
    };

    // Unix time now, seconds.
    int64_t nowUnix();

}
