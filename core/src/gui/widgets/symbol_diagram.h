#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <dsp/stream.h>
#include <vector>
#include <mutex>

namespace ImGui {
    class SymbolDiagram {
    public:
        SymbolDiagram(float _scale = 1.0f, int count = 1024);
        ~SymbolDiagram();

        void draw(const ImVec2& size_arg = ImVec2(0, 0));

        float* acquireBuffer();

        void releaseBuffer();

        // Copies min(count, sample count) values in if the GUI is not drawing
        // right now, and drops them otherwise. Never blocks, so a DSP thread
        // can call it inline (see dsp::buffer::FrameTap).
        void tryWrite(const float* data, int count);

        std::vector<float> lines;

    private:
        std::mutex bufferMtx;
        float* buffer;
        float _scale;
        int sampleCount = 0;
    };
}