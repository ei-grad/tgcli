#include "daemon/raw_contract.hpp"

#include "common/sha256.hpp"
#include "common/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace tgcli::daemon::raw {

namespace {

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

Value* find(Object& object, std::string_view name) {
    const auto entry =
        std::ranges::find_if(object, [&](auto& item) { return item.first.view() == name; });
    return entry == object.end() ? nullptr : &entry->second;
}

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

// NOLINTNEXTLINE(misc-no-recursion): invalid cyclic or overdeep schemas stop at the same fixed cap.
bool valid_schema(const Type& type, std::size_t depth = 0) {
    if (depth > kMaximumJsonDepth) {
        return false;
    }
    if (type.kind == Type::Kind::Primitive) {
        return true;
    }
    if (type.kind == Type::Kind::Vector) {
        return type.element && valid_schema(*type.element, depth + 1);
    }
    if (!ascii_identifier(type.object_type)) {
        return false;
    }
    std::set<std::string_view, std::less<>> names;
    for (const auto& field : type.fields) {
        if (!field.type || !ascii_identifier(field.name) || field.name == "@type" ||
            field.name == "@extra" || field.name == "@client_id" ||
            !names.emplace(field.name).second || !valid_schema(*field.type, depth + 1)) {
            return false;
        }
    }
    return true;
}

Error validate(Value& value, const Type& type);

bool known_field(const Type& type, std::string_view name) {
    return std::ranges::any_of(type.fields, [&](const Field& field) { return field.name == name; });
}

Error validate_member_names(const Object& members, const Type& type) {
    for (const auto& [name, member] : members) {
        static_cast<void>(member);
        if (name.view() == "@type") {
            continue;
        }
        if (name.view() == "@extra" || name.view() == "@client_id" ||
            !known_field(type, name.view())) {
            return Error::UnknownField;
        }
    }
    return Error::InvalidJson;
}

// NOLINTNEXTLINE(misc-no-recursion): raw schemas and JSON are validated against a fixed depth cap.
Error validate_members(Object& members, const Type& type) {
    for (const auto& field : type.fields) {
        auto* member = find(members, field.name);
        if (member == nullptr) {
            if (field.required) {
                return Error::MissingField;
            }
            continue;
        }
        if (!field.type) {
            return Error::InvalidPolicyMetadata;
        }
        const auto error = validate(*member, *field.type);
        if (error != Error::InvalidJson) {
            return error;
        }
    }
    return Error::InvalidJson;
}

// NOLINTNEXTLINE(misc-no-recursion): raw schemas and JSON are validated against a fixed depth cap.
Error validate_object(Value& value, const Type& type) {
    if (value.kind != ValueKind::Object) {
        return Error::InvalidFieldType;
    }
    auto& members = std::get<Object>(value.storage);
    const auto* tag = find(members, "@type");
    if (tag == nullptr || tag->kind != ValueKind::String) {
        return Error::MissingType;
    }
    if (std::get<secure::SensitiveString>(tag->storage).view() != type.object_type) {
        return Error::UnexpectedType;
    }
    const auto names = validate_member_names(members, type);
    if (names != Error::InvalidJson) {
        return names;
    }
    return validate_members(members, type);
}

Error validate_bounded_integer(Value& value, Primitive type) {
    const auto number = signed_integer(value);
    const auto minimum = type == Primitive::Int32
                             ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())
                             : -kMaximumInt53;
    const auto maximum = type == Primitive::Int32
                             ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())
                             : kMaximumInt53;
    if (!number || *number < minimum || *number > maximum) {
        return Error::InvalidInteger;
    }
    value.kind = ValueKind::Signed;
    value.storage = *number;
    return Error::InvalidJson;
}

Error validate_int64(Value& value) {
    std::optional<std::int64_t> number = signed_integer(value);
    if (!number && value.kind == ValueKind::String) {
        std::int64_t parsed = 0;
        if (canonical_integer_string(std::get<secure::SensitiveString>(value.storage).view(),
                                     parsed)) {
            number = parsed;
        }
    }
    if (!number) {
        return Error::InvalidInteger;
    }
    value.kind = ValueKind::Signed;
    value.storage = *number;
    return Error::InvalidJson;
}

Error validate_bytes(Value& value) {
    if (value.kind != ValueKind::String) {
        return Error::InvalidFieldType;
    }
    auto& encoded = std::get<secure::SensitiveString>(value.storage);
    auto decoded = decode_base64(encoded.view());
    if (!decoded) {
        return Error::InvalidBase64;
    }
    const auto observer = encoded.wipe_observer();
    value.kind = ValueKind::Bytes;
    value.storage = secure::SensitiveString(std::move(*decoded), observer, "raw_typed_bytes");
    return Error::InvalidJson;
}

Error validate_double(Value& value) {
    if (value.kind == ValueKind::Signed) {
        value.storage = static_cast<double>(std::get<std::int64_t>(value.storage));
        value.kind = ValueKind::Double;
    } else if (value.kind == ValueKind::Unsigned) {
        value.storage = static_cast<double>(std::get<std::uint64_t>(value.storage));
        value.kind = ValueKind::Double;
    }
    return value.kind == ValueKind::Double && std::isfinite(std::get<double>(value.storage))
               ? Error::InvalidJson
               : Error::InvalidDouble;
}

Error validate_primitive(Value& value, Primitive type) {
    switch (type) {
    case Primitive::Boolean:
        return value.kind == ValueKind::Boolean ? Error::InvalidJson : Error::InvalidFieldType;
    case Primitive::Int32:
    case Primitive::Int53:
        return validate_bounded_integer(value, type);
    case Primitive::Int64:
        return validate_int64(value);
    case Primitive::Bytes:
        return validate_bytes(value);
    case Primitive::String:
        if (value.kind != ValueKind::String ||
            !common::valid_utf8(std::get<secure::SensitiveString>(value.storage).view())) {
            return Error::InvalidFieldType;
        }
        return Error::InvalidJson;
    case Primitive::Double:
        return validate_double(value);
    }
    return Error::InvalidPolicyMetadata;
}

// NOLINTNEXTLINE(misc-no-recursion): raw schemas and JSON are validated against a fixed depth cap.
Error validate(Value& value, const Type& type) {
    switch (type.kind) {
    case Type::Kind::Primitive:
        return validate_primitive(value, type.primitive);
    case Type::Kind::Object:
        return validate_object(value, type);
    case Type::Kind::Vector:
        if (!type.element) {
            return Error::InvalidPolicyMetadata;
        }
        if (value.kind != ValueKind::Array) {
            return Error::InvalidFieldType;
        }
        for (auto& item : std::get<Array>(value.storage)) {
            const auto error = validate(item, *type.element);
            if (error != Error::InvalidJson) {
                return error;
            }
        }
        return Error::InvalidJson;
    }
    return Error::InvalidPolicyMetadata;
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

bool append_canonical(const Value& value, const Type& type, std::string& output);

// NOLINTNEXTLINE(misc-no-recursion): validated raw values cannot exceed the parser depth cap.
bool append_object(const Value& value, const Type& type, std::string& output) {
    const auto& members = std::get<Object>(value.storage);
    output += "{\"@type\":";
    append_json_string(type.object_type, output);
    for (const auto& field : type.fields) {
        const auto* member = find(members, field.name);
        if (member == nullptr) {
            continue;
        }
        output.push_back(',');
        append_json_string(field.name, output);
        output.push_back(':');
        if (!field.type || !append_canonical(*member, *field.type, output)) {
            return false;
        }
    }
    output.push_back('}');
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): validated raw values cannot exceed the parser depth cap.
bool append_canonical(const Value& value, const Type& type, std::string& output) {
    if (type.kind == Type::Kind::Object) {
        return append_object(value, type, output);
    }
    if (type.kind == Type::Kind::Vector) {
        output.push_back('[');
        bool first = true;
        for (const auto& item : std::get<Array>(value.storage)) {
            if (!std::exchange(first, false)) {
                output.push_back(',');
            }
            if (!append_canonical(item, *type.element, output)) {
                return false;
            }
        }
        output.push_back(']');
        return true;
    }
    switch (type.primitive) {
    case Primitive::Boolean:
        output += std::get<bool>(value.storage) ? "true" : "false";
        return true;
    case Primitive::Int32:
    case Primitive::Int53:
        output += std::to_string(std::get<std::int64_t>(value.storage));
        return true;
    case Primitive::Int64:
        append_json_string(std::to_string(std::get<std::int64_t>(value.storage)), output);
        return true;
    case Primitive::Bytes:
        append_base64_json_string(std::get<secure::SensitiveString>(value.storage).view(), output);
        return true;
    case Primitive::String:
        append_json_string(std::get<secure::SensitiveString>(value.storage).view(), output);
        return true;
    case Primitive::Double: {
        const auto serialized = canonical_double(std::get<double>(value.storage));
        if (!serialized) {
            return false;
        }
        output += *serialized;
        return true;
    }
    }
    return false;
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

} // namespace

struct TypedFunction::Impl {
    secure::SensitiveString name;
    Value value;
    TypePtr schema;
    secure::SensitiveString canonical;
};

TypePtr primitive(Primitive kind) {
    return std::make_shared<Type>(Type{.kind = Type::Kind::Primitive,
                                       .primitive = kind,
                                       .object_type = {},
                                       .fields = {},
                                       .element = {}});
}

TypePtr object(std::string type, std::vector<Field> fields) {
    return std::make_shared<Type>(Type{.kind = Type::Kind::Object,
                                       .primitive = Primitive::String,
                                       .object_type = std::move(type),
                                       .fields = std::move(fields),
                                       .element = {}});
}

TypePtr vector(TypePtr element) {
    return std::make_shared<Type>(Type{.kind = Type::Kind::Vector,
                                       .primitive = Primitive::String,
                                       .object_type = {},
                                       .fields = {},
                                       .element = std::move(element)});
}

TypePtr function(std::string type, std::vector<Field> fields) {
    return object(std::move(type), std::move(fields));
}

TypedFunction::TypedFunction(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

TypedFunction::TypedFunction(TypedFunction&&) noexcept = default;
TypedFunction& TypedFunction::operator=(TypedFunction&&) noexcept = default;
TypedFunction::~TypedFunction() = default;

std::string_view TypedFunction::name() const noexcept {
    return implementation_->name.view();
}

std::string_view TypedFunction::canonical() const noexcept {
    return implementation_->canonical.view();
}

const void* TypedFunction::identity() const noexcept {
    return &implementation_->value;
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

std::variant<TypedFunction, Failure> parse(std::string&& input, const TypePtr& schema,
                                           const secure::WipeObserver& wipe_observer) {
    const secure::SensitiveString source(std::move(input), wipe_observer, "raw_physical_input");
    if (source.empty()) {
        return Failure{Error::EmptyInput};
    }
    if (source.view().size() > kMaximumRequestBytes) {
        return Failure{Error::InputTooLarge};
    }
    if (!schema || schema->kind != Type::Kind::Object || !valid_schema(*schema)) {
        return Failure{Error::InvalidPolicyMetadata};
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
    const auto validation = validate(*root, *schema);
    if (validation != Error::InvalidJson) {
        return Failure{validation};
    }
    const auto& members = std::get<Object>(root->storage);
    const auto* tag = find(members, "@type");
    std::string canonical;
    canonical.reserve(source.view().size());
    if (!append_canonical(*root, *schema, canonical)) {
        secure::wipe(canonical, wipe_observer, "raw_canonical_failure");
        return Failure{Error::InvalidDouble};
    }
    if (canonical.size() > kMaximumRequestBytes) {
        secure::wipe(canonical, wipe_observer, "raw_canonical_oversized");
        return Failure{Error::CanonicalTooLarge};
    }
    auto implementation = std::make_unique<TypedFunction::Impl>(TypedFunction::Impl{
        .name = secure::SensitiveString(
            std::string(std::get<secure::SensitiveString>(tag->storage).view()), wipe_observer,
            "raw_function_name"),
        .value = std::move(*root),
        .schema = schema,
        .canonical = secure::SensitiveString(std::move(canonical), wipe_observer, "raw_canonical"),
    });
    return TypedFunction(std::move(implementation));
}

std::variant<Digest, Failure> response_digest(std::string_view function_name,
                                              std::string&& canonical_response) {
    const secure::SensitiveString response(std::move(canonical_response), {}, "raw_response");
    if (!ascii_identifier(function_name) || response.view().size() < 2) {
        return Failure{Error::InvalidPolicyMetadata};
    }
    if (response.view().size() > kMaximumResponseBytes) {
        return Failure{Error::CanonicalTooLarge};
    }
    common::Sha256 digest;
    digest.update("tgcli.raw.response.v1");
    constexpr std::array<unsigned char, 1> nul{0};
    digest.update(nul);
    digest.update(function_name);
    digest.update(nul);
    const auto length = static_cast<std::uint64_t>(response.view().size());
    const std::array<unsigned char, 8> size{
        static_cast<unsigned char>(length >> 56U), static_cast<unsigned char>(length >> 48U),
        static_cast<unsigned char>(length >> 40U), static_cast<unsigned char>(length >> 32U),
        static_cast<unsigned char>(length >> 24U), static_cast<unsigned char>(length >> 16U),
        static_cast<unsigned char>(length >> 8U),  static_cast<unsigned char>(length)};
    digest.update(size);
    digest.update(response.view());
    return Digest{.sha256 = "sha256:" + digest.finish_hex(), .bytes = response.view().size()};
}

} // namespace tgcli::daemon::raw
