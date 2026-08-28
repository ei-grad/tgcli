#include "daemon/raw_contract.hpp"

#include "common/sha256.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api_json.h>
#include <td/utils/JsonBuilder.h>
#include <td/utils/Slice.h>

namespace tgcli::daemon::raw {

namespace {

enum class RawTdConstructorKind {
    Object,
    Function,
};

struct RawTdFieldSpec {
    std::string_view name;
    std::string_view type;
};

struct RawTdConstructorSpec {
    std::string_view name;
    std::string_view result_type;
    std::int32_t constructor_id;
    RawTdConstructorKind kind;
    std::size_t field_offset;
    std::size_t field_count;
};

#include "daemon/raw_td_schema.generated.inc"

inline constexpr std::array<std::string_view, 1> kCompiledRawBodyValidatorSymbols{"deny"};
static_assert(kGeneratedRawBodyValidatorSymbols == kCompiledRawBodyValidatorSymbols);

enum class ValueKind {
    Null,
    Boolean,
    Signed,
    Unsigned,
    Double,
    String,
    Bytes,
    Array,
    Object,
};

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<secure::SensitiveString, Value>>;

struct Value {
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double,
                                 secure::SensitiveString, Array, Object>;

    ValueKind kind = ValueKind::Null;
    Storage storage = nullptr;

    Value() = default;
    explicit Value(bool value) : kind(ValueKind::Boolean), storage(value) {}
    explicit Value(std::int64_t value) : kind(ValueKind::Signed), storage(value) {}
    explicit Value(std::uint64_t value) : kind(ValueKind::Unsigned), storage(value) {}
    explicit Value(double value) : kind(ValueKind::Double), storage(value) {}
    explicit Value(secure::SensitiveString value)
        : kind(ValueKind::String), storage(std::move(value)) {}
    explicit Value(Array value) : kind(ValueKind::Array), storage(std::move(value)) {}
    explicit Value(Object value) : kind(ValueKind::Object), storage(std::move(value)) {}

    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;
    ~Value() = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
};

struct Frame {
    ValueKind kind = ValueKind::Null;
    Array array;
    Object object;
    std::set<std::array<unsigned char, 32>> key_hashes;
    std::optional<secure::SensitiveString> pending_key;
};

class SaxParser final : public nlohmann::json_sax<nlohmann::json> {
  public:
    explicit SaxParser(secure::WipeObserver observer) : observer_(std::move(observer)) {}

    [[nodiscard]] std::optional<Value> finish() {
        if (failed_ || !root_ || !frames_.empty()) {
            return std::nullopt;
        }
        return std::move(root_);
    }

    [[nodiscard]] Error error() const noexcept {
        return error_;
    }

    bool null() override {
        return append(Value{});
    }

    bool boolean(bool value) override {
        return append(Value(value));
    }

    bool number_integer(number_integer_t value) override {
        return append(Value(static_cast<std::int64_t>(value)));
    }

    bool number_unsigned(number_unsigned_t value) override {
        return append(Value(static_cast<std::uint64_t>(value)));
    }

    bool number_float(number_float_t value, const string_t& source) override {
        static_cast<void>(source);
        return append(Value(static_cast<double>(value)));
    }

    bool string(string_t& value) override {
        return append(
            Value(secure::SensitiveString(std::move(value), observer_, "raw_ast_string")));
    }

    bool binary(binary_t& value) override {
        static_cast<void>(value);
        return fail(Error::InvalidJson);
    }

    bool start_object(std::size_t elements) override {
        static_cast<void>(elements);
        return start(ValueKind::Object);
    }

    bool key(string_t& value) override {
        if (frames_.empty() || frames_.back().kind != ValueKind::Object ||
            frames_.back().pending_key) {
            return fail(Error::InvalidJson);
        }
        auto& frame = frames_.back();
        common::Sha256 digest;
        digest.update(value);
        if (!frame.key_hashes.emplace(digest.finish()).second) {
            return fail(Error::DuplicateField);
        }
        frame.pending_key.emplace(std::move(value), observer_, "raw_ast_key");
        return true;
    }

    bool end_object() override {
        if (frames_.empty() || frames_.back().kind != ValueKind::Object ||
            frames_.back().pending_key) {
            return fail(Error::InvalidJson);
        }
        auto frame = std::move(frames_.back());
        frames_.pop_back();
        return append(Value(std::move(frame.object)));
    }

    bool start_array(std::size_t elements) override {
        static_cast<void>(elements);
        return start(ValueKind::Array);
    }

    bool end_array() override {
        if (frames_.empty() || frames_.back().kind != ValueKind::Array) {
            return fail(Error::InvalidJson);
        }
        auto frame = std::move(frames_.back());
        frames_.pop_back();
        return append(Value(std::move(frame.array)));
    }

    bool parse_error(std::size_t position, const std::string& last_token,
                     const nlohmann::detail::exception& exception) override {
        static_cast<void>(position);
        static_cast<void>(last_token);
        static_cast<void>(exception);
        return fail(Error::InvalidJson);
    }

  private:
    bool start(ValueKind kind) {
        if (frames_.size() >= kMaximumJsonDepth) {
            return fail(Error::MaximumDepthExceeded);
        }
        frames_.push_back(Frame{.kind = kind,
                                .array = {},
                                .object = {},
                                .key_hashes = {},
                                .pending_key = std::nullopt});
        return true;
    }

    bool append(Value value) {
        if (frames_.empty()) {
            if (root_) {
                return fail(Error::InvalidJson);
            }
            root_.emplace(std::move(value));
            return true;
        }
        auto& frame = frames_.back();
        if (frame.kind == ValueKind::Array) {
            frame.array.push_back(std::move(value));
            return true;
        }
        if (frame.kind != ValueKind::Object || !frame.pending_key) {
            return fail(Error::InvalidJson);
        }
        frame.object.emplace_back(std::move(*frame.pending_key), std::move(value));
        frame.pending_key.reset();
        return true;
    }

    bool fail(Error error) {
        error_ = error;
        failed_ = true;
        return false;
    }

    secure::WipeObserver observer_;
    std::vector<Frame> frames_;
    std::optional<Value> root_;
    Error error_ = Error::InvalidJson;
    bool failed_ = false;
};

const Value* find(const Object& object, std::string_view name) {
    const auto entry =
        std::ranges::find_if(object, [&](const auto& item) { return item.first.view() == name; });
    return entry == object.end() ? nullptr : &entry->second;
}

std::optional<std::int64_t> signed_integer(const Value& value) {
    if (value.kind == ValueKind::Signed) {
        return std::get<std::int64_t>(value.storage);
    }
    if (value.kind == ValueKind::Unsigned) {
        const auto number = std::get<std::uint64_t>(value.storage);
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(number);
        }
    }
    return std::nullopt;
}

bool canonical_integer_string(std::string_view text, std::int64_t& output) {
    if (text.empty() || text.front() == '+' || text == "-0" ||
        (text.size() > 1 && text.front() == '0') ||
        (text.size() > 2 && text[0] == '-' && text[1] == '0')) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
    return error == std::errc{} && end == text.data() + text.size();
}

int base64_value(unsigned char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

std::optional<std::string> decode_base64(std::string_view input) {
    if (input.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string output;
    output.reserve(input.size() / 4 * 3);
    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
        const bool final = offset + 4 == input.size();
        const bool pad2 = input[offset + 2] == '=';
        const bool pad3 = input[offset + 3] == '=';
        if ((pad2 && !pad3) || ((!final) && (pad2 || pad3))) {
            return std::nullopt;
        }
        const int first = base64_value(static_cast<unsigned char>(input[offset]));
        const int second = base64_value(static_cast<unsigned char>(input[offset + 1]));
        const int third = pad2 ? 0 : base64_value(static_cast<unsigned char>(input[offset + 2]));
        const int fourth = pad3 ? 0 : base64_value(static_cast<unsigned char>(input[offset + 3]));
        if (first < 0 || second < 0 || third < 0 || fourth < 0 || (pad2 && (second & 0x0f) != 0) ||
            (pad3 && !pad2 && (third & 0x03) != 0)) {
            return std::nullopt;
        }
        output.push_back(static_cast<char>((first << 2) | (second >> 4)));
        if (!pad2) {
            output.push_back(static_cast<char>(((second & 0x0f) << 4) | (third >> 2)));
        }
        if (!pad3) {
            output.push_back(static_cast<char>(((third & 0x03) << 6) | fourth));
        }
    }
    return output;
}

void append_base64_json_string(std::string_view input, std::string& output) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    output.push_back('"');
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
        const auto first = static_cast<unsigned char>(input[offset]);
        const bool has_second = offset + 1 < input.size();
        const bool has_third = offset + 2 < input.size();
        const auto second = has_second ? static_cast<unsigned char>(input[offset + 1]) : 0U;
        const auto third = has_third ? static_cast<unsigned char>(input[offset + 2]) : 0U;
        output.push_back(alphabet[first >> 2U]);
        output.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
        output.push_back(has_second ? alphabet[((second & 0x0fU) << 2U) | (third >> 6U)] : '=');
        output.push_back(has_third ? alphabet[third & 0x3fU] : '=');
    }
    output.push_back('"');
}

bool ascii_identifier(std::string_view text) {
    return !text.empty() && text.size() <= 128 &&
           ((text.front() >= 'A' && text.front() <= 'Z') ||
            (text.front() >= 'a' && text.front() <= 'z')) &&
           std::ranges::all_of(text.substr(1), [](char character) {
               return (character >= 'A' && character <= 'Z') ||
                      (character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') || character == '_';
           });
}

void append_json_string(std::string_view input, std::string& output) {
    constexpr std::string_view digits = "0123456789abcdef";
    output.push_back('"');
    for (const char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(character);
        } else if (byte <= 0x1fU) {
            output += "\\u00";
            output.push_back(digits[byte >> 4U]);
            output.push_back(digits[byte & 0x0fU]);
        } else {
            output.push_back(character);
        }
    }
    output.push_back('"');
}

std::optional<std::string> canonical_double(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    if (value == 0.0) {
        return std::string("0");
    }
    std::array<char, 128> buffer{};
    const double absolute = std::abs(value);
    const auto format = absolute >= 1e-6 && absolute < 1e21 ? std::chars_format::fixed
                                                            : std::chars_format::scientific;
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, format);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    std::string output(buffer.data(), end);
    const auto exponent = output.find('e');
    if (exponent != std::string::npos) {
        auto digits = exponent + 1;
        if (output[digits] == '+' || output[digits] == '-') {
            ++digits;
        }
        while (digits + 1 < output.size() && output[digits] == '0') {
            output.erase(digits, 1);
        }
    }
    return output;
}

const RawTdConstructorSpec* constructor_by_name(std::string_view name) {
    const auto* const row =
        std::ranges::lower_bound(kRawTdConstructors, name, {}, &RawTdConstructorSpec::name);
    return row != kRawTdConstructors.end() && row->name == name ? &*row : nullptr;
}

const RawTdConstructorSpec* constructor_by_id(std::int32_t identifier) {
    const auto* const row =
        std::ranges::find(kRawTdConstructors, identifier, &RawTdConstructorSpec::constructor_id);
    return row == kRawTdConstructors.end() ? nullptr : &*row;
}

std::span<const RawTdFieldSpec> fields_of(const RawTdConstructorSpec& constructor) {
    return std::span(kRawTdFields).subspan(constructor.field_offset, constructor.field_count);
}

std::optional<std::string_view> vector_element(std::string_view type) {
    constexpr std::string_view prefix = "vector<";
    if (!type.starts_with(prefix) || !type.ends_with('>')) {
        return std::nullopt;
    }
    return type.substr(prefix.size(), type.size() - prefix.size() - 1);
}

bool primitive_type(std::string_view type) {
    return type == "Bool" || type == "int32" || type == "int53" || type == "int64" ||
           type == "double" || type == "string" || type == "bytes";
}

class ConversionArena final {
  public:
    explicit ConversionArena(secure::WipeObserver observer) : observer_(std::move(observer)) {}

    ~ConversionArena() {
        for (auto& value : values_) {
            secure::wipe(value, observer_, "raw_native_conversion_staging");
        }
    }

    ConversionArena(const ConversionArena&) = delete;
    ConversionArena& operator=(const ConversionArena&) = delete;
    ConversionArena(ConversionArena&&) = delete;
    ConversionArena& operator=(ConversionArena&&) = delete;

    std::string& store(std::string value) {
        values_.push_back(std::move(value));
        return values_.back();
    }

    [[nodiscard]] const secure::WipeObserver& observer() const noexcept {
        return observer_;
    }

  private:
    secure::WipeObserver observer_;
    std::deque<std::string> values_;
};

void wipe_native_object(td::td_api::Object& value, const secure::WipeObserver& observer) noexcept;

template <typename Value>
void wipe_native_value(Value& value, const secure::WipeObserver& observer) noexcept {
    static_cast<void>(value);
    static_cast<void>(observer);
}

void wipe_native_value(std::string& value, const secure::WipeObserver& observer) noexcept {
    secure::wipe(value, observer, "raw_native_string_or_bytes");
}

template <typename Value>
// NOLINTNEXTLINE(misc-no-recursion): TD object_ptr fields form an ownership tree.
void wipe_native_value(td::td_api::object_ptr<Value>& value,
                       const secure::WipeObserver& observer) noexcept {
    if (value != nullptr) {
        wipe_native_object(*value, observer);
    }
}

template <typename Value>
// NOLINTNEXTLINE(misc-no-recursion): TD vectors recurse only through owned elements.
void wipe_native_value(std::vector<Value>& values, const secure::WipeObserver& observer) noexcept {
    for (auto& value : values) {
        wipe_native_value(value, observer);
    }
}

#include "daemon/raw_td_wipe.generated.inc"

bool append_native_object(const td::td_api::Object& value, std::string& output);

void append_native_begin(std::string_view type, std::string& output) {
    output += "{\"@type\":";
    append_json_string(type, output);
}

bool append_native_value(bool value, std::string_view type, std::string& output) {
    if (type != "Bool") {
        return false;
    }
    output += value ? "true" : "false";
    return true;
}

bool append_native_value(std::int32_t value, std::string_view type, std::string& output) {
    if (type != "int32") {
        return false;
    }
    output += std::to_string(value);
    return true;
}

bool append_native_value(std::int64_t value, std::string_view type, std::string& output) {
    if (type == "int53") {
        if (value < -kMaximumInt53 || value > kMaximumInt53) {
            return false;
        }
        output += std::to_string(value);
        return true;
    }
    if (type != "int64") {
        return false;
    }
    append_json_string(std::to_string(value), output);
    return true;
}

bool append_native_value(double value, std::string_view type, std::string& output) {
    if (type != "double") {
        return false;
    }
    const auto serialized = canonical_double(value);
    if (!serialized) {
        return false;
    }
    output += *serialized;
    return true;
}

bool append_native_value(const std::string& value, std::string_view type, std::string& output) {
    if (type == "bytes") {
        append_base64_json_string(value, output);
        return true;
    }
    if (type != "string" || !common::valid_utf8(value)) {
        return false;
    }
    append_json_string(value, output);
    return true;
}

template <typename Value>
// NOLINTNEXTLINE(misc-no-recursion): TD object_ptr fields form an ownership tree.
bool append_native_value(const td::td_api::object_ptr<Value>& value, std::string_view type,
                         std::string& output) {
    if (value == nullptr) {
        output += "null";
        return true;
    }
    const auto* actual = constructor_by_id(static_cast<const td::td_api::Object&>(*value).get_id());
    const auto* concrete = constructor_by_name(type);
    const bool type_matches =
        actual != nullptr && actual->kind == RawTdConstructorKind::Object &&
        ((concrete != nullptr && concrete->kind == RawTdConstructorKind::Object &&
          actual->name == concrete->name) ||
         (concrete == nullptr && actual->result_type == type));
    if (!type_matches) {
        return false;
    }
    return append_native_object(*value, output);
}

template <typename Value>
// NOLINTNEXTLINE(misc-no-recursion): TD vectors recurse only through owned elements.
bool append_native_value(const std::vector<Value>& values, std::string_view type,
                         std::string& output) {
    const auto element_type = vector_element(type);
    if (!element_type) {
        return false;
    }
    output.push_back('[');
    bool first = true;
    for (const auto& value : values) {
        if (!std::exchange(first, false)) {
            output.push_back(',');
        }
        if (!append_native_value(value, *element_type, output)) {
            return false;
        }
    }
    output.push_back(']');
    return true;
}

template <typename Value>
// NOLINTNEXTLINE(misc-no-recursion): generated fields recursively serialize owned TD values.
bool append_native_field(std::string_view name, const Value& value, std::string_view type,
                         std::string& output) {
    output.push_back(',');
    append_json_string(name, output);
    output.push_back(':');
    return append_native_value(value, type, output);
}

#include "daemon/raw_td_canonical.generated.inc"

td::JsonValue json_string(ConversionArena& arena, std::string value) {
    auto& stored = arena.store(std::move(value));
    return td::JsonValue::create_string(td::MutableSlice(stored));
}

td::JsonValue json_number(ConversionArena& arena, std::string value) {
    auto& stored = arena.store(std::move(value));
    return td::JsonValue::create_number(td::MutableSlice(stored));
}

std::string encode_base64(std::string_view input) {
    std::string encoded;
    append_base64_json_string(input, encoded);
    encoded.erase(encoded.begin());
    encoded.pop_back();
    return encoded;
}

Error convert_value(const Value* value, std::string_view expected_type, std::string& canonical,
                    td::JsonValue& converted, ConversionArena& arena, std::size_t depth);

// NOLINTNEXTLINE(readability-function-cognitive-complexity): closed TD primitive dispatch.
Error convert_primitive(const Value* value, std::string_view type, std::string& canonical,
                        td::JsonValue& converted, ConversionArena& arena) {
    const bool defaulted = value == nullptr || value->kind == ValueKind::Null;
    if (type == "Bool") {
        if (!defaulted && value->kind != ValueKind::Boolean) {
            return Error::InvalidFieldType;
        }
        const bool boolean = defaulted ? false : std::get<bool>(value->storage);
        canonical += boolean ? "true" : "false";
        converted = td::JsonValue::create_boolean(boolean);
        return Error::InvalidJson;
    }
    if (type == "int32" || type == "int53") {
        const auto number = defaulted ? std::optional<std::int64_t>(0) : signed_integer(*value);
        const auto minimum =
            type == "int32" ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())
                            : -kMaximumInt53;
        const auto maximum =
            type == "int32" ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())
                            : kMaximumInt53;
        if (!number || *number < minimum || *number > maximum) {
            return Error::InvalidInteger;
        }
        auto serialized = std::to_string(*number);
        canonical += serialized;
        converted = json_number(arena, std::move(serialized));
        return Error::InvalidJson;
    }
    if (type == "int64") {
        std::optional<std::int64_t> number =
            defaulted ? std::optional<std::int64_t>(0) : signed_integer(*value);
        if (!number && value != nullptr && value->kind == ValueKind::String) {
            std::int64_t parsed = 0;
            if (canonical_integer_string(std::get<secure::SensitiveString>(value->storage).view(),
                                         parsed)) {
                number = parsed;
            }
        }
        if (!number) {
            return Error::InvalidInteger;
        }
        auto serialized = std::to_string(*number);
        append_json_string(serialized, canonical);
        converted = json_string(arena, std::move(serialized));
        return Error::InvalidJson;
    }
    if (type == "double") {
        double number = 0.0;
        if (!defaulted) {
            if (value->kind == ValueKind::Signed) {
                number = static_cast<double>(std::get<std::int64_t>(value->storage));
            } else if (value->kind == ValueKind::Unsigned) {
                number = static_cast<double>(std::get<std::uint64_t>(value->storage));
            } else if (value->kind == ValueKind::Double) {
                number = std::get<double>(value->storage);
            } else {
                return Error::InvalidDouble;
            }
        }
        const auto serialized = canonical_double(number);
        if (!serialized) {
            return Error::InvalidDouble;
        }
        canonical += *serialized;
        converted = json_number(arena, *serialized);
        return Error::InvalidJson;
    }
    if (type == "string") {
        if (!defaulted && value->kind != ValueKind::String) {
            return Error::InvalidFieldType;
        }
        const auto text = defaulted ? std::string_view{}
                                    : std::get<secure::SensitiveString>(value->storage).view();
        if (!common::valid_utf8(text)) {
            return Error::InvalidFieldType;
        }
        append_json_string(text, canonical);
        converted = json_string(arena, std::string(text));
        return Error::InvalidJson;
    }
    if (type == "bytes") {
        if (!defaulted && value->kind != ValueKind::String) {
            return Error::InvalidFieldType;
        }
        std::optional<std::string> decoded = std::string{};
        if (!defaulted) {
            decoded = decode_base64(std::get<secure::SensitiveString>(value->storage).view());
        }
        if (!decoded) {
            return Error::InvalidBase64;
        }
        append_base64_json_string(*decoded, canonical);
        converted = json_string(arena, encode_base64(*decoded));
        secure::wipe(*decoded, arena.observer(), "raw_decoded_bytes_staging");
        return Error::InvalidJson;
    }
    return Error::InvalidPolicyMetadata;
}

const RawTdConstructorSpec*
resolve_object_constructor(const Value& value, std::string_view expected_type, Error& error) {
    if (value.kind != ValueKind::Object) {
        error = Error::InvalidFieldType;
        return nullptr;
    }
    const auto& members = std::get<Object>(value.storage);
    const auto* tag = find(members, "@type");
    const auto* concrete = constructor_by_name(expected_type);
    if (concrete != nullptr && concrete->kind == RawTdConstructorKind::Object) {
        if (tag != nullptr) {
            if (tag->kind != ValueKind::String) {
                error = Error::InvalidFieldType;
                return nullptr;
            }
            if (std::get<secure::SensitiveString>(tag->storage).view() != concrete->name) {
                error = Error::UnexpectedType;
                return nullptr;
            }
        }
        return concrete;
    }
    if (tag == nullptr) {
        error = Error::MissingType;
        return nullptr;
    }
    if (tag->kind != ValueKind::String) {
        error = Error::InvalidFieldType;
        return nullptr;
    }
    const auto* candidate =
        constructor_by_name(std::get<secure::SensitiveString>(tag->storage).view());
    if (candidate == nullptr || candidate->kind != RawTdConstructorKind::Object ||
        candidate->result_type != expected_type) {
        error = Error::UnexpectedType;
        return nullptr;
    }
    return candidate;
}

// NOLINTNEXTLINE(misc-no-recursion): generated TD types and input share the fixed depth cap.
Error convert_object(const Value& value, const RawTdConstructorSpec& constructor,
                     std::string& canonical, td::JsonValue& converted, ConversionArena& arena,
                     std::size_t depth) {
    if (value.kind != ValueKind::Object) {
        return Error::InvalidFieldType;
    }
    const auto& members = std::get<Object>(value.storage);
    const auto fields = fields_of(constructor);
    for (const auto& member : members) {
        const std::string_view name = member.first.view();
        if (name == "@type") {
            continue;
        }
        if (name == "@extra" || name == "@client_id" ||
            !std::ranges::any_of(
                fields, [name](const RawTdFieldSpec& field) { return field.name == name; })) {
            return Error::UnknownField;
        }
    }

    td::vector<std::pair<td::Slice, td::JsonValue>> json_members;
    json_members.reserve(fields.size() + 1);
    auto& type_key = arena.store("@type");
    json_members.emplace_back(td::Slice(type_key),
                              json_string(arena, std::string(constructor.name)));
    canonical += "{\"@type\":";
    append_json_string(constructor.name, canonical);
    for (const auto& field : fields) {
        canonical.push_back(',');
        append_json_string(field.name, canonical);
        canonical.push_back(':');
        td::JsonValue child;
        const auto error = convert_value(find(members, field.name), field.type, canonical, child,
                                         arena, depth + 1);
        if (error != Error::InvalidJson) {
            return error;
        }
        auto& key = arena.store(std::string(field.name));
        json_members.emplace_back(td::Slice(key), std::move(child));
    }
    canonical.push_back('}');
    converted = td::JsonValue::make_object(td::JsonObject(std::move(json_members)));
    return Error::InvalidJson;
}

// NOLINTNEXTLINE(misc-no-recursion): generated TD types and input share the fixed depth cap.
Error convert_value(const Value* value, std::string_view expected_type, std::string& canonical,
                    td::JsonValue& converted, ConversionArena& arena, std::size_t depth) {
    if (depth > kMaximumJsonDepth) {
        return Error::MaximumDepthExceeded;
    }
    if (primitive_type(expected_type)) {
        return convert_primitive(value, expected_type, canonical, converted, arena);
    }
    if (const auto element = vector_element(expected_type)) {
        const bool defaulted = value == nullptr || value->kind == ValueKind::Null;
        if (!defaulted && value->kind != ValueKind::Array) {
            return Error::InvalidFieldType;
        }
        const Array empty;
        const auto& items = defaulted ? empty : std::get<Array>(value->storage);
        td::JsonArray json_items;
        json_items.reserve(items.size());
        canonical.push_back('[');
        bool first = true;
        for (const auto& item : items) {
            if (!std::exchange(first, false)) {
                canonical.push_back(',');
            }
            td::JsonValue child;
            const auto error = convert_value(&item, *element, canonical, child, arena, depth + 1);
            if (error != Error::InvalidJson) {
                return error;
            }
            json_items.push_back(std::move(child));
        }
        canonical.push_back(']');
        converted = td::JsonValue::create_array(std::move(json_items));
        return Error::InvalidJson;
    }
    if (value == nullptr || value->kind == ValueKind::Null) {
        canonical += "null";
        converted = td::JsonValue{};
        return Error::InvalidJson;
    }
    Error resolution = Error::InvalidJson;
    const auto* constructor = resolve_object_constructor(*value, expected_type, resolution);
    if (constructor == nullptr) {
        return resolution;
    }
    return convert_object(*value, *constructor, canonical, converted, arena, depth);
}

Error convert_function(const Value& value, const RawTdConstructorSpec& constructor,
                       std::string& canonical, td::JsonValue& converted, ConversionArena& arena) {
    if (constructor.kind != RawTdConstructorKind::Function) {
        return Error::UnexpectedType;
    }
    return convert_object(value, constructor, canonical, converted, arena, 0);
}

Digest hash_request(std::string_view function_name, std::string_view tdlib_sha,
                    std::string_view effective_tier, std::string_view canonical) {
    common::Sha256 digest;
    digest.update("tgcli.raw.request.v1");
    constexpr std::array<unsigned char, 1> nul{0};
    digest.update(nul);
    digest.update(tdlib_sha);
    digest.update(nul);
    digest.update(function_name);
    digest.update(nul);
    digest.update(effective_tier);
    digest.update(nul);
    const auto length = static_cast<std::uint64_t>(canonical.size());
    const std::array<unsigned char, 8> size{
        static_cast<unsigned char>(length >> 56U), static_cast<unsigned char>(length >> 48U),
        static_cast<unsigned char>(length >> 40U), static_cast<unsigned char>(length >> 32U),
        static_cast<unsigned char>(length >> 24U), static_cast<unsigned char>(length >> 16U),
        static_cast<unsigned char>(length >> 8U),  static_cast<unsigned char>(length)};
    digest.update(size);
    digest.update(canonical);
    return {.sha256 = "sha256:" + digest.finish_hex(), .bytes = canonical.size()};
}

Digest hash_response(std::string_view function_name, std::string_view canonical) {
    common::Sha256 digest;
    digest.update("tgcli.raw.response.v1");
    constexpr std::array<unsigned char, 1> nul{0};
    digest.update(nul);
    digest.update(function_name);
    digest.update(nul);
    const auto length = static_cast<std::uint64_t>(canonical.size());
    const std::array<unsigned char, 8> size{
        static_cast<unsigned char>(length >> 56U), static_cast<unsigned char>(length >> 48U),
        static_cast<unsigned char>(length >> 40U), static_cast<unsigned char>(length >> 32U),
        static_cast<unsigned char>(length >> 24U), static_cast<unsigned char>(length >> 16U),
        static_cast<unsigned char>(length >> 8U),  static_cast<unsigned char>(length)};
    digest.update(size);
    digest.update(canonical);
    return {.sha256 = "sha256:" + digest.finish_hex(), .bytes = canonical.size()};
}

} // namespace

struct TypedFunction::Impl {
    Impl(std::string function_name, std::string declared_result_type,
         td::td_api::object_ptr<td::td_api::Function> function, std::string canonical_bytes,
         secure::WipeObserver observer)
        : name(std::move(function_name)), result_type(std::move(declared_result_type)),
          native(std::move(function)),
          canonical(std::move(canonical_bytes), observer, "raw_canonical"),
          wipe_observer(std::move(observer)) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::string name;
    std::string result_type;
    td::td_api::object_ptr<td::td_api::Function> native;
    secure::SensitiveString canonical;
    secure::WipeObserver wipe_observer;

    ~Impl() {
        if (native != nullptr) {
            wipe_native_function(*native, wipe_observer);
        }
    }
};

TypedFunction::TypedFunction(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

TypedFunction::TypedFunction(TypedFunction&&) noexcept = default;
TypedFunction& TypedFunction::operator=(TypedFunction&&) noexcept = default;
TypedFunction::~TypedFunction() = default;

std::string_view TypedFunction::name() const noexcept {
    return implementation_->name;
}

std::string_view TypedFunction::result_type() const noexcept {
    return implementation_->result_type;
}

std::string_view TypedFunction::canonical() const noexcept {
    return implementation_->canonical.view();
}

const void* TypedFunction::identity() const noexcept {
    return implementation_->native.get();
}

const td::td_api::Function& TypedFunction::native() const noexcept {
    return *implementation_->native;
}

std::variant<Digest, Failure> TypedFunction::request_digest(std::string_view tdlib_sha,
                                                            std::string_view effective_tier) const {
    if (tdlib_sha.size() != 40 ||
        !std::ranges::all_of(tdlib_sha,
                             [](char character) {
                                 return (character >= '0' && character <= '9') ||
                                        (character >= 'a' && character <= 'f');
                             }) ||
        (effective_tier != "read" && effective_tier != "write" &&
         effective_tier != "destructive")) {
        return Failure{Error::InvalidPolicyMetadata};
    }
    return hash_request(name(), tdlib_sha, effective_tier, canonical());
}

std::variant<TypedFunction, Failure> parse(std::string&& input,
                                           const secure::WipeObserver& wipe_observer) {
    const secure::SensitiveString source(std::move(input), wipe_observer, "raw_physical_input");
    if (source.empty()) {
        return Failure{Error::EmptyInput};
    }
    if (source.view().size() > kMaximumRequestBytes) {
        return Failure{Error::InputTooLarge};
    }
    SaxParser sax(wipe_observer);
    const bool parsed = nlohmann::json::sax_parse(source.view(), &sax);
    auto root = sax.finish();
    if (!parsed || !root) {
        return Failure{sax.error()};
    }
    if (root->kind != ValueKind::Object) {
        return Failure{Error::InvalidTopLevel};
    }
    const auto& members = std::get<Object>(root->storage);
    const auto* tag = find(members, "@type");
    if (tag == nullptr) {
        return Failure{Error::MissingType};
    }
    if (tag->kind != ValueKind::String) {
        return Failure{Error::InvalidFieldType};
    }
    const auto function_name = std::get<secure::SensitiveString>(tag->storage).view();
    const auto* constructor = constructor_by_name(function_name);
    if (constructor == nullptr || constructor->kind != RawTdConstructorKind::Function) {
        return Failure{Error::UnexpectedType};
    }
    std::string canonical;
    canonical.reserve(source.view().size());
    ConversionArena conversion_arena(wipe_observer);
    td::JsonValue json_value;
    const auto conversion =
        convert_function(*root, *constructor, canonical, json_value, conversion_arena);
    if (conversion != Error::InvalidJson) {
        secure::wipe(canonical, wipe_observer, "raw_canonical_failure");
        return Failure{conversion};
    }
    if (canonical.size() > kMaximumRequestBytes) {
        secure::wipe(canonical, wipe_observer, "raw_canonical_oversized");
        return Failure{Error::CanonicalTooLarge};
    }
    td::td_api::object_ptr<td::td_api::Function> native;
    const auto native_status = td::td_api::from_json(native, std::move(json_value));
    if (native_status.is_error() || native == nullptr ||
        native->get_id() != constructor->constructor_id) {
        if (native != nullptr) {
            wipe_native_function(*native, wipe_observer);
        }
        secure::wipe(canonical, wipe_observer, "raw_native_conversion_failure");
        return Failure{Error::NativeConversionFailed};
    }
    std::string native_canonical;
    native_canonical.reserve(canonical.size());
    const bool native_matches =
        append_native_function(*native, native_canonical) && native_canonical == canonical;
    secure::wipe(native_canonical, wipe_observer, "raw_native_canonical_proof");
    if (!native_matches) {
        wipe_native_function(*native, wipe_observer);
        secure::wipe(canonical, wipe_observer, "raw_native_canonical_mismatch");
        return Failure{Error::NativeConversionFailed};
    }
    auto implementation = std::make_unique<TypedFunction::Impl>(
        std::string(constructor->name), std::string(constructor->result_type), std::move(native),
        std::move(canonical), wipe_observer);
    return TypedFunction(std::move(implementation));
}

std::variant<Digest, Failure> response_digest(const TypedFunction& function,
                                              const td::td_api::Object& response,
                                              const secure::WipeObserver& wipe_observer) {
    if (!ascii_identifier(function.name()) || !ascii_identifier(function.result_type()) ||
        function.identity() == nullptr) {
        return Failure{Error::InvalidPolicyMetadata};
    }
    const auto* response_constructor = constructor_by_id(response.get_id());
    if (response_constructor == nullptr ||
        response_constructor->kind != RawTdConstructorKind::Object) {
        return Failure{Error::UnexpectedResponseType};
    }
    const std::string_view expected_type = response.get_id() == td::td_api::error::ID
                                               ? std::string_view("Error")
                                               : function.result_type();
    if (response_constructor->result_type != expected_type) {
        return Failure{Error::UnexpectedResponseType};
    }
    std::string canonical;
    if (!append_native_object(response, canonical)) {
        secure::wipe(canonical, wipe_observer, "raw_response_canonical_failure");
        return Failure{Error::UnexpectedResponseType};
    }
    if (canonical.size() > kMaximumResponseBytes) {
        secure::wipe(canonical, wipe_observer, "raw_response_canonical_oversized");
        return Failure{Error::CanonicalTooLarge};
    }
    auto digest = hash_response(function.name(), canonical);
    secure::wipe(canonical, wipe_observer, "raw_response_canonical");
    return digest;
}

} // namespace tgcli::daemon::raw
