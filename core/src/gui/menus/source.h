#pragma once
#include <string>

namespace sourcemenu {
    void init();
    void draw(void* ctx);
    // Select a source as the combo does (falls back to the first source
    // when name is not registered). UI thread.
    void selectSource(std::string name);
    const std::string& selectedSourceName();
}