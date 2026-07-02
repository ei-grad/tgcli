#include "proto/frame.hpp"

#include <utility>

namespace tgcli::proto {

namespace {

using nlohmann::json;

constexpr const char* kAuthorityGrant = "grant";
constexpr const char* kAuthorityDeny = "deny";
constexpr const char* kAuthorityUnset = "unset";

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

struct FrameWriter {
    json operator()(const Hello& f) const {
        return {{"type", "hello"},
                {"binary_version", f.binary_version},
                {"protocol_version", f.protocol_version}};
    }

    json operator()(const Request& f) const {
        json context{
            {"tty", f.context.tty},
            {"json", f.context.json},
            {"yes", f.context.yes},
            {"dry_run", f.context.dry_run},
            {"timeout",
             f.context.timeout_seconds ? json(*f.context.timeout_seconds) : json(nullptr)},
            {"cwd", f.context.cwd},
            {"media_dir", f.context.media_dir ? json(*f.context.media_dir) : json(nullptr)},
            {"write_authority", authority_to_json(f.context.write_authority)}};
        return {{"type", "request"},
                {"id", f.id},
                {"command", f.command},
                {"args", f.args},
                {"context", std::move(context)}};
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
    explicit Parser(const json& doc) : doc_(&doc) {}

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
            return parse_data_frame<Challenge>("challenge");
        }
        if (t == "answer") {
            return parse_data_frame<Answer>("answer");
        }
        return fail("unknown frame type '" + t + "'");
    }

  private:
    const json* field(const char* name) const {
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
        const json* binary = field("binary_version");
        const json* protocol = field("protocol_version");
        if (binary == nullptr || !binary->is_string()) {
            return fail("hello: missing or non-string 'binary_version'");
        }
        if (protocol == nullptr || !protocol->is_number_integer()) {
            return fail("hello: missing or non-integer 'protocol_version'");
        }
        return Hello{binary->get<std::string>(), protocol->get<int>()};
    }

    std::optional<Frame> parse_request() {
        auto id = parse_id();
        if (!id) {
            return std::nullopt;
        }
        const json* command = field("command");
        if (command == nullptr || !command->is_array() || command->empty()) {
            return fail("request: 'command' must be a non-empty array");
        }
        Request req;
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
        return req;
    }

    std::optional<RequestContext> parse_context(const json& context) {
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
            out.timeout_seconds = timeout->get<double>();
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
        return out;
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
        if (exit_code == nullptr || !exit_code->is_number_integer()) {
            return fail("error: missing or non-integer 'exit_code'");
        }
        return Error{*id, code->get<std::string>(), message->get<std::string>(), *details,
                     exit_code->get<int>()};
    }

    const json* doc_;
    std::string* error_ = nullptr;
};

} // namespace

std::string serialize(const Frame& frame) {
    return std::visit(FrameWriter{}, frame).dump();
}

std::optional<Frame> parse(std::string_view line, std::string& error) {
    const json doc = json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        error = "invalid JSON";
        return std::nullopt;
    }
    return Parser(doc).run(error);
}

} // namespace tgcli::proto
