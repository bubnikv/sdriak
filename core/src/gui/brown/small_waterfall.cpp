
#include "small_waterfall.h"

#include <vector>
#include <mutex>
#include <dsp/types.h>
#include <dsp/multirate/rational_resampler.h>
#include <gui/widgets/waterfall.h>
#include <dsp/buffer/fftw.h>
#include <volk/volk.h>

struct SubWaterfall::SubWaterfallPrivate {
    SubWaterfall *pub;
    dsp::multirate::RationalResampler<dsp::stereo_t> res;
    std::vector<dsp::stereo_t> inputBuffer;
    std::mutex inputBufferMutex;
    std::vector<dsp::stereo_t> resampledV;
    std::vector<std::pair<float, float>> minMaxQueue;
    ImGui::WaterFall waterfall;
    dsp::buffer::FFTWBuffer<fftwf_complex> fft_in;
    dsp::buffer::FFTWBuffer<fftwf_complex> fft_out;
    dsp::buffer::FFTWPlan fftwPlan;
    dsp::buffer::Buffer<float> spectrumLine;
    float hiFreq = 5000;
    int fftSize;
    float waterfallRate = 10;
    int sampleRate;
    int outputSampleRate;
    std::string lbl;


    void flushDrawUpdates() {
        std::lock_guard<std::mutex> lock(inputBufferMutex);
        while (inputBuffer.size() > fftSize) {
            for (int i = 0; i < fftSize; i++) {
                fft_in[i][0] = inputBuffer[i].l;
                fft_in[i][1] = 0;
            }
            fftwf_execute(fftwPlan);
            volk_32fc_s32f_power_spectrum_32f(spectrumLine, (const lv_32fc_t*)fft_out.get(), fftSize, fftSize);
            auto mx = -5000.0;
            for(int i=0; i<fftSize; i++) {
                if(spectrumLine[i] > mx) {
                    mx = spectrumLine[i];
                }
            }
            pub->peaks.emplace_back(mx);
            while(pub->peaks.size() > 1000) {
                pub->peaks.erase(pub->peaks.begin());
            }
            float* dest = waterfall.getFFTBuffer();
            memcpy(dest, spectrumLine, fftSize * sizeof(float));
            for (int q = 0; q < fftSize / 2; q++) {
                std::swap(dest[q], dest[q + fftSize / 2]);
            }
            // bins are located
            // -hiFreq .. 0 ... hiFreq  // total fftSize
            int startBin = fftSize/2 - fftSize/16; // 1/8th of the fftSize in the middle
            int endBin = fftSize/2 + fftSize/16;
            float minn = dest[startBin];
            float maxx = dest[startBin];
            for (int q = startBin; q < endBin; q++) {
                if (dest[q] < minn) {
                    minn = dest[q];
                }
                if (dest[q] > maxx) {
                    maxx = dest[q];
                }
            }
            minMaxQueue.emplace_back(minn, maxx);

            int AVERAGE_SECONDS = 5;
            waterfall.pushFFT();
            inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + fftSize);
            int start = (int)minMaxQueue.size() - waterfallRate * AVERAGE_SECONDS;
            if (start < 0) {
                start = 0;
            }
            float bmin = 0;
            float bmax = 0;
            for (int z = start; z < minMaxQueue.size(); z++) {
                bmin += minMaxQueue[z].first;
                bmax += minMaxQueue[z].second;
            }
            bmin /= (minMaxQueue.size() - start);
            bmax /= (minMaxQueue.size() - start);
            waterfall.setWaterfallMin(bmin);
            waterfall.setWaterfallMax(bmax + 30);
            if (bmin < waterfall.getFFTMin()) {
                waterfall.setFFTMin(bmin);
            }
            else {
                waterfall.setFFTMin(waterfall.getFFTMin() + 1);
            }
            if (bmax > waterfall.getFFTMax()) {
                waterfall.setFFTMax(bmax);
            }
            else {
                waterfall.setFFTMax(waterfall.getFFTMax() - 1);
            }
            while (minMaxQueue.size() > waterfallRate * AVERAGE_SECONDS) {
                minMaxQueue.erase(minMaxQueue.begin());
            }
        }

    }
};

SubWaterfall::SubWaterfall(int sampleRate, int wfrange, const std::string & lbl) {
    pvt = std::make_shared<SubWaterfallPrivate>();
    pvt->pub = this;
    pvt->sampleRate = sampleRate;
    pvt->outputSampleRate = 2 * wfrange;
    pvt->lbl = lbl;
    pvt->hiFreq = wfrange;
    pvt->fftSize = pvt->hiFreq / pvt->waterfallRate;
    pvt->waterfall.setRawFFTSize(pvt->fftSize);
    pvt->waterfall.setBandwidth(2 * pvt->hiFreq);
    pvt->waterfall.setViewBandwidth(pvt->hiFreq);
    pvt->waterfall.setViewOffset(pvt->hiFreq);
    pvt->waterfall.setFFTMin(-150);
    pvt->waterfall.setFFTMax(0);
    pvt->waterfall.setWaterfallMin(-150);
    pvt->waterfall.setWaterfallMax(0);
    pvt->waterfall.setFullWaterfallUpdate(false);

    pvt->fft_in.allocate(pvt->fftSize);
    pvt->fft_out.allocate(pvt->fftSize);
    pvt->fftwPlan.planDFT1D(pvt->fftSize, pvt->fft_in, pvt->fft_out, FFTW_FORWARD);
    pvt->spectrumLine.allocate(pvt->fftSize);
}

SubWaterfall::~SubWaterfall() {
    // Everything pvt holds is released by its own destructor.
}

void SubWaterfall::init() {
    pvt->waterfall.init();
    pvt->res.init(pvt->sampleRate, 2 * pvt->hiFreq);
}

void SubWaterfall::draw() {
    pvt->waterfall.draw();
    pvt->flushDrawUpdates();
}

void SubWaterfall::addAudioSamples(dsp::stereo_t* samples, int count, int sampleRate) {
    // RationalResampler uses its destination for intermediate power-decimator
    // stages, which can be larger than the final returned count. Size from the
    // selected plan rather than the requested flat rate ratio.
    int newSize = dsp::multirate::RationalResampler<dsp::stereo_t>::maxOutputBufferForRates(
        sampleRate, pvt->outputSampleRate, count);
    if (pvt->resampledV.size() < newSize) {
        pvt->resampledV.resize(newSize);
    }
    if (pvt->sampleRate != sampleRate) {
        pvt->res.setInSamplerate(sampleRate);
        pvt->sampleRate = sampleRate;
    }
    count = pvt->res.process(count, samples, pvt->resampledV.data());
    samples = pvt->resampledV.data();

    std::lock_guard<std::mutex> lock(pvt->inputBufferMutex);
    int curr = pvt->inputBuffer.size();
    pvt->inputBuffer.resize(curr + count);
    memcpy(pvt->inputBuffer.data() + curr, samples, count * sizeof(dsp::stereo_t));
}
void SubWaterfall::setFreqVisible(bool visible) {
    (void)visible;
}
