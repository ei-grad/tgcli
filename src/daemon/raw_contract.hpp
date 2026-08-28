#pragma once

#include "common/secure_wipe.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tgcli::daemon::raw {

constexpr std::size_t kMaximumRequestBytes = 1'048'576;
constexpr std::size_t kMaximumResponseBytes = 16'777'216;
constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

enum class Primitive {
    Boolean,
    Int32,
    Int53,
    Int64,
    Bytes,
    String,
    Double,
};

struct Type;
using TypePtr = std::shared_ptr<const Type>;

struct Field {
    std::string name;
    TypePtr type;
    bool required = true;
};

struct Type {
    enum class Kind {
        Primitive,
        Object,
        Vector,
    };

    Kind kind = Kind::Primitive;
    Primitive primitive = Primitive::String;
    std::string object_type;
    std::vector<Field> fields;
    TypePtr element;
};

[[nodiscard]] TypePtr primitive(Primitive kind);
[[nodiscard]] TypePtr object(std::string type, std::vector<Field> fields);
[[nodiscard]] TypePtr vector(TypePtr element);
[[nodiscard]] TypePtr function(std::string type, std::vector<Field> fields);

enum class Error {
    EmptyInput,
    InputTooLarge,
    InvalidJson,
    MaximumDepthExceeded,
    DuplicateField,
    InvalidTopLevel,
    MissingType,
    UnexpectedType,
    UnknownField,
    MissingField,
    InvalidFieldType,
    InvalidInteger,
    InvalidBase64,
    InvalidDouble,
    CanonicalTooLarge,
    InvalidPolicyMetadata,
};

struct Failure {
    Error error = Error::InvalidJson;
};

struct Digest {
    std::string sha256;
    std::uint64_t bytes = 0;
};

class TypedFunction final {
  public:
    TypedFunction(TypedFunction&&) noexcept;
    TypedFunction& operator=(TypedFunction&&) noexcept;
    ~TypedFunction();

    TypedFunction(const TypedFunction&) = delete;
    TypedFunction& operator=(const TypedFunction&) = delete;

    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::string_view canonical() const noexcept;
    [[nodiscard]] const void* identity() const noexcept;
    [[nodiscard]] std::variant<Digest, Failure>
    request_digest(std::string_view tdlib_sha, std::string_view effective_tier) const;

  private:
    struct Impl;
    explicit TypedFunction(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;

    friend std::variant<TypedFunction, Failure> parse(std::string&& input, const TypePtr& schema,
                                                      const secure::WipeObserver& wipe_observer);
};

[[nodiscard]] std::variant<TypedFunction, Failure>
parse(std::string&& input, const TypePtr& schema, const secure::WipeObserver& wipe_observer = {});

[[nodiscard]] std::variant<Digest, Failure> response_digest(std::string_view function_name,
                                                            std::string&& canonical_response);

} // namespace tgcli::daemon::raw
