/*
 * Adapted from the SDRPP distribution (https://github.com/qrp73/SDRPP).
 * Copyright (c) 2023 qrp73.
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utils/flog.h>
#include "sample_reader.h"

// Reads RIFF and RF64 WAV files with any chunk layout, including
// WAVE_FORMAT_EXTENSIBLE headers. Files whose headers declare a wrong data
// size (e.g. from a recorder that crashed before finalizing the header) are
// recovered using the physical file size.
class WavReader : public SampleReader {
public:
    WavReader(std::string path) {
        _file = std::ifstream(path.c_str(), std::ios::binary);
        uint32_t riffId = 0;
        uint32_t riffSize = 0;
        uint32_t riffType = 0;
        _file.read((char*)&riffId,   sizeof(riffId));
        _file.read((char*)&riffSize, sizeof(riffSize));
        _file.read((char*)&riffType, sizeof(riffType));
        bool isRIFF = memcmp(&riffId, "RIFF", 4) == 0;
        bool isRF64 = memcmp(&riffId, "RF64", 4) == 0;
        if (!(isRIFF || isRF64) || memcmp(&riffType, "WAVE", 4) != 0) {
            throw std::runtime_error("Invalid WAV file");
        }
        reset();
    }

    WAVE_FORMAT getFormat() { return (WAVE_FORMAT)_hdr.wFormatTag; }

    const char* getFormatName() {
        switch (getFormat()) {
            case WAVE_FORMAT::PCM:        return "PCM";
            case WAVE_FORMAT::ADPCM:      return "ADPCM";
            case WAVE_FORMAT::IEEE_FLOAT: return "IEEE_FLOAT";
            case WAVE_FORMAT::ALAW:       return "ALAW";
            case WAVE_FORMAT::MULAW:      return "MULAW";
            case WAVE_FORMAT::EXTENSIBLE: return "EXTENSIBLE";
            default:                      return "UNKNOWN";
        }
    }

    uint16_t getBitDepth() { return _hdr.wBitsPerSample; }
    uint16_t getChannelCount() { return _hdr.wChannels; }
    uint16_t getBlockAlign() { return _hdr.wBlockAlign; }
    uint32_t getSampleRate() { return _hdr.dwSamplesPerSec; }
    bool isValid() { return _valid; }

    uint64_t getSampleCount() {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!_valid) { return 0; }
        return _dataSize / _hdr.wBlockAlign;
    }

    // Parse the chunk structure and leave the read position at the start of
    // the data chunk. Also picks up data appended since the last parse.
    void reset() {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        _valid = false;
        _dataOffset = 0;
        _dataSize = 0;
        std::memset(&_hdr, 0, sizeof(_hdr));
        _file.clear();
        _file.seekg(0, std::ios_base::end);
        _fileSize = _file.tellg();
        _file.seekg(sizeof(uint32_t) * 3, std::ios_base::beg);
        uint64_t ds64DataSize = 0;
        uint32_t dataChunkSize = 0;
        bool isFmtPresent = false;
        bool isDataPresent = false;
        while ((int64_t)_file.tellg() < _fileSize) {
            uint32_t chunkId = 0;
            uint32_t chunkSize = 0;
            _file.read((char*)&chunkId,   sizeof(chunkId));
            _file.read((char*)&chunkSize, sizeof(chunkSize));
            if (!_file.good()) { break; }
            // Chunks are word-aligned; the pad byte is not counted in chunkSize
            int64_t nextChunk = (int64_t)_file.tellg() + chunkSize + (chunkSize & 1);
            if (memcmp(&chunkId, "ds64", 4) == 0) {
                // RF64 keeps the real 64-bit sizes here; the RIFF-level size
                // fields are set to 0xFFFFFFFF
                uint64_t riffSize64 = 0;
                _file.read((char*)&riffSize64,   sizeof(riffSize64));
                _file.read((char*)&ds64DataSize, sizeof(ds64DataSize));
            }
            else if (memcmp(&chunkId, "fmt ", 4) == 0) {
                if (chunkSize < sizeof(_hdr)) {
                    flog::error("WavReader: invalid fmt chunk size {0}", chunkSize);
                    break;
                }
                _file.read((char*)&_hdr, sizeof(_hdr));
                if (_hdr.wFormatTag == (uint16_t)WAVE_FORMAT::EXTENSIBLE && chunkSize >= 40) {
                    uint16_t cbSize = 0;
                    uint16_t validBitsPerSample = 0;
                    uint32_t channelMask = 0;
                    uint8_t subFormat[16];
                    _file.read((char*)&cbSize,             sizeof(cbSize));
                    _file.read((char*)&validBitsPerSample, sizeof(validBitsPerSample));
                    _file.read((char*)&channelMask,        sizeof(channelMask));
                    _file.read((char*)subFormat,           sizeof(subFormat));
                    // The subformat GUIDs are the plain format tag followed by
                    // 0000-0010-8000-00AA00389B71, so the first dword suffices
                    uint32_t subFormatTag = subFormat[0] | (subFormat[1] << 8) | (subFormat[2] << 16) | (subFormat[3] << 24);
                    _hdr.wFormatTag = (uint16_t)subFormatTag;
                }
                isFmtPresent = true;
            }
            else if (memcmp(&chunkId, "data", 4) == 0) {
                _dataOffset = _file.tellg();
                dataChunkSize = chunkSize;
                isDataPresent = true;
                break;
            }
            else {
                char name[5] = { 0 };
                memcpy(name, &chunkId, 4);
                flog::debug("WavReader: skipping chunk \"{0}\", size {1}", name, chunkSize);
            }
            _file.seekg(nextChunk, std::ios_base::beg);
        }
        if (isFmtPresent && !_hdr.wBlockAlign) {
            _hdr.wBlockAlign = _hdr.wChannels * (_hdr.wBitsPerSample / 8);
        }
        // Determine the data size. RF64 stores it in the ds64 chunk; a RIFF
        // header may declare a wrong size if the recorder did not finalize
        // the file, in which case fall back to the physical file size.
        // Size 0 also takes the fallback: it is what an unfinalized recording
        // contains (SDR++'s own riff::Writer patches the 0 only on close), and
        // recovering those beats the pathological case it misreads (a truly
        // empty data chunk followed by metadata chunks).
        uint64_t maxDataSize = (_fileSize > _dataOffset) ? (uint64_t)(_fileSize - _dataOffset) : 0;
        _dataSize = (dataChunkSize == 0xFFFFFFFF && ds64DataSize) ? ds64DataSize : dataChunkSize;
        if (!_dataSize || _dataSize > maxDataSize) { _dataSize = maxDataSize; }
        _valid = isFmtPresent && isDataPresent && _hdr.wBlockAlign != 0;
        if (_valid) {
            _file.clear();
            _file.seekg(_dataOffset, std::ios_base::beg);
        }
    }

    uint64_t getSamplePosition() {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!_valid) { return 0; }
        return (uint64_t)((int64_t)_file.tellg() - _dataOffset) / _hdr.wBlockAlign;
    }

    void seek(uint64_t sampleNumber) {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!_valid) { return; }
        sampleNumber = std::min(sampleNumber, getSampleCount());
        _file.clear();
        _file.seekg(_dataOffset + (int64_t)(sampleNumber * _hdr.wBlockAlign), std::ios_base::beg);
    }

    void rewind() { seek(0); }

    size_t readSamples(void* data, size_t size) {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        if (!_valid) { return 0; }
        int64_t remaining = _dataOffset + (int64_t)_dataSize - (int64_t)_file.tellg();
        if (remaining <= 0) { return 0; }
        _file.read((char*)data, std::min((int64_t)size, remaining));
        return _file.gcount();
    }

    void close() {
        std::lock_guard<std::recursive_mutex> lck(_mtx);
        _file.close();
    }

private:
    struct RIFF_HDR_t {
        uint16_t wFormatTag;         // Format category
        uint16_t wChannels;          // Number of channels
        uint32_t dwSamplesPerSec;    // Sampling rate
        uint32_t dwAvgBytesPerSec;   // For buffer estimation
        uint16_t wBlockAlign;        // Data block (frame) size in bytes
        uint16_t wBitsPerSample;     // Sample size
    };

    std::recursive_mutex _mtx;
    std::ifstream _file;
    bool _valid = false;
    int64_t _fileSize = 0;
    int64_t _dataOffset = 0;
    uint64_t _dataSize = 0;
    RIFF_HDR_t _hdr = {};
};
