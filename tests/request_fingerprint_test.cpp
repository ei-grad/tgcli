#include "daemon/request_fingerprint.hpp"
#include "daemon/resolver.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace tgcli;

namespace {

constexpr std::string_view kFileHash =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

const daemon::ResolverPrincipal kPrincipal{.id = 42, .is_bot = false};

std::string fingerprint(const daemon::FingerprintPayload& payload,
                        std::string_view account = "main",
                        daemon::ResolverPrincipal principal = kPrincipal) {
    auto result = daemon::request_fingerprint(account, principal, payload);
    REQUIRE(std::holds_alternative<std::string>(result));
    return std::get<std::string>(std::move(result));
}

daemon::SendFingerprintPayload send_payload() {
    return {.chat_selector = "-001001",
            .text = "Hello",
            .parse_mode = daemon::FingerprintParseMode::Plain,
            .reply_to = std::nullopt,
            .requested_topic = std::nullopt,
            .silent = false,
            .schedule = std::nullopt};
}

std::vector<std::pair<daemon::FingerprintPayload, std::string>> golden_cases() {
    using namespace daemon;
    return {
        {FingerprintPayload{send_payload()},
         "sha256:77fedce438c0d6c481e491465b9d43ee8f7a4abd5afd131df1a11161b81d22ea"},
        {FingerprintPayload{MsgEditFingerprintPayload{"@alice", 11, "revised"}},
         "sha256:14b796a4e2f5b8f7beeb07df8110b6247ae0b4a3bc708377193fef84de2ecdb0"},
        {FingerprintPayload{
             MsgDeleteFingerprintPayload{"t.me/alice", std::vector<std::int64_t>{11, 12}, true}},
         "sha256:84b8fc00d8b551cbccb36b3f7a07170f2d263031d8c1df2581f3430a46c096f7"},
        {FingerprintPayload{MsgForwardFingerprintPayload{"-1001", "@bob",
                                                         std::vector<std::int64_t>{21, 22}, false}},
         "sha256:1bc206f3126c15ce402f6e4813e6e1a12f770b498beb16dda2f68fb012358d59"},
        {FingerprintPayload{MsgReactFingerprintPayload{"@alice", 11, "👍", false, true}},
         "sha256:e1d463ca1adb705a6199a79e0e59249650cb5b6f7f38f51cd48be9d0ce7f74f1"},
        {FingerprintPayload{MsgPinFingerprintPayload{"@alice", 11}},
         "sha256:4037f9d22f88009522f0d8058cdd7bba5e29414da6cba02515529716443526b9"},
        {FingerprintPayload{MsgUnpinFingerprintPayload{"@alice", 11}},
         "sha256:989d9e36713a4600b48e417ecb33f243dedd2055edd39d84d1fe65fca19bc7bd"},
        {FingerprintPayload{ChatMarkReadFingerprintPayload{"@alice"}},
         "sha256:e020de245323eb5e4dc205dcc3c12976d2d820ba91bbf5643cad169e1e210357"},
        {FingerprintPayload{ChatMuteFingerprintPayload{"@alice", 3'600}},
         "sha256:3371330d718f4ac001da84dc542fbc1a8970ea1d760a4755ecf9fe706e19096b"},
        {FingerprintPayload{ChatUnmuteFingerprintPayload{"@alice", 0}},
         "sha256:98aba68c01c249afb10b98e1f9b599a10c95cde605e4ef8b37c6b187b104e364"},
        {FingerprintPayload{ChatPinFingerprintPayload{"@alice"}},
         "sha256:f5189907874211bdcaad11a0fe64ed187afae0d4c911de9069c92726cf55af9a"},
        {FingerprintPayload{ChatUnpinFingerprintPayload{"@alice"}},
         "sha256:9b6093c05010a4d7d1b9ca1fb0f32dc01bce4c975815043500fd4b8a30a994c0"},
        {FingerprintPayload{ChatArchiveFingerprintPayload{"@alice"}},
         "sha256:c09d580e7ab97c6e771578a4ffd09ec493745c387774ec157521bf3f5e43a2b9"},
        {FingerprintPayload{ChatUnarchiveFingerprintPayload{"@alice"}},
         "sha256:131d13e6870e041492430961105de388f1a661a25d9b6bb8cf24787338a9176a"},
        {FingerprintPayload{ChatJoinFingerprintPayload{ChatJoinUsernameFingerprint{"@alice"}}},
         "sha256:f7bbc74c6f9e304b89ffd1e4991160dc1c8fcf8cb379062f287a75a603243000"},
        {FingerprintPayload{ChatLeaveFingerprintPayload{"@group"}},
         "sha256:c44419fae0bf062901bbf65c2823aa556ddd40e380bbee5fcf857f0976debf08"},
        {FingerprintPayload{
             SavedAttachFingerprintPayload{50, "a.txt", 3, std::string(kFileHash), "cap"}},
         "sha256:d2db09acb4b7df6350eb37984ee15371182f8aeb7f09daa94efcbec371775f49"},
    };
}

void check_changed(const daemon::FingerprintPayload& original,
                   const daemon::FingerprintPayload& changed) {
    CHECK(fingerprint(original) != fingerprint(changed));
}

} // namespace

TEST_CASE("all seventeen operation payloads have independent golden fingerprints",
          "[fingerprint]") {
    const auto cases = golden_cases();
    REQUIRE(cases.size() == proto::m3_operation_identities().size());
    std::array<bool, proto::kM3OperationIdentities.size()> seen{};
    for (const auto& [payload, expected] : cases) {
        CHECK(fingerprint(payload) == expected);
        const auto operation = daemon::fingerprint_operation(payload);
        const auto* identity = proto::m3_operation_identity(operation);
        REQUIRE(identity != nullptr);
        const auto offset = static_cast<std::size_t>(operation);
        REQUIRE(offset < seen.size());
        CHECK_FALSE(seen.at(offset));
        seen.at(offset) = true;
    }
    CHECK(std::ranges::all_of(seen, [](bool value) { return value; }));
}

TEST_CASE("send fingerprint includes every caller-controlled field", "[fingerprint]") {
    using namespace daemon;
    const FingerprintPayload original{send_payload()};

    auto changed = send_payload();
    changed.chat_selector = "@alice";
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.text = "Other";
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.parse_mode = FingerprintParseMode::Html;
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.reply_to = 7;
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.requested_topic = TopicRef{.kind = TopicKind::Forum, .id = 8};
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.silent = true;
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.schedule = FingerprintScheduleOnline{};
    check_changed(original, FingerprintPayload{changed});
    changed = send_payload();
    changed.schedule = FingerprintScheduleAt{.send_date = 1'800'000'000};
    check_changed(original, FingerprintPayload{changed});

    CHECK(fingerprint(FingerprintPayload{send_payload()}) ==
          fingerprint(
              FingerprintPayload{SendFingerprintPayload{.chat_selector = "-1001",
                                                        .text = "Hello",
                                                        .parse_mode = FingerprintParseMode::Plain,
                                                        .reply_to = std::nullopt,
                                                        .requested_topic = std::nullopt,
                                                        .silent = false,
                                                        .schedule = std::nullopt}}));
}

TEST_CASE("every non-send payload field contributes to its fingerprint", "[fingerprint]") {
    using namespace daemon;

    check_changed(FingerprintPayload{MsgEditFingerprintPayload{"@a", 1, "x"}},
                  FingerprintPayload{MsgEditFingerprintPayload{"@b", 1, "x"}});
    check_changed(FingerprintPayload{MsgEditFingerprintPayload{"@a", 1, "x"}},
                  FingerprintPayload{MsgEditFingerprintPayload{"@a", 2, "x"}});
    check_changed(FingerprintPayload{MsgEditFingerprintPayload{"@a", 1, "x"}},
                  FingerprintPayload{MsgEditFingerprintPayload{"@a", 1, "y"}});

    check_changed(FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {1, 2}, false}},
                  FingerprintPayload{MsgDeleteFingerprintPayload{"@b", {1, 2}, false}});
    check_changed(FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {1, 2}, false}},
                  FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {1, 3}, false}});
    check_changed(FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {1, 2}, false}},
                  FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {1, 2}, true}});

    const FingerprintPayload forward{MsgForwardFingerprintPayload{"@a", "@b", {1, 2}, false}};
    check_changed(forward,
                  FingerprintPayload{MsgForwardFingerprintPayload{"@c", "@b", {1, 2}, false}});
    check_changed(forward,
                  FingerprintPayload{MsgForwardFingerprintPayload{"@a", "@c", {1, 2}, false}});
    check_changed(forward,
                  FingerprintPayload{MsgForwardFingerprintPayload{"@a", "@b", {1, 3}, false}});
    check_changed(forward,
                  FingerprintPayload{MsgForwardFingerprintPayload{"@a", "@b", {1, 2}, true}});

    const FingerprintPayload reaction{MsgReactFingerprintPayload{"@a", 1, "x", false, false}};
    check_changed(reaction,
                  FingerprintPayload{MsgReactFingerprintPayload{"@b", 1, "x", false, false}});
    check_changed(reaction,
                  FingerprintPayload{MsgReactFingerprintPayload{"@a", 2, "x", false, false}});
    check_changed(reaction,
                  FingerprintPayload{MsgReactFingerprintPayload{"@a", 1, "y", false, false}});
    check_changed(reaction,
                  FingerprintPayload{MsgReactFingerprintPayload{"@a", 1, "x", true, false}});
    check_changed(reaction,
                  FingerprintPayload{MsgReactFingerprintPayload{"@a", 1, "x", false, true}});

    check_changed(FingerprintPayload{MsgPinFingerprintPayload{"@a", 1}},
                  FingerprintPayload{MsgPinFingerprintPayload{"@b", 1}});
    check_changed(FingerprintPayload{MsgPinFingerprintPayload{"@a", 1}},
                  FingerprintPayload{MsgPinFingerprintPayload{"@a", 2}});
    check_changed(FingerprintPayload{MsgUnpinFingerprintPayload{"@a", 1}},
                  FingerprintPayload{MsgUnpinFingerprintPayload{"@a", 2}});

    check_changed(FingerprintPayload{ChatMarkReadFingerprintPayload{"@a"}},
                  FingerprintPayload{ChatMarkReadFingerprintPayload{"@b"}});
    check_changed(FingerprintPayload{ChatMuteFingerprintPayload{"@a", 1}},
                  FingerprintPayload{ChatMuteFingerprintPayload{"@a", 2}});
    check_changed(FingerprintPayload{ChatUnmuteFingerprintPayload{"@a", 0}},
                  FingerprintPayload{ChatUnmuteFingerprintPayload{"@b", 0}});
    check_changed(FingerprintPayload{ChatPinFingerprintPayload{"@a"}},
                  FingerprintPayload{ChatPinFingerprintPayload{"@b"}});
    check_changed(FingerprintPayload{ChatUnpinFingerprintPayload{"@a"}},
                  FingerprintPayload{ChatUnpinFingerprintPayload{"@b"}});
    check_changed(FingerprintPayload{ChatArchiveFingerprintPayload{"@a"}},
                  FingerprintPayload{ChatArchiveFingerprintPayload{"@b"}});
    check_changed(FingerprintPayload{ChatUnarchiveFingerprintPayload{"@a"}},
                  FingerprintPayload{ChatUnarchiveFingerprintPayload{"@b"}});
    check_changed(
        FingerprintPayload{ChatJoinFingerprintPayload{ChatJoinUsernameFingerprint{"@a"}}},
        FingerprintPayload{ChatJoinFingerprintPayload{ChatJoinUsernameFingerprint{"@b"}}});
    check_changed(FingerprintPayload{ChatLeaveFingerprintPayload{"@a"}},
                  FingerprintPayload{ChatLeaveFingerprintPayload{"@b"}});

    const FingerprintPayload attachment{
        SavedAttachFingerprintPayload{1, "a", 2, std::string(kFileHash), "caption"}};
    check_changed(attachment, FingerprintPayload{SavedAttachFingerprintPayload{
                                  2, "a", 2, std::string(kFileHash), "caption"}});
    check_changed(attachment, FingerprintPayload{SavedAttachFingerprintPayload{
                                  1, "b", 2, std::string(kFileHash), "caption"}});
    check_changed(attachment, FingerprintPayload{SavedAttachFingerprintPayload{
                                  1, "a", 3, std::string(kFileHash), "caption"}});
    check_changed(
        attachment,
        FingerprintPayload{SavedAttachFingerprintPayload{
            1, "a", 2, "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "caption"}});
    check_changed(attachment, FingerprintPayload{SavedAttachFingerprintPayload{
                                  1, "a", 2, std::string(kFileHash), "other"}});
}

TEST_CASE("account and exact principal identity contribute to the fingerprint", "[fingerprint]") {
    const daemon::FingerprintPayload payload{send_payload()};
    CHECK(fingerprint(payload, "main") != fingerprint(payload, "work"));
    CHECK(fingerprint(payload, "main", {.id = 43, .is_bot = false}) != fingerprint(payload));
    CHECK(fingerprint(payload, "main", {.id = 42, .is_bot = true}) != fingerprint(payload));
}

TEST_CASE("selector canonicalization is syntactic and byte-preserving", "[fingerprint]") {
    CHECK(daemon::canonical_write_selector("+00042") == std::optional<std::string>{"42"});
    CHECK(daemon::canonical_write_selector("-00042") == std::optional<std::string>{"-42"});
    CHECK(daemon::canonical_write_selector("@Case_Name") ==
          std::optional<std::string>{"@Case_Name"});
    CHECK(daemon::canonical_write_selector("https://t.me/Case_Name?start=A_B") ==
          std::optional<std::string>{"https://t.me/Case_Name?start=A_B"});
    CHECK_FALSE(daemon::canonical_write_selector("title substring"));
    CHECK_FALSE(daemon::canonical_write_selector("HTTPS://t.me/alice"));

    const std::string invite = "https://t.me/+RawInviteSentinel_77";
    const auto canonical_invite = daemon::canonical_write_selector(invite);
    REQUIRE(canonical_invite);
    CHECK(*canonical_invite == daemon::invite_link_hash(invite));
    CHECK(*canonical_invite ==
          "sha256:a8bf1dd9e4eadb0bdf828b5f324ea85bfa8f79853581cf10fea60f2437331357");
    CHECK(canonical_invite->find(invite) == std::string::npos);

    auto invite_send = send_payload();
    invite_send.chat_selector = invite;
    CHECK(fingerprint(daemon::FingerprintPayload{invite_send}) ==
          "sha256:dbcdb8fd83e13683aa84e6b093a4956e548ff23f914830e9bb20d0f0ba5a609d");
}

TEST_CASE("fingerprints defer globally admissible links and hash only exact invites",
          "[fingerprint][selector][secrets]") {
    struct LinkVector {
        std::string_view selector;
        std::string_view fingerprint;
    };
    constexpr std::array normal_links{
        LinkVector{"t.me/project",
                   "sha256:cc3077f522088d58973eca292523b472de60ff1641aa635b098186b5f792d9ae"},
        LinkVector{"t.me/project?start=A_B",
                   "sha256:e27a0264d01838cfa8adf9d7f1b3bb4696ff2f5a7f3de1c26def850b08003f66"},
        LinkVector{"t.me/project/123/456",
                   "sha256:8d65575f80be6593f254e096f24016fdc0d44e64529ce3a646a23b63ca255704"},
        LinkVector{"https://t.me/project?boost=abc&ref=1",
                   "sha256:742ef089a0bb4abb6c58a7ad052bd3bbf8d2b3b55da67bfaf1dbe4736a0bfcb0"},
        LinkVector{"t.me/project/+not_invite",
                   "sha256:7c4d48588d3a3c9453a63fb12218143fdd0cbf151a3654c71a7bcc3e41835cd3"},
        LinkVector{"t.me/joinchat/token/extra",
                   "sha256:3e1efdbb405d40bbe6b074cb99acc6fa02c1a696b1e5f06e3336c37254b965e8"},
    };
    for (const auto& [selector, expected_fingerprint] : normal_links) {
        INFO(selector);
        CHECK(daemon::valid_resolve_selector(selector));
        CHECK(daemon::canonical_write_selector(selector) == std::optional<std::string>{selector});
        auto send = send_payload();
        send.chat_selector = selector;
        CHECK(fingerprint(daemon::FingerprintPayload{send}) == expected_fingerprint);
    }

    constexpr std::array invite_links{
        std::pair{"t.me/+RawInviteSentinel_77",
                  "sha256:f97332b4a7a62eead038cabeff64d166a1ae12a6bd63f9ee5ce999cf36dbafd4"},
        std::pair{"https://t.me/joinchat/JoinChatSentinel_88",
                  "sha256:6bf05b2c238857f90a2de722ea4ef399a8fb06b8471b47d61da7522fea1ea723"},
    };
    for (const auto& [selector, expected_hash] : invite_links) {
        INFO(selector);
        CHECK(daemon::valid_resolve_selector(selector));
        CHECK(daemon::canonical_write_selector(selector) ==
              std::optional<std::string>{expected_hash});
    }

    CHECK_FALSE(daemon::canonical_write_selector("HTTPS://t.me/project"));
    CHECK_FALSE(daemon::canonical_write_selector("http://t.me/project"));
    CHECK_FALSE(daemon::canonical_write_selector(std::string("t.me/\xc3\x28", 7)));
}

TEST_CASE("fingerprints preserve array order and Unicode scalar bytes", "[fingerprint]") {
    using namespace daemon;
    const FingerprintPayload ordered{MsgForwardFingerprintPayload{"@a", "@b", {1, 2}, false}};
    auto reversed = request_fingerprint(
        "main", kPrincipal,
        FingerprintPayload{MsgForwardFingerprintPayload{"@a", "@b", {2, 1}, false}});
    REQUIRE(std::holds_alternative<FingerprintError>(reversed));
    CHECK(std::get<FingerprintError>(reversed) == FingerprintError::InvalidPayload);

    const FingerprintPayload composed{
        MsgEditFingerprintPayload{"@a", 1, std::string("\xc3\xa9", 2)}};
    const FingerprintPayload decomposed{
        MsgEditFingerprintPayload{"@a", 1, std::string("e\xcc\x81", 3)}};
    CHECK(fingerprint(composed) != fingerprint(decomposed));
    CHECK_FALSE(fingerprint(ordered).empty());
}

TEST_CASE("key and invite helpers never return their raw sentinels", "[fingerprint][secrets]") {
    const std::string key = "KeySentinel_2d74e16b";
    const std::string invite = "https://t.me/+InviteSentinel_79e234";
    const auto key_hash = daemon::idempotency_key_hash(key);
    const auto invite_hash = daemon::invite_link_hash(invite);
    CHECK(key_hash.find(key) == std::string::npos);
    CHECK(invite_hash.find(invite) == std::string::npos);
    CHECK(key_hash.starts_with("sha256:"));
    CHECK(invite_hash.starts_with("sha256:"));

    const daemon::FingerprintPayload join{
        daemon::ChatJoinFingerprintPayload{daemon::ChatJoinInviteFingerprint{invite_hash}}};
    const auto result = fingerprint(join);
    CHECK(result.find(invite) == std::string::npos);
    CHECK(fingerprint(daemon::FingerprintPayload{
              daemon::ChatJoinFingerprintPayload{daemon::ChatJoinInviteFingerprint{
                  "sha256:6605f3c708c3aa61aed5f82aa89061e8e96a1f0340264de1fc6476fbc4aa4f76"}}}) ==
          "sha256:4e89f40747a96c8342aafd7e890ef62bb2e594dba8f384753d8ec1bf4feecfea");
}

TEST_CASE("invalid fingerprint facts fail without returning caller bytes", "[fingerprint]") {
    using namespace daemon;
    const FingerprintPayload valid{send_payload()};
    for (const auto& [account, principal, payload] :
         std::vector<std::tuple<std::string, ResolverPrincipal, FingerprintPayload>>{
             {"bad account", kPrincipal, valid},
             {"main", {.id = 0, .is_bot = false}, valid},
             {"main", {.id = -1, .is_bot = false}, valid},
             {"main", kPrincipal, FingerprintPayload{MsgEditFingerprintPayload{"title", 1, "x"}}},
             {"main", kPrincipal,
              FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {2, 1}, false}}},
             {"main", kPrincipal, FingerprintPayload{ChatMuteFingerprintPayload{"@a", 31'622'401}}},
             {"main", kPrincipal, FingerprintPayload{ChatUnmuteFingerprintPayload{"@a", 1}}},
             {"main", kPrincipal,
              FingerprintPayload{ChatJoinFingerprintPayload{
                  ChatJoinInviteFingerprint{"raw-invite-must-not-be-accepted"}}}},
             {"main", kPrincipal,
              FingerprintPayload{
                  SavedAttachFingerprintPayload{1, "../bad", 1, std::string(kFileHash), ""}}}}) {
        const auto result = request_fingerprint(account, principal, payload);
        REQUIRE(std::holds_alternative<FingerprintError>(result));
    }

    auto invalid_send = send_payload();
    invalid_send.requested_topic = TopicRef{.kind = TopicKind::Saved, .id = 1};
    CHECK(std::holds_alternative<FingerprintError>(
        request_fingerprint("main", kPrincipal, FingerprintPayload{invalid_send})));
    invalid_send = send_payload();
    invalid_send.schedule = FingerprintScheduleAt{.send_date = 0};
    CHECK(std::holds_alternative<FingerprintError>(
        request_fingerprint("main", kPrincipal, FingerprintPayload{invalid_send})));
    invalid_send = send_payload();
    invalid_send.parse_mode = static_cast<FingerprintParseMode>(99);
    CHECK(std::holds_alternative<FingerprintError>(
        request_fingerprint("main", kPrincipal, FingerprintPayload{invalid_send})));
}

TEST_CASE("fingerprint validation pins caller-input boundaries", "[fingerprint]") {
    using namespace daemon;
    const auto accepted = [](const FingerprintPayload& payload) {
        return std::holds_alternative<std::string>(
            request_fingerprint("main", kPrincipal, payload));
    };

    auto send = send_payload();
    send.text.assign(4'096, 'a');
    CHECK(accepted(FingerprintPayload{send}));
    send.text.push_back('a');
    CHECK_FALSE(accepted(FingerprintPayload{send}));
    send = send_payload();
    send.reply_to = 9'007'199'254'740'991LL;
    CHECK(accepted(FingerprintPayload{send}));
    send.reply_to = -9'007'199'254'740'991LL;
    CHECK(accepted(FingerprintPayload{send}));
    send.reply_to = 0;
    CHECK_FALSE(accepted(FingerprintPayload{send}));
    send.reply_to = -9'007'199'254'740'992LL;
    CHECK_FALSE(accepted(FingerprintPayload{send}));
    send.reply_to = std::nullopt;
    send.requested_topic =
        TopicRef{.kind = TopicKind::Forum, .id = std::numeric_limits<std::int32_t>::max()};
    CHECK(accepted(FingerprintPayload{send}));
    send.requested_topic = TopicRef{.kind = TopicKind::Forum, .id = -1};
    CHECK_FALSE(accepted(FingerprintPayload{send}));
    send.requested_topic =
        TopicRef{.kind = TopicKind::Forum,
                 .id = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1};
    CHECK_FALSE(accepted(FingerprintPayload{send}));

    CHECK(accepted(FingerprintPayload{
        MsgReactFingerprintPayload{"@a", 1, std::string(64, 'x'), false, false}}));
    CHECK_FALSE(accepted(FingerprintPayload{
        MsgReactFingerprintPayload{"@a", 1, std::string(65, 'x'), false, false}}));

    std::vector<std::int64_t> one_hundred;
    for (std::int64_t value = 1; value <= 100; ++value) {
        one_hundred.push_back(value);
    }
    CHECK(accepted(FingerprintPayload{MsgDeleteFingerprintPayload{"@a", one_hundred, false}}));
    one_hundred.push_back(101);
    CHECK_FALSE(
        accepted(FingerprintPayload{MsgDeleteFingerprintPayload{"@a", one_hundred, false}}));

    const std::string emoji = "\xf0\x9f\x91\x8d";
    std::string maximum_caption;
    for (std::size_t index = 0; index < 1'024; ++index) {
        maximum_caption += emoji;
    }
    CHECK(accepted(FingerprintPayload{SavedAttachFingerprintPayload{
        1, std::string(255, 'n'), 1, std::string(kFileHash), maximum_caption}}));
    CHECK_FALSE(accepted(FingerprintPayload{SavedAttachFingerprintPayload{
        1, std::string(256, 'n'), 1, std::string(kFileHash), maximum_caption}}));
    maximum_caption += emoji;
    CHECK_FALSE(accepted(FingerprintPayload{
        SavedAttachFingerprintPayload{1, "name", 1, std::string(kFileHash), maximum_caption}}));

    CHECK(daemon::canonical_write_selector("+9007199254740991") ==
          std::optional<std::string>{"9007199254740991"});
    CHECK_FALSE(daemon::canonical_write_selector("+9007199254740992"));
    CHECK_FALSE(daemon::canonical_write_selector(std::string("@a\0b", 4)));
}

TEST_CASE("all fingerprint message ids are signed nonzero int53", "[fingerprint][message-id]") {
    using namespace daemon;
    constexpr std::int64_t minimum = -9'007'199'254'740'991LL;
    constexpr std::int64_t maximum = 9'007'199'254'740'991LL;
    const auto accepted = [](const FingerprintPayload& payload) {
        return std::holds_alternative<std::string>(
            request_fingerprint("main", kPrincipal, payload));
    };

    CHECK(accepted(FingerprintPayload{MsgEditFingerprintPayload{"@a", minimum, "x"}}));
    CHECK(accepted(
        FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {minimum, -1, maximum}, false}}));
    CHECK(accepted(FingerprintPayload{
        MsgForwardFingerprintPayload{"@a", "@b", {minimum, -1, maximum}, false}}));
    CHECK(accepted(FingerprintPayload{MsgReactFingerprintPayload{"@a", -1, "x", false, false}}));
    CHECK(accepted(FingerprintPayload{MsgPinFingerprintPayload{"@a", -1}}));
    CHECK(accepted(FingerprintPayload{MsgUnpinFingerprintPayload{"@a", -1}}));
    CHECK(accepted(FingerprintPayload{
        SavedAttachFingerprintPayload{minimum, "name", 1, std::string(kFileHash), ""}}));

    CHECK_FALSE(accepted(FingerprintPayload{MsgEditFingerprintPayload{"@a", 0, "x"}}));
    CHECK_FALSE(accepted(
        FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {minimum, 0, maximum}, false}}));
    CHECK_FALSE(
        accepted(FingerprintPayload{MsgDeleteFingerprintPayload{"@a", {minimum, -1, -1}, false}}));
    CHECK_FALSE(accepted(
        FingerprintPayload{MsgForwardFingerprintPayload{"@a", "@b", {-1, minimum}, false}}));
    CHECK_FALSE(
        accepted(FingerprintPayload{MsgReactFingerprintPayload{"@a", 0, "x", false, false}}));
    CHECK_FALSE(accepted(FingerprintPayload{MsgPinFingerprintPayload{"@a", 0}}));
    CHECK_FALSE(accepted(FingerprintPayload{MsgUnpinFingerprintPayload{"@a", 0}}));
    CHECK_FALSE(accepted(FingerprintPayload{
        SavedAttachFingerprintPayload{0, "name", 1, std::string(kFileHash), ""}}));
}

TEST_CASE("chat mute fingerprints admit only explicit durations or the default sentinel",
          "[fingerprint][mute]") {
    using namespace daemon;
    const auto accepted = [](std::int32_t duration) {
        return std::holds_alternative<std::string>(request_fingerprint(
            "main", kPrincipal,
            FingerprintPayload{ChatMuteFingerprintPayload{"@alice", duration}}));
    };

    CHECK(accepted(1));
    CHECK(accepted(31'622'400));
    CHECK(accepted(std::numeric_limits<std::int32_t>::max()));
    CHECK(fingerprint(FingerprintPayload{
              ChatMuteFingerprintPayload{"@alice", std::numeric_limits<std::int32_t>::max()}}) ==
          "sha256:a0fe93dcf9b27be0a85ddc174db571dc0dabaed078de51f8bd8d6643789b82ed");
    CHECK_FALSE(accepted(0));
    CHECK_FALSE(accepted(31'622'401));
    CHECK_FALSE(accepted(std::numeric_limits<std::int32_t>::max() - 1));
}
