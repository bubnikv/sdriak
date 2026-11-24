#include "curl_http.h"

#include <stdexcept>
#include <utility>

#include <utils/curl_init.h>

namespace net::http {

    namespace {
        struct ResponseBuffer {
            std::string body;
            size_t maxBody = 0;
            bool overflow = false;
        };

        struct SListGuard {
            curl_slist* list = nullptr;

            SListGuard() = default;

            ~SListGuard() {
                if (list) {
                    curl_slist_free_all(list);
                }
            }

            SListGuard(const SListGuard&) = delete;
            SListGuard& operator=(const SListGuard&) = delete;
        };

        struct EasyGuard {
            CURL* handle = nullptr;

            explicit EasyGuard(CURL* handle) : handle(handle) {}

            ~EasyGuard() {
                if (handle) {
                    curl_easy_cleanup(handle);
                }
            }

            EasyGuard(const EasyGuard&) = delete;
            EasyGuard& operator=(const EasyGuard&) = delete;
        };

        size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
            size_t total = size * nmemb;
            auto* buffer = static_cast<ResponseBuffer*>(userdata);

            if (buffer->maxBody > 0 && buffer->body.size() + total > buffer->maxBody) {
                buffer->overflow = true;
                return 0;
            }

            buffer->body.append(ptr, total);
            return total;
        }

        int xferInfo(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            const auto* cb = static_cast<const std::function<bool()>*>(clientp);
            return (*cb && (*cb)()) ? 1 : 0;
        }

        void setOption(CURL* handle, CURLoption option, long value, const char* name) {
            CURLcode rc = curl_easy_setopt(handle, option, value);
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(") + name + ") failed: " + curl_easy_strerror(rc));
            }
        }
    }

    CurlResponse curlGet(const std::string& url, const CurlRequestOptions& options) {
        CurlRequest request;
        request.url = url;
        return curlRequest(request, options);
    }

    namespace {
        class UrlGuard {
        public:
            explicit UrlGuard(const std::string& url) : m_url(curl_url()) {
                m_ok = m_url && curl_url_set(m_url, CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK;
            }
            ~UrlGuard() { curl_url_cleanup(m_url); }
            UrlGuard(const UrlGuard&) = delete;
            UrlGuard& operator=(const UrlGuard&) = delete;

            bool ok() const { return m_ok; }

            std::string get(CURLUPart part, unsigned int flags = 0) const {
                char* value = nullptr;
                if (!m_ok || curl_url_get(m_url, part, &value, flags) != CURLUE_OK || !value) { return {}; }
                std::string out(value);
                curl_free(value);
                return out;
            }

        private:
            CURLU* m_url;
            bool m_ok = false;
        };
    }

    std::string urlOrigin(const std::string& url) {
        UrlGuard u(url);
        if (!u.ok()) { return {}; }
        std::string origin = u.get(CURLUPART_SCHEME) + "://" + u.get(CURLUPART_HOST);
        const std::string port = u.get(CURLUPART_PORT, CURLU_NO_DEFAULT_PORT);
        if (!port.empty()) { origin += ":" + port; }
        return origin;
    }

    std::string urlPath(const std::string& url) {
        UrlGuard u(url);
        if (!u.ok()) { return {}; }
        const std::string path = u.get(CURLUPART_PATH);
        return path.empty() ? "/" : path;
    }

    CurlResponse curlRequest(const CurlRequest& request, const CurlRequestOptions& options) {
        const std::string& url = request.url;
        EasyGuard easy(curl::make_easy(url));
        if (!easy.handle) {
            throw std::runtime_error("curl_easy_init failed");
        }

        char error[CURL_ERROR_SIZE] = {};
        ResponseBuffer buffer;
        buffer.maxBody = options.maxBody;

        CURLcode rc = curl_easy_setopt(easy.handle, CURLOPT_ERRORBUFFER, error);
        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_ERRORBUFFER) failed: ") + curl_easy_strerror(rc));
        }

        rc = curl_easy_setopt(easy.handle, CURLOPT_URL, url.c_str());
        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_URL) failed: ") + curl_easy_strerror(rc));
        }

        rc = curl_easy_setopt(easy.handle, CURLOPT_WRITEFUNCTION, writeBody);
        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_WRITEFUNCTION) failed: ") + curl_easy_strerror(rc));
        }

        rc = curl_easy_setopt(easy.handle, CURLOPT_WRITEDATA, &buffer);
        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_WRITEDATA) failed: ") + curl_easy_strerror(rc));
        }

        setOption(easy.handle, CURLOPT_CONNECTTIMEOUT_MS, options.connectTimeoutMs, "CURLOPT_CONNECTTIMEOUT_MS");
        if (options.timeoutMs > 0) {
            setOption(easy.handle, CURLOPT_TIMEOUT_MS, options.timeoutMs, "CURLOPT_TIMEOUT_MS");
        }
        setOption(easy.handle, CURLOPT_MAXREDIRS, options.maxRedirects, "CURLOPT_MAXREDIRS");

        if (options.shouldCancel) {
            rc = curl_easy_setopt(easy.handle, CURLOPT_XFERINFOFUNCTION, xferInfo);
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_XFERINFOFUNCTION) failed: ") + curl_easy_strerror(rc));
            }
            rc = curl_easy_setopt(easy.handle, CURLOPT_XFERINFODATA, &options.shouldCancel);
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_XFERINFODATA) failed: ") + curl_easy_strerror(rc));
            }
            setOption(easy.handle, CURLOPT_NOPROGRESS, 0, "CURLOPT_NOPROGRESS");
        }

        const bool post = request.method == "POST";
        if (post || !request.body.empty()) {
            // POSTFIELDS is not copied; request outlives the perform below.
            setOption(easy.handle, CURLOPT_POST, 1L, "CURLOPT_POST");
            rc = curl_easy_setopt(easy.handle, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request.body.size());
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_POSTFIELDSIZE_LARGE) failed: ") + curl_easy_strerror(rc));
            }
            rc = curl_easy_setopt(easy.handle, CURLOPT_POSTFIELDS, request.body.data());
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_POSTFIELDS) failed: ") + curl_easy_strerror(rc));
            }
        }
        if (!request.method.empty() && request.method != "GET" && !post) {
            rc = curl_easy_setopt(easy.handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_CUSTOMREQUEST) failed: ") + curl_easy_strerror(rc));
            }
        }

        SListGuard headers;
        auto appendHeader = [&headers](const std::string& header) {
            curl_slist* next = curl_slist_append(headers.list, header.c_str());
            if (!next) {
                throw std::runtime_error("curl_slist_append failed");
            }
            headers.list = next;
        };
        if (!request.contentType.empty() && !options.headers.count("Content-Type")) {
            appendHeader("Content-Type: " + request.contentType);
        }
        for (const auto& [name, value] : options.headers) {
            appendHeader(name + ": " + value);
        }
        if (headers.list) {
            rc = curl_easy_setopt(easy.handle, CURLOPT_HTTPHEADER, headers.list);
            if (rc != CURLE_OK) {
                throw std::runtime_error(std::string("curl_easy_setopt(CURLOPT_HTTPHEADER) failed: ") + curl_easy_strerror(rc));
            }
        }

        rc = curl_easy_perform(easy.handle);
        if (rc != CURLE_OK) {
            if (buffer.overflow) {
                throw std::runtime_error("HTTP response exceeded maximum body size");
            }
            const char* detail = error[0] ? error : curl_easy_strerror(rc);
            throw std::runtime_error(std::string("curl_easy_perform failed: ") + detail);
        }

        CurlResponse response;
        response.body = std::move(buffer.body);

        rc = curl_easy_getinfo(easy.handle, CURLINFO_RESPONSE_CODE, &response.status);
        if (rc != CURLE_OK) {
            throw std::runtime_error(std::string("curl_easy_getinfo(CURLINFO_RESPONSE_CODE) failed: ") + curl_easy_strerror(rc));
        }

        char* effectiveUrl = nullptr;
        rc = curl_easy_getinfo(easy.handle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        if (rc == CURLE_OK && effectiveUrl) {
            response.effectiveUrl = effectiveUrl;
        }

        return response;
    }

}
