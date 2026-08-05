#include "core/td_authorization.hpp"

namespace tgcli::core {

namespace {

struct FunctionPolicy {
    DescriptorKind tier = DescriptorKind::Read;
    AuthState state = AuthState::Unknown;
    TdOwnerKind owner = TdOwnerKind::Request;
    bool login_owner_allowed = false;
    bool enabled = true;
};

FunctionPolicy policy_for(TdFunctionKind function) {
    switch (function) {
    case TdFunctionKind::GetAuthorizationState:
        return {DescriptorKind::AuthBootstrap, AuthState::Unknown, TdOwnerKind::InternalAuth};
    case TdFunctionKind::SetTdlibParameters:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitTdlibParameters,
                TdOwnerKind::InternalAuth, true};
    case TdFunctionKind::SetAuthenticationPhoneNumber:
    case TdFunctionKind::RequestQrCodeAuthentication:
    case TdFunctionKind::CheckAuthenticationBotToken:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitPhoneNumber, TdOwnerKind::Login};
    case TdFunctionKind::SetAuthenticationEmailAddress:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitEmailAddress, TdOwnerKind::Login};
    case TdFunctionKind::CheckAuthenticationEmailCode:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitEmailCode, TdOwnerKind::Login};
    case TdFunctionKind::CheckAuthenticationCode:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitCode, TdOwnerKind::Login};
    case TdFunctionKind::RegisterUser:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitRegistration, TdOwnerKind::Login};
    case TdFunctionKind::CheckAuthenticationPassword:
        return {DescriptorKind::AuthBootstrap, AuthState::WaitPassword, TdOwnerKind::Login};
    case TdFunctionKind::GetOption:
    case TdFunctionKind::GetMe:
    case TdFunctionKind::GetSavedMessagesTags:
    case TdFunctionKind::SearchSavedMessages:
        return {DescriptorKind::Read, AuthState::Ready, TdOwnerKind::Request};
    case TdFunctionKind::LogOut:
        return {DescriptorKind::Destructive, AuthState::Ready, TdOwnerKind::Request};
    case TdFunctionKind::Close:
        return {DescriptorKind::Lifecycle, AuthState::Unknown, TdOwnerKind::Lifecycle};
    }
    return {DescriptorKind::Write, AuthState::Unknown, TdOwnerKind::Request, false, false};
}

bool owner_matches(const TdSendDescriptor& descriptor, const FunctionPolicy& policy) {
    if (descriptor.owner.id == 0) {
        return false;
    }
    if (descriptor.owner.kind == policy.owner ||
        (policy.login_owner_allowed && descriptor.owner.kind == TdOwnerKind::Login)) {
        return true;
    }
    return false;
}

} // namespace

std::optional<TdAuthorizationFailure> authorize_td_send(const TdSendDescriptor& descriptor,
                                                        const TdFunctionData* function,
                                                        const AuthStateSnapshot& current,
                                                        bool generation_closed) {
    if (generation_closed) {
        return TdAuthorizationFailure::GenerationClosed;
    }
    if (descriptor.client_generation != current.client_generation) {
        return TdAuthorizationFailure::GenerationMismatch;
    }
    if (descriptor.auth_sequence != current.auth_sequence) {
        return TdAuthorizationFailure::AuthSequenceMismatch;
    }
    if (descriptor.auth_state != current.data.state) {
        return TdAuthorizationFailure::AuthStateMismatch;
    }
    if (function == nullptr || function->kind() != descriptor.function) {
        return TdAuthorizationFailure::FunctionMismatch;
    }

    const auto policy = policy_for(descriptor.function);
    if (!policy.enabled) {
        return TdAuthorizationFailure::FunctionDenied;
    }
    if (descriptor.tier != policy.tier) {
        return TdAuthorizationFailure::TierMismatch;
    }
    if (!owner_matches(descriptor, policy)) {
        return TdAuthorizationFailure::OwnerMismatch;
    }
    if (descriptor.function == TdFunctionKind::Close) {
        if (current.data.state == AuthState::Closed) {
            return TdAuthorizationFailure::FunctionDenied;
        }
        return std::nullopt;
    }
    if (descriptor.function == TdFunctionKind::GetAuthorizationState) {
        if (current.auth_sequence != 0 || current.data.state != AuthState::Unknown) {
            return TdAuthorizationFailure::FunctionDenied;
        }
        return std::nullopt;
    }
    if (current.data.state != policy.state) {
        return TdAuthorizationFailure::FunctionDenied;
    }
    return std::nullopt;
}

} // namespace tgcli::core
