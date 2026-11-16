#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <dsp/stream.h>
#include <mutex>
#include <dsp/types.h>

namespace ImGui {
    class ConstellationDiagram {
    public:
        ConstellationDiagram();

        void draw(const ImVec2& size_arg = ImVec2(0, 0));

        dsp::complex_t* acquireBuffer();

        void releaseBuffer();

        // Copies min(count, 1024) points in if the GUI is not drawing right
        // now, and drops them otherwise. Never blocks, so a DSP thread can
        // call it inline (see dsp::buffer::FrameTap).
        void tryWrite(const dsp::complex_t* data, int count);

    private:
        std::mutex bufferMtx;
        dsp::complex_t buffer[1024];
    };
}