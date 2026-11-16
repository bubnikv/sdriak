#pragma once
#include <assert.h>
#include <exception>
#include <thread>
#include <vector>
#include <algorithm>
#include "stream.h"
#include "../utils/flog.h"
#include "types.h"

namespace dsp {
    class generic_block {
    public:
        virtual ~generic_block() {}
        virtual void start() {}
        virtual void stop() {}
        virtual int run() { return -1; }
    };

    class block : public generic_block {
    public:
        virtual ~block() {
            if (!_block_init) { return; }
            stop();
            _block_init = false;
        }

        virtual void start() {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            if (running) {
                return;
            }
            running = true;
            tempStopped = false;
            try {
                doStart();
            }
            catch (...) {
                const std::exception_ptr error = std::current_exception();
                recoverFailedStart();
                std::rethrow_exception(error);
            }
        }

        virtual void stop() {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            if (!running) {
                tempStopped = false;
                return;
            }
            doStop();
            running = false;
            // A permanent stop cancels any restart still owed by a matching
            // tempStart(), including when stop() is called between tempStop()
            // and tempStart().
            tempStopped = false;
        }

        void tempStart() {
            assert(_block_init);
            if (!tempStopDepth || --tempStopDepth) { return; }
            if (tempStopped) {
                try {
                    doStart();
                    tempStopped = false;
                }
                catch (...) {
                    const std::exception_ptr error = std::current_exception();
                    // doStart() may have created only part of a composite
                    // block's workers (one that starts two). Join anything it
                    // published before reporting the failed restart.
                    recoverFailedStart();
                    std::rethrow_exception(error);
                }
            }
        }

        void tempStop() {
            assert(_block_init);
            if (tempStopDepth++) { return; }
            if (running && !tempStopped) {
                // Restore the depth if the worker cannot be joined. Left
                // elevated, it turns the matching tempStart() into a silent
                // no-op and the block stays stopped for the rest of the run.
                try {
                    doStop();
                }
                catch (...) {
                    --tempStopDepth;
                    throw;
                }
                tempStopped = true;
            }
        }

        virtual int run() = 0;

        // Scope guard for the tempStop()/tempStart() pairs that bracket every
        // reconfiguration. Doing it by hand is not exception-safe: buffer::alloc()
        // throws on OOM, and a throw between the two calls skips tempStart(),
        // leaving tempStopDepth elevated and the block permanently wedged even
        // if the caller handles the exception.
        class TempStop {
        public:
            explicit TempStop(block* blk) : _blk(blk) { _blk->tempStop(); }
            ~TempStop() {
                // A destructor must not throw: tempStart() spawns the worker via
                // std::thread, whose constructor can throw, and this destructor
                // runs during unwinding of the very exceptions it exists to
                // survive. Leaving the block stopped is recoverable; calling
                // std::terminate mid-unwind is not.
                try {
                    _blk->tempStart();
                }
                catch (const std::exception& e) {
                    flog::error("Failed to restart a DSP block after reconfiguration: {0}", e.what());
                }
                catch (...) {
                    flog::error("Failed to restart a DSP block after reconfiguration");
                }
            }
            TempStop(const TempStop&) = delete;
            TempStop& operator=(const TempStop&) = delete;
        private:
            block* _blk;
        };

    protected:
        // Restore a coherent state after doStart() throws. doStop() is virtual
        // here, so composite blocks can join every worker they managed to
        // create before the failure. If cleanup itself fails, keep `running`
        // true and tempStopped false: the next stop/tempStop retries cleanup
        // instead of assigning over a possibly joinable std::thread.
        void recoverFailedStart() noexcept {
            try {
                doStop();
                running = false;
                tempStopped = false;
            }
            catch (const std::exception& e) {
                running = true;
                tempStopped = false;
                flog::error("Failed to clean up a partially started DSP block: {0}", e.what());
            }
            catch (...) {
                running = true;
                tempStopped = false;
                flog::error("Failed to clean up a partially started DSP block");
            }
        }

        void workerLoop() {
            while (run() >= 0) {}
        }

        virtual void doStart() {
            workerThread = std::thread(&block::workerLoop, this);
        }

        virtual void doStop() {
            for (auto& in : inputs) {
                in->stopReader();
            }
            for (auto& out : outputs) {
                out->stopWriter();
            }

            // TODO: Make sure this isn't needed, I don't know why it stops
            if (workerThread.joinable()) {
                workerThread.join();
            }

            for (auto& in : inputs) {
                in->clearReadStop();
            }
            for (auto& out : outputs) {
                out->clearWriteStop();
            }
        }
    
        void acquire() {
            ctrlMtx.lock();
        }

        void release() {
            ctrlMtx.unlock();
        }

        void registerInput(untyped_stream* inStream) {
            inputs.push_back(inStream);
        }

        void unregisterInput(untyped_stream* inStream) {
            inputs.erase(std::remove(inputs.begin(), inputs.end(), inStream), inputs.end());
        }

        void registerOutput(untyped_stream* outStream) {
            outputs.push_back(outStream);
        }

        void unregisterOutput(untyped_stream* outStream) {
            outputs.erase(std::remove(outputs.begin(), outputs.end(), outStream), outputs.end());
        }

        bool _block_init = false;

        std::recursive_mutex ctrlMtx;

        std::vector<untyped_stream*> inputs;
        std::vector<untyped_stream*> outputs;

        bool running = false;
        bool tempStopped = false;
        int tempStopDepth = 0;
        std::thread workerThread;
    };
}
