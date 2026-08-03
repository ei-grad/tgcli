#include "common/daemon_lock.hpp"

#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;

namespace {

constexpr std::string_view kToken = "0123456789abcdef0123456789abcdef";

std::string valid_start() {
#if defined(__linux__)
    return "linux:42";
#elif defined(__APPLE__)
    return "macos:42:7";
#else
    return "unsupported:42";
#endif
}

std::string valid_record(std::string_view pid = "1") {
    return "tgcli-lock-v1 " + std::string(pid) + " " + valid_start() + " " + std::string(kToken) +
           "\n";
}

} // namespace

TEST_CASE("frozen daemon identity record accepts PID 1", "[daemon-lock][r4]") {
    daemon_lock::Identity identity;
    std::string error;
    REQUIRE(daemon_lock::parse_identity_record(valid_record(), identity, error));
    CHECK(identity.pid == 1);
    CHECK(identity.process_start == valid_start());
    CHECK(identity.control_token == kToken);
    CHECK(daemon_lock::owner_pid_matches(identity.pid, 1, error));
}

TEST_CASE("frozen daemon identity owner and fields fail closed", "[daemon-lock][r4]") {
    daemon_lock::Identity identity;
    std::string error;
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("0"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record() + "extra\n", identity, error));
    REQUIRE(daemon_lock::parse_identity_record(valid_record(), identity, error));
    CHECK_FALSE(daemon_lock::owner_pid_matches(identity.pid, 2, error));
}

TEST_CASE("frozen daemon identity parser is byte exact", "[daemon-lock][r1]") {
    daemon_lock::Identity identity;
    std::string error;
    const std::string valid = valid_record();

    CHECK_FALSE(
        daemon_lock::parse_identity_record(valid.substr(0, valid.size() - 1), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid + " ", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 " + valid_start() + " " + std::string(kToken) + " \n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1\t1 " + valid_start() + " " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("01"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("+1"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(valid_record("-1"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        valid_record("999999999999999999999999999999999999"), identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 arbitrary:42 " + std::string(kToken) + "\n", identity, error));
#if defined(__linux__)
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:42:7 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:0 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:01 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:+1 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record("tgcli-lock-v1 1 linux:18446744073709551616 " +
                                                       std::string(kToken) + "\n",
                                                   identity, error));
#elif defined(__APPLE__)
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 linux:42 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:01:7 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:42:07 " + std::string(kToken) + "\n", identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 macos:42:1000000 " + std::string(kToken) + "\n", identity, error));
#endif
    CHECK_FALSE(daemon_lock::parse_identity_record("tgcli-lock-v1 1 " + valid_start() +
                                                       " 0123456789abcdef0123456789abcdeF\n",
                                                   identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record("tgcli-lock-v1 1 " + valid_start() +
                                                       " 0123456789abcdef0123456789abcdeg\n",
                                                   identity, error));
    CHECK_FALSE(daemon_lock::parse_identity_record(
        "tgcli-lock-v1 1 " + valid_start() + " 0123456789abcdef\n", identity, error));
}

TEST_CASE("owner observation distinguishes transitions from stable corruption",
          "[daemon-lock][r3]") {
    const std::string record = valid_record("42");
    daemon_lock::Identity identity{42, valid_start(), std::string(kToken)};
    std::string error;
    using daemon_lock::detail::ObservationStatus;
    const auto classify = [&](bool final_held, pid_t final_owner,
                              std::optional<std::string_view> initial_record,
                              std::optional<std::string_view> final_record,
                              const daemon_lock::Identity* parsed,
                              std::optional<std::string_view> live_start) {
        return daemon_lock::detail::classify_owner_observation(
            42, final_held, final_owner, initial_record, final_record, parsed, live_start, error);
    };

    CHECK(classify(true, 42, record, record, &identity, identity.process_start) ==
          ObservationStatus::Stable);
    CHECK(classify(false, -1, record, record, &identity, identity.process_start) ==
          ObservationStatus::Transition);
    CHECK(classify(true, 43, record, record, &identity, identity.process_start) ==
          ObservationStatus::Transition);
    CHECK(classify(true, 42, record, record + " ", &identity, identity.process_start) ==
          ObservationStatus::Transition);
    CHECK(classify(true, 42, std::nullopt, record, nullptr, std::nullopt) ==
          ObservationStatus::Transition);

    daemon_lock::Identity wrong_pid = identity;
    wrong_pid.pid = 43;
    CHECK(classify(true, 42, record, record, &wrong_pid, identity.process_start) ==
          ObservationStatus::Invalid);
    CHECK(classify(true, 42, record, record, &identity, "linux:999") == ObservationStatus::Invalid);
    CHECK(classify(true, 42, std::nullopt, std::nullopt, nullptr, std::nullopt) ==
          ObservationStatus::Invalid);
}
