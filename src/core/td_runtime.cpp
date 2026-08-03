#include "core/td_runtime.hpp"

#include "core/td_runtime_test_adapter.hpp"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

namespace tgcli::core {

namespace td_api = td::td_api;

namespace {

using NativeFunctionPtr = td_api::object_ptr<td_api::Function>;
using NativeObjectPtr = td_api::object_ptr<td_api::Object>;

void enforce_error_verbosity() {
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(1));
}

AuthCodeDeliveryInfo code_delivery(const td_api::AuthenticationCodeType& type) {
    switch (type.get_id()) {
    case td_api::authenticationCodeTypeTelegramMessage::ID:
        return {AuthCodeDelivery::TelegramMessage,
                static_cast<const td_api::authenticationCodeTypeTelegramMessage&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeSms::ID:
        return {AuthCodeDelivery::Sms,
                static_cast<const td_api::authenticationCodeTypeSms&>(type).length_, std::nullopt};
    case td_api::authenticationCodeTypeSmsWord::ID:
        return {AuthCodeDelivery::SmsWord, std::nullopt, std::nullopt};
    case td_api::authenticationCodeTypeSmsPhrase::ID:
        return {AuthCodeDelivery::SmsPhrase, std::nullopt, std::nullopt};
    case td_api::authenticationCodeTypeCall::ID:
        return {AuthCodeDelivery::Call,
                static_cast<const td_api::authenticationCodeTypeCall&>(type).length_, std::nullopt};
    case td_api::authenticationCodeTypeFlashCall::ID:
        return {AuthCodeDelivery::FlashCall, std::nullopt, std::nullopt};
    case td_api::authenticationCodeTypeMissedCall::ID:
        return {AuthCodeDelivery::MissedCall,
                static_cast<const td_api::authenticationCodeTypeMissedCall&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeFragment::ID:
        return {AuthCodeDelivery::Fragment,
                static_cast<const td_api::authenticationCodeTypeFragment&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeFirebaseAndroid::ID:
        return {AuthCodeDelivery::FirebaseAndroid,
                static_cast<const td_api::authenticationCodeTypeFirebaseAndroid&>(type).length_,
                std::nullopt};
    case td_api::authenticationCodeTypeFirebaseIos::ID:
        return {AuthCodeDelivery::FirebaseIos,
                static_cast<const td_api::authenticationCodeTypeFirebaseIos&>(type).length_,
                std::nullopt};
    default:
        return {AuthCodeDelivery::Unknown, std::nullopt, type.get_id()};
    }
}

struct EmailResetData {
    AuthEmailResetState state = AuthEmailResetState::None;
    std::int32_t delay = 0;
    std::optional<std::int32_t> unsupported_tdlib_type_id;
};

EmailResetData email_reset_state(const td_api::EmailAddressResetState* state) {
    if (state == nullptr) {
        return {};
    }
    switch (state->get_id()) {
    case td_api::emailAddressResetStateAvailable::ID:
        return {AuthEmailResetState::Available,
                static_cast<const td_api::emailAddressResetStateAvailable&>(*state).wait_period_,
                std::nullopt};
    case td_api::emailAddressResetStatePending::ID:
        return {AuthEmailResetState::Pending,
                static_cast<const td_api::emailAddressResetStatePending&>(*state).reset_in_,
                std::nullopt};
    default:
        return {AuthEmailResetState::Unknown, 0, state->get_id()};
    }
}

AuthStateData auth_state_data(const td_api::AuthorizationState& state) {
    switch (state.get_id()) {
    case td_api::authorizationStateWaitTdlibParameters::ID:
        return AuthStateData{AuthState::WaitTdlibParameters};
    case td_api::authorizationStateWaitPhoneNumber::ID:
        return AuthStateData{AuthState::WaitPhoneNumber};
    case td_api::authorizationStateWaitPremiumPurchase::ID: {
        const auto& value =
            static_cast<const td_api::authorizationStateWaitPremiumPurchase&>(state);
        return AuthStateData{
            AuthState::WaitPremiumPurchase,
            AuthWaitPremiumPurchase{.store_product_id = value.store_product_id_,
                                    .premium_day_count = value.premium_day_count_,
                                    .support_email_address = value.support_email_address_,
                                    .support_email_subject = value.support_email_subject_}};
    }
    case td_api::authorizationStateWaitEmailAddress::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitEmailAddress&>(state);
        return AuthStateData{AuthState::WaitEmailAddress,
                             AuthWaitEmailAddress{.allow_apple_id = value.allow_apple_id_,
                                                  .allow_google_id = value.allow_google_id_}};
    }
    case td_api::authorizationStateWaitEmailCode::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitEmailCode&>(state);
        const auto reset = email_reset_state(value.email_address_reset_state_.get());
        AuthWaitEmailCode metadata{.allow_apple_id = value.allow_apple_id_,
                                   .allow_google_id = value.allow_google_id_,
                                   .email_address_pattern = {},
                                   .expected_length = 0,
                                   .reset_state = reset.state,
                                   .reset_delay = reset.delay,
                                   .unsupported_reset_tdlib_type_id =
                                       reset.unsupported_tdlib_type_id};
        if (value.code_info_ != nullptr) {
            metadata.email_address_pattern = value.code_info_->email_address_pattern_;
            metadata.expected_length = value.code_info_->length_;
        }
        return AuthStateData{AuthState::WaitEmailCode, std::move(metadata)};
    }
    case td_api::authorizationStateWaitCode::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitCode&>(state);
        AuthWaitCode metadata;
        if (value.code_info_ != nullptr) {
            if (value.code_info_->type_ != nullptr) {
                metadata.delivery = code_delivery(*value.code_info_->type_);
            }
            if (value.code_info_->next_type_ != nullptr) {
                metadata.next_delivery = code_delivery(*value.code_info_->next_type_);
            }
            metadata.resend_timeout = value.code_info_->timeout_;
        }
        return AuthStateData{AuthState::WaitCode, metadata};
    }
    case td_api::authorizationStateWaitOtherDeviceConfirmation::ID: {
        const auto& value =
            static_cast<const td_api::authorizationStateWaitOtherDeviceConfirmation&>(state);
        return AuthStateData{AuthState::WaitOtherDeviceConfirmation,
                             AuthWaitOtherDeviceConfirmation{.link = value.link_}};
    }
    case td_api::authorizationStateWaitRegistration::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitRegistration&>(state);
        AuthWaitRegistration metadata;
        if (value.terms_of_service_ != nullptr) {
            if (value.terms_of_service_->text_ != nullptr) {
                metadata.terms_text = value.terms_of_service_->text_->text_;
            }
            metadata.minimum_user_age = value.terms_of_service_->min_user_age_;
            metadata.show_popup = value.terms_of_service_->show_popup_;
        }
        return AuthStateData{AuthState::WaitRegistration, std::move(metadata)};
    }
    case td_api::authorizationStateWaitPassword::ID: {
        const auto& value = static_cast<const td_api::authorizationStateWaitPassword&>(state);
        return AuthStateData{
            AuthState::WaitPassword,
            AuthWaitPassword{.hint = value.password_hint_,
                             .has_recovery_email_address = value.has_recovery_email_address_,
                             .has_passport_data = value.has_passport_data_,
                             .recovery_email_address_pattern =
                                 value.recovery_email_address_pattern_}};
    }
    case td_api::authorizationStateReady::ID:
        return AuthStateData{AuthState::Ready};
    case td_api::authorizationStateLoggingOut::ID:
        return AuthStateData{AuthState::LoggingOut};
    case td_api::authorizationStateClosing::ID:
        return AuthStateData{AuthState::Closing};
    case td_api::authorizationStateClosed::ID:
        return AuthStateData{AuthState::Closed};
    default:
        return AuthStateData{AuthState::Unknown, {}, state.get_id()};
    }
}

std::optional<AuthStateData> extract_auth_state(const td_api::Object& object,
                                                bool authorization_state_response) {
    if (object.get_id() == td_api::updateAuthorizationState::ID) {
        const auto& update = static_cast<const td_api::updateAuthorizationState&>(object);
        if (update.authorization_state_ == nullptr) {
            return std::nullopt;
        }
        return auth_state_data(*update.authorization_state_);
    }

    if (authorization_state_response && object.get_id() != td_api::error::ID) {
        return auth_state_data(static_cast<const td_api::AuthorizationState&>(object));
    }
    return std::nullopt;
}

class ProductionTdRuntime final : public TdRuntime {
  public:
    void initialize_process() override {
        enforce_error_verbosity();
    }

    std::int32_t create_client(std::uint64_t client_generation) override {
        const auto client_id = manager_.create_client_id();
        const std::lock_guard<std::mutex> lock(generations_mutex_);
        generations_.insert_or_assign(client_id, client_generation);
        return client_id;
    }

    TdValue make_function(TdBuiltinFunction function) override {
        switch (function) {
        case TdBuiltinFunction::GetAuthorizationState: {
            NativeFunctionPtr native = td_api::make_object<td_api::getAuthorizationState>();
            return TdValue::function(std::move(native), TdFunctionData{"getAuthorizationState"});
        }
        case TdBuiltinFunction::Close: {
            NativeFunctionPtr native = td_api::make_object<td_api::close>();
            return TdValue::function(std::move(native), TdFunctionData{"close"});
        }
        }
        throw std::logic_error("unknown built-in TDLib function");
    }

    void send(std::int32_t client_id, std::uint64_t client_generation, std::uint64_t query_id,
              TdValue function) override {
        static_cast<void>(client_generation);
        auto* native_function = function.get_if<NativeFunctionPtr>();
        if (native_function == nullptr || *native_function == nullptr) {
            throw std::invalid_argument("TdClient request does not contain a native function");
        }
        const bool is_authorization_state_query =
            (*native_function)->get_id() == td_api::getAuthorizationState::ID;
        if (is_authorization_state_query) {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            authorization_queries_[client_id].insert(query_id);
        }
        manager_.send(client_id, query_id, std::move(*native_function));
    }

    std::optional<TdRuntimeEvent> receive(std::chrono::milliseconds timeout) override {
        auto response = manager_.receive(static_cast<double>(timeout.count()) / 1000.0);
        if (response.object == nullptr) {
            return std::nullopt;
        }

        std::uint64_t generation = 0;
        bool authorization_state_response = false;
        {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            const auto it = generations_.find(response.client_id);
            if (it == generations_.end()) {
                return std::nullopt;
            }
            generation = it->second;
            const auto queries = authorization_queries_.find(response.client_id);
            if (queries != authorization_queries_.end()) {
                authorization_state_response = queries->second.erase(response.request_id) != 0;
                if (queries->second.empty()) {
                    authorization_queries_.erase(queries);
                }
            }
        }

        auto authorization_state =
            extract_auth_state(*response.object, authorization_state_response);
        if (authorization_state.has_value() && authorization_state->state == AuthState::Closed) {
            const std::lock_guard<std::mutex> lock(generations_mutex_);
            generations_.erase(response.client_id);
            authorization_queries_.erase(response.client_id);
        }
        return TdRuntimeEvent{.client_id = response.client_id,
                              .client_generation = generation,
                              .query_id = response.request_id,
                              .object = TdValue::from(std::move(response.object)),
                              .authorization_state = std::move(authorization_state)};
    }

  private:
    td::ClientManager manager_;
    std::mutex generations_mutex_;
    std::unordered_map<std::int32_t, std::uint64_t> generations_;
    std::unordered_map<std::int32_t, std::unordered_set<std::uint64_t>> authorization_queries_;
};

} // namespace

namespace detail {

std::optional<AuthStateData>
convert_production_authorization_state_for_test(const TdValue& object,
                                                bool authorization_state_response) {
    const auto* native = object.get_if<NativeObjectPtr>();
    if (native == nullptr || *native == nullptr) {
        return std::nullopt;
    }
    return extract_auth_state(**native, authorization_state_response);
}

} // namespace detail

std::unique_ptr<TdRuntime> make_production_td_runtime() {
    return std::make_unique<ProductionTdRuntime>();
}

std::string production_tdlib_version() {
    enforce_error_verbosity();
    auto value = td::ClientManager::execute(td_api::make_object<td_api::getOption>("version"));
    if (value != nullptr && value->get_id() == td_api::optionValueString::ID) {
        return static_cast<td_api::optionValueString&>(*value).value_;
    }
    return "unknown";
}

} // namespace tgcli::core
