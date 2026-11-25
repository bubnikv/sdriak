#include <utils/gzip.h>

#include <zlib.h>

namespace gzip {
    bool isGzip(const void* data, size_t len) {
        const auto* b = static_cast<const uint8_t*>(data);
        return len >= 2 && b[0] == 0x1f && b[1] == 0x8b;
    }

    bool gunzip(const void* data, size_t len, std::string& out, size_t maxOut) {
        out.clear();
        z_stream zs{};
        // 32 + MAX_WBITS: detect gzip or zlib headers.
        if (inflateInit2(&zs, 32 + MAX_WBITS) != Z_OK) { return false; }
        zs.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(data));
        zs.avail_in = (uInt)len;
        char buf[16384];
        int ret = Z_OK;
        while (ret != Z_STREAM_END) {
            zs.next_out = reinterpret_cast<Bytef*>(buf);
            zs.avail_out = sizeof(buf);
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) { break; }
            out.append(buf, sizeof(buf) - zs.avail_out);
            if (out.size() > maxOut) { ret = Z_MEM_ERROR; break; }
            if (ret == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) { ret = Z_DATA_ERROR; break; }
        }
        inflateEnd(&zs);
        if (ret != Z_STREAM_END) {
            out.clear();
            return false;
        }
        return true;
    }
}
