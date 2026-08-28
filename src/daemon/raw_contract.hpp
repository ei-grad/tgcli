#pragma once

#include "common/secure_wipe.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <td/tl/TlObject.h>

namespace td::td_api {
class Function;
class Object;
} // namespace td::td_api

namespace tgcli::daemon::raw {

constexpr std::size_t kMaximumRequestBytes = 1'048'576;
constexpr std::size_t kMaximumResponseBytes = 16'777'216;
constexpr std::size_t kMaximumJsonDepth = 64;
constexpr std::int64_t kMaximumInt53 = 9'007'199'254'740'991LL;

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
    NativeConversionFailed,
    UnexpectedResponseType,
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
    [[nodiscard]] std::string_view result_type() const noexcept;
    [[nodiscard]] std::string_view canonical() const noexcept;
    [[nodiscard]] const void* identity() const noexcept;
    [[nodiscard]] const td::td_api::Function& native() const noexcept;
    [[nodiscard]] std::variant<Digest, Failure>
    request_digest(std::string_view tdlib_sha, std::string_view effective_tier) const;

  private:
    struct Impl;
    explicit TypedFunction(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;

    friend std::variant<TypedFunction, Failure> parse(std::string&& input,
                                                      const secure::WipeObserver& wipe_observer);
};

[[nodiscard]] std::variant<TypedFunction, Failure>
parse(std::string&& input, const secure::WipeObserver& wipe_observer = {});

[[nodiscard]] std::variant<Digest, Failure>
response_digest(const TypedFunction& function, td::tl_object_ptr<td::td_api::Object> response,
                const secure::WipeObserver& wipe_observer = {});

[[nodiscard]] bool body_policy_allows(std::string_view validator,
                                      const TypedFunction& function) noexcept;

} // namespace tgcli::daemon::raw
