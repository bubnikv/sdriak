// FLAC and 24-bit PCM writing adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include "wav.h"
#include <volk/volk.h>
#include <stdexcept>
#include <dsp/buffer/buffer.h>
#include <dsp/stream.h>
#include <utils/flog.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>
#include <map>
#include <FLAC/stream_encoder.h>
#include <ogg/ogg.h>
#include <opus/opus.h>

namespace wav {
    const char* WAVE_FILE_TYPE          = "WAVE";
    const char* FORMAT_MARKER           = "fmt ";
    const char* DATA_MARKER             = "data";
    const uint32_t FORMAT_HEADER_LEN    = 16;
    const uint16_t SAMPLE_TYPE_PCM      = 1;

    // Opus always decodes at 48 kHz; granule positions and the pre-skip in the
    // Ogg-Opus header (RFC 7845) are counted in 48 kHz samples.
    const int OPUS_DECODE_RATE          = 48000;

    std::map<SampleType, int> SAMP_BITS = {
        { SAMP_TYPE_UINT8, 8 },
        { SAMP_TYPE_INT16, 16 },
        { SAMP_TYPE_INT24, 24 },
        { SAMP_TYPE_INT32, 32 },
        { SAMP_TYPE_FLOAT32, 32 }
    };

    // ---------------------------------------------------------------------
    // Ogg-Opus muxer (RFC 7845). Encapsulates Opus packets from the float
    // encode API into a standard .opus file. Held opaquely by Writer via a
    // void* so opus/ogg headers stay out of wav.h.
    // ---------------------------------------------------------------------
    struct OggOpusEncoder {
        OpusEncoder* enc = NULL;
        ogg_stream_state os;
        bool osInit = false;
        FILE* file = NULL;

        int channels = 2;
        int inRate = 48000;       // encoder input sample rate (Hz)
        int frameSize = 960;      // input samples/channel per Opus frame (20 ms)
        int preskip = 0;          // encoder delay, in 48 kHz samples

        ogg_int64_t granulepos = 0;    // cumulative 48 kHz samples encoded
        ogg_int64_t inputSamples = 0;  // cumulative input samples/channel fed (pre-pad)
        ogg_int64_t packetno = 0;

        std::vector<float> fifo;             // interleaved accumulation
        std::vector<float> held;             // one-frame hold-back (for EOS on the true last frame)
        bool hasHeld = false;
        std::vector<unsigned char> packet;   // encoder output buffer
        bool errLogged = false;
    };

    static void oggWritePage(FILE* f, ogg_page* og) {
        fwrite(og->header, 1, og->header_len, f);
        fwrite(og->body, 1, og->body_len, f);
    }

    static void putLE16(std::vector<unsigned char>& v, uint16_t x) {
        v.push_back((unsigned char)(x & 0xff));
        v.push_back((unsigned char)((x >> 8) & 0xff));
    }

    static void putLE32(std::vector<unsigned char>& v, uint32_t x) {
        v.push_back((unsigned char)(x & 0xff));
        v.push_back((unsigned char)((x >> 8) & 0xff));
        v.push_back((unsigned char)((x >> 16) & 0xff));
        v.push_back((unsigned char)((x >> 24) & 0xff));
    }

    // Write the OpusHead + OpusTags packets, each forced onto its own page.
    static bool oggOpusWriteHeaders(OggOpusEncoder* e) {
        ogg_page og;
        ogg_packet op;

        // OpusHead (channel mapping family 0 covers mono/stereo)
        std::vector<unsigned char> head;
        const char* headMagic = "OpusHead";
        head.insert(head.end(), headMagic, headMagic + 8);
        head.push_back(1);                              // version
        head.push_back((unsigned char)e->channels);     // channel count
        putLE16(head, (uint16_t)e->preskip);            // pre-skip
        putLE32(head, (uint32_t)e->inRate);             // original input rate (informational)
        putLE16(head, 0);                               // output gain
        head.push_back(0);                              // channel mapping family
        memset(&op, 0, sizeof(op));
        op.packet = head.data();
        op.bytes = (long)head.size();
        op.b_o_s = 1;
        op.granulepos = 0;
        op.packetno = e->packetno++;
        if (ogg_stream_packetin(&e->os, &op)) { return false; }
        while (ogg_stream_flush(&e->os, &og)) { oggWritePage(e->file, &og); }

        // OpusTags (empty user comment list)
        const char* vendor = opus_get_version_string();
        uint32_t vlen = (uint32_t)strlen(vendor);
        std::vector<unsigned char> tags;
        const char* tagsMagic = "OpusTags";
        tags.insert(tags.end(), tagsMagic, tagsMagic + 8);
        putLE32(tags, vlen);
        tags.insert(tags.end(), vendor, vendor + vlen);
        putLE32(tags, 0);                               // user comment list length
        memset(&op, 0, sizeof(op));
        op.packet = tags.data();
        op.bytes = (long)tags.size();
        op.granulepos = 0;
        op.packetno = e->packetno++;
        if (ogg_stream_packetin(&e->os, &op)) { return false; }
        while (ogg_stream_flush(&e->os, &og)) { oggWritePage(e->file, &og); }
        return true;
    }

    // Encode one full frame (frameSize samples/channel in pcm) and emit page(s).
    // On the last frame the granule position is trimmed to the real input
    // length (pre-skip + fed samples at 48 kHz) so decoders drop the encoder
    // delay and the zero padding of the final frame.
    static bool oggOpusEncodeFrame(OggOpusEncoder* e, const float* pcm, bool last) {
        opus_int32 nb = opus_encode_float(e->enc, pcm, e->frameSize, e->packet.data(), (opus_int32)e->packet.size());
        if (nb < 0) {
            if (!e->errLogged) {
                flog::error("opus_encode_float() failed: {}", opus_strerror(nb));
                e->errLogged = true;
            }
            return false;
        }
        e->granulepos += (ogg_int64_t)e->frameSize * OPUS_DECODE_RATE / e->inRate;

        ogg_packet op;
        memset(&op, 0, sizeof(op));
        op.packet = e->packet.data();
        op.bytes = nb;
        op.e_o_s = last ? 1 : 0;
        op.granulepos = last ? (e->preskip + e->inputSamples * OPUS_DECODE_RATE / e->inRate) : e->granulepos;
        op.packetno = e->packetno++;
        if (ogg_stream_packetin(&e->os, &op)) { return false; }

        ogg_page og;
        // pageout during streaming holds the tail back (buffered in os); flush
        // forces everything out, so the EOS packet's page is emitted at close.
        if (last) {
            while (ogg_stream_flush(&e->os, &og)) { oggWritePage(e->file, &og); }
        }
        else {
            while (ogg_stream_pageout(&e->os, &og)) { oggWritePage(e->file, &og); }
        }
        return true;
    }

    static void oggOpusDestroy(OggOpusEncoder* e) {
        if (!e) { return; }
        if (e->osInit) { ogg_stream_clear(&e->os); }
        if (e->enc) { opus_encoder_destroy(e->enc); }
        if (e->file) { fclose(e->file); }
        delete e;
    }

    Writer::Writer(int channels, uint64_t samplerate, Format format, SampleType type) {
        // Validate channels and samplerate
        if (channels < 1) { throw std::runtime_error("Channel count must be greater or equal to 1"); }
        if (!samplerate) { throw std::runtime_error("Samplerate must be non-zero"); }

        // Initialize variables
        _channels = channels;
        _samplerate = samplerate;
        _format = format;
        _type = type;
    }

    Writer::~Writer() { close(); }

    bool Writer::open(std::string path) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Close previous file
        if (isOpenInt()) { close(); }

        // Reset work values
        samplesWritten = 0;
        writeErrorLogged = false;
        auto bitsIt = SAMP_BITS.find(_type);
        if (bitsIt == SAMP_BITS.end()) {
            flog::error("Unknown sample type: {}", (int)_type);
            return false;
        }
        int bits = bitsIt->second;
        bytesPerSamp = (bits / 8) * _channels;

        // Symmetric full-scale factor (2^(bits-1) - 1), the same convention as
        // the volk int16/int32 conversions below: 0 maps to 0 (no DC offset on
        // silence) and clamped +/-1.0 input can't overflow the integer range,
        // at the cost of the deepest negative code staying unused. Computed in
        // double so the scale and products are exact for depths up to 32 bits.
        intScale = (double)((1ull << (bits - 1)) - 1);

        if (_format == FORMAT_FLAC) {
            // The FLAC codec takes integer samples only
            if (_type == SAMP_TYPE_FLOAT32) {
                flog::error("FLAC container requires an integer sample type");
                return false;
            }

            // Create and configure the encoder
            FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
            if (!enc) {
                flog::error("FLAC__stream_encoder_new() failed");
                return false;
            }
            FLAC__stream_encoder_set_channels(enc, _channels);
            FLAC__stream_encoder_set_sample_rate(enc, (uint32_t)_samplerate);
            FLAC__stream_encoder_set_bits_per_sample(enc, bits);
            FLAC__stream_encoder_set_compression_level(enc, 5);

            // Open the output file. Fails (among others) for samplerates above
            // FLAC__MAX_SAMPLE_RATE (1048575 Hz; 655350 Hz on libFLAC < 1.4)
            // and for 32-bit samples on libFLAC < 1.4.
            auto status = FLAC__stream_encoder_init_file(enc, path.c_str(), NULL, NULL);
            if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
                flog::error("FLAC__stream_encoder_init_file() failed: {}", FLAC__StreamEncoderInitStatusString[status]);
                FLAC__stream_encoder_delete(enc);
                return false;
            }

            // Conversion buffer (the encoder takes sign-extended int32 at every depth)
            bufI32 = dsp::buffer::alloc<int32_t>(STREAM_BUFFER_SIZE * _channels);

            flacEnc = enc;
            return true;
        }

        if (_format == FORMAT_OPUS) {
            // Opus only accepts a fixed set of input sample rates and 1 or 2
            // channels; anything else (notably baseband IQ rates) is rejected.
            if (!isOpusSamplerateSupported(_samplerate)) {
                flog::error("Opus requires a sample rate of 8, 12, 16, 24 or 48 kHz (got {} Hz)", (unsigned long long)_samplerate);
                return false;
            }
            if (_channels != 1 && _channels != 2) {
                flog::error("Opus requires 1 or 2 channels (got {})", _channels);
                return false;
            }

            int err = OPUS_OK;
            OpusEncoder* enc = opus_encoder_create((opus_int32)_samplerate, _channels, OPUS_APPLICATION_AUDIO, &err);
            if (err != OPUS_OK || !enc) {
                flog::error("opus_encoder_create() failed: {}", opus_strerror(err));
                return false;
            }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(_opusBitrate));
            int lookahead = 0;
            opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

            FILE* f = fopen(path.c_str(), "wb");
            if (!f) {
                flog::error("Failed to open Opus output file: {}", path);
                opus_encoder_destroy(enc);
                return false;
            }

            OggOpusEncoder* e = new OggOpusEncoder();
            e->enc = enc;
            e->file = f;
            e->channels = _channels;
            e->inRate = (int)_samplerate;
            e->frameSize = (int)_samplerate / 50;   // 20 ms
            e->preskip = (int)((int64_t)lookahead * OPUS_DECODE_RATE / (int)_samplerate);
            e->packet.resize(4000);                 // recommended max Opus packet size
            e->held.resize((size_t)e->frameSize * _channels);

            // Serial number just has to identify this stream within the file;
            // uniqueness across files is not required for a single-stream .opus.
            int serial = (int)((uint32_t)time(NULL) * 2654435761u + (uint32_t)(uintptr_t)e);
            if (ogg_stream_init(&e->os, serial)) {
                flog::error("ogg_stream_init() failed");
                oggOpusDestroy(e);
                return false;
            }
            e->osInit = true;

            if (!oggOpusWriteHeaders(e)) {
                flog::error("Failed to write Ogg-Opus headers");
                oggOpusDestroy(e);
                return false;
            }

            opusState = e;
            return true;
        }

        // Fill header
        hdr.codec = (_type == SAMP_TYPE_FLOAT32) ? CODEC_FLOAT : CODEC_PCM;
        hdr.channelCount = _channels;
        hdr.sampleRate = _samplerate;
        hdr.bitDepth = bits;
        hdr.bytesPerSample = bytesPerSamp;
        hdr.bytesPerSecond = bytesPerSamp * _samplerate;

        // Precompute sizes and allocate buffers
        switch (_type) {
        case SAMP_TYPE_UINT8:
            bufU8 = dsp::buffer::alloc<uint8_t>(STREAM_BUFFER_SIZE * _channels);
            break;
        case SAMP_TYPE_INT16:
            bufI16 = dsp::buffer::alloc<int16_t>(STREAM_BUFFER_SIZE * _channels);
            break;
        case SAMP_TYPE_INT24:
            bufI24 = dsp::buffer::alloc<uint8_t>(STREAM_BUFFER_SIZE * _channels * 3);
            break;
        case SAMP_TYPE_INT32:
            bufI32 = dsp::buffer::alloc<int32_t>(STREAM_BUFFER_SIZE * _channels);
            break;
        case SAMP_TYPE_FLOAT32:
            break;
        default:
            return false;
            break;
        }

        // Open file
        if (!rw.open(path, WAVE_FILE_TYPE)) {
            freeBuffers();
            return false;
        }

        // Write format chunk
        rw.beginChunk(FORMAT_MARKER);
        rw.write((uint8_t*)&hdr, sizeof(FormatHeader));
        rw.endChunk();

        // Begin data chunk
        rw.beginChunk(DATA_MARKER);

        return true;
    }

    bool Writer::isOpenInt() {
        return flacEnc || opusState || rw.isOpen();
    }

    bool Writer::isOpen() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        return isOpenInt();
    }

    void Writer::close() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do nothing if the file is not open
        if (!isOpenInt()) { return; }

        if (flacEnc) {
            // Flush the encoder and finalize the file (rewrites STREAMINFO)
            FLAC__StreamEncoder* enc = (FLAC__StreamEncoder*)flacEnc;
            if (!FLAC__stream_encoder_finish(enc)) {
                flog::error("FLAC__stream_encoder_finish() failed");
            }
            FLAC__stream_encoder_delete(enc);
            flacEnc = NULL;
        }
        else if (opusState) {
            OggOpusEncoder* e = (OggOpusEncoder*)opusState;
            int perFrame = e->frameSize * e->channels;

            // Emit the final frame with the EOS flag set. The one-frame
            // hold-back guarantees the true last audio frame carries EOS even
            // when the stream ends exactly on a frame boundary.
            if (!e->fifo.empty()) {
                // A partial frame remains: flush the held frame (not last),
                // then zero-pad the remainder and encode it as the last frame.
                if (e->hasHeld) { oggOpusEncodeFrame(e, e->held.data(), false); }
                std::vector<float> tail((size_t)perFrame, 0.0f);
                std::copy(e->fifo.begin(), e->fifo.end(), tail.begin());
                oggOpusEncodeFrame(e, tail.data(), true);
            }
            else if (e->hasHeld) {
                oggOpusEncodeFrame(e, e->held.data(), true);
            }
            else {
                // No audio at all: encode one silent frame so the file has a
                // valid audio page carrying EOS (granule trims it to zero).
                std::vector<float> silent((size_t)perFrame, 0.0f);
                oggOpusEncodeFrame(e, silent.data(), true);
            }

            oggOpusDestroy(e);
            opusState = NULL;
        }
        else {
            // Finish data chunk
            rw.endChunk();

            // Close the file
            rw.close();
        }

        // Free buffers
        freeBuffers();
    }

    void Writer::freeBuffers() {
        if (bufU8) {
            dsp::buffer::free(bufU8);
            bufU8 = NULL;
        }
        if (bufI16) {
            dsp::buffer::free(bufI16);
            bufI16 = NULL;
        }
        if (bufI24) {
            dsp::buffer::free(bufI24);
            bufI24 = NULL;
        }
        if (bufI32) {
            dsp::buffer::free(bufI32);
            bufI32 = NULL;
        }
    }

    void Writer::setChannels(int channels) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (isOpenInt()) { throw std::runtime_error("Cannot change parameters while file is open"); }

        // Validate channel count
        if (channels < 1) { throw std::runtime_error("Channel count must be greater or equal to 1"); }
        _channels = channels;
    }

    void Writer::setSamplerate(uint64_t samplerate) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (isOpenInt()) { throw std::runtime_error("Cannot change parameters while file is open"); }

        // Validate samplerate
        if (!samplerate) { throw std::runtime_error("Samplerate must be non-zero"); }
        _samplerate = samplerate;
    }

    void Writer::setFormat(Format format) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (isOpenInt()) { throw std::runtime_error("Cannot change parameters while file is open"); }
        _format = format;
    }

    void Writer::setSampleType(SampleType type) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (isOpenInt()) { throw std::runtime_error("Cannot change parameters while file is open"); }
        _type = type;
    }

    void Writer::setOpusBitrate(int bitrate) {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        // Do not allow settings to change while open
        if (isOpenInt()) { throw std::runtime_error("Cannot change parameters while file is open"); }
        _opusBitrate = bitrate;
    }

    bool Writer::isOpusSamplerateSupported(uint64_t samplerate) {
        return samplerate == 8000 || samplerate == 12000 || samplerate == 16000 ||
               samplerate == 24000 || samplerate == 48000;
    }

    std::string Writer::getFileExtension() {
        std::lock_guard<std::recursive_mutex> lck(mtx);
        switch (_format) {
        case FORMAT_FLAC: return ".flac";
        case FORMAT_OPUS: return ".opus";
        default:          return ".wav";
        }
    }

    void Writer::write(float* samples, int count) {
        std::lock_guard<std::recursive_mutex> lck(mtx);

        int tcount = count * _channels;
        int tbytes = count * bytesPerSamp;

        if (flacEnc) {
            for (int i = 0; i < tcount; i++) {
                bufI32[i] = (int32_t)llround((double)std::clamp<float>(samples[i], -1.0f, 1.0f) * intScale);
            }
            FLAC__StreamEncoder* enc = (FLAC__StreamEncoder*)flacEnc;
            if (!FLAC__stream_encoder_process_interleaved(enc, bufI32, count)) {
                // Log the first failure only (this runs per DSP block)
                if (!writeErrorLogged) {
                    auto state = FLAC__stream_encoder_get_state(enc);
                    flog::error("FLAC__stream_encoder_process_interleaved() failed: {}", FLAC__StreamEncoderStateString[state]);
                    writeErrorLogged = true;
                }
                return;
            }
            samplesWritten += count;
            return;
        }

        if (opusState) {
            OggOpusEncoder* e = (OggOpusEncoder*)opusState;
            int perFrame = e->frameSize * e->channels;

            // Accumulate interleaved input, then drain complete frames with a
            // one-frame hold-back so the final frame can be flagged EOS at close.
            e->fifo.insert(e->fifo.end(), samples, samples + tcount);
            e->inputSamples += count;

            size_t pos = 0;
            while (e->fifo.size() - pos >= (size_t)perFrame) {
                if (e->hasHeld) {
                    if (!oggOpusEncodeFrame(e, e->held.data(), false)) { break; }
                }
                std::copy(e->fifo.begin() + pos, e->fifo.begin() + pos + perFrame, e->held.begin());
                e->hasHeld = true;
                pos += perFrame;
            }
            if (pos > 0) { e->fifo.erase(e->fifo.begin(), e->fifo.begin() + pos); }

            samplesWritten += count;
            return;
        }

        if (!rw.isOpen()) { return; }

        // Select different writer function depending on the chose depth
        switch (_type) {
        case SAMP_TYPE_UINT8:
            // Volk doesn't support unsigned ints yet :/
            for (int i = 0; i < tcount; i++) {
                bufU8[i] = (samples[i] * 127.0f) + 128.0f;
            }
            rw.write(bufU8, tbytes);
            break;
        case SAMP_TYPE_INT16:
            volk_32f_s32f_convert_16i(bufI16, samples, 32767.0f, tcount);
            rw.write((uint8_t*)bufI16, tbytes);
            break;
        case SAMP_TYPE_INT24:
        {
            // Little-endian 3-byte packing
            uint8_t* dst = bufI24;
            for (int i = 0; i < tcount; i++) {
                int32_t sval = (int32_t)llround((double)std::clamp<float>(samples[i], -1.0f, 1.0f) * intScale);
                *(dst++) = (uint8_t)sval;
                *(dst++) = (uint8_t)(sval >> 8);
                *(dst++) = (uint8_t)(sval >> 16);
            }
            rw.write(bufI24, tbytes);
            break;
        }
        case SAMP_TYPE_INT32:
            volk_32f_s32f_convert_32i(bufI32, samples, 2147483647.0f, tcount);
            rw.write((uint8_t*)bufI32, tbytes);
            break;
        case SAMP_TYPE_FLOAT32:
            rw.write((uint8_t*)samples, tbytes);
            break;
        default:
            break;
        }

        // Increment sample counter
        samplesWritten += count;
    }
}
