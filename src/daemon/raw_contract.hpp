#pragma once

#include "common/secure_wipe.hpp"
#include "core/td_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <td/tl/TlObject.h>

namespace td::td_api {
class Function;
class Object;
} // namespace td::td_api

namespace tgcli::daemon {
enum class Tier;
}

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

struct MaterializedResponse {
    Digest digest;
    secure::SensitiveString canonical;
    std::string response_type;
    std::optional<std::int32_t> td_error_code;
    std::optional<std::int32_t> retry_after;
};

struct MalformedResponse {
    std::optional<std::string> response_type;
    std::optional<Digest> digest;
};

struct OversizedResponse {
    std::string response_type;
};

using ClassifiedResponse =
    std::variant<MaterializedResponse, MalformedResponse, OversizedResponse, Failure>;

enum class BodyPolicyDecision { Deny, Preserve, RaiseWrite, RaiseDestructive };
enum class AdmissionTier { Denied, Read, Write, Destructive };
enum class RawPrincipal { User, Bot, Both };

namespace detail {
enum class OwnershipFailpoint {
    None,
    AfterNativeConversion,
    BeforeImplementationAllocation,
};
} // namespace detail

struct RawPreflightPlan {
    static constexpr std::size_t kMaximumChatTargets = 8;
    std::array<std::int64_t, kMaximumChatTargets> non_secret_chat_ids{};
    std::size_t non_secret_chat_count = 0;
};

struct BodyPolicyOutcome {
    BodyPolicyDecision decision = BodyPolicyDecision::Deny;
    std::optional<Tier> effective_tier;
    RawPreflightPlan preflight;
};

struct RawPolicyMetadata {
    std::string_view name;
    RawPrincipal principal = RawPrincipal::Both;
    AdmissionTier admission = AdmissionTier::Denied;
    std::string_view body_validator;
    bool sensitive_input = true;
    bool sensitive_output = true;
    bool reviewed = false;
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
    [[nodiscard]] std::optional<core::TdValue> release_for_dispatch(Tier effective_tier);
    [[nodiscard]] std::variant<Digest, Failure>
    request_digest(std::string_view tdlib_sha, std::string_view effective_tier) const;

  private:
    struct Impl;
    explicit TypedFunction(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;

    friend std::variant<TypedFunction, Failure> parse(std::string&& input,
                                                      const secure::WipeObserver& wipe_observer,
                                                      detail::OwnershipFailpoint failpoint);
};

[[nodiscard]] std::variant<TypedFunction, Failure>
parse(std::string&& input, const secure::WipeObserver& wipe_observer = {},
      detail::OwnershipFailpoint failpoint = detail::OwnershipFailpoint::None);

[[nodiscard]] std::variant<Digest, Failure>
response_digest(const TypedFunction& function, td::tl_object_ptr<td::td_api::Object> response,
                const secure::WipeObserver& wipe_observer = {});

[[nodiscard]] std::variant<MaterializedResponse, Failure>
materialize_response(const TypedFunction& function, core::TdRawObjectPtr response,
                     const secure::WipeObserver& wipe_observer = {});

[[nodiscard]] ClassifiedResponse classify_response(const TypedFunction& function,
                                                   core::TdRawObjectPtr response,
                                                   const secure::WipeObserver& wipe_observer = {});

[[nodiscard]] BodyPolicyOutcome apply_body_policy_decision(AdmissionTier static_tier,
                                                           BodyPolicyDecision decision) noexcept;

[[nodiscard]] std::optional<RawPolicyMetadata>
policy_metadata(const TypedFunction& function) noexcept;
[[nodiscard]] std::optional<std::string_view>
declared_result_type(std::string_view function_name) noexcept;
[[nodiscard]] bool response_type_matches_result(std::string_view function_name,
                                                std::string_view response_type) noexcept;

[[nodiscard]] BodyPolicyOutcome evaluate_body_policy(const TypedFunction& function) noexcept;
[[nodiscard]] bool policy_activation_ready() noexcept;

} // namespace tgcli::daemon::raw
