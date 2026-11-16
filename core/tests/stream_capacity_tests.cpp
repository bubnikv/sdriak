#include "test_support.h"

#include <dsp/stream.h>
#include <dsp/compression/sample_stream_decompressor.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using test_support::expect;

void testCapacityTracksAllocation() {
    dsp::stream<float> s;
    expect(s.capacity() == STREAM_BUFFER_SIZE, "new stream does not report STREAM_BUFFER_SIZE");

    s.setBufferSize(1234);
    expect(s.capacity() == 1234, "setBufferSize() did not update capacity()");

    // A swap within capacity still works; the reader side is never touched,
    // and the first swap of a fresh stream does not wait.
    expect(s.swap(1234), "swap at exactly capacity failed");
}

// Builds an encoded packet of `samples` samples in the given format. Sample i
// decodes to (i, -i) scaled into the format's range.
std::vector<uint8_t> encode(dsp::compression::PCMType type, int samples, float scaler) {
    using D = dsp::compression::SampleStreamDecompressor;
    const int width = D::bytesPerSample((uint16_t)type);
    std::vector<uint8_t> packet((size_t)D::HEADER_SIZE + (size_t)samples * width, 0);
    *(uint16_t*)&packet[2] = (uint16_t)type;
    *(float*)&packet[4] = scaler;
    uint8_t* data = &packet[D::HEADER_SIZE];
    for (int i = 0; i < samples; i++) {
        const int v = i % 100;
        if (type == dsp::compression::PCM_TYPE_F32) {
            float re = (float)v, im = (float)-v;
            memcpy(&data[(size_t)i * 8], &re, 4);
            memcpy(&data[(size_t)i * 8 + 4], &im, 4);
        }
        else if (type == dsp::compression::PCM_TYPE_I8) {
            data[(size_t)i * 2] = (uint8_t)(int8_t)v;
            data[(size_t)i * 2 + 1] = (uint8_t)(int8_t)-v;
        }
    }
    return packet;
}

// Feeds one packet through a running decompressor whose output holds only
// `outCapacity` samples, and checks it arrives as capacity-sized pieces that
// together carry every sample.
void expectSplit(dsp::compression::PCMType type, int samples, int outCapacity, float expectScale) {
    const std::vector<uint8_t> packet = encode(type, samples, 1.0f);

    dsp::stream<uint8_t> in;
    in.setBufferSize((int)packet.size());
    dsp::compression::SampleStreamDecompressor decomp(&in);
    decomp.out.setBufferSize(outCapacity);
    decomp.start();

    memcpy(in.writeBuf, packet.data(), packet.size());
    expect(in.swap((int)packet.size()), "could not publish the encoded packet");

    int received = 0;
    while (received < samples) {
        const int n = decomp.out.read();
        expect(n > 0, "decompressor stopped before publishing every sample");
        expect(n <= outCapacity, "decompressor published more than its output holds");
        for (int k = 0; k < n; k++) {
            const int v = (received + k) % 100;
            const dsp::complex_t s = decomp.out.readBuf[k];
            if (s.re != (float)v * expectScale || s.im != (float)-v * expectScale) {
                decomp.out.flush();
                decomp.stop();
                expect(false, "decompressor output does not match the encoded samples");
            }
        }
        received += n;
        decomp.out.flush();
    }
    expect(received == samples, "decompressor published the wrong number of samples");

    decomp.stop();
}

void testDecompressorSplitsToCapacity() {
    // F32: one packet of 2500 samples into a 1000-sample stream -> 1000, 1000, 500.
    expectSplit(dsp::compression::PCM_TYPE_F32, 2500, 1000, 1.0f);
    // I8 carries four samples per complex-sized byte span, the case that used
    // to overrun a same-sized output by 4x.
    expectSplit(dsp::compression::PCM_TYPE_I8, 4000, 1000, 1.0f / 128.0f);
}

} // namespace

int main() {
    return test_support::run("stream_capacity_tests", [] {
        testCapacityTracksAllocation();
        testDecompressorSplitsToCapacity();
    });
}
