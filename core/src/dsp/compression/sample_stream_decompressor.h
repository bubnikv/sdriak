#pragma once
#include <algorithm>
#include "../processor.h"
#include "pcm_type.h"

namespace dsp::compression {
    // Decodes SampleStreamCompressor packets: an 8-byte header (compression
    // type, sample type, scaler) followed by the samples.
    //
    // One packet can hold more samples than the output stream: I16 and I8
    // carry two and four times as many samples per byte as the complex
    // output, and a desktop server's blocks can exceed a 32-bit Android
    // client's STREAM_BUFFER_SIZE. run() therefore publishes each packet in
    // pieces of at most out.capacity() samples.
    class SampleStreamDecompressor : public Processor<uint8_t, complex_t> {
        using base_type = Processor<uint8_t, complex_t>;
    public:
        static constexpr int HEADER_SIZE = 8;

        SampleStreamDecompressor() {}

        SampleStreamDecompressor(stream<uint8_t>* in) { base_type::init(in); }

        // Bytes one encoded sample occupies, or 0 for an unknown type.
        static int bytesPerSample(uint16_t sampleType) {
            switch (sampleType) {
                case PCMType::PCM_TYPE_F32: return sizeof(complex_t);
                case PCMType::PCM_TYPE_I16: return sizeof(int16_t) * 2;
                case PCMType::PCM_TYPE_I8:  return sizeof(int8_t) * 2;
                default:                    return 0;
            }
        }

        // Decodes n samples from data (already offset past the header and any
        // samples published before) into out.
        static void decode(uint16_t sampleType, float scaler, const uint8_t* data, complex_t* out, int n) {
            if (sampleType == PCMType::PCM_TYPE_F32) {
                memcpy(out, data, (size_t)n * sizeof(complex_t));
            }
            else if (sampleType == PCMType::PCM_TYPE_I16) {
                volk_16i_s32f_convert_32f((float*)out, (const int16_t*)data, 32768.0f / scaler, n * 2);
            }
            else if (sampleType == PCMType::PCM_TYPE_I8) {
                volk_8i_s32f_convert_32f((float*)out, (const int8_t*)data, 128.0f / scaler, n * 2);
            }
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            // The input buffer stays held until every piece is published,
            // because each piece is decoded from it.
            const uint8_t* in = base_type::_in->readBuf;
            if (count >= HEADER_SIZE) {
                const uint16_t sampleType = *(const uint16_t*)&in[2];
                const float scaler = *(const float*)&in[4];
                const int width = bytesPerSample(sampleType);
                if (width) {
                    const int total = (count - HEADER_SIZE) / width;
                    for (int done = 0; done < total;) {
                        const int n = (std::min)(total - done, base_type::out.capacity());
                        decode(sampleType, scaler, &in[HEADER_SIZE + (size_t)done * width], base_type::out.writeBuf, n);
                        if (!base_type::out.swap(n)) {
                            base_type::_in->flush();
                            return -1;
                        }
                        done += n;
                    }
                }
            }

            base_type::_in->flush();
            return count;
        }
    };
}
