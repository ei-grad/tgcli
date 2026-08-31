#include "daemon/raw_audit_log.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
using namespace tgcli::daemon::raw::audit_v3;

constexpr std::string_view kInvocation = "00112233445566778899aabbccddeeff";
constexpr std::string_view kToken = "ffeeddccbbaa99887766554433221100";
constexpr std::string_view kHash =
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class TempAudit final {
  public:
    TempAudit() {
        auto pattern = (std::filesystem::temp_directory_path() / "tgcli-raw-audit-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const auto* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
    }
    ~TempAudit() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    TempAudit(const TempAudit&) = delete;
    TempAudit& operator=(const TempAudit&) = delete;
    TempAudit(TempAudit&&) = delete;
    TempAudit& operator=(TempAudit&&) = delete;
    [[nodiscard]] const std::string& root() const noexcept {
        return root_;
    }

  private:
    std::string root_;
};

json intent() {
    return {
        {"schema_version", 3},          {"record_type", "raw_intent"},
        {"invocation_id", kInvocation}, {"function", "deleteMessages"},
        {"tier", "destructive"},        {"tdlib_sha", "a17f87c4cff7b90b278d12b91ba0614383aaee82"},
        {"request_sha256", kHash},      {"request_bytes", 128}};
}

json dispatch() {
    return {{"schema_version", 3},
            {"record_type", "raw_checkpoint"},
            {"invocation_id", kInvocation},
            {"stage", "raw_dispatch_started"},
            {"data", {{"dispatch_token", kToken}, {"generation", "7"}}}};
}

std::vector<json> records(const std::string& filename) {
    std::ifstream input(filename);
    std::vector<json> output;
    for (std::string line; std::getline(input, line);) {
        output.push_back(json::parse(line));
    }
    return output;
}

} // namespace

TEST_CASE("raw audit log repairs intent-only with exact durable none outcome",
          "[raw][audit-v3][filesystem][recovery]") {
    const TempAudit temporary;
    const Log log(temporary.root(), ::getuid());
    REQUIRE(log.append(intent()));
    const auto recovery = log.recover();
    CHECK(recovery.status == LogStatus::Repaired);
    const auto stored = records(log.path());
    REQUIRE(stored.size() == 2);
    CHECK(stored.back() == json{{"schema_version", 3},
                                {"record_type", "raw_outcome"},
                                {"invocation_id", kInvocation},
                                {"mutation_state", "none"},
                                {"terminal", nullptr}});
    CHECK(log.recover().status == LogStatus::Clean);
}

TEST_CASE("raw audit log seals dispatch-only exactly once without payload or resend",
          "[raw][audit-v3][filesystem][unconfirmed]") {
    const TempAudit temporary;
    const Log log(temporary.root(), ::getuid());
    REQUIRE(log.append(intent()));
    REQUIRE(log.append(dispatch()));
    const auto recovery = log.recover();
    REQUIRE(recovery.status == LogStatus::Unconfirmed);
    REQUIRE(recovery.unconfirmed);
    CHECK(recovery.unconfirmed->function == "deleteMessages");
    CHECK(recovery.unconfirmed->request_sha256 == kHash);
    const auto stored = records(log.path());
    REQUIRE(stored.size() == 3);
    CHECK(stored.back()["terminal"] == json{{"kind", "error_summary"},
                                            {"code", "RAW_OUTCOME_UNCONFIRMED"},
                                            {"td_error_code", nullptr}});
    CHECK(log.recover().status == LogStatus::Clean);
    const auto bytes = std::filesystem::file_size(log.path());
    CHECK(bytes > 0);
}

TEST_CASE("raw audit log rejects schema-invalid and contradictory records fail closed",
          "[raw][audit-v3][filesystem][negative]") {
    SECTION("schema and ordering") {
        const TempAudit temporary;
        const Log log(temporary.root(), ::getuid());
        auto leaked = intent();
        leaked["request"] = {{"@type", "deleteMessages"}};
        CHECK_FALSE(log.append(leaked));
        REQUIRE(log.append(intent()));
        REQUIRE(log.append(dispatch()));
        auto duplicate = dispatch();
        REQUIRE(log.append(duplicate));
        CHECK(log.recover().status == LogStatus::Contradiction);
    }
    SECTION("duplicate JSON key") {
        const TempAudit temporary;
        const Log log(temporary.root(), ::getuid());
        auto bytes = intent().dump();
        bytes.insert(1, R"("schema_version":3,)");
        std::ofstream output(log.path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << bytes << '\n';
        output.close();
        REQUIRE(output.good());
        CHECK(log.recover().status == LogStatus::Unavailable);
    }
    SECTION("possible outcome with missing terminal keys") {
        const TempAudit temporary;
        const Log log(temporary.root(), ::getuid());
        std::ofstream output(log.path(), std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << intent().dump() << '\n';
        output << dispatch().dump() << '\n';
        output << json({{"schema_version", 3},
                        {"record_type", "raw_outcome"},
                        {"invocation_id", kInvocation},
                        {"mutation_state", "possible"},
                        {"terminal", json::object()}})
                      .dump()
               << '\n';
        output.close();
        REQUIRE(output.good());
        REQUIRE(::chmod(log.path().c_str(), 0600) == 0);
        const auto bytes = std::filesystem::file_size(log.path());
        LogRecovery recovery;
        CHECK_NOTHROW(recovery = log.recover());
        CHECK(recovery.status == LogStatus::Contradiction);
        CHECK(std::filesystem::file_size(log.path()) == bytes);
    }
}
