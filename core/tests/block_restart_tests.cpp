#include "test_support.h"

#include <dsp/block.h>

#include <stdexcept>

namespace {

using test_support::expect;

// Starts a real std::thread and can then fail, modelling a doStart() that spawns two workers
// when its first worker starts but construction of its second worker throws.
class PartiallyFailingBlock : public dsp::block {
public:
    PartiallyFailingBlock() { _block_init = true; }

    ~PartiallyFailingBlock() override {
        if (_block_init) { stop(); }
    }

    int run() override { return -1; }

    void failNextStart() { failNext = true; }

    bool isRunning() const { return running; }
    bool isTempStopped() const { return tempStopped; }
    int stopDepth() const { return tempStopDepth; }
    bool hasJoinableWorker() const { return workerThread.joinable(); }

protected:
    void doStart() override {
        workerThread = std::thread([this] { workerLoop(); });
        if (failNext) {
            failNext = false;
            throw std::runtime_error("injected thread-start failure");
        }
    }

    void doStop() override {
        if (workerThread.joinable()) { workerThread.join(); }
    }

private:
    bool failNext = false;
};

void expectCleanlyStopped(const PartiallyFailingBlock& block, const char* context) {
    expect(!block.isRunning(), context);
    expect(!block.isTempStopped(), "failed restart left tempStopped set");
    expect(block.stopDepth() == 0, "failed restart left tempStopDepth elevated");
    expect(!block.hasJoinableWorker(), "failed restart left a joinable worker");
}

void testOrdinaryStartFailureIsRetryable() {
    PartiallyFailingBlock block;
    block.failNextStart();

    bool threw = false;
    try {
        block.start();
    }
    catch (const std::runtime_error&) {
        threw = true;
    }
    expect(threw, "ordinary start failure did not propagate");
    expectCleanlyStopped(block, "ordinary start failure left running set");

    block.start();
    expect(block.isRunning(), "block could not restart after start failure");
    block.stop();
    expectCleanlyStopped(block, "stop after start retry left block running");
}

void testTempRestartFailureIsSwallowedButClean() {
    PartiallyFailingBlock block;
    block.start();
    block.failNextStart();

    {
        dsp::block::TempStop guard(&block);
    }

    // TempStop's destructor cannot throw, but tempStart() must have joined the
    // partially created worker and made an explicit later start safe.
    expectCleanlyStopped(block, "failed temporary restart left running set");

    // A later reconfiguration must not try to assign over the failed worker.
    {
        dsp::block::TempStop guard(&block);
    }
    expectCleanlyStopped(block, "stopped block changed during reconfiguration");

    block.start();
    expect(block.isRunning(), "block could not start after failed temporary restart");
    block.stop();
}

void testStopCancelsPendingRestart() {
    PartiallyFailingBlock block;
    block.start();
    block.tempStop();
    expect(block.isRunning() && block.isTempStopped(),
           "tempStop did not record a pending restart");

    block.stop();
    expect(!block.isRunning(), "stop did not clear running during tempStop");
    expect(!block.isTempStopped(), "stop did not cancel the pending restart");

    block.tempStart();
    expectCleanlyStopped(block, "balanced tempStart restarted a stopped block");
}

} // namespace

int main() {
    return test_support::run("block_restart_tests", [] {
        testOrdinaryStartFailureIsRetryable();
        testTempRestartFailureIsSwallowedButClean();
        testStopCancelsPendingRestart();
    });
}
