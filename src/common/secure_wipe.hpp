#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace tgcli::secure {

using WipeObserver =
    std::function<void(std::string_view stage, const char* bytes, std::size_t size)>;

inline void wipe(char* bytes, std::size_t size, const WipeObserver& observer = {},
                 std::string_view stage = {}) noexcept {
    volatile char* output = bytes;
    for (std::size_t index = 0; index < size; ++index) {
        output[index] = '\0';
    }
    if (observer) {
        try {
            observer(stage, bytes, size);
        } catch (...) {
            // Instrumentation failures cannot escape a secure destructor boundary.
            return;
        }
    }
}

inline void wipe(std::string& value, const WipeObserver& observer = {},
                 std::string_view stage = {}) noexcept {
    wipe(value.data(), value.size(), observer, stage);
    value.clear();
}

// NOLINTNEXTLINE(misc-no-recursion)
inline void wipe(nlohmann::json& value, const WipeObserver& observer = {},
                 std::string_view stage = {}) noexcept {
    try {
        if (value.is_string()) {
            wipe(value.get_ref<std::string&>(), observer, stage);
            return;
        }
        if (value.is_array() || value.is_object()) {
            for (auto& child : value) {
                wipe(child, observer, stage);
            }
        }
    } catch (...) {
        // JSON traversal cannot be allowed to escape a secure destructor boundary.
        return;
    }
}

class StringWiper {
  public:
    explicit StringWiper(std::string& value, WipeObserver observer = {},
                         std::string_view stage = {})
        : value_(&value), observer_(std::move(observer)), stage_(stage) {}
    ~StringWiper() {
        wipe(*value_, observer_, stage_);
    }
    StringWiper(const StringWiper&) = delete;
    StringWiper& operator=(const StringWiper&) = delete;
    StringWiper(StringWiper&&) = delete;
    StringWiper& operator=(StringWiper&&) = delete;

  private:
    std::string* value_;
    WipeObserver observer_;
    std::string_view stage_;
};

class JsonWiper {
  public:
    explicit JsonWiper(nlohmann::json& value, WipeObserver observer = {},
                       std::string_view stage = {})
        : value_(&value), observer_(std::move(observer)), stage_(stage) {}
    ~JsonWiper() {
        wipe(*value_, observer_, stage_);
    }
    JsonWiper(const JsonWiper&) = delete;
    JsonWiper& operator=(const JsonWiper&) = delete;
    JsonWiper(JsonWiper&&) = delete;
    JsonWiper& operator=(JsonWiper&&) = delete;

  private:
    nlohmann::json* value_;
    WipeObserver observer_;
    std::string_view stage_;
};

inline void transfer(std::string& source, std::string& destination,
                     const WipeObserver& observer = {},
                     std::string_view source_stage = "string_move_source") {
    if (&source == &destination) {
        return;
    }
    const StringWiper source_wiper(source, observer, source_stage);
    wipe(destination, observer, "string_move_destination");
    destination.assign(source);
}

inline void transfer(nlohmann::json& source, nlohmann::json& destination,
                     const WipeObserver& observer = {},
                     std::string_view source_stage = "json_move_source") {
    if (&source == &destination) {
        return;
    }
    const JsonWiper source_wiper(source, observer, source_stage);
    wipe(destination, observer, "json_move_destination");
    destination = source;
}

} // namespace tgcli::secure
