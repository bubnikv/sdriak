#pragma once
// FLAC and 24-bit PCM writing adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include <string>
#include <fstream>
#include <stdint.h>
#include <mutex>
#include "riff.h"

namespace wav {
    #pragma pack(push, 1)
    struct FormatHeader {
        uint16_t codec;
        uint16_t channelCount;
        uint32_t sampleRate;
        uint32_t bytesPerSecond;
        uint16_t bytesPerSample;
        uint16_t bitDepth;
    };
    #pragma pack(pop)

    enum Format {
        FORMAT_WAV,
        FORMAT_RF64,
        FORMAT_FLAC,
        FORMAT_OPUS
    };

    // The recorder persists these as their integer value — append new entries
    // at the end, never renumber.
    enum SampleType {
        SAMP_TYPE_UINT8,
        SAMP_TYPE_INT16,
        SAMP_TYPE_INT32,
        SAMP_TYPE_FLOAT32,
        SAMP_TYPE_INT24
    };

    enum Codec {
        CODEC_PCM   = 1,
        CODEC_FLOAT = 3
    };

    class Writer {
    public:
        Writer(int channels = 2, uint64_t samplerate = 48000, Format format = FORMAT_WAV, SampleType type = SAMP_TYPE_INT16);
        ~Writer();

        bool open(std::string path);
        bool isOpen();
        void close();

        void setChannels(int channels);
        void setSamplerate(uint64_t samplerate);
        void setFormat(Format format);
        void setSampleType(SampleType type);
        void setOpusBitrate(int bitrate);

        std::string getFileExtension();

        // Opus only accepts these input sample rates (Hz). Other formats
        // accept any rate.
        static bool isOpusSamplerateSupported(uint64_t samplerate);

        size_t getSamplesWritten() { return samplesWritten; }

        void write(float* samples, int count);

    private:
        bool isOpenInt();
        void freeBuffers();

        std::recursive_mutex mtx;
        FormatHeader hdr;
        riff::Writer rw;

        // FLAC encoder handle. Opaque on purpose: FLAC__StreamEncoder is a
        // typedef of an anonymous struct (not forward-declarable), and
        // including <FLAC/stream_encoder.h> here would force the FLAC include
        // dirs onto every consumer of this header while libFLAC links PRIVATE
        // into sdrpp_core.
        void* flacEnc = NULL;

        // Ogg-Opus muxer state (opaque OggOpusEncoder defined in wav.cpp).
        // Keeps the opus/ogg headers out of this header, same reasoning as
        // flacEnc above.
        void* opusState = NULL;

        int _channels;
        uint64_t _samplerate;
        Format _format;
        SampleType _type;
        int _opusBitrate = 128000;
        size_t bytesPerSamp;
        double intScale;

        uint8_t* bufU8 = NULL;
        int16_t* bufI16 = NULL;
        uint8_t* bufI24 = NULL;
        int32_t* bufI32 = NULL;
        size_t samplesWritten = 0;
        bool writeErrorLogged = false;
    };
}
