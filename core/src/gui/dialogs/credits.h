#pragma once

namespace credits {
    void init();
    // Draws the credits modal. Returns false when the user dismissed it.
    bool show();
    // Drops the modal's popup-lifecycle state. Only needed when the dialog is
    // taken down without show() getting another frame to notice (see
    // MainWindow::handleBackPress); otherwise show() resets itself.
    void reset();
}