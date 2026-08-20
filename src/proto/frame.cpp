#include "proto/frame.hpp"

#include "common/paths.hpp"
#include "proto/destructive_plan.hpp"
#include "proto/operation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tgcli::proto {

namespace {

using nlohmann::json;

constexpr const char* kAuthorityGrant = "grant";
constexpr const char* kAuthorityDeny = "deny";
constexpr const char* kAuthorityUnset = "unset";

using NamedChallenge = std::pair<ChallengeKind, std::string_view>;
constexpr std::array kChallengeKinds{
    NamedChallenge{ChallengeKind::ApiId, "api_id"},
    NamedChallenge{ChallengeKind::ApiHash, "api_hash"},
    NamedChallenge{ChallengeKind::DatabaseKey, "database_key"},
    NamedChallenge{ChallengeKind::PhoneNumber, "phone_number"},
    NamedChallenge{ChallengeKind::AuthenticationCode, "authentication_code"},
    NamedChallenge{ChallengeKind::EmailAddress, "email_address"},
    NamedChallenge{ChallengeKind::EmailCode, "email_code"},
    NamedChallenge{ChallengeKind::Password, "password"},
    NamedChallenge{ChallengeKind::BotToken, "bot_token"},
    NamedChallenge{ChallengeKind::RegistrationTerms, "registration_terms"},
    NamedChallenge{ChallengeKind::RegistrationFirstName, "registration_first_name"},
    NamedChallenge{ChallengeKind::RegistrationLastName, "registration_last_name"},
    NamedChallenge{ChallengeKind::DestructiveConfirmation, "destructive_confirmation"},
};

bool exact_fields(const json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) {
        return false;
    }
    return std::all_of(fields.begin(), fields.end(), [&value](std::string_view field_name) {
        return value.contains(std::string(field_name));
    });
}

bool nonnegative_integer(const json& value, bool positive = false) {
    if (value.is_number_unsigned()) {
        return !positive || value.get<std::uint64_t>() != 0;
    }
    if (!value.is_number_integer()) {
        return false;
    }
    const auto number = value.get<std::int64_t>();
    return positive ? number > 0 : number >= 0;
}

bool nullable_nonnegative_integer(const json& value) {
    return value.is_null() || nonnegative_integer(value);
}

bool valid_nonce(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const auto& nonce = value.get_ref<const std::string&>();
    return nonce.size() == 32 && std::all_of(nonce.begin(), nonce.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

bool validate_destructive_details(const json& details, std::string& error) {
    if (!exact_fields(details, {"action", "target"}) || !details["action"].is_string() ||
        !details["target"].is_object()) {
        error = "challenge: invalid destructive_confirmation details";
        return false;
    }
    std::string plan_error;
    const auto plan = parse_destructive_plan(details["target"], plan_error);
    const auto& action = details["action"].get_ref<const std::string&>();
    const bool matches =
        plan && ((action == "logout" && std::holds_alternative<LogoutPlan>(*plan)) ||
                 (action == "account_remove" && std::holds_alternative<AccountRemovePlan>(*plan)));
    if (!matches) {
        error = "challenge: invalid destructive_confirmation target";
    }
    return matches;
}

bool validate_challenge_details(ChallengeKind kind, const json& details, std::string& error) {
    switch (kind) {
    case ChallengeKind::AuthenticationCode: {
        if (!exact_fields(details, {"delivery_type", "expected_length", "resend_timeout"}) ||
            !details["delivery_type"].is_string() ||
            (!details["expected_length"].is_null() &&
             !nonnegative_integer(details["expected_length"])) ||
            !nonnegative_integer(details["resend_timeout"])) {
            error = "challenge: invalid authentication_code details";
            return false;
        }
        static constexpr std::array<std::string_view, 10> delivery_types{
            "telegram_message", "sms",         "sms_word", "sms_phrase",       "call",
            "flash_call",       "missed_call", "fragment", "firebase_android", "firebase_ios"};
        const auto& delivery = details["delivery_type"].get_ref<const std::string&>();
        if (std::find(delivery_types.begin(), delivery_types.end(), delivery) ==
            delivery_types.end()) {
            error = "challenge: unknown authentication_code delivery_type";
            return false;
        }
        return true;
    }
    case ChallengeKind::EmailCode:
        if (!exact_fields(details, {"address_pattern", "expected_length"}) ||
            !details["address_pattern"].is_string() ||
            !nonnegative_integer(details["expected_length"])) {
            error = "challenge: invalid email_code details";
            return false;
        }
        return true;
    case ChallengeKind::Password:
        if (!exact_fields(details, {"hint", "has_recovery_email", "has_passport_data",
                                    "recovery_email_pattern"}) ||
            !details["hint"].is_string() || !details["has_recovery_email"].is_boolean() ||
            !details["has_passport_data"].is_boolean() ||
            !details["recovery_email_pattern"].is_string()) {
            error = "challenge: invalid password details";
            return false;
        }
        return true;
    case ChallengeKind::RegistrationTerms:
        if (!exact_fields(details, {"text", "min_user_age", "show_popup"}) ||
            !details["text"].is_string() || !nonnegative_integer(details["min_user_age"]) ||
            !details["show_popup"].is_boolean()) {
            error = "challenge: invalid registration_terms details";
            return false;
        }
        return true;
    case ChallengeKind::DestructiveConfirmation:
        return validate_destructive_details(details, error);
    case ChallengeKind::ApiId:
    case ChallengeKind::ApiHash:
    case ChallengeKind::DatabaseKey:
    case ChallengeKind::PhoneNumber:
    case ChallengeKind::EmailAddress:
    case ChallengeKind::BotToken:
    case ChallengeKind::RegistrationFirstName:
    case ChallengeKind::RegistrationLastName:
        if (!details.is_object() || !details.empty()) {
            error = "challenge: details must be empty for this kind";
            return false;
        }
        return true;
    }
    error = "challenge: unknown kind";
    return false;
}

json authority_to_json(WriteAuthority authority) {
    switch (authority) {
    case WriteAuthority::Grant:
        return kAuthorityGrant;
    case WriteAuthority::Deny:
        return kAuthorityDeny;
    case WriteAuthority::Unset:
        return kAuthorityUnset;
    }
    return kAuthorityUnset; // unreachable for a valid enum value
}

std::optional<WriteAuthority> authority_from_json(const json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto& s = value.get_ref<const std::string&>();
    if (s == kAuthorityGrant) {
        return WriteAuthority::Grant;
    }
    if (s == kAuthorityDeny) {
        return WriteAuthority::Deny;
    }
    if (s == kAuthorityUnset) {
        return WriteAuthority::Unset;
    }
    return std::nullopt;
}

enum class IdempotencyValidation { Valid, InvalidGrammar, DryRun, UnsupportedOperation };

IdempotencyValidation validate_idempotency(const Request& request) {
    if (!request.context.idempotency_key) {
        return IdempotencyValidation::Valid;
    }
    if (!valid_idempotency_key(*request.context.idempotency_key)) {
        return IdempotencyValidation::InvalidGrammar;
    }
    if (request.context.dry_run) {
        return IdempotencyValidation::DryRun;
    }
    if (!m3_operation_for_command(request.command)) {
        return IdempotencyValidation::UnsupportedOperation;
    }
    return IdempotencyValidation::Valid;
}

std::string_view idempotency_validation_error(IdempotencyValidation validation) {
    switch (validation) {
    case IdempotencyValidation::Valid:
        return {};
    case IdempotencyValidation::InvalidGrammar:
        return "request context: invalid 'idempotency_key'";
    case IdempotencyValidation::DryRun:
        return "request context: 'idempotency_key' and 'dry_run' are mutually exclusive";
    case IdempotencyValidation::UnsupportedOperation:
        return "request context: 'idempotency_key' is unsupported for this command";
    }
    return "request context: invalid 'idempotency_key'";
}

struct FrameWriter {
    json operator()(const Hello& f) const {
        return {{"type", "hello"},
                {"binary_version", f.binary_version},
                {"protocol_version", f.protocol_version}};
    }

    json operator()(const Request& f) const {
        if (!paths::valid_account_name(f.account)) {
            throw std::invalid_argument("request account is invalid");
        }
        if (const auto validation = validate_idempotency(f);
            validation != IdempotencyValidation::Valid) {
            throw std::invalid_argument(std::string(idempotency_validation_error(validation)));
        }
        json context{
            {"tty", f.context.tty},
            {"json", f.context.json},
            {"yes", f.context.yes},
            {"dry_run", f.context.dry_run},
            {"timeout",
             f.context.timeout_seconds ? json(*f.context.timeout_seconds) : json(nullptr)},
            {"cwd", f.context.cwd},
            {"media_dir", f.context.media_dir ? json(*f.context.media_dir) : json(nullptr)},
            {"write_authority", authority_to_json(f.context.write_authority)},
            {"idempotency_key",
             f.context.idempotency_key ? json(*f.context.idempotency_key) : json(nullptr)}};
        return {{"type", "request"},    {"id", f.id},     {"account", f.account},
                {"command", f.command}, {"args", f.args}, {"context", std::move(context)}};
    }

    json operator()(const Result& f) const {
        return {{"type", "result"}, {"id", f.id}, {"data", f.data}};
    }

    json operator()(const Item& f) const {
        return {{"type", "item"}, {"id", f.id}, {"data", f.data}};
    }

    json operator()(const Progress& f) const {
        return {{"type", "progress"}, {"id", f.id}, {"data", f.data}};
    }

    json operator()(const Error& f) const {
        return {{"type", "error"},
                {"id", f.id},
                {"error", json{{"code", f.code}, {"message", f.message}, {"details", f.details}}},
                {"exit_code", f.exit_code}};
    }

    json operator()(const Challenge& f) const {
        return {{"type", "challenge"}, {"id", f.id}, {"challenge", f.challenge}};
    }

    json operator()(const Answer& f) const {
        return {{"type", "answer"}, {"id", f.id}, {"answer", f.answer}};
    }
};

class Parser {
  public:
    Parser(json& doc, secure::WipeObserver wipe_observer,
           std::optional<Answer>* invalid_answer = nullptr)
        : doc_(&doc), wipe_observer_(std::move(wipe_observer)), invalid_answer_(invalid_answer) {}

    std::optional<Frame> run(std::string& error) {
        error_ = &error;
        if (!doc_->is_object()) {
            return fail("frame is not a JSON object");
        }
        const json* type = field("type");
        if (type == nullptr || !type->is_string()) {
            return fail("missing or non-string 'type'");
        }
        const auto& t = type->get_ref<const std::string&>();
        if (t == "hello") {
            return parse_hello();
        }
        if (t == "request") {
            return parse_request();
        }
        if (t == "result") {
            return parse_data_frame<Result>("data");
        }
        if (t == "item") {
            return parse_data_frame<Item>("data");
        }
        if (t == "progress") {
            return parse_data_frame<Progress>("data");
        }
        if (t == "error") {
            return parse_error();
        }
        if (t == "challenge") {
            return parse_challenge();
        }
        if (t == "answer") {
            return parse_answer();
        }
        return fail("unknown frame type '" + t + "'");
    }

  private:
    json* field(const char* name) const {
        auto it = doc_->find(name);
        return it == doc_->end() ? nullptr : &*it;
    }

    std::nullopt_t fail(std::string message) {
        *error_ = std::move(message);
        return std::nullopt;
    }

    std::optional<std::uint64_t> parse_id() {
        const json* id = field("id");
        if (id == nullptr || !id->is_number_unsigned()) {
            fail("missing or non-unsigned-integer 'id'");
            return std::nullopt;
        }
        return id->get<std::uint64_t>();
    }

    std::optional<Frame> parse_hello() {
        if (!exact_fields(*doc_, {"type", "binary_version", "protocol_version"})) {
            return fail("hello: frame must contain exactly the protocol fields");
        }
        const json* binary = field("binary_version");
        const json* protocol = field("protocol_version");
        if (binary == nullptr || !binary->is_string()) {
            return fail("hello: missing or non-string 'binary_version'");
        }
        if (protocol == nullptr || !protocol->is_number_integer() ||
            (protocol->is_number_unsigned() &&
             protocol->get<std::uint64_t>() >
                 static_cast<std::uint64_t>(std::numeric_limits<int>::max())) ||
            (protocol->is_number_integer() && !protocol->is_number_unsigned() &&
             (protocol->get<std::int64_t>() < std::numeric_limits<int>::min() ||
              protocol->get<std::int64_t>() > std::numeric_limits<int>::max()))) {
            return fail("hello: missing or non-integer 'protocol_version'");
        }
        return Hello{binary->get<std::string>(), protocol->get<int>()};
    }

    std::optional<Frame> parse_request() {
        if (!exact_fields(*doc_, {"type", "id", "account", "command", "args", "context"})) {
            return fail("request: frame must contain exactly the protocol fields");
        }
        auto id = parse_id();
        if (!id) {
            return std::nullopt;
        }
        const json* account = field("account");
        if (account == nullptr || !account->is_string()) {
            return fail("request: missing or non-string 'account'");
        }
        const auto& account_name = account->get_ref<const std::string&>();
        if (!paths::valid_account_name(account_name)) {
            return fail("request: invalid 'account'");
        }
        const json* command = field("command");
        if (command == nullptr || !command->is_array() || command->empty()) {
            return fail("request: 'command' must be a non-empty array");
        }
        Request req(account_name);
        req.id = *id;
        for (const auto& part : *command) {
            if (!part.is_string()) {
                return fail("request: 'command' elements must be strings");
            }
            req.command.push_back(part.get<std::string>());
        }
        const json* args = field("args");
        if (args == nullptr || !args->is_object()) {
            return fail("request: missing or non-object 'args'");
        }
        req.args = *args;
        const json* context = field("context");
        if (context == nullptr || !context->is_object()) {
            return fail("request: missing or non-object 'context'");
        }
        if (auto parsed = parse_context(*context)) {
            req.context = std::move(*parsed);
        } else {
            return std::nullopt;
        }
        if (const auto validation = validate_idempotency(req);
            validation != IdempotencyValidation::Valid) {
            return fail(std::string(idempotency_validation_error(validation)));
        }
        return req;
    }

    std::optional<RequestContext> parse_context(const json& context) {
        if (!exact_fields(context, {"tty", "json", "yes", "dry_run", "timeout", "cwd", "media_dir",
                                    "write_authority", "idempotency_key"})) {
            fail("request context: must contain exactly the protocol fields");
            return std::nullopt;
        }
        RequestContext out;
        struct BoolField {
            const char* name;
            bool RequestContext::*member;
        };
        for (const auto& [name, member] :
             {BoolField{"tty", &RequestContext::tty}, BoolField{"json", &RequestContext::json},
              BoolField{"yes", &RequestContext::yes},
              BoolField{"dry_run", &RequestContext::dry_run}}) {
            auto it = context.find(name);
            if (it == context.end() || !it->is_boolean()) {
                fail(std::string("request context: missing or non-boolean '") + name + "'");
                return std::nullopt;
            }
            out.*member = it->get<bool>();
        }
        auto timeout = context.find("timeout");
        if (timeout == context.end() || (!timeout->is_null() && !timeout->is_number())) {
            fail("request context: 'timeout' must be a number or null");
            return std::nullopt;
        }
        if (timeout->is_number()) {
            const double seconds = timeout->get<double>();
            if (!request_deadline(seconds, DeadlineDefault::Default60)) {
                fail("request context: 'timeout' must be finite, positive, and representable");
                return std::nullopt;
            }
            out.timeout_seconds = seconds;
        }
        auto cwd = context.find("cwd");
        if (cwd == context.end() || !cwd->is_string()) {
            fail("request context: missing or non-string 'cwd'");
            return std::nullopt;
        }
        out.cwd = cwd->get<std::string>();
        auto media_dir = context.find("media_dir");
        if (media_dir == context.end() || (!media_dir->is_null() && !media_dir->is_string())) {
            fail("request context: 'media_dir' must be a string or null");
            return std::nullopt;
        }
        if (media_dir->is_string()) {
            out.media_dir = media_dir->get<std::string>();
        }
        auto authority = context.find("write_authority");
        if (authority == context.end()) {
            fail("request context: missing 'write_authority'");
            return std::nullopt;
        }
        if (auto parsed = authority_from_json(*authority)) {
            out.write_authority = *parsed;
        } else {
            fail("request context: 'write_authority' must be one of "
                 "\"grant\", \"deny\", \"unset\"");
            return std::nullopt;
        }
        if (!parse_idempotency_key(context, out)) {
            return std::nullopt;
        }
        return out;
    }

    bool parse_idempotency_key(const json& context, RequestContext& out) {
        const auto idempotency_key = context.find("idempotency_key");
        if (idempotency_key == context.end() ||
            (!idempotency_key->is_null() && !idempotency_key->is_string())) {
            fail("request context: 'idempotency_key' must be a string or null");
            return false;
        }
        if (idempotency_key->is_string()) {
            const auto& key = idempotency_key->get_ref<const std::string&>();
            if (!valid_idempotency_key(key)) {
                fail("request context: invalid 'idempotency_key'");
                return false;
            }
            out.idempotency_key = key;
        }
        return true;
    }

    template <typename FrameT> std::optional<Frame> parse_data_frame(const char* payload_key) {
        auto id = parse_id();
        if (!id) {
            return std::nullopt;
        }
        const json* payload = field(payload_key);
        if (payload == nullptr) {
            return fail(std::string("missing '") + payload_key + "'");
        }
        return FrameT{*id, *payload};
    }

    std::optional<Frame> parse_challenge() {
        auto id = parse_id();
        if (!id) {
            return std::nullopt;
        }
        const json* payload = field("challenge");
        if (payload == nullptr) {
            return fail("missing 'challenge'");
        }
        std::string validation_error;
        if (!validate_challenge_payload(*payload, validation_error)) {
            return fail(std::move(validation_error));
        }
        return Challenge{*id, *payload};
    }

    std::optional<Frame> parse_answer() {
        auto id = parse_id();
        if (!id) {
            return std::nullopt;
        }
        json* payload = field("answer");
        if (payload == nullptr) {
            return fail("missing 'answer'");
        }
        std::string validation_error;
        if (!validate_answer_payload(*payload, validation_error)) {
            if (invalid_answer_ != nullptr) {
                invalid_answer_->emplace(*id, std::move(*payload), wipe_observer_);
            }
            return fail(std::move(validation_error));
        }
        return Answer{*id, std::move(*payload), wipe_observer_};
    }

    std::optional<Frame> parse_error() {
        auto id = parse_id();
        if (!id) {
            return std::nullopt;
        }
        const json* error_obj = field("error");
        if (error_obj == nullptr || !error_obj->is_object()) {
            return fail("error: missing or non-object 'error'");
        }
        auto code = error_obj->find("code");
        auto message = error_obj->find("message");
        auto details = error_obj->find("details");
        if (code == error_obj->end() || !code->is_string()) {
            return fail("error: missing or non-string 'error.code'");
        }
        if (message == error_obj->end() || !message->is_string()) {
            return fail("error: missing or non-string 'error.message'");
        }
        if (details == error_obj->end() || !details->is_object()) {
            return fail("error: missing or non-object 'error.details'");
        }
        const json* exit_code = field("exit_code");
        if (exit_code == nullptr || !exit_code->is_number_integer() ||
            (exit_code->is_number_unsigned() &&
             exit_code->get<std::uint64_t>() >
                 static_cast<std::uint64_t>(std::numeric_limits<int>::max())) ||
            (exit_code->is_number_integer() && !exit_code->is_number_unsigned() &&
             (exit_code->get<std::int64_t>() < std::numeric_limits<int>::min() ||
              exit_code->get<std::int64_t>() > std::numeric_limits<int>::max()))) {
            return fail("error: missing or non-integer 'exit_code'");
        }
        return Error{*id, code->get<std::string>(), message->get<std::string>(), *details,
                     exit_code->get<int>()};
    }

    json* doc_;
    std::string* error_ = nullptr;
    secure::WipeObserver wipe_observer_;
    std::optional<Answer>* invalid_answer_ = nullptr;
};

} // namespace

Answer::Answer() = default;

Request::Request(std::string account_value) : account(std::move(account_value)) {
    if (!paths::valid_account_name(account)) {
        throw std::invalid_argument("request account is invalid");
    }
}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
Answer::Answer(std::uint64_t id_value, nlohmann::json&& answer_value,
               secure::WipeObserver wipe_observer_value)
    : id(id_value), wipe_observer_(std::move(wipe_observer_value)) {
    secure::transfer(answer_value, answer, wipe_observer_, "answer_source");
}

Answer::~Answer() {
    secure::wipe(answer, wipe_observer_, "answer_payload");
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,cppcoreguidelines-prefer-member-initializer,performance-noexcept-move-constructor)
Answer::Answer(Answer&& other) : id(other.id) {
    secure::transfer(other.answer, answer, other.wipe_observer_, "answer_move_source");
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    wipe_observer_ = std::move(other.wipe_observer_);
}

// NOLINTNEXTLINE(bugprone-exception-escape,cppcoreguidelines-noexcept-move-operations,performance-noexcept-move-constructor)
Answer& Answer::operator=(Answer&& other) {
    if (this != &other) {
        secure::wipe(answer, wipe_observer_, "answer_payload");
        id = other.id;
        secure::transfer(other.answer, answer, other.wipe_observer_, "answer_move_source");
        wipe_observer_ = std::move(other.wipe_observer_);
    }
    return *this;
}

const secure::WipeObserver& Answer::wipe_observer() const {
    return wipe_observer_;
}

std::string_view challenge_kind_name(ChallengeKind kind) {
    const auto* const it =
        std::find_if(kChallengeKinds.begin(), kChallengeKinds.end(),
                     [kind](const NamedChallenge& item) { return item.first == kind; });
    return it == kChallengeKinds.end() ? std::string_view{} : it->second;
}

std::optional<ChallengeKind> parse_challenge_kind(std::string_view name) {
    const auto* const it =
        std::find_if(kChallengeKinds.begin(), kChallengeKinds.end(),
                     [name](const NamedChallenge& item) { return item.second == name; });
    if (it == kChallengeKinds.end()) {
        return std::nullopt;
    }
    return it->first;
}

bool challenge_kind_is_secret(ChallengeKind kind) {
    switch (kind) {
    case ChallengeKind::ApiHash:
    case ChallengeKind::DatabaseKey:
    case ChallengeKind::AuthenticationCode:
    case ChallengeKind::EmailCode:
    case ChallengeKind::Password:
    case ChallengeKind::BotToken:
        return true;
    case ChallengeKind::ApiId:
    case ChallengeKind::PhoneNumber:
    case ChallengeKind::EmailAddress:
    case ChallengeKind::RegistrationTerms:
    case ChallengeKind::RegistrationFirstName:
    case ChallengeKind::RegistrationLastName:
    case ChallengeKind::DestructiveConfirmation:
        return false;
    }
    return false;
}

bool challenge_kind_expects_boolean(ChallengeKind kind) {
    return kind == ChallengeKind::RegistrationTerms ||
           kind == ChallengeKind::DestructiveConfirmation;
}

bool validate_challenge_payload(const json& payload, std::string& error) {
    if (!exact_fields(payload, {"kind", "nonce", "sequence", "client_generation", "auth_sequence",
                                "secret", "prompt", "details"})) {
        error = "challenge: payload must contain exactly the protocol fields";
        return false;
    }
    if (!payload["kind"].is_string()) {
        error = "challenge: kind must be a string";
        return false;
    }
    const auto kind = parse_challenge_kind(payload["kind"].get_ref<const std::string&>());
    if (!kind) {
        error = "challenge: unknown kind";
        return false;
    }
    if (!valid_nonce(payload["nonce"])) {
        error = "challenge: nonce must be 32 lowercase hexadecimal characters";
        return false;
    }
    if (!nonnegative_integer(payload["sequence"], true)) {
        error = "challenge: sequence must be a positive integer";
        return false;
    }
    if (!nullable_nonnegative_integer(payload["client_generation"]) ||
        !nullable_nonnegative_integer(payload["auth_sequence"]) ||
        payload["client_generation"].is_null() != payload["auth_sequence"].is_null()) {
        error = "challenge: generation fields must be matching integers or null";
        return false;
    }
    if (*kind != ChallengeKind::DestructiveConfirmation && payload["client_generation"].is_null()) {
        error = "challenge: authentication kinds require generation fields";
        return false;
    }
    if (!payload["secret"].is_boolean() ||
        payload["secret"].get<bool>() != challenge_kind_is_secret(*kind)) {
        error = "challenge: secret flag does not match kind";
        return false;
    }
    if (!payload["prompt"].is_string()) {
        error = "challenge: prompt must be a string";
        return false;
    }
    return validate_challenge_details(*kind, payload["details"], error);
}

bool validate_answer_payload(const json& payload, std::string& error) {
    if (!payload.is_object() || payload.size() != 5 || !payload.contains("nonce") ||
        !payload.contains("sequence") || !payload.contains("client_generation") ||
        !payload.contains("auth_sequence")) {
        error = "answer: payload must contain exactly identity fields and value or cancellation";
        return false;
    }
    const bool has_value = payload.contains("value");
    const bool has_cancelled = payload.contains("cancelled");
    if (has_value == has_cancelled) {
        error = "answer: exactly one of value or cancelled is required";
        return false;
    }
    if (!valid_nonce(payload["nonce"]) || !nonnegative_integer(payload["sequence"], true) ||
        !nullable_nonnegative_integer(payload["client_generation"]) ||
        !nullable_nonnegative_integer(payload["auth_sequence"]) ||
        payload["client_generation"].is_null() != payload["auth_sequence"].is_null()) {
        error = "answer: invalid challenge identity";
        return false;
    }
    if (has_cancelled) {
        if (!payload["cancelled"].is_boolean() || !payload["cancelled"].get<bool>()) {
            error = "answer: cancelled must be true";
            return false;
        }
        return true;
    }
    if (!payload["value"].is_string() && !payload["value"].is_boolean()) {
        error = "answer: value must be a string or boolean";
        return false;
    }
    return true;
}

std::string serialize(const Frame& frame, const secure::WipeObserver& wipe_observer) {
    auto document = std::visit(FrameWriter{}, frame);
    const secure::JsonWiper document_wiper(document, wipe_observer, "serialized_json");
    return document.dump();
}

std::optional<Frame> parse(std::string line, std::string& error,
                           const secure::WipeObserver& wipe_observer,
                           std::optional<Answer>* invalid_answer) {
    const secure::StringWiper line_wiper(line, wipe_observer, "parsed_line");
    auto doc = json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        error = "invalid JSON";
        return std::nullopt;
    }
    const secure::JsonWiper document_wiper(doc, wipe_observer, "parsed_json");
    try {
        return Parser(doc, wipe_observer, invalid_answer).run(error);
    } catch (const std::exception&) {
        error = "invalid frame value";
        return std::nullopt;
    }
}

std::optional<Answer> parse_answer_candidate(std::string line,
                                             const secure::WipeObserver& wipe_observer) {
    const secure::StringWiper line_wiper(line, wipe_observer, "candidate_line");
    auto doc = json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return std::nullopt;
    }
    const secure::JsonWiper document_wiper(doc, wipe_observer, "candidate_json");
    const auto type = doc.find("type");
    const auto id = doc.find("id");
    const auto answer = doc.find("answer");
    if (type == doc.end() || !type->is_string() ||
        type->get_ref<const std::string&>() != "answer" || id == doc.end() ||
        !id->is_number_unsigned() || answer == doc.end()) {
        return std::nullopt;
    }
    try {
        return Answer{id->get<std::uint64_t>(), std::move(*answer), wipe_observer};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace tgcli::proto
