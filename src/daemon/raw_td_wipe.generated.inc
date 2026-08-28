// Generated from pinned td_api.tl. Do not edit.
void wipe_native_object(td::td_api::Object& value,
                        const secure::WipeObserver& observer) noexcept {
    switch (value.get_id()) {
case td::td_api::accentColor::ID: {
    auto& typed = static_cast<td::td_api::accentColor&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.built_in_accent_color_id_, observer);
    wipe_native_value(typed.light_theme_colors_, observer);
    wipe_native_value(typed.dark_theme_colors_, observer);
    wipe_native_value(typed.min_channel_chat_boost_level_, observer);
    return;
}
case td::td_api::acceptedGiftTypes::ID: {
    auto& typed = static_cast<td::td_api::acceptedGiftTypes&>(value);
    wipe_native_value(typed.unlimited_gifts_, observer);
    wipe_native_value(typed.limited_gifts_, observer);
    wipe_native_value(typed.upgraded_gifts_, observer);
    wipe_native_value(typed.gifts_from_channels_, observer);
    wipe_native_value(typed.premium_subscription_, observer);
    return;
}
case td::td_api::accountInfo::ID: {
    auto& typed = static_cast<td::td_api::accountInfo&>(value);
    wipe_native_value(typed.registration_month_, observer);
    wipe_native_value(typed.registration_year_, observer);
    wipe_native_value(typed.phone_number_country_code_, observer);
    wipe_native_value(typed.last_name_change_date_, observer);
    wipe_native_value(typed.last_photo_change_date_, observer);
    return;
}
case td::td_api::accountTtl::ID: {
    auto& typed = static_cast<td::td_api::accountTtl&>(value);
    wipe_native_value(typed.days_, observer);
    return;
}
case td::td_api::activeStoryStateLive::ID: {
    auto& typed = static_cast<td::td_api::activeStoryStateLive&>(value);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::activeStoryStateRead::ID: {
    return;
}
case td::td_api::activeStoryStateUnread::ID: {
    return;
}
case td::td_api::addedProxies::ID: {
    auto& typed = static_cast<td::td_api::addedProxies&>(value);
    wipe_native_value(typed.proxies_, observer);
    return;
}
case td::td_api::addedProxy::ID: {
    auto& typed = static_cast<td::td_api::addedProxy&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.last_used_date_, observer);
    wipe_native_value(typed.is_enabled_, observer);
    wipe_native_value(typed.comment_, observer);
    wipe_native_value(typed.proxy_, observer);
    return;
}
case td::td_api::addedReaction::ID: {
    auto& typed = static_cast<td::td_api::addedReaction&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.is_outgoing_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::addedReactions::ID: {
    auto& typed = static_cast<td::td_api::addedReactions&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.reactions_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::address::ID: {
    auto& typed = static_cast<td::td_api::address&>(value);
    wipe_native_value(typed.country_code_, observer);
    wipe_native_value(typed.state_, observer);
    wipe_native_value(typed.city_, observer);
    wipe_native_value(typed.street_line1_, observer);
    wipe_native_value(typed.street_line2_, observer);
    wipe_native_value(typed.postal_code_, observer);
    return;
}
case td::td_api::advertisementSponsor::ID: {
    auto& typed = static_cast<td::td_api::advertisementSponsor&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.info_, observer);
    return;
}
case td::td_api::affiliateInfo::ID: {
    auto& typed = static_cast<td::td_api::affiliateInfo&>(value);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.affiliate_chat_id_, observer);
    wipe_native_value(typed.star_amount_, observer);
    return;
}
case td::td_api::affiliateProgramInfo::ID: {
    auto& typed = static_cast<td::td_api::affiliateProgramInfo&>(value);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.end_date_, observer);
    wipe_native_value(typed.daily_revenue_per_user_amount_, observer);
    return;
}
case td::td_api::affiliateProgramParameters::ID: {
    auto& typed = static_cast<td::td_api::affiliateProgramParameters&>(value);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.month_count_, observer);
    return;
}
case td::td_api::affiliateProgramSortOrderCreationDate::ID: {
    return;
}
case td::td_api::affiliateProgramSortOrderProfitability::ID: {
    return;
}
case td::td_api::affiliateProgramSortOrderRevenue::ID: {
    return;
}
case td::td_api::affiliateTypeBot::ID: {
    auto& typed = static_cast<td::td_api::affiliateTypeBot&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::affiliateTypeChannel::ID: {
    auto& typed = static_cast<td::td_api::affiliateTypeChannel&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::affiliateTypeCurrentUser::ID: {
    return;
}
case td::td_api::ageVerificationParameters::ID: {
    auto& typed = static_cast<td::td_api::ageVerificationParameters&>(value);
    wipe_native_value(typed.min_age_, observer);
    wipe_native_value(typed.verification_bot_username_, observer);
    wipe_native_value(typed.country_, observer);
    return;
}
case td::td_api::alternativeVideo::ID: {
    auto& typed = static_cast<td::td_api::alternativeVideo&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.codec_, observer);
    wipe_native_value(typed.hls_file_, observer);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::animatedChatPhoto::ID: {
    auto& typed = static_cast<td::td_api::animatedChatPhoto&>(value);
    wipe_native_value(typed.length_, observer);
    wipe_native_value(typed.file_, observer);
    wipe_native_value(typed.main_frame_timestamp_, observer);
    return;
}
case td::td_api::animatedEmoji::ID: {
    auto& typed = static_cast<td::td_api::animatedEmoji&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.sticker_width_, observer);
    wipe_native_value(typed.sticker_height_, observer);
    wipe_native_value(typed.fitzpatrick_type_, observer);
    wipe_native_value(typed.sound_, observer);
    return;
}
case td::td_api::animation::ID: {
    auto& typed = static_cast<td::td_api::animation&>(value);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.file_name_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.has_stickers_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::animations::ID: {
    auto& typed = static_cast<td::td_api::animations&>(value);
    wipe_native_value(typed.animations_, observer);
    return;
}
case td::td_api::archiveChatListSettings::ID: {
    auto& typed = static_cast<td::td_api::archiveChatListSettings&>(value);
    wipe_native_value(typed.archive_and_mute_new_chats_from_unknown_users_, observer);
    wipe_native_value(typed.keep_unmuted_chats_archived_, observer);
    wipe_native_value(typed.keep_chats_from_folders_archived_, observer);
    return;
}
case td::td_api::attachmentMenuBot::ID: {
    auto& typed = static_cast<td::td_api::attachmentMenuBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.supports_self_chat_, observer);
    wipe_native_value(typed.supports_user_chats_, observer);
    wipe_native_value(typed.supports_bot_chats_, observer);
    wipe_native_value(typed.supports_group_chats_, observer);
    wipe_native_value(typed.supports_channel_chats_, observer);
    wipe_native_value(typed.request_write_access_, observer);
    wipe_native_value(typed.is_added_, observer);
    wipe_native_value(typed.show_in_attachment_menu_, observer);
    wipe_native_value(typed.show_in_side_menu_, observer);
    wipe_native_value(typed.show_disclaimer_in_side_menu_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.name_color_, observer);
    wipe_native_value(typed.default_icon_, observer);
    wipe_native_value(typed.ios_static_icon_, observer);
    wipe_native_value(typed.ios_animated_icon_, observer);
    wipe_native_value(typed.ios_side_menu_icon_, observer);
    wipe_native_value(typed.android_icon_, observer);
    wipe_native_value(typed.android_side_menu_icon_, observer);
    wipe_native_value(typed.macos_icon_, observer);
    wipe_native_value(typed.macos_side_menu_icon_, observer);
    wipe_native_value(typed.icon_color_, observer);
    wipe_native_value(typed.web_app_placeholder_, observer);
    return;
}
case td::td_api::attachmentMenuBotColor::ID: {
    auto& typed = static_cast<td::td_api::attachmentMenuBotColor&>(value);
    wipe_native_value(typed.light_color_, observer);
    wipe_native_value(typed.dark_color_, observer);
    return;
}
case td::td_api::attributeCraftPersistenceProbability::ID: {
    auto& typed = static_cast<td::td_api::attributeCraftPersistenceProbability&>(value);
    wipe_native_value(typed.persistence_chance_per_mille_, observer);
    return;
}
case td::td_api::auctionBid::ID: {
    auto& typed = static_cast<td::td_api::auctionBid&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.bid_date_, observer);
    wipe_native_value(typed.position_, observer);
    return;
}
case td::td_api::auctionRound::ID: {
    auto& typed = static_cast<td::td_api::auctionRound&>(value);
    wipe_native_value(typed.number_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.extend_time_, observer);
    wipe_native_value(typed.top_winner_count_, observer);
    return;
}
case td::td_api::auctionStateActive::ID: {
    auto& typed = static_cast<td::td_api::auctionStateActive&>(value);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.end_date_, observer);
    wipe_native_value(typed.min_bid_, observer);
    wipe_native_value(typed.bid_levels_, observer);
    wipe_native_value(typed.top_bidder_user_ids_, observer);
    wipe_native_value(typed.rounds_, observer);
    wipe_native_value(typed.current_round_end_date_, observer);
    wipe_native_value(typed.current_round_number_, observer);
    wipe_native_value(typed.total_round_count_, observer);
    wipe_native_value(typed.distributed_item_count_, observer);
    wipe_native_value(typed.left_item_count_, observer);
    wipe_native_value(typed.acquired_item_count_, observer);
    wipe_native_value(typed.user_bid_, observer);
    return;
}
case td::td_api::auctionStateFinished::ID: {
    auto& typed = static_cast<td::td_api::auctionStateFinished&>(value);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.end_date_, observer);
    wipe_native_value(typed.average_price_, observer);
    wipe_native_value(typed.acquired_item_count_, observer);
    wipe_native_value(typed.telegram_listed_item_count_, observer);
    wipe_native_value(typed.fragment_listed_item_count_, observer);
    wipe_native_value(typed.fragment_url_, observer);
    return;
}
case td::td_api::audio::ID: {
    auto& typed = static_cast<td::td_api::audio&>(value);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.performer_, observer);
    wipe_native_value(typed.file_name_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.album_cover_minithumbnail_, observer);
    wipe_native_value(typed.album_cover_thumbnail_, observer);
    wipe_native_value(typed.external_album_covers_, observer);
    wipe_native_value(typed.audio_, observer);
    return;
}
case td::td_api::audios::ID: {
    auto& typed = static_cast<td::td_api::audios&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.audios_, observer);
    return;
}
case td::td_api::authenticationCodeInfo::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeInfo&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.next_type_, observer);
    wipe_native_value(typed.timeout_, observer);
    return;
}
case td::td_api::authenticationCodeTypeCall::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeCall&>(value);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authenticationCodeTypeFirebaseAndroid::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeFirebaseAndroid&>(value);
    wipe_native_value(typed.device_verification_parameters_, observer);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authenticationCodeTypeFirebaseIos::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeFirebaseIos&>(value);
    wipe_native_value(typed.receipt_, observer);
    wipe_native_value(typed.push_timeout_, observer);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authenticationCodeTypeFlashCall::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeFlashCall&>(value);
    wipe_native_value(typed.pattern_, observer);
    return;
}
case td::td_api::authenticationCodeTypeFragment::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeFragment&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authenticationCodeTypeMissedCall::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeMissedCall&>(value);
    wipe_native_value(typed.phone_number_prefix_, observer);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authenticationCodeTypeSms::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeSms&>(value);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authenticationCodeTypeSmsPhrase::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeSmsPhrase&>(value);
    wipe_native_value(typed.first_word_, observer);
    return;
}
case td::td_api::authenticationCodeTypeSmsWord::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeSmsWord&>(value);
    wipe_native_value(typed.first_letter_, observer);
    return;
}
case td::td_api::authenticationCodeTypeTelegramMessage::ID: {
    auto& typed = static_cast<td::td_api::authenticationCodeTypeTelegramMessage&>(value);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::authorizationStateClosed::ID: {
    return;
}
case td::td_api::authorizationStateClosing::ID: {
    return;
}
case td::td_api::authorizationStateLoggingOut::ID: {
    return;
}
case td::td_api::authorizationStateReady::ID: {
    return;
}
case td::td_api::authorizationStateWaitCode::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitCode&>(value);
    wipe_native_value(typed.code_info_, observer);
    return;
}
case td::td_api::authorizationStateWaitEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitEmailAddress&>(value);
    wipe_native_value(typed.allow_apple_id_, observer);
    wipe_native_value(typed.allow_google_id_, observer);
    return;
}
case td::td_api::authorizationStateWaitEmailCode::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitEmailCode&>(value);
    wipe_native_value(typed.allow_apple_id_, observer);
    wipe_native_value(typed.allow_google_id_, observer);
    wipe_native_value(typed.code_info_, observer);
    wipe_native_value(typed.email_address_reset_state_, observer);
    return;
}
case td::td_api::authorizationStateWaitOtherDeviceConfirmation::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitOtherDeviceConfirmation&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::authorizationStateWaitPassword::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitPassword&>(value);
    wipe_native_value(typed.password_hint_, observer);
    wipe_native_value(typed.has_recovery_email_address_, observer);
    wipe_native_value(typed.has_passport_data_, observer);
    wipe_native_value(typed.recovery_email_address_pattern_, observer);
    return;
}
case td::td_api::authorizationStateWaitPhoneNumber::ID: {
    return;
}
case td::td_api::authorizationStateWaitPremiumPurchase::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitPremiumPurchase&>(value);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.premium_day_count_, observer);
    wipe_native_value(typed.support_email_address_, observer);
    wipe_native_value(typed.support_email_subject_, observer);
    return;
}
case td::td_api::authorizationStateWaitRegistration::ID: {
    auto& typed = static_cast<td::td_api::authorizationStateWaitRegistration&>(value);
    wipe_native_value(typed.terms_of_service_, observer);
    return;
}
case td::td_api::authorizationStateWaitTdlibParameters::ID: {
    return;
}
case td::td_api::autoDownloadSettings::ID: {
    auto& typed = static_cast<td::td_api::autoDownloadSettings&>(value);
    wipe_native_value(typed.is_auto_download_enabled_, observer);
    wipe_native_value(typed.max_photo_file_size_, observer);
    wipe_native_value(typed.max_video_file_size_, observer);
    wipe_native_value(typed.max_other_file_size_, observer);
    wipe_native_value(typed.video_upload_bitrate_, observer);
    wipe_native_value(typed.preload_large_videos_, observer);
    wipe_native_value(typed.preload_next_audio_, observer);
    wipe_native_value(typed.preload_stories_, observer);
    wipe_native_value(typed.use_less_data_for_calls_, observer);
    return;
}
case td::td_api::autoDownloadSettingsPresets::ID: {
    auto& typed = static_cast<td::td_api::autoDownloadSettingsPresets&>(value);
    wipe_native_value(typed.low_, observer);
    wipe_native_value(typed.medium_, observer);
    wipe_native_value(typed.high_, observer);
    return;
}
case td::td_api::autosaveSettings::ID: {
    auto& typed = static_cast<td::td_api::autosaveSettings&>(value);
    wipe_native_value(typed.private_chat_settings_, observer);
    wipe_native_value(typed.group_settings_, observer);
    wipe_native_value(typed.channel_settings_, observer);
    wipe_native_value(typed.exceptions_, observer);
    return;
}
case td::td_api::autosaveSettingsException::ID: {
    auto& typed = static_cast<td::td_api::autosaveSettingsException&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::autosaveSettingsScopeChannelChats::ID: {
    return;
}
case td::td_api::autosaveSettingsScopeChat::ID: {
    auto& typed = static_cast<td::td_api::autosaveSettingsScopeChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::autosaveSettingsScopeGroupChats::ID: {
    return;
}
case td::td_api::autosaveSettingsScopePrivateChats::ID: {
    return;
}
case td::td_api::availableGift::ID: {
    auto& typed = static_cast<td::td_api::availableGift&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.resale_count_, observer);
    wipe_native_value(typed.min_resale_star_count_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::availableGifts::ID: {
    auto& typed = static_cast<td::td_api::availableGifts&>(value);
    wipe_native_value(typed.gifts_, observer);
    return;
}
case td::td_api::availableReaction::ID: {
    auto& typed = static_cast<td::td_api::availableReaction&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.needs_premium_, observer);
    return;
}
case td::td_api::availableReactions::ID: {
    auto& typed = static_cast<td::td_api::availableReactions&>(value);
    wipe_native_value(typed.top_reactions_, observer);
    wipe_native_value(typed.recent_reactions_, observer);
    wipe_native_value(typed.popular_reactions_, observer);
    wipe_native_value(typed.allow_custom_emoji_, observer);
    wipe_native_value(typed.are_tags_, observer);
    wipe_native_value(typed.unavailability_reason_, observer);
    return;
}
case td::td_api::background::ID: {
    auto& typed = static_cast<td::td_api::background&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.is_default_, observer);
    wipe_native_value(typed.is_dark_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::backgroundFillFreeformGradient::ID: {
    auto& typed = static_cast<td::td_api::backgroundFillFreeformGradient&>(value);
    wipe_native_value(typed.colors_, observer);
    return;
}
case td::td_api::backgroundFillGradient::ID: {
    auto& typed = static_cast<td::td_api::backgroundFillGradient&>(value);
    wipe_native_value(typed.top_color_, observer);
    wipe_native_value(typed.bottom_color_, observer);
    wipe_native_value(typed.rotation_angle_, observer);
    return;
}
case td::td_api::backgroundFillSolid::ID: {
    auto& typed = static_cast<td::td_api::backgroundFillSolid&>(value);
    wipe_native_value(typed.color_, observer);
    return;
}
case td::td_api::backgroundTypeChatTheme::ID: {
    auto& typed = static_cast<td::td_api::backgroundTypeChatTheme&>(value);
    wipe_native_value(typed.theme_name_, observer);
    return;
}
case td::td_api::backgroundTypeFill::ID: {
    auto& typed = static_cast<td::td_api::backgroundTypeFill&>(value);
    wipe_native_value(typed.fill_, observer);
    return;
}
case td::td_api::backgroundTypePattern::ID: {
    auto& typed = static_cast<td::td_api::backgroundTypePattern&>(value);
    wipe_native_value(typed.fill_, observer);
    wipe_native_value(typed.intensity_, observer);
    wipe_native_value(typed.is_inverted_, observer);
    wipe_native_value(typed.is_moving_, observer);
    return;
}
case td::td_api::backgroundTypeWallpaper::ID: {
    auto& typed = static_cast<td::td_api::backgroundTypeWallpaper&>(value);
    wipe_native_value(typed.is_blurred_, observer);
    wipe_native_value(typed.is_moving_, observer);
    return;
}
case td::td_api::backgrounds::ID: {
    auto& typed = static_cast<td::td_api::backgrounds&>(value);
    wipe_native_value(typed.backgrounds_, observer);
    return;
}
case td::td_api::bankCardActionOpenUrl::ID: {
    auto& typed = static_cast<td::td_api::bankCardActionOpenUrl&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::bankCardInfo::ID: {
    auto& typed = static_cast<td::td_api::bankCardInfo&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.actions_, observer);
    return;
}
case td::td_api::basicGroup::ID: {
    auto& typed = static_cast<td::td_api::basicGroup&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.status_, observer);
    wipe_native_value(typed.is_active_, observer);
    wipe_native_value(typed.upgraded_to_supergroup_id_, observer);
    return;
}
case td::td_api::basicGroupFullInfo::ID: {
    auto& typed = static_cast<td::td_api::basicGroupFullInfo&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.creator_user_id_, observer);
    wipe_native_value(typed.members_, observer);
    wipe_native_value(typed.can_hide_members_, observer);
    wipe_native_value(typed.can_toggle_aggressive_anti_spam_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.bot_commands_, observer);
    return;
}
case td::td_api::birthdate::ID: {
    auto& typed = static_cast<td::td_api::birthdate&>(value);
    wipe_native_value(typed.day_, observer);
    wipe_native_value(typed.month_, observer);
    wipe_native_value(typed.year_, observer);
    return;
}
case td::td_api::blockListMain::ID: {
    return;
}
case td::td_api::blockListStories::ID: {
    return;
}
case td::td_api::botAccessSettings::ID: {
    auto& typed = static_cast<td::td_api::botAccessSettings&>(value);
    wipe_native_value(typed.is_restricted_, observer);
    wipe_native_value(typed.added_user_ids_, observer);
    return;
}
case td::td_api::botCommand::ID: {
    auto& typed = static_cast<td::td_api::botCommand&>(value);
    wipe_native_value(typed.command_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::botCommandScopeAllChatAdministrators::ID: {
    return;
}
case td::td_api::botCommandScopeAllGroupChats::ID: {
    return;
}
case td::td_api::botCommandScopeAllPrivateChats::ID: {
    return;
}
case td::td_api::botCommandScopeChat::ID: {
    auto& typed = static_cast<td::td_api::botCommandScopeChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::botCommandScopeChatAdministrators::ID: {
    auto& typed = static_cast<td::td_api::botCommandScopeChatAdministrators&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::botCommandScopeChatMember::ID: {
    auto& typed = static_cast<td::td_api::botCommandScopeChatMember&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::botCommandScopeDefault::ID: {
    return;
}
case td::td_api::botCommands::ID: {
    auto& typed = static_cast<td::td_api::botCommands&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.commands_, observer);
    return;
}
case td::td_api::botInfo::ID: {
    auto& typed = static_cast<td::td_api::botInfo&>(value);
    wipe_native_value(typed.short_description_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.manager_bot_user_id_, observer);
    wipe_native_value(typed.menu_button_, observer);
    wipe_native_value(typed.commands_, observer);
    wipe_native_value(typed.privacy_policy_url_, observer);
    wipe_native_value(typed.default_group_administrator_rights_, observer);
    wipe_native_value(typed.default_channel_administrator_rights_, observer);
    wipe_native_value(typed.affiliate_program_, observer);
    wipe_native_value(typed.web_app_background_light_color_, observer);
    wipe_native_value(typed.web_app_background_dark_color_, observer);
    wipe_native_value(typed.web_app_header_light_color_, observer);
    wipe_native_value(typed.web_app_header_dark_color_, observer);
    wipe_native_value(typed.verification_parameters_, observer);
    wipe_native_value(typed.can_get_revenue_statistics_, observer);
    wipe_native_value(typed.can_manage_emoji_status_, observer);
    wipe_native_value(typed.has_media_previews_, observer);
    wipe_native_value(typed.edit_commands_link_, observer);
    wipe_native_value(typed.edit_description_link_, observer);
    wipe_native_value(typed.edit_description_media_link_, observer);
    wipe_native_value(typed.edit_settings_link_, observer);
    return;
}
case td::td_api::botMediaPreview::ID: {
    auto& typed = static_cast<td::td_api::botMediaPreview&>(value);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::botMediaPreviewInfo::ID: {
    auto& typed = static_cast<td::td_api::botMediaPreviewInfo&>(value);
    wipe_native_value(typed.previews_, observer);
    wipe_native_value(typed.language_codes_, observer);
    return;
}
case td::td_api::botMediaPreviews::ID: {
    auto& typed = static_cast<td::td_api::botMediaPreviews&>(value);
    wipe_native_value(typed.previews_, observer);
    return;
}
case td::td_api::botMenuButton::ID: {
    auto& typed = static_cast<td::td_api::botMenuButton&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::botVerification::ID: {
    auto& typed = static_cast<td::td_api::botVerification&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.icon_custom_emoji_id_, observer);
    wipe_native_value(typed.custom_description_, observer);
    return;
}
case td::td_api::botVerificationParameters::ID: {
    auto& typed = static_cast<td::td_api::botVerificationParameters&>(value);
    wipe_native_value(typed.icon_custom_emoji_id_, observer);
    wipe_native_value(typed.organization_name_, observer);
    wipe_native_value(typed.default_custom_description_, observer);
    wipe_native_value(typed.can_set_custom_description_, observer);
    return;
}
case td::td_api::botWriteAccessAllowReasonAcceptedRequest::ID: {
    return;
}
case td::td_api::botWriteAccessAllowReasonAddedToAttachmentMenu::ID: {
    return;
}
case td::td_api::botWriteAccessAllowReasonConnectedWebsite::ID: {
    auto& typed = static_cast<td::td_api::botWriteAccessAllowReasonConnectedWebsite&>(value);
    wipe_native_value(typed.domain_name_, observer);
    return;
}
case td::td_api::botWriteAccessAllowReasonLaunchedWebApp::ID: {
    auto& typed = static_cast<td::td_api::botWriteAccessAllowReasonLaunchedWebApp&>(value);
    wipe_native_value(typed.web_app_, observer);
    return;
}
case td::td_api::builtInThemeArctic::ID: {
    return;
}
case td::td_api::builtInThemeClassic::ID: {
    return;
}
case td::td_api::builtInThemeDay::ID: {
    return;
}
case td::td_api::builtInThemeNight::ID: {
    return;
}
case td::td_api::builtInThemeTinted::ID: {
    return;
}
case td::td_api::businessAwayMessageScheduleAlways::ID: {
    return;
}
case td::td_api::businessAwayMessageScheduleCustom::ID: {
    auto& typed = static_cast<td::td_api::businessAwayMessageScheduleCustom&>(value);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.end_date_, observer);
    return;
}
case td::td_api::businessAwayMessageScheduleOutsideOfOpeningHours::ID: {
    return;
}
case td::td_api::businessAwayMessageSettings::ID: {
    auto& typed = static_cast<td::td_api::businessAwayMessageSettings&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.recipients_, observer);
    wipe_native_value(typed.schedule_, observer);
    wipe_native_value(typed.offline_only_, observer);
    return;
}
case td::td_api::businessBotManageBar::ID: {
    auto& typed = static_cast<td::td_api::businessBotManageBar&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.manage_url_, observer);
    wipe_native_value(typed.is_bot_paused_, observer);
    wipe_native_value(typed.can_bot_reply_, observer);
    return;
}
case td::td_api::businessBotRights::ID: {
    auto& typed = static_cast<td::td_api::businessBotRights&>(value);
    wipe_native_value(typed.can_reply_, observer);
    wipe_native_value(typed.can_read_messages_, observer);
    wipe_native_value(typed.can_delete_sent_messages_, observer);
    wipe_native_value(typed.can_delete_all_messages_, observer);
    wipe_native_value(typed.can_edit_name_, observer);
    wipe_native_value(typed.can_edit_bio_, observer);
    wipe_native_value(typed.can_edit_profile_photo_, observer);
    wipe_native_value(typed.can_edit_username_, observer);
    wipe_native_value(typed.can_view_gifts_and_stars_, observer);
    wipe_native_value(typed.can_sell_gifts_, observer);
    wipe_native_value(typed.can_change_gift_settings_, observer);
    wipe_native_value(typed.can_transfer_and_upgrade_gifts_, observer);
    wipe_native_value(typed.can_transfer_stars_, observer);
    wipe_native_value(typed.can_manage_stories_, observer);
    return;
}
case td::td_api::businessChatLink::ID: {
    auto& typed = static_cast<td::td_api::businessChatLink&>(value);
    wipe_native_value(typed.link_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.view_count_, observer);
    return;
}
case td::td_api::businessChatLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::businessChatLinkInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::businessChatLinks::ID: {
    auto& typed = static_cast<td::td_api::businessChatLinks&>(value);
    wipe_native_value(typed.links_, observer);
    return;
}
case td::td_api::businessConnectedBot::ID: {
    auto& typed = static_cast<td::td_api::businessConnectedBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.recipients_, observer);
    wipe_native_value(typed.rights_, observer);
    return;
}
case td::td_api::businessConnectedBotInfo::ID: {
    auto& typed = static_cast<td::td_api::businessConnectedBotInfo&>(value);
    wipe_native_value(typed.bot_, observer);
    wipe_native_value(typed.connection_date_, observer);
    wipe_native_value(typed.device_model_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::businessConnection::ID: {
    auto& typed = static_cast<td::td_api::businessConnection&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.user_chat_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.rights_, observer);
    wipe_native_value(typed.is_enabled_, observer);
    return;
}
case td::td_api::businessFeatureAccountLinks::ID: {
    return;
}
case td::td_api::businessFeatureAwayMessage::ID: {
    return;
}
case td::td_api::businessFeatureBots::ID: {
    return;
}
case td::td_api::businessFeatureChatFolderTags::ID: {
    return;
}
case td::td_api::businessFeatureEmojiStatus::ID: {
    return;
}
case td::td_api::businessFeatureGreetingMessage::ID: {
    return;
}
case td::td_api::businessFeatureLocation::ID: {
    return;
}
case td::td_api::businessFeatureOpeningHours::ID: {
    return;
}
case td::td_api::businessFeaturePromotionAnimation::ID: {
    auto& typed = static_cast<td::td_api::businessFeaturePromotionAnimation&>(value);
    wipe_native_value(typed.feature_, observer);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::businessFeatureQuickReplies::ID: {
    return;
}
case td::td_api::businessFeatureStartPage::ID: {
    return;
}
case td::td_api::businessFeatureUpgradedStories::ID: {
    return;
}
case td::td_api::businessFeatures::ID: {
    auto& typed = static_cast<td::td_api::businessFeatures&>(value);
    wipe_native_value(typed.features_, observer);
    return;
}
case td::td_api::businessGreetingMessageSettings::ID: {
    auto& typed = static_cast<td::td_api::businessGreetingMessageSettings&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.recipients_, observer);
    wipe_native_value(typed.inactivity_days_, observer);
    return;
}
case td::td_api::businessInfo::ID: {
    auto& typed = static_cast<td::td_api::businessInfo&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.opening_hours_, observer);
    wipe_native_value(typed.local_opening_hours_, observer);
    wipe_native_value(typed.next_open_in_, observer);
    wipe_native_value(typed.next_close_in_, observer);
    wipe_native_value(typed.greeting_message_settings_, observer);
    wipe_native_value(typed.away_message_settings_, observer);
    wipe_native_value(typed.start_page_, observer);
    return;
}
case td::td_api::businessLocation::ID: {
    auto& typed = static_cast<td::td_api::businessLocation&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.address_, observer);
    return;
}
case td::td_api::businessMessage::ID: {
    auto& typed = static_cast<td::td_api::businessMessage&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.reply_to_message_, observer);
    return;
}
case td::td_api::businessMessages::ID: {
    auto& typed = static_cast<td::td_api::businessMessages&>(value);
    wipe_native_value(typed.messages_, observer);
    return;
}
case td::td_api::businessOpeningHours::ID: {
    auto& typed = static_cast<td::td_api::businessOpeningHours&>(value);
    wipe_native_value(typed.time_zone_id_, observer);
    wipe_native_value(typed.opening_hours_, observer);
    return;
}
case td::td_api::businessOpeningHoursInterval::ID: {
    auto& typed = static_cast<td::td_api::businessOpeningHoursInterval&>(value);
    wipe_native_value(typed.start_minute_, observer);
    wipe_native_value(typed.end_minute_, observer);
    return;
}
case td::td_api::businessRecipients::ID: {
    auto& typed = static_cast<td::td_api::businessRecipients&>(value);
    wipe_native_value(typed.chat_ids_, observer);
    wipe_native_value(typed.excluded_chat_ids_, observer);
    wipe_native_value(typed.select_existing_chats_, observer);
    wipe_native_value(typed.select_new_chats_, observer);
    wipe_native_value(typed.select_contacts_, observer);
    wipe_native_value(typed.select_non_contacts_, observer);
    wipe_native_value(typed.exclude_selected_, observer);
    return;
}
case td::td_api::businessStartPage::ID: {
    auto& typed = static_cast<td::td_api::businessStartPage&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::buttonStyleDanger::ID: {
    return;
}
case td::td_api::buttonStyleDefault::ID: {
    return;
}
case td::td_api::buttonStylePrimary::ID: {
    return;
}
case td::td_api::buttonStyleSuccess::ID: {
    return;
}
case td::td_api::call::ID: {
    auto& typed = static_cast<td::td_api::call&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.is_outgoing_, observer);
    wipe_native_value(typed.is_video_, observer);
    wipe_native_value(typed.state_, observer);
    return;
}
case td::td_api::callDiscardReasonDeclined::ID: {
    return;
}
case td::td_api::callDiscardReasonDisconnected::ID: {
    return;
}
case td::td_api::callDiscardReasonEmpty::ID: {
    return;
}
case td::td_api::callDiscardReasonHungUp::ID: {
    return;
}
case td::td_api::callDiscardReasonMissed::ID: {
    return;
}
case td::td_api::callDiscardReasonUpgradeToGroupCall::ID: {
    auto& typed = static_cast<td::td_api::callDiscardReasonUpgradeToGroupCall&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::callId::ID: {
    auto& typed = static_cast<td::td_api::callId&>(value);
    wipe_native_value(typed.id_, observer);
    return;
}
case td::td_api::callProblemDistortedSpeech::ID: {
    return;
}
case td::td_api::callProblemDistortedVideo::ID: {
    return;
}
case td::td_api::callProblemDropped::ID: {
    return;
}
case td::td_api::callProblemEcho::ID: {
    return;
}
case td::td_api::callProblemInterruptions::ID: {
    return;
}
case td::td_api::callProblemNoise::ID: {
    return;
}
case td::td_api::callProblemPixelatedVideo::ID: {
    return;
}
case td::td_api::callProblemSilentLocal::ID: {
    return;
}
case td::td_api::callProblemSilentRemote::ID: {
    return;
}
case td::td_api::callProtocol::ID: {
    auto& typed = static_cast<td::td_api::callProtocol&>(value);
    wipe_native_value(typed.udp_p2p_, observer);
    wipe_native_value(typed.udp_reflector_, observer);
    wipe_native_value(typed.min_layer_, observer);
    wipe_native_value(typed.max_layer_, observer);
    wipe_native_value(typed.library_versions_, observer);
    return;
}
case td::td_api::callServer::ID: {
    auto& typed = static_cast<td::td_api::callServer&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.ip_address_, observer);
    wipe_native_value(typed.ipv6_address_, observer);
    wipe_native_value(typed.port_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::callServerTypeTelegramReflector::ID: {
    auto& typed = static_cast<td::td_api::callServerTypeTelegramReflector&>(value);
    wipe_native_value(typed.peer_tag_, observer);
    wipe_native_value(typed.is_tcp_, observer);
    return;
}
case td::td_api::callServerTypeWebrtc::ID: {
    auto& typed = static_cast<td::td_api::callServerTypeWebrtc&>(value);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.password_, observer);
    wipe_native_value(typed.supports_turn_, observer);
    wipe_native_value(typed.supports_stun_, observer);
    return;
}
case td::td_api::callStateDiscarded::ID: {
    auto& typed = static_cast<td::td_api::callStateDiscarded&>(value);
    wipe_native_value(typed.reason_, observer);
    wipe_native_value(typed.need_rating_, observer);
    wipe_native_value(typed.need_debug_information_, observer);
    wipe_native_value(typed.need_log_, observer);
    return;
}
case td::td_api::callStateError::ID: {
    auto& typed = static_cast<td::td_api::callStateError&>(value);
    wipe_native_value(typed.error_, observer);
    return;
}
case td::td_api::callStateExchangingKeys::ID: {
    return;
}
case td::td_api::callStateHangingUp::ID: {
    return;
}
case td::td_api::callStatePending::ID: {
    auto& typed = static_cast<td::td_api::callStatePending&>(value);
    wipe_native_value(typed.is_created_, observer);
    wipe_native_value(typed.is_received_, observer);
    return;
}
case td::td_api::callStateReady::ID: {
    auto& typed = static_cast<td::td_api::callStateReady&>(value);
    wipe_native_value(typed.protocol_, observer);
    wipe_native_value(typed.servers_, observer);
    wipe_native_value(typed.config_, observer);
    wipe_native_value(typed.encryption_key_, observer);
    wipe_native_value(typed.emojis_, observer);
    wipe_native_value(typed.allow_p2p_, observer);
    wipe_native_value(typed.is_group_call_supported_, observer);
    wipe_native_value(typed.custom_parameters_, observer);
    return;
}
case td::td_api::callbackQueryAnswer::ID: {
    auto& typed = static_cast<td::td_api::callbackQueryAnswer&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.show_alert_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::callbackQueryPayloadData::ID: {
    auto& typed = static_cast<td::td_api::callbackQueryPayloadData&>(value);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::callbackQueryPayloadDataWithPassword::ID: {
    auto& typed = static_cast<td::td_api::callbackQueryPayloadDataWithPassword&>(value);
    wipe_native_value(typed.password_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::callbackQueryPayloadGame::ID: {
    auto& typed = static_cast<td::td_api::callbackQueryPayloadGame&>(value);
    wipe_native_value(typed.game_short_name_, observer);
    return;
}
case td::td_api::canPostStoryResultActiveStoryLimitExceeded::ID: {
    return;
}
case td::td_api::canPostStoryResultBoostNeeded::ID: {
    return;
}
case td::td_api::canPostStoryResultLiveStoryIsActive::ID: {
    auto& typed = static_cast<td::td_api::canPostStoryResultLiveStoryIsActive&>(value);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::canPostStoryResultMonthlyLimitExceeded::ID: {
    auto& typed = static_cast<td::td_api::canPostStoryResultMonthlyLimitExceeded&>(value);
    wipe_native_value(typed.retry_after_, observer);
    return;
}
case td::td_api::canPostStoryResultOk::ID: {
    auto& typed = static_cast<td::td_api::canPostStoryResultOk&>(value);
    wipe_native_value(typed.story_count_, observer);
    return;
}
case td::td_api::canPostStoryResultPremiumNeeded::ID: {
    return;
}
case td::td_api::canPostStoryResultWeeklyLimitExceeded::ID: {
    auto& typed = static_cast<td::td_api::canPostStoryResultWeeklyLimitExceeded&>(value);
    wipe_native_value(typed.retry_after_, observer);
    return;
}
case td::td_api::canSendGiftResultFail::ID: {
    auto& typed = static_cast<td::td_api::canSendGiftResultFail&>(value);
    wipe_native_value(typed.reason_, observer);
    return;
}
case td::td_api::canSendGiftResultOk::ID: {
    return;
}
case td::td_api::canSendMessageToUserResultOk::ID: {
    return;
}
case td::td_api::canSendMessageToUserResultUserHasPaidMessages::ID: {
    auto& typed = static_cast<td::td_api::canSendMessageToUserResultUserHasPaidMessages&>(value);
    wipe_native_value(typed.outgoing_paid_message_star_count_, observer);
    return;
}
case td::td_api::canSendMessageToUserResultUserIsDeleted::ID: {
    return;
}
case td::td_api::canSendMessageToUserResultUserRestrictsNewChats::ID: {
    return;
}
case td::td_api::canTransferOwnershipResultOk::ID: {
    return;
}
case td::td_api::canTransferOwnershipResultPasswordNeeded::ID: {
    return;
}
case td::td_api::canTransferOwnershipResultPasswordTooFresh::ID: {
    auto& typed = static_cast<td::td_api::canTransferOwnershipResultPasswordTooFresh&>(value);
    wipe_native_value(typed.retry_after_, observer);
    return;
}
case td::td_api::canTransferOwnershipResultSessionTooFresh::ID: {
    auto& typed = static_cast<td::td_api::canTransferOwnershipResultSessionTooFresh&>(value);
    wipe_native_value(typed.retry_after_, observer);
    return;
}
case td::td_api::chat::ID: {
    auto& typed = static_cast<td::td_api::chat&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.background_custom_emoji_id_, observer);
    wipe_native_value(typed.upgraded_gift_colors_, observer);
    wipe_native_value(typed.profile_accent_color_id_, observer);
    wipe_native_value(typed.profile_background_custom_emoji_id_, observer);
    wipe_native_value(typed.permissions_, observer);
    wipe_native_value(typed.last_message_, observer);
    wipe_native_value(typed.positions_, observer);
    wipe_native_value(typed.chat_lists_, observer);
    wipe_native_value(typed.message_sender_id_, observer);
    wipe_native_value(typed.block_list_, observer);
    wipe_native_value(typed.has_protected_content_, observer);
    wipe_native_value(typed.is_translatable_, observer);
    wipe_native_value(typed.is_marked_as_unread_, observer);
    wipe_native_value(typed.view_as_topics_, observer);
    wipe_native_value(typed.has_scheduled_messages_, observer);
    wipe_native_value(typed.can_be_deleted_only_for_self_, observer);
    wipe_native_value(typed.can_be_deleted_for_all_users_, observer);
    wipe_native_value(typed.can_be_reported_, observer);
    wipe_native_value(typed.default_disable_notification_, observer);
    wipe_native_value(typed.unread_count_, observer);
    wipe_native_value(typed.last_read_inbox_message_id_, observer);
    wipe_native_value(typed.last_read_outbox_message_id_, observer);
    wipe_native_value(typed.unread_mention_count_, observer);
    wipe_native_value(typed.unread_reaction_count_, observer);
    wipe_native_value(typed.unread_poll_vote_count_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    wipe_native_value(typed.available_reactions_, observer);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    wipe_native_value(typed.emoji_status_, observer);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.theme_, observer);
    wipe_native_value(typed.action_bar_, observer);
    wipe_native_value(typed.business_bot_manage_bar_, observer);
    wipe_native_value(typed.video_chat_, observer);
    wipe_native_value(typed.pending_join_requests_, observer);
    wipe_native_value(typed.reply_markup_message_id_, observer);
    wipe_native_value(typed.draft_message_, observer);
    wipe_native_value(typed.client_data_, observer);
    return;
}
case td::td_api::chatActionBarAddContact::ID: {
    return;
}
case td::td_api::chatActionBarInviteMembers::ID: {
    return;
}
case td::td_api::chatActionBarJoinRequest::ID: {
    auto& typed = static_cast<td::td_api::chatActionBarJoinRequest&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.is_channel_, observer);
    wipe_native_value(typed.request_date_, observer);
    return;
}
case td::td_api::chatActionBarReportAddBlock::ID: {
    auto& typed = static_cast<td::td_api::chatActionBarReportAddBlock&>(value);
    wipe_native_value(typed.can_unarchive_, observer);
    wipe_native_value(typed.account_info_, observer);
    return;
}
case td::td_api::chatActionBarReportSpam::ID: {
    auto& typed = static_cast<td::td_api::chatActionBarReportSpam&>(value);
    wipe_native_value(typed.can_unarchive_, observer);
    return;
}
case td::td_api::chatActionBarSharePhoneNumber::ID: {
    return;
}
case td::td_api::chatActionCancel::ID: {
    return;
}
case td::td_api::chatActionChoosingContact::ID: {
    return;
}
case td::td_api::chatActionChoosingLocation::ID: {
    return;
}
case td::td_api::chatActionChoosingSticker::ID: {
    return;
}
case td::td_api::chatActionRecordingVideo::ID: {
    return;
}
case td::td_api::chatActionRecordingVideoNote::ID: {
    return;
}
case td::td_api::chatActionRecordingVoiceNote::ID: {
    return;
}
case td::td_api::chatActionStartPlayingGame::ID: {
    return;
}
case td::td_api::chatActionTyping::ID: {
    return;
}
case td::td_api::chatActionUploadingDocument::ID: {
    auto& typed = static_cast<td::td_api::chatActionUploadingDocument&>(value);
    wipe_native_value(typed.progress_, observer);
    return;
}
case td::td_api::chatActionUploadingPhoto::ID: {
    auto& typed = static_cast<td::td_api::chatActionUploadingPhoto&>(value);
    wipe_native_value(typed.progress_, observer);
    return;
}
case td::td_api::chatActionUploadingVideo::ID: {
    auto& typed = static_cast<td::td_api::chatActionUploadingVideo&>(value);
    wipe_native_value(typed.progress_, observer);
    return;
}
case td::td_api::chatActionUploadingVideoNote::ID: {
    auto& typed = static_cast<td::td_api::chatActionUploadingVideoNote&>(value);
    wipe_native_value(typed.progress_, observer);
    return;
}
case td::td_api::chatActionUploadingVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::chatActionUploadingVoiceNote&>(value);
    wipe_native_value(typed.progress_, observer);
    return;
}
case td::td_api::chatActionWatchingAnimations::ID: {
    auto& typed = static_cast<td::td_api::chatActionWatchingAnimations&>(value);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::chatActiveStories::ID: {
    auto& typed = static_cast<td::td_api::chatActiveStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.list_, observer);
    wipe_native_value(typed.order_, observer);
    wipe_native_value(typed.can_be_archived_, observer);
    wipe_native_value(typed.max_read_story_id_, observer);
    wipe_native_value(typed.stories_, observer);
    return;
}
case td::td_api::chatAdministrator::ID: {
    auto& typed = static_cast<td::td_api::chatAdministrator&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.custom_title_, observer);
    wipe_native_value(typed.is_owner_, observer);
    wipe_native_value(typed.can_be_edited_, observer);
    return;
}
case td::td_api::chatAdministratorRights::ID: {
    auto& typed = static_cast<td::td_api::chatAdministratorRights&>(value);
    wipe_native_value(typed.can_manage_chat_, observer);
    wipe_native_value(typed.can_change_info_, observer);
    wipe_native_value(typed.can_post_messages_, observer);
    wipe_native_value(typed.can_edit_messages_, observer);
    wipe_native_value(typed.can_delete_messages_, observer);
    wipe_native_value(typed.can_invite_users_, observer);
    wipe_native_value(typed.can_restrict_members_, observer);
    wipe_native_value(typed.can_pin_messages_, observer);
    wipe_native_value(typed.can_manage_topics_, observer);
    wipe_native_value(typed.can_promote_members_, observer);
    wipe_native_value(typed.can_manage_video_chats_, observer);
    wipe_native_value(typed.can_post_stories_, observer);
    wipe_native_value(typed.can_edit_stories_, observer);
    wipe_native_value(typed.can_delete_stories_, observer);
    wipe_native_value(typed.can_manage_direct_messages_, observer);
    wipe_native_value(typed.can_manage_tags_, observer);
    wipe_native_value(typed.is_anonymous_, observer);
    return;
}
case td::td_api::chatAdministrators::ID: {
    auto& typed = static_cast<td::td_api::chatAdministrators&>(value);
    wipe_native_value(typed.administrators_, observer);
    return;
}
case td::td_api::chatAvailableReactionsAll::ID: {
    auto& typed = static_cast<td::td_api::chatAvailableReactionsAll&>(value);
    wipe_native_value(typed.max_reaction_count_, observer);
    return;
}
case td::td_api::chatAvailableReactionsSome::ID: {
    auto& typed = static_cast<td::td_api::chatAvailableReactionsSome&>(value);
    wipe_native_value(typed.reactions_, observer);
    wipe_native_value(typed.max_reaction_count_, observer);
    return;
}
case td::td_api::chatBackground::ID: {
    auto& typed = static_cast<td::td_api::chatBackground&>(value);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.dark_theme_dimming_, observer);
    return;
}
case td::td_api::chatBoost::ID: {
    auto& typed = static_cast<td::td_api::chatBoost&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.count_, observer);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    return;
}
case td::td_api::chatBoostFeatures::ID: {
    auto& typed = static_cast<td::td_api::chatBoostFeatures&>(value);
    wipe_native_value(typed.features_, observer);
    wipe_native_value(typed.min_profile_background_custom_emoji_boost_level_, observer);
    wipe_native_value(typed.min_background_custom_emoji_boost_level_, observer);
    wipe_native_value(typed.min_emoji_status_boost_level_, observer);
    wipe_native_value(typed.min_chat_theme_background_boost_level_, observer);
    wipe_native_value(typed.min_custom_background_boost_level_, observer);
    wipe_native_value(typed.min_custom_emoji_sticker_set_boost_level_, observer);
    wipe_native_value(typed.min_automatic_translation_boost_level_, observer);
    wipe_native_value(typed.min_speech_recognition_boost_level_, observer);
    wipe_native_value(typed.min_sponsored_message_disable_boost_level_, observer);
    return;
}
case td::td_api::chatBoostLevelFeatures::ID: {
    auto& typed = static_cast<td::td_api::chatBoostLevelFeatures&>(value);
    wipe_native_value(typed.level_, observer);
    wipe_native_value(typed.story_per_day_count_, observer);
    wipe_native_value(typed.custom_emoji_reaction_count_, observer);
    wipe_native_value(typed.title_color_count_, observer);
    wipe_native_value(typed.profile_accent_color_count_, observer);
    wipe_native_value(typed.can_set_profile_background_custom_emoji_, observer);
    wipe_native_value(typed.accent_color_count_, observer);
    wipe_native_value(typed.can_set_background_custom_emoji_, observer);
    wipe_native_value(typed.can_set_emoji_status_, observer);
    wipe_native_value(typed.chat_theme_background_count_, observer);
    wipe_native_value(typed.can_set_custom_background_, observer);
    wipe_native_value(typed.can_set_custom_emoji_sticker_set_, observer);
    wipe_native_value(typed.can_enable_automatic_translation_, observer);
    wipe_native_value(typed.can_recognize_speech_, observer);
    wipe_native_value(typed.can_disable_sponsored_messages_, observer);
    return;
}
case td::td_api::chatBoostLink::ID: {
    auto& typed = static_cast<td::td_api::chatBoostLink&>(value);
    wipe_native_value(typed.link_, observer);
    wipe_native_value(typed.is_public_, observer);
    return;
}
case td::td_api::chatBoostLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::chatBoostLinkInfo&>(value);
    wipe_native_value(typed.is_public_, observer);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::chatBoostSlot::ID: {
    auto& typed = static_cast<td::td_api::chatBoostSlot&>(value);
    wipe_native_value(typed.slot_id_, observer);
    wipe_native_value(typed.currently_boosted_chat_id_, observer);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.cooldown_until_date_, observer);
    return;
}
case td::td_api::chatBoostSlots::ID: {
    auto& typed = static_cast<td::td_api::chatBoostSlots&>(value);
    wipe_native_value(typed.slots_, observer);
    return;
}
case td::td_api::chatBoostSourceGiftCode::ID: {
    auto& typed = static_cast<td::td_api::chatBoostSourceGiftCode&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_code_, observer);
    return;
}
case td::td_api::chatBoostSourceGiveaway::ID: {
    auto& typed = static_cast<td::td_api::chatBoostSourceGiveaway&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_code_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.giveaway_message_id_, observer);
    wipe_native_value(typed.is_unclaimed_, observer);
    return;
}
case td::td_api::chatBoostSourcePremium::ID: {
    auto& typed = static_cast<td::td_api::chatBoostSourcePremium&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::chatBoostStatus::ID: {
    auto& typed = static_cast<td::td_api::chatBoostStatus&>(value);
    wipe_native_value(typed.boost_url_, observer);
    wipe_native_value(typed.applied_slot_ids_, observer);
    wipe_native_value(typed.level_, observer);
    wipe_native_value(typed.gift_code_boost_count_, observer);
    wipe_native_value(typed.boost_count_, observer);
    wipe_native_value(typed.current_level_boost_count_, observer);
    wipe_native_value(typed.next_level_boost_count_, observer);
    wipe_native_value(typed.premium_member_count_, observer);
    wipe_native_value(typed.premium_member_percentage_, observer);
    wipe_native_value(typed.prepaid_giveaways_, observer);
    return;
}
case td::td_api::chatEvent::ID: {
    auto& typed = static_cast<td::td_api::chatEvent&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.member_id_, observer);
    wipe_native_value(typed.action_, observer);
    return;
}
case td::td_api::chatEventAccentColorChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventAccentColorChanged&>(value);
    wipe_native_value(typed.old_accent_color_id_, observer);
    wipe_native_value(typed.old_background_custom_emoji_id_, observer);
    wipe_native_value(typed.new_accent_color_id_, observer);
    wipe_native_value(typed.new_background_custom_emoji_id_, observer);
    return;
}
case td::td_api::chatEventActiveUsernamesChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventActiveUsernamesChanged&>(value);
    wipe_native_value(typed.old_usernames_, observer);
    wipe_native_value(typed.new_usernames_, observer);
    return;
}
case td::td_api::chatEventAutomaticTranslationToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventAutomaticTranslationToggled&>(value);
    wipe_native_value(typed.has_automatic_translation_, observer);
    return;
}
case td::td_api::chatEventAvailableReactionsChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventAvailableReactionsChanged&>(value);
    wipe_native_value(typed.old_available_reactions_, observer);
    wipe_native_value(typed.new_available_reactions_, observer);
    return;
}
case td::td_api::chatEventBackgroundChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventBackgroundChanged&>(value);
    wipe_native_value(typed.old_background_, observer);
    wipe_native_value(typed.new_background_, observer);
    return;
}
case td::td_api::chatEventCustomEmojiStickerSetChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventCustomEmojiStickerSetChanged&>(value);
    wipe_native_value(typed.old_sticker_set_id_, observer);
    wipe_native_value(typed.new_sticker_set_id_, observer);
    return;
}
case td::td_api::chatEventDescriptionChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventDescriptionChanged&>(value);
    wipe_native_value(typed.old_description_, observer);
    wipe_native_value(typed.new_description_, observer);
    return;
}
case td::td_api::chatEventEmojiStatusChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventEmojiStatusChanged&>(value);
    wipe_native_value(typed.old_emoji_status_, observer);
    wipe_native_value(typed.new_emoji_status_, observer);
    return;
}
case td::td_api::chatEventForumTopicCreated::ID: {
    auto& typed = static_cast<td::td_api::chatEventForumTopicCreated&>(value);
    wipe_native_value(typed.topic_info_, observer);
    return;
}
case td::td_api::chatEventForumTopicDeleted::ID: {
    auto& typed = static_cast<td::td_api::chatEventForumTopicDeleted&>(value);
    wipe_native_value(typed.topic_info_, observer);
    return;
}
case td::td_api::chatEventForumTopicEdited::ID: {
    auto& typed = static_cast<td::td_api::chatEventForumTopicEdited&>(value);
    wipe_native_value(typed.old_topic_info_, observer);
    wipe_native_value(typed.new_topic_info_, observer);
    return;
}
case td::td_api::chatEventForumTopicPinned::ID: {
    auto& typed = static_cast<td::td_api::chatEventForumTopicPinned&>(value);
    wipe_native_value(typed.old_topic_info_, observer);
    wipe_native_value(typed.new_topic_info_, observer);
    return;
}
case td::td_api::chatEventForumTopicToggleIsClosed::ID: {
    auto& typed = static_cast<td::td_api::chatEventForumTopicToggleIsClosed&>(value);
    wipe_native_value(typed.topic_info_, observer);
    return;
}
case td::td_api::chatEventForumTopicToggleIsHidden::ID: {
    auto& typed = static_cast<td::td_api::chatEventForumTopicToggleIsHidden&>(value);
    wipe_native_value(typed.topic_info_, observer);
    return;
}
case td::td_api::chatEventHasAggressiveAntiSpamEnabledToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventHasAggressiveAntiSpamEnabledToggled&>(value);
    wipe_native_value(typed.has_aggressive_anti_spam_enabled_, observer);
    return;
}
case td::td_api::chatEventHasProtectedContentToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventHasProtectedContentToggled&>(value);
    wipe_native_value(typed.has_protected_content_, observer);
    return;
}
case td::td_api::chatEventInviteLinkDeleted::ID: {
    auto& typed = static_cast<td::td_api::chatEventInviteLinkDeleted&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::chatEventInviteLinkEdited::ID: {
    auto& typed = static_cast<td::td_api::chatEventInviteLinkEdited&>(value);
    wipe_native_value(typed.old_invite_link_, observer);
    wipe_native_value(typed.new_invite_link_, observer);
    return;
}
case td::td_api::chatEventInviteLinkRevoked::ID: {
    auto& typed = static_cast<td::td_api::chatEventInviteLinkRevoked&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::chatEventInvitesToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventInvitesToggled&>(value);
    wipe_native_value(typed.can_invite_users_, observer);
    return;
}
case td::td_api::chatEventIsAllHistoryAvailableToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventIsAllHistoryAvailableToggled&>(value);
    wipe_native_value(typed.is_all_history_available_, observer);
    return;
}
case td::td_api::chatEventIsForumToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventIsForumToggled&>(value);
    wipe_native_value(typed.is_forum_, observer);
    return;
}
case td::td_api::chatEventLinkedChatChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventLinkedChatChanged&>(value);
    wipe_native_value(typed.old_linked_chat_id_, observer);
    wipe_native_value(typed.new_linked_chat_id_, observer);
    return;
}
case td::td_api::chatEventLocationChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventLocationChanged&>(value);
    wipe_native_value(typed.old_location_, observer);
    wipe_native_value(typed.new_location_, observer);
    return;
}
case td::td_api::chatEventLogFilters::ID: {
    auto& typed = static_cast<td::td_api::chatEventLogFilters&>(value);
    wipe_native_value(typed.message_edits_, observer);
    wipe_native_value(typed.message_deletions_, observer);
    wipe_native_value(typed.message_pins_, observer);
    wipe_native_value(typed.member_joins_, observer);
    wipe_native_value(typed.member_leaves_, observer);
    wipe_native_value(typed.member_invites_, observer);
    wipe_native_value(typed.member_promotions_, observer);
    wipe_native_value(typed.member_restrictions_, observer);
    wipe_native_value(typed.member_tag_changes_, observer);
    wipe_native_value(typed.info_changes_, observer);
    wipe_native_value(typed.setting_changes_, observer);
    wipe_native_value(typed.invite_link_changes_, observer);
    wipe_native_value(typed.video_chat_changes_, observer);
    wipe_native_value(typed.forum_changes_, observer);
    wipe_native_value(typed.subscription_extensions_, observer);
    return;
}
case td::td_api::chatEventMemberInvited::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberInvited&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.status_, observer);
    return;
}
case td::td_api::chatEventMemberJoined::ID: {
    return;
}
case td::td_api::chatEventMemberJoinedByInviteLink::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberJoinedByInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.via_chat_folder_invite_link_, observer);
    return;
}
case td::td_api::chatEventMemberJoinedByRequest::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberJoinedByRequest&>(value);
    wipe_native_value(typed.approver_user_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::chatEventMemberLeft::ID: {
    return;
}
case td::td_api::chatEventMemberPromoted::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberPromoted&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.old_status_, observer);
    wipe_native_value(typed.new_status_, observer);
    return;
}
case td::td_api::chatEventMemberRestricted::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberRestricted&>(value);
    wipe_native_value(typed.member_id_, observer);
    wipe_native_value(typed.old_status_, observer);
    wipe_native_value(typed.new_status_, observer);
    return;
}
case td::td_api::chatEventMemberSubscriptionExtended::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberSubscriptionExtended&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.old_status_, observer);
    wipe_native_value(typed.new_status_, observer);
    return;
}
case td::td_api::chatEventMemberTagChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventMemberTagChanged&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.old_tag_, observer);
    wipe_native_value(typed.new_tag_, observer);
    return;
}
case td::td_api::chatEventMessageAutoDeleteTimeChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventMessageAutoDeleteTimeChanged&>(value);
    wipe_native_value(typed.old_message_auto_delete_time_, observer);
    wipe_native_value(typed.new_message_auto_delete_time_, observer);
    return;
}
case td::td_api::chatEventMessageDeleted::ID: {
    auto& typed = static_cast<td::td_api::chatEventMessageDeleted&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.can_report_anti_spam_false_positive_, observer);
    return;
}
case td::td_api::chatEventMessageEdited::ID: {
    auto& typed = static_cast<td::td_api::chatEventMessageEdited&>(value);
    wipe_native_value(typed.old_message_, observer);
    wipe_native_value(typed.new_message_, observer);
    return;
}
case td::td_api::chatEventMessagePinned::ID: {
    auto& typed = static_cast<td::td_api::chatEventMessagePinned&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::chatEventMessageUnpinned::ID: {
    auto& typed = static_cast<td::td_api::chatEventMessageUnpinned&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::chatEventPermissionsChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventPermissionsChanged&>(value);
    wipe_native_value(typed.old_permissions_, observer);
    wipe_native_value(typed.new_permissions_, observer);
    return;
}
case td::td_api::chatEventPhotoChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventPhotoChanged&>(value);
    wipe_native_value(typed.old_photo_, observer);
    wipe_native_value(typed.new_photo_, observer);
    return;
}
case td::td_api::chatEventPollStopped::ID: {
    auto& typed = static_cast<td::td_api::chatEventPollStopped&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::chatEventProfileAccentColorChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventProfileAccentColorChanged&>(value);
    wipe_native_value(typed.old_profile_accent_color_id_, observer);
    wipe_native_value(typed.old_profile_background_custom_emoji_id_, observer);
    wipe_native_value(typed.new_profile_accent_color_id_, observer);
    wipe_native_value(typed.new_profile_background_custom_emoji_id_, observer);
    return;
}
case td::td_api::chatEventShowMessageSenderToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventShowMessageSenderToggled&>(value);
    wipe_native_value(typed.show_message_sender_, observer);
    return;
}
case td::td_api::chatEventSignMessagesToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventSignMessagesToggled&>(value);
    wipe_native_value(typed.sign_messages_, observer);
    return;
}
case td::td_api::chatEventSlowModeDelayChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventSlowModeDelayChanged&>(value);
    wipe_native_value(typed.old_slow_mode_delay_, observer);
    wipe_native_value(typed.new_slow_mode_delay_, observer);
    return;
}
case td::td_api::chatEventStickerSetChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventStickerSetChanged&>(value);
    wipe_native_value(typed.old_sticker_set_id_, observer);
    wipe_native_value(typed.new_sticker_set_id_, observer);
    return;
}
case td::td_api::chatEventTitleChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventTitleChanged&>(value);
    wipe_native_value(typed.old_title_, observer);
    wipe_native_value(typed.new_title_, observer);
    return;
}
case td::td_api::chatEventUsernameChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventUsernameChanged&>(value);
    wipe_native_value(typed.old_username_, observer);
    wipe_native_value(typed.new_username_, observer);
    return;
}
case td::td_api::chatEventVideoChatCreated::ID: {
    auto& typed = static_cast<td::td_api::chatEventVideoChatCreated&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::chatEventVideoChatEnded::ID: {
    auto& typed = static_cast<td::td_api::chatEventVideoChatEnded&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::chatEventVideoChatMuteNewParticipantsToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventVideoChatMuteNewParticipantsToggled&>(value);
    wipe_native_value(typed.mute_new_participants_, observer);
    return;
}
case td::td_api::chatEventVideoChatParticipantIsMutedToggled::ID: {
    auto& typed = static_cast<td::td_api::chatEventVideoChatParticipantIsMutedToggled&>(value);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.is_muted_, observer);
    return;
}
case td::td_api::chatEventVideoChatParticipantVolumeLevelChanged::ID: {
    auto& typed = static_cast<td::td_api::chatEventVideoChatParticipantVolumeLevelChanged&>(value);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.volume_level_, observer);
    return;
}
case td::td_api::chatEvents::ID: {
    auto& typed = static_cast<td::td_api::chatEvents&>(value);
    wipe_native_value(typed.events_, observer);
    return;
}
case td::td_api::chatFolder::ID: {
    auto& typed = static_cast<td::td_api::chatFolder&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.icon_, observer);
    wipe_native_value(typed.color_id_, observer);
    wipe_native_value(typed.is_shareable_, observer);
    wipe_native_value(typed.pinned_chat_ids_, observer);
    wipe_native_value(typed.included_chat_ids_, observer);
    wipe_native_value(typed.excluded_chat_ids_, observer);
    wipe_native_value(typed.exclude_muted_, observer);
    wipe_native_value(typed.exclude_read_, observer);
    wipe_native_value(typed.exclude_archived_, observer);
    wipe_native_value(typed.include_contacts_, observer);
    wipe_native_value(typed.include_non_contacts_, observer);
    wipe_native_value(typed.include_bots_, observer);
    wipe_native_value(typed.include_groups_, observer);
    wipe_native_value(typed.include_channels_, observer);
    return;
}
case td::td_api::chatFolderIcon::ID: {
    auto& typed = static_cast<td::td_api::chatFolderIcon&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::chatFolderInfo::ID: {
    auto& typed = static_cast<td::td_api::chatFolderInfo&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.icon_, observer);
    wipe_native_value(typed.color_id_, observer);
    wipe_native_value(typed.is_shareable_, observer);
    wipe_native_value(typed.has_my_invite_links_, observer);
    return;
}
case td::td_api::chatFolderInviteLink::ID: {
    auto& typed = static_cast<td::td_api::chatFolderInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::chatFolderInviteLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::chatFolderInviteLinkInfo&>(value);
    wipe_native_value(typed.chat_folder_info_, observer);
    wipe_native_value(typed.missing_chat_ids_, observer);
    wipe_native_value(typed.added_chat_ids_, observer);
    return;
}
case td::td_api::chatFolderInviteLinks::ID: {
    auto& typed = static_cast<td::td_api::chatFolderInviteLinks&>(value);
    wipe_native_value(typed.invite_links_, observer);
    return;
}
case td::td_api::chatFolderName::ID: {
    auto& typed = static_cast<td::td_api::chatFolderName&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.animate_custom_emoji_, observer);
    return;
}
case td::td_api::chatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.creator_user_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.edit_date_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.subscription_pricing_, observer);
    wipe_native_value(typed.member_limit_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.expired_member_count_, observer);
    wipe_native_value(typed.pending_join_request_count_, observer);
    wipe_native_value(typed.creates_join_request_, observer);
    wipe_native_value(typed.is_primary_, observer);
    wipe_native_value(typed.is_revoked_, observer);
    return;
}
case td::td_api::chatInviteLinkCount::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinkCount&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.invite_link_count_, observer);
    wipe_native_value(typed.revoked_invite_link_count_, observer);
    return;
}
case td::td_api::chatInviteLinkCounts::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinkCounts&>(value);
    wipe_native_value(typed.invite_link_counts_, observer);
    return;
}
case td::td_api::chatInviteLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinkInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.accessible_for_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.member_user_ids_, observer);
    wipe_native_value(typed.subscription_info_, observer);
    wipe_native_value(typed.creates_join_request_, observer);
    wipe_native_value(typed.is_public_, observer);
    wipe_native_value(typed.verification_status_, observer);
    return;
}
case td::td_api::chatInviteLinkMember::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinkMember&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.joined_chat_date_, observer);
    wipe_native_value(typed.via_chat_folder_invite_link_, observer);
    wipe_native_value(typed.approver_user_id_, observer);
    return;
}
case td::td_api::chatInviteLinkMembers::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinkMembers&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.members_, observer);
    return;
}
case td::td_api::chatInviteLinkSubscriptionInfo::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinkSubscriptionInfo&>(value);
    wipe_native_value(typed.pricing_, observer);
    wipe_native_value(typed.can_reuse_, observer);
    wipe_native_value(typed.form_id_, observer);
    return;
}
case td::td_api::chatInviteLinks::ID: {
    auto& typed = static_cast<td::td_api::chatInviteLinks&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.invite_links_, observer);
    return;
}
case td::td_api::chatJoinRequest::ID: {
    auto& typed = static_cast<td::td_api::chatJoinRequest&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.bio_, observer);
    return;
}
case td::td_api::chatJoinRequestResultApproved::ID: {
    return;
}
case td::td_api::chatJoinRequestResultDeclined::ID: {
    return;
}
case td::td_api::chatJoinRequestResultQueued::ID: {
    return;
}
case td::td_api::chatJoinRequests::ID: {
    auto& typed = static_cast<td::td_api::chatJoinRequests&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.requests_, observer);
    return;
}
case td::td_api::chatJoinRequestsInfo::ID: {
    auto& typed = static_cast<td::td_api::chatJoinRequestsInfo&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::chatJoinResultDeclined::ID: {
    return;
}
case td::td_api::chatJoinResultGuardBotApprovalRequired::ID: {
    auto& typed = static_cast<td::td_api::chatJoinResultGuardBotApprovalRequired&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.query_id_, observer);
    return;
}
case td::td_api::chatJoinResultRequestSent::ID: {
    return;
}
case td::td_api::chatJoinResultSuccess::ID: {
    auto& typed = static_cast<td::td_api::chatJoinResultSuccess&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::chatListArchive::ID: {
    return;
}
case td::td_api::chatListFolder::ID: {
    auto& typed = static_cast<td::td_api::chatListFolder&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    return;
}
case td::td_api::chatListMain::ID: {
    return;
}
case td::td_api::chatLists::ID: {
    auto& typed = static_cast<td::td_api::chatLists&>(value);
    wipe_native_value(typed.chat_lists_, observer);
    return;
}
case td::td_api::chatLocation::ID: {
    auto& typed = static_cast<td::td_api::chatLocation&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.address_, observer);
    return;
}
case td::td_api::chatMember::ID: {
    auto& typed = static_cast<td::td_api::chatMember&>(value);
    wipe_native_value(typed.member_id_, observer);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.inviter_user_id_, observer);
    wipe_native_value(typed.joined_chat_date_, observer);
    wipe_native_value(typed.status_, observer);
    return;
}
case td::td_api::chatMemberStatusAdministrator::ID: {
    auto& typed = static_cast<td::td_api::chatMemberStatusAdministrator&>(value);
    wipe_native_value(typed.can_be_edited_, observer);
    wipe_native_value(typed.rights_, observer);
    return;
}
case td::td_api::chatMemberStatusBanned::ID: {
    auto& typed = static_cast<td::td_api::chatMemberStatusBanned&>(value);
    wipe_native_value(typed.banned_until_date_, observer);
    return;
}
case td::td_api::chatMemberStatusCreator::ID: {
    auto& typed = static_cast<td::td_api::chatMemberStatusCreator&>(value);
    wipe_native_value(typed.is_anonymous_, observer);
    wipe_native_value(typed.is_member_, observer);
    return;
}
case td::td_api::chatMemberStatusLeft::ID: {
    return;
}
case td::td_api::chatMemberStatusMember::ID: {
    auto& typed = static_cast<td::td_api::chatMemberStatusMember&>(value);
    wipe_native_value(typed.member_until_date_, observer);
    return;
}
case td::td_api::chatMemberStatusRestricted::ID: {
    auto& typed = static_cast<td::td_api::chatMemberStatusRestricted&>(value);
    wipe_native_value(typed.is_member_, observer);
    wipe_native_value(typed.restricted_until_date_, observer);
    wipe_native_value(typed.permissions_, observer);
    return;
}
case td::td_api::chatMembers::ID: {
    auto& typed = static_cast<td::td_api::chatMembers&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.members_, observer);
    return;
}
case td::td_api::chatMembersFilterAdministrators::ID: {
    return;
}
case td::td_api::chatMembersFilterBanned::ID: {
    return;
}
case td::td_api::chatMembersFilterBots::ID: {
    return;
}
case td::td_api::chatMembersFilterContacts::ID: {
    return;
}
case td::td_api::chatMembersFilterMembers::ID: {
    return;
}
case td::td_api::chatMembersFilterMention::ID: {
    auto& typed = static_cast<td::td_api::chatMembersFilterMention&>(value);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::chatMembersFilterRestricted::ID: {
    return;
}
case td::td_api::chatMessageSender::ID: {
    auto& typed = static_cast<td::td_api::chatMessageSender&>(value);
    wipe_native_value(typed.sender_, observer);
    wipe_native_value(typed.needs_premium_, observer);
    return;
}
case td::td_api::chatMessageSenders::ID: {
    auto& typed = static_cast<td::td_api::chatMessageSenders&>(value);
    wipe_native_value(typed.senders_, observer);
    return;
}
case td::td_api::chatNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::chatNotificationSettings&>(value);
    wipe_native_value(typed.use_default_mute_for_, observer);
    wipe_native_value(typed.mute_for_, observer);
    wipe_native_value(typed.use_default_sound_, observer);
    wipe_native_value(typed.sound_id_, observer);
    wipe_native_value(typed.use_default_show_preview_, observer);
    wipe_native_value(typed.show_preview_, observer);
    wipe_native_value(typed.use_default_mute_stories_, observer);
    wipe_native_value(typed.mute_stories_, observer);
    wipe_native_value(typed.use_default_story_sound_, observer);
    wipe_native_value(typed.story_sound_id_, observer);
    wipe_native_value(typed.use_default_show_story_poster_, observer);
    wipe_native_value(typed.show_story_poster_, observer);
    wipe_native_value(typed.use_default_disable_pinned_message_notifications_, observer);
    wipe_native_value(typed.disable_pinned_message_notifications_, observer);
    wipe_native_value(typed.use_default_disable_mention_notifications_, observer);
    wipe_native_value(typed.disable_mention_notifications_, observer);
    return;
}
case td::td_api::chatPermissions::ID: {
    auto& typed = static_cast<td::td_api::chatPermissions&>(value);
    wipe_native_value(typed.can_send_basic_messages_, observer);
    wipe_native_value(typed.can_send_audios_, observer);
    wipe_native_value(typed.can_send_documents_, observer);
    wipe_native_value(typed.can_send_photos_, observer);
    wipe_native_value(typed.can_send_videos_, observer);
    wipe_native_value(typed.can_send_video_notes_, observer);
    wipe_native_value(typed.can_send_voice_notes_, observer);
    wipe_native_value(typed.can_send_polls_, observer);
    wipe_native_value(typed.can_send_other_messages_, observer);
    wipe_native_value(typed.can_add_link_previews_, observer);
    wipe_native_value(typed.can_react_to_messages_, observer);
    wipe_native_value(typed.can_edit_tag_, observer);
    wipe_native_value(typed.can_change_info_, observer);
    wipe_native_value(typed.can_invite_users_, observer);
    wipe_native_value(typed.can_pin_messages_, observer);
    wipe_native_value(typed.can_create_topics_, observer);
    return;
}
case td::td_api::chatPhoto::ID: {
    auto& typed = static_cast<td::td_api::chatPhoto&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.added_date_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.sizes_, observer);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.small_animation_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::chatPhotoInfo::ID: {
    auto& typed = static_cast<td::td_api::chatPhotoInfo&>(value);
    wipe_native_value(typed.small_, observer);
    wipe_native_value(typed.big_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.has_animation_, observer);
    wipe_native_value(typed.is_personal_, observer);
    return;
}
case td::td_api::chatPhotoSticker::ID: {
    auto& typed = static_cast<td::td_api::chatPhotoSticker&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.background_fill_, observer);
    return;
}
case td::td_api::chatPhotoStickerTypeCustomEmoji::ID: {
    auto& typed = static_cast<td::td_api::chatPhotoStickerTypeCustomEmoji&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::chatPhotoStickerTypeRegularOrMask::ID: {
    auto& typed = static_cast<td::td_api::chatPhotoStickerTypeRegularOrMask&>(value);
    wipe_native_value(typed.sticker_set_id_, observer);
    wipe_native_value(typed.sticker_id_, observer);
    return;
}
case td::td_api::chatPhotos::ID: {
    auto& typed = static_cast<td::td_api::chatPhotos&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.photos_, observer);
    return;
}
case td::td_api::chatPosition::ID: {
    auto& typed = static_cast<td::td_api::chatPosition&>(value);
    wipe_native_value(typed.list_, observer);
    wipe_native_value(typed.order_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    wipe_native_value(typed.source_, observer);
    return;
}
case td::td_api::chatRevenueAmount::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueAmount&>(value);
    wipe_native_value(typed.cryptocurrency_, observer);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.balance_amount_, observer);
    wipe_native_value(typed.available_amount_, observer);
    wipe_native_value(typed.withdrawal_enabled_, observer);
    return;
}
case td::td_api::chatRevenueStatistics::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueStatistics&>(value);
    wipe_native_value(typed.revenue_by_hour_graph_, observer);
    wipe_native_value(typed.revenue_graph_, observer);
    wipe_native_value(typed.revenue_amount_, observer);
    wipe_native_value(typed.usd_rate_, observer);
    return;
}
case td::td_api::chatRevenueTransaction::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueTransaction&>(value);
    wipe_native_value(typed.cryptocurrency_, observer);
    wipe_native_value(typed.cryptocurrency_amount_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::chatRevenueTransactionTypeFragmentRefund::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueTransactionTypeFragmentRefund&>(value);
    wipe_native_value(typed.refund_date_, observer);
    return;
}
case td::td_api::chatRevenueTransactionTypeFragmentWithdrawal::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueTransactionTypeFragmentWithdrawal&>(value);
    wipe_native_value(typed.withdrawal_date_, observer);
    wipe_native_value(typed.state_, observer);
    return;
}
case td::td_api::chatRevenueTransactionTypeSponsoredMessageEarnings::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueTransactionTypeSponsoredMessageEarnings&>(value);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.end_date_, observer);
    return;
}
case td::td_api::chatRevenueTransactionTypeSuggestedPostEarnings::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueTransactionTypeSuggestedPostEarnings&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::chatRevenueTransactionTypeUnsupported::ID: {
    return;
}
case td::td_api::chatRevenueTransactions::ID: {
    auto& typed = static_cast<td::td_api::chatRevenueTransactions&>(value);
    wipe_native_value(typed.ton_amount_, observer);
    wipe_native_value(typed.transactions_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::chatSourceMtprotoProxy::ID: {
    return;
}
case td::td_api::chatSourcePublicServiceAnnouncement::ID: {
    auto& typed = static_cast<td::td_api::chatSourcePublicServiceAnnouncement&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::chatStatisticsAdministratorActionsInfo::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsAdministratorActionsInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.deleted_message_count_, observer);
    wipe_native_value(typed.banned_user_count_, observer);
    wipe_native_value(typed.restricted_user_count_, observer);
    return;
}
case td::td_api::chatStatisticsChannel::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsChannel&>(value);
    wipe_native_value(typed.period_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.mean_message_view_count_, observer);
    wipe_native_value(typed.mean_message_share_count_, observer);
    wipe_native_value(typed.mean_message_reaction_count_, observer);
    wipe_native_value(typed.mean_story_view_count_, observer);
    wipe_native_value(typed.mean_story_share_count_, observer);
    wipe_native_value(typed.mean_story_reaction_count_, observer);
    wipe_native_value(typed.enabled_notifications_percentage_, observer);
    wipe_native_value(typed.member_count_graph_, observer);
    wipe_native_value(typed.join_graph_, observer);
    wipe_native_value(typed.mute_graph_, observer);
    wipe_native_value(typed.view_count_by_hour_graph_, observer);
    wipe_native_value(typed.view_count_by_source_graph_, observer);
    wipe_native_value(typed.join_by_source_graph_, observer);
    wipe_native_value(typed.language_graph_, observer);
    wipe_native_value(typed.message_interaction_graph_, observer);
    wipe_native_value(typed.message_reaction_graph_, observer);
    wipe_native_value(typed.story_interaction_graph_, observer);
    wipe_native_value(typed.story_reaction_graph_, observer);
    wipe_native_value(typed.instant_view_interaction_graph_, observer);
    wipe_native_value(typed.recent_interactions_, observer);
    return;
}
case td::td_api::chatStatisticsInteractionInfo::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsInteractionInfo&>(value);
    wipe_native_value(typed.object_type_, observer);
    wipe_native_value(typed.view_count_, observer);
    wipe_native_value(typed.forward_count_, observer);
    wipe_native_value(typed.reaction_count_, observer);
    return;
}
case td::td_api::chatStatisticsInviterInfo::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsInviterInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.added_member_count_, observer);
    return;
}
case td::td_api::chatStatisticsMessageSenderInfo::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsMessageSenderInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.sent_message_count_, observer);
    wipe_native_value(typed.average_character_count_, observer);
    return;
}
case td::td_api::chatStatisticsObjectTypeMessage::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsObjectTypeMessage&>(value);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::chatStatisticsObjectTypeStory::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsObjectTypeStory&>(value);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::chatStatisticsSupergroup::ID: {
    auto& typed = static_cast<td::td_api::chatStatisticsSupergroup&>(value);
    wipe_native_value(typed.period_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.message_count_, observer);
    wipe_native_value(typed.viewer_count_, observer);
    wipe_native_value(typed.sender_count_, observer);
    wipe_native_value(typed.member_count_graph_, observer);
    wipe_native_value(typed.join_graph_, observer);
    wipe_native_value(typed.join_by_source_graph_, observer);
    wipe_native_value(typed.language_graph_, observer);
    wipe_native_value(typed.message_content_graph_, observer);
    wipe_native_value(typed.action_graph_, observer);
    wipe_native_value(typed.day_graph_, observer);
    wipe_native_value(typed.week_graph_, observer);
    wipe_native_value(typed.top_senders_, observer);
    wipe_native_value(typed.top_administrators_, observer);
    wipe_native_value(typed.top_inviters_, observer);
    return;
}
case td::td_api::chatThemeEmoji::ID: {
    auto& typed = static_cast<td::td_api::chatThemeEmoji&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::chatThemeGift::ID: {
    auto& typed = static_cast<td::td_api::chatThemeGift&>(value);
    wipe_native_value(typed.gift_theme_, observer);
    return;
}
case td::td_api::chatTypeBasicGroup::ID: {
    auto& typed = static_cast<td::td_api::chatTypeBasicGroup&>(value);
    wipe_native_value(typed.basic_group_id_, observer);
    return;
}
case td::td_api::chatTypePrivate::ID: {
    auto& typed = static_cast<td::td_api::chatTypePrivate&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::chatTypeSecret::ID: {
    auto& typed = static_cast<td::td_api::chatTypeSecret&>(value);
    wipe_native_value(typed.secret_chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::chatTypeSupergroup::ID: {
    auto& typed = static_cast<td::td_api::chatTypeSupergroup&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.is_channel_, observer);
    return;
}
case td::td_api::chats::ID: {
    auto& typed = static_cast<td::td_api::chats&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::checkChatUsernameResultOk::ID: {
    return;
}
case td::td_api::checkChatUsernameResultPublicChatsTooMany::ID: {
    return;
}
case td::td_api::checkChatUsernameResultPublicGroupsUnavailable::ID: {
    return;
}
case td::td_api::checkChatUsernameResultUsernameInvalid::ID: {
    return;
}
case td::td_api::checkChatUsernameResultUsernameOccupied::ID: {
    return;
}
case td::td_api::checkChatUsernameResultUsernamePurchasable::ID: {
    return;
}
case td::td_api::checkStickerSetNameResultNameInvalid::ID: {
    return;
}
case td::td_api::checkStickerSetNameResultNameOccupied::ID: {
    return;
}
case td::td_api::checkStickerSetNameResultOk::ID: {
    return;
}
case td::td_api::checklist::ID: {
    auto& typed = static_cast<td::td_api::checklist&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.tasks_, observer);
    wipe_native_value(typed.others_can_add_tasks_, observer);
    wipe_native_value(typed.can_add_tasks_, observer);
    wipe_native_value(typed.others_can_mark_tasks_as_done_, observer);
    wipe_native_value(typed.can_mark_tasks_as_done_, observer);
    return;
}
case td::td_api::checklistTask::ID: {
    auto& typed = static_cast<td::td_api::checklistTask&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.completed_by_, observer);
    wipe_native_value(typed.completion_date_, observer);
    return;
}
case td::td_api::closeBirthdayUser::ID: {
    auto& typed = static_cast<td::td_api::closeBirthdayUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.birthdate_, observer);
    return;
}
case td::td_api::closedVectorPath::ID: {
    auto& typed = static_cast<td::td_api::closedVectorPath&>(value);
    wipe_native_value(typed.commands_, observer);
    return;
}
case td::td_api::collectibleItemInfo::ID: {
    auto& typed = static_cast<td::td_api::collectibleItemInfo&>(value);
    wipe_native_value(typed.purchase_date_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.cryptocurrency_, observer);
    wipe_native_value(typed.cryptocurrency_amount_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::collectibleItemTypePhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::collectibleItemTypePhoneNumber&>(value);
    wipe_native_value(typed.phone_number_, observer);
    return;
}
case td::td_api::collectibleItemTypeUsername::ID: {
    auto& typed = static_cast<td::td_api::collectibleItemTypeUsername&>(value);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::connectedAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::connectedAffiliateProgram&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.connection_date_, observer);
    wipe_native_value(typed.is_disconnected_, observer);
    wipe_native_value(typed.user_count_, observer);
    wipe_native_value(typed.revenue_star_count_, observer);
    return;
}
case td::td_api::connectedAffiliatePrograms::ID: {
    auto& typed = static_cast<td::td_api::connectedAffiliatePrograms&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.programs_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::connectedWebsite::ID: {
    auto& typed = static_cast<td::td_api::connectedWebsite&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.domain_name_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.browser_, observer);
    wipe_native_value(typed.platform_, observer);
    wipe_native_value(typed.log_in_date_, observer);
    wipe_native_value(typed.last_active_date_, observer);
    wipe_native_value(typed.ip_address_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::connectedWebsites::ID: {
    auto& typed = static_cast<td::td_api::connectedWebsites&>(value);
    wipe_native_value(typed.websites_, observer);
    return;
}
case td::td_api::connectionStateConnecting::ID: {
    return;
}
case td::td_api::connectionStateConnectingToProxy::ID: {
    return;
}
case td::td_api::connectionStateReady::ID: {
    return;
}
case td::td_api::connectionStateUpdating::ID: {
    return;
}
case td::td_api::connectionStateWaitingForNetwork::ID: {
    return;
}
case td::td_api::contact::ID: {
    auto& typed = static_cast<td::td_api::contact&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    wipe_native_value(typed.vcard_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::count::ID: {
    auto& typed = static_cast<td::td_api::count&>(value);
    wipe_native_value(typed.count_, observer);
    return;
}
case td::td_api::countries::ID: {
    auto& typed = static_cast<td::td_api::countries&>(value);
    wipe_native_value(typed.countries_, observer);
    return;
}
case td::td_api::countryInfo::ID: {
    auto& typed = static_cast<td::td_api::countryInfo&>(value);
    wipe_native_value(typed.country_code_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.english_name_, observer);
    wipe_native_value(typed.is_hidden_, observer);
    wipe_native_value(typed.calling_codes_, observer);
    return;
}
case td::td_api::craftGiftResultFail::ID: {
    return;
}
case td::td_api::craftGiftResultInvalidGift::ID: {
    return;
}
case td::td_api::craftGiftResultSuccess::ID: {
    auto& typed = static_cast<td::td_api::craftGiftResultSuccess&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    return;
}
case td::td_api::craftGiftResultTooEarly::ID: {
    auto& typed = static_cast<td::td_api::craftGiftResultTooEarly&>(value);
    wipe_native_value(typed.retry_after_, observer);
    return;
}
case td::td_api::createdBasicGroupChat::ID: {
    auto& typed = static_cast<td::td_api::createdBasicGroupChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.failed_to_add_members_, observer);
    return;
}
case td::td_api::currentWeather::ID: {
    auto& typed = static_cast<td::td_api::currentWeather&>(value);
    wipe_native_value(typed.temperature_, observer);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::customRequestResult::ID: {
    auto& typed = static_cast<td::td_api::customRequestResult&>(value);
    wipe_native_value(typed.result_, observer);
    return;
}
case td::td_api::data::ID: {
    auto& typed = static_cast<td::td_api::data&>(value);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::databaseStatistics::ID: {
    auto& typed = static_cast<td::td_api::databaseStatistics&>(value);
    wipe_native_value(typed.statistics_, observer);
    return;
}
case td::td_api::date::ID: {
    auto& typed = static_cast<td::td_api::date&>(value);
    wipe_native_value(typed.day_, observer);
    wipe_native_value(typed.month_, observer);
    wipe_native_value(typed.year_, observer);
    return;
}
case td::td_api::dateRange::ID: {
    auto& typed = static_cast<td::td_api::dateRange&>(value);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.end_date_, observer);
    return;
}
case td::td_api::dateTimeFormattingTypeAbsolute::ID: {
    auto& typed = static_cast<td::td_api::dateTimeFormattingTypeAbsolute&>(value);
    wipe_native_value(typed.time_precision_, observer);
    wipe_native_value(typed.date_precision_, observer);
    wipe_native_value(typed.show_day_of_week_, observer);
    return;
}
case td::td_api::dateTimeFormattingTypeRelative::ID: {
    return;
}
case td::td_api::dateTimePartPrecisionLong::ID: {
    return;
}
case td::td_api::dateTimePartPrecisionNone::ID: {
    return;
}
case td::td_api::dateTimePartPrecisionShort::ID: {
    return;
}
case td::td_api::datedFile::ID: {
    auto& typed = static_cast<td::td_api::datedFile&>(value);
    wipe_native_value(typed.file_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::deepLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::deepLinkInfo&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.need_update_application_, observer);
    return;
}
case td::td_api::deviceTokenApplePush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenApplePush&>(value);
    wipe_native_value(typed.device_token_, observer);
    wipe_native_value(typed.is_app_sandbox_, observer);
    return;
}
case td::td_api::deviceTokenApplePushVoIP::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenApplePushVoIP&>(value);
    wipe_native_value(typed.device_token_, observer);
    wipe_native_value(typed.is_app_sandbox_, observer);
    wipe_native_value(typed.encrypt_, observer);
    return;
}
case td::td_api::deviceTokenBlackBerryPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenBlackBerryPush&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::deviceTokenFirebaseCloudMessaging::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenFirebaseCloudMessaging&>(value);
    wipe_native_value(typed.token_, observer);
    wipe_native_value(typed.encrypt_, observer);
    return;
}
case td::td_api::deviceTokenHuaweiPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenHuaweiPush&>(value);
    wipe_native_value(typed.token_, observer);
    wipe_native_value(typed.encrypt_, observer);
    return;
}
case td::td_api::deviceTokenMicrosoftPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenMicrosoftPush&>(value);
    wipe_native_value(typed.channel_uri_, observer);
    return;
}
case td::td_api::deviceTokenMicrosoftPushVoIP::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenMicrosoftPushVoIP&>(value);
    wipe_native_value(typed.channel_uri_, observer);
    return;
}
case td::td_api::deviceTokenSimplePush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenSimplePush&>(value);
    wipe_native_value(typed.endpoint_, observer);
    return;
}
case td::td_api::deviceTokenTizenPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenTizenPush&>(value);
    wipe_native_value(typed.reg_id_, observer);
    return;
}
case td::td_api::deviceTokenUbuntuPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenUbuntuPush&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::deviceTokenWebPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenWebPush&>(value);
    wipe_native_value(typed.endpoint_, observer);
    wipe_native_value(typed.p256dh_base64url_, observer);
    wipe_native_value(typed.auth_base64url_, observer);
    return;
}
case td::td_api::deviceTokenWindowsPush::ID: {
    auto& typed = static_cast<td::td_api::deviceTokenWindowsPush&>(value);
    wipe_native_value(typed.access_token_, observer);
    return;
}
case td::td_api::diceStickersRegular::ID: {
    auto& typed = static_cast<td::td_api::diceStickersRegular&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::diceStickersSlotMachine::ID: {
    auto& typed = static_cast<td::td_api::diceStickersSlotMachine&>(value);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.lever_, observer);
    wipe_native_value(typed.left_reel_, observer);
    wipe_native_value(typed.center_reel_, observer);
    wipe_native_value(typed.right_reel_, observer);
    return;
}
case td::td_api::diffEntity::ID: {
    auto& typed = static_cast<td::td_api::diffEntity&>(value);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.length_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::diffEntityTypeDelete::ID: {
    return;
}
case td::td_api::diffEntityTypeInsert::ID: {
    return;
}
case td::td_api::diffEntityTypeReplace::ID: {
    auto& typed = static_cast<td::td_api::diffEntityTypeReplace&>(value);
    wipe_native_value(typed.old_text_, observer);
    return;
}
case td::td_api::diffText::ID: {
    auto& typed = static_cast<td::td_api::diffText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.entities_, observer);
    return;
}
case td::td_api::directMessagesChatTopic::ID: {
    auto& typed = static_cast<td::td_api::directMessagesChatTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.order_, observer);
    wipe_native_value(typed.can_send_unpaid_messages_, observer);
    wipe_native_value(typed.is_marked_as_unread_, observer);
    wipe_native_value(typed.unread_count_, observer);
    wipe_native_value(typed.last_read_inbox_message_id_, observer);
    wipe_native_value(typed.last_read_outbox_message_id_, observer);
    wipe_native_value(typed.unread_reaction_count_, observer);
    wipe_native_value(typed.last_message_, observer);
    wipe_native_value(typed.draft_message_, observer);
    return;
}
case td::td_api::document::ID: {
    auto& typed = static_cast<td::td_api::document&>(value);
    wipe_native_value(typed.file_name_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.document_, observer);
    return;
}
case td::td_api::downloadedFileCounts::ID: {
    auto& typed = static_cast<td::td_api::downloadedFileCounts&>(value);
    wipe_native_value(typed.active_count_, observer);
    wipe_native_value(typed.paused_count_, observer);
    wipe_native_value(typed.completed_count_, observer);
    return;
}
case td::td_api::draftMessage::ID: {
    auto& typed = static_cast<td::td_api::draftMessage&>(value);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.effect_id_, observer);
    wipe_native_value(typed.suggested_post_info_, observer);
    return;
}
case td::td_api::draftMessageContentRichMessage::ID: {
    auto& typed = static_cast<td::td_api::draftMessageContentRichMessage&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::draftMessageContentText::ID: {
    auto& typed = static_cast<td::td_api::draftMessageContentText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.link_preview_options_, observer);
    return;
}
case td::td_api::draftMessageContentVideoNote::ID: {
    auto& typed = static_cast<td::td_api::draftMessageContentVideoNote&>(value);
    wipe_native_value(typed.file_path_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.length_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    return;
}
case td::td_api::draftMessageContentVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::draftMessageContentVoiceNote&>(value);
    wipe_native_value(typed.file_path_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.waveform_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    return;
}
case td::td_api::emailAddressAuthenticationAppleId::ID: {
    auto& typed = static_cast<td::td_api::emailAddressAuthenticationAppleId&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::emailAddressAuthenticationCode::ID: {
    auto& typed = static_cast<td::td_api::emailAddressAuthenticationCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::emailAddressAuthenticationCodeInfo::ID: {
    auto& typed = static_cast<td::td_api::emailAddressAuthenticationCodeInfo&>(value);
    wipe_native_value(typed.email_address_pattern_, observer);
    wipe_native_value(typed.length_, observer);
    return;
}
case td::td_api::emailAddressAuthenticationGoogleId::ID: {
    auto& typed = static_cast<td::td_api::emailAddressAuthenticationGoogleId&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::emailAddressResetStateAvailable::ID: {
    auto& typed = static_cast<td::td_api::emailAddressResetStateAvailable&>(value);
    wipe_native_value(typed.wait_period_, observer);
    return;
}
case td::td_api::emailAddressResetStatePending::ID: {
    auto& typed = static_cast<td::td_api::emailAddressResetStatePending&>(value);
    wipe_native_value(typed.reset_in_, observer);
    return;
}
case td::td_api::emojiCategories::ID: {
    auto& typed = static_cast<td::td_api::emojiCategories&>(value);
    wipe_native_value(typed.categories_, observer);
    return;
}
case td::td_api::emojiCategory::ID: {
    auto& typed = static_cast<td::td_api::emojiCategory&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.icon_, observer);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.is_greeting_, observer);
    return;
}
case td::td_api::emojiCategorySourcePremium::ID: {
    return;
}
case td::td_api::emojiCategorySourceSearch::ID: {
    auto& typed = static_cast<td::td_api::emojiCategorySourceSearch&>(value);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::emojiCategoryTypeChatPhoto::ID: {
    return;
}
case td::td_api::emojiCategoryTypeDefault::ID: {
    return;
}
case td::td_api::emojiCategoryTypeEmojiStatus::ID: {
    return;
}
case td::td_api::emojiCategoryTypeRegularStickers::ID: {
    return;
}
case td::td_api::emojiChatTheme::ID: {
    auto& typed = static_cast<td::td_api::emojiChatTheme&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.light_settings_, observer);
    wipe_native_value(typed.dark_settings_, observer);
    return;
}
case td::td_api::emojiKeyword::ID: {
    auto& typed = static_cast<td::td_api::emojiKeyword&>(value);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.keyword_, observer);
    return;
}
case td::td_api::emojiKeywords::ID: {
    auto& typed = static_cast<td::td_api::emojiKeywords&>(value);
    wipe_native_value(typed.emoji_keywords_, observer);
    return;
}
case td::td_api::emojiReaction::ID: {
    auto& typed = static_cast<td::td_api::emojiReaction&>(value);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.is_active_, observer);
    wipe_native_value(typed.static_icon_, observer);
    wipe_native_value(typed.appear_animation_, observer);
    wipe_native_value(typed.select_animation_, observer);
    wipe_native_value(typed.activate_animation_, observer);
    wipe_native_value(typed.effect_animation_, observer);
    wipe_native_value(typed.around_animation_, observer);
    wipe_native_value(typed.center_animation_, observer);
    return;
}
case td::td_api::emojiStatus::ID: {
    auto& typed = static_cast<td::td_api::emojiStatus&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    return;
}
case td::td_api::emojiStatusCustomEmojis::ID: {
    auto& typed = static_cast<td::td_api::emojiStatusCustomEmojis&>(value);
    wipe_native_value(typed.custom_emoji_ids_, observer);
    return;
}
case td::td_api::emojiStatusTypeCustomEmoji::ID: {
    auto& typed = static_cast<td::td_api::emojiStatusTypeCustomEmoji&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::emojiStatusTypeUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::emojiStatusTypeUpgradedGift&>(value);
    wipe_native_value(typed.upgraded_gift_id_, observer);
    wipe_native_value(typed.gift_title_, observer);
    wipe_native_value(typed.gift_name_, observer);
    wipe_native_value(typed.model_custom_emoji_id_, observer);
    wipe_native_value(typed.symbol_custom_emoji_id_, observer);
    wipe_native_value(typed.backdrop_colors_, observer);
    return;
}
case td::td_api::emojiStatuses::ID: {
    auto& typed = static_cast<td::td_api::emojiStatuses&>(value);
    wipe_native_value(typed.emoji_statuses_, observer);
    return;
}
case td::td_api::emojis::ID: {
    auto& typed = static_cast<td::td_api::emojis&>(value);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::encryptedCredentials::ID: {
    auto& typed = static_cast<td::td_api::encryptedCredentials&>(value);
    wipe_native_value(typed.data_, observer);
    wipe_native_value(typed.hash_, observer);
    wipe_native_value(typed.secret_, observer);
    return;
}
case td::td_api::encryptedPassportElement::ID: {
    auto& typed = static_cast<td::td_api::encryptedPassportElement&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.data_, observer);
    wipe_native_value(typed.front_side_, observer);
    wipe_native_value(typed.reverse_side_, observer);
    wipe_native_value(typed.selfie_, observer);
    wipe_native_value(typed.translation_, observer);
    wipe_native_value(typed.files_, observer);
    wipe_native_value(typed.value_, observer);
    wipe_native_value(typed.hash_, observer);
    return;
}
case td::td_api::error::ID: {
    auto& typed = static_cast<td::td_api::error&>(value);
    wipe_native_value(typed.code_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::factCheck::ID: {
    auto& typed = static_cast<td::td_api::factCheck&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.country_code_, observer);
    return;
}
case td::td_api::failedToAddMember::ID: {
    auto& typed = static_cast<td::td_api::failedToAddMember&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.premium_would_allow_invite_, observer);
    wipe_native_value(typed.premium_required_to_send_messages_, observer);
    return;
}
case td::td_api::failedToAddMembers::ID: {
    auto& typed = static_cast<td::td_api::failedToAddMembers&>(value);
    wipe_native_value(typed.failed_to_add_members_, observer);
    return;
}
case td::td_api::file::ID: {
    auto& typed = static_cast<td::td_api::file&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.size_, observer);
    wipe_native_value(typed.expected_size_, observer);
    wipe_native_value(typed.local_, observer);
    wipe_native_value(typed.remote_, observer);
    return;
}
case td::td_api::fileDownload::ID: {
    auto& typed = static_cast<td::td_api::fileDownload&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.add_date_, observer);
    wipe_native_value(typed.complete_date_, observer);
    wipe_native_value(typed.is_paused_, observer);
    return;
}
case td::td_api::fileDownloadedPrefixSize::ID: {
    auto& typed = static_cast<td::td_api::fileDownloadedPrefixSize&>(value);
    wipe_native_value(typed.size_, observer);
    return;
}
case td::td_api::fileTypeAnimation::ID: {
    return;
}
case td::td_api::fileTypeAudio::ID: {
    return;
}
case td::td_api::fileTypeDocument::ID: {
    return;
}
case td::td_api::fileTypeLivePhotoVideo::ID: {
    return;
}
case td::td_api::fileTypeNone::ID: {
    return;
}
case td::td_api::fileTypeNotificationSound::ID: {
    return;
}
case td::td_api::fileTypePhoto::ID: {
    return;
}
case td::td_api::fileTypePhotoStory::ID: {
    return;
}
case td::td_api::fileTypeProfilePhoto::ID: {
    return;
}
case td::td_api::fileTypeSecret::ID: {
    return;
}
case td::td_api::fileTypeSecretThumbnail::ID: {
    return;
}
case td::td_api::fileTypeSecure::ID: {
    return;
}
case td::td_api::fileTypeSelfDestructingLivePhotoVideo::ID: {
    return;
}
case td::td_api::fileTypeSelfDestructingPhoto::ID: {
    return;
}
case td::td_api::fileTypeSelfDestructingVideo::ID: {
    return;
}
case td::td_api::fileTypeSelfDestructingVideoNote::ID: {
    return;
}
case td::td_api::fileTypeSelfDestructingVoiceNote::ID: {
    return;
}
case td::td_api::fileTypeSticker::ID: {
    return;
}
case td::td_api::fileTypeThumbnail::ID: {
    return;
}
case td::td_api::fileTypeUnknown::ID: {
    return;
}
case td::td_api::fileTypeVideo::ID: {
    return;
}
case td::td_api::fileTypeVideoNote::ID: {
    return;
}
case td::td_api::fileTypeVideoStory::ID: {
    return;
}
case td::td_api::fileTypeVoiceNote::ID: {
    return;
}
case td::td_api::fileTypeWallpaper::ID: {
    return;
}
case td::td_api::firebaseAuthenticationSettingsAndroid::ID: {
    return;
}
case td::td_api::firebaseAuthenticationSettingsIos::ID: {
    auto& typed = static_cast<td::td_api::firebaseAuthenticationSettingsIos&>(value);
    wipe_native_value(typed.device_token_, observer);
    wipe_native_value(typed.is_app_sandbox_, observer);
    return;
}
case td::td_api::firebaseDeviceVerificationParametersPlayIntegrity::ID: {
    auto& typed = static_cast<td::td_api::firebaseDeviceVerificationParametersPlayIntegrity&>(value);
    wipe_native_value(typed.nonce_, observer);
    wipe_native_value(typed.cloud_project_number_, observer);
    return;
}
case td::td_api::firebaseDeviceVerificationParametersSafetyNet::ID: {
    auto& typed = static_cast<td::td_api::firebaseDeviceVerificationParametersSafetyNet&>(value);
    wipe_native_value(typed.nonce_, observer);
    return;
}
case td::td_api::fixedText::ID: {
    auto& typed = static_cast<td::td_api::fixedText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.diff_text_, observer);
    return;
}
case td::td_api::formattedText::ID: {
    auto& typed = static_cast<td::td_api::formattedText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.entities_, observer);
    return;
}
case td::td_api::forumTopic::ID: {
    auto& typed = static_cast<td::td_api::forumTopic&>(value);
    wipe_native_value(typed.info_, observer);
    wipe_native_value(typed.last_message_, observer);
    wipe_native_value(typed.order_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    wipe_native_value(typed.unread_count_, observer);
    wipe_native_value(typed.last_read_inbox_message_id_, observer);
    wipe_native_value(typed.last_read_outbox_message_id_, observer);
    wipe_native_value(typed.unread_mention_count_, observer);
    wipe_native_value(typed.unread_reaction_count_, observer);
    wipe_native_value(typed.unread_poll_vote_count_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    wipe_native_value(typed.draft_message_, observer);
    return;
}
case td::td_api::forumTopicIcon::ID: {
    auto& typed = static_cast<td::td_api::forumTopicIcon&>(value);
    wipe_native_value(typed.color_, observer);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::forumTopicInfo::ID: {
    auto& typed = static_cast<td::td_api::forumTopicInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.icon_, observer);
    wipe_native_value(typed.creation_date_, observer);
    wipe_native_value(typed.creator_id_, observer);
    wipe_native_value(typed.is_general_, observer);
    wipe_native_value(typed.is_outgoing_, observer);
    wipe_native_value(typed.is_closed_, observer);
    wipe_native_value(typed.is_hidden_, observer);
    wipe_native_value(typed.is_name_implicit_, observer);
    return;
}
case td::td_api::forumTopics::ID: {
    auto& typed = static_cast<td::td_api::forumTopics&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.topics_, observer);
    wipe_native_value(typed.next_offset_date_, observer);
    wipe_native_value(typed.next_offset_message_id_, observer);
    wipe_native_value(typed.next_offset_forum_topic_id_, observer);
    return;
}
case td::td_api::forwardSource::ID: {
    auto& typed = static_cast<td::td_api::forwardSource&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.sender_name_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.is_outgoing_, observer);
    return;
}
case td::td_api::foundAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::foundAffiliateProgram&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.info_, observer);
    return;
}
case td::td_api::foundAffiliatePrograms::ID: {
    auto& typed = static_cast<td::td_api::foundAffiliatePrograms&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.programs_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::foundChatBoosts::ID: {
    auto& typed = static_cast<td::td_api::foundChatBoosts&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.boosts_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::foundChatMessages::ID: {
    auto& typed = static_cast<td::td_api::foundChatMessages&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.messages_, observer);
    wipe_native_value(typed.next_from_message_id_, observer);
    return;
}
case td::td_api::foundFileDownloads::ID: {
    auto& typed = static_cast<td::td_api::foundFileDownloads&>(value);
    wipe_native_value(typed.total_counts_, observer);
    wipe_native_value(typed.files_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::foundMessages::ID: {
    auto& typed = static_cast<td::td_api::foundMessages&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.messages_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::foundPosition::ID: {
    auto& typed = static_cast<td::td_api::foundPosition&>(value);
    wipe_native_value(typed.position_, observer);
    return;
}
case td::td_api::foundPositions::ID: {
    auto& typed = static_cast<td::td_api::foundPositions&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.positions_, observer);
    return;
}
case td::td_api::foundPublicPosts::ID: {
    auto& typed = static_cast<td::td_api::foundPublicPosts&>(value);
    wipe_native_value(typed.messages_, observer);
    wipe_native_value(typed.next_offset_, observer);
    wipe_native_value(typed.search_limits_, observer);
    wipe_native_value(typed.are_limits_exceeded_, observer);
    return;
}
case td::td_api::foundStories::ID: {
    auto& typed = static_cast<td::td_api::foundStories&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.stories_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::foundUsers::ID: {
    auto& typed = static_cast<td::td_api::foundUsers&>(value);
    wipe_native_value(typed.user_ids_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::foundWebApp::ID: {
    auto& typed = static_cast<td::td_api::foundWebApp&>(value);
    wipe_native_value(typed.web_app_, observer);
    wipe_native_value(typed.request_write_access_, observer);
    wipe_native_value(typed.skip_confirmation_, observer);
    return;
}
case td::td_api::game::ID: {
    auto& typed = static_cast<td::td_api::game&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.short_name_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::gameHighScore::ID: {
    auto& typed = static_cast<td::td_api::gameHighScore&>(value);
    wipe_native_value(typed.position_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.score_, observer);
    return;
}
case td::td_api::gameHighScores::ID: {
    auto& typed = static_cast<td::td_api::gameHighScores&>(value);
    wipe_native_value(typed.scores_, observer);
    return;
}
case td::td_api::gift::ID: {
    auto& typed = static_cast<td::td_api::gift&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.publisher_chat_id_, observer);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.default_sell_star_count_, observer);
    wipe_native_value(typed.upgrade_star_count_, observer);
    wipe_native_value(typed.upgrade_variant_count_, observer);
    wipe_native_value(typed.has_colors_, observer);
    wipe_native_value(typed.is_for_birthday_, observer);
    wipe_native_value(typed.is_premium_, observer);
    wipe_native_value(typed.auction_info_, observer);
    wipe_native_value(typed.next_send_date_, observer);
    wipe_native_value(typed.user_limits_, observer);
    wipe_native_value(typed.overall_limits_, observer);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.first_send_date_, observer);
    wipe_native_value(typed.last_send_date_, observer);
    return;
}
case td::td_api::giftAuction::ID: {
    auto& typed = static_cast<td::td_api::giftAuction&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.gifts_per_round_, observer);
    wipe_native_value(typed.start_date_, observer);
    return;
}
case td::td_api::giftAuctionAcquiredGift::ID: {
    auto& typed = static_cast<td::td_api::giftAuctionAcquiredGift&>(value);
    wipe_native_value(typed.receiver_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.auction_round_number_, observer);
    wipe_native_value(typed.auction_round_position_, observer);
    wipe_native_value(typed.unique_gift_number_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.is_private_, observer);
    return;
}
case td::td_api::giftAuctionAcquiredGifts::ID: {
    auto& typed = static_cast<td::td_api::giftAuctionAcquiredGifts&>(value);
    wipe_native_value(typed.gifts_, observer);
    return;
}
case td::td_api::giftAuctionState::ID: {
    auto& typed = static_cast<td::td_api::giftAuctionState&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.state_, observer);
    return;
}
case td::td_api::giftBackground::ID: {
    auto& typed = static_cast<td::td_api::giftBackground&>(value);
    wipe_native_value(typed.center_color_, observer);
    wipe_native_value(typed.edge_color_, observer);
    wipe_native_value(typed.text_color_, observer);
    return;
}
case td::td_api::giftChatTheme::ID: {
    auto& typed = static_cast<td::td_api::giftChatTheme&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.light_settings_, observer);
    wipe_native_value(typed.dark_settings_, observer);
    return;
}
case td::td_api::giftChatThemes::ID: {
    auto& typed = static_cast<td::td_api::giftChatThemes&>(value);
    wipe_native_value(typed.themes_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::giftCollection::ID: {
    auto& typed = static_cast<td::td_api::giftCollection&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.icon_, observer);
    wipe_native_value(typed.gift_count_, observer);
    return;
}
case td::td_api::giftCollections::ID: {
    auto& typed = static_cast<td::td_api::giftCollections&>(value);
    wipe_native_value(typed.collections_, observer);
    return;
}
case td::td_api::giftForResale::ID: {
    auto& typed = static_cast<td::td_api::giftForResale&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    return;
}
case td::td_api::giftForResaleOrderNumber::ID: {
    return;
}
case td::td_api::giftForResaleOrderPrice::ID: {
    return;
}
case td::td_api::giftForResaleOrderPriceChangeDate::ID: {
    return;
}
case td::td_api::giftPurchaseLimits::ID: {
    auto& typed = static_cast<td::td_api::giftPurchaseLimits&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.remaining_count_, observer);
    return;
}
case td::td_api::giftPurchaseOfferStateAccepted::ID: {
    return;
}
case td::td_api::giftPurchaseOfferStatePending::ID: {
    return;
}
case td::td_api::giftPurchaseOfferStateRejected::ID: {
    return;
}
case td::td_api::giftResaleParameters::ID: {
    auto& typed = static_cast<td::td_api::giftResaleParameters&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.toncoin_cent_count_, observer);
    wipe_native_value(typed.toncoin_only_, observer);
    return;
}
case td::td_api::giftResalePriceStar::ID: {
    auto& typed = static_cast<td::td_api::giftResalePriceStar&>(value);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::giftResalePriceTon::ID: {
    auto& typed = static_cast<td::td_api::giftResalePriceTon&>(value);
    wipe_native_value(typed.toncoin_cent_count_, observer);
    return;
}
case td::td_api::giftResaleResultOk::ID: {
    auto& typed = static_cast<td::td_api::giftResaleResultOk&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    return;
}
case td::td_api::giftResaleResultPriceIncreased::ID: {
    auto& typed = static_cast<td::td_api::giftResaleResultPriceIncreased&>(value);
    wipe_native_value(typed.price_, observer);
    return;
}
case td::td_api::giftSettings::ID: {
    auto& typed = static_cast<td::td_api::giftSettings&>(value);
    wipe_native_value(typed.show_gift_button_, observer);
    wipe_native_value(typed.accepted_gift_types_, observer);
    return;
}
case td::td_api::giftUpgradePreview::ID: {
    auto& typed = static_cast<td::td_api::giftUpgradePreview&>(value);
    wipe_native_value(typed.models_, observer);
    wipe_native_value(typed.symbols_, observer);
    wipe_native_value(typed.backdrops_, observer);
    wipe_native_value(typed.prices_, observer);
    wipe_native_value(typed.next_prices_, observer);
    return;
}
case td::td_api::giftUpgradePrice::ID: {
    auto& typed = static_cast<td::td_api::giftUpgradePrice&>(value);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::giftUpgradeVariants::ID: {
    auto& typed = static_cast<td::td_api::giftUpgradeVariants&>(value);
    wipe_native_value(typed.models_, observer);
    wipe_native_value(typed.symbols_, observer);
    wipe_native_value(typed.backdrops_, observer);
    return;
}
case td::td_api::giftsForCrafting::ID: {
    auto& typed = static_cast<td::td_api::giftsForCrafting&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.gifts_, observer);
    wipe_native_value(typed.attribute_persistence_probabilities_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::giftsForResale::ID: {
    auto& typed = static_cast<td::td_api::giftsForResale&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.gifts_, observer);
    wipe_native_value(typed.models_, observer);
    wipe_native_value(typed.symbols_, observer);
    wipe_native_value(typed.backdrops_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::giveawayInfoCompleted::ID: {
    auto& typed = static_cast<td::td_api::giveawayInfoCompleted&>(value);
    wipe_native_value(typed.creation_date_, observer);
    wipe_native_value(typed.actual_winners_selection_date_, observer);
    wipe_native_value(typed.was_refunded_, observer);
    wipe_native_value(typed.is_winner_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.activation_count_, observer);
    wipe_native_value(typed.gift_code_, observer);
    wipe_native_value(typed.won_star_count_, observer);
    return;
}
case td::td_api::giveawayInfoOngoing::ID: {
    auto& typed = static_cast<td::td_api::giveawayInfoOngoing&>(value);
    wipe_native_value(typed.creation_date_, observer);
    wipe_native_value(typed.status_, observer);
    wipe_native_value(typed.is_ended_, observer);
    return;
}
case td::td_api::giveawayParameters::ID: {
    auto& typed = static_cast<td::td_api::giveawayParameters&>(value);
    wipe_native_value(typed.boosted_chat_id_, observer);
    wipe_native_value(typed.additional_chat_ids_, observer);
    wipe_native_value(typed.winners_selection_date_, observer);
    wipe_native_value(typed.only_new_members_, observer);
    wipe_native_value(typed.has_public_winners_, observer);
    wipe_native_value(typed.country_codes_, observer);
    wipe_native_value(typed.prize_description_, observer);
    return;
}
case td::td_api::giveawayParticipantStatusAdministrator::ID: {
    auto& typed = static_cast<td::td_api::giveawayParticipantStatusAdministrator&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::giveawayParticipantStatusAlreadyWasMember::ID: {
    auto& typed = static_cast<td::td_api::giveawayParticipantStatusAlreadyWasMember&>(value);
    wipe_native_value(typed.joined_chat_date_, observer);
    return;
}
case td::td_api::giveawayParticipantStatusDisallowedCountry::ID: {
    auto& typed = static_cast<td::td_api::giveawayParticipantStatusDisallowedCountry&>(value);
    wipe_native_value(typed.user_country_code_, observer);
    return;
}
case td::td_api::giveawayParticipantStatusEligible::ID: {
    return;
}
case td::td_api::giveawayParticipantStatusParticipating::ID: {
    return;
}
case td::td_api::giveawayPrizePremium::ID: {
    auto& typed = static_cast<td::td_api::giveawayPrizePremium&>(value);
    wipe_native_value(typed.month_count_, observer);
    return;
}
case td::td_api::giveawayPrizeStars::ID: {
    auto& typed = static_cast<td::td_api::giveawayPrizeStars&>(value);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::groupCall::ID: {
    auto& typed = static_cast<td::td_api::groupCall&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    wipe_native_value(typed.scheduled_start_date_, observer);
    wipe_native_value(typed.enabled_start_notification_, observer);
    wipe_native_value(typed.is_active_, observer);
    wipe_native_value(typed.is_video_chat_, observer);
    wipe_native_value(typed.is_live_story_, observer);
    wipe_native_value(typed.is_rtmp_stream_, observer);
    wipe_native_value(typed.is_joined_, observer);
    wipe_native_value(typed.need_rejoin_, observer);
    wipe_native_value(typed.is_owned_, observer);
    wipe_native_value(typed.can_be_managed_, observer);
    wipe_native_value(typed.participant_count_, observer);
    wipe_native_value(typed.has_hidden_listeners_, observer);
    wipe_native_value(typed.loaded_all_participants_, observer);
    wipe_native_value(typed.message_sender_id_, observer);
    wipe_native_value(typed.recent_speakers_, observer);
    wipe_native_value(typed.is_my_video_enabled_, observer);
    wipe_native_value(typed.is_my_video_paused_, observer);
    wipe_native_value(typed.can_enable_video_, observer);
    wipe_native_value(typed.mute_new_participants_, observer);
    wipe_native_value(typed.can_toggle_mute_new_participants_, observer);
    wipe_native_value(typed.can_send_messages_, observer);
    wipe_native_value(typed.are_messages_allowed_, observer);
    wipe_native_value(typed.can_toggle_are_messages_allowed_, observer);
    wipe_native_value(typed.can_delete_messages_, observer);
    wipe_native_value(typed.record_duration_, observer);
    wipe_native_value(typed.is_video_recorded_, observer);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::groupCallDataChannelMain::ID: {
    return;
}
case td::td_api::groupCallDataChannelScreenSharing::ID: {
    return;
}
case td::td_api::groupCallId::ID: {
    auto& typed = static_cast<td::td_api::groupCallId&>(value);
    wipe_native_value(typed.id_, observer);
    return;
}
case td::td_api::groupCallInfo::ID: {
    auto& typed = static_cast<td::td_api::groupCallInfo&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.join_payload_, observer);
    return;
}
case td::td_api::groupCallJoinParameters::ID: {
    auto& typed = static_cast<td::td_api::groupCallJoinParameters&>(value);
    wipe_native_value(typed.audio_source_id_, observer);
    wipe_native_value(typed.payload_, observer);
    wipe_native_value(typed.is_muted_, observer);
    wipe_native_value(typed.is_my_video_enabled_, observer);
    return;
}
case td::td_api::groupCallMessage::ID: {
    auto& typed = static_cast<td::td_api::groupCallMessage&>(value);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    wipe_native_value(typed.is_from_owner_, observer);
    wipe_native_value(typed.can_be_deleted_, observer);
    return;
}
case td::td_api::groupCallMessageLevel::ID: {
    auto& typed = static_cast<td::td_api::groupCallMessageLevel&>(value);
    wipe_native_value(typed.min_star_count_, observer);
    wipe_native_value(typed.pin_duration_, observer);
    wipe_native_value(typed.max_text_length_, observer);
    wipe_native_value(typed.max_custom_emoji_count_, observer);
    wipe_native_value(typed.first_color_, observer);
    wipe_native_value(typed.second_color_, observer);
    wipe_native_value(typed.background_color_, observer);
    return;
}
case td::td_api::groupCallParticipant::ID: {
    auto& typed = static_cast<td::td_api::groupCallParticipant&>(value);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.audio_source_id_, observer);
    wipe_native_value(typed.screen_sharing_audio_source_id_, observer);
    wipe_native_value(typed.video_info_, observer);
    wipe_native_value(typed.screen_sharing_video_info_, observer);
    wipe_native_value(typed.bio_, observer);
    wipe_native_value(typed.is_current_user_, observer);
    wipe_native_value(typed.is_speaking_, observer);
    wipe_native_value(typed.is_hand_raised_, observer);
    wipe_native_value(typed.can_be_muted_for_all_users_, observer);
    wipe_native_value(typed.can_be_unmuted_for_all_users_, observer);
    wipe_native_value(typed.can_be_muted_for_current_user_, observer);
    wipe_native_value(typed.can_be_unmuted_for_current_user_, observer);
    wipe_native_value(typed.is_muted_for_all_users_, observer);
    wipe_native_value(typed.is_muted_for_current_user_, observer);
    wipe_native_value(typed.can_unmute_self_, observer);
    wipe_native_value(typed.volume_level_, observer);
    wipe_native_value(typed.order_, observer);
    return;
}
case td::td_api::groupCallParticipantVideoInfo::ID: {
    auto& typed = static_cast<td::td_api::groupCallParticipantVideoInfo&>(value);
    wipe_native_value(typed.source_groups_, observer);
    wipe_native_value(typed.endpoint_id_, observer);
    wipe_native_value(typed.is_paused_, observer);
    return;
}
case td::td_api::groupCallParticipants::ID: {
    auto& typed = static_cast<td::td_api::groupCallParticipants&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.participant_ids_, observer);
    return;
}
case td::td_api::groupCallRecentSpeaker::ID: {
    auto& typed = static_cast<td::td_api::groupCallRecentSpeaker&>(value);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.is_speaking_, observer);
    return;
}
case td::td_api::groupCallStream::ID: {
    auto& typed = static_cast<td::td_api::groupCallStream&>(value);
    wipe_native_value(typed.channel_id_, observer);
    wipe_native_value(typed.scale_, observer);
    wipe_native_value(typed.time_offset_, observer);
    return;
}
case td::td_api::groupCallStreams::ID: {
    auto& typed = static_cast<td::td_api::groupCallStreams&>(value);
    wipe_native_value(typed.streams_, observer);
    return;
}
case td::td_api::groupCallVideoQualityFull::ID: {
    return;
}
case td::td_api::groupCallVideoQualityMedium::ID: {
    return;
}
case td::td_api::groupCallVideoQualityThumbnail::ID: {
    return;
}
case td::td_api::groupCallVideoSourceGroup::ID: {
    auto& typed = static_cast<td::td_api::groupCallVideoSourceGroup&>(value);
    wipe_native_value(typed.semantics_, observer);
    wipe_native_value(typed.source_ids_, observer);
    return;
}
case td::td_api::hashtags::ID: {
    auto& typed = static_cast<td::td_api::hashtags&>(value);
    wipe_native_value(typed.hashtags_, observer);
    return;
}
case td::td_api::httpUrl::ID: {
    auto& typed = static_cast<td::td_api::httpUrl&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::identityDocument::ID: {
    auto& typed = static_cast<td::td_api::identityDocument&>(value);
    wipe_native_value(typed.number_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.front_side_, observer);
    wipe_native_value(typed.reverse_side_, observer);
    wipe_native_value(typed.selfie_, observer);
    wipe_native_value(typed.translation_, observer);
    return;
}
case td::td_api::importedContact::ID: {
    auto& typed = static_cast<td::td_api::importedContact&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    wipe_native_value(typed.note_, observer);
    return;
}
case td::td_api::importedContacts::ID: {
    auto& typed = static_cast<td::td_api::importedContacts&>(value);
    wipe_native_value(typed.user_ids_, observer);
    wipe_native_value(typed.importer_count_, observer);
    return;
}
case td::td_api::inlineKeyboardButton::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButton&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.icon_custom_emoji_id_, observer);
    wipe_native_value(typed.style_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeBuy::ID: {
    return;
}
case td::td_api::inlineKeyboardButtonTypeCallback::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeCallback&>(value);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeCallbackGame::ID: {
    return;
}
case td::td_api::inlineKeyboardButtonTypeCallbackWithPassword::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeCallbackWithPassword&>(value);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeCopyText::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeCopyText&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeLoginUrl::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeLoginUrl&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.forward_text_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeSwitchInline::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeSwitchInline&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.target_chat_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeUrl::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeUrl&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeUser::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::inlineKeyboardButtonTypeWebApp::ID: {
    auto& typed = static_cast<td::td_api::inlineKeyboardButtonTypeWebApp&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::inlineMessageId::ID: {
    auto& typed = static_cast<td::td_api::inlineMessageId&>(value);
    wipe_native_value(typed.id_, observer);
    return;
}
case td::td_api::inlineQueryResultAnimation::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultAnimation&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::inlineQueryResultArticle::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultArticle&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    return;
}
case td::td_api::inlineQueryResultAudio::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultAudio&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.audio_, observer);
    return;
}
case td::td_api::inlineQueryResultContact::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultContact&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.contact_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    return;
}
case td::td_api::inlineQueryResultDocument::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultDocument&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::inlineQueryResultGame::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultGame&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.game_, observer);
    return;
}
case td::td_api::inlineQueryResultLocation::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultLocation&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    return;
}
case td::td_api::inlineQueryResultPhoto::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultPhoto&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::inlineQueryResultSticker::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultSticker&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::inlineQueryResultVenue::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultVenue&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.venue_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    return;
}
case td::td_api::inlineQueryResultVideo::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultVideo&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::inlineQueryResultVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultVoiceNote&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.voice_note_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::inlineQueryResults::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResults&>(value);
    wipe_native_value(typed.inline_query_id_, observer);
    wipe_native_value(typed.button_, observer);
    wipe_native_value(typed.results_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::inlineQueryResultsButton::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultsButton&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::inlineQueryResultsButtonTypeStartBot::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultsButtonTypeStartBot&>(value);
    wipe_native_value(typed.parameter_, observer);
    return;
}
case td::td_api::inlineQueryResultsButtonTypeWebApp::ID: {
    auto& typed = static_cast<td::td_api::inlineQueryResultsButtonTypeWebApp&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::inputAnimation::ID: {
    auto& typed = static_cast<td::td_api::inputAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.added_sticker_file_ids_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::inputAudio::ID: {
    auto& typed = static_cast<td::td_api::inputAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.album_cover_thumbnail_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.performer_, observer);
    return;
}
case td::td_api::inputBackgroundLocal::ID: {
    auto& typed = static_cast<td::td_api::inputBackgroundLocal&>(value);
    wipe_native_value(typed.background_, observer);
    return;
}
case td::td_api::inputBackgroundPrevious::ID: {
    auto& typed = static_cast<td::td_api::inputBackgroundPrevious&>(value);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::inputBackgroundRemote::ID: {
    auto& typed = static_cast<td::td_api::inputBackgroundRemote&>(value);
    wipe_native_value(typed.background_id_, observer);
    return;
}
case td::td_api::inputBusinessChatLink::ID: {
    auto& typed = static_cast<td::td_api::inputBusinessChatLink&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::inputBusinessStartPage::ID: {
    auto& typed = static_cast<td::td_api::inputBusinessStartPage&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::inputCallDiscarded::ID: {
    auto& typed = static_cast<td::td_api::inputCallDiscarded&>(value);
    wipe_native_value(typed.call_id_, observer);
    return;
}
case td::td_api::inputCallFromMessage::ID: {
    auto& typed = static_cast<td::td_api::inputCallFromMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::inputChatPhotoAnimation::ID: {
    auto& typed = static_cast<td::td_api::inputChatPhotoAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.main_frame_timestamp_, observer);
    return;
}
case td::td_api::inputChatPhotoPrevious::ID: {
    auto& typed = static_cast<td::td_api::inputChatPhotoPrevious&>(value);
    wipe_native_value(typed.chat_photo_id_, observer);
    return;
}
case td::td_api::inputChatPhotoStatic::ID: {
    auto& typed = static_cast<td::td_api::inputChatPhotoStatic&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::inputChatPhotoSticker::ID: {
    auto& typed = static_cast<td::td_api::inputChatPhotoSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::inputChatThemeEmoji::ID: {
    auto& typed = static_cast<td::td_api::inputChatThemeEmoji&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::inputChatThemeGift::ID: {
    auto& typed = static_cast<td::td_api::inputChatThemeGift&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::inputChecklist::ID: {
    auto& typed = static_cast<td::td_api::inputChecklist&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.tasks_, observer);
    wipe_native_value(typed.others_can_add_tasks_, observer);
    wipe_native_value(typed.others_can_mark_tasks_as_done_, observer);
    return;
}
case td::td_api::inputChecklistTask::ID: {
    auto& typed = static_cast<td::td_api::inputChecklistTask&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::inputCredentialsApplePay::ID: {
    auto& typed = static_cast<td::td_api::inputCredentialsApplePay&>(value);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::inputCredentialsGooglePay::ID: {
    auto& typed = static_cast<td::td_api::inputCredentialsGooglePay&>(value);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::inputCredentialsNew::ID: {
    auto& typed = static_cast<td::td_api::inputCredentialsNew&>(value);
    wipe_native_value(typed.data_, observer);
    wipe_native_value(typed.allow_save_, observer);
    return;
}
case td::td_api::inputCredentialsSaved::ID: {
    auto& typed = static_cast<td::td_api::inputCredentialsSaved&>(value);
    wipe_native_value(typed.saved_credentials_id_, observer);
    return;
}
case td::td_api::inputDocument::ID: {
    auto& typed = static_cast<td::td_api::inputDocument&>(value);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.disable_content_type_detection_, observer);
    return;
}
case td::td_api::inputFileGenerated::ID: {
    auto& typed = static_cast<td::td_api::inputFileGenerated&>(value);
    wipe_native_value(typed.original_path_, observer);
    wipe_native_value(typed.conversion_, observer);
    wipe_native_value(typed.expected_size_, observer);
    return;
}
case td::td_api::inputFileId::ID: {
    auto& typed = static_cast<td::td_api::inputFileId&>(value);
    wipe_native_value(typed.id_, observer);
    return;
}
case td::td_api::inputFileLocal::ID: {
    auto& typed = static_cast<td::td_api::inputFileLocal&>(value);
    wipe_native_value(typed.path_, observer);
    return;
}
case td::td_api::inputFileRemote::ID: {
    auto& typed = static_cast<td::td_api::inputFileRemote&>(value);
    wipe_native_value(typed.id_, observer);
    return;
}
case td::td_api::inputGroupCallLink::ID: {
    auto& typed = static_cast<td::td_api::inputGroupCallLink&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::inputGroupCallMessage::ID: {
    auto& typed = static_cast<td::td_api::inputGroupCallMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::inputIdentityDocument::ID: {
    auto& typed = static_cast<td::td_api::inputIdentityDocument&>(value);
    wipe_native_value(typed.number_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.front_side_, observer);
    wipe_native_value(typed.reverse_side_, observer);
    wipe_native_value(typed.selfie_, observer);
    wipe_native_value(typed.translation_, observer);
    return;
}
case td::td_api::inputInlineQueryResultAnimation::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultAnimation&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.thumbnail_mime_type_, observer);
    wipe_native_value(typed.video_url_, observer);
    wipe_native_value(typed.video_mime_type_, observer);
    wipe_native_value(typed.video_duration_, observer);
    wipe_native_value(typed.video_width_, observer);
    wipe_native_value(typed.video_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultArticle::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultArticle&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.thumbnail_width_, observer);
    wipe_native_value(typed.thumbnail_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultAudio::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultAudio&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.performer_, observer);
    wipe_native_value(typed.audio_url_, observer);
    wipe_native_value(typed.audio_duration_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultContact::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultContact&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.contact_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.thumbnail_width_, observer);
    wipe_native_value(typed.thumbnail_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultDocument::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultDocument&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.document_url_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.thumbnail_width_, observer);
    wipe_native_value(typed.thumbnail_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultGame::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultGame&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.game_short_name_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::inputInlineQueryResultLocation::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultLocation&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.live_period_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.thumbnail_width_, observer);
    wipe_native_value(typed.thumbnail_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultPhoto::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultPhoto&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.photo_url_, observer);
    wipe_native_value(typed.photo_width_, observer);
    wipe_native_value(typed.photo_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultSticker::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultSticker&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.sticker_url_, observer);
    wipe_native_value(typed.sticker_width_, observer);
    wipe_native_value(typed.sticker_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultVenue::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultVenue&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.venue_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.thumbnail_width_, observer);
    wipe_native_value(typed.thumbnail_height_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultVideo::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultVideo&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.thumbnail_url_, observer);
    wipe_native_value(typed.video_url_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.video_width_, observer);
    wipe_native_value(typed.video_height_, observer);
    wipe_native_value(typed.video_duration_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInlineQueryResultVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::inputInlineQueryResultVoiceNote&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.voice_note_url_, observer);
    wipe_native_value(typed.voice_note_duration_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::inputInvoiceMessage::ID: {
    auto& typed = static_cast<td::td_api::inputInvoiceMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::inputInvoiceName::ID: {
    auto& typed = static_cast<td::td_api::inputInvoiceName&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::inputInvoiceTelegram::ID: {
    auto& typed = static_cast<td::td_api::inputInvoiceTelegram&>(value);
    wipe_native_value(typed.purpose_, observer);
    return;
}
case td::td_api::inputMessageAnimation::ID: {
    auto& typed = static_cast<td::td_api::inputMessageAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    return;
}
case td::td_api::inputMessageAudio::ID: {
    auto& typed = static_cast<td::td_api::inputMessageAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::inputMessageChecklist::ID: {
    auto& typed = static_cast<td::td_api::inputMessageChecklist&>(value);
    wipe_native_value(typed.checklist_, observer);
    return;
}
case td::td_api::inputMessageContact::ID: {
    auto& typed = static_cast<td::td_api::inputMessageContact&>(value);
    wipe_native_value(typed.contact_, observer);
    return;
}
case td::td_api::inputMessageDice::ID: {
    auto& typed = static_cast<td::td_api::inputMessageDice&>(value);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.clear_draft_, observer);
    return;
}
case td::td_api::inputMessageDocument::ID: {
    auto& typed = static_cast<td::td_api::inputMessageDocument&>(value);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::inputMessageForwarded::ID: {
    auto& typed = static_cast<td::td_api::inputMessageForwarded&>(value);
    wipe_native_value(typed.from_chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.in_game_share_, observer);
    wipe_native_value(typed.replace_video_start_timestamp_, observer);
    wipe_native_value(typed.new_video_start_timestamp_, observer);
    wipe_native_value(typed.copy_options_, observer);
    return;
}
case td::td_api::inputMessageGame::ID: {
    auto& typed = static_cast<td::td_api::inputMessageGame&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.game_short_name_, observer);
    return;
}
case td::td_api::inputMessageInvoice::ID: {
    auto& typed = static_cast<td::td_api::inputMessageInvoice&>(value);
    wipe_native_value(typed.invoice_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.photo_url_, observer);
    wipe_native_value(typed.photo_size_, observer);
    wipe_native_value(typed.photo_width_, observer);
    wipe_native_value(typed.photo_height_, observer);
    wipe_native_value(typed.payload_, observer);
    wipe_native_value(typed.provider_token_, observer);
    wipe_native_value(typed.provider_data_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.paid_media_, observer);
    wipe_native_value(typed.paid_media_caption_, observer);
    return;
}
case td::td_api::inputMessageLiveLocation::ID: {
    auto& typed = static_cast<td::td_api::inputMessageLiveLocation&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::inputMessageLocation::ID: {
    auto& typed = static_cast<td::td_api::inputMessageLocation&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::inputMessagePaidMedia::ID: {
    auto& typed = static_cast<td::td_api::inputMessagePaidMedia&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.paid_media_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::inputMessagePhoto::ID: {
    auto& typed = static_cast<td::td_api::inputMessagePhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    return;
}
case td::td_api::inputMessagePoll::ID: {
    auto& typed = static_cast<td::td_api::inputMessagePoll&>(value);
    wipe_native_value(typed.question_, observer);
    wipe_native_value(typed.options_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.is_anonymous_, observer);
    wipe_native_value(typed.allows_multiple_answers_, observer);
    wipe_native_value(typed.allows_revoting_, observer);
    wipe_native_value(typed.members_only_, observer);
    wipe_native_value(typed.country_codes_, observer);
    wipe_native_value(typed.shuffle_options_, observer);
    wipe_native_value(typed.hide_results_until_closes_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.open_period_, observer);
    wipe_native_value(typed.close_date_, observer);
    wipe_native_value(typed.is_closed_, observer);
    return;
}
case td::td_api::inputMessageReplyToExternalMessage::ID: {
    auto& typed = static_cast<td::td_api::inputMessageReplyToExternalMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.quote_, observer);
    wipe_native_value(typed.checklist_task_id_, observer);
    wipe_native_value(typed.poll_option_id_, observer);
    return;
}
case td::td_api::inputMessageReplyToMessage::ID: {
    auto& typed = static_cast<td::td_api::inputMessageReplyToMessage&>(value);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.quote_, observer);
    wipe_native_value(typed.checklist_task_id_, observer);
    wipe_native_value(typed.poll_option_id_, observer);
    return;
}
case td::td_api::inputMessageReplyToStory::ID: {
    auto& typed = static_cast<td::td_api::inputMessageReplyToStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::inputMessageRichMessage::ID: {
    auto& typed = static_cast<td::td_api::inputMessageRichMessage&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.clear_draft_, observer);
    return;
}
case td::td_api::inputMessageStakeDice::ID: {
    auto& typed = static_cast<td::td_api::inputMessageStakeDice&>(value);
    wipe_native_value(typed.state_hash_, observer);
    wipe_native_value(typed.stake_toncoin_amount_, observer);
    wipe_native_value(typed.clear_draft_, observer);
    return;
}
case td::td_api::inputMessageSticker::ID: {
    auto& typed = static_cast<td::td_api::inputMessageSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::inputMessageStory::ID: {
    auto& typed = static_cast<td::td_api::inputMessageStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::inputMessageText::ID: {
    auto& typed = static_cast<td::td_api::inputMessageText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.link_preview_options_, observer);
    wipe_native_value(typed.clear_draft_, observer);
    return;
}
case td::td_api::inputMessageVenue::ID: {
    auto& typed = static_cast<td::td_api::inputMessageVenue&>(value);
    wipe_native_value(typed.venue_, observer);
    return;
}
case td::td_api::inputMessageVideo::ID: {
    auto& typed = static_cast<td::td_api::inputMessageVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    return;
}
case td::td_api::inputMessageVideoNote::ID: {
    auto& typed = static_cast<td::td_api::inputMessageVideoNote&>(value);
    wipe_native_value(typed.video_note_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.length_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    return;
}
case td::td_api::inputMessageVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::inputMessageVoiceNote&>(value);
    wipe_native_value(typed.voice_note_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.waveform_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    return;
}
case td::td_api::inputPaidMedia::ID: {
    auto& typed = static_cast<td::td_api::inputPaidMedia&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.added_sticker_file_ids_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::inputPaidMediaTypePhoto::ID: {
    auto& typed = static_cast<td::td_api::inputPaidMediaTypePhoto&>(value);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::inputPaidMediaTypeVideo::ID: {
    auto& typed = static_cast<td::td_api::inputPaidMediaTypeVideo&>(value);
    wipe_native_value(typed.cover_, observer);
    wipe_native_value(typed.start_timestamp_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.supports_streaming_, observer);
    return;
}
case td::td_api::inputPassportElementAddress::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementAddress&>(value);
    wipe_native_value(typed.address_, observer);
    return;
}
case td::td_api::inputPassportElementBankStatement::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementBankStatement&>(value);
    wipe_native_value(typed.bank_statement_, observer);
    return;
}
case td::td_api::inputPassportElementDriverLicense::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementDriverLicense&>(value);
    wipe_native_value(typed.driver_license_, observer);
    return;
}
case td::td_api::inputPassportElementEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementEmailAddress&>(value);
    wipe_native_value(typed.email_address_, observer);
    return;
}
case td::td_api::inputPassportElementError::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementError&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.source_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceDataField::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceDataField&>(value);
    wipe_native_value(typed.field_name_, observer);
    wipe_native_value(typed.data_hash_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceFile::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceFile&>(value);
    wipe_native_value(typed.file_hash_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceFiles::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceFiles&>(value);
    wipe_native_value(typed.file_hashes_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceFrontSide::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceFrontSide&>(value);
    wipe_native_value(typed.file_hash_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceReverseSide::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceReverseSide&>(value);
    wipe_native_value(typed.file_hash_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceSelfie::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceSelfie&>(value);
    wipe_native_value(typed.file_hash_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceTranslationFile::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceTranslationFile&>(value);
    wipe_native_value(typed.file_hash_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceTranslationFiles::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceTranslationFiles&>(value);
    wipe_native_value(typed.file_hashes_, observer);
    return;
}
case td::td_api::inputPassportElementErrorSourceUnspecified::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementErrorSourceUnspecified&>(value);
    wipe_native_value(typed.element_hash_, observer);
    return;
}
case td::td_api::inputPassportElementIdentityCard::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementIdentityCard&>(value);
    wipe_native_value(typed.identity_card_, observer);
    return;
}
case td::td_api::inputPassportElementInternalPassport::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementInternalPassport&>(value);
    wipe_native_value(typed.internal_passport_, observer);
    return;
}
case td::td_api::inputPassportElementPassport::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementPassport&>(value);
    wipe_native_value(typed.passport_, observer);
    return;
}
case td::td_api::inputPassportElementPassportRegistration::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementPassportRegistration&>(value);
    wipe_native_value(typed.passport_registration_, observer);
    return;
}
case td::td_api::inputPassportElementPersonalDetails::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementPersonalDetails&>(value);
    wipe_native_value(typed.personal_details_, observer);
    return;
}
case td::td_api::inputPassportElementPhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementPhoneNumber&>(value);
    wipe_native_value(typed.phone_number_, observer);
    return;
}
case td::td_api::inputPassportElementRentalAgreement::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementRentalAgreement&>(value);
    wipe_native_value(typed.rental_agreement_, observer);
    return;
}
case td::td_api::inputPassportElementTemporaryRegistration::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementTemporaryRegistration&>(value);
    wipe_native_value(typed.temporary_registration_, observer);
    return;
}
case td::td_api::inputPassportElementUtilityBill::ID: {
    auto& typed = static_cast<td::td_api::inputPassportElementUtilityBill&>(value);
    wipe_native_value(typed.utility_bill_, observer);
    return;
}
case td::td_api::inputPersonalDocument::ID: {
    auto& typed = static_cast<td::td_api::inputPersonalDocument&>(value);
    wipe_native_value(typed.files_, observer);
    wipe_native_value(typed.translation_, observer);
    return;
}
case td::td_api::inputPhoto::ID: {
    auto& typed = static_cast<td::td_api::inputPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.added_sticker_file_ids_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::inputPollMediaAnimation::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::inputPollMediaAudio::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    return;
}
case td::td_api::inputPollMediaDocument::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaDocument&>(value);
    wipe_native_value(typed.document_, observer);
    return;
}
case td::td_api::inputPollMediaLink::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaLink&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::inputPollMediaLocation::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaLocation&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::inputPollMediaPhoto::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::inputPollMediaSticker::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::inputPollMediaVenue::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaVenue&>(value);
    wipe_native_value(typed.venue_, observer);
    return;
}
case td::td_api::inputPollMediaVideo::ID: {
    auto& typed = static_cast<td::td_api::inputPollMediaVideo&>(value);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::inputPollOption::ID: {
    auto& typed = static_cast<td::td_api::inputPollOption&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.media_, observer);
    return;
}
case td::td_api::inputPollTypeQuiz::ID: {
    auto& typed = static_cast<td::td_api::inputPollTypeQuiz&>(value);
    wipe_native_value(typed.correct_option_ids_, observer);
    wipe_native_value(typed.explanation_, observer);
    wipe_native_value(typed.explanation_media_, observer);
    return;
}
case td::td_api::inputPollTypeRegular::ID: {
    auto& typed = static_cast<td::td_api::inputPollTypeRegular&>(value);
    wipe_native_value(typed.allow_adding_options_, observer);
    return;
}
case td::td_api::inputRichMessage::ID: {
    auto& typed = static_cast<td::td_api::inputRichMessage&>(value);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.is_rtl_, observer);
    wipe_native_value(typed.detect_automatic_blocks_, observer);
    return;
}
case td::td_api::inputSticker::ID: {
    auto& typed = static_cast<td::td_api::inputSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.format_, observer);
    wipe_native_value(typed.emojis_, observer);
    wipe_native_value(typed.mask_position_, observer);
    wipe_native_value(typed.keywords_, observer);
    return;
}
case td::td_api::inputStoryArea::ID: {
    auto& typed = static_cast<td::td_api::inputStoryArea&>(value);
    wipe_native_value(typed.position_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeFoundVenue::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeFoundVenue&>(value);
    wipe_native_value(typed.query_id_, observer);
    wipe_native_value(typed.result_id_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeLink::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeLink&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeLocation::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeLocation&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.address_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeMessage::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::inputStoryAreaTypePreviousVenue::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypePreviousVenue&>(value);
    wipe_native_value(typed.venue_provider_, observer);
    wipe_native_value(typed.venue_id_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeSuggestedReaction::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeSuggestedReaction&>(value);
    wipe_native_value(typed.reaction_type_, observer);
    wipe_native_value(typed.is_dark_, observer);
    wipe_native_value(typed.is_flipped_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeUpgradedGift&>(value);
    wipe_native_value(typed.gift_name_, observer);
    return;
}
case td::td_api::inputStoryAreaTypeWeather::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreaTypeWeather&>(value);
    wipe_native_value(typed.temperature_, observer);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.background_color_, observer);
    return;
}
case td::td_api::inputStoryAreas::ID: {
    auto& typed = static_cast<td::td_api::inputStoryAreas&>(value);
    wipe_native_value(typed.areas_, observer);
    return;
}
case td::td_api::inputStoryContentPhoto::ID: {
    auto& typed = static_cast<td::td_api::inputStoryContentPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.added_sticker_file_ids_, observer);
    return;
}
case td::td_api::inputStoryContentVideo::ID: {
    auto& typed = static_cast<td::td_api::inputStoryContentVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.added_sticker_file_ids_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.cover_frame_timestamp_, observer);
    wipe_native_value(typed.is_animation_, observer);
    return;
}
case td::td_api::inputSuggestedPostInfo::ID: {
    auto& typed = static_cast<td::td_api::inputSuggestedPostInfo&>(value);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.send_date_, observer);
    return;
}
case td::td_api::inputTextQuote::ID: {
    auto& typed = static_cast<td::td_api::inputTextQuote&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.position_, observer);
    return;
}
case td::td_api::inputThumbnail::ID: {
    auto& typed = static_cast<td::td_api::inputThumbnail&>(value);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::inputVideo::ID: {
    auto& typed = static_cast<td::td_api::inputVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.cover_, observer);
    wipe_native_value(typed.start_timestamp_, observer);
    wipe_native_value(typed.added_sticker_file_ids_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.supports_streaming_, observer);
    return;
}
case td::td_api::internalLinkTypeAttachmentMenuBot::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeAttachmentMenuBot&>(value);
    wipe_native_value(typed.target_chat_, observer);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::internalLinkTypeAuthenticationCode::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeAuthenticationCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::internalLinkTypeBackground::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeBackground&>(value);
    wipe_native_value(typed.background_name_, observer);
    return;
}
case td::td_api::internalLinkTypeBotAddToChannel::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeBotAddToChannel&>(value);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.administrator_rights_, observer);
    return;
}
case td::td_api::internalLinkTypeBotStart::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeBotStart&>(value);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.autostart_, observer);
    return;
}
case td::td_api::internalLinkTypeBotStartInGroup::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeBotStartInGroup&>(value);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.administrator_rights_, observer);
    return;
}
case td::td_api::internalLinkTypeBusinessChat::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeBusinessChat&>(value);
    wipe_native_value(typed.link_name_, observer);
    return;
}
case td::td_api::internalLinkTypeCallsPage::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeCallsPage&>(value);
    wipe_native_value(typed.section_, observer);
    return;
}
case td::td_api::internalLinkTypeChatAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeChatAffiliateProgram&>(value);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.referrer_, observer);
    return;
}
case td::td_api::internalLinkTypeChatBoost::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeChatBoost&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::internalLinkTypeChatFolderInvite::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeChatFolderInvite&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::internalLinkTypeChatInvite::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeChatInvite&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::internalLinkTypeChatSelection::ID: {
    return;
}
case td::td_api::internalLinkTypeContactsPage::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeContactsPage&>(value);
    wipe_native_value(typed.section_, observer);
    return;
}
case td::td_api::internalLinkTypeDirectMessagesChat::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeDirectMessagesChat&>(value);
    wipe_native_value(typed.channel_username_, observer);
    return;
}
case td::td_api::internalLinkTypeGame::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeGame&>(value);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.game_short_name_, observer);
    return;
}
case td::td_api::internalLinkTypeGiftAuction::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeGiftAuction&>(value);
    wipe_native_value(typed.auction_id_, observer);
    return;
}
case td::td_api::internalLinkTypeGiftCollection::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeGiftCollection&>(value);
    wipe_native_value(typed.gift_owner_username_, observer);
    wipe_native_value(typed.collection_id_, observer);
    return;
}
case td::td_api::internalLinkTypeGroupCall::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeGroupCall&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::internalLinkTypeInstantView::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeInstantView&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.fallback_url_, observer);
    return;
}
case td::td_api::internalLinkTypeInvoice::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeInvoice&>(value);
    wipe_native_value(typed.invoice_name_, observer);
    return;
}
case td::td_api::internalLinkTypeLanguagePack::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeLanguagePack&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    return;
}
case td::td_api::internalLinkTypeLiveStory::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeLiveStory&>(value);
    wipe_native_value(typed.story_poster_username_, observer);
    return;
}
case td::td_api::internalLinkTypeMainWebApp::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeMainWebApp&>(value);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.mode_, observer);
    return;
}
case td::td_api::internalLinkTypeMessage::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeMessage&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::internalLinkTypeMessageDraft::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeMessageDraft&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.contains_link_, observer);
    return;
}
case td::td_api::internalLinkTypeMyProfilePage::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeMyProfilePage&>(value);
    wipe_native_value(typed.section_, observer);
    return;
}
case td::td_api::internalLinkTypeNewChannelChat::ID: {
    return;
}
case td::td_api::internalLinkTypeNewGroupChat::ID: {
    return;
}
case td::td_api::internalLinkTypeNewPrivateChat::ID: {
    return;
}
case td::td_api::internalLinkTypeNewStory::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeNewStory&>(value);
    wipe_native_value(typed.content_type_, observer);
    return;
}
case td::td_api::internalLinkTypeOauth::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeOauth&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::internalLinkTypePassportDataRequest::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypePassportDataRequest&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.public_key_, observer);
    wipe_native_value(typed.nonce_, observer);
    wipe_native_value(typed.callback_url_, observer);
    return;
}
case td::td_api::internalLinkTypePhoneNumberConfirmation::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypePhoneNumberConfirmation&>(value);
    wipe_native_value(typed.hash_, observer);
    wipe_native_value(typed.phone_number_, observer);
    return;
}
case td::td_api::internalLinkTypePremiumFeaturesPage::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypePremiumFeaturesPage&>(value);
    wipe_native_value(typed.referrer_, observer);
    return;
}
case td::td_api::internalLinkTypePremiumGiftCode::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypePremiumGiftCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::internalLinkTypePremiumGiftPurchase::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypePremiumGiftPurchase&>(value);
    wipe_native_value(typed.referrer_, observer);
    return;
}
case td::td_api::internalLinkTypeProxy::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeProxy&>(value);
    wipe_native_value(typed.proxy_, observer);
    return;
}
case td::td_api::internalLinkTypePublicChat::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypePublicChat&>(value);
    wipe_native_value(typed.chat_username_, observer);
    wipe_native_value(typed.draft_text_, observer);
    wipe_native_value(typed.open_profile_, observer);
    return;
}
case td::td_api::internalLinkTypeQrCodeAuthentication::ID: {
    return;
}
case td::td_api::internalLinkTypeRequestManagedBot::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeRequestManagedBot&>(value);
    wipe_native_value(typed.manager_bot_username_, observer);
    wipe_native_value(typed.suggested_bot_username_, observer);
    wipe_native_value(typed.suggested_bot_name_, observer);
    return;
}
case td::td_api::internalLinkTypeRestorePurchases::ID: {
    return;
}
case td::td_api::internalLinkTypeSavedMessages::ID: {
    return;
}
case td::td_api::internalLinkTypeSearch::ID: {
    return;
}
case td::td_api::internalLinkTypeSettings::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeSettings&>(value);
    wipe_native_value(typed.section_, observer);
    return;
}
case td::td_api::internalLinkTypeStarPurchase::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeStarPurchase&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.purpose_, observer);
    return;
}
case td::td_api::internalLinkTypeStickerSet::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeStickerSet&>(value);
    wipe_native_value(typed.sticker_set_name_, observer);
    wipe_native_value(typed.expect_custom_emoji_, observer);
    return;
}
case td::td_api::internalLinkTypeStory::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeStory&>(value);
    wipe_native_value(typed.story_poster_username_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::internalLinkTypeStoryAlbum::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeStoryAlbum&>(value);
    wipe_native_value(typed.story_album_owner_username_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    return;
}
case td::td_api::internalLinkTypeTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeTextCompositionStyle&>(value);
    wipe_native_value(typed.style_name_, observer);
    return;
}
case td::td_api::internalLinkTypeTheme::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeTheme&>(value);
    wipe_native_value(typed.theme_name_, observer);
    return;
}
case td::td_api::internalLinkTypeUnknownDeepLink::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeUnknownDeepLink&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::internalLinkTypeUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeUpgradedGift&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::internalLinkTypeUserPhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeUserPhoneNumber&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.draft_text_, observer);
    wipe_native_value(typed.open_profile_, observer);
    return;
}
case td::td_api::internalLinkTypeUserToken::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeUserToken&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::internalLinkTypeVideoChat::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeVideoChat&>(value);
    wipe_native_value(typed.chat_username_, observer);
    wipe_native_value(typed.invite_hash_, observer);
    wipe_native_value(typed.is_live_stream_, observer);
    return;
}
case td::td_api::internalLinkTypeWebApp::ID: {
    auto& typed = static_cast<td::td_api::internalLinkTypeWebApp&>(value);
    wipe_native_value(typed.bot_username_, observer);
    wipe_native_value(typed.web_app_short_name_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.mode_, observer);
    return;
}
case td::td_api::inviteGroupCallParticipantResultSuccess::ID: {
    auto& typed = static_cast<td::td_api::inviteGroupCallParticipantResultSuccess&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::inviteGroupCallParticipantResultUserAlreadyParticipant::ID: {
    return;
}
case td::td_api::inviteGroupCallParticipantResultUserPrivacyRestricted::ID: {
    return;
}
case td::td_api::inviteGroupCallParticipantResultUserWasBanned::ID: {
    return;
}
case td::td_api::inviteLinkChatTypeBasicGroup::ID: {
    return;
}
case td::td_api::inviteLinkChatTypeChannel::ID: {
    return;
}
case td::td_api::inviteLinkChatTypeSupergroup::ID: {
    return;
}
case td::td_api::invoice::ID: {
    auto& typed = static_cast<td::td_api::invoice&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.price_parts_, observer);
    wipe_native_value(typed.subscription_period_, observer);
    wipe_native_value(typed.max_tip_amount_, observer);
    wipe_native_value(typed.suggested_tip_amounts_, observer);
    wipe_native_value(typed.recurring_payment_terms_of_service_url_, observer);
    wipe_native_value(typed.terms_of_service_url_, observer);
    wipe_native_value(typed.is_test_, observer);
    wipe_native_value(typed.need_name_, observer);
    wipe_native_value(typed.need_phone_number_, observer);
    wipe_native_value(typed.need_email_address_, observer);
    wipe_native_value(typed.need_shipping_address_, observer);
    wipe_native_value(typed.send_phone_number_to_provider_, observer);
    wipe_native_value(typed.send_email_address_to_provider_, observer);
    wipe_native_value(typed.is_flexible_, observer);
    return;
}
case td::td_api::jsonObjectMember::ID: {
    auto& typed = static_cast<td::td_api::jsonObjectMember&>(value);
    wipe_native_value(typed.key_, observer);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::jsonValueArray::ID: {
    auto& typed = static_cast<td::td_api::jsonValueArray&>(value);
    wipe_native_value(typed.values_, observer);
    return;
}
case td::td_api::jsonValueBoolean::ID: {
    auto& typed = static_cast<td::td_api::jsonValueBoolean&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::jsonValueNull::ID: {
    return;
}
case td::td_api::jsonValueNumber::ID: {
    auto& typed = static_cast<td::td_api::jsonValueNumber&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::jsonValueObject::ID: {
    auto& typed = static_cast<td::td_api::jsonValueObject&>(value);
    wipe_native_value(typed.members_, observer);
    return;
}
case td::td_api::jsonValueString::ID: {
    auto& typed = static_cast<td::td_api::jsonValueString&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::keyboardButton::ID: {
    auto& typed = static_cast<td::td_api::keyboardButton&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.icon_custom_emoji_id_, observer);
    wipe_native_value(typed.style_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::keyboardButtonSourceMessage::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonSourceMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::keyboardButtonSourceWebApp::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonSourceWebApp&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.prepared_button_id_, observer);
    return;
}
case td::td_api::keyboardButtonTypeRequestChat::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonTypeRequestChat&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.chat_is_channel_, observer);
    wipe_native_value(typed.restrict_chat_is_forum_, observer);
    wipe_native_value(typed.chat_is_forum_, observer);
    wipe_native_value(typed.restrict_chat_has_username_, observer);
    wipe_native_value(typed.chat_has_username_, observer);
    wipe_native_value(typed.chat_is_created_, observer);
    wipe_native_value(typed.user_administrator_rights_, observer);
    wipe_native_value(typed.bot_administrator_rights_, observer);
    wipe_native_value(typed.bot_is_member_, observer);
    wipe_native_value(typed.request_title_, observer);
    wipe_native_value(typed.request_username_, observer);
    wipe_native_value(typed.request_photo_, observer);
    return;
}
case td::td_api::keyboardButtonTypeRequestLocation::ID: {
    return;
}
case td::td_api::keyboardButtonTypeRequestManagedBot::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonTypeRequestManagedBot&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.suggested_name_, observer);
    wipe_native_value(typed.suggested_username_, observer);
    return;
}
case td::td_api::keyboardButtonTypeRequestPhoneNumber::ID: {
    return;
}
case td::td_api::keyboardButtonTypeRequestPoll::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonTypeRequestPoll&>(value);
    wipe_native_value(typed.force_regular_, observer);
    wipe_native_value(typed.force_quiz_, observer);
    return;
}
case td::td_api::keyboardButtonTypeRequestUsers::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonTypeRequestUsers&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.restrict_user_is_bot_, observer);
    wipe_native_value(typed.user_is_bot_, observer);
    wipe_native_value(typed.restrict_user_is_premium_, observer);
    wipe_native_value(typed.user_is_premium_, observer);
    wipe_native_value(typed.max_quantity_, observer);
    wipe_native_value(typed.request_name_, observer);
    wipe_native_value(typed.request_username_, observer);
    wipe_native_value(typed.request_photo_, observer);
    return;
}
case td::td_api::keyboardButtonTypeText::ID: {
    return;
}
case td::td_api::keyboardButtonTypeWebApp::ID: {
    auto& typed = static_cast<td::td_api::keyboardButtonTypeWebApp&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::labeledPricePart::ID: {
    auto& typed = static_cast<td::td_api::labeledPricePart&>(value);
    wipe_native_value(typed.label_, observer);
    wipe_native_value(typed.amount_, observer);
    return;
}
case td::td_api::languagePackInfo::ID: {
    auto& typed = static_cast<td::td_api::languagePackInfo&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.base_language_pack_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.native_name_, observer);
    wipe_native_value(typed.plural_code_, observer);
    wipe_native_value(typed.is_official_, observer);
    wipe_native_value(typed.is_rtl_, observer);
    wipe_native_value(typed.is_beta_, observer);
    wipe_native_value(typed.is_installed_, observer);
    wipe_native_value(typed.total_string_count_, observer);
    wipe_native_value(typed.translated_string_count_, observer);
    wipe_native_value(typed.local_string_count_, observer);
    wipe_native_value(typed.translation_url_, observer);
    return;
}
case td::td_api::languagePackString::ID: {
    auto& typed = static_cast<td::td_api::languagePackString&>(value);
    wipe_native_value(typed.key_, observer);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::languagePackStringValueDeleted::ID: {
    return;
}
case td::td_api::languagePackStringValueOrdinary::ID: {
    auto& typed = static_cast<td::td_api::languagePackStringValueOrdinary&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::languagePackStringValuePluralized::ID: {
    auto& typed = static_cast<td::td_api::languagePackStringValuePluralized&>(value);
    wipe_native_value(typed.zero_value_, observer);
    wipe_native_value(typed.one_value_, observer);
    wipe_native_value(typed.two_value_, observer);
    wipe_native_value(typed.few_value_, observer);
    wipe_native_value(typed.many_value_, observer);
    wipe_native_value(typed.other_value_, observer);
    return;
}
case td::td_api::languagePackStrings::ID: {
    auto& typed = static_cast<td::td_api::languagePackStrings&>(value);
    wipe_native_value(typed.strings_, observer);
    return;
}
case td::td_api::linkPreview::ID: {
    auto& typed = static_cast<td::td_api::linkPreview&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.display_url_, observer);
    wipe_native_value(typed.site_name_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.author_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.has_large_media_, observer);
    wipe_native_value(typed.show_large_media_, observer);
    wipe_native_value(typed.show_media_above_description_, observer);
    wipe_native_value(typed.skip_confirmation_, observer);
    wipe_native_value(typed.show_above_text_, observer);
    wipe_native_value(typed.instant_view_version_, observer);
    return;
}
case td::td_api::linkPreviewAlbumMediaPhoto::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewAlbumMediaPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewAlbumMediaVideo::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewAlbumMediaVideo&>(value);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::linkPreviewOptions::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewOptions&>(value);
    wipe_native_value(typed.is_disabled_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.force_small_media_, observer);
    wipe_native_value(typed.force_large_media_, observer);
    wipe_native_value(typed.show_above_text_, observer);
    return;
}
case td::td_api::linkPreviewTypeAlbum::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeAlbum&>(value);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::linkPreviewTypeAnimation::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::linkPreviewTypeApp::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeApp&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewTypeArticle::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeArticle&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewTypeAudio::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    return;
}
case td::td_api::linkPreviewTypeBackground::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeBackground&>(value);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.background_type_, observer);
    return;
}
case td::td_api::linkPreviewTypeChannelBoost::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeChannelBoost&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewTypeChat::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeChat&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.creates_join_request_, observer);
    return;
}
case td::td_api::linkPreviewTypeDirectMessagesChat::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeDirectMessagesChat&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewTypeDocument::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeDocument&>(value);
    wipe_native_value(typed.document_, observer);
    return;
}
case td::td_api::linkPreviewTypeEmbeddedAnimationPlayer::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeEmbeddedAnimationPlayer&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::linkPreviewTypeEmbeddedAudioPlayer::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeEmbeddedAudioPlayer&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::linkPreviewTypeEmbeddedVideoPlayer::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeEmbeddedVideoPlayer&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::linkPreviewTypeExternalAudio::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeExternalAudio&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::linkPreviewTypeExternalVideo::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeExternalVideo&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::linkPreviewTypeGiftAuction::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeGiftAuction&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.auction_end_date_, observer);
    return;
}
case td::td_api::linkPreviewTypeGiftCollection::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeGiftCollection&>(value);
    wipe_native_value(typed.icons_, observer);
    return;
}
case td::td_api::linkPreviewTypeGroupCall::ID: {
    return;
}
case td::td_api::linkPreviewTypeInvoice::ID: {
    return;
}
case td::td_api::linkPreviewTypeLiveStory::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeLiveStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::linkPreviewTypeMessage::ID: {
    return;
}
case td::td_api::linkPreviewTypePhoto::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypePhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewTypePremiumGiftCode::ID: {
    return;
}
case td::td_api::linkPreviewTypeRequestManagedBot::ID: {
    return;
}
case td::td_api::linkPreviewTypeShareableChatFolder::ID: {
    return;
}
case td::td_api::linkPreviewTypeSticker::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::linkPreviewTypeStickerSet::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeStickerSet&>(value);
    wipe_native_value(typed.stickers_, observer);
    return;
}
case td::td_api::linkPreviewTypeStory::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::linkPreviewTypeStoryAlbum::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeStoryAlbum&>(value);
    wipe_native_value(typed.photo_icon_, observer);
    wipe_native_value(typed.video_icon_, observer);
    return;
}
case td::td_api::linkPreviewTypeSupergroupBoost::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeSupergroupBoost&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::linkPreviewTypeTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeTextCompositionStyle&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::linkPreviewTypeTheme::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeTheme&>(value);
    wipe_native_value(typed.documents_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::linkPreviewTypeUnsupported::ID: {
    return;
}
case td::td_api::linkPreviewTypeUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeUpgradedGift&>(value);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::linkPreviewTypeUser::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeUser&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.is_bot_, observer);
    return;
}
case td::td_api::linkPreviewTypeVideo::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.cover_, observer);
    wipe_native_value(typed.start_timestamp_, observer);
    return;
}
case td::td_api::linkPreviewTypeVideoChat::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeVideoChat&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.is_live_stream_, observer);
    wipe_native_value(typed.joins_as_speaker_, observer);
    return;
}
case td::td_api::linkPreviewTypeVideoNote::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeVideoNote&>(value);
    wipe_native_value(typed.video_note_, observer);
    return;
}
case td::td_api::linkPreviewTypeVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeVoiceNote&>(value);
    wipe_native_value(typed.voice_note_, observer);
    return;
}
case td::td_api::linkPreviewTypeWebApp::ID: {
    auto& typed = static_cast<td::td_api::linkPreviewTypeWebApp&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::liveLocation::ID: {
    auto& typed = static_cast<td::td_api::liveLocation&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.live_period_, observer);
    wipe_native_value(typed.heading_, observer);
    wipe_native_value(typed.proximity_alert_radius_, observer);
    return;
}
case td::td_api::liveStoryDonors::ID: {
    auto& typed = static_cast<td::td_api::liveStoryDonors&>(value);
    wipe_native_value(typed.total_star_count_, observer);
    wipe_native_value(typed.top_donors_, observer);
    return;
}
case td::td_api::localFile::ID: {
    auto& typed = static_cast<td::td_api::localFile&>(value);
    wipe_native_value(typed.path_, observer);
    wipe_native_value(typed.can_be_downloaded_, observer);
    wipe_native_value(typed.can_be_deleted_, observer);
    wipe_native_value(typed.is_downloading_active_, observer);
    wipe_native_value(typed.is_downloading_completed_, observer);
    wipe_native_value(typed.download_offset_, observer);
    wipe_native_value(typed.downloaded_prefix_size_, observer);
    wipe_native_value(typed.downloaded_size_, observer);
    return;
}
case td::td_api::localizationTargetInfo::ID: {
    auto& typed = static_cast<td::td_api::localizationTargetInfo&>(value);
    wipe_native_value(typed.language_packs_, observer);
    return;
}
case td::td_api::location::ID: {
    auto& typed = static_cast<td::td_api::location&>(value);
    wipe_native_value(typed.latitude_, observer);
    wipe_native_value(typed.longitude_, observer);
    wipe_native_value(typed.horizontal_accuracy_, observer);
    return;
}
case td::td_api::locationAddress::ID: {
    auto& typed = static_cast<td::td_api::locationAddress&>(value);
    wipe_native_value(typed.country_code_, observer);
    wipe_native_value(typed.state_, observer);
    wipe_native_value(typed.city_, observer);
    wipe_native_value(typed.street_, observer);
    return;
}
case td::td_api::logStreamDefault::ID: {
    return;
}
case td::td_api::logStreamEmpty::ID: {
    return;
}
case td::td_api::logStreamFile::ID: {
    auto& typed = static_cast<td::td_api::logStreamFile&>(value);
    wipe_native_value(typed.path_, observer);
    wipe_native_value(typed.max_file_size_, observer);
    wipe_native_value(typed.redirect_stderr_, observer);
    return;
}
case td::td_api::logTags::ID: {
    auto& typed = static_cast<td::td_api::logTags&>(value);
    wipe_native_value(typed.tags_, observer);
    return;
}
case td::td_api::logVerbosityLevel::ID: {
    auto& typed = static_cast<td::td_api::logVerbosityLevel&>(value);
    wipe_native_value(typed.verbosity_level_, observer);
    return;
}
case td::td_api::loginUrlInfoOpen::ID: {
    auto& typed = static_cast<td::td_api::loginUrlInfoOpen&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.skip_confirmation_, observer);
    return;
}
case td::td_api::loginUrlInfoRequestConfirmation::ID: {
    auto& typed = static_cast<td::td_api::loginUrlInfoRequestConfirmation&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.domain_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.request_write_access_, observer);
    return;
}
case td::td_api::mainWebApp::ID: {
    auto& typed = static_cast<td::td_api::mainWebApp&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.mode_, observer);
    return;
}
case td::td_api::maskPointChin::ID: {
    return;
}
case td::td_api::maskPointEyes::ID: {
    return;
}
case td::td_api::maskPointForehead::ID: {
    return;
}
case td::td_api::maskPointMouth::ID: {
    return;
}
case td::td_api::maskPosition::ID: {
    auto& typed = static_cast<td::td_api::maskPosition&>(value);
    wipe_native_value(typed.point_, observer);
    wipe_native_value(typed.x_shift_, observer);
    wipe_native_value(typed.y_shift_, observer);
    wipe_native_value(typed.scale_, observer);
    return;
}
case td::td_api::message::ID: {
    auto& typed = static_cast<td::td_api::message&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.sending_state_, observer);
    wipe_native_value(typed.scheduling_state_, observer);
    wipe_native_value(typed.is_outgoing_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    wipe_native_value(typed.is_from_offline_, observer);
    wipe_native_value(typed.can_be_saved_, observer);
    wipe_native_value(typed.has_timestamped_media_, observer);
    wipe_native_value(typed.is_channel_post_, observer);
    wipe_native_value(typed.is_paid_star_suggested_post_, observer);
    wipe_native_value(typed.is_paid_ton_suggested_post_, observer);
    wipe_native_value(typed.contains_unread_mention_, observer);
    wipe_native_value(typed.contains_unread_poll_votes_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.edit_date_, observer);
    wipe_native_value(typed.forward_info_, observer);
    wipe_native_value(typed.import_info_, observer);
    wipe_native_value(typed.interaction_info_, observer);
    wipe_native_value(typed.unread_reactions_, observer);
    wipe_native_value(typed.fact_check_, observer);
    wipe_native_value(typed.suggested_post_info_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.self_destruct_type_, observer);
    wipe_native_value(typed.self_destruct_in_, observer);
    wipe_native_value(typed.auto_delete_in_, observer);
    wipe_native_value(typed.via_bot_user_id_, observer);
    wipe_native_value(typed.guest_bot_caller_id_, observer);
    wipe_native_value(typed.sender_business_bot_user_id_, observer);
    wipe_native_value(typed.sender_boost_count_, observer);
    wipe_native_value(typed.sender_tag_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    wipe_native_value(typed.author_signature_, observer);
    wipe_native_value(typed.media_album_id_, observer);
    wipe_native_value(typed.effect_id_, observer);
    wipe_native_value(typed.restriction_info_, observer);
    wipe_native_value(typed.summary_language_code_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::messageAnimatedEmoji::ID: {
    auto& typed = static_cast<td::td_api::messageAnimatedEmoji&>(value);
    wipe_native_value(typed.animated_emoji_, observer);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::messageAnimation::ID: {
    auto& typed = static_cast<td::td_api::messageAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    wipe_native_value(typed.is_secret_, observer);
    return;
}
case td::td_api::messageAudio::ID: {
    auto& typed = static_cast<td::td_api::messageAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::messageAutoDeleteTime::ID: {
    auto& typed = static_cast<td::td_api::messageAutoDeleteTime&>(value);
    wipe_native_value(typed.time_, observer);
    return;
}
case td::td_api::messageBasicGroupChatCreate::ID: {
    auto& typed = static_cast<td::td_api::messageBasicGroupChatCreate&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.member_user_ids_, observer);
    return;
}
case td::td_api::messageBotWriteAccessAllowed::ID: {
    auto& typed = static_cast<td::td_api::messageBotWriteAccessAllowed&>(value);
    wipe_native_value(typed.reason_, observer);
    return;
}
case td::td_api::messageCalendar::ID: {
    auto& typed = static_cast<td::td_api::messageCalendar&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.days_, observer);
    return;
}
case td::td_api::messageCalendarDay::ID: {
    auto& typed = static_cast<td::td_api::messageCalendarDay&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::messageCall::ID: {
    auto& typed = static_cast<td::td_api::messageCall&>(value);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.is_video_, observer);
    wipe_native_value(typed.discard_reason_, observer);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::messageChatAddMembers::ID: {
    auto& typed = static_cast<td::td_api::messageChatAddMembers&>(value);
    wipe_native_value(typed.member_user_ids_, observer);
    return;
}
case td::td_api::messageChatBoost::ID: {
    auto& typed = static_cast<td::td_api::messageChatBoost&>(value);
    wipe_native_value(typed.boost_count_, observer);
    return;
}
case td::td_api::messageChatChangePhoto::ID: {
    auto& typed = static_cast<td::td_api::messageChatChangePhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::messageChatChangeTitle::ID: {
    auto& typed = static_cast<td::td_api::messageChatChangeTitle&>(value);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::messageChatDeleteMember::ID: {
    auto& typed = static_cast<td::td_api::messageChatDeleteMember&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::messageChatDeletePhoto::ID: {
    return;
}
case td::td_api::messageChatHasProtectedContentDisableRequested::ID: {
    auto& typed = static_cast<td::td_api::messageChatHasProtectedContentDisableRequested&>(value);
    wipe_native_value(typed.is_expired_, observer);
    return;
}
case td::td_api::messageChatHasProtectedContentToggled::ID: {
    auto& typed = static_cast<td::td_api::messageChatHasProtectedContentToggled&>(value);
    wipe_native_value(typed.request_message_id_, observer);
    wipe_native_value(typed.old_has_protected_content_, observer);
    wipe_native_value(typed.new_has_protected_content_, observer);
    return;
}
case td::td_api::messageChatJoinByLink::ID: {
    return;
}
case td::td_api::messageChatJoinByRequest::ID: {
    return;
}
case td::td_api::messageChatOwnerChanged::ID: {
    auto& typed = static_cast<td::td_api::messageChatOwnerChanged&>(value);
    wipe_native_value(typed.new_owner_user_id_, observer);
    return;
}
case td::td_api::messageChatOwnerLeft::ID: {
    auto& typed = static_cast<td::td_api::messageChatOwnerLeft&>(value);
    wipe_native_value(typed.new_owner_user_id_, observer);
    return;
}
case td::td_api::messageChatSetBackground::ID: {
    auto& typed = static_cast<td::td_api::messageChatSetBackground&>(value);
    wipe_native_value(typed.old_background_message_id_, observer);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.only_for_self_, observer);
    return;
}
case td::td_api::messageChatSetMessageAutoDeleteTime::ID: {
    auto& typed = static_cast<td::td_api::messageChatSetMessageAutoDeleteTime&>(value);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    wipe_native_value(typed.from_user_id_, observer);
    return;
}
case td::td_api::messageChatSetTheme::ID: {
    auto& typed = static_cast<td::td_api::messageChatSetTheme&>(value);
    wipe_native_value(typed.theme_, observer);
    return;
}
case td::td_api::messageChatShared::ID: {
    auto& typed = static_cast<td::td_api::messageChatShared&>(value);
    wipe_native_value(typed.chat_, observer);
    wipe_native_value(typed.button_id_, observer);
    return;
}
case td::td_api::messageChatUpgradeFrom::ID: {
    auto& typed = static_cast<td::td_api::messageChatUpgradeFrom&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.basic_group_id_, observer);
    return;
}
case td::td_api::messageChatUpgradeTo::ID: {
    auto& typed = static_cast<td::td_api::messageChatUpgradeTo&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::messageChecklist::ID: {
    auto& typed = static_cast<td::td_api::messageChecklist&>(value);
    wipe_native_value(typed.list_, observer);
    return;
}
case td::td_api::messageChecklistTasksAdded::ID: {
    auto& typed = static_cast<td::td_api::messageChecklistTasksAdded&>(value);
    wipe_native_value(typed.checklist_message_id_, observer);
    wipe_native_value(typed.tasks_, observer);
    return;
}
case td::td_api::messageChecklistTasksDone::ID: {
    auto& typed = static_cast<td::td_api::messageChecklistTasksDone&>(value);
    wipe_native_value(typed.checklist_message_id_, observer);
    wipe_native_value(typed.marked_as_done_task_ids_, observer);
    wipe_native_value(typed.marked_as_not_done_task_ids_, observer);
    return;
}
case td::td_api::messageContact::ID: {
    auto& typed = static_cast<td::td_api::messageContact&>(value);
    wipe_native_value(typed.contact_, observer);
    return;
}
case td::td_api::messageContactRegistered::ID: {
    return;
}
case td::td_api::messageCopyOptions::ID: {
    auto& typed = static_cast<td::td_api::messageCopyOptions&>(value);
    wipe_native_value(typed.send_copy_, observer);
    wipe_native_value(typed.replace_caption_, observer);
    wipe_native_value(typed.new_caption_, observer);
    wipe_native_value(typed.new_show_caption_above_media_, observer);
    return;
}
case td::td_api::messageCustomServiceAction::ID: {
    auto& typed = static_cast<td::td_api::messageCustomServiceAction&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::messageDice::ID: {
    auto& typed = static_cast<td::td_api::messageDice&>(value);
    wipe_native_value(typed.initial_state_, observer);
    wipe_native_value(typed.final_state_, observer);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.value_, observer);
    wipe_native_value(typed.success_animation_frame_number_, observer);
    return;
}
case td::td_api::messageDirectMessagePriceChanged::ID: {
    auto& typed = static_cast<td::td_api::messageDirectMessagePriceChanged&>(value);
    wipe_native_value(typed.is_enabled_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::messageDocument::ID: {
    auto& typed = static_cast<td::td_api::messageDocument&>(value);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::messageEffect::ID: {
    auto& typed = static_cast<td::td_api::messageEffect&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.static_icon_, observer);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.is_premium_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::messageEffectTypeEmojiReaction::ID: {
    auto& typed = static_cast<td::td_api::messageEffectTypeEmojiReaction&>(value);
    wipe_native_value(typed.select_animation_, observer);
    wipe_native_value(typed.effect_animation_, observer);
    return;
}
case td::td_api::messageEffectTypePremiumSticker::ID: {
    auto& typed = static_cast<td::td_api::messageEffectTypePremiumSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::messageExpiredPhoto::ID: {
    return;
}
case td::td_api::messageExpiredVideo::ID: {
    return;
}
case td::td_api::messageExpiredVideoNote::ID: {
    return;
}
case td::td_api::messageExpiredVoiceNote::ID: {
    return;
}
case td::td_api::messageFileTypeGroup::ID: {
    auto& typed = static_cast<td::td_api::messageFileTypeGroup&>(value);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::messageFileTypePrivate::ID: {
    auto& typed = static_cast<td::td_api::messageFileTypePrivate&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::messageFileTypeUnknown::ID: {
    return;
}
case td::td_api::messageForumTopicCreated::ID: {
    auto& typed = static_cast<td::td_api::messageForumTopicCreated&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.is_name_implicit_, observer);
    wipe_native_value(typed.icon_, observer);
    return;
}
case td::td_api::messageForumTopicEdited::ID: {
    auto& typed = static_cast<td::td_api::messageForumTopicEdited&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.edit_icon_custom_emoji_id_, observer);
    wipe_native_value(typed.icon_custom_emoji_id_, observer);
    return;
}
case td::td_api::messageForumTopicIsClosedToggled::ID: {
    auto& typed = static_cast<td::td_api::messageForumTopicIsClosedToggled&>(value);
    wipe_native_value(typed.is_closed_, observer);
    return;
}
case td::td_api::messageForumTopicIsHiddenToggled::ID: {
    auto& typed = static_cast<td::td_api::messageForumTopicIsHiddenToggled&>(value);
    wipe_native_value(typed.is_hidden_, observer);
    return;
}
case td::td_api::messageForwardInfo::ID: {
    auto& typed = static_cast<td::td_api::messageForwardInfo&>(value);
    wipe_native_value(typed.origin_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.public_service_announcement_type_, observer);
    return;
}
case td::td_api::messageGame::ID: {
    auto& typed = static_cast<td::td_api::messageGame&>(value);
    wipe_native_value(typed.game_, observer);
    return;
}
case td::td_api::messageGameScore::ID: {
    auto& typed = static_cast<td::td_api::messageGameScore&>(value);
    wipe_native_value(typed.game_message_id_, observer);
    wipe_native_value(typed.game_id_, observer);
    wipe_native_value(typed.score_, observer);
    return;
}
case td::td_api::messageGift::ID: {
    auto& typed = static_cast<td::td_api::messageGift&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.receiver_id_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.unique_gift_number_, observer);
    wipe_native_value(typed.sell_star_count_, observer);
    wipe_native_value(typed.prepaid_upgrade_star_count_, observer);
    wipe_native_value(typed.is_upgrade_separate_, observer);
    wipe_native_value(typed.is_from_auction_, observer);
    wipe_native_value(typed.is_private_, observer);
    wipe_native_value(typed.is_saved_, observer);
    wipe_native_value(typed.is_prepaid_upgrade_, observer);
    wipe_native_value(typed.can_be_upgraded_, observer);
    wipe_native_value(typed.was_converted_, observer);
    wipe_native_value(typed.was_upgraded_, observer);
    wipe_native_value(typed.was_refunded_, observer);
    wipe_native_value(typed.upgraded_received_gift_id_, observer);
    wipe_native_value(typed.prepaid_upgrade_hash_, observer);
    return;
}
case td::td_api::messageGiftedPremium::ID: {
    auto& typed = static_cast<td::td_api::messageGiftedPremium&>(value);
    wipe_native_value(typed.gifter_user_id_, observer);
    wipe_native_value(typed.receiver_user_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.cryptocurrency_, observer);
    wipe_native_value(typed.cryptocurrency_amount_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.day_count_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::messageGiftedStars::ID: {
    auto& typed = static_cast<td::td_api::messageGiftedStars&>(value);
    wipe_native_value(typed.gifter_user_id_, observer);
    wipe_native_value(typed.receiver_user_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.cryptocurrency_, observer);
    wipe_native_value(typed.cryptocurrency_amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.transaction_id_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::messageGiftedTon::ID: {
    auto& typed = static_cast<td::td_api::messageGiftedTon&>(value);
    wipe_native_value(typed.gifter_user_id_, observer);
    wipe_native_value(typed.receiver_user_id_, observer);
    wipe_native_value(typed.ton_amount_, observer);
    wipe_native_value(typed.transaction_id_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::messageGiveaway::ID: {
    auto& typed = static_cast<td::td_api::messageGiveaway&>(value);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.prize_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::messageGiveawayCompleted::ID: {
    auto& typed = static_cast<td::td_api::messageGiveawayCompleted&>(value);
    wipe_native_value(typed.giveaway_message_id_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.is_star_giveaway_, observer);
    wipe_native_value(typed.unclaimed_prize_count_, observer);
    return;
}
case td::td_api::messageGiveawayCreated::ID: {
    auto& typed = static_cast<td::td_api::messageGiveawayCreated&>(value);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::messageGiveawayPrizeStars::ID: {
    auto& typed = static_cast<td::td_api::messageGiveawayPrizeStars&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.transaction_id_, observer);
    wipe_native_value(typed.boosted_chat_id_, observer);
    wipe_native_value(typed.giveaway_message_id_, observer);
    wipe_native_value(typed.is_unclaimed_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::messageGiveawayWinners::ID: {
    auto& typed = static_cast<td::td_api::messageGiveawayWinners&>(value);
    wipe_native_value(typed.boosted_chat_id_, observer);
    wipe_native_value(typed.giveaway_message_id_, observer);
    wipe_native_value(typed.additional_chat_count_, observer);
    wipe_native_value(typed.actual_winners_selection_date_, observer);
    wipe_native_value(typed.only_new_members_, observer);
    wipe_native_value(typed.was_refunded_, observer);
    wipe_native_value(typed.prize_, observer);
    wipe_native_value(typed.prize_description_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.winner_user_ids_, observer);
    wipe_native_value(typed.unclaimed_prize_count_, observer);
    return;
}
case td::td_api::messageGroupCall::ID: {
    auto& typed = static_cast<td::td_api::messageGroupCall&>(value);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.is_active_, observer);
    wipe_native_value(typed.was_missed_, observer);
    wipe_native_value(typed.is_video_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.other_participant_ids_, observer);
    return;
}
case td::td_api::messageImportInfo::ID: {
    auto& typed = static_cast<td::td_api::messageImportInfo&>(value);
    wipe_native_value(typed.sender_name_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::messageInteractionInfo::ID: {
    auto& typed = static_cast<td::td_api::messageInteractionInfo&>(value);
    wipe_native_value(typed.view_count_, observer);
    wipe_native_value(typed.forward_count_, observer);
    wipe_native_value(typed.reply_info_, observer);
    wipe_native_value(typed.reactions_, observer);
    return;
}
case td::td_api::messageInviteVideoChatParticipants::ID: {
    auto& typed = static_cast<td::td_api::messageInviteVideoChatParticipants&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::messageInvoice::ID: {
    auto& typed = static_cast<td::td_api::messageInvoice&>(value);
    wipe_native_value(typed.product_info_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.is_test_, observer);
    wipe_native_value(typed.need_shipping_address_, observer);
    wipe_native_value(typed.receipt_message_id_, observer);
    wipe_native_value(typed.paid_media_, observer);
    wipe_native_value(typed.paid_media_caption_, observer);
    return;
}
case td::td_api::messageLink::ID: {
    auto& typed = static_cast<td::td_api::messageLink&>(value);
    wipe_native_value(typed.link_, observer);
    wipe_native_value(typed.is_public_, observer);
    return;
}
case td::td_api::messageLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::messageLinkInfo&>(value);
    wipe_native_value(typed.is_public_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.media_timestamp_, observer);
    wipe_native_value(typed.checklist_task_id_, observer);
    wipe_native_value(typed.poll_option_id_, observer);
    wipe_native_value(typed.for_album_, observer);
    return;
}
case td::td_api::messageLiveLocation::ID: {
    auto& typed = static_cast<td::td_api::messageLiveLocation&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.expires_in_, observer);
    return;
}
case td::td_api::messageLocation::ID: {
    auto& typed = static_cast<td::td_api::messageLocation&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::messageManagedBotCreated::ID: {
    auto& typed = static_cast<td::td_api::messageManagedBotCreated&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::messageOriginChannel::ID: {
    auto& typed = static_cast<td::td_api::messageOriginChannel&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.author_signature_, observer);
    return;
}
case td::td_api::messageOriginChat::ID: {
    auto& typed = static_cast<td::td_api::messageOriginChat&>(value);
    wipe_native_value(typed.sender_chat_id_, observer);
    wipe_native_value(typed.author_signature_, observer);
    return;
}
case td::td_api::messageOriginHiddenUser::ID: {
    auto& typed = static_cast<td::td_api::messageOriginHiddenUser&>(value);
    wipe_native_value(typed.sender_name_, observer);
    return;
}
case td::td_api::messageOriginUser::ID: {
    auto& typed = static_cast<td::td_api::messageOriginUser&>(value);
    wipe_native_value(typed.sender_user_id_, observer);
    return;
}
case td::td_api::messagePaidMedia::ID: {
    auto& typed = static_cast<td::td_api::messagePaidMedia&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    return;
}
case td::td_api::messagePaidMessagePriceChanged::ID: {
    auto& typed = static_cast<td::td_api::messagePaidMessagePriceChanged&>(value);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::messagePaidMessagesRefunded::ID: {
    auto& typed = static_cast<td::td_api::messagePaidMessagesRefunded&>(value);
    wipe_native_value(typed.message_count_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::messagePassportDataReceived::ID: {
    auto& typed = static_cast<td::td_api::messagePassportDataReceived&>(value);
    wipe_native_value(typed.elements_, observer);
    wipe_native_value(typed.credentials_, observer);
    return;
}
case td::td_api::messagePassportDataSent::ID: {
    auto& typed = static_cast<td::td_api::messagePassportDataSent&>(value);
    wipe_native_value(typed.types_, observer);
    return;
}
case td::td_api::messagePaymentRefunded::ID: {
    auto& typed = static_cast<td::td_api::messagePaymentRefunded&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.invoice_payload_, observer);
    wipe_native_value(typed.telegram_payment_charge_id_, observer);
    wipe_native_value(typed.provider_payment_charge_id_, observer);
    return;
}
case td::td_api::messagePaymentSuccessful::ID: {
    auto& typed = static_cast<td::td_api::messagePaymentSuccessful&>(value);
    wipe_native_value(typed.invoice_chat_id_, observer);
    wipe_native_value(typed.invoice_message_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.subscription_until_date_, observer);
    wipe_native_value(typed.is_recurring_, observer);
    wipe_native_value(typed.is_first_recurring_, observer);
    wipe_native_value(typed.invoice_name_, observer);
    return;
}
case td::td_api::messagePaymentSuccessfulBot::ID: {
    auto& typed = static_cast<td::td_api::messagePaymentSuccessfulBot&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.subscription_until_date_, observer);
    wipe_native_value(typed.is_recurring_, observer);
    wipe_native_value(typed.is_first_recurring_, observer);
    wipe_native_value(typed.invoice_payload_, observer);
    wipe_native_value(typed.shipping_option_id_, observer);
    wipe_native_value(typed.order_info_, observer);
    wipe_native_value(typed.telegram_payment_charge_id_, observer);
    wipe_native_value(typed.provider_payment_charge_id_, observer);
    return;
}
case td::td_api::messagePhoto::ID: {
    auto& typed = static_cast<td::td_api::messagePhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    wipe_native_value(typed.is_secret_, observer);
    return;
}
case td::td_api::messagePinMessage::ID: {
    auto& typed = static_cast<td::td_api::messagePinMessage&>(value);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::messagePoll::ID: {
    auto& typed = static_cast<td::td_api::messagePoll&>(value);
    wipe_native_value(typed.poll_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.can_add_option_, observer);
    return;
}
case td::td_api::messagePollOptionAdded::ID: {
    auto& typed = static_cast<td::td_api::messagePollOptionAdded&>(value);
    wipe_native_value(typed.poll_message_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::messagePollOptionDeleted::ID: {
    auto& typed = static_cast<td::td_api::messagePollOptionDeleted&>(value);
    wipe_native_value(typed.poll_message_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::messagePosition::ID: {
    auto& typed = static_cast<td::td_api::messagePosition&>(value);
    wipe_native_value(typed.position_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::messagePositions::ID: {
    auto& typed = static_cast<td::td_api::messagePositions&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.positions_, observer);
    return;
}
case td::td_api::messagePremiumGiftCode::ID: {
    auto& typed = static_cast<td::td_api::messagePremiumGiftCode&>(value);
    wipe_native_value(typed.creator_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.is_from_giveaway_, observer);
    wipe_native_value(typed.is_unclaimed_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.cryptocurrency_, observer);
    wipe_native_value(typed.cryptocurrency_amount_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.day_count_, observer);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::messageProperties::ID: {
    auto& typed = static_cast<td::td_api::messageProperties&>(value);
    wipe_native_value(typed.can_add_offer_, observer);
    wipe_native_value(typed.can_add_tasks_, observer);
    wipe_native_value(typed.can_be_approved_, observer);
    wipe_native_value(typed.can_be_copied_, observer);
    wipe_native_value(typed.can_be_copied_to_secret_chat_, observer);
    wipe_native_value(typed.can_be_declined_, observer);
    wipe_native_value(typed.can_be_deleted_only_for_self_, observer);
    wipe_native_value(typed.can_be_deleted_for_all_users_, observer);
    wipe_native_value(typed.can_be_edited_, observer);
    wipe_native_value(typed.can_be_forwarded_, observer);
    wipe_native_value(typed.can_be_paid_, observer);
    wipe_native_value(typed.can_be_pinned_, observer);
    wipe_native_value(typed.can_be_replied_, observer);
    wipe_native_value(typed.can_be_replied_in_another_chat_, observer);
    wipe_native_value(typed.can_be_saved_, observer);
    wipe_native_value(typed.can_be_shared_in_story_, observer);
    wipe_native_value(typed.can_delete_reactions_, observer);
    wipe_native_value(typed.can_edit_media_, observer);
    wipe_native_value(typed.can_edit_scheduling_state_, observer);
    wipe_native_value(typed.can_edit_suggested_post_info_, observer);
    wipe_native_value(typed.can_get_author_, observer);
    wipe_native_value(typed.can_get_embedding_code_, observer);
    wipe_native_value(typed.can_get_link_, observer);
    wipe_native_value(typed.can_get_media_timestamp_links_, observer);
    wipe_native_value(typed.can_get_message_thread_, observer);
    wipe_native_value(typed.can_get_poll_vote_statistics_, observer);
    wipe_native_value(typed.can_get_read_date_, observer);
    wipe_native_value(typed.can_get_statistics_, observer);
    wipe_native_value(typed.can_get_video_advertisements_, observer);
    wipe_native_value(typed.can_get_viewers_, observer);
    wipe_native_value(typed.can_mark_tasks_as_done_, observer);
    wipe_native_value(typed.can_recognize_speech_, observer);
    wipe_native_value(typed.can_report_chat_, observer);
    wipe_native_value(typed.can_report_reactions_, observer);
    wipe_native_value(typed.can_report_supergroup_spam_, observer);
    wipe_native_value(typed.can_set_fact_check_, observer);
    wipe_native_value(typed.has_protected_content_by_current_user_, observer);
    wipe_native_value(typed.has_protected_content_by_other_user_, observer);
    wipe_native_value(typed.need_show_statistics_, observer);
    return;
}
case td::td_api::messageProximityAlertTriggered::ID: {
    auto& typed = static_cast<td::td_api::messageProximityAlertTriggered&>(value);
    wipe_native_value(typed.traveler_id_, observer);
    wipe_native_value(typed.watcher_id_, observer);
    wipe_native_value(typed.distance_, observer);
    return;
}
case td::td_api::messageReaction::ID: {
    auto& typed = static_cast<td::td_api::messageReaction&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.is_chosen_, observer);
    wipe_native_value(typed.used_sender_id_, observer);
    wipe_native_value(typed.recent_sender_ids_, observer);
    return;
}
case td::td_api::messageReactions::ID: {
    auto& typed = static_cast<td::td_api::messageReactions&>(value);
    wipe_native_value(typed.reactions_, observer);
    wipe_native_value(typed.are_tags_, observer);
    wipe_native_value(typed.paid_reactors_, observer);
    wipe_native_value(typed.can_get_added_reactions_, observer);
    return;
}
case td::td_api::messageReadDateMyPrivacyRestricted::ID: {
    return;
}
case td::td_api::messageReadDateRead::ID: {
    auto& typed = static_cast<td::td_api::messageReadDateRead&>(value);
    wipe_native_value(typed.read_date_, observer);
    return;
}
case td::td_api::messageReadDateTooOld::ID: {
    return;
}
case td::td_api::messageReadDateUnread::ID: {
    return;
}
case td::td_api::messageReadDateUserPrivacyRestricted::ID: {
    return;
}
case td::td_api::messageRefundedUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::messageRefundedUpgradedGift&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.receiver_id_, observer);
    wipe_native_value(typed.origin_, observer);
    return;
}
case td::td_api::messageReplyInfo::ID: {
    auto& typed = static_cast<td::td_api::messageReplyInfo&>(value);
    wipe_native_value(typed.reply_count_, observer);
    wipe_native_value(typed.recent_replier_ids_, observer);
    wipe_native_value(typed.last_read_inbox_message_id_, observer);
    wipe_native_value(typed.last_read_outbox_message_id_, observer);
    wipe_native_value(typed.last_message_id_, observer);
    return;
}
case td::td_api::messageReplyToMessage::ID: {
    auto& typed = static_cast<td::td_api::messageReplyToMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.quote_, observer);
    wipe_native_value(typed.checklist_task_id_, observer);
    wipe_native_value(typed.poll_option_id_, observer);
    wipe_native_value(typed.origin_, observer);
    wipe_native_value(typed.origin_send_date_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::messageReplyToStory::ID: {
    auto& typed = static_cast<td::td_api::messageReplyToStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::messageRichMessage::ID: {
    auto& typed = static_cast<td::td_api::messageRichMessage&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::messageSchedulingStateSendAtDate::ID: {
    auto& typed = static_cast<td::td_api::messageSchedulingStateSendAtDate&>(value);
    wipe_native_value(typed.send_date_, observer);
    wipe_native_value(typed.repeat_period_, observer);
    return;
}
case td::td_api::messageSchedulingStateSendWhenOnline::ID: {
    return;
}
case td::td_api::messageSchedulingStateSendWhenVideoProcessed::ID: {
    auto& typed = static_cast<td::td_api::messageSchedulingStateSendWhenVideoProcessed&>(value);
    wipe_native_value(typed.send_date_, observer);
    return;
}
case td::td_api::messageScreenshotTaken::ID: {
    return;
}
case td::td_api::messageSelfDestructTypeImmediately::ID: {
    return;
}
case td::td_api::messageSelfDestructTypeTimer::ID: {
    auto& typed = static_cast<td::td_api::messageSelfDestructTypeTimer&>(value);
    wipe_native_value(typed.self_destruct_time_, observer);
    return;
}
case td::td_api::messageSendOptions::ID: {
    auto& typed = static_cast<td::td_api::messageSendOptions&>(value);
    wipe_native_value(typed.suggested_post_info_, observer);
    wipe_native_value(typed.disable_notification_, observer);
    wipe_native_value(typed.from_background_, observer);
    wipe_native_value(typed.protect_content_, observer);
    wipe_native_value(typed.allow_paid_broadcast_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    wipe_native_value(typed.update_order_of_installed_sticker_sets_, observer);
    wipe_native_value(typed.scheduling_state_, observer);
    wipe_native_value(typed.effect_id_, observer);
    wipe_native_value(typed.sending_id_, observer);
    wipe_native_value(typed.only_preview_, observer);
    return;
}
case td::td_api::messageSenderChat::ID: {
    auto& typed = static_cast<td::td_api::messageSenderChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::messageSenderUser::ID: {
    auto& typed = static_cast<td::td_api::messageSenderUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::messageSenders::ID: {
    auto& typed = static_cast<td::td_api::messageSenders&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.senders_, observer);
    return;
}
case td::td_api::messageSendingStateFailed::ID: {
    auto& typed = static_cast<td::td_api::messageSendingStateFailed&>(value);
    wipe_native_value(typed.error_, observer);
    wipe_native_value(typed.can_retry_, observer);
    wipe_native_value(typed.need_another_sender_, observer);
    wipe_native_value(typed.need_another_reply_quote_, observer);
    wipe_native_value(typed.need_drop_reply_, observer);
    wipe_native_value(typed.required_paid_message_star_count_, observer);
    wipe_native_value(typed.retry_after_, observer);
    return;
}
case td::td_api::messageSendingStatePending::ID: {
    auto& typed = static_cast<td::td_api::messageSendingStatePending&>(value);
    wipe_native_value(typed.sending_id_, observer);
    return;
}
case td::td_api::messageSourceChatEventLog::ID: {
    return;
}
case td::td_api::messageSourceChatHistory::ID: {
    return;
}
case td::td_api::messageSourceChatList::ID: {
    return;
}
case td::td_api::messageSourceDirectMessagesChatTopicHistory::ID: {
    return;
}
case td::td_api::messageSourceForumTopicHistory::ID: {
    return;
}
case td::td_api::messageSourceHistoryPreview::ID: {
    return;
}
case td::td_api::messageSourceMessageThreadHistory::ID: {
    return;
}
case td::td_api::messageSourceNotification::ID: {
    return;
}
case td::td_api::messageSourceOther::ID: {
    return;
}
case td::td_api::messageSourceScreenshot::ID: {
    return;
}
case td::td_api::messageSourceSearch::ID: {
    return;
}
case td::td_api::messageStakeDice::ID: {
    auto& typed = static_cast<td::td_api::messageStakeDice&>(value);
    wipe_native_value(typed.initial_state_, observer);
    wipe_native_value(typed.final_state_, observer);
    wipe_native_value(typed.value_, observer);
    wipe_native_value(typed.stake_toncoin_amount_, observer);
    wipe_native_value(typed.prize_toncoin_amount_, observer);
    return;
}
case td::td_api::messageStatistics::ID: {
    auto& typed = static_cast<td::td_api::messageStatistics&>(value);
    wipe_native_value(typed.message_interaction_graph_, observer);
    wipe_native_value(typed.message_reaction_graph_, observer);
    return;
}
case td::td_api::messageSticker::ID: {
    auto& typed = static_cast<td::td_api::messageSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.is_premium_, observer);
    return;
}
case td::td_api::messageStory::ID: {
    auto& typed = static_cast<td::td_api::messageStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.via_mention_, observer);
    return;
}
case td::td_api::messageSuggestBirthdate::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestBirthdate&>(value);
    wipe_native_value(typed.birthdate_, observer);
    return;
}
case td::td_api::messageSuggestProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestProfilePhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::messageSuggestedPostApprovalFailed::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestedPostApprovalFailed&>(value);
    wipe_native_value(typed.suggested_post_message_id_, observer);
    wipe_native_value(typed.price_, observer);
    return;
}
case td::td_api::messageSuggestedPostApproved::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestedPostApproved&>(value);
    wipe_native_value(typed.suggested_post_message_id_, observer);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.send_date_, observer);
    return;
}
case td::td_api::messageSuggestedPostDeclined::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestedPostDeclined&>(value);
    wipe_native_value(typed.suggested_post_message_id_, observer);
    wipe_native_value(typed.comment_, observer);
    return;
}
case td::td_api::messageSuggestedPostPaid::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestedPostPaid&>(value);
    wipe_native_value(typed.suggested_post_message_id_, observer);
    wipe_native_value(typed.star_amount_, observer);
    wipe_native_value(typed.ton_amount_, observer);
    return;
}
case td::td_api::messageSuggestedPostRefunded::ID: {
    auto& typed = static_cast<td::td_api::messageSuggestedPostRefunded&>(value);
    wipe_native_value(typed.suggested_post_message_id_, observer);
    wipe_native_value(typed.reason_, observer);
    return;
}
case td::td_api::messageSupergroupChatCreate::ID: {
    auto& typed = static_cast<td::td_api::messageSupergroupChatCreate&>(value);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::messageText::ID: {
    auto& typed = static_cast<td::td_api::messageText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.link_preview_, observer);
    wipe_native_value(typed.link_preview_options_, observer);
    return;
}
case td::td_api::messageThreadInfo::ID: {
    auto& typed = static_cast<td::td_api::messageThreadInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_thread_id_, observer);
    wipe_native_value(typed.reply_info_, observer);
    wipe_native_value(typed.unread_message_count_, observer);
    wipe_native_value(typed.messages_, observer);
    wipe_native_value(typed.draft_message_, observer);
    return;
}
case td::td_api::messageTopicDirectMessages::ID: {
    auto& typed = static_cast<td::td_api::messageTopicDirectMessages&>(value);
    wipe_native_value(typed.direct_messages_chat_topic_id_, observer);
    return;
}
case td::td_api::messageTopicForum::ID: {
    auto& typed = static_cast<td::td_api::messageTopicForum&>(value);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::messageTopicSavedMessages::ID: {
    auto& typed = static_cast<td::td_api::messageTopicSavedMessages&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    return;
}
case td::td_api::messageTopicThread::ID: {
    auto& typed = static_cast<td::td_api::messageTopicThread&>(value);
    wipe_native_value(typed.message_thread_id_, observer);
    return;
}
case td::td_api::messageUnsupported::ID: {
    return;
}
case td::td_api::messageUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::messageUpgradedGift&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.receiver_id_, observer);
    wipe_native_value(typed.origin_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.is_saved_, observer);
    wipe_native_value(typed.can_be_transferred_, observer);
    wipe_native_value(typed.was_transferred_, observer);
    wipe_native_value(typed.transfer_star_count_, observer);
    wipe_native_value(typed.drop_original_details_star_count_, observer);
    wipe_native_value(typed.next_transfer_date_, observer);
    wipe_native_value(typed.next_resale_date_, observer);
    wipe_native_value(typed.export_date_, observer);
    wipe_native_value(typed.craft_date_, observer);
    return;
}
case td::td_api::messageUpgradedGiftPurchaseOffer::ID: {
    auto& typed = static_cast<td::td_api::messageUpgradedGiftPurchaseOffer&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.state_, observer);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    return;
}
case td::td_api::messageUpgradedGiftPurchaseOfferRejected::ID: {
    auto& typed = static_cast<td::td_api::messageUpgradedGiftPurchaseOfferRejected&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.offer_message_id_, observer);
    wipe_native_value(typed.was_expired_, observer);
    return;
}
case td::td_api::messageUsersShared::ID: {
    auto& typed = static_cast<td::td_api::messageUsersShared&>(value);
    wipe_native_value(typed.users_, observer);
    wipe_native_value(typed.button_id_, observer);
    return;
}
case td::td_api::messageVenue::ID: {
    auto& typed = static_cast<td::td_api::messageVenue&>(value);
    wipe_native_value(typed.venue_, observer);
    return;
}
case td::td_api::messageVideo::ID: {
    auto& typed = static_cast<td::td_api::messageVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.alternative_videos_, observer);
    wipe_native_value(typed.storyboards_, observer);
    wipe_native_value(typed.cover_, observer);
    wipe_native_value(typed.start_timestamp_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    wipe_native_value(typed.is_secret_, observer);
    return;
}
case td::td_api::messageVideoChatEnded::ID: {
    auto& typed = static_cast<td::td_api::messageVideoChatEnded&>(value);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::messageVideoChatScheduled::ID: {
    auto& typed = static_cast<td::td_api::messageVideoChatScheduled&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.start_date_, observer);
    return;
}
case td::td_api::messageVideoChatStarted::ID: {
    auto& typed = static_cast<td::td_api::messageVideoChatStarted&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::messageVideoNote::ID: {
    auto& typed = static_cast<td::td_api::messageVideoNote&>(value);
    wipe_native_value(typed.video_note_, observer);
    wipe_native_value(typed.is_viewed_, observer);
    wipe_native_value(typed.is_secret_, observer);
    return;
}
case td::td_api::messageViewer::ID: {
    auto& typed = static_cast<td::td_api::messageViewer&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.view_date_, observer);
    return;
}
case td::td_api::messageViewers::ID: {
    auto& typed = static_cast<td::td_api::messageViewers&>(value);
    wipe_native_value(typed.viewers_, observer);
    return;
}
case td::td_api::messageVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::messageVoiceNote&>(value);
    wipe_native_value(typed.voice_note_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.is_listened_, observer);
    return;
}
case td::td_api::messageWebAppDataReceived::ID: {
    auto& typed = static_cast<td::td_api::messageWebAppDataReceived&>(value);
    wipe_native_value(typed.button_text_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::messageWebAppDataSent::ID: {
    auto& typed = static_cast<td::td_api::messageWebAppDataSent&>(value);
    wipe_native_value(typed.button_text_, observer);
    return;
}
case td::td_api::messages::ID: {
    auto& typed = static_cast<td::td_api::messages&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.messages_, observer);
    return;
}
case td::td_api::minithumbnail::ID: {
    auto& typed = static_cast<td::td_api::minithumbnail&>(value);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::networkStatistics::ID: {
    auto& typed = static_cast<td::td_api::networkStatistics&>(value);
    wipe_native_value(typed.since_date_, observer);
    wipe_native_value(typed.entries_, observer);
    return;
}
case td::td_api::networkStatisticsEntryCall::ID: {
    auto& typed = static_cast<td::td_api::networkStatisticsEntryCall&>(value);
    wipe_native_value(typed.network_type_, observer);
    wipe_native_value(typed.sent_bytes_, observer);
    wipe_native_value(typed.received_bytes_, observer);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::networkStatisticsEntryFile::ID: {
    auto& typed = static_cast<td::td_api::networkStatisticsEntryFile&>(value);
    wipe_native_value(typed.file_type_, observer);
    wipe_native_value(typed.network_type_, observer);
    wipe_native_value(typed.sent_bytes_, observer);
    wipe_native_value(typed.received_bytes_, observer);
    return;
}
case td::td_api::networkTypeMobile::ID: {
    return;
}
case td::td_api::networkTypeMobileRoaming::ID: {
    return;
}
case td::td_api::networkTypeNone::ID: {
    return;
}
case td::td_api::networkTypeOther::ID: {
    return;
}
case td::td_api::networkTypeWiFi::ID: {
    return;
}
case td::td_api::newChatPrivacySettings::ID: {
    auto& typed = static_cast<td::td_api::newChatPrivacySettings&>(value);
    wipe_native_value(typed.allow_new_chats_from_unknown_users_, observer);
    wipe_native_value(typed.incoming_paid_message_star_count_, observer);
    return;
}
case td::td_api::notification::ID: {
    auto& typed = static_cast<td::td_api::notification&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.is_silent_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::notificationGroup::ID: {
    auto& typed = static_cast<td::td_api::notificationGroup&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.notifications_, observer);
    return;
}
case td::td_api::notificationGroupTypeCalls::ID: {
    return;
}
case td::td_api::notificationGroupTypeMentions::ID: {
    return;
}
case td::td_api::notificationGroupTypeMessages::ID: {
    return;
}
case td::td_api::notificationGroupTypeSecretChat::ID: {
    return;
}
case td::td_api::notificationSettingsScopeChannelChats::ID: {
    return;
}
case td::td_api::notificationSettingsScopeGroupChats::ID: {
    return;
}
case td::td_api::notificationSettingsScopePrivateChats::ID: {
    return;
}
case td::td_api::notificationSound::ID: {
    auto& typed = static_cast<td::td_api::notificationSound&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.data_, observer);
    wipe_native_value(typed.sound_, observer);
    return;
}
case td::td_api::notificationSounds::ID: {
    auto& typed = static_cast<td::td_api::notificationSounds&>(value);
    wipe_native_value(typed.notification_sounds_, observer);
    return;
}
case td::td_api::notificationTypeNewCall::ID: {
    auto& typed = static_cast<td::td_api::notificationTypeNewCall&>(value);
    wipe_native_value(typed.call_id_, observer);
    return;
}
case td::td_api::notificationTypeNewMessage::ID: {
    auto& typed = static_cast<td::td_api::notificationTypeNewMessage&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.show_preview_, observer);
    return;
}
case td::td_api::notificationTypeNewPushMessage::ID: {
    auto& typed = static_cast<td::td_api::notificationTypeNewPushMessage&>(value);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.sender_name_, observer);
    wipe_native_value(typed.is_outgoing_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::notificationTypeNewSecretChat::ID: {
    return;
}
case td::td_api::oauthLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::oauthLinkInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.domain_, observer);
    wipe_native_value(typed.from_app_, observer);
    wipe_native_value(typed.verified_app_name_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.request_write_access_, observer);
    wipe_native_value(typed.request_phone_number_access_, observer);
    wipe_native_value(typed.browser_, observer);
    wipe_native_value(typed.platform_, observer);
    wipe_native_value(typed.ip_address_, observer);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.match_code_first_, observer);
    wipe_native_value(typed.match_codes_, observer);
    return;
}
case td::td_api::ok::ID: {
    return;
}
case td::td_api::optionValueBoolean::ID: {
    auto& typed = static_cast<td::td_api::optionValueBoolean&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::optionValueEmpty::ID: {
    return;
}
case td::td_api::optionValueInteger::ID: {
    auto& typed = static_cast<td::td_api::optionValueInteger&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::optionValueString::ID: {
    auto& typed = static_cast<td::td_api::optionValueString&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::orderInfo::ID: {
    auto& typed = static_cast<td::td_api::orderInfo&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.email_address_, observer);
    wipe_native_value(typed.shipping_address_, observer);
    return;
}
case td::td_api::outline::ID: {
    auto& typed = static_cast<td::td_api::outline&>(value);
    wipe_native_value(typed.paths_, observer);
    return;
}
case td::td_api::pageBlockAnchor::ID: {
    auto& typed = static_cast<td::td_api::pageBlockAnchor&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::pageBlockAnimation::ID: {
    auto& typed = static_cast<td::td_api::pageBlockAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.need_autoplay_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    return;
}
case td::td_api::pageBlockAudio::ID: {
    auto& typed = static_cast<td::td_api::pageBlockAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::pageBlockAuthorDate::ID: {
    auto& typed = static_cast<td::td_api::pageBlockAuthorDate&>(value);
    wipe_native_value(typed.author_, observer);
    wipe_native_value(typed.publish_date_, observer);
    return;
}
case td::td_api::pageBlockBlockQuote::ID: {
    auto& typed = static_cast<td::td_api::pageBlockBlockQuote&>(value);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.credit_, observer);
    return;
}
case td::td_api::pageBlockCaption::ID: {
    auto& typed = static_cast<td::td_api::pageBlockCaption&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.credit_, observer);
    return;
}
case td::td_api::pageBlockChatLink::ID: {
    auto& typed = static_cast<td::td_api::pageBlockChatLink&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::pageBlockCollage::ID: {
    auto& typed = static_cast<td::td_api::pageBlockCollage&>(value);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::pageBlockCover::ID: {
    auto& typed = static_cast<td::td_api::pageBlockCover&>(value);
    wipe_native_value(typed.cover_, observer);
    return;
}
case td::td_api::pageBlockDetails::ID: {
    auto& typed = static_cast<td::td_api::pageBlockDetails&>(value);
    wipe_native_value(typed.header_, observer);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.is_open_, observer);
    return;
}
case td::td_api::pageBlockDivider::ID: {
    return;
}
case td::td_api::pageBlockEmbedded::ID: {
    auto& typed = static_cast<td::td_api::pageBlockEmbedded&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.html_, observer);
    wipe_native_value(typed.poster_photo_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.is_full_width_, observer);
    wipe_native_value(typed.allow_scrolling_, observer);
    return;
}
case td::td_api::pageBlockEmbeddedPost::ID: {
    auto& typed = static_cast<td::td_api::pageBlockEmbeddedPost&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.author_, observer);
    wipe_native_value(typed.author_photo_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::pageBlockFooter::ID: {
    auto& typed = static_cast<td::td_api::pageBlockFooter&>(value);
    wipe_native_value(typed.footer_, observer);
    return;
}
case td::td_api::pageBlockHeader::ID: {
    auto& typed = static_cast<td::td_api::pageBlockHeader&>(value);
    wipe_native_value(typed.header_, observer);
    return;
}
case td::td_api::pageBlockHorizontalAlignmentCenter::ID: {
    return;
}
case td::td_api::pageBlockHorizontalAlignmentLeft::ID: {
    return;
}
case td::td_api::pageBlockHorizontalAlignmentRight::ID: {
    return;
}
case td::td_api::pageBlockKicker::ID: {
    auto& typed = static_cast<td::td_api::pageBlockKicker&>(value);
    wipe_native_value(typed.kicker_, observer);
    return;
}
case td::td_api::pageBlockList::ID: {
    auto& typed = static_cast<td::td_api::pageBlockList&>(value);
    wipe_native_value(typed.items_, observer);
    return;
}
case td::td_api::pageBlockListItem::ID: {
    auto& typed = static_cast<td::td_api::pageBlockListItem&>(value);
    wipe_native_value(typed.label_, observer);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.has_checkbox_, observer);
    wipe_native_value(typed.is_checked_, observer);
    wipe_native_value(typed.value_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::pageBlockMap::ID: {
    auto& typed = static_cast<td::td_api::pageBlockMap&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.zoom_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::pageBlockMathematicalExpression::ID: {
    auto& typed = static_cast<td::td_api::pageBlockMathematicalExpression&>(value);
    wipe_native_value(typed.expression_, observer);
    return;
}
case td::td_api::pageBlockParagraph::ID: {
    auto& typed = static_cast<td::td_api::pageBlockParagraph&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::pageBlockPhoto::ID: {
    auto& typed = static_cast<td::td_api::pageBlockPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    return;
}
case td::td_api::pageBlockPreformatted::ID: {
    auto& typed = static_cast<td::td_api::pageBlockPreformatted&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.language_, observer);
    return;
}
case td::td_api::pageBlockPullQuote::ID: {
    auto& typed = static_cast<td::td_api::pageBlockPullQuote&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.credit_, observer);
    return;
}
case td::td_api::pageBlockRelatedArticle::ID: {
    auto& typed = static_cast<td::td_api::pageBlockRelatedArticle&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.author_, observer);
    wipe_native_value(typed.publish_date_, observer);
    return;
}
case td::td_api::pageBlockRelatedArticles::ID: {
    auto& typed = static_cast<td::td_api::pageBlockRelatedArticles&>(value);
    wipe_native_value(typed.header_, observer);
    wipe_native_value(typed.articles_, observer);
    return;
}
case td::td_api::pageBlockSectionHeading::ID: {
    auto& typed = static_cast<td::td_api::pageBlockSectionHeading&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.size_, observer);
    return;
}
case td::td_api::pageBlockSlideshow::ID: {
    auto& typed = static_cast<td::td_api::pageBlockSlideshow&>(value);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::pageBlockSubheader::ID: {
    auto& typed = static_cast<td::td_api::pageBlockSubheader&>(value);
    wipe_native_value(typed.subheader_, observer);
    return;
}
case td::td_api::pageBlockSubtitle::ID: {
    auto& typed = static_cast<td::td_api::pageBlockSubtitle&>(value);
    wipe_native_value(typed.subtitle_, observer);
    return;
}
case td::td_api::pageBlockTable::ID: {
    auto& typed = static_cast<td::td_api::pageBlockTable&>(value);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.cells_, observer);
    wipe_native_value(typed.is_bordered_, observer);
    wipe_native_value(typed.is_striped_, observer);
    return;
}
case td::td_api::pageBlockTableCell::ID: {
    auto& typed = static_cast<td::td_api::pageBlockTableCell&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.is_header_, observer);
    wipe_native_value(typed.colspan_, observer);
    wipe_native_value(typed.rowspan_, observer);
    wipe_native_value(typed.align_, observer);
    wipe_native_value(typed.valign_, observer);
    return;
}
case td::td_api::pageBlockThinking::ID: {
    auto& typed = static_cast<td::td_api::pageBlockThinking&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::pageBlockTitle::ID: {
    auto& typed = static_cast<td::td_api::pageBlockTitle&>(value);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::pageBlockVerticalAlignmentBottom::ID: {
    return;
}
case td::td_api::pageBlockVerticalAlignmentMiddle::ID: {
    return;
}
case td::td_api::pageBlockVerticalAlignmentTop::ID: {
    return;
}
case td::td_api::pageBlockVideo::ID: {
    auto& typed = static_cast<td::td_api::pageBlockVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.need_autoplay_, observer);
    wipe_native_value(typed.is_looped_, observer);
    wipe_native_value(typed.has_spoiler_, observer);
    return;
}
case td::td_api::pageBlockVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::pageBlockVoiceNote&>(value);
    wipe_native_value(typed.voice_note_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::paidMediaPhoto::ID: {
    auto& typed = static_cast<td::td_api::paidMediaPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::paidMediaPreview::ID: {
    auto& typed = static_cast<td::td_api::paidMediaPreview&>(value);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    return;
}
case td::td_api::paidMediaUnsupported::ID: {
    return;
}
case td::td_api::paidMediaVideo::ID: {
    auto& typed = static_cast<td::td_api::paidMediaVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.cover_, observer);
    wipe_native_value(typed.start_timestamp_, observer);
    return;
}
case td::td_api::paidReactionTypeAnonymous::ID: {
    return;
}
case td::td_api::paidReactionTypeChat::ID: {
    auto& typed = static_cast<td::td_api::paidReactionTypeChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::paidReactionTypeRegular::ID: {
    return;
}
case td::td_api::paidReactor::ID: {
    auto& typed = static_cast<td::td_api::paidReactor&>(value);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.is_top_, observer);
    wipe_native_value(typed.is_me_, observer);
    wipe_native_value(typed.is_anonymous_, observer);
    return;
}
case td::td_api::passkey::ID: {
    auto& typed = static_cast<td::td_api::passkey&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.addition_date_, observer);
    wipe_native_value(typed.last_usage_date_, observer);
    wipe_native_value(typed.software_icon_custom_emoji_id_, observer);
    return;
}
case td::td_api::passkeys::ID: {
    auto& typed = static_cast<td::td_api::passkeys&>(value);
    wipe_native_value(typed.passkeys_, observer);
    return;
}
case td::td_api::passportAuthorizationForm::ID: {
    auto& typed = static_cast<td::td_api::passportAuthorizationForm&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.required_elements_, observer);
    wipe_native_value(typed.privacy_policy_url_, observer);
    return;
}
case td::td_api::passportElementAddress::ID: {
    auto& typed = static_cast<td::td_api::passportElementAddress&>(value);
    wipe_native_value(typed.address_, observer);
    return;
}
case td::td_api::passportElementBankStatement::ID: {
    auto& typed = static_cast<td::td_api::passportElementBankStatement&>(value);
    wipe_native_value(typed.bank_statement_, observer);
    return;
}
case td::td_api::passportElementDriverLicense::ID: {
    auto& typed = static_cast<td::td_api::passportElementDriverLicense&>(value);
    wipe_native_value(typed.driver_license_, observer);
    return;
}
case td::td_api::passportElementEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::passportElementEmailAddress&>(value);
    wipe_native_value(typed.email_address_, observer);
    return;
}
case td::td_api::passportElementError::ID: {
    auto& typed = static_cast<td::td_api::passportElementError&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.source_, observer);
    return;
}
case td::td_api::passportElementErrorSourceDataField::ID: {
    auto& typed = static_cast<td::td_api::passportElementErrorSourceDataField&>(value);
    wipe_native_value(typed.field_name_, observer);
    return;
}
case td::td_api::passportElementErrorSourceFile::ID: {
    auto& typed = static_cast<td::td_api::passportElementErrorSourceFile&>(value);
    wipe_native_value(typed.file_index_, observer);
    return;
}
case td::td_api::passportElementErrorSourceFiles::ID: {
    return;
}
case td::td_api::passportElementErrorSourceFrontSide::ID: {
    return;
}
case td::td_api::passportElementErrorSourceReverseSide::ID: {
    return;
}
case td::td_api::passportElementErrorSourceSelfie::ID: {
    return;
}
case td::td_api::passportElementErrorSourceTranslationFile::ID: {
    auto& typed = static_cast<td::td_api::passportElementErrorSourceTranslationFile&>(value);
    wipe_native_value(typed.file_index_, observer);
    return;
}
case td::td_api::passportElementErrorSourceTranslationFiles::ID: {
    return;
}
case td::td_api::passportElementErrorSourceUnspecified::ID: {
    return;
}
case td::td_api::passportElementIdentityCard::ID: {
    auto& typed = static_cast<td::td_api::passportElementIdentityCard&>(value);
    wipe_native_value(typed.identity_card_, observer);
    return;
}
case td::td_api::passportElementInternalPassport::ID: {
    auto& typed = static_cast<td::td_api::passportElementInternalPassport&>(value);
    wipe_native_value(typed.internal_passport_, observer);
    return;
}
case td::td_api::passportElementPassport::ID: {
    auto& typed = static_cast<td::td_api::passportElementPassport&>(value);
    wipe_native_value(typed.passport_, observer);
    return;
}
case td::td_api::passportElementPassportRegistration::ID: {
    auto& typed = static_cast<td::td_api::passportElementPassportRegistration&>(value);
    wipe_native_value(typed.passport_registration_, observer);
    return;
}
case td::td_api::passportElementPersonalDetails::ID: {
    auto& typed = static_cast<td::td_api::passportElementPersonalDetails&>(value);
    wipe_native_value(typed.personal_details_, observer);
    return;
}
case td::td_api::passportElementPhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::passportElementPhoneNumber&>(value);
    wipe_native_value(typed.phone_number_, observer);
    return;
}
case td::td_api::passportElementRentalAgreement::ID: {
    auto& typed = static_cast<td::td_api::passportElementRentalAgreement&>(value);
    wipe_native_value(typed.rental_agreement_, observer);
    return;
}
case td::td_api::passportElementTemporaryRegistration::ID: {
    auto& typed = static_cast<td::td_api::passportElementTemporaryRegistration&>(value);
    wipe_native_value(typed.temporary_registration_, observer);
    return;
}
case td::td_api::passportElementTypeAddress::ID: {
    return;
}
case td::td_api::passportElementTypeBankStatement::ID: {
    return;
}
case td::td_api::passportElementTypeDriverLicense::ID: {
    return;
}
case td::td_api::passportElementTypeEmailAddress::ID: {
    return;
}
case td::td_api::passportElementTypeIdentityCard::ID: {
    return;
}
case td::td_api::passportElementTypeInternalPassport::ID: {
    return;
}
case td::td_api::passportElementTypePassport::ID: {
    return;
}
case td::td_api::passportElementTypePassportRegistration::ID: {
    return;
}
case td::td_api::passportElementTypePersonalDetails::ID: {
    return;
}
case td::td_api::passportElementTypePhoneNumber::ID: {
    return;
}
case td::td_api::passportElementTypeRentalAgreement::ID: {
    return;
}
case td::td_api::passportElementTypeTemporaryRegistration::ID: {
    return;
}
case td::td_api::passportElementTypeUtilityBill::ID: {
    return;
}
case td::td_api::passportElementUtilityBill::ID: {
    auto& typed = static_cast<td::td_api::passportElementUtilityBill&>(value);
    wipe_native_value(typed.utility_bill_, observer);
    return;
}
case td::td_api::passportElements::ID: {
    auto& typed = static_cast<td::td_api::passportElements&>(value);
    wipe_native_value(typed.elements_, observer);
    return;
}
case td::td_api::passportElementsWithErrors::ID: {
    auto& typed = static_cast<td::td_api::passportElementsWithErrors&>(value);
    wipe_native_value(typed.elements_, observer);
    wipe_native_value(typed.errors_, observer);
    return;
}
case td::td_api::passportRequiredElement::ID: {
    auto& typed = static_cast<td::td_api::passportRequiredElement&>(value);
    wipe_native_value(typed.suitable_elements_, observer);
    return;
}
case td::td_api::passportSuitableElement::ID: {
    auto& typed = static_cast<td::td_api::passportSuitableElement&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.is_selfie_required_, observer);
    wipe_native_value(typed.is_translation_required_, observer);
    wipe_native_value(typed.is_native_name_required_, observer);
    return;
}
case td::td_api::passwordState::ID: {
    auto& typed = static_cast<td::td_api::passwordState&>(value);
    wipe_native_value(typed.has_password_, observer);
    wipe_native_value(typed.password_hint_, observer);
    wipe_native_value(typed.has_recovery_email_address_, observer);
    wipe_native_value(typed.has_passport_data_, observer);
    wipe_native_value(typed.recovery_email_address_code_info_, observer);
    wipe_native_value(typed.login_email_address_pattern_, observer);
    wipe_native_value(typed.pending_reset_date_, observer);
    return;
}
case td::td_api::paymentForm::ID: {
    auto& typed = static_cast<td::td_api::paymentForm&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.seller_bot_user_id_, observer);
    wipe_native_value(typed.product_info_, observer);
    return;
}
case td::td_api::paymentFormTypeRegular::ID: {
    auto& typed = static_cast<td::td_api::paymentFormTypeRegular&>(value);
    wipe_native_value(typed.invoice_, observer);
    wipe_native_value(typed.payment_provider_user_id_, observer);
    wipe_native_value(typed.payment_provider_, observer);
    wipe_native_value(typed.additional_payment_options_, observer);
    wipe_native_value(typed.saved_order_info_, observer);
    wipe_native_value(typed.saved_credentials_, observer);
    wipe_native_value(typed.can_save_credentials_, observer);
    wipe_native_value(typed.need_password_, observer);
    return;
}
case td::td_api::paymentFormTypeStarSubscription::ID: {
    auto& typed = static_cast<td::td_api::paymentFormTypeStarSubscription&>(value);
    wipe_native_value(typed.pricing_, observer);
    return;
}
case td::td_api::paymentFormTypeStars::ID: {
    auto& typed = static_cast<td::td_api::paymentFormTypeStars&>(value);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::paymentOption::ID: {
    auto& typed = static_cast<td::td_api::paymentOption&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::paymentProviderOther::ID: {
    auto& typed = static_cast<td::td_api::paymentProviderOther&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::paymentProviderSmartGlocal::ID: {
    auto& typed = static_cast<td::td_api::paymentProviderSmartGlocal&>(value);
    wipe_native_value(typed.public_token_, observer);
    wipe_native_value(typed.tokenize_url_, observer);
    return;
}
case td::td_api::paymentProviderStripe::ID: {
    auto& typed = static_cast<td::td_api::paymentProviderStripe&>(value);
    wipe_native_value(typed.publishable_key_, observer);
    wipe_native_value(typed.need_country_, observer);
    wipe_native_value(typed.need_postal_code_, observer);
    wipe_native_value(typed.need_cardholder_name_, observer);
    return;
}
case td::td_api::paymentReceipt::ID: {
    auto& typed = static_cast<td::td_api::paymentReceipt&>(value);
    wipe_native_value(typed.product_info_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.seller_bot_user_id_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::paymentReceiptTypeRegular::ID: {
    auto& typed = static_cast<td::td_api::paymentReceiptTypeRegular&>(value);
    wipe_native_value(typed.payment_provider_user_id_, observer);
    wipe_native_value(typed.invoice_, observer);
    wipe_native_value(typed.order_info_, observer);
    wipe_native_value(typed.shipping_option_, observer);
    wipe_native_value(typed.credentials_title_, observer);
    wipe_native_value(typed.tip_amount_, observer);
    return;
}
case td::td_api::paymentReceiptTypeStars::ID: {
    auto& typed = static_cast<td::td_api::paymentReceiptTypeStars&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.transaction_id_, observer);
    return;
}
case td::td_api::paymentResult::ID: {
    auto& typed = static_cast<td::td_api::paymentResult&>(value);
    wipe_native_value(typed.success_, observer);
    wipe_native_value(typed.verification_url_, observer);
    return;
}
case td::td_api::personalDetails::ID: {
    auto& typed = static_cast<td::td_api::personalDetails&>(value);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.middle_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    wipe_native_value(typed.native_first_name_, observer);
    wipe_native_value(typed.native_middle_name_, observer);
    wipe_native_value(typed.native_last_name_, observer);
    wipe_native_value(typed.birthdate_, observer);
    wipe_native_value(typed.gender_, observer);
    wipe_native_value(typed.country_code_, observer);
    wipe_native_value(typed.residence_country_code_, observer);
    return;
}
case td::td_api::personalDocument::ID: {
    auto& typed = static_cast<td::td_api::personalDocument&>(value);
    wipe_native_value(typed.files_, observer);
    wipe_native_value(typed.translation_, observer);
    return;
}
case td::td_api::phoneNumberAuthenticationSettings::ID: {
    auto& typed = static_cast<td::td_api::phoneNumberAuthenticationSettings&>(value);
    wipe_native_value(typed.allow_flash_call_, observer);
    wipe_native_value(typed.allow_missed_call_, observer);
    wipe_native_value(typed.is_current_phone_number_, observer);
    wipe_native_value(typed.has_unknown_phone_number_, observer);
    wipe_native_value(typed.allow_sms_retriever_api_, observer);
    wipe_native_value(typed.firebase_authentication_settings_, observer);
    wipe_native_value(typed.authentication_tokens_, observer);
    return;
}
case td::td_api::phoneNumberCodeTypeChange::ID: {
    return;
}
case td::td_api::phoneNumberCodeTypeConfirmOwnership::ID: {
    auto& typed = static_cast<td::td_api::phoneNumberCodeTypeConfirmOwnership&>(value);
    wipe_native_value(typed.hash_, observer);
    return;
}
case td::td_api::phoneNumberCodeTypeVerify::ID: {
    return;
}
case td::td_api::phoneNumberInfo::ID: {
    auto& typed = static_cast<td::td_api::phoneNumberInfo&>(value);
    wipe_native_value(typed.country_, observer);
    wipe_native_value(typed.country_calling_code_, observer);
    wipe_native_value(typed.formatted_phone_number_, observer);
    wipe_native_value(typed.is_anonymous_, observer);
    return;
}
case td::td_api::photo::ID: {
    auto& typed = static_cast<td::td_api::photo&>(value);
    wipe_native_value(typed.has_stickers_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.sizes_, observer);
    return;
}
case td::td_api::photoSize::ID: {
    auto& typed = static_cast<td::td_api::photoSize&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.progressive_sizes_, observer);
    return;
}
case td::td_api::point::ID: {
    auto& typed = static_cast<td::td_api::point&>(value);
    wipe_native_value(typed.x_, observer);
    wipe_native_value(typed.y_, observer);
    return;
}
case td::td_api::poll::ID: {
    auto& typed = static_cast<td::td_api::poll&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.question_, observer);
    wipe_native_value(typed.options_, observer);
    wipe_native_value(typed.total_voter_count_, observer);
    wipe_native_value(typed.recent_voter_ids_, observer);
    wipe_native_value(typed.can_get_voters_, observer);
    wipe_native_value(typed.can_see_results_, observer);
    wipe_native_value(typed.is_anonymous_, observer);
    wipe_native_value(typed.allows_multiple_answers_, observer);
    wipe_native_value(typed.allows_revoting_, observer);
    wipe_native_value(typed.members_only_, observer);
    wipe_native_value(typed.country_codes_, observer);
    wipe_native_value(typed.option_order_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.open_period_, observer);
    wipe_native_value(typed.close_date_, observer);
    wipe_native_value(typed.is_closed_, observer);
    wipe_native_value(typed.vote_restriction_reason_, observer);
    return;
}
case td::td_api::pollMediaAnimation::ID: {
    auto& typed = static_cast<td::td_api::pollMediaAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::pollMediaAudio::ID: {
    auto& typed = static_cast<td::td_api::pollMediaAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    return;
}
case td::td_api::pollMediaDocument::ID: {
    auto& typed = static_cast<td::td_api::pollMediaDocument&>(value);
    wipe_native_value(typed.document_, observer);
    return;
}
case td::td_api::pollMediaLink::ID: {
    auto& typed = static_cast<td::td_api::pollMediaLink&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.link_preview_, observer);
    return;
}
case td::td_api::pollMediaLocation::ID: {
    auto& typed = static_cast<td::td_api::pollMediaLocation&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::pollMediaPhoto::ID: {
    auto& typed = static_cast<td::td_api::pollMediaPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::pollMediaSticker::ID: {
    auto& typed = static_cast<td::td_api::pollMediaSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::pollMediaVenue::ID: {
    auto& typed = static_cast<td::td_api::pollMediaVenue&>(value);
    wipe_native_value(typed.venue_, observer);
    return;
}
case td::td_api::pollMediaVideo::ID: {
    auto& typed = static_cast<td::td_api::pollMediaVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.alternative_videos_, observer);
    wipe_native_value(typed.storyboards_, observer);
    wipe_native_value(typed.cover_, observer);
    wipe_native_value(typed.start_timestamp_, observer);
    return;
}
case td::td_api::pollOption::ID: {
    auto& typed = static_cast<td::td_api::pollOption&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.voter_count_, observer);
    wipe_native_value(typed.vote_percentage_, observer);
    wipe_native_value(typed.recent_voter_ids_, observer);
    wipe_native_value(typed.is_chosen_, observer);
    wipe_native_value(typed.is_being_chosen_, observer);
    wipe_native_value(typed.author_, observer);
    wipe_native_value(typed.addition_date_, observer);
    return;
}
case td::td_api::pollOptionProperties::ID: {
    auto& typed = static_cast<td::td_api::pollOptionProperties&>(value);
    wipe_native_value(typed.can_be_deleted_, observer);
    wipe_native_value(typed.can_be_replied_, observer);
    wipe_native_value(typed.can_be_replied_in_another_chat_, observer);
    wipe_native_value(typed.can_get_link_, observer);
    return;
}
case td::td_api::pollTypeQuiz::ID: {
    auto& typed = static_cast<td::td_api::pollTypeQuiz&>(value);
    wipe_native_value(typed.correct_option_ids_, observer);
    wipe_native_value(typed.explanation_, observer);
    wipe_native_value(typed.explanation_media_, observer);
    return;
}
case td::td_api::pollTypeRegular::ID: {
    return;
}
case td::td_api::pollVoteRestrictionReasonClosed::ID: {
    return;
}
case td::td_api::pollVoteRestrictionReasonCountryRestricted::ID: {
    auto& typed = static_cast<td::td_api::pollVoteRestrictionReasonCountryRestricted&>(value);
    wipe_native_value(typed.country_code_, observer);
    return;
}
case td::td_api::pollVoteRestrictionReasonMembershipRequired::ID: {
    auto& typed = static_cast<td::td_api::pollVoteRestrictionReasonMembershipRequired&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::pollVoteRestrictionReasonOther::ID: {
    return;
}
case td::td_api::pollVoteRestrictionReasonScheduled::ID: {
    return;
}
case td::td_api::pollVoteRestrictionReasonYetUnsent::ID: {
    return;
}
case td::td_api::pollVoteStatistics::ID: {
    auto& typed = static_cast<td::td_api::pollVoteStatistics&>(value);
    wipe_native_value(typed.vote_graph_, observer);
    return;
}
case td::td_api::pollVoter::ID: {
    auto& typed = static_cast<td::td_api::pollVoter&>(value);
    wipe_native_value(typed.voter_id_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::pollVoters::ID: {
    auto& typed = static_cast<td::td_api::pollVoters&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.voters_, observer);
    return;
}
case td::td_api::premiumFeatureAccentColor::ID: {
    return;
}
case td::td_api::premiumFeatureAdvancedChatManagement::ID: {
    return;
}
case td::td_api::premiumFeatureAnimatedProfilePhoto::ID: {
    return;
}
case td::td_api::premiumFeatureAppIcons::ID: {
    return;
}
case td::td_api::premiumFeatureBackgroundForBoth::ID: {
    return;
}
case td::td_api::premiumFeatureBusiness::ID: {
    return;
}
case td::td_api::premiumFeatureChatBoost::ID: {
    return;
}
case td::td_api::premiumFeatureChecklists::ID: {
    return;
}
case td::td_api::premiumFeatureCustomEmoji::ID: {
    return;
}
case td::td_api::premiumFeatureDisabledAds::ID: {
    return;
}
case td::td_api::premiumFeatureEmojiStatus::ID: {
    return;
}
case td::td_api::premiumFeatureForumTopicIcon::ID: {
    return;
}
case td::td_api::premiumFeatureImprovedDownloadSpeed::ID: {
    return;
}
case td::td_api::premiumFeatureIncreasedLimits::ID: {
    return;
}
case td::td_api::premiumFeatureIncreasedUploadFileSize::ID: {
    return;
}
case td::td_api::premiumFeatureLastSeenTimes::ID: {
    return;
}
case td::td_api::premiumFeatureMessageEffects::ID: {
    return;
}
case td::td_api::premiumFeatureMessagePrivacy::ID: {
    return;
}
case td::td_api::premiumFeaturePaidMessages::ID: {
    return;
}
case td::td_api::premiumFeatureProfileBadge::ID: {
    return;
}
case td::td_api::premiumFeaturePromotionAnimation::ID: {
    auto& typed = static_cast<td::td_api::premiumFeaturePromotionAnimation&>(value);
    wipe_native_value(typed.feature_, observer);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::premiumFeatureProtectPrivateChatContent::ID: {
    return;
}
case td::td_api::premiumFeatureRealTimeChatTranslation::ID: {
    return;
}
case td::td_api::premiumFeatureSavedMessagesTags::ID: {
    return;
}
case td::td_api::premiumFeatureTextComposition::ID: {
    return;
}
case td::td_api::premiumFeatureUniqueReactions::ID: {
    return;
}
case td::td_api::premiumFeatureUniqueStickers::ID: {
    return;
}
case td::td_api::premiumFeatureUpgradedStories::ID: {
    return;
}
case td::td_api::premiumFeatureVoiceRecognition::ID: {
    return;
}
case td::td_api::premiumFeatures::ID: {
    auto& typed = static_cast<td::td_api::premiumFeatures&>(value);
    wipe_native_value(typed.features_, observer);
    wipe_native_value(typed.limits_, observer);
    wipe_native_value(typed.payment_link_, observer);
    return;
}
case td::td_api::premiumGiftCodeInfo::ID: {
    auto& typed = static_cast<td::td_api::premiumGiftCodeInfo&>(value);
    wipe_native_value(typed.creator_id_, observer);
    wipe_native_value(typed.creation_date_, observer);
    wipe_native_value(typed.is_from_giveaway_, observer);
    wipe_native_value(typed.giveaway_message_id_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.day_count_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.use_date_, observer);
    return;
}
case td::td_api::premiumGiftPaymentOption::ID: {
    auto& typed = static_cast<td::td_api::premiumGiftPaymentOption&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.discount_percentage_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::premiumGiftPaymentOptions::ID: {
    auto& typed = static_cast<td::td_api::premiumGiftPaymentOptions&>(value);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::premiumGiveawayPaymentOption::ID: {
    auto& typed = static_cast<td::td_api::premiumGiveawayPaymentOption&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.store_product_quantity_, observer);
    return;
}
case td::td_api::premiumGiveawayPaymentOptions::ID: {
    auto& typed = static_cast<td::td_api::premiumGiveawayPaymentOptions&>(value);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::premiumLimit::ID: {
    auto& typed = static_cast<td::td_api::premiumLimit&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.default_value_, observer);
    wipe_native_value(typed.premium_value_, observer);
    return;
}
case td::td_api::premiumLimitTypeActiveStoryCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeBioLength::ID: {
    return;
}
case td::td_api::premiumLimitTypeCaptionLength::ID: {
    return;
}
case td::td_api::premiumLimitTypeChatFolderChosenChatCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeChatFolderCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeChatFolderInviteLinkCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeCreatedPublicChatCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeCustomTextCompositionStyleCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeFavoriteStickerCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeMessageTextLength::ID: {
    return;
}
case td::td_api::premiumLimitTypeMonthlyPostedStoryCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeOwnedBotCount::ID: {
    return;
}
case td::td_api::premiumLimitTypePinnedArchivedChatCount::ID: {
    return;
}
case td::td_api::premiumLimitTypePinnedChatCount::ID: {
    return;
}
case td::td_api::premiumLimitTypePinnedSavedMessagesTopicCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeSavedAnimationCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeShareableChatFolderCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeSimilarChatCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeStoryCaptionLength::ID: {
    return;
}
case td::td_api::premiumLimitTypeStorySuggestedReactionAreaCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeSupergroupCount::ID: {
    return;
}
case td::td_api::premiumLimitTypeWeeklyPostedStoryCount::ID: {
    return;
}
case td::td_api::premiumPaymentOption::ID: {
    auto& typed = static_cast<td::td_api::premiumPaymentOption&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.discount_percentage_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.payment_link_, observer);
    return;
}
case td::td_api::premiumSourceBusinessFeature::ID: {
    auto& typed = static_cast<td::td_api::premiumSourceBusinessFeature&>(value);
    wipe_native_value(typed.feature_, observer);
    return;
}
case td::td_api::premiumSourceFeature::ID: {
    auto& typed = static_cast<td::td_api::premiumSourceFeature&>(value);
    wipe_native_value(typed.feature_, observer);
    return;
}
case td::td_api::premiumSourceLimitExceeded::ID: {
    auto& typed = static_cast<td::td_api::premiumSourceLimitExceeded&>(value);
    wipe_native_value(typed.limit_type_, observer);
    return;
}
case td::td_api::premiumSourceLink::ID: {
    auto& typed = static_cast<td::td_api::premiumSourceLink&>(value);
    wipe_native_value(typed.referrer_, observer);
    return;
}
case td::td_api::premiumSourceSettings::ID: {
    return;
}
case td::td_api::premiumSourceStoryFeature::ID: {
    auto& typed = static_cast<td::td_api::premiumSourceStoryFeature&>(value);
    wipe_native_value(typed.feature_, observer);
    return;
}
case td::td_api::premiumState::ID: {
    auto& typed = static_cast<td::td_api::premiumState&>(value);
    wipe_native_value(typed.state_, observer);
    wipe_native_value(typed.payment_options_, observer);
    wipe_native_value(typed.animations_, observer);
    wipe_native_value(typed.business_animations_, observer);
    return;
}
case td::td_api::premiumStatePaymentOption::ID: {
    auto& typed = static_cast<td::td_api::premiumStatePaymentOption&>(value);
    wipe_native_value(typed.payment_option_, observer);
    wipe_native_value(typed.is_current_, observer);
    wipe_native_value(typed.is_upgrade_, observer);
    wipe_native_value(typed.last_transaction_id_, observer);
    return;
}
case td::td_api::premiumStoryFeatureCustomExpirationDuration::ID: {
    return;
}
case td::td_api::premiumStoryFeatureLinksAndFormatting::ID: {
    return;
}
case td::td_api::premiumStoryFeaturePermanentViewsHistory::ID: {
    return;
}
case td::td_api::premiumStoryFeaturePriorityOrder::ID: {
    return;
}
case td::td_api::premiumStoryFeatureSaveStories::ID: {
    return;
}
case td::td_api::premiumStoryFeatureStealthMode::ID: {
    return;
}
case td::td_api::premiumStoryFeatureVideoQuality::ID: {
    return;
}
case td::td_api::prepaidGiveaway::ID: {
    auto& typed = static_cast<td::td_api::prepaidGiveaway&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.prize_, observer);
    wipe_native_value(typed.boost_count_, observer);
    wipe_native_value(typed.payment_date_, observer);
    return;
}
case td::td_api::preparedInlineMessage::ID: {
    auto& typed = static_cast<td::td_api::preparedInlineMessage&>(value);
    wipe_native_value(typed.inline_query_id_, observer);
    wipe_native_value(typed.result_, observer);
    wipe_native_value(typed.chat_types_, observer);
    return;
}
case td::td_api::preparedInlineMessageId::ID: {
    auto& typed = static_cast<td::td_api::preparedInlineMessageId&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    return;
}
case td::td_api::productInfo::ID: {
    auto& typed = static_cast<td::td_api::productInfo&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::profileAccentColor::ID: {
    auto& typed = static_cast<td::td_api::profileAccentColor&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.light_theme_colors_, observer);
    wipe_native_value(typed.dark_theme_colors_, observer);
    wipe_native_value(typed.min_supergroup_chat_boost_level_, observer);
    wipe_native_value(typed.min_channel_chat_boost_level_, observer);
    return;
}
case td::td_api::profileAccentColors::ID: {
    auto& typed = static_cast<td::td_api::profileAccentColors&>(value);
    wipe_native_value(typed.palette_colors_, observer);
    wipe_native_value(typed.background_colors_, observer);
    wipe_native_value(typed.story_colors_, observer);
    return;
}
case td::td_api::profilePhoto::ID: {
    auto& typed = static_cast<td::td_api::profilePhoto&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.small_, observer);
    wipe_native_value(typed.big_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.has_animation_, observer);
    wipe_native_value(typed.is_personal_, observer);
    return;
}
case td::td_api::profileTabFiles::ID: {
    return;
}
case td::td_api::profileTabGifs::ID: {
    return;
}
case td::td_api::profileTabGifts::ID: {
    return;
}
case td::td_api::profileTabLinks::ID: {
    return;
}
case td::td_api::profileTabMedia::ID: {
    return;
}
case td::td_api::profileTabMusic::ID: {
    return;
}
case td::td_api::profileTabPosts::ID: {
    return;
}
case td::td_api::profileTabVoice::ID: {
    return;
}
case td::td_api::proxy::ID: {
    auto& typed = static_cast<td::td_api::proxy&>(value);
    wipe_native_value(typed.server_, observer);
    wipe_native_value(typed.port_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::proxyTypeHttp::ID: {
    auto& typed = static_cast<td::td_api::proxyTypeHttp&>(value);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.password_, observer);
    wipe_native_value(typed.http_only_, observer);
    return;
}
case td::td_api::proxyTypeMtproto::ID: {
    auto& typed = static_cast<td::td_api::proxyTypeMtproto&>(value);
    wipe_native_value(typed.secret_, observer);
    return;
}
case td::td_api::proxyTypeSocks5::ID: {
    auto& typed = static_cast<td::td_api::proxyTypeSocks5&>(value);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::publicChatTypeHasUsername::ID: {
    return;
}
case td::td_api::publicChatTypeIsLocationBased::ID: {
    return;
}
case td::td_api::publicForwardMessage::ID: {
    auto& typed = static_cast<td::td_api::publicForwardMessage&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::publicForwardStory::ID: {
    auto& typed = static_cast<td::td_api::publicForwardStory&>(value);
    wipe_native_value(typed.story_, observer);
    return;
}
case td::td_api::publicForwards::ID: {
    auto& typed = static_cast<td::td_api::publicForwards&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.forwards_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::publicPostSearchLimits::ID: {
    auto& typed = static_cast<td::td_api::publicPostSearchLimits&>(value);
    wipe_native_value(typed.daily_free_query_count_, observer);
    wipe_native_value(typed.remaining_free_query_count_, observer);
    wipe_native_value(typed.next_free_query_in_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.is_current_query_free_, observer);
    return;
}
case td::td_api::pushMessageContentAnimation::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentAudio::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentBasicGroupChatCreate::ID: {
    return;
}
case td::td_api::pushMessageContentChatAddMembers::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChatAddMembers&>(value);
    wipe_native_value(typed.member_name_, observer);
    wipe_native_value(typed.is_current_user_, observer);
    wipe_native_value(typed.is_returned_, observer);
    return;
}
case td::td_api::pushMessageContentChatChangePhoto::ID: {
    return;
}
case td::td_api::pushMessageContentChatChangeTitle::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChatChangeTitle&>(value);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::pushMessageContentChatDeleteMember::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChatDeleteMember&>(value);
    wipe_native_value(typed.member_name_, observer);
    wipe_native_value(typed.is_current_user_, observer);
    wipe_native_value(typed.is_left_, observer);
    return;
}
case td::td_api::pushMessageContentChatJoinByLink::ID: {
    return;
}
case td::td_api::pushMessageContentChatJoinByRequest::ID: {
    return;
}
case td::td_api::pushMessageContentChatSetBackground::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChatSetBackground&>(value);
    wipe_native_value(typed.is_same_, observer);
    return;
}
case td::td_api::pushMessageContentChatSetTheme::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChatSetTheme&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::pushMessageContentChecklist::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChecklist&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentChecklistTasksAdded::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChecklistTasksAdded&>(value);
    wipe_native_value(typed.task_count_, observer);
    return;
}
case td::td_api::pushMessageContentChecklistTasksDone::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentChecklistTasksDone&>(value);
    wipe_native_value(typed.task_count_, observer);
    return;
}
case td::td_api::pushMessageContentContact::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentContact&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentContactRegistered::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentContactRegistered&>(value);
    wipe_native_value(typed.as_premium_account_, observer);
    return;
}
case td::td_api::pushMessageContentDocument::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentDocument&>(value);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentGame::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentGame&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentGameScore::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentGameScore&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.score_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentGift::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentGift&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.is_prepaid_upgrade_, observer);
    return;
}
case td::td_api::pushMessageContentGiveaway::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentGiveaway&>(value);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.prize_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentHidden::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentHidden&>(value);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentInviteVideoChatParticipants::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentInviteVideoChatParticipants&>(value);
    wipe_native_value(typed.is_current_user_, observer);
    return;
}
case td::td_api::pushMessageContentInvoice::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentInvoice&>(value);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentLocation::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentLocation&>(value);
    wipe_native_value(typed.is_live_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentMediaAlbum::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentMediaAlbum&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.has_photos_, observer);
    wipe_native_value(typed.has_videos_, observer);
    wipe_native_value(typed.has_audios_, observer);
    wipe_native_value(typed.has_documents_, observer);
    return;
}
case td::td_api::pushMessageContentMessageForwards::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentMessageForwards&>(value);
    wipe_native_value(typed.total_count_, observer);
    return;
}
case td::td_api::pushMessageContentPaidMedia::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentPaidMedia&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentPhoto::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.is_secret_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentPoll::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentPoll&>(value);
    wipe_native_value(typed.question_, observer);
    wipe_native_value(typed.is_regular_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentPollOptionAdded::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentPollOptionAdded&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::pushMessageContentPremiumGiftCode::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentPremiumGiftCode&>(value);
    wipe_native_value(typed.month_count_, observer);
    return;
}
case td::td_api::pushMessageContentProximityAlertTriggered::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentProximityAlertTriggered&>(value);
    wipe_native_value(typed.distance_, observer);
    return;
}
case td::td_api::pushMessageContentRecurringPayment::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentRecurringPayment&>(value);
    wipe_native_value(typed.amount_, observer);
    return;
}
case td::td_api::pushMessageContentScreenshotTaken::ID: {
    return;
}
case td::td_api::pushMessageContentSticker::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentStory::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentStory&>(value);
    wipe_native_value(typed.is_mention_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentSuggestBirthdate::ID: {
    return;
}
case td::td_api::pushMessageContentSuggestProfilePhoto::ID: {
    return;
}
case td::td_api::pushMessageContentText::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentUpgradedGift&>(value);
    wipe_native_value(typed.is_upgrade_, observer);
    wipe_native_value(typed.is_prepaid_upgrade_, observer);
    return;
}
case td::td_api::pushMessageContentVideo::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.is_secret_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentVideoChatEnded::ID: {
    return;
}
case td::td_api::pushMessageContentVideoChatStarted::ID: {
    return;
}
case td::td_api::pushMessageContentVideoNote::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentVideoNote&>(value);
    wipe_native_value(typed.video_note_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushMessageContentVoiceNote::ID: {
    auto& typed = static_cast<td::td_api::pushMessageContentVoiceNote&>(value);
    wipe_native_value(typed.voice_note_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::pushReceiverId::ID: {
    auto& typed = static_cast<td::td_api::pushReceiverId&>(value);
    wipe_native_value(typed.id_, observer);
    return;
}
case td::td_api::quickReplyMessage::ID: {
    auto& typed = static_cast<td::td_api::quickReplyMessage&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sending_state_, observer);
    wipe_native_value(typed.can_be_edited_, observer);
    wipe_native_value(typed.reply_to_message_id_, observer);
    wipe_native_value(typed.via_bot_user_id_, observer);
    wipe_native_value(typed.media_album_id_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::quickReplyMessages::ID: {
    auto& typed = static_cast<td::td_api::quickReplyMessages&>(value);
    wipe_native_value(typed.messages_, observer);
    return;
}
case td::td_api::quickReplyShortcut::ID: {
    auto& typed = static_cast<td::td_api::quickReplyShortcut&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.first_message_, observer);
    wipe_native_value(typed.message_count_, observer);
    return;
}
case td::td_api::reactionNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::reactionNotificationSettings&>(value);
    wipe_native_value(typed.message_reaction_source_, observer);
    wipe_native_value(typed.story_reaction_source_, observer);
    wipe_native_value(typed.poll_vote_source_, observer);
    wipe_native_value(typed.sound_id_, observer);
    wipe_native_value(typed.show_preview_, observer);
    return;
}
case td::td_api::reactionNotificationSourceAll::ID: {
    return;
}
case td::td_api::reactionNotificationSourceContacts::ID: {
    return;
}
case td::td_api::reactionNotificationSourceNone::ID: {
    return;
}
case td::td_api::reactionTypeCustomEmoji::ID: {
    auto& typed = static_cast<td::td_api::reactionTypeCustomEmoji&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::reactionTypeEmoji::ID: {
    auto& typed = static_cast<td::td_api::reactionTypeEmoji&>(value);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::reactionTypePaid::ID: {
    return;
}
case td::td_api::reactionUnavailabilityReasonAnonymousAdministrator::ID: {
    return;
}
case td::td_api::reactionUnavailabilityReasonGuest::ID: {
    return;
}
case td::td_api::reactionUnavailabilityReasonRestricted::ID: {
    return;
}
case td::td_api::readDatePrivacySettings::ID: {
    auto& typed = static_cast<td::td_api::readDatePrivacySettings&>(value);
    wipe_native_value(typed.show_read_date_, observer);
    return;
}
case td::td_api::receivedGift::ID: {
    auto& typed = static_cast<td::td_api::receivedGift&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.unique_gift_number_, observer);
    wipe_native_value(typed.is_private_, observer);
    wipe_native_value(typed.is_saved_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    wipe_native_value(typed.can_be_upgraded_, observer);
    wipe_native_value(typed.can_be_transferred_, observer);
    wipe_native_value(typed.was_refunded_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.collection_ids_, observer);
    wipe_native_value(typed.sell_star_count_, observer);
    wipe_native_value(typed.prepaid_upgrade_star_count_, observer);
    wipe_native_value(typed.is_upgrade_separate_, observer);
    wipe_native_value(typed.transfer_star_count_, observer);
    wipe_native_value(typed.drop_original_details_star_count_, observer);
    wipe_native_value(typed.next_transfer_date_, observer);
    wipe_native_value(typed.next_resale_date_, observer);
    wipe_native_value(typed.export_date_, observer);
    wipe_native_value(typed.prepaid_upgrade_hash_, observer);
    wipe_native_value(typed.craft_date_, observer);
    return;
}
case td::td_api::receivedGifts::ID: {
    auto& typed = static_cast<td::td_api::receivedGifts&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.gifts_, observer);
    wipe_native_value(typed.are_notifications_enabled_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::recommendedChatFolder::ID: {
    auto& typed = static_cast<td::td_api::recommendedChatFolder&>(value);
    wipe_native_value(typed.folder_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::recommendedChatFolders::ID: {
    auto& typed = static_cast<td::td_api::recommendedChatFolders&>(value);
    wipe_native_value(typed.chat_folders_, observer);
    return;
}
case td::td_api::recoveryEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::recoveryEmailAddress&>(value);
    wipe_native_value(typed.recovery_email_address_, observer);
    return;
}
case td::td_api::remoteFile::ID: {
    auto& typed = static_cast<td::td_api::remoteFile&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.is_uploading_active_, observer);
    wipe_native_value(typed.is_uploading_completed_, observer);
    wipe_native_value(typed.uploaded_size_, observer);
    return;
}
case td::td_api::replyMarkupForceReply::ID: {
    auto& typed = static_cast<td::td_api::replyMarkupForceReply&>(value);
    wipe_native_value(typed.is_personal_, observer);
    wipe_native_value(typed.input_field_placeholder_, observer);
    return;
}
case td::td_api::replyMarkupInlineKeyboard::ID: {
    auto& typed = static_cast<td::td_api::replyMarkupInlineKeyboard&>(value);
    wipe_native_value(typed.rows_, observer);
    return;
}
case td::td_api::replyMarkupRemoveKeyboard::ID: {
    auto& typed = static_cast<td::td_api::replyMarkupRemoveKeyboard&>(value);
    wipe_native_value(typed.is_personal_, observer);
    return;
}
case td::td_api::replyMarkupShowKeyboard::ID: {
    auto& typed = static_cast<td::td_api::replyMarkupShowKeyboard&>(value);
    wipe_native_value(typed.rows_, observer);
    wipe_native_value(typed.is_persistent_, observer);
    wipe_native_value(typed.resize_keyboard_, observer);
    wipe_native_value(typed.one_time_, observer);
    wipe_native_value(typed.is_personal_, observer);
    wipe_native_value(typed.input_field_placeholder_, observer);
    return;
}
case td::td_api::reportChatResultMessagesRequired::ID: {
    return;
}
case td::td_api::reportChatResultOk::ID: {
    return;
}
case td::td_api::reportChatResultOptionRequired::ID: {
    auto& typed = static_cast<td::td_api::reportChatResultOptionRequired&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::reportChatResultTextRequired::ID: {
    auto& typed = static_cast<td::td_api::reportChatResultTextRequired&>(value);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.is_optional_, observer);
    return;
}
case td::td_api::reportOption::ID: {
    auto& typed = static_cast<td::td_api::reportOption&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::reportReasonChildAbuse::ID: {
    return;
}
case td::td_api::reportReasonCopyright::ID: {
    return;
}
case td::td_api::reportReasonCustom::ID: {
    return;
}
case td::td_api::reportReasonFake::ID: {
    return;
}
case td::td_api::reportReasonIllegalDrugs::ID: {
    return;
}
case td::td_api::reportReasonPersonalDetails::ID: {
    return;
}
case td::td_api::reportReasonPornography::ID: {
    return;
}
case td::td_api::reportReasonSpam::ID: {
    return;
}
case td::td_api::reportReasonUnrelatedLocation::ID: {
    return;
}
case td::td_api::reportReasonViolence::ID: {
    return;
}
case td::td_api::reportSponsoredResultAdsHidden::ID: {
    return;
}
case td::td_api::reportSponsoredResultFailed::ID: {
    return;
}
case td::td_api::reportSponsoredResultOk::ID: {
    return;
}
case td::td_api::reportSponsoredResultOptionRequired::ID: {
    auto& typed = static_cast<td::td_api::reportSponsoredResultOptionRequired&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::reportSponsoredResultPremiumRequired::ID: {
    return;
}
case td::td_api::reportStoryResultOk::ID: {
    return;
}
case td::td_api::reportStoryResultOptionRequired::ID: {
    auto& typed = static_cast<td::td_api::reportStoryResultOptionRequired&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::reportStoryResultTextRequired::ID: {
    auto& typed = static_cast<td::td_api::reportStoryResultTextRequired&>(value);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.is_optional_, observer);
    return;
}
case td::td_api::resendCodeReasonUserRequest::ID: {
    return;
}
case td::td_api::resendCodeReasonVerificationFailed::ID: {
    auto& typed = static_cast<td::td_api::resendCodeReasonVerificationFailed&>(value);
    wipe_native_value(typed.error_message_, observer);
    return;
}
case td::td_api::resetPasswordResultDeclined::ID: {
    auto& typed = static_cast<td::td_api::resetPasswordResultDeclined&>(value);
    wipe_native_value(typed.retry_date_, observer);
    return;
}
case td::td_api::resetPasswordResultOk::ID: {
    return;
}
case td::td_api::resetPasswordResultPending::ID: {
    auto& typed = static_cast<td::td_api::resetPasswordResultPending&>(value);
    wipe_native_value(typed.pending_reset_date_, observer);
    return;
}
case td::td_api::restrictionInfo::ID: {
    auto& typed = static_cast<td::td_api::restrictionInfo&>(value);
    wipe_native_value(typed.restriction_reason_, observer);
    wipe_native_value(typed.has_sensitive_content_, observer);
    return;
}
case td::td_api::revenueWithdrawalStateFailed::ID: {
    return;
}
case td::td_api::revenueWithdrawalStatePending::ID: {
    return;
}
case td::td_api::revenueWithdrawalStateSucceeded::ID: {
    auto& typed = static_cast<td::td_api::revenueWithdrawalStateSucceeded&>(value);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::richMessage::ID: {
    auto& typed = static_cast<td::td_api::richMessage&>(value);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.is_rtl_, observer);
    wipe_native_value(typed.is_full_, observer);
    return;
}
case td::td_api::richMessageSourceHtml::ID: {
    auto& typed = static_cast<td::td_api::richMessageSourceHtml&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richMessageSourceMarkdown::ID: {
    auto& typed = static_cast<td::td_api::richMessageSourceMarkdown&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextAnchor::ID: {
    auto& typed = static_cast<td::td_api::richTextAnchor&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::richTextAnchorLink::ID: {
    auto& typed = static_cast<td::td_api::richTextAnchorLink&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.anchor_name_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::richTextBankCardNumber::ID: {
    auto& typed = static_cast<td::td_api::richTextBankCardNumber&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.bank_card_number_, observer);
    return;
}
case td::td_api::richTextBold::ID: {
    auto& typed = static_cast<td::td_api::richTextBold&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextBotCommand::ID: {
    auto& typed = static_cast<td::td_api::richTextBotCommand&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.bot_command_, observer);
    return;
}
case td::td_api::richTextCashtag::ID: {
    auto& typed = static_cast<td::td_api::richTextCashtag&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.cashtag_, observer);
    return;
}
case td::td_api::richTextCustomEmoji::ID: {
    auto& typed = static_cast<td::td_api::richTextCustomEmoji&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    wipe_native_value(typed.alternative_text_, observer);
    return;
}
case td::td_api::richTextDateTime::ID: {
    auto& typed = static_cast<td::td_api::richTextDateTime&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.unix_time_, observer);
    wipe_native_value(typed.formatting_type_, observer);
    return;
}
case td::td_api::richTextEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::richTextEmailAddress&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.email_address_, observer);
    return;
}
case td::td_api::richTextFixed::ID: {
    auto& typed = static_cast<td::td_api::richTextFixed&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextHashtag::ID: {
    auto& typed = static_cast<td::td_api::richTextHashtag&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.hashtag_, observer);
    return;
}
case td::td_api::richTextIcon::ID: {
    auto& typed = static_cast<td::td_api::richTextIcon&>(value);
    wipe_native_value(typed.document_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    return;
}
case td::td_api::richTextItalic::ID: {
    auto& typed = static_cast<td::td_api::richTextItalic&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextMarked::ID: {
    auto& typed = static_cast<td::td_api::richTextMarked&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextMathematicalExpression::ID: {
    auto& typed = static_cast<td::td_api::richTextMathematicalExpression&>(value);
    wipe_native_value(typed.expression_, observer);
    return;
}
case td::td_api::richTextMention::ID: {
    auto& typed = static_cast<td::td_api::richTextMention&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::richTextMentionName::ID: {
    auto& typed = static_cast<td::td_api::richTextMentionName&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::richTextPhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::richTextPhoneNumber&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.phone_number_, observer);
    return;
}
case td::td_api::richTextPlain::ID: {
    auto& typed = static_cast<td::td_api::richTextPlain&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextReference::ID: {
    auto& typed = static_cast<td::td_api::richTextReference&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextReferenceLink::ID: {
    auto& typed = static_cast<td::td_api::richTextReferenceLink&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.reference_name_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::richTextSpoiler::ID: {
    auto& typed = static_cast<td::td_api::richTextSpoiler&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextStrikethrough::ID: {
    auto& typed = static_cast<td::td_api::richTextStrikethrough&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextSubscript::ID: {
    auto& typed = static_cast<td::td_api::richTextSubscript&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextSuperscript::ID: {
    auto& typed = static_cast<td::td_api::richTextSuperscript&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextUnderline::ID: {
    auto& typed = static_cast<td::td_api::richTextUnderline&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::richTextUrl::ID: {
    auto& typed = static_cast<td::td_api::richTextUrl&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.is_cached_, observer);
    return;
}
case td::td_api::richTexts::ID: {
    auto& typed = static_cast<td::td_api::richTexts&>(value);
    wipe_native_value(typed.texts_, observer);
    return;
}
case td::td_api::rtmpUrl::ID: {
    auto& typed = static_cast<td::td_api::rtmpUrl&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.stream_key_, observer);
    return;
}
case td::td_api::savedCredentials::ID: {
    auto& typed = static_cast<td::td_api::savedCredentials&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::savedMessagesTag::ID: {
    auto& typed = static_cast<td::td_api::savedMessagesTag&>(value);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.label_, observer);
    wipe_native_value(typed.count_, observer);
    return;
}
case td::td_api::savedMessagesTags::ID: {
    auto& typed = static_cast<td::td_api::savedMessagesTags&>(value);
    wipe_native_value(typed.tags_, observer);
    return;
}
case td::td_api::savedMessagesTopic::ID: {
    auto& typed = static_cast<td::td_api::savedMessagesTopic&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    wipe_native_value(typed.order_, observer);
    wipe_native_value(typed.last_message_, observer);
    wipe_native_value(typed.draft_message_, observer);
    return;
}
case td::td_api::savedMessagesTopicTypeAuthorHidden::ID: {
    return;
}
case td::td_api::savedMessagesTopicTypeMyNotes::ID: {
    return;
}
case td::td_api::savedMessagesTopicTypeSavedFromChat::ID: {
    auto& typed = static_cast<td::td_api::savedMessagesTopicTypeSavedFromChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::scopeAutosaveSettings::ID: {
    auto& typed = static_cast<td::td_api::scopeAutosaveSettings&>(value);
    wipe_native_value(typed.autosave_photos_, observer);
    wipe_native_value(typed.autosave_videos_, observer);
    wipe_native_value(typed.max_video_file_size_, observer);
    return;
}
case td::td_api::scopeNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::scopeNotificationSettings&>(value);
    wipe_native_value(typed.mute_for_, observer);
    wipe_native_value(typed.sound_id_, observer);
    wipe_native_value(typed.show_preview_, observer);
    wipe_native_value(typed.use_default_mute_stories_, observer);
    wipe_native_value(typed.mute_stories_, observer);
    wipe_native_value(typed.story_sound_id_, observer);
    wipe_native_value(typed.show_story_poster_, observer);
    wipe_native_value(typed.disable_pinned_message_notifications_, observer);
    wipe_native_value(typed.disable_mention_notifications_, observer);
    return;
}
case td::td_api::searchChatTypeFilterBot::ID: {
    return;
}
case td::td_api::searchChatTypeFilterChannel::ID: {
    return;
}
case td::td_api::searchMessagesChatTypeFilterChannel::ID: {
    return;
}
case td::td_api::searchMessagesChatTypeFilterGroup::ID: {
    return;
}
case td::td_api::searchMessagesChatTypeFilterPrivate::ID: {
    return;
}
case td::td_api::searchMessagesFilterAnimation::ID: {
    return;
}
case td::td_api::searchMessagesFilterAudio::ID: {
    return;
}
case td::td_api::searchMessagesFilterChatPhoto::ID: {
    return;
}
case td::td_api::searchMessagesFilterDocument::ID: {
    return;
}
case td::td_api::searchMessagesFilterEmpty::ID: {
    return;
}
case td::td_api::searchMessagesFilterFailedToSend::ID: {
    return;
}
case td::td_api::searchMessagesFilterMention::ID: {
    return;
}
case td::td_api::searchMessagesFilterPhoto::ID: {
    return;
}
case td::td_api::searchMessagesFilterPhotoAndVideo::ID: {
    return;
}
case td::td_api::searchMessagesFilterPinned::ID: {
    return;
}
case td::td_api::searchMessagesFilterPoll::ID: {
    return;
}
case td::td_api::searchMessagesFilterUnreadMention::ID: {
    return;
}
case td::td_api::searchMessagesFilterUnreadPollVote::ID: {
    return;
}
case td::td_api::searchMessagesFilterUnreadReaction::ID: {
    return;
}
case td::td_api::searchMessagesFilterUrl::ID: {
    return;
}
case td::td_api::searchMessagesFilterVideo::ID: {
    return;
}
case td::td_api::searchMessagesFilterVideoNote::ID: {
    return;
}
case td::td_api::searchMessagesFilterVoiceAndVideoNote::ID: {
    return;
}
case td::td_api::searchMessagesFilterVoiceNote::ID: {
    return;
}
case td::td_api::seconds::ID: {
    auto& typed = static_cast<td::td_api::seconds&>(value);
    wipe_native_value(typed.seconds_, observer);
    return;
}
case td::td_api::secretChat::ID: {
    auto& typed = static_cast<td::td_api::secretChat&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.state_, observer);
    wipe_native_value(typed.is_outbound_, observer);
    wipe_native_value(typed.key_hash_, observer);
    wipe_native_value(typed.layer_, observer);
    return;
}
case td::td_api::secretChatStateClosed::ID: {
    return;
}
case td::td_api::secretChatStatePending::ID: {
    return;
}
case td::td_api::secretChatStateReady::ID: {
    return;
}
case td::td_api::sentGiftRegular::ID: {
    auto& typed = static_cast<td::td_api::sentGiftRegular&>(value);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::sentGiftUpgraded::ID: {
    auto& typed = static_cast<td::td_api::sentGiftUpgraded&>(value);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::session::ID: {
    auto& typed = static_cast<td::td_api::session&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.is_current_, observer);
    wipe_native_value(typed.is_password_pending_, observer);
    wipe_native_value(typed.is_unconfirmed_, observer);
    wipe_native_value(typed.can_accept_secret_chats_, observer);
    wipe_native_value(typed.can_accept_calls_, observer);
    wipe_native_value(typed.device_type_, observer);
    wipe_native_value(typed.api_id_, observer);
    wipe_native_value(typed.application_name_, observer);
    wipe_native_value(typed.application_version_, observer);
    wipe_native_value(typed.is_official_application_, observer);
    wipe_native_value(typed.device_model_, observer);
    wipe_native_value(typed.platform_, observer);
    wipe_native_value(typed.system_version_, observer);
    wipe_native_value(typed.log_in_date_, observer);
    wipe_native_value(typed.last_active_date_, observer);
    wipe_native_value(typed.ip_address_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::sessionDeviceTypeAndroid::ID: {
    return;
}
case td::td_api::sessionDeviceTypeApple::ID: {
    return;
}
case td::td_api::sessionDeviceTypeBrave::ID: {
    return;
}
case td::td_api::sessionDeviceTypeChrome::ID: {
    return;
}
case td::td_api::sessionDeviceTypeEdge::ID: {
    return;
}
case td::td_api::sessionDeviceTypeFirefox::ID: {
    return;
}
case td::td_api::sessionDeviceTypeIpad::ID: {
    return;
}
case td::td_api::sessionDeviceTypeIphone::ID: {
    return;
}
case td::td_api::sessionDeviceTypeLinux::ID: {
    return;
}
case td::td_api::sessionDeviceTypeMac::ID: {
    return;
}
case td::td_api::sessionDeviceTypeOpera::ID: {
    return;
}
case td::td_api::sessionDeviceTypeSafari::ID: {
    return;
}
case td::td_api::sessionDeviceTypeUbuntu::ID: {
    return;
}
case td::td_api::sessionDeviceTypeUnknown::ID: {
    return;
}
case td::td_api::sessionDeviceTypeVivaldi::ID: {
    return;
}
case td::td_api::sessionDeviceTypeWindows::ID: {
    return;
}
case td::td_api::sessionDeviceTypeXbox::ID: {
    return;
}
case td::td_api::sessionTypeConnectedBot::ID: {
    auto& typed = static_cast<td::td_api::sessionTypeConnectedBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::sessionTypeDevice::ID: {
    auto& typed = static_cast<td::td_api::sessionTypeDevice&>(value);
    wipe_native_value(typed.session_id_, observer);
    return;
}
case td::td_api::sessions::ID: {
    auto& typed = static_cast<td::td_api::sessions&>(value);
    wipe_native_value(typed.sessions_, observer);
    wipe_native_value(typed.inactive_session_ttl_days_, observer);
    return;
}
case td::td_api::settingsSectionAppearance::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionAppearance&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionAskQuestion::ID: {
    return;
}
case td::td_api::settingsSectionBusiness::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionBusiness&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionChatFolders::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionChatFolders&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionDataAndStorage::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionDataAndStorage&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionDevices::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionDevices&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionEditProfile::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionEditProfile&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionFaq::ID: {
    return;
}
case td::td_api::settingsSectionFeatures::ID: {
    return;
}
case td::td_api::settingsSectionInAppBrowser::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionInAppBrowser&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionLanguage::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionLanguage&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionMyStars::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionMyStars&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionMyToncoins::ID: {
    return;
}
case td::td_api::settingsSectionNotifications::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionNotifications&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionPowerSaving::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionPowerSaving&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionPremium::ID: {
    return;
}
case td::td_api::settingsSectionPrivacyAndSecurity::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionPrivacyAndSecurity&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionPrivacyPolicy::ID: {
    return;
}
case td::td_api::settingsSectionQrCode::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionQrCode&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::settingsSectionSearch::ID: {
    return;
}
case td::td_api::settingsSectionSendGift::ID: {
    auto& typed = static_cast<td::td_api::settingsSectionSendGift&>(value);
    wipe_native_value(typed.subsection_, observer);
    return;
}
case td::td_api::sharedChat::ID: {
    auto& typed = static_cast<td::td_api::sharedChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::sharedUser::ID: {
    auto& typed = static_cast<td::td_api::sharedUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::shippingOption::ID: {
    auto& typed = static_cast<td::td_api::shippingOption&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.price_parts_, observer);
    return;
}
case td::td_api::speechRecognitionResultError::ID: {
    auto& typed = static_cast<td::td_api::speechRecognitionResultError&>(value);
    wipe_native_value(typed.error_, observer);
    return;
}
case td::td_api::speechRecognitionResultPending::ID: {
    auto& typed = static_cast<td::td_api::speechRecognitionResultPending&>(value);
    wipe_native_value(typed.partial_text_, observer);
    return;
}
case td::td_api::speechRecognitionResultText::ID: {
    auto& typed = static_cast<td::td_api::speechRecognitionResultText&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::sponsoredChat::ID: {
    auto& typed = static_cast<td::td_api::sponsoredChat&>(value);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.sponsor_info_, observer);
    wipe_native_value(typed.additional_info_, observer);
    return;
}
case td::td_api::sponsoredChats::ID: {
    auto& typed = static_cast<td::td_api::sponsoredChats&>(value);
    wipe_native_value(typed.chats_, observer);
    return;
}
case td::td_api::sponsoredMessage::ID: {
    auto& typed = static_cast<td::td_api::sponsoredMessage&>(value);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_recommended_, observer);
    wipe_native_value(typed.can_be_reported_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.sponsor_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.button_text_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.background_custom_emoji_id_, observer);
    wipe_native_value(typed.additional_info_, observer);
    return;
}
case td::td_api::sponsoredMessages::ID: {
    auto& typed = static_cast<td::td_api::sponsoredMessages&>(value);
    wipe_native_value(typed.messages_, observer);
    wipe_native_value(typed.messages_between_, observer);
    return;
}
case td::td_api::stakeDiceState::ID: {
    auto& typed = static_cast<td::td_api::stakeDiceState&>(value);
    wipe_native_value(typed.state_hash_, observer);
    wipe_native_value(typed.stake_toncoin_amount_, observer);
    wipe_native_value(typed.suggested_stake_toncoin_amounts_, observer);
    wipe_native_value(typed.current_streak_, observer);
    wipe_native_value(typed.prize_per_mille_, observer);
    wipe_native_value(typed.streak_prize_per_mille_, observer);
    return;
}
case td::td_api::starAmount::ID: {
    auto& typed = static_cast<td::td_api::starAmount&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.nanostar_count_, observer);
    return;
}
case td::td_api::starCount::ID: {
    auto& typed = static_cast<td::td_api::starCount&>(value);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::starGiveawayPaymentOption::ID: {
    auto& typed = static_cast<td::td_api::starGiveawayPaymentOption&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.yearly_boost_count_, observer);
    wipe_native_value(typed.winner_options_, observer);
    wipe_native_value(typed.is_default_, observer);
    wipe_native_value(typed.is_additional_, observer);
    return;
}
case td::td_api::starGiveawayPaymentOptions::ID: {
    auto& typed = static_cast<td::td_api::starGiveawayPaymentOptions&>(value);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::starGiveawayWinnerOption::ID: {
    auto& typed = static_cast<td::td_api::starGiveawayWinnerOption&>(value);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.won_star_count_, observer);
    wipe_native_value(typed.is_default_, observer);
    return;
}
case td::td_api::starPaymentOption::ID: {
    auto& typed = static_cast<td::td_api::starPaymentOption&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.is_additional_, observer);
    return;
}
case td::td_api::starPaymentOptions::ID: {
    auto& typed = static_cast<td::td_api::starPaymentOptions&>(value);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::starRevenueStatistics::ID: {
    auto& typed = static_cast<td::td_api::starRevenueStatistics&>(value);
    wipe_native_value(typed.revenue_by_day_graph_, observer);
    wipe_native_value(typed.status_, observer);
    wipe_native_value(typed.usd_rate_, observer);
    return;
}
case td::td_api::starRevenueStatus::ID: {
    auto& typed = static_cast<td::td_api::starRevenueStatus&>(value);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.current_amount_, observer);
    wipe_native_value(typed.available_amount_, observer);
    wipe_native_value(typed.withdrawal_enabled_, observer);
    wipe_native_value(typed.next_withdrawal_in_, observer);
    return;
}
case td::td_api::starSubscription::ID: {
    auto& typed = static_cast<td::td_api::starSubscription&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.is_canceled_, observer);
    wipe_native_value(typed.is_expiring_, observer);
    wipe_native_value(typed.pricing_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::starSubscriptionPricing::ID: {
    auto& typed = static_cast<td::td_api::starSubscriptionPricing&>(value);
    wipe_native_value(typed.period_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::starSubscriptionTypeBot::ID: {
    auto& typed = static_cast<td::td_api::starSubscriptionTypeBot&>(value);
    wipe_native_value(typed.is_canceled_by_bot_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.invoice_link_, observer);
    return;
}
case td::td_api::starSubscriptionTypeChannel::ID: {
    auto& typed = static_cast<td::td_api::starSubscriptionTypeChannel&>(value);
    wipe_native_value(typed.can_reuse_, observer);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::starSubscriptions::ID: {
    auto& typed = static_cast<td::td_api::starSubscriptions&>(value);
    wipe_native_value(typed.star_amount_, observer);
    wipe_native_value(typed.subscriptions_, observer);
    wipe_native_value(typed.required_star_count_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::starTransaction::ID: {
    auto& typed = static_cast<td::td_api::starTransaction&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.star_amount_, observer);
    wipe_native_value(typed.is_refund_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::starTransactionTypeAffiliateProgramCommission::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeAffiliateProgramCommission&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.commission_per_mille_, observer);
    return;
}
case td::td_api::starTransactionTypeAppStoreDeposit::ID: {
    return;
}
case td::td_api::starTransactionTypeBotInvoicePurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBotInvoicePurchase&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.product_info_, observer);
    return;
}
case td::td_api::starTransactionTypeBotInvoiceSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBotInvoiceSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.product_info_, observer);
    wipe_native_value(typed.invoice_payload_, observer);
    wipe_native_value(typed.affiliate_, observer);
    return;
}
case td::td_api::starTransactionTypeBotPaidMediaPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBotPaidMediaPurchase&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.media_, observer);
    return;
}
case td::td_api::starTransactionTypeBotPaidMediaSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBotPaidMediaSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.media_, observer);
    wipe_native_value(typed.payload_, observer);
    wipe_native_value(typed.affiliate_, observer);
    return;
}
case td::td_api::starTransactionTypeBotSubscriptionPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBotSubscriptionPurchase&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.subscription_period_, observer);
    wipe_native_value(typed.product_info_, observer);
    return;
}
case td::td_api::starTransactionTypeBotSubscriptionSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBotSubscriptionSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.subscription_period_, observer);
    wipe_native_value(typed.product_info_, observer);
    wipe_native_value(typed.invoice_payload_, observer);
    wipe_native_value(typed.affiliate_, observer);
    return;
}
case td::td_api::starTransactionTypeBusinessBotTransferReceive::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBusinessBotTransferReceive&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::starTransactionTypeBusinessBotTransferSend::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeBusinessBotTransferSend&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::starTransactionTypeChannelPaidMediaPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeChannelPaidMediaPurchase&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.media_, observer);
    return;
}
case td::td_api::starTransactionTypeChannelPaidMediaSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeChannelPaidMediaSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.media_, observer);
    return;
}
case td::td_api::starTransactionTypeChannelPaidReactionReceive::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeChannelPaidReactionReceive&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::starTransactionTypeChannelPaidReactionSend::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeChannelPaidReactionSend&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::starTransactionTypeChannelSubscriptionPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeChannelSubscriptionPurchase&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.subscription_period_, observer);
    return;
}
case td::td_api::starTransactionTypeChannelSubscriptionSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeChannelSubscriptionSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.subscription_period_, observer);
    return;
}
case td::td_api::starTransactionTypeFragmentDeposit::ID: {
    return;
}
case td::td_api::starTransactionTypeFragmentWithdrawal::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeFragmentWithdrawal&>(value);
    wipe_native_value(typed.withdrawal_state_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftAuctionBid::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftAuctionBid&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftOriginalDetailsDrop::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftOriginalDetailsDrop&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftPurchase&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftPurchaseOffer::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftPurchaseOffer&>(value);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftTransfer::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftTransfer&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftUpgrade::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftUpgrade&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiftUpgradePurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiftUpgradePurchase&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeGiveawayDeposit::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeGiveawayDeposit&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.giveaway_message_id_, observer);
    return;
}
case td::td_api::starTransactionTypeGooglePlayDeposit::ID: {
    return;
}
case td::td_api::starTransactionTypePaidGroupCallMessageReceive::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePaidGroupCallMessageReceive&>(value);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.commission_star_amount_, observer);
    return;
}
case td::td_api::starTransactionTypePaidGroupCallMessageSend::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePaidGroupCallMessageSend&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::starTransactionTypePaidGroupCallReactionReceive::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePaidGroupCallReactionReceive&>(value);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.commission_star_amount_, observer);
    return;
}
case td::td_api::starTransactionTypePaidGroupCallReactionSend::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePaidGroupCallReactionSend&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::starTransactionTypePaidMessageReceive::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePaidMessageReceive&>(value);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.message_count_, observer);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.commission_star_amount_, observer);
    return;
}
case td::td_api::starTransactionTypePaidMessageSend::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePaidMessageSend&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_count_, observer);
    return;
}
case td::td_api::starTransactionTypePremiumBotDeposit::ID: {
    return;
}
case td::td_api::starTransactionTypePremiumPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypePremiumPurchase&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::starTransactionTypePublicPostSearch::ID: {
    return;
}
case td::td_api::starTransactionTypeSuggestedPostPaymentReceive::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeSuggestedPostPaymentReceive&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::starTransactionTypeSuggestedPostPaymentSend::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeSuggestedPostPaymentSend&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::starTransactionTypeTelegramAdsWithdrawal::ID: {
    return;
}
case td::td_api::starTransactionTypeTelegramApiUsage::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeTelegramApiUsage&>(value);
    wipe_native_value(typed.request_count_, observer);
    return;
}
case td::td_api::starTransactionTypeUnsupported::ID: {
    return;
}
case td::td_api::starTransactionTypeUpgradedGiftPurchase::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeUpgradedGiftPurchase&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::starTransactionTypeUpgradedGiftSale::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeUpgradedGiftSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.commission_star_amount_, observer);
    wipe_native_value(typed.via_offer_, observer);
    return;
}
case td::td_api::starTransactionTypeUserDeposit::ID: {
    auto& typed = static_cast<td::td_api::starTransactionTypeUserDeposit&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::starTransactions::ID: {
    auto& typed = static_cast<td::td_api::starTransactions&>(value);
    wipe_native_value(typed.star_amount_, observer);
    wipe_native_value(typed.transactions_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::startLiveStoryResultFail::ID: {
    auto& typed = static_cast<td::td_api::startLiveStoryResultFail&>(value);
    wipe_native_value(typed.error_type_, observer);
    return;
}
case td::td_api::startLiveStoryResultOk::ID: {
    auto& typed = static_cast<td::td_api::startLiveStoryResultOk&>(value);
    wipe_native_value(typed.story_, observer);
    return;
}
case td::td_api::statisticalGraphAsync::ID: {
    auto& typed = static_cast<td::td_api::statisticalGraphAsync&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::statisticalGraphData::ID: {
    auto& typed = static_cast<td::td_api::statisticalGraphData&>(value);
    wipe_native_value(typed.json_data_, observer);
    wipe_native_value(typed.zoom_token_, observer);
    return;
}
case td::td_api::statisticalGraphError::ID: {
    auto& typed = static_cast<td::td_api::statisticalGraphError&>(value);
    wipe_native_value(typed.error_message_, observer);
    return;
}
case td::td_api::statisticalValue::ID: {
    auto& typed = static_cast<td::td_api::statisticalValue&>(value);
    wipe_native_value(typed.value_, observer);
    wipe_native_value(typed.previous_value_, observer);
    wipe_native_value(typed.growth_rate_percentage_, observer);
    return;
}
case td::td_api::sticker::ID: {
    auto& typed = static_cast<td::td_api::sticker&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.set_id_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.format_, observer);
    wipe_native_value(typed.full_type_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::stickerFormatTgs::ID: {
    return;
}
case td::td_api::stickerFormatWebm::ID: {
    return;
}
case td::td_api::stickerFormatWebp::ID: {
    return;
}
case td::td_api::stickerFullTypeCustomEmoji::ID: {
    auto& typed = static_cast<td::td_api::stickerFullTypeCustomEmoji&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    wipe_native_value(typed.needs_repainting_, observer);
    return;
}
case td::td_api::stickerFullTypeMask::ID: {
    auto& typed = static_cast<td::td_api::stickerFullTypeMask&>(value);
    wipe_native_value(typed.mask_position_, observer);
    return;
}
case td::td_api::stickerFullTypeRegular::ID: {
    auto& typed = static_cast<td::td_api::stickerFullTypeRegular&>(value);
    wipe_native_value(typed.premium_animation_, observer);
    return;
}
case td::td_api::stickerSet::ID: {
    auto& typed = static_cast<td::td_api::stickerSet&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.thumbnail_outline_, observer);
    wipe_native_value(typed.is_owned_, observer);
    wipe_native_value(typed.is_installed_, observer);
    wipe_native_value(typed.is_archived_, observer);
    wipe_native_value(typed.is_official_, observer);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.needs_repainting_, observer);
    wipe_native_value(typed.is_allowed_as_chat_emoji_status_, observer);
    wipe_native_value(typed.is_viewed_, observer);
    wipe_native_value(typed.stickers_, observer);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::stickerSetInfo::ID: {
    auto& typed = static_cast<td::td_api::stickerSetInfo&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.thumbnail_outline_, observer);
    wipe_native_value(typed.is_owned_, observer);
    wipe_native_value(typed.is_installed_, observer);
    wipe_native_value(typed.is_archived_, observer);
    wipe_native_value(typed.is_official_, observer);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.needs_repainting_, observer);
    wipe_native_value(typed.is_allowed_as_chat_emoji_status_, observer);
    wipe_native_value(typed.is_viewed_, observer);
    wipe_native_value(typed.size_, observer);
    wipe_native_value(typed.covers_, observer);
    return;
}
case td::td_api::stickerSets::ID: {
    auto& typed = static_cast<td::td_api::stickerSets&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.sets_, observer);
    return;
}
case td::td_api::stickerTypeCustomEmoji::ID: {
    return;
}
case td::td_api::stickerTypeMask::ID: {
    return;
}
case td::td_api::stickerTypeRegular::ID: {
    return;
}
case td::td_api::stickers::ID: {
    auto& typed = static_cast<td::td_api::stickers&>(value);
    wipe_native_value(typed.stickers_, observer);
    return;
}
case td::td_api::storageStatistics::ID: {
    auto& typed = static_cast<td::td_api::storageStatistics&>(value);
    wipe_native_value(typed.size_, observer);
    wipe_native_value(typed.count_, observer);
    wipe_native_value(typed.by_chat_, observer);
    return;
}
case td::td_api::storageStatisticsByChat::ID: {
    auto& typed = static_cast<td::td_api::storageStatisticsByChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.size_, observer);
    wipe_native_value(typed.count_, observer);
    wipe_native_value(typed.by_file_type_, observer);
    return;
}
case td::td_api::storageStatisticsByFileType::ID: {
    auto& typed = static_cast<td::td_api::storageStatisticsByFileType&>(value);
    wipe_native_value(typed.file_type_, observer);
    wipe_native_value(typed.size_, observer);
    wipe_native_value(typed.count_, observer);
    return;
}
case td::td_api::storageStatisticsFast::ID: {
    auto& typed = static_cast<td::td_api::storageStatisticsFast&>(value);
    wipe_native_value(typed.files_size_, observer);
    wipe_native_value(typed.file_count_, observer);
    wipe_native_value(typed.database_size_, observer);
    wipe_native_value(typed.language_pack_database_size_, observer);
    wipe_native_value(typed.log_size_, observer);
    return;
}
case td::td_api::storePaymentPurposeGiftedStars::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposeGiftedStars&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::storePaymentPurposePremiumGift::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposePremiumGift&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::storePaymentPurposePremiumGiftCodes::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposePremiumGiftCodes&>(value);
    wipe_native_value(typed.boosted_chat_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.user_ids_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::storePaymentPurposePremiumGiveaway::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposePremiumGiveaway&>(value);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    return;
}
case td::td_api::storePaymentPurposePremiumSubscription::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposePremiumSubscription&>(value);
    wipe_native_value(typed.is_restore_, observer);
    wipe_native_value(typed.is_upgrade_, observer);
    return;
}
case td::td_api::storePaymentPurposeStarGiveaway::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposeStarGiveaway&>(value);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::storePaymentPurposeStars::ID: {
    auto& typed = static_cast<td::td_api::storePaymentPurposeStars&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::storeTransactionAppStore::ID: {
    auto& typed = static_cast<td::td_api::storeTransactionAppStore&>(value);
    wipe_native_value(typed.receipt_, observer);
    return;
}
case td::td_api::storeTransactionGooglePlay::ID: {
    auto& typed = static_cast<td::td_api::storeTransactionGooglePlay&>(value);
    wipe_native_value(typed.package_name_, observer);
    wipe_native_value(typed.store_product_id_, observer);
    wipe_native_value(typed.purchase_token_, observer);
    return;
}
case td::td_api::stories::ID: {
    auto& typed = static_cast<td::td_api::stories&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.stories_, observer);
    wipe_native_value(typed.pinned_story_ids_, observer);
    return;
}
case td::td_api::story::ID: {
    auto& typed = static_cast<td::td_api::story&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.poster_chat_id_, observer);
    wipe_native_value(typed.poster_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.is_being_posted_, observer);
    wipe_native_value(typed.is_being_edited_, observer);
    wipe_native_value(typed.is_edited_, observer);
    wipe_native_value(typed.is_posted_to_chat_page_, observer);
    wipe_native_value(typed.is_visible_only_for_self_, observer);
    wipe_native_value(typed.can_be_added_to_album_, observer);
    wipe_native_value(typed.can_be_deleted_, observer);
    wipe_native_value(typed.can_be_edited_, observer);
    wipe_native_value(typed.can_be_forwarded_, observer);
    wipe_native_value(typed.can_be_replied_, observer);
    wipe_native_value(typed.can_set_privacy_settings_, observer);
    wipe_native_value(typed.can_toggle_is_posted_to_chat_page_, observer);
    wipe_native_value(typed.can_get_statistics_, observer);
    wipe_native_value(typed.can_get_interactions_, observer);
    wipe_native_value(typed.has_expired_viewers_, observer);
    wipe_native_value(typed.repost_info_, observer);
    wipe_native_value(typed.interaction_info_, observer);
    wipe_native_value(typed.chosen_reaction_type_, observer);
    wipe_native_value(typed.privacy_settings_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.areas_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.album_ids_, observer);
    return;
}
case td::td_api::storyAlbum::ID: {
    auto& typed = static_cast<td::td_api::storyAlbum&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.photo_icon_, observer);
    wipe_native_value(typed.video_icon_, observer);
    return;
}
case td::td_api::storyAlbums::ID: {
    auto& typed = static_cast<td::td_api::storyAlbums&>(value);
    wipe_native_value(typed.albums_, observer);
    return;
}
case td::td_api::storyArea::ID: {
    auto& typed = static_cast<td::td_api::storyArea&>(value);
    wipe_native_value(typed.position_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::storyAreaPosition::ID: {
    auto& typed = static_cast<td::td_api::storyAreaPosition&>(value);
    wipe_native_value(typed.x_percentage_, observer);
    wipe_native_value(typed.y_percentage_, observer);
    wipe_native_value(typed.width_percentage_, observer);
    wipe_native_value(typed.height_percentage_, observer);
    wipe_native_value(typed.rotation_angle_, observer);
    wipe_native_value(typed.corner_radius_percentage_, observer);
    return;
}
case td::td_api::storyAreaTypeLink::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeLink&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::storyAreaTypeLocation::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeLocation&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.address_, observer);
    return;
}
case td::td_api::storyAreaTypeMessage::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::storyAreaTypeSuggestedReaction::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeSuggestedReaction&>(value);
    wipe_native_value(typed.reaction_type_, observer);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.is_dark_, observer);
    wipe_native_value(typed.is_flipped_, observer);
    return;
}
case td::td_api::storyAreaTypeUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeUpgradedGift&>(value);
    wipe_native_value(typed.gift_name_, observer);
    return;
}
case td::td_api::storyAreaTypeVenue::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeVenue&>(value);
    wipe_native_value(typed.venue_, observer);
    return;
}
case td::td_api::storyAreaTypeWeather::ID: {
    auto& typed = static_cast<td::td_api::storyAreaTypeWeather&>(value);
    wipe_native_value(typed.temperature_, observer);
    wipe_native_value(typed.emoji_, observer);
    wipe_native_value(typed.background_color_, observer);
    return;
}
case td::td_api::storyContentLive::ID: {
    auto& typed = static_cast<td::td_api::storyContentLive&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.is_rtmp_stream_, observer);
    return;
}
case td::td_api::storyContentPhoto::ID: {
    auto& typed = static_cast<td::td_api::storyContentPhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::storyContentTypeLive::ID: {
    return;
}
case td::td_api::storyContentTypePhoto::ID: {
    return;
}
case td::td_api::storyContentTypeUnsupported::ID: {
    return;
}
case td::td_api::storyContentTypeVideo::ID: {
    return;
}
case td::td_api::storyContentUnsupported::ID: {
    return;
}
case td::td_api::storyContentVideo::ID: {
    auto& typed = static_cast<td::td_api::storyContentVideo&>(value);
    wipe_native_value(typed.video_, observer);
    wipe_native_value(typed.alternative_video_, observer);
    return;
}
case td::td_api::storyFullId::ID: {
    auto& typed = static_cast<td::td_api::storyFullId&>(value);
    wipe_native_value(typed.poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::storyInfo::ID: {
    auto& typed = static_cast<td::td_api::storyInfo&>(value);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.is_for_close_friends_, observer);
    wipe_native_value(typed.is_live_, observer);
    return;
}
case td::td_api::storyInteraction::ID: {
    auto& typed = static_cast<td::td_api::storyInteraction&>(value);
    wipe_native_value(typed.actor_id_, observer);
    wipe_native_value(typed.interaction_date_, observer);
    wipe_native_value(typed.block_list_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::storyInteractionInfo::ID: {
    auto& typed = static_cast<td::td_api::storyInteractionInfo&>(value);
    wipe_native_value(typed.view_count_, observer);
    wipe_native_value(typed.forward_count_, observer);
    wipe_native_value(typed.reaction_count_, observer);
    wipe_native_value(typed.recent_viewer_user_ids_, observer);
    return;
}
case td::td_api::storyInteractionTypeForward::ID: {
    auto& typed = static_cast<td::td_api::storyInteractionTypeForward&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::storyInteractionTypeRepost::ID: {
    auto& typed = static_cast<td::td_api::storyInteractionTypeRepost&>(value);
    wipe_native_value(typed.story_, observer);
    return;
}
case td::td_api::storyInteractionTypeView::ID: {
    auto& typed = static_cast<td::td_api::storyInteractionTypeView&>(value);
    wipe_native_value(typed.chosen_reaction_type_, observer);
    return;
}
case td::td_api::storyInteractions::ID: {
    auto& typed = static_cast<td::td_api::storyInteractions&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.total_forward_count_, observer);
    wipe_native_value(typed.total_reaction_count_, observer);
    wipe_native_value(typed.interactions_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::storyListArchive::ID: {
    return;
}
case td::td_api::storyListMain::ID: {
    return;
}
case td::td_api::storyOriginHiddenUser::ID: {
    auto& typed = static_cast<td::td_api::storyOriginHiddenUser&>(value);
    wipe_native_value(typed.poster_name_, observer);
    return;
}
case td::td_api::storyOriginPublicStory::ID: {
    auto& typed = static_cast<td::td_api::storyOriginPublicStory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::storyPrivacySettingsCloseFriends::ID: {
    return;
}
case td::td_api::storyPrivacySettingsContacts::ID: {
    auto& typed = static_cast<td::td_api::storyPrivacySettingsContacts&>(value);
    wipe_native_value(typed.except_user_ids_, observer);
    return;
}
case td::td_api::storyPrivacySettingsEveryone::ID: {
    auto& typed = static_cast<td::td_api::storyPrivacySettingsEveryone&>(value);
    wipe_native_value(typed.except_user_ids_, observer);
    return;
}
case td::td_api::storyPrivacySettingsSelectedUsers::ID: {
    auto& typed = static_cast<td::td_api::storyPrivacySettingsSelectedUsers&>(value);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::storyRepostInfo::ID: {
    auto& typed = static_cast<td::td_api::storyRepostInfo&>(value);
    wipe_native_value(typed.origin_, observer);
    wipe_native_value(typed.is_content_modified_, observer);
    return;
}
case td::td_api::storyStatistics::ID: {
    auto& typed = static_cast<td::td_api::storyStatistics&>(value);
    wipe_native_value(typed.story_interaction_graph_, observer);
    wipe_native_value(typed.story_reaction_graph_, observer);
    return;
}
case td::td_api::storyVideo::ID: {
    auto& typed = static_cast<td::td_api::storyVideo&>(value);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.has_stickers_, observer);
    wipe_native_value(typed.is_animation_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.preload_prefix_size_, observer);
    wipe_native_value(typed.cover_frame_timestamp_, observer);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::suggestedActionAddLoginPasskey::ID: {
    return;
}
case td::td_api::suggestedActionCheckPassword::ID: {
    return;
}
case td::td_api::suggestedActionCheckPhoneNumber::ID: {
    return;
}
case td::td_api::suggestedActionConvertToBroadcastGroup::ID: {
    auto& typed = static_cast<td::td_api::suggestedActionConvertToBroadcastGroup&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::suggestedActionCustom::ID: {
    auto& typed = static_cast<td::td_api::suggestedActionCustom&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::suggestedActionEnableArchiveAndMuteNewChats::ID: {
    return;
}
case td::td_api::suggestedActionExtendPremium::ID: {
    auto& typed = static_cast<td::td_api::suggestedActionExtendPremium&>(value);
    wipe_native_value(typed.manage_premium_subscription_url_, observer);
    return;
}
case td::td_api::suggestedActionExtendStarSubscriptions::ID: {
    return;
}
case td::td_api::suggestedActionGiftPremiumForChristmas::ID: {
    return;
}
case td::td_api::suggestedActionRestorePremium::ID: {
    return;
}
case td::td_api::suggestedActionSetBirthdate::ID: {
    return;
}
case td::td_api::suggestedActionSetLoginEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::suggestedActionSetLoginEmailAddress&>(value);
    wipe_native_value(typed.can_be_hidden_, observer);
    return;
}
case td::td_api::suggestedActionSetPassword::ID: {
    auto& typed = static_cast<td::td_api::suggestedActionSetPassword&>(value);
    wipe_native_value(typed.authorization_delay_, observer);
    return;
}
case td::td_api::suggestedActionSetProfilePhoto::ID: {
    return;
}
case td::td_api::suggestedActionSubscribeToAnnualPremium::ID: {
    return;
}
case td::td_api::suggestedActionUpgradePremium::ID: {
    return;
}
case td::td_api::suggestedActionViewChecksHint::ID: {
    return;
}
case td::td_api::suggestedPostInfo::ID: {
    auto& typed = static_cast<td::td_api::suggestedPostInfo&>(value);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.send_date_, observer);
    wipe_native_value(typed.state_, observer);
    wipe_native_value(typed.can_be_approved_, observer);
    wipe_native_value(typed.can_be_declined_, observer);
    return;
}
case td::td_api::suggestedPostPriceStar::ID: {
    auto& typed = static_cast<td::td_api::suggestedPostPriceStar&>(value);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::suggestedPostPriceTon::ID: {
    auto& typed = static_cast<td::td_api::suggestedPostPriceTon&>(value);
    wipe_native_value(typed.toncoin_cent_count_, observer);
    return;
}
case td::td_api::suggestedPostRefundReasonPaymentRefunded::ID: {
    return;
}
case td::td_api::suggestedPostRefundReasonPostDeleted::ID: {
    return;
}
case td::td_api::suggestedPostStateApproved::ID: {
    return;
}
case td::td_api::suggestedPostStateDeclined::ID: {
    return;
}
case td::td_api::suggestedPostStatePending::ID: {
    return;
}
case td::td_api::supergroup::ID: {
    auto& typed = static_cast<td::td_api::supergroup&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.usernames_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.status_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.boost_level_, observer);
    wipe_native_value(typed.has_automatic_translation_, observer);
    wipe_native_value(typed.has_linked_chat_, observer);
    wipe_native_value(typed.has_location_, observer);
    wipe_native_value(typed.sign_messages_, observer);
    wipe_native_value(typed.show_message_sender_, observer);
    wipe_native_value(typed.join_to_send_messages_, observer);
    wipe_native_value(typed.join_by_request_, observer);
    wipe_native_value(typed.is_slow_mode_enabled_, observer);
    wipe_native_value(typed.is_channel_, observer);
    wipe_native_value(typed.is_broadcast_group_, observer);
    wipe_native_value(typed.is_forum_, observer);
    wipe_native_value(typed.is_direct_messages_group_, observer);
    wipe_native_value(typed.is_administered_direct_messages_group_, observer);
    wipe_native_value(typed.verification_status_, observer);
    wipe_native_value(typed.has_direct_messages_group_, observer);
    wipe_native_value(typed.has_forum_tabs_, observer);
    wipe_native_value(typed.restriction_info_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    wipe_native_value(typed.active_story_state_, observer);
    return;
}
case td::td_api::supergroupFullInfo::ID: {
    auto& typed = static_cast<td::td_api::supergroupFullInfo&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.member_count_, observer);
    wipe_native_value(typed.administrator_count_, observer);
    wipe_native_value(typed.restricted_count_, observer);
    wipe_native_value(typed.banned_count_, observer);
    wipe_native_value(typed.linked_chat_id_, observer);
    wipe_native_value(typed.direct_messages_chat_id_, observer);
    wipe_native_value(typed.slow_mode_delay_, observer);
    wipe_native_value(typed.slow_mode_delay_expires_in_, observer);
    wipe_native_value(typed.can_enable_paid_messages_, observer);
    wipe_native_value(typed.can_enable_paid_reaction_, observer);
    wipe_native_value(typed.can_get_members_, observer);
    wipe_native_value(typed.has_hidden_members_, observer);
    wipe_native_value(typed.can_hide_members_, observer);
    wipe_native_value(typed.can_set_sticker_set_, observer);
    wipe_native_value(typed.can_set_location_, observer);
    wipe_native_value(typed.can_get_statistics_, observer);
    wipe_native_value(typed.can_get_revenue_statistics_, observer);
    wipe_native_value(typed.can_get_star_revenue_statistics_, observer);
    wipe_native_value(typed.can_send_gift_, observer);
    wipe_native_value(typed.can_toggle_aggressive_anti_spam_, observer);
    wipe_native_value(typed.is_all_history_available_, observer);
    wipe_native_value(typed.can_have_sponsored_messages_, observer);
    wipe_native_value(typed.has_aggressive_anti_spam_enabled_, observer);
    wipe_native_value(typed.has_paid_media_allowed_, observer);
    wipe_native_value(typed.has_pinned_stories_, observer);
    wipe_native_value(typed.gift_count_, observer);
    wipe_native_value(typed.my_boost_count_, observer);
    wipe_native_value(typed.unrestrict_boost_count_, observer);
    wipe_native_value(typed.outgoing_paid_message_star_count_, observer);
    wipe_native_value(typed.sticker_set_id_, observer);
    wipe_native_value(typed.custom_emoji_sticker_set_id_, observer);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.guard_bot_user_id_, observer);
    wipe_native_value(typed.bot_commands_, observer);
    wipe_native_value(typed.bot_verification_, observer);
    wipe_native_value(typed.main_profile_tab_, observer);
    wipe_native_value(typed.upgraded_from_basic_group_id_, observer);
    wipe_native_value(typed.upgraded_from_max_message_id_, observer);
    return;
}
case td::td_api::supergroupMembersFilterAdministrators::ID: {
    return;
}
case td::td_api::supergroupMembersFilterBanned::ID: {
    auto& typed = static_cast<td::td_api::supergroupMembersFilterBanned&>(value);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::supergroupMembersFilterBots::ID: {
    return;
}
case td::td_api::supergroupMembersFilterContacts::ID: {
    auto& typed = static_cast<td::td_api::supergroupMembersFilterContacts&>(value);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::supergroupMembersFilterMention::ID: {
    auto& typed = static_cast<td::td_api::supergroupMembersFilterMention&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::supergroupMembersFilterRecent::ID: {
    return;
}
case td::td_api::supergroupMembersFilterRestricted::ID: {
    auto& typed = static_cast<td::td_api::supergroupMembersFilterRestricted&>(value);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::supergroupMembersFilterSearch::ID: {
    auto& typed = static_cast<td::td_api::supergroupMembersFilterSearch&>(value);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::tMeUrl::ID: {
    auto& typed = static_cast<td::td_api::tMeUrl&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::tMeUrlTypeChatInvite::ID: {
    auto& typed = static_cast<td::td_api::tMeUrlTypeChatInvite&>(value);
    wipe_native_value(typed.info_, observer);
    return;
}
case td::td_api::tMeUrlTypeStickerSet::ID: {
    auto& typed = static_cast<td::td_api::tMeUrlTypeStickerSet&>(value);
    wipe_native_value(typed.sticker_set_id_, observer);
    return;
}
case td::td_api::tMeUrlTypeSupergroup::ID: {
    auto& typed = static_cast<td::td_api::tMeUrlTypeSupergroup&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::tMeUrlTypeUser::ID: {
    auto& typed = static_cast<td::td_api::tMeUrlTypeUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::tMeUrls::ID: {
    auto& typed = static_cast<td::td_api::tMeUrls&>(value);
    wipe_native_value(typed.urls_, observer);
    return;
}
case td::td_api::targetChatChosen::ID: {
    auto& typed = static_cast<td::td_api::targetChatChosen&>(value);
    wipe_native_value(typed.types_, observer);
    return;
}
case td::td_api::targetChatCurrent::ID: {
    return;
}
case td::td_api::targetChatInternalLink::ID: {
    auto& typed = static_cast<td::td_api::targetChatInternalLink&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::targetChatTypes::ID: {
    auto& typed = static_cast<td::td_api::targetChatTypes&>(value);
    wipe_native_value(typed.allow_user_chats_, observer);
    wipe_native_value(typed.allow_bot_chats_, observer);
    wipe_native_value(typed.allow_group_chats_, observer);
    wipe_native_value(typed.allow_channel_chats_, observer);
    return;
}
case td::td_api::telegramPaymentPurposeGiftedStars::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposeGiftedStars&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::telegramPaymentPurposeJoinChat::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposeJoinChat&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::telegramPaymentPurposePremiumGift::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposePremiumGift&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::telegramPaymentPurposePremiumGiftCodes::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposePremiumGiftCodes&>(value);
    wipe_native_value(typed.boosted_chat_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.user_ids_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::telegramPaymentPurposePremiumGiveaway::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposePremiumGiveaway&>(value);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.month_count_, observer);
    return;
}
case td::td_api::telegramPaymentPurposeStarGiveaway::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposeStarGiveaway&>(value);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::telegramPaymentPurposeStars::ID: {
    auto& typed = static_cast<td::td_api::telegramPaymentPurposeStars&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::temporaryPasswordState::ID: {
    auto& typed = static_cast<td::td_api::temporaryPasswordState&>(value);
    wipe_native_value(typed.has_password_, observer);
    wipe_native_value(typed.valid_for_, observer);
    return;
}
case td::td_api::termsOfService::ID: {
    auto& typed = static_cast<td::td_api::termsOfService&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.min_user_age_, observer);
    wipe_native_value(typed.show_popup_, observer);
    return;
}
case td::td_api::testBytes::ID: {
    auto& typed = static_cast<td::td_api::testBytes&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::testInt::ID: {
    auto& typed = static_cast<td::td_api::testInt&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::testString::ID: {
    auto& typed = static_cast<td::td_api::testString&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::testVectorInt::ID: {
    auto& typed = static_cast<td::td_api::testVectorInt&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::testVectorIntObject::ID: {
    auto& typed = static_cast<td::td_api::testVectorIntObject&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::testVectorString::ID: {
    auto& typed = static_cast<td::td_api::testVectorString&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::testVectorStringObject::ID: {
    auto& typed = static_cast<td::td_api::testVectorStringObject&>(value);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::text::ID: {
    auto& typed = static_cast<td::td_api::text&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::textCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::textCompositionStyle&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.custom_emoji_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.is_custom_, observer);
    wipe_native_value(typed.is_creator_, observer);
    wipe_native_value(typed.install_count_, observer);
    wipe_native_value(typed.prompt_, observer);
    wipe_native_value(typed.creator_user_id_, observer);
    wipe_native_value(typed.english_example_, observer);
    return;
}
case td::td_api::textCompositionStyleExample::ID: {
    auto& typed = static_cast<td::td_api::textCompositionStyleExample&>(value);
    wipe_native_value(typed.source_text_, observer);
    wipe_native_value(typed.result_text_, observer);
    return;
}
case td::td_api::textEntities::ID: {
    auto& typed = static_cast<td::td_api::textEntities&>(value);
    wipe_native_value(typed.entities_, observer);
    return;
}
case td::td_api::textEntity::ID: {
    auto& typed = static_cast<td::td_api::textEntity&>(value);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.length_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::textEntityTypeBankCardNumber::ID: {
    return;
}
case td::td_api::textEntityTypeBlockQuote::ID: {
    return;
}
case td::td_api::textEntityTypeBold::ID: {
    return;
}
case td::td_api::textEntityTypeBotCommand::ID: {
    return;
}
case td::td_api::textEntityTypeCashtag::ID: {
    return;
}
case td::td_api::textEntityTypeCode::ID: {
    return;
}
case td::td_api::textEntityTypeCustomEmoji::ID: {
    auto& typed = static_cast<td::td_api::textEntityTypeCustomEmoji&>(value);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::textEntityTypeDateTime::ID: {
    auto& typed = static_cast<td::td_api::textEntityTypeDateTime&>(value);
    wipe_native_value(typed.unix_time_, observer);
    wipe_native_value(typed.formatting_type_, observer);
    return;
}
case td::td_api::textEntityTypeEmailAddress::ID: {
    return;
}
case td::td_api::textEntityTypeExpandableBlockQuote::ID: {
    return;
}
case td::td_api::textEntityTypeHashtag::ID: {
    return;
}
case td::td_api::textEntityTypeItalic::ID: {
    return;
}
case td::td_api::textEntityTypeMediaTimestamp::ID: {
    auto& typed = static_cast<td::td_api::textEntityTypeMediaTimestamp&>(value);
    wipe_native_value(typed.media_timestamp_, observer);
    return;
}
case td::td_api::textEntityTypeMention::ID: {
    return;
}
case td::td_api::textEntityTypeMentionName::ID: {
    auto& typed = static_cast<td::td_api::textEntityTypeMentionName&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::textEntityTypePhoneNumber::ID: {
    return;
}
case td::td_api::textEntityTypePre::ID: {
    return;
}
case td::td_api::textEntityTypePreCode::ID: {
    auto& typed = static_cast<td::td_api::textEntityTypePreCode&>(value);
    wipe_native_value(typed.language_, observer);
    return;
}
case td::td_api::textEntityTypeSpoiler::ID: {
    return;
}
case td::td_api::textEntityTypeStrikethrough::ID: {
    return;
}
case td::td_api::textEntityTypeTextUrl::ID: {
    auto& typed = static_cast<td::td_api::textEntityTypeTextUrl&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::textEntityTypeUnderline::ID: {
    return;
}
case td::td_api::textEntityTypeUrl::ID: {
    return;
}
case td::td_api::textParseModeHTML::ID: {
    return;
}
case td::td_api::textParseModeMarkdown::ID: {
    auto& typed = static_cast<td::td_api::textParseModeMarkdown&>(value);
    wipe_native_value(typed.version_, observer);
    return;
}
case td::td_api::textQuote::ID: {
    auto& typed = static_cast<td::td_api::textQuote&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.position_, observer);
    wipe_native_value(typed.is_manual_, observer);
    return;
}
case td::td_api::themeParameters::ID: {
    auto& typed = static_cast<td::td_api::themeParameters&>(value);
    wipe_native_value(typed.background_color_, observer);
    wipe_native_value(typed.secondary_background_color_, observer);
    wipe_native_value(typed.header_background_color_, observer);
    wipe_native_value(typed.bottom_bar_background_color_, observer);
    wipe_native_value(typed.section_background_color_, observer);
    wipe_native_value(typed.section_separator_color_, observer);
    wipe_native_value(typed.text_color_, observer);
    wipe_native_value(typed.accent_text_color_, observer);
    wipe_native_value(typed.section_header_text_color_, observer);
    wipe_native_value(typed.subtitle_text_color_, observer);
    wipe_native_value(typed.destructive_text_color_, observer);
    wipe_native_value(typed.hint_color_, observer);
    wipe_native_value(typed.link_color_, observer);
    wipe_native_value(typed.button_color_, observer);
    wipe_native_value(typed.button_text_color_, observer);
    return;
}
case td::td_api::themeSettings::ID: {
    auto& typed = static_cast<td::td_api::themeSettings&>(value);
    wipe_native_value(typed.base_theme_, observer);
    wipe_native_value(typed.accent_color_, observer);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.outgoing_message_fill_, observer);
    wipe_native_value(typed.animate_outgoing_message_fill_, observer);
    wipe_native_value(typed.outgoing_message_accent_color_, observer);
    return;
}
case td::td_api::thumbnail::ID: {
    auto& typed = static_cast<td::td_api::thumbnail&>(value);
    wipe_native_value(typed.format_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.file_, observer);
    return;
}
case td::td_api::thumbnailFormatGif::ID: {
    return;
}
case td::td_api::thumbnailFormatJpeg::ID: {
    return;
}
case td::td_api::thumbnailFormatMpeg4::ID: {
    return;
}
case td::td_api::thumbnailFormatPng::ID: {
    return;
}
case td::td_api::thumbnailFormatTgs::ID: {
    return;
}
case td::td_api::thumbnailFormatWebm::ID: {
    return;
}
case td::td_api::thumbnailFormatWebp::ID: {
    return;
}
case td::td_api::timeZone::ID: {
    auto& typed = static_cast<td::td_api::timeZone&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.utc_time_offset_, observer);
    return;
}
case td::td_api::timeZones::ID: {
    auto& typed = static_cast<td::td_api::timeZones&>(value);
    wipe_native_value(typed.time_zones_, observer);
    return;
}
case td::td_api::tonRevenueStatistics::ID: {
    auto& typed = static_cast<td::td_api::tonRevenueStatistics&>(value);
    wipe_native_value(typed.revenue_by_day_graph_, observer);
    wipe_native_value(typed.status_, observer);
    wipe_native_value(typed.usd_rate_, observer);
    return;
}
case td::td_api::tonRevenueStatus::ID: {
    auto& typed = static_cast<td::td_api::tonRevenueStatus&>(value);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.balance_amount_, observer);
    wipe_native_value(typed.available_amount_, observer);
    wipe_native_value(typed.withdrawal_enabled_, observer);
    return;
}
case td::td_api::tonTransaction::ID: {
    auto& typed = static_cast<td::td_api::tonTransaction&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.ton_amount_, observer);
    wipe_native_value(typed.is_refund_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::tonTransactionTypeFragmentDeposit::ID: {
    auto& typed = static_cast<td::td_api::tonTransactionTypeFragmentDeposit&>(value);
    wipe_native_value(typed.is_gift_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::tonTransactionTypeFragmentWithdrawal::ID: {
    auto& typed = static_cast<td::td_api::tonTransactionTypeFragmentWithdrawal&>(value);
    wipe_native_value(typed.withdrawal_state_, observer);
    return;
}
case td::td_api::tonTransactionTypeGiftPurchaseOffer::ID: {
    auto& typed = static_cast<td::td_api::tonTransactionTypeGiftPurchaseOffer&>(value);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::tonTransactionTypeStakeDicePayout::ID: {
    return;
}
case td::td_api::tonTransactionTypeStakeDiceStake::ID: {
    return;
}
case td::td_api::tonTransactionTypeSuggestedPostPayment::ID: {
    auto& typed = static_cast<td::td_api::tonTransactionTypeSuggestedPostPayment&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::tonTransactionTypeUnsupported::ID: {
    return;
}
case td::td_api::tonTransactionTypeUpgradedGiftPurchase::ID: {
    auto& typed = static_cast<td::td_api::tonTransactionTypeUpgradedGiftPurchase&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_, observer);
    return;
}
case td::td_api::tonTransactionTypeUpgradedGiftSale::ID: {
    auto& typed = static_cast<td::td_api::tonTransactionTypeUpgradedGiftSale&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.commission_per_mille_, observer);
    wipe_native_value(typed.commission_toncoin_amount_, observer);
    wipe_native_value(typed.via_offer_, observer);
    return;
}
case td::td_api::tonTransactions::ID: {
    auto& typed = static_cast<td::td_api::tonTransactions&>(value);
    wipe_native_value(typed.ton_amount_, observer);
    wipe_native_value(typed.transactions_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::topChatCategoryBots::ID: {
    return;
}
case td::td_api::topChatCategoryCalls::ID: {
    return;
}
case td::td_api::topChatCategoryChannels::ID: {
    return;
}
case td::td_api::topChatCategoryForwardChats::ID: {
    return;
}
case td::td_api::topChatCategoryGroups::ID: {
    return;
}
case td::td_api::topChatCategoryGuestBots::ID: {
    return;
}
case td::td_api::topChatCategoryInlineBots::ID: {
    return;
}
case td::td_api::topChatCategoryUsers::ID: {
    return;
}
case td::td_api::topChatCategoryWebAppBots::ID: {
    return;
}
case td::td_api::transactionDirectionIncoming::ID: {
    return;
}
case td::td_api::transactionDirectionOutgoing::ID: {
    return;
}
case td::td_api::trendingStickerSets::ID: {
    auto& typed = static_cast<td::td_api::trendingStickerSets&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.sets_, observer);
    wipe_native_value(typed.is_premium_, observer);
    return;
}
case td::td_api::unconfirmedSession::ID: {
    auto& typed = static_cast<td::td_api::unconfirmedSession&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.device_model_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::unreadReaction::ID: {
    auto& typed = static_cast<td::td_api::unreadReaction&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.is_big_, observer);
    return;
}
case td::td_api::updateAccentColors::ID: {
    auto& typed = static_cast<td::td_api::updateAccentColors&>(value);
    wipe_native_value(typed.colors_, observer);
    wipe_native_value(typed.available_accent_color_ids_, observer);
    return;
}
case td::td_api::updateActiveEmojiReactions::ID: {
    auto& typed = static_cast<td::td_api::updateActiveEmojiReactions&>(value);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::updateActiveGiftAuctions::ID: {
    auto& typed = static_cast<td::td_api::updateActiveGiftAuctions&>(value);
    wipe_native_value(typed.states_, observer);
    return;
}
case td::td_api::updateActiveLiveLocationMessages::ID: {
    auto& typed = static_cast<td::td_api::updateActiveLiveLocationMessages&>(value);
    wipe_native_value(typed.messages_, observer);
    return;
}
case td::td_api::updateActiveNotifications::ID: {
    auto& typed = static_cast<td::td_api::updateActiveNotifications&>(value);
    wipe_native_value(typed.groups_, observer);
    return;
}
case td::td_api::updateAgeVerificationParameters::ID: {
    auto& typed = static_cast<td::td_api::updateAgeVerificationParameters&>(value);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::updateAnimatedEmojiMessageClicked::ID: {
    auto& typed = static_cast<td::td_api::updateAnimatedEmojiMessageClicked&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::updateAnimationSearchParameters::ID: {
    auto& typed = static_cast<td::td_api::updateAnimationSearchParameters&>(value);
    wipe_native_value(typed.provider_, observer);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::updateApplicationRecaptchaVerificationRequired::ID: {
    auto& typed = static_cast<td::td_api::updateApplicationRecaptchaVerificationRequired&>(value);
    wipe_native_value(typed.verification_id_, observer);
    wipe_native_value(typed.action_, observer);
    wipe_native_value(typed.recaptcha_key_id_, observer);
    return;
}
case td::td_api::updateApplicationVerificationRequired::ID: {
    auto& typed = static_cast<td::td_api::updateApplicationVerificationRequired&>(value);
    wipe_native_value(typed.verification_id_, observer);
    wipe_native_value(typed.nonce_, observer);
    wipe_native_value(typed.cloud_project_number_, observer);
    return;
}
case td::td_api::updateAttachmentMenuBots::ID: {
    auto& typed = static_cast<td::td_api::updateAttachmentMenuBots&>(value);
    wipe_native_value(typed.bots_, observer);
    return;
}
case td::td_api::updateAuthorizationState::ID: {
    auto& typed = static_cast<td::td_api::updateAuthorizationState&>(value);
    wipe_native_value(typed.authorization_state_, observer);
    return;
}
case td::td_api::updateAutosaveSettings::ID: {
    auto& typed = static_cast<td::td_api::updateAutosaveSettings&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::updateAvailableMessageEffects::ID: {
    auto& typed = static_cast<td::td_api::updateAvailableMessageEffects&>(value);
    wipe_native_value(typed.reaction_effect_ids_, observer);
    wipe_native_value(typed.sticker_effect_ids_, observer);
    return;
}
case td::td_api::updateBasicGroup::ID: {
    auto& typed = static_cast<td::td_api::updateBasicGroup&>(value);
    wipe_native_value(typed.basic_group_, observer);
    return;
}
case td::td_api::updateBasicGroupFullInfo::ID: {
    auto& typed = static_cast<td::td_api::updateBasicGroupFullInfo&>(value);
    wipe_native_value(typed.basic_group_id_, observer);
    wipe_native_value(typed.basic_group_full_info_, observer);
    return;
}
case td::td_api::updateBusinessConnection::ID: {
    auto& typed = static_cast<td::td_api::updateBusinessConnection&>(value);
    wipe_native_value(typed.connection_, observer);
    return;
}
case td::td_api::updateBusinessMessageEdited::ID: {
    auto& typed = static_cast<td::td_api::updateBusinessMessageEdited&>(value);
    wipe_native_value(typed.connection_id_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::updateBusinessMessagesDeleted::ID: {
    auto& typed = static_cast<td::td_api::updateBusinessMessagesDeleted&>(value);
    wipe_native_value(typed.connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::updateCall::ID: {
    auto& typed = static_cast<td::td_api::updateCall&>(value);
    wipe_native_value(typed.call_, observer);
    return;
}
case td::td_api::updateChatAccentColors::ID: {
    auto& typed = static_cast<td::td_api::updateChatAccentColors&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.background_custom_emoji_id_, observer);
    wipe_native_value(typed.upgraded_gift_colors_, observer);
    wipe_native_value(typed.profile_accent_color_id_, observer);
    wipe_native_value(typed.profile_background_custom_emoji_id_, observer);
    return;
}
case td::td_api::updateChatAction::ID: {
    auto& typed = static_cast<td::td_api::updateChatAction&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.action_, observer);
    return;
}
case td::td_api::updateChatActionBar::ID: {
    auto& typed = static_cast<td::td_api::updateChatActionBar&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.action_bar_, observer);
    return;
}
case td::td_api::updateChatActiveStories::ID: {
    auto& typed = static_cast<td::td_api::updateChatActiveStories&>(value);
    wipe_native_value(typed.active_stories_, observer);
    return;
}
case td::td_api::updateChatAddedToList::ID: {
    auto& typed = static_cast<td::td_api::updateChatAddedToList&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.chat_list_, observer);
    return;
}
case td::td_api::updateChatAvailableReactions::ID: {
    auto& typed = static_cast<td::td_api::updateChatAvailableReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.available_reactions_, observer);
    return;
}
case td::td_api::updateChatBackground::ID: {
    auto& typed = static_cast<td::td_api::updateChatBackground&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.background_, observer);
    return;
}
case td::td_api::updateChatBlockList::ID: {
    auto& typed = static_cast<td::td_api::updateChatBlockList&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.block_list_, observer);
    return;
}
case td::td_api::updateChatBoost::ID: {
    auto& typed = static_cast<td::td_api::updateChatBoost&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.boost_, observer);
    return;
}
case td::td_api::updateChatBusinessBotManageBar::ID: {
    auto& typed = static_cast<td::td_api::updateChatBusinessBotManageBar&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.business_bot_manage_bar_, observer);
    return;
}
case td::td_api::updateChatDefaultDisableNotification::ID: {
    auto& typed = static_cast<td::td_api::updateChatDefaultDisableNotification&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.default_disable_notification_, observer);
    return;
}
case td::td_api::updateChatDraftMessage::ID: {
    auto& typed = static_cast<td::td_api::updateChatDraftMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.draft_message_, observer);
    wipe_native_value(typed.positions_, observer);
    return;
}
case td::td_api::updateChatEmojiStatus::ID: {
    auto& typed = static_cast<td::td_api::updateChatEmojiStatus&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.emoji_status_, observer);
    return;
}
case td::td_api::updateChatFolders::ID: {
    auto& typed = static_cast<td::td_api::updateChatFolders&>(value);
    wipe_native_value(typed.chat_folders_, observer);
    wipe_native_value(typed.main_chat_list_position_, observer);
    wipe_native_value(typed.are_tags_enabled_, observer);
    return;
}
case td::td_api::updateChatHasProtectedContent::ID: {
    auto& typed = static_cast<td::td_api::updateChatHasProtectedContent&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.has_protected_content_, observer);
    return;
}
case td::td_api::updateChatHasScheduledMessages::ID: {
    auto& typed = static_cast<td::td_api::updateChatHasScheduledMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.has_scheduled_messages_, observer);
    return;
}
case td::td_api::updateChatIsMarkedAsUnread::ID: {
    auto& typed = static_cast<td::td_api::updateChatIsMarkedAsUnread&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_marked_as_unread_, observer);
    return;
}
case td::td_api::updateChatIsTranslatable::ID: {
    auto& typed = static_cast<td::td_api::updateChatIsTranslatable&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_translatable_, observer);
    return;
}
case td::td_api::updateChatJoinResult::ID: {
    auto& typed = static_cast<td::td_api::updateChatJoinResult&>(value);
    wipe_native_value(typed.query_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.result_, observer);
    return;
}
case td::td_api::updateChatLastMessage::ID: {
    auto& typed = static_cast<td::td_api::updateChatLastMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.last_message_, observer);
    wipe_native_value(typed.positions_, observer);
    return;
}
case td::td_api::updateChatMember::ID: {
    auto& typed = static_cast<td::td_api::updateChatMember&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.actor_user_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.via_join_request_, observer);
    wipe_native_value(typed.via_chat_folder_invite_link_, observer);
    wipe_native_value(typed.old_chat_member_, observer);
    wipe_native_value(typed.new_chat_member_, observer);
    return;
}
case td::td_api::updateChatMessageAutoDeleteTime::ID: {
    auto& typed = static_cast<td::td_api::updateChatMessageAutoDeleteTime&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    return;
}
case td::td_api::updateChatMessageSender::ID: {
    auto& typed = static_cast<td::td_api::updateChatMessageSender&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_sender_id_, observer);
    return;
}
case td::td_api::updateChatNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::updateChatNotificationSettings&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::updateChatOnlineMemberCount::ID: {
    auto& typed = static_cast<td::td_api::updateChatOnlineMemberCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.online_member_count_, observer);
    return;
}
case td::td_api::updateChatPendingJoinRequests::ID: {
    auto& typed = static_cast<td::td_api::updateChatPendingJoinRequests&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.pending_join_requests_, observer);
    return;
}
case td::td_api::updateChatPermissions::ID: {
    auto& typed = static_cast<td::td_api::updateChatPermissions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.permissions_, observer);
    return;
}
case td::td_api::updateChatPhoto::ID: {
    auto& typed = static_cast<td::td_api::updateChatPhoto&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::updateChatPosition::ID: {
    auto& typed = static_cast<td::td_api::updateChatPosition&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.position_, observer);
    return;
}
case td::td_api::updateChatReadInbox::ID: {
    auto& typed = static_cast<td::td_api::updateChatReadInbox&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.last_read_inbox_message_id_, observer);
    wipe_native_value(typed.unread_count_, observer);
    return;
}
case td::td_api::updateChatReadOutbox::ID: {
    auto& typed = static_cast<td::td_api::updateChatReadOutbox&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.last_read_outbox_message_id_, observer);
    return;
}
case td::td_api::updateChatRemovedFromList::ID: {
    auto& typed = static_cast<td::td_api::updateChatRemovedFromList&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.chat_list_, observer);
    return;
}
case td::td_api::updateChatReplyMarkup::ID: {
    auto& typed = static_cast<td::td_api::updateChatReplyMarkup&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.reply_markup_message_, observer);
    return;
}
case td::td_api::updateChatRevenueAmount::ID: {
    auto& typed = static_cast<td::td_api::updateChatRevenueAmount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.revenue_amount_, observer);
    return;
}
case td::td_api::updateChatTheme::ID: {
    auto& typed = static_cast<td::td_api::updateChatTheme&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.theme_, observer);
    return;
}
case td::td_api::updateChatTitle::ID: {
    auto& typed = static_cast<td::td_api::updateChatTitle&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::updateChatUnreadMentionCount::ID: {
    auto& typed = static_cast<td::td_api::updateChatUnreadMentionCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.unread_mention_count_, observer);
    return;
}
case td::td_api::updateChatUnreadPollVoteCount::ID: {
    auto& typed = static_cast<td::td_api::updateChatUnreadPollVoteCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.unread_poll_vote_count_, observer);
    return;
}
case td::td_api::updateChatUnreadReactionCount::ID: {
    auto& typed = static_cast<td::td_api::updateChatUnreadReactionCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.unread_reaction_count_, observer);
    return;
}
case td::td_api::updateChatVideoChat::ID: {
    auto& typed = static_cast<td::td_api::updateChatVideoChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.video_chat_, observer);
    return;
}
case td::td_api::updateChatViewAsTopics::ID: {
    auto& typed = static_cast<td::td_api::updateChatViewAsTopics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.view_as_topics_, observer);
    return;
}
case td::td_api::updateConnectionState::ID: {
    auto& typed = static_cast<td::td_api::updateConnectionState&>(value);
    wipe_native_value(typed.state_, observer);
    return;
}
case td::td_api::updateContactCloseBirthdays::ID: {
    auto& typed = static_cast<td::td_api::updateContactCloseBirthdays&>(value);
    wipe_native_value(typed.close_birthday_users_, observer);
    return;
}
case td::td_api::updateDefaultBackground::ID: {
    auto& typed = static_cast<td::td_api::updateDefaultBackground&>(value);
    wipe_native_value(typed.for_dark_theme_, observer);
    wipe_native_value(typed.background_, observer);
    return;
}
case td::td_api::updateDefaultPaidReactionType::ID: {
    auto& typed = static_cast<td::td_api::updateDefaultPaidReactionType&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::updateDefaultReactionType::ID: {
    auto& typed = static_cast<td::td_api::updateDefaultReactionType&>(value);
    wipe_native_value(typed.reaction_type_, observer);
    return;
}
case td::td_api::updateDeleteMessages::ID: {
    auto& typed = static_cast<td::td_api::updateDeleteMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.is_permanent_, observer);
    wipe_native_value(typed.from_cache_, observer);
    return;
}
case td::td_api::updateDiceEmojis::ID: {
    auto& typed = static_cast<td::td_api::updateDiceEmojis&>(value);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::updateDirectMessagesChatTopic::ID: {
    auto& typed = static_cast<td::td_api::updateDirectMessagesChatTopic&>(value);
    wipe_native_value(typed.topic_, observer);
    return;
}
case td::td_api::updateEmojiChatThemes::ID: {
    auto& typed = static_cast<td::td_api::updateEmojiChatThemes&>(value);
    wipe_native_value(typed.chat_themes_, observer);
    return;
}
case td::td_api::updateFavoriteStickers::ID: {
    auto& typed = static_cast<td::td_api::updateFavoriteStickers&>(value);
    wipe_native_value(typed.sticker_ids_, observer);
    return;
}
case td::td_api::updateFile::ID: {
    auto& typed = static_cast<td::td_api::updateFile&>(value);
    wipe_native_value(typed.file_, observer);
    return;
}
case td::td_api::updateFileAddedToDownloads::ID: {
    auto& typed = static_cast<td::td_api::updateFileAddedToDownloads&>(value);
    wipe_native_value(typed.file_download_, observer);
    wipe_native_value(typed.counts_, observer);
    return;
}
case td::td_api::updateFileDownload::ID: {
    auto& typed = static_cast<td::td_api::updateFileDownload&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.complete_date_, observer);
    wipe_native_value(typed.is_paused_, observer);
    wipe_native_value(typed.counts_, observer);
    return;
}
case td::td_api::updateFileDownloads::ID: {
    auto& typed = static_cast<td::td_api::updateFileDownloads&>(value);
    wipe_native_value(typed.total_size_, observer);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.downloaded_size_, observer);
    return;
}
case td::td_api::updateFileGenerationStart::ID: {
    auto& typed = static_cast<td::td_api::updateFileGenerationStart&>(value);
    wipe_native_value(typed.generation_id_, observer);
    wipe_native_value(typed.original_path_, observer);
    wipe_native_value(typed.destination_path_, observer);
    wipe_native_value(typed.conversion_, observer);
    return;
}
case td::td_api::updateFileGenerationStop::ID: {
    auto& typed = static_cast<td::td_api::updateFileGenerationStop&>(value);
    wipe_native_value(typed.generation_id_, observer);
    return;
}
case td::td_api::updateFileRemovedFromDownloads::ID: {
    auto& typed = static_cast<td::td_api::updateFileRemovedFromDownloads&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.counts_, observer);
    return;
}
case td::td_api::updateForumTopic::ID: {
    auto& typed = static_cast<td::td_api::updateForumTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    wipe_native_value(typed.last_read_inbox_message_id_, observer);
    wipe_native_value(typed.last_read_outbox_message_id_, observer);
    wipe_native_value(typed.unread_mention_count_, observer);
    wipe_native_value(typed.unread_reaction_count_, observer);
    wipe_native_value(typed.unread_poll_vote_count_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    wipe_native_value(typed.draft_message_, observer);
    return;
}
case td::td_api::updateForumTopicInfo::ID: {
    auto& typed = static_cast<td::td_api::updateForumTopicInfo&>(value);
    wipe_native_value(typed.info_, observer);
    return;
}
case td::td_api::updateFreezeState::ID: {
    auto& typed = static_cast<td::td_api::updateFreezeState&>(value);
    wipe_native_value(typed.is_frozen_, observer);
    wipe_native_value(typed.freezing_date_, observer);
    wipe_native_value(typed.deletion_date_, observer);
    wipe_native_value(typed.appeal_link_, observer);
    return;
}
case td::td_api::updateGiftAuctionState::ID: {
    auto& typed = static_cast<td::td_api::updateGiftAuctionState&>(value);
    wipe_native_value(typed.state_, observer);
    return;
}
case td::td_api::updateGroupCall::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCall&>(value);
    wipe_native_value(typed.group_call_, observer);
    return;
}
case td::td_api::updateGroupCallMessageLevels::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCallMessageLevels&>(value);
    wipe_native_value(typed.levels_, observer);
    return;
}
case td::td_api::updateGroupCallMessageSendFailed::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCallMessageSendFailed&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.error_, observer);
    return;
}
case td::td_api::updateGroupCallMessagesDeleted::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCallMessagesDeleted&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::updateGroupCallParticipant::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCallParticipant&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_, observer);
    return;
}
case td::td_api::updateGroupCallParticipants::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCallParticipants&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_user_ids_, observer);
    return;
}
case td::td_api::updateGroupCallVerificationState::ID: {
    auto& typed = static_cast<td::td_api::updateGroupCallVerificationState&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.generation_, observer);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::updateHavePendingNotifications::ID: {
    auto& typed = static_cast<td::td_api::updateHavePendingNotifications&>(value);
    wipe_native_value(typed.have_delayed_notifications_, observer);
    wipe_native_value(typed.have_unreceived_notifications_, observer);
    return;
}
case td::td_api::updateInstalledStickerSets::ID: {
    auto& typed = static_cast<td::td_api::updateInstalledStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.sticker_set_ids_, observer);
    return;
}
case td::td_api::updateLanguagePackStrings::ID: {
    auto& typed = static_cast<td::td_api::updateLanguagePackStrings&>(value);
    wipe_native_value(typed.localization_target_, observer);
    wipe_native_value(typed.language_pack_id_, observer);
    wipe_native_value(typed.strings_, observer);
    return;
}
case td::td_api::updateLiveStoryTopDonors::ID: {
    auto& typed = static_cast<td::td_api::updateLiveStoryTopDonors&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.donors_, observer);
    return;
}
case td::td_api::updateManagedBot::ID: {
    auto& typed = static_cast<td::td_api::updateManagedBot&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::updateMessageContainsUnreadPollVotes::ID: {
    auto& typed = static_cast<td::td_api::updateMessageContainsUnreadPollVotes&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.contains_unread_poll_votes_, observer);
    wipe_native_value(typed.unread_poll_vote_count_, observer);
    return;
}
case td::td_api::updateMessageContent::ID: {
    auto& typed = static_cast<td::td_api::updateMessageContent&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.new_content_, observer);
    return;
}
case td::td_api::updateMessageContentOpened::ID: {
    auto& typed = static_cast<td::td_api::updateMessageContentOpened&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::updateMessageEdited::ID: {
    auto& typed = static_cast<td::td_api::updateMessageEdited&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.edit_date_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::updateMessageFactCheck::ID: {
    auto& typed = static_cast<td::td_api::updateMessageFactCheck&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.fact_check_, observer);
    return;
}
case td::td_api::updateMessageInteractionInfo::ID: {
    auto& typed = static_cast<td::td_api::updateMessageInteractionInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.interaction_info_, observer);
    return;
}
case td::td_api::updateMessageIsPinned::ID: {
    auto& typed = static_cast<td::td_api::updateMessageIsPinned&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::updateMessageLiveLocationViewed::ID: {
    auto& typed = static_cast<td::td_api::updateMessageLiveLocationViewed&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::updateMessageMentionRead::ID: {
    auto& typed = static_cast<td::td_api::updateMessageMentionRead&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.unread_mention_count_, observer);
    return;
}
case td::td_api::updateMessageReaction::ID: {
    auto& typed = static_cast<td::td_api::updateMessageReaction&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.actor_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.old_reaction_types_, observer);
    wipe_native_value(typed.new_reaction_types_, observer);
    return;
}
case td::td_api::updateMessageReactions::ID: {
    auto& typed = static_cast<td::td_api::updateMessageReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.date_, observer);
    wipe_native_value(typed.reactions_, observer);
    return;
}
case td::td_api::updateMessageSendAcknowledged::ID: {
    auto& typed = static_cast<td::td_api::updateMessageSendAcknowledged&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::updateMessageSendFailed::ID: {
    auto& typed = static_cast<td::td_api::updateMessageSendFailed&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.old_message_id_, observer);
    wipe_native_value(typed.error_, observer);
    return;
}
case td::td_api::updateMessageSendSucceeded::ID: {
    auto& typed = static_cast<td::td_api::updateMessageSendSucceeded&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.old_message_id_, observer);
    return;
}
case td::td_api::updateMessageSuggestedPostInfo::ID: {
    auto& typed = static_cast<td::td_api::updateMessageSuggestedPostInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.suggested_post_info_, observer);
    return;
}
case td::td_api::updateMessageUnreadReactions::ID: {
    auto& typed = static_cast<td::td_api::updateMessageUnreadReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.unread_reactions_, observer);
    wipe_native_value(typed.unread_reaction_count_, observer);
    return;
}
case td::td_api::updateNewBusinessCallbackQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewBusinessCallbackQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.connection_id_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.chat_instance_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::updateNewBusinessMessage::ID: {
    auto& typed = static_cast<td::td_api::updateNewBusinessMessage&>(value);
    wipe_native_value(typed.connection_id_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::updateNewCallSignalingData::ID: {
    auto& typed = static_cast<td::td_api::updateNewCallSignalingData&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::updateNewCallbackQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewCallbackQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.chat_instance_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::updateNewChat::ID: {
    auto& typed = static_cast<td::td_api::updateNewChat&>(value);
    wipe_native_value(typed.chat_, observer);
    return;
}
case td::td_api::updateNewChatJoinRequest::ID: {
    auto& typed = static_cast<td::td_api::updateNewChatJoinRequest&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.request_, observer);
    wipe_native_value(typed.user_chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.query_id_, observer);
    return;
}
case td::td_api::updateNewChosenInlineResult::ID: {
    auto& typed = static_cast<td::td_api::updateNewChosenInlineResult&>(value);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.user_location_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.result_id_, observer);
    wipe_native_value(typed.inline_message_id_, observer);
    return;
}
case td::td_api::updateNewCustomEvent::ID: {
    auto& typed = static_cast<td::td_api::updateNewCustomEvent&>(value);
    wipe_native_value(typed.event_, observer);
    return;
}
case td::td_api::updateNewCustomQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewCustomQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.data_, observer);
    wipe_native_value(typed.timeout_, observer);
    return;
}
case td::td_api::updateNewGroupCallMessage::ID: {
    auto& typed = static_cast<td::td_api::updateNewGroupCallMessage&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::updateNewGroupCallPaidReaction::ID: {
    auto& typed = static_cast<td::td_api::updateNewGroupCallPaidReaction&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::updateNewGuestQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewGuestQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.reference_messages_, observer);
    return;
}
case td::td_api::updateNewInlineCallbackQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewInlineCallbackQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.chat_instance_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::updateNewInlineQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewInlineQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.user_location_, observer);
    wipe_native_value(typed.chat_type_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_, observer);
    return;
}
case td::td_api::updateNewMessage::ID: {
    auto& typed = static_cast<td::td_api::updateNewMessage&>(value);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::updateNewOauthRequest::ID: {
    auto& typed = static_cast<td::td_api::updateNewOauthRequest&>(value);
    wipe_native_value(typed.domain_, observer);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::updateNewPreCheckoutQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewPreCheckoutQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.total_amount_, observer);
    wipe_native_value(typed.invoice_payload_, observer);
    wipe_native_value(typed.shipping_option_id_, observer);
    wipe_native_value(typed.order_info_, observer);
    return;
}
case td::td_api::updateNewShippingQuery::ID: {
    auto& typed = static_cast<td::td_api::updateNewShippingQuery&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.sender_user_id_, observer);
    wipe_native_value(typed.invoice_payload_, observer);
    wipe_native_value(typed.shipping_address_, observer);
    return;
}
case td::td_api::updateNotification::ID: {
    auto& typed = static_cast<td::td_api::updateNotification&>(value);
    wipe_native_value(typed.notification_group_id_, observer);
    wipe_native_value(typed.notification_, observer);
    return;
}
case td::td_api::updateNotificationGroup::ID: {
    auto& typed = static_cast<td::td_api::updateNotificationGroup&>(value);
    wipe_native_value(typed.notification_group_id_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.notification_settings_chat_id_, observer);
    wipe_native_value(typed.notification_sound_id_, observer);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.added_notifications_, observer);
    wipe_native_value(typed.removed_notification_ids_, observer);
    return;
}
case td::td_api::updateOption::ID: {
    auto& typed = static_cast<td::td_api::updateOption&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::updateOwnedStarCount::ID: {
    auto& typed = static_cast<td::td_api::updateOwnedStarCount&>(value);
    wipe_native_value(typed.star_amount_, observer);
    return;
}
case td::td_api::updateOwnedTonCount::ID: {
    auto& typed = static_cast<td::td_api::updateOwnedTonCount&>(value);
    wipe_native_value(typed.ton_amount_, observer);
    return;
}
case td::td_api::updatePaidMediaPurchased::ID: {
    auto& typed = static_cast<td::td_api::updatePaidMediaPurchased&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::updatePendingMessage::ID: {
    auto& typed = static_cast<td::td_api::updatePendingMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.draft_id_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::updatePoll::ID: {
    auto& typed = static_cast<td::td_api::updatePoll&>(value);
    wipe_native_value(typed.poll_, observer);
    return;
}
case td::td_api::updatePollAnswer::ID: {
    auto& typed = static_cast<td::td_api::updatePollAnswer&>(value);
    wipe_native_value(typed.poll_id_, observer);
    wipe_native_value(typed.voter_id_, observer);
    wipe_native_value(typed.option_ids_, observer);
    wipe_native_value(typed.option_positions_, observer);
    return;
}
case td::td_api::updateProfileAccentColors::ID: {
    auto& typed = static_cast<td::td_api::updateProfileAccentColors&>(value);
    wipe_native_value(typed.colors_, observer);
    wipe_native_value(typed.available_accent_color_ids_, observer);
    return;
}
case td::td_api::updateQuickReplyShortcut::ID: {
    auto& typed = static_cast<td::td_api::updateQuickReplyShortcut&>(value);
    wipe_native_value(typed.shortcut_, observer);
    return;
}
case td::td_api::updateQuickReplyShortcutDeleted::ID: {
    auto& typed = static_cast<td::td_api::updateQuickReplyShortcutDeleted&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    return;
}
case td::td_api::updateQuickReplyShortcutMessages::ID: {
    auto& typed = static_cast<td::td_api::updateQuickReplyShortcutMessages&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.messages_, observer);
    return;
}
case td::td_api::updateQuickReplyShortcuts::ID: {
    auto& typed = static_cast<td::td_api::updateQuickReplyShortcuts&>(value);
    wipe_native_value(typed.shortcut_ids_, observer);
    return;
}
case td::td_api::updateReactionNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::updateReactionNotificationSettings&>(value);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::updateRecentStickers::ID: {
    auto& typed = static_cast<td::td_api::updateRecentStickers&>(value);
    wipe_native_value(typed.is_attached_, observer);
    wipe_native_value(typed.sticker_ids_, observer);
    return;
}
case td::td_api::updateSavedAnimations::ID: {
    auto& typed = static_cast<td::td_api::updateSavedAnimations&>(value);
    wipe_native_value(typed.animation_ids_, observer);
    return;
}
case td::td_api::updateSavedMessagesTags::ID: {
    auto& typed = static_cast<td::td_api::updateSavedMessagesTags&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    wipe_native_value(typed.tags_, observer);
    return;
}
case td::td_api::updateSavedMessagesTopic::ID: {
    auto& typed = static_cast<td::td_api::updateSavedMessagesTopic&>(value);
    wipe_native_value(typed.topic_, observer);
    return;
}
case td::td_api::updateSavedMessagesTopicCount::ID: {
    auto& typed = static_cast<td::td_api::updateSavedMessagesTopicCount&>(value);
    wipe_native_value(typed.topic_count_, observer);
    return;
}
case td::td_api::updateSavedNotificationSounds::ID: {
    auto& typed = static_cast<td::td_api::updateSavedNotificationSounds&>(value);
    wipe_native_value(typed.notification_sound_ids_, observer);
    return;
}
case td::td_api::updateScopeNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::updateScopeNotificationSettings&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::updateSecretChat::ID: {
    auto& typed = static_cast<td::td_api::updateSecretChat&>(value);
    wipe_native_value(typed.secret_chat_, observer);
    return;
}
case td::td_api::updateServiceNotification::ID: {
    auto& typed = static_cast<td::td_api::updateServiceNotification&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::updateSpeechRecognitionTrial::ID: {
    auto& typed = static_cast<td::td_api::updateSpeechRecognitionTrial&>(value);
    wipe_native_value(typed.max_media_duration_, observer);
    wipe_native_value(typed.weekly_count_, observer);
    wipe_native_value(typed.left_count_, observer);
    wipe_native_value(typed.next_reset_date_, observer);
    return;
}
case td::td_api::updateSpeedLimitNotification::ID: {
    auto& typed = static_cast<td::td_api::updateSpeedLimitNotification&>(value);
    wipe_native_value(typed.is_upload_, observer);
    return;
}
case td::td_api::updateStakeDiceState::ID: {
    auto& typed = static_cast<td::td_api::updateStakeDiceState&>(value);
    wipe_native_value(typed.state_, observer);
    return;
}
case td::td_api::updateStarRevenueStatus::ID: {
    auto& typed = static_cast<td::td_api::updateStarRevenueStatus&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.status_, observer);
    return;
}
case td::td_api::updateStickerSet::ID: {
    auto& typed = static_cast<td::td_api::updateStickerSet&>(value);
    wipe_native_value(typed.sticker_set_, observer);
    return;
}
case td::td_api::updateStory::ID: {
    auto& typed = static_cast<td::td_api::updateStory&>(value);
    wipe_native_value(typed.story_, observer);
    return;
}
case td::td_api::updateStoryDeleted::ID: {
    auto& typed = static_cast<td::td_api::updateStoryDeleted&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::updateStoryListChatCount::ID: {
    auto& typed = static_cast<td::td_api::updateStoryListChatCount&>(value);
    wipe_native_value(typed.story_list_, observer);
    wipe_native_value(typed.chat_count_, observer);
    return;
}
case td::td_api::updateStoryPostFailed::ID: {
    auto& typed = static_cast<td::td_api::updateStoryPostFailed&>(value);
    wipe_native_value(typed.story_, observer);
    wipe_native_value(typed.error_, observer);
    wipe_native_value(typed.error_type_, observer);
    return;
}
case td::td_api::updateStoryPostSucceeded::ID: {
    auto& typed = static_cast<td::td_api::updateStoryPostSucceeded&>(value);
    wipe_native_value(typed.story_, observer);
    wipe_native_value(typed.old_story_id_, observer);
    return;
}
case td::td_api::updateStoryStealthMode::ID: {
    auto& typed = static_cast<td::td_api::updateStoryStealthMode&>(value);
    wipe_native_value(typed.active_until_date_, observer);
    wipe_native_value(typed.cooldown_until_date_, observer);
    return;
}
case td::td_api::updateSuggestedActions::ID: {
    auto& typed = static_cast<td::td_api::updateSuggestedActions&>(value);
    wipe_native_value(typed.added_actions_, observer);
    wipe_native_value(typed.removed_actions_, observer);
    return;
}
case td::td_api::updateSupergroup::ID: {
    auto& typed = static_cast<td::td_api::updateSupergroup&>(value);
    wipe_native_value(typed.supergroup_, observer);
    return;
}
case td::td_api::updateSupergroupFullInfo::ID: {
    auto& typed = static_cast<td::td_api::updateSupergroupFullInfo&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.supergroup_full_info_, observer);
    return;
}
case td::td_api::updateTermsOfService::ID: {
    auto& typed = static_cast<td::td_api::updateTermsOfService&>(value);
    wipe_native_value(typed.terms_of_service_id_, observer);
    wipe_native_value(typed.terms_of_service_, observer);
    return;
}
case td::td_api::updateTextCompositionStyles::ID: {
    auto& typed = static_cast<td::td_api::updateTextCompositionStyles&>(value);
    wipe_native_value(typed.styles_, observer);
    return;
}
case td::td_api::updateTonRevenueStatus::ID: {
    auto& typed = static_cast<td::td_api::updateTonRevenueStatus&>(value);
    wipe_native_value(typed.status_, observer);
    return;
}
case td::td_api::updateTopicMessageCount::ID: {
    auto& typed = static_cast<td::td_api::updateTopicMessageCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.message_count_, observer);
    return;
}
case td::td_api::updateTrendingStickerSets::ID: {
    auto& typed = static_cast<td::td_api::updateTrendingStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.sticker_sets_, observer);
    return;
}
case td::td_api::updateTrustedMiniAppBots::ID: {
    auto& typed = static_cast<td::td_api::updateTrustedMiniAppBots&>(value);
    wipe_native_value(typed.bot_user_ids_, observer);
    return;
}
case td::td_api::updateUnconfirmedSession::ID: {
    auto& typed = static_cast<td::td_api::updateUnconfirmedSession&>(value);
    wipe_native_value(typed.session_, observer);
    wipe_native_value(typed.unconfirmed_session_count_, observer);
    return;
}
case td::td_api::updateUnreadChatCount::ID: {
    auto& typed = static_cast<td::td_api::updateUnreadChatCount&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.unread_count_, observer);
    wipe_native_value(typed.unread_unmuted_count_, observer);
    wipe_native_value(typed.marked_as_unread_count_, observer);
    wipe_native_value(typed.marked_as_unread_unmuted_count_, observer);
    return;
}
case td::td_api::updateUnreadMessageCount::ID: {
    auto& typed = static_cast<td::td_api::updateUnreadMessageCount&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.unread_count_, observer);
    wipe_native_value(typed.unread_unmuted_count_, observer);
    return;
}
case td::td_api::updateUser::ID: {
    auto& typed = static_cast<td::td_api::updateUser&>(value);
    wipe_native_value(typed.user_, observer);
    return;
}
case td::td_api::updateUserFullInfo::ID: {
    auto& typed = static_cast<td::td_api::updateUserFullInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.user_full_info_, observer);
    return;
}
case td::td_api::updateUserPrivacySettingRules::ID: {
    auto& typed = static_cast<td::td_api::updateUserPrivacySettingRules&>(value);
    wipe_native_value(typed.setting_, observer);
    wipe_native_value(typed.rules_, observer);
    return;
}
case td::td_api::updateUserStatus::ID: {
    auto& typed = static_cast<td::td_api::updateUserStatus&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.status_, observer);
    return;
}
case td::td_api::updateVideoPublished::ID: {
    auto& typed = static_cast<td::td_api::updateVideoPublished&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::updateWebAppMessageSent::ID: {
    auto& typed = static_cast<td::td_api::updateWebAppMessageSent&>(value);
    wipe_native_value(typed.web_app_launch_id_, observer);
    return;
}
case td::td_api::updateWebBrowserSettings::ID: {
    auto& typed = static_cast<td::td_api::updateWebBrowserSettings&>(value);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::updates::ID: {
    auto& typed = static_cast<td::td_api::updates&>(value);
    wipe_native_value(typed.updates_, observer);
    return;
}
case td::td_api::upgradeGiftResult::ID: {
    auto& typed = static_cast<td::td_api::upgradeGiftResult&>(value);
    wipe_native_value(typed.gift_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.is_saved_, observer);
    wipe_native_value(typed.can_be_transferred_, observer);
    wipe_native_value(typed.transfer_star_count_, observer);
    wipe_native_value(typed.drop_original_details_star_count_, observer);
    wipe_native_value(typed.next_transfer_date_, observer);
    wipe_native_value(typed.next_resale_date_, observer);
    wipe_native_value(typed.export_date_, observer);
    return;
}
case td::td_api::upgradedGift::ID: {
    auto& typed = static_cast<td::td_api::upgradedGift&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.regular_gift_id_, observer);
    wipe_native_value(typed.publisher_chat_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.number_, observer);
    wipe_native_value(typed.total_upgraded_count_, observer);
    wipe_native_value(typed.max_upgraded_count_, observer);
    wipe_native_value(typed.is_burned_, observer);
    wipe_native_value(typed.is_crafted_, observer);
    wipe_native_value(typed.is_premium_, observer);
    wipe_native_value(typed.is_theme_available_, observer);
    wipe_native_value(typed.used_theme_chat_id_, observer);
    wipe_native_value(typed.host_id_, observer);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.owner_address_, observer);
    wipe_native_value(typed.owner_name_, observer);
    wipe_native_value(typed.gift_address_, observer);
    wipe_native_value(typed.model_, observer);
    wipe_native_value(typed.symbol_, observer);
    wipe_native_value(typed.backdrop_, observer);
    wipe_native_value(typed.original_details_, observer);
    wipe_native_value(typed.colors_, observer);
    wipe_native_value(typed.resale_parameters_, observer);
    wipe_native_value(typed.can_send_purchase_offer_, observer);
    wipe_native_value(typed.craft_probability_per_mille_, observer);
    wipe_native_value(typed.value_currency_, observer);
    wipe_native_value(typed.value_amount_, observer);
    wipe_native_value(typed.value_usd_amount_, observer);
    return;
}
case td::td_api::upgradedGiftAttributeIdBackdrop::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftAttributeIdBackdrop&>(value);
    wipe_native_value(typed.backdrop_id_, observer);
    return;
}
case td::td_api::upgradedGiftAttributeIdModel::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftAttributeIdModel&>(value);
    wipe_native_value(typed.sticker_id_, observer);
    return;
}
case td::td_api::upgradedGiftAttributeIdSymbol::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftAttributeIdSymbol&>(value);
    wipe_native_value(typed.sticker_id_, observer);
    return;
}
case td::td_api::upgradedGiftAttributeRarityEpic::ID: {
    return;
}
case td::td_api::upgradedGiftAttributeRarityLegendary::ID: {
    return;
}
case td::td_api::upgradedGiftAttributeRarityPerMille::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftAttributeRarityPerMille&>(value);
    wipe_native_value(typed.per_mille_, observer);
    return;
}
case td::td_api::upgradedGiftAttributeRarityRare::ID: {
    return;
}
case td::td_api::upgradedGiftAttributeRarityUncommon::ID: {
    return;
}
case td::td_api::upgradedGiftBackdrop::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftBackdrop&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.colors_, observer);
    wipe_native_value(typed.rarity_, observer);
    return;
}
case td::td_api::upgradedGiftBackdropColors::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftBackdropColors&>(value);
    wipe_native_value(typed.center_color_, observer);
    wipe_native_value(typed.edge_color_, observer);
    wipe_native_value(typed.symbol_color_, observer);
    wipe_native_value(typed.text_color_, observer);
    return;
}
case td::td_api::upgradedGiftBackdropCount::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftBackdropCount&>(value);
    wipe_native_value(typed.backdrop_, observer);
    wipe_native_value(typed.total_count_, observer);
    return;
}
case td::td_api::upgradedGiftColors::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftColors&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.model_custom_emoji_id_, observer);
    wipe_native_value(typed.symbol_custom_emoji_id_, observer);
    wipe_native_value(typed.light_theme_accent_color_, observer);
    wipe_native_value(typed.light_theme_colors_, observer);
    wipe_native_value(typed.dark_theme_accent_color_, observer);
    wipe_native_value(typed.dark_theme_colors_, observer);
    return;
}
case td::td_api::upgradedGiftModel::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftModel&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.rarity_, observer);
    wipe_native_value(typed.is_crafted_, observer);
    return;
}
case td::td_api::upgradedGiftModelCount::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftModelCount&>(value);
    wipe_native_value(typed.model_, observer);
    wipe_native_value(typed.total_count_, observer);
    return;
}
case td::td_api::upgradedGiftOriginBlockchain::ID: {
    return;
}
case td::td_api::upgradedGiftOriginCraft::ID: {
    return;
}
case td::td_api::upgradedGiftOriginOffer::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftOriginOffer&>(value);
    wipe_native_value(typed.price_, observer);
    return;
}
case td::td_api::upgradedGiftOriginPrepaidUpgrade::ID: {
    return;
}
case td::td_api::upgradedGiftOriginResale::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftOriginResale&>(value);
    wipe_native_value(typed.price_, observer);
    return;
}
case td::td_api::upgradedGiftOriginTransfer::ID: {
    return;
}
case td::td_api::upgradedGiftOriginUpgrade::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftOriginUpgrade&>(value);
    wipe_native_value(typed.gift_message_id_, observer);
    return;
}
case td::td_api::upgradedGiftOriginalDetails::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftOriginalDetails&>(value);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.receiver_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::upgradedGiftSymbol::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftSymbol&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.rarity_, observer);
    return;
}
case td::td_api::upgradedGiftSymbolCount::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftSymbolCount&>(value);
    wipe_native_value(typed.symbol_, observer);
    wipe_native_value(typed.total_count_, observer);
    return;
}
case td::td_api::upgradedGiftValueInfo::ID: {
    auto& typed = static_cast<td::td_api::upgradedGiftValueInfo&>(value);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.value_, observer);
    wipe_native_value(typed.is_value_average_, observer);
    wipe_native_value(typed.initial_sale_date_, observer);
    wipe_native_value(typed.initial_sale_star_count_, observer);
    wipe_native_value(typed.initial_sale_price_, observer);
    wipe_native_value(typed.last_sale_date_, observer);
    wipe_native_value(typed.last_sale_price_, observer);
    wipe_native_value(typed.is_last_sale_on_fragment_, observer);
    wipe_native_value(typed.minimum_price_, observer);
    wipe_native_value(typed.average_sale_price_, observer);
    wipe_native_value(typed.telegram_listed_gift_count_, observer);
    wipe_native_value(typed.fragment_listed_gift_count_, observer);
    wipe_native_value(typed.fragment_url_, observer);
    return;
}
case td::td_api::user::ID: {
    auto& typed = static_cast<td::td_api::user&>(value);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    wipe_native_value(typed.usernames_, observer);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.status_, observer);
    wipe_native_value(typed.profile_photo_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.background_custom_emoji_id_, observer);
    wipe_native_value(typed.upgraded_gift_colors_, observer);
    wipe_native_value(typed.profile_accent_color_id_, observer);
    wipe_native_value(typed.profile_background_custom_emoji_id_, observer);
    wipe_native_value(typed.emoji_status_, observer);
    wipe_native_value(typed.is_contact_, observer);
    wipe_native_value(typed.is_mutual_contact_, observer);
    wipe_native_value(typed.is_close_friend_, observer);
    wipe_native_value(typed.verification_status_, observer);
    wipe_native_value(typed.is_premium_, observer);
    wipe_native_value(typed.is_support_, observer);
    wipe_native_value(typed.restriction_info_, observer);
    wipe_native_value(typed.active_story_state_, observer);
    wipe_native_value(typed.restricts_new_chats_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    wipe_native_value(typed.have_access_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.added_to_attachment_menu_, observer);
    return;
}
case td::td_api::userAuctionBid::ID: {
    auto& typed = static_cast<td::td_api::userAuctionBid&>(value);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.bid_date_, observer);
    wipe_native_value(typed.next_bid_star_count_, observer);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.was_returned_, observer);
    return;
}
case td::td_api::userFullInfo::ID: {
    auto& typed = static_cast<td::td_api::userFullInfo&>(value);
    wipe_native_value(typed.personal_photo_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.public_photo_, observer);
    wipe_native_value(typed.block_list_, observer);
    wipe_native_value(typed.can_be_called_, observer);
    wipe_native_value(typed.supports_video_calls_, observer);
    wipe_native_value(typed.has_private_calls_, observer);
    wipe_native_value(typed.has_private_forwards_, observer);
    wipe_native_value(typed.has_restricted_voice_and_video_note_messages_, observer);
    wipe_native_value(typed.has_posted_to_profile_stories_, observer);
    wipe_native_value(typed.has_sponsored_messages_enabled_, observer);
    wipe_native_value(typed.need_phone_number_privacy_exception_, observer);
    wipe_native_value(typed.set_chat_background_, observer);
    wipe_native_value(typed.uses_unofficial_app_, observer);
    wipe_native_value(typed.bio_, observer);
    wipe_native_value(typed.birthdate_, observer);
    wipe_native_value(typed.personal_chat_id_, observer);
    wipe_native_value(typed.gift_count_, observer);
    wipe_native_value(typed.group_in_common_count_, observer);
    wipe_native_value(typed.incoming_paid_message_star_count_, observer);
    wipe_native_value(typed.outgoing_paid_message_star_count_, observer);
    wipe_native_value(typed.gift_settings_, observer);
    wipe_native_value(typed.bot_verification_, observer);
    wipe_native_value(typed.main_profile_tab_, observer);
    wipe_native_value(typed.first_profile_audio_, observer);
    wipe_native_value(typed.rating_, observer);
    wipe_native_value(typed.pending_rating_, observer);
    wipe_native_value(typed.pending_rating_date_, observer);
    wipe_native_value(typed.note_, observer);
    wipe_native_value(typed.business_info_, observer);
    wipe_native_value(typed.bot_info_, observer);
    return;
}
case td::td_api::userLink::ID: {
    auto& typed = static_cast<td::td_api::userLink&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.expires_in_, observer);
    return;
}
case td::td_api::userPrivacySettingAllowCalls::ID: {
    return;
}
case td::td_api::userPrivacySettingAllowChatInvites::ID: {
    return;
}
case td::td_api::userPrivacySettingAllowFindingByPhoneNumber::ID: {
    return;
}
case td::td_api::userPrivacySettingAllowPeerToPeerCalls::ID: {
    return;
}
case td::td_api::userPrivacySettingAllowPrivateVoiceAndVideoNoteMessages::ID: {
    return;
}
case td::td_api::userPrivacySettingAllowUnpaidMessages::ID: {
    return;
}
case td::td_api::userPrivacySettingAutosaveGifts::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleAllowAll::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleAllowBots::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleAllowChatMembers::ID: {
    auto& typed = static_cast<td::td_api::userPrivacySettingRuleAllowChatMembers&>(value);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::userPrivacySettingRuleAllowContacts::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleAllowPremiumUsers::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleAllowUsers::ID: {
    auto& typed = static_cast<td::td_api::userPrivacySettingRuleAllowUsers&>(value);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::userPrivacySettingRuleRestrictAll::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleRestrictBots::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleRestrictChatMembers::ID: {
    auto& typed = static_cast<td::td_api::userPrivacySettingRuleRestrictChatMembers&>(value);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::userPrivacySettingRuleRestrictContacts::ID: {
    return;
}
case td::td_api::userPrivacySettingRuleRestrictUsers::ID: {
    auto& typed = static_cast<td::td_api::userPrivacySettingRuleRestrictUsers&>(value);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::userPrivacySettingRules::ID: {
    auto& typed = static_cast<td::td_api::userPrivacySettingRules&>(value);
    wipe_native_value(typed.rules_, observer);
    return;
}
case td::td_api::userPrivacySettingShowBio::ID: {
    return;
}
case td::td_api::userPrivacySettingShowBirthdate::ID: {
    return;
}
case td::td_api::userPrivacySettingShowLinkInForwardedMessages::ID: {
    return;
}
case td::td_api::userPrivacySettingShowPhoneNumber::ID: {
    return;
}
case td::td_api::userPrivacySettingShowProfileAudio::ID: {
    return;
}
case td::td_api::userPrivacySettingShowProfilePhoto::ID: {
    return;
}
case td::td_api::userPrivacySettingShowStatus::ID: {
    return;
}
case td::td_api::userRating::ID: {
    auto& typed = static_cast<td::td_api::userRating&>(value);
    wipe_native_value(typed.level_, observer);
    wipe_native_value(typed.is_maximum_level_reached_, observer);
    wipe_native_value(typed.rating_, observer);
    wipe_native_value(typed.current_level_rating_, observer);
    wipe_native_value(typed.next_level_rating_, observer);
    return;
}
case td::td_api::userStatusEmpty::ID: {
    return;
}
case td::td_api::userStatusLastMonth::ID: {
    auto& typed = static_cast<td::td_api::userStatusLastMonth&>(value);
    wipe_native_value(typed.by_my_privacy_settings_, observer);
    return;
}
case td::td_api::userStatusLastWeek::ID: {
    auto& typed = static_cast<td::td_api::userStatusLastWeek&>(value);
    wipe_native_value(typed.by_my_privacy_settings_, observer);
    return;
}
case td::td_api::userStatusOffline::ID: {
    auto& typed = static_cast<td::td_api::userStatusOffline&>(value);
    wipe_native_value(typed.was_online_, observer);
    return;
}
case td::td_api::userStatusOnline::ID: {
    auto& typed = static_cast<td::td_api::userStatusOnline&>(value);
    wipe_native_value(typed.expires_, observer);
    return;
}
case td::td_api::userStatusRecently::ID: {
    auto& typed = static_cast<td::td_api::userStatusRecently&>(value);
    wipe_native_value(typed.by_my_privacy_settings_, observer);
    return;
}
case td::td_api::userSupportInfo::ID: {
    auto& typed = static_cast<td::td_api::userSupportInfo&>(value);
    wipe_native_value(typed.message_, observer);
    wipe_native_value(typed.author_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::userTypeBot::ID: {
    auto& typed = static_cast<td::td_api::userTypeBot&>(value);
    wipe_native_value(typed.can_be_edited_, observer);
    wipe_native_value(typed.can_join_groups_, observer);
    wipe_native_value(typed.can_read_all_group_messages_, observer);
    wipe_native_value(typed.has_main_web_app_, observer);
    wipe_native_value(typed.has_topics_, observer);
    wipe_native_value(typed.allows_users_to_create_topics_, observer);
    wipe_native_value(typed.can_manage_bots_, observer);
    wipe_native_value(typed.is_inline_, observer);
    wipe_native_value(typed.inline_query_placeholder_, observer);
    wipe_native_value(typed.supports_guest_queries_, observer);
    wipe_native_value(typed.is_guard_, observer);
    wipe_native_value(typed.need_location_, observer);
    wipe_native_value(typed.can_connect_to_business_, observer);
    wipe_native_value(typed.can_be_added_to_attachment_menu_, observer);
    wipe_native_value(typed.active_user_count_, observer);
    return;
}
case td::td_api::userTypeDeleted::ID: {
    return;
}
case td::td_api::userTypeRegular::ID: {
    return;
}
case td::td_api::userTypeUnknown::ID: {
    return;
}
case td::td_api::usernames::ID: {
    auto& typed = static_cast<td::td_api::usernames&>(value);
    wipe_native_value(typed.active_usernames_, observer);
    wipe_native_value(typed.disabled_usernames_, observer);
    wipe_native_value(typed.editable_username_, observer);
    wipe_native_value(typed.collectible_usernames_, observer);
    return;
}
case td::td_api::users::ID: {
    auto& typed = static_cast<td::td_api::users&>(value);
    wipe_native_value(typed.total_count_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::validatedOrderInfo::ID: {
    auto& typed = static_cast<td::td_api::validatedOrderInfo&>(value);
    wipe_native_value(typed.order_info_id_, observer);
    wipe_native_value(typed.shipping_options_, observer);
    return;
}
case td::td_api::vectorPathCommandCubicBezierCurve::ID: {
    auto& typed = static_cast<td::td_api::vectorPathCommandCubicBezierCurve&>(value);
    wipe_native_value(typed.start_control_point_, observer);
    wipe_native_value(typed.end_control_point_, observer);
    wipe_native_value(typed.end_point_, observer);
    return;
}
case td::td_api::vectorPathCommandLine::ID: {
    auto& typed = static_cast<td::td_api::vectorPathCommandLine&>(value);
    wipe_native_value(typed.end_point_, observer);
    return;
}
case td::td_api::venue::ID: {
    auto& typed = static_cast<td::td_api::venue&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.address_, observer);
    wipe_native_value(typed.provider_, observer);
    wipe_native_value(typed.id_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::verificationStatus::ID: {
    auto& typed = static_cast<td::td_api::verificationStatus&>(value);
    wipe_native_value(typed.is_verified_, observer);
    wipe_native_value(typed.is_scam_, observer);
    wipe_native_value(typed.is_fake_, observer);
    wipe_native_value(typed.bot_verification_icon_custom_emoji_id_, observer);
    return;
}
case td::td_api::video::ID: {
    auto& typed = static_cast<td::td_api::video&>(value);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.file_name_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.has_stickers_, observer);
    wipe_native_value(typed.supports_streaming_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::videoChat::ID: {
    auto& typed = static_cast<td::td_api::videoChat&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.has_participants_, observer);
    wipe_native_value(typed.default_participant_id_, observer);
    return;
}
case td::td_api::videoMessageAdvertisement::ID: {
    auto& typed = static_cast<td::td_api::videoMessageAdvertisement&>(value);
    wipe_native_value(typed.unique_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.min_display_duration_, observer);
    wipe_native_value(typed.max_display_duration_, observer);
    wipe_native_value(typed.can_be_reported_, observer);
    wipe_native_value(typed.sponsor_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.additional_info_, observer);
    return;
}
case td::td_api::videoMessageAdvertisements::ID: {
    auto& typed = static_cast<td::td_api::videoMessageAdvertisements&>(value);
    wipe_native_value(typed.advertisements_, observer);
    wipe_native_value(typed.start_delay_, observer);
    wipe_native_value(typed.between_delay_, observer);
    return;
}
case td::td_api::videoNote::ID: {
    auto& typed = static_cast<td::td_api::videoNote&>(value);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.waveform_, observer);
    wipe_native_value(typed.length_, observer);
    wipe_native_value(typed.minithumbnail_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.speech_recognition_result_, observer);
    wipe_native_value(typed.video_, observer);
    return;
}
case td::td_api::videoStoryboard::ID: {
    auto& typed = static_cast<td::td_api::videoStoryboard&>(value);
    wipe_native_value(typed.storyboard_file_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.map_file_, observer);
    return;
}
case td::td_api::voiceNote::ID: {
    auto& typed = static_cast<td::td_api::voiceNote&>(value);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.waveform_, observer);
    wipe_native_value(typed.mime_type_, observer);
    wipe_native_value(typed.speech_recognition_result_, observer);
    wipe_native_value(typed.voice_, observer);
    return;
}
case td::td_api::webApp::ID: {
    auto& typed = static_cast<td::td_api::webApp&>(value);
    wipe_native_value(typed.short_name_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::webAppInfo::ID: {
    auto& typed = static_cast<td::td_api::webAppInfo&>(value);
    wipe_native_value(typed.launch_id_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::webAppOpenModeCompact::ID: {
    return;
}
case td::td_api::webAppOpenModeFullScreen::ID: {
    return;
}
case td::td_api::webAppOpenModeFullSize::ID: {
    return;
}
case td::td_api::webAppOpenParameters::ID: {
    auto& typed = static_cast<td::td_api::webAppOpenParameters&>(value);
    wipe_native_value(typed.theme_, observer);
    wipe_native_value(typed.application_name_, observer);
    wipe_native_value(typed.mode_, observer);
    return;
}
case td::td_api::webAppUrl::ID: {
    auto& typed = static_cast<td::td_api::webAppUrl&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.require_same_origin_, observer);
    return;
}
case td::td_api::webBrowserSettings::ID: {
    auto& typed = static_cast<td::td_api::webBrowserSettings&>(value);
    wipe_native_value(typed.open_external_browser_, observer);
    wipe_native_value(typed.external_exceptions_, observer);
    wipe_native_value(typed.in_app_exceptions_, observer);
    wipe_native_value(typed.display_close_button_, observer);
    return;
}
case td::td_api::webBrowserTypeExternal::ID: {
    return;
}
case td::td_api::webBrowserTypeInApp::ID: {
    return;
}
case td::td_api::webDomainException::ID: {
    auto& typed = static_cast<td::td_api::webDomainException&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.domain_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.favicon_custom_emoji_id_, observer);
    return;
}
case td::td_api::webPageInstantView::ID: {
    auto& typed = static_cast<td::td_api::webPageInstantView&>(value);
    wipe_native_value(typed.blocks_, observer);
    wipe_native_value(typed.view_count_, observer);
    wipe_native_value(typed.version_, observer);
    wipe_native_value(typed.is_rtl_, observer);
    wipe_native_value(typed.is_full_, observer);
    wipe_native_value(typed.feedback_link_, observer);
    return;
}
    default:
        return;
    }
}

void wipe_native_function(td::td_api::Function& value,
                          const secure::WipeObserver& observer) noexcept {
    switch (value.get_id()) {
case td::td_api::acceptCall::ID: {
    auto& typed = static_cast<td::td_api::acceptCall&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.protocol_, observer);
    return;
}
case td::td_api::acceptOauthRequest::ID: {
    auto& typed = static_cast<td::td_api::acceptOauthRequest&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.match_code_, observer);
    wipe_native_value(typed.allow_write_access_, observer);
    wipe_native_value(typed.allow_phone_number_access_, observer);
    return;
}
case td::td_api::acceptTermsOfService::ID: {
    auto& typed = static_cast<td::td_api::acceptTermsOfService&>(value);
    wipe_native_value(typed.terms_of_service_id_, observer);
    return;
}
case td::td_api::activateStoryStealthMode::ID: {
    return;
}
case td::td_api::addBotMediaPreview::ID: {
    auto& typed = static_cast<td::td_api::addBotMediaPreview&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::addChatFolderByInviteLink::ID: {
    auto& typed = static_cast<td::td_api::addChatFolderByInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::addChatMember::ID: {
    auto& typed = static_cast<td::td_api::addChatMember&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.forward_limit_, observer);
    return;
}
case td::td_api::addChatMembers::ID: {
    auto& typed = static_cast<td::td_api::addChatMembers&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::addChatToList::ID: {
    auto& typed = static_cast<td::td_api::addChatToList&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.chat_list_, observer);
    return;
}
case td::td_api::addChecklistTasks::ID: {
    auto& typed = static_cast<td::td_api::addChecklistTasks&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.tasks_, observer);
    return;
}
case td::td_api::addContact::ID: {
    auto& typed = static_cast<td::td_api::addContact&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.contact_, observer);
    wipe_native_value(typed.share_phone_number_, observer);
    return;
}
case td::td_api::addCustomServerLanguagePack::ID: {
    auto& typed = static_cast<td::td_api::addCustomServerLanguagePack&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    return;
}
case td::td_api::addFavoriteSticker::ID: {
    auto& typed = static_cast<td::td_api::addFavoriteSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::addFileToDownloads::ID: {
    auto& typed = static_cast<td::td_api::addFileToDownloads&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.priority_, observer);
    return;
}
case td::td_api::addGiftCollectionGifts::ID: {
    auto& typed = static_cast<td::td_api::addGiftCollectionGifts&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_id_, observer);
    wipe_native_value(typed.received_gift_ids_, observer);
    return;
}
case td::td_api::addLocalMessage::ID: {
    auto& typed = static_cast<td::td_api::addLocalMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.disable_notification_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::addLogMessage::ID: {
    auto& typed = static_cast<td::td_api::addLogMessage&>(value);
    wipe_native_value(typed.verbosity_level_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::addLoginPasskey::ID: {
    auto& typed = static_cast<td::td_api::addLoginPasskey&>(value);
    wipe_native_value(typed.client_data_, observer);
    wipe_native_value(typed.attestation_object_, observer);
    return;
}
case td::td_api::addMessageReaction::ID: {
    auto& typed = static_cast<td::td_api::addMessageReaction&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reaction_type_, observer);
    wipe_native_value(typed.is_big_, observer);
    wipe_native_value(typed.update_recent_reactions_, observer);
    return;
}
case td::td_api::addNetworkStatistics::ID: {
    auto& typed = static_cast<td::td_api::addNetworkStatistics&>(value);
    wipe_native_value(typed.entry_, observer);
    return;
}
case td::td_api::addOffer::ID: {
    auto& typed = static_cast<td::td_api::addOffer&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.options_, observer);
    return;
}
case td::td_api::addPendingLiveStoryReaction::ID: {
    auto& typed = static_cast<td::td_api::addPendingLiveStoryReaction&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::addPendingPaidMessageReaction::ID: {
    auto& typed = static_cast<td::td_api::addPendingPaidMessageReaction&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::addPollOption::ID: {
    auto& typed = static_cast<td::td_api::addPollOption&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.option_, observer);
    return;
}
case td::td_api::addProfileAudio::ID: {
    auto& typed = static_cast<td::td_api::addProfileAudio&>(value);
    wipe_native_value(typed.audio_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.performer_, observer);
    return;
}
case td::td_api::addProxy::ID: {
    auto& typed = static_cast<td::td_api::addProxy&>(value);
    wipe_native_value(typed.proxy_, observer);
    wipe_native_value(typed.enable_, observer);
    wipe_native_value(typed.comment_, observer);
    return;
}
case td::td_api::addQuickReplyShortcutInlineQueryResultMessage::ID: {
    auto& typed = static_cast<td::td_api::addQuickReplyShortcutInlineQueryResultMessage&>(value);
    wipe_native_value(typed.shortcut_name_, observer);
    wipe_native_value(typed.reply_to_message_id_, observer);
    wipe_native_value(typed.query_id_, observer);
    wipe_native_value(typed.result_id_, observer);
    wipe_native_value(typed.hide_via_bot_, observer);
    return;
}
case td::td_api::addQuickReplyShortcutMessage::ID: {
    auto& typed = static_cast<td::td_api::addQuickReplyShortcutMessage&>(value);
    wipe_native_value(typed.shortcut_name_, observer);
    wipe_native_value(typed.reply_to_message_id_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::addQuickReplyShortcutMessageAlbum::ID: {
    auto& typed = static_cast<td::td_api::addQuickReplyShortcutMessageAlbum&>(value);
    wipe_native_value(typed.shortcut_name_, observer);
    wipe_native_value(typed.reply_to_message_id_, observer);
    wipe_native_value(typed.input_message_contents_, observer);
    return;
}
case td::td_api::addRecentSticker::ID: {
    auto& typed = static_cast<td::td_api::addRecentSticker&>(value);
    wipe_native_value(typed.is_attached_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::addRecentlyFoundChat::ID: {
    auto& typed = static_cast<td::td_api::addRecentlyFoundChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::addSavedAnimation::ID: {
    auto& typed = static_cast<td::td_api::addSavedAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::addSavedNotificationSound::ID: {
    auto& typed = static_cast<td::td_api::addSavedNotificationSound&>(value);
    wipe_native_value(typed.sound_, observer);
    return;
}
case td::td_api::addStickerToSet::ID: {
    auto& typed = static_cast<td::td_api::addStickerToSet&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::addStoryAlbumStories::ID: {
    auto& typed = static_cast<td::td_api::addStoryAlbumStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    wipe_native_value(typed.story_ids_, observer);
    return;
}
case td::td_api::addTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::addTextCompositionStyle&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::addWebBrowserSettingsException::ID: {
    auto& typed = static_cast<td::td_api::addWebBrowserSettingsException&>(value);
    wipe_native_value(typed.open_external_browser_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::allowBotToSendMessages::ID: {
    auto& typed = static_cast<td::td_api::allowBotToSendMessages&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::allowUnpaidMessagesFromUser::ID: {
    auto& typed = static_cast<td::td_api::allowUnpaidMessagesFromUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.refund_payments_, observer);
    return;
}
case td::td_api::answerCallbackQuery::ID: {
    auto& typed = static_cast<td::td_api::answerCallbackQuery&>(value);
    wipe_native_value(typed.callback_query_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.show_alert_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.cache_time_, observer);
    return;
}
case td::td_api::answerChatJoinRequestQuery::ID: {
    auto& typed = static_cast<td::td_api::answerChatJoinRequestQuery&>(value);
    wipe_native_value(typed.query_id_, observer);
    wipe_native_value(typed.result_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::answerCustomQuery::ID: {
    auto& typed = static_cast<td::td_api::answerCustomQuery&>(value);
    wipe_native_value(typed.custom_query_id_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::answerGuestQuery::ID: {
    auto& typed = static_cast<td::td_api::answerGuestQuery&>(value);
    wipe_native_value(typed.guest_query_id_, observer);
    wipe_native_value(typed.result_, observer);
    return;
}
case td::td_api::answerInlineQuery::ID: {
    auto& typed = static_cast<td::td_api::answerInlineQuery&>(value);
    wipe_native_value(typed.inline_query_id_, observer);
    wipe_native_value(typed.is_personal_, observer);
    wipe_native_value(typed.button_, observer);
    wipe_native_value(typed.results_, observer);
    wipe_native_value(typed.cache_time_, observer);
    wipe_native_value(typed.next_offset_, observer);
    return;
}
case td::td_api::answerPreCheckoutQuery::ID: {
    auto& typed = static_cast<td::td_api::answerPreCheckoutQuery&>(value);
    wipe_native_value(typed.pre_checkout_query_id_, observer);
    wipe_native_value(typed.error_message_, observer);
    return;
}
case td::td_api::answerShippingQuery::ID: {
    auto& typed = static_cast<td::td_api::answerShippingQuery&>(value);
    wipe_native_value(typed.shipping_query_id_, observer);
    wipe_native_value(typed.shipping_options_, observer);
    wipe_native_value(typed.error_message_, observer);
    return;
}
case td::td_api::answerWebAppQuery::ID: {
    auto& typed = static_cast<td::td_api::answerWebAppQuery&>(value);
    wipe_native_value(typed.web_app_query_id_, observer);
    wipe_native_value(typed.result_, observer);
    return;
}
case td::td_api::applyPremiumGiftCode::ID: {
    auto& typed = static_cast<td::td_api::applyPremiumGiftCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::approveSuggestedPost::ID: {
    auto& typed = static_cast<td::td_api::approveSuggestedPost&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.send_date_, observer);
    return;
}
case td::td_api::assignStoreTransaction::ID: {
    auto& typed = static_cast<td::td_api::assignStoreTransaction&>(value);
    wipe_native_value(typed.transaction_, observer);
    wipe_native_value(typed.purpose_, observer);
    return;
}
case td::td_api::banChatMember::ID: {
    auto& typed = static_cast<td::td_api::banChatMember&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.member_id_, observer);
    wipe_native_value(typed.banned_until_date_, observer);
    wipe_native_value(typed.revoke_messages_, observer);
    return;
}
case td::td_api::banGroupCallParticipants::ID: {
    auto& typed = static_cast<td::td_api::banGroupCallParticipants&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::blockMessageSenderFromReplies::ID: {
    auto& typed = static_cast<td::td_api::blockMessageSenderFromReplies&>(value);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.delete_message_, observer);
    wipe_native_value(typed.delete_all_messages_, observer);
    wipe_native_value(typed.report_spam_, observer);
    return;
}
case td::td_api::boostChat::ID: {
    auto& typed = static_cast<td::td_api::boostChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.slot_ids_, observer);
    return;
}
case td::td_api::buyGiftUpgrade::ID: {
    auto& typed = static_cast<td::td_api::buyGiftUpgrade&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.prepaid_upgrade_hash_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::canBotSendMessages::ID: {
    auto& typed = static_cast<td::td_api::canBotSendMessages&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::canPostStory::ID: {
    auto& typed = static_cast<td::td_api::canPostStory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::canPurchaseFromStore::ID: {
    auto& typed = static_cast<td::td_api::canPurchaseFromStore&>(value);
    wipe_native_value(typed.purpose_, observer);
    return;
}
case td::td_api::canSendGift::ID: {
    auto& typed = static_cast<td::td_api::canSendGift&>(value);
    wipe_native_value(typed.gift_id_, observer);
    return;
}
case td::td_api::canSendMessageToUser::ID: {
    auto& typed = static_cast<td::td_api::canSendMessageToUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.only_local_, observer);
    return;
}
case td::td_api::canTransferOwnership::ID: {
    return;
}
case td::td_api::cancelDownloadFile::ID: {
    auto& typed = static_cast<td::td_api::cancelDownloadFile&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.only_if_pending_, observer);
    return;
}
case td::td_api::cancelPasswordReset::ID: {
    return;
}
case td::td_api::cancelPreliminaryUploadFile::ID: {
    auto& typed = static_cast<td::td_api::cancelPreliminaryUploadFile&>(value);
    wipe_native_value(typed.file_id_, observer);
    return;
}
case td::td_api::cancelRecoveryEmailAddressVerification::ID: {
    return;
}
case td::td_api::changeImportedContacts::ID: {
    auto& typed = static_cast<td::td_api::changeImportedContacts&>(value);
    wipe_native_value(typed.contacts_, observer);
    return;
}
case td::td_api::changeStickerSet::ID: {
    auto& typed = static_cast<td::td_api::changeStickerSet&>(value);
    wipe_native_value(typed.set_id_, observer);
    wipe_native_value(typed.is_installed_, observer);
    wipe_native_value(typed.is_archived_, observer);
    return;
}
case td::td_api::changeWebBrowserSettings::ID: {
    auto& typed = static_cast<td::td_api::changeWebBrowserSettings&>(value);
    wipe_native_value(typed.open_external_browser_, observer);
    wipe_native_value(typed.display_close_button_, observer);
    return;
}
case td::td_api::checkAuthenticationBotToken::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationBotToken&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::checkAuthenticationCode::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkAuthenticationEmailCode::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationEmailCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkAuthenticationPasskey::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationPasskey&>(value);
    wipe_native_value(typed.credential_id_, observer);
    wipe_native_value(typed.client_data_, observer);
    wipe_native_value(typed.authenticator_data_, observer);
    wipe_native_value(typed.signature_, observer);
    wipe_native_value(typed.user_handle_, observer);
    return;
}
case td::td_api::checkAuthenticationPassword::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationPassword&>(value);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::checkAuthenticationPasswordRecoveryCode::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationPasswordRecoveryCode&>(value);
    wipe_native_value(typed.recovery_code_, observer);
    return;
}
case td::td_api::checkAuthenticationPremiumPurchase::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationPremiumPurchase&>(value);
    wipe_native_value(typed.premium_day_count_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    return;
}
case td::td_api::checkAuthenticationWebToken::ID: {
    auto& typed = static_cast<td::td_api::checkAuthenticationWebToken&>(value);
    wipe_native_value(typed.token_, observer);
    wipe_native_value(typed.dc_id_, observer);
    return;
}
case td::td_api::checkBotUsername::ID: {
    auto& typed = static_cast<td::td_api::checkBotUsername&>(value);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::checkChatFolderInviteLink::ID: {
    auto& typed = static_cast<td::td_api::checkChatFolderInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::checkChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::checkChatInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::checkChatUsername::ID: {
    auto& typed = static_cast<td::td_api::checkChatUsername&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::checkCreatedPublicChatsLimit::ID: {
    auto& typed = static_cast<td::td_api::checkCreatedPublicChatsLimit&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::checkEmailAddressVerificationCode::ID: {
    auto& typed = static_cast<td::td_api::checkEmailAddressVerificationCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkLoginEmailAddressCode::ID: {
    auto& typed = static_cast<td::td_api::checkLoginEmailAddressCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkOauthRequestMatchCode::ID: {
    auto& typed = static_cast<td::td_api::checkOauthRequestMatchCode&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.match_code_, observer);
    return;
}
case td::td_api::checkPasswordRecoveryCode::ID: {
    auto& typed = static_cast<td::td_api::checkPasswordRecoveryCode&>(value);
    wipe_native_value(typed.recovery_code_, observer);
    return;
}
case td::td_api::checkPhoneNumberCode::ID: {
    auto& typed = static_cast<td::td_api::checkPhoneNumberCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkPremiumGiftCode::ID: {
    auto& typed = static_cast<td::td_api::checkPremiumGiftCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkQuickReplyShortcutName::ID: {
    auto& typed = static_cast<td::td_api::checkQuickReplyShortcutName&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::checkRecoveryEmailAddressCode::ID: {
    auto& typed = static_cast<td::td_api::checkRecoveryEmailAddressCode&>(value);
    wipe_native_value(typed.code_, observer);
    return;
}
case td::td_api::checkStickerSetName::ID: {
    auto& typed = static_cast<td::td_api::checkStickerSetName&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::checkWebAppFileDownload::ID: {
    auto& typed = static_cast<td::td_api::checkWebAppFileDownload&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.file_name_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::cleanFileName::ID: {
    auto& typed = static_cast<td::td_api::cleanFileName&>(value);
    wipe_native_value(typed.file_name_, observer);
    return;
}
case td::td_api::clearAllDraftMessages::ID: {
    auto& typed = static_cast<td::td_api::clearAllDraftMessages&>(value);
    wipe_native_value(typed.exclude_secret_chats_, observer);
    return;
}
case td::td_api::clearAutosaveSettingsExceptions::ID: {
    return;
}
case td::td_api::clearImportedContacts::ID: {
    return;
}
case td::td_api::clearRecentEmojiStatuses::ID: {
    return;
}
case td::td_api::clearRecentReactions::ID: {
    return;
}
case td::td_api::clearRecentStickers::ID: {
    auto& typed = static_cast<td::td_api::clearRecentStickers&>(value);
    wipe_native_value(typed.is_attached_, observer);
    return;
}
case td::td_api::clearRecentlyFoundChats::ID: {
    return;
}
case td::td_api::clearSearchedForTags::ID: {
    auto& typed = static_cast<td::td_api::clearSearchedForTags&>(value);
    wipe_native_value(typed.clear_cashtags_, observer);
    return;
}
case td::td_api::clickAnimatedEmojiMessage::ID: {
    auto& typed = static_cast<td::td_api::clickAnimatedEmojiMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::clickChatSponsoredMessage::ID: {
    auto& typed = static_cast<td::td_api::clickChatSponsoredMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_media_click_, observer);
    wipe_native_value(typed.from_fullscreen_, observer);
    return;
}
case td::td_api::clickPremiumSubscriptionButton::ID: {
    return;
}
case td::td_api::clickVideoMessageAdvertisement::ID: {
    auto& typed = static_cast<td::td_api::clickVideoMessageAdvertisement&>(value);
    wipe_native_value(typed.advertisement_unique_id_, observer);
    return;
}
case td::td_api::close::ID: {
    return;
}
case td::td_api::closeChat::ID: {
    auto& typed = static_cast<td::td_api::closeChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::closeGiftAuction::ID: {
    auto& typed = static_cast<td::td_api::closeGiftAuction&>(value);
    wipe_native_value(typed.gift_id_, observer);
    return;
}
case td::td_api::closeSecretChat::ID: {
    auto& typed = static_cast<td::td_api::closeSecretChat&>(value);
    wipe_native_value(typed.secret_chat_id_, observer);
    return;
}
case td::td_api::closeStory::ID: {
    auto& typed = static_cast<td::td_api::closeStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::closeWebApp::ID: {
    auto& typed = static_cast<td::td_api::closeWebApp&>(value);
    wipe_native_value(typed.web_app_launch_id_, observer);
    return;
}
case td::td_api::commitPendingLiveStoryReactions::ID: {
    auto& typed = static_cast<td::td_api::commitPendingLiveStoryReactions&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::commitPendingPaidMessageReactions::ID: {
    auto& typed = static_cast<td::td_api::commitPendingPaidMessageReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::composeTextWithAi::ID: {
    auto& typed = static_cast<td::td_api::composeTextWithAi&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.translate_to_language_code_, observer);
    wipe_native_value(typed.style_name_, observer);
    wipe_native_value(typed.add_emojis_, observer);
    return;
}
case td::td_api::confirmBusinessConnectedBot::ID: {
    auto& typed = static_cast<td::td_api::confirmBusinessConnectedBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::confirmQrCodeAuthentication::ID: {
    auto& typed = static_cast<td::td_api::confirmQrCodeAuthentication&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::confirmSession::ID: {
    auto& typed = static_cast<td::td_api::confirmSession&>(value);
    wipe_native_value(typed.session_id_, observer);
    return;
}
case td::td_api::connectAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::connectAffiliateProgram&>(value);
    wipe_native_value(typed.affiliate_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::craftGift::ID: {
    auto& typed = static_cast<td::td_api::craftGift&>(value);
    wipe_native_value(typed.received_gift_ids_, observer);
    return;
}
case td::td_api::createBasicGroupChat::ID: {
    auto& typed = static_cast<td::td_api::createBasicGroupChat&>(value);
    wipe_native_value(typed.basic_group_id_, observer);
    wipe_native_value(typed.force_, observer);
    return;
}
case td::td_api::createBot::ID: {
    auto& typed = static_cast<td::td_api::createBot&>(value);
    wipe_native_value(typed.manager_bot_user_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.via_link_, observer);
    return;
}
case td::td_api::createBusinessChatLink::ID: {
    auto& typed = static_cast<td::td_api::createBusinessChatLink&>(value);
    wipe_native_value(typed.link_info_, observer);
    return;
}
case td::td_api::createCall::ID: {
    auto& typed = static_cast<td::td_api::createCall&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.protocol_, observer);
    wipe_native_value(typed.is_video_, observer);
    return;
}
case td::td_api::createChatFolder::ID: {
    auto& typed = static_cast<td::td_api::createChatFolder&>(value);
    wipe_native_value(typed.folder_, observer);
    return;
}
case td::td_api::createChatFolderInviteLink::ID: {
    auto& typed = static_cast<td::td_api::createChatFolderInviteLink&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::createChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::createChatInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.member_limit_, observer);
    wipe_native_value(typed.creates_join_request_, observer);
    return;
}
case td::td_api::createChatSubscriptionInviteLink::ID: {
    auto& typed = static_cast<td::td_api::createChatSubscriptionInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.subscription_pricing_, observer);
    return;
}
case td::td_api::createForumTopic::ID: {
    auto& typed = static_cast<td::td_api::createForumTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.is_name_implicit_, observer);
    wipe_native_value(typed.icon_, observer);
    return;
}
case td::td_api::createGiftCollection::ID: {
    auto& typed = static_cast<td::td_api::createGiftCollection&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.received_gift_ids_, observer);
    return;
}
case td::td_api::createGroupCall::ID: {
    auto& typed = static_cast<td::td_api::createGroupCall&>(value);
    wipe_native_value(typed.join_parameters_, observer);
    return;
}
case td::td_api::createInvoiceLink::ID: {
    auto& typed = static_cast<td::td_api::createInvoiceLink&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.invoice_, observer);
    return;
}
case td::td_api::createNewBasicGroupChat::ID: {
    auto& typed = static_cast<td::td_api::createNewBasicGroupChat&>(value);
    wipe_native_value(typed.user_ids_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    return;
}
case td::td_api::createNewSecretChat::ID: {
    auto& typed = static_cast<td::td_api::createNewSecretChat&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::createNewStickerSet::ID: {
    auto& typed = static_cast<td::td_api::createNewStickerSet&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.needs_repainting_, observer);
    wipe_native_value(typed.stickers_, observer);
    wipe_native_value(typed.source_, observer);
    return;
}
case td::td_api::createNewSupergroupChat::ID: {
    auto& typed = static_cast<td::td_api::createNewSupergroupChat&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.is_forum_, observer);
    wipe_native_value(typed.is_channel_, observer);
    wipe_native_value(typed.description_, observer);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    wipe_native_value(typed.for_import_, observer);
    return;
}
case td::td_api::createPrivateChat::ID: {
    auto& typed = static_cast<td::td_api::createPrivateChat&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.force_, observer);
    return;
}
case td::td_api::createSecretChat::ID: {
    auto& typed = static_cast<td::td_api::createSecretChat&>(value);
    wipe_native_value(typed.secret_chat_id_, observer);
    return;
}
case td::td_api::createStoryAlbum::ID: {
    auto& typed = static_cast<td::td_api::createStoryAlbum&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.story_ids_, observer);
    return;
}
case td::td_api::createSupergroupChat::ID: {
    auto& typed = static_cast<td::td_api::createSupergroupChat&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.force_, observer);
    return;
}
case td::td_api::createTemporaryPassword::ID: {
    auto& typed = static_cast<td::td_api::createTemporaryPassword&>(value);
    wipe_native_value(typed.password_, observer);
    wipe_native_value(typed.valid_for_, observer);
    return;
}
case td::td_api::createTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::createTextCompositionStyle&>(value);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.custom_emoji_id_, observer);
    wipe_native_value(typed.prompt_, observer);
    wipe_native_value(typed.show_creator_, observer);
    return;
}
case td::td_api::createVideoChat::ID: {
    auto& typed = static_cast<td::td_api::createVideoChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.start_date_, observer);
    wipe_native_value(typed.is_rtmp_stream_, observer);
    return;
}
case td::td_api::declineGroupCallInvitation::ID: {
    auto& typed = static_cast<td::td_api::declineGroupCallInvitation&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::declineOauthRequest::ID: {
    auto& typed = static_cast<td::td_api::declineOauthRequest&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::declineSuggestedPost::ID: {
    auto& typed = static_cast<td::td_api::declineSuggestedPost&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.comment_, observer);
    return;
}
case td::td_api::decryptGroupCallData::ID: {
    auto& typed = static_cast<td::td_api::decryptGroupCallData&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.data_channel_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::deleteAccount::ID: {
    auto& typed = static_cast<td::td_api::deleteAccount&>(value);
    wipe_native_value(typed.reason_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::deleteAllCallMessages::ID: {
    auto& typed = static_cast<td::td_api::deleteAllCallMessages&>(value);
    wipe_native_value(typed.revoke_, observer);
    return;
}
case td::td_api::deleteAllRecentMessageReactionsFromSender::ID: {
    auto& typed = static_cast<td::td_api::deleteAllRecentMessageReactionsFromSender&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    return;
}
case td::td_api::deleteAllRevokedChatInviteLinks::ID: {
    auto& typed = static_cast<td::td_api::deleteAllRevokedChatInviteLinks&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.creator_user_id_, observer);
    return;
}
case td::td_api::deleteBotMediaPreviews::ID: {
    auto& typed = static_cast<td::td_api::deleteBotMediaPreviews&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.file_ids_, observer);
    return;
}
case td::td_api::deleteBusinessChatLink::ID: {
    auto& typed = static_cast<td::td_api::deleteBusinessChatLink&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::deleteBusinessConnectedBot::ID: {
    auto& typed = static_cast<td::td_api::deleteBusinessConnectedBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::deleteBusinessMessages::ID: {
    auto& typed = static_cast<td::td_api::deleteBusinessMessages&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::deleteBusinessStory::ID: {
    auto& typed = static_cast<td::td_api::deleteBusinessStory&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::deleteChat::ID: {
    auto& typed = static_cast<td::td_api::deleteChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::deleteChatBackground::ID: {
    auto& typed = static_cast<td::td_api::deleteChatBackground&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.restore_previous_, observer);
    return;
}
case td::td_api::deleteChatFolder::ID: {
    auto& typed = static_cast<td::td_api::deleteChatFolder&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    wipe_native_value(typed.leave_chat_ids_, observer);
    return;
}
case td::td_api::deleteChatFolderInviteLink::ID: {
    auto& typed = static_cast<td::td_api::deleteChatFolderInviteLink&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::deleteChatHistory::ID: {
    auto& typed = static_cast<td::td_api::deleteChatHistory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.remove_from_chat_list_, observer);
    wipe_native_value(typed.revoke_, observer);
    return;
}
case td::td_api::deleteChatMessagesByDate::ID: {
    auto& typed = static_cast<td::td_api::deleteChatMessagesByDate&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.min_date_, observer);
    wipe_native_value(typed.max_date_, observer);
    wipe_native_value(typed.revoke_, observer);
    return;
}
case td::td_api::deleteChatMessagesBySender::ID: {
    auto& typed = static_cast<td::td_api::deleteChatMessagesBySender&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    return;
}
case td::td_api::deleteChatReplyMarkup::ID: {
    auto& typed = static_cast<td::td_api::deleteChatReplyMarkup&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::deleteCommands::ID: {
    auto& typed = static_cast<td::td_api::deleteCommands&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::deleteDefaultBackground::ID: {
    auto& typed = static_cast<td::td_api::deleteDefaultBackground&>(value);
    wipe_native_value(typed.for_dark_theme_, observer);
    return;
}
case td::td_api::deleteDirectMessagesChatTopicHistory::ID: {
    auto& typed = static_cast<td::td_api::deleteDirectMessagesChatTopicHistory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::deleteDirectMessagesChatTopicMessagesByDate::ID: {
    auto& typed = static_cast<td::td_api::deleteDirectMessagesChatTopicMessagesByDate&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.min_date_, observer);
    wipe_native_value(typed.max_date_, observer);
    return;
}
case td::td_api::deleteFile::ID: {
    auto& typed = static_cast<td::td_api::deleteFile&>(value);
    wipe_native_value(typed.file_id_, observer);
    return;
}
case td::td_api::deleteForumTopic::ID: {
    auto& typed = static_cast<td::td_api::deleteForumTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::deleteGiftCollection::ID: {
    auto& typed = static_cast<td::td_api::deleteGiftCollection&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_id_, observer);
    return;
}
case td::td_api::deleteGroupCallMessages::ID: {
    auto& typed = static_cast<td::td_api::deleteGroupCallMessages&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.report_spam_, observer);
    return;
}
case td::td_api::deleteGroupCallMessagesBySender::ID: {
    auto& typed = static_cast<td::td_api::deleteGroupCallMessagesBySender&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.report_spam_, observer);
    return;
}
case td::td_api::deleteLanguagePack::ID: {
    auto& typed = static_cast<td::td_api::deleteLanguagePack&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    return;
}
case td::td_api::deleteMessageReactionsFromSender::ID: {
    auto& typed = static_cast<td::td_api::deleteMessageReactionsFromSender&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    return;
}
case td::td_api::deleteMessages::ID: {
    auto& typed = static_cast<td::td_api::deleteMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.revoke_, observer);
    return;
}
case td::td_api::deletePassportElement::ID: {
    auto& typed = static_cast<td::td_api::deletePassportElement&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::deletePollOption::ID: {
    auto& typed = static_cast<td::td_api::deletePollOption&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    return;
}
case td::td_api::deleteProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::deleteProfilePhoto&>(value);
    wipe_native_value(typed.profile_photo_id_, observer);
    return;
}
case td::td_api::deleteQuickReplyShortcut::ID: {
    auto& typed = static_cast<td::td_api::deleteQuickReplyShortcut&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    return;
}
case td::td_api::deleteQuickReplyShortcutMessages::ID: {
    auto& typed = static_cast<td::td_api::deleteQuickReplyShortcutMessages&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::deleteRevokedChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::deleteRevokedChatInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::deleteSavedCredentials::ID: {
    return;
}
case td::td_api::deleteSavedMessagesTopicHistory::ID: {
    auto& typed = static_cast<td::td_api::deleteSavedMessagesTopicHistory&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    return;
}
case td::td_api::deleteSavedMessagesTopicMessagesByDate::ID: {
    auto& typed = static_cast<td::td_api::deleteSavedMessagesTopicMessagesByDate&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    wipe_native_value(typed.min_date_, observer);
    wipe_native_value(typed.max_date_, observer);
    return;
}
case td::td_api::deleteSavedOrderInfo::ID: {
    return;
}
case td::td_api::deleteStickerSet::ID: {
    auto& typed = static_cast<td::td_api::deleteStickerSet&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::deleteStory::ID: {
    auto& typed = static_cast<td::td_api::deleteStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::deleteStoryAlbum::ID: {
    auto& typed = static_cast<td::td_api::deleteStoryAlbum&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    return;
}
case td::td_api::deleteTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::deleteTextCompositionStyle&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::destroy::ID: {
    return;
}
case td::td_api::disableAllSupergroupUsernames::ID: {
    auto& typed = static_cast<td::td_api::disableAllSupergroupUsernames&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::disableProxy::ID: {
    return;
}
case td::td_api::discardCall::ID: {
    auto& typed = static_cast<td::td_api::discardCall&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.is_disconnected_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.is_video_, observer);
    wipe_native_value(typed.connection_id_, observer);
    return;
}
case td::td_api::disconnectAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::disconnectAffiliateProgram&>(value);
    wipe_native_value(typed.affiliate_, observer);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::disconnectAllWebsites::ID: {
    return;
}
case td::td_api::disconnectWebsite::ID: {
    auto& typed = static_cast<td::td_api::disconnectWebsite&>(value);
    wipe_native_value(typed.website_id_, observer);
    return;
}
case td::td_api::downloadFile::ID: {
    auto& typed = static_cast<td::td_api::downloadFile&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.priority_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.synchronous_, observer);
    return;
}
case td::td_api::dropGiftOriginalDetails::ID: {
    auto& typed = static_cast<td::td_api::dropGiftOriginalDetails&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::editBotMediaPreview::ID: {
    auto& typed = static_cast<td::td_api::editBotMediaPreview&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.content_, observer);
    return;
}
case td::td_api::editBusinessChatLink::ID: {
    auto& typed = static_cast<td::td_api::editBusinessChatLink&>(value);
    wipe_native_value(typed.link_, observer);
    wipe_native_value(typed.link_info_, observer);
    return;
}
case td::td_api::editBusinessMessageCaption::ID: {
    auto& typed = static_cast<td::td_api::editBusinessMessageCaption&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    return;
}
case td::td_api::editBusinessMessageChecklist::ID: {
    auto& typed = static_cast<td::td_api::editBusinessMessageChecklist&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.checklist_, observer);
    return;
}
case td::td_api::editBusinessMessageLiveLocation::ID: {
    auto& typed = static_cast<td::td_api::editBusinessMessageLiveLocation&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::editBusinessMessageMedia::ID: {
    auto& typed = static_cast<td::td_api::editBusinessMessageMedia&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editBusinessMessageReplyMarkup::ID: {
    auto& typed = static_cast<td::td_api::editBusinessMessageReplyMarkup&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::editBusinessMessageText::ID: {
    auto& typed = static_cast<td::td_api::editBusinessMessageText&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editBusinessStory::ID: {
    auto& typed = static_cast<td::td_api::editBusinessStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.areas_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.privacy_settings_, observer);
    return;
}
case td::td_api::editChatFolder::ID: {
    auto& typed = static_cast<td::td_api::editChatFolder&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    wipe_native_value(typed.folder_, observer);
    return;
}
case td::td_api::editChatFolderInviteLink::ID: {
    auto& typed = static_cast<td::td_api::editChatFolderInviteLink&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::editChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::editChatInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.expiration_date_, observer);
    wipe_native_value(typed.member_limit_, observer);
    wipe_native_value(typed.creates_join_request_, observer);
    return;
}
case td::td_api::editChatSubscriptionInviteLink::ID: {
    auto& typed = static_cast<td::td_api::editChatSubscriptionInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::editCustomLanguagePackInfo::ID: {
    auto& typed = static_cast<td::td_api::editCustomLanguagePackInfo&>(value);
    wipe_native_value(typed.info_, observer);
    return;
}
case td::td_api::editForumTopic::ID: {
    auto& typed = static_cast<td::td_api::editForumTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.edit_icon_custom_emoji_, observer);
    wipe_native_value(typed.icon_custom_emoji_id_, observer);
    return;
}
case td::td_api::editInlineMessageCaption::ID: {
    auto& typed = static_cast<td::td_api::editInlineMessageCaption&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    return;
}
case td::td_api::editInlineMessageLiveLocation::ID: {
    auto& typed = static_cast<td::td_api::editInlineMessageLiveLocation&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::editInlineMessageMedia::ID: {
    auto& typed = static_cast<td::td_api::editInlineMessageMedia&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editInlineMessageReplyMarkup::ID: {
    auto& typed = static_cast<td::td_api::editInlineMessageReplyMarkup&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::editInlineMessageText::ID: {
    auto& typed = static_cast<td::td_api::editInlineMessageText&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editMessageCaption::ID: {
    auto& typed = static_cast<td::td_api::editMessageCaption&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.show_caption_above_media_, observer);
    return;
}
case td::td_api::editMessageChecklist::ID: {
    auto& typed = static_cast<td::td_api::editMessageChecklist&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.checklist_, observer);
    return;
}
case td::td_api::editMessageLiveLocation::ID: {
    auto& typed = static_cast<td::td_api::editMessageLiveLocation&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::editMessageMedia::ID: {
    auto& typed = static_cast<td::td_api::editMessageMedia&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editMessageReplyMarkup::ID: {
    auto& typed = static_cast<td::td_api::editMessageReplyMarkup&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::editMessageSchedulingState::ID: {
    auto& typed = static_cast<td::td_api::editMessageSchedulingState&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.scheduling_state_, observer);
    return;
}
case td::td_api::editMessageText::ID: {
    auto& typed = static_cast<td::td_api::editMessageText&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editProxy::ID: {
    auto& typed = static_cast<td::td_api::editProxy&>(value);
    wipe_native_value(typed.proxy_id_, observer);
    wipe_native_value(typed.proxy_, observer);
    wipe_native_value(typed.enable_, observer);
    wipe_native_value(typed.comment_, observer);
    return;
}
case td::td_api::editQuickReplyMessage::ID: {
    auto& typed = static_cast<td::td_api::editQuickReplyMessage&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::editStarSubscription::ID: {
    auto& typed = static_cast<td::td_api::editStarSubscription&>(value);
    wipe_native_value(typed.subscription_id_, observer);
    wipe_native_value(typed.is_canceled_, observer);
    return;
}
case td::td_api::editStory::ID: {
    auto& typed = static_cast<td::td_api::editStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.areas_, observer);
    wipe_native_value(typed.caption_, observer);
    return;
}
case td::td_api::editStoryCover::ID: {
    auto& typed = static_cast<td::td_api::editStoryCover&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.cover_frame_timestamp_, observer);
    return;
}
case td::td_api::editTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::editTextCompositionStyle&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.custom_emoji_id_, observer);
    wipe_native_value(typed.prompt_, observer);
    wipe_native_value(typed.show_creator_, observer);
    return;
}
case td::td_api::editUserStarSubscription::ID: {
    auto& typed = static_cast<td::td_api::editUserStarSubscription&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.telegram_payment_charge_id_, observer);
    wipe_native_value(typed.is_canceled_, observer);
    return;
}
case td::td_api::enableProxy::ID: {
    auto& typed = static_cast<td::td_api::enableProxy&>(value);
    wipe_native_value(typed.proxy_id_, observer);
    return;
}
case td::td_api::encryptGroupCallData::ID: {
    auto& typed = static_cast<td::td_api::encryptGroupCallData&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.data_channel_, observer);
    wipe_native_value(typed.data_, observer);
    wipe_native_value(typed.unencrypted_prefix_size_, observer);
    return;
}
case td::td_api::endGroupCall::ID: {
    auto& typed = static_cast<td::td_api::endGroupCall&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::endGroupCallRecording::ID: {
    auto& typed = static_cast<td::td_api::endGroupCallRecording&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::endGroupCallScreenSharing::ID: {
    auto& typed = static_cast<td::td_api::endGroupCallScreenSharing&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::finishFileGeneration::ID: {
    auto& typed = static_cast<td::td_api::finishFileGeneration&>(value);
    wipe_native_value(typed.generation_id_, observer);
    wipe_native_value(typed.error_, observer);
    return;
}
case td::td_api::fixTextWithAi::ID: {
    auto& typed = static_cast<td::td_api::fixTextWithAi&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::forwardMessages::ID: {
    auto& typed = static_cast<td::td_api::forwardMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.from_chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.options_, observer);
    wipe_native_value(typed.send_copy_, observer);
    wipe_native_value(typed.remove_caption_, observer);
    return;
}
case td::td_api::getAccountTtl::ID: {
    return;
}
case td::td_api::getActiveSessions::ID: {
    return;
}
case td::td_api::getAllPassportElements::ID: {
    auto& typed = static_cast<td::td_api::getAllPassportElements&>(value);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getAllStickerEmojis::ID: {
    auto& typed = static_cast<td::td_api::getAllStickerEmojis&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.return_only_main_emoji_, observer);
    return;
}
case td::td_api::getAnimatedEmoji::ID: {
    auto& typed = static_cast<td::td_api::getAnimatedEmoji&>(value);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::getApplicationConfig::ID: {
    return;
}
case td::td_api::getApplicationDownloadLink::ID: {
    return;
}
case td::td_api::getArchiveChatListSettings::ID: {
    return;
}
case td::td_api::getArchivedStickerSets::ID: {
    auto& typed = static_cast<td::td_api::getArchivedStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.offset_sticker_set_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getAttachedStickerSets::ID: {
    auto& typed = static_cast<td::td_api::getAttachedStickerSets&>(value);
    wipe_native_value(typed.file_id_, observer);
    return;
}
case td::td_api::getAttachmentMenuBot::ID: {
    auto& typed = static_cast<td::td_api::getAttachmentMenuBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::getAuthenticationPasskeyParameters::ID: {
    return;
}
case td::td_api::getAuthorizationState::ID: {
    return;
}
case td::td_api::getAutoDownloadSettingsPresets::ID: {
    return;
}
case td::td_api::getAutosaveSettings::ID: {
    return;
}
case td::td_api::getAvailableChatBoostSlots::ID: {
    return;
}
case td::td_api::getAvailableGifts::ID: {
    return;
}
case td::td_api::getBackgroundUrl::ID: {
    auto& typed = static_cast<td::td_api::getBackgroundUrl&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::getBankCardInfo::ID: {
    auto& typed = static_cast<td::td_api::getBankCardInfo&>(value);
    wipe_native_value(typed.bank_card_number_, observer);
    return;
}
case td::td_api::getBasicGroup::ID: {
    auto& typed = static_cast<td::td_api::getBasicGroup&>(value);
    wipe_native_value(typed.basic_group_id_, observer);
    return;
}
case td::td_api::getBasicGroupFullInfo::ID: {
    auto& typed = static_cast<td::td_api::getBasicGroupFullInfo&>(value);
    wipe_native_value(typed.basic_group_id_, observer);
    return;
}
case td::td_api::getBlockedMessageSenders::ID: {
    auto& typed = static_cast<td::td_api::getBlockedMessageSenders&>(value);
    wipe_native_value(typed.block_list_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getBotInfoDescription::ID: {
    auto& typed = static_cast<td::td_api::getBotInfoDescription&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::getBotInfoShortDescription::ID: {
    auto& typed = static_cast<td::td_api::getBotInfoShortDescription&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::getBotMediaPreviewInfo::ID: {
    auto& typed = static_cast<td::td_api::getBotMediaPreviewInfo&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::getBotMediaPreviews::ID: {
    auto& typed = static_cast<td::td_api::getBotMediaPreviews&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::getBotName::ID: {
    auto& typed = static_cast<td::td_api::getBotName&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::getBotSimilarBotCount::ID: {
    auto& typed = static_cast<td::td_api::getBotSimilarBotCount&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.return_local_, observer);
    return;
}
case td::td_api::getBotSimilarBots::ID: {
    auto& typed = static_cast<td::td_api::getBotSimilarBots&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::getBusinessAccountStarAmount::ID: {
    auto& typed = static_cast<td::td_api::getBusinessAccountStarAmount&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    return;
}
case td::td_api::getBusinessChatLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::getBusinessChatLinkInfo&>(value);
    wipe_native_value(typed.link_name_, observer);
    return;
}
case td::td_api::getBusinessChatLinks::ID: {
    return;
}
case td::td_api::getBusinessConnectedBot::ID: {
    return;
}
case td::td_api::getBusinessConnection::ID: {
    auto& typed = static_cast<td::td_api::getBusinessConnection&>(value);
    wipe_native_value(typed.connection_id_, observer);
    return;
}
case td::td_api::getBusinessFeatures::ID: {
    auto& typed = static_cast<td::td_api::getBusinessFeatures&>(value);
    wipe_native_value(typed.source_, observer);
    return;
}
case td::td_api::getCallbackQueryAnswer::ID: {
    auto& typed = static_cast<td::td_api::getCallbackQueryAnswer&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::getCallbackQueryMessage::ID: {
    auto& typed = static_cast<td::td_api::getCallbackQueryMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.callback_query_id_, observer);
    return;
}
case td::td_api::getChat::ID: {
    auto& typed = static_cast<td::td_api::getChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatActiveStories::ID: {
    auto& typed = static_cast<td::td_api::getChatActiveStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatAdministrators::ID: {
    auto& typed = static_cast<td::td_api::getChatAdministrators&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatArchivedStories::ID: {
    auto& typed = static_cast<td::td_api::getChatArchivedStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.from_story_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatAvailableMessageSenders::ID: {
    auto& typed = static_cast<td::td_api::getChatAvailableMessageSenders&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatAvailablePaidMessageReactionSenders::ID: {
    auto& typed = static_cast<td::td_api::getChatAvailablePaidMessageReactionSenders&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatBoostFeatures::ID: {
    auto& typed = static_cast<td::td_api::getChatBoostFeatures&>(value);
    wipe_native_value(typed.is_channel_, observer);
    return;
}
case td::td_api::getChatBoostLevelFeatures::ID: {
    auto& typed = static_cast<td::td_api::getChatBoostLevelFeatures&>(value);
    wipe_native_value(typed.is_channel_, observer);
    wipe_native_value(typed.level_, observer);
    return;
}
case td::td_api::getChatBoostLink::ID: {
    auto& typed = static_cast<td::td_api::getChatBoostLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatBoostLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::getChatBoostLinkInfo&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::getChatBoostStatus::ID: {
    auto& typed = static_cast<td::td_api::getChatBoostStatus&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatBoosts::ID: {
    auto& typed = static_cast<td::td_api::getChatBoosts&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.only_gift_codes_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatEventLog::ID: {
    auto& typed = static_cast<td::td_api::getChatEventLog&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.from_event_id_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.filters_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::getChatFolder::ID: {
    auto& typed = static_cast<td::td_api::getChatFolder&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    return;
}
case td::td_api::getChatFolderChatCount::ID: {
    auto& typed = static_cast<td::td_api::getChatFolderChatCount&>(value);
    wipe_native_value(typed.folder_, observer);
    return;
}
case td::td_api::getChatFolderChatsToLeave::ID: {
    auto& typed = static_cast<td::td_api::getChatFolderChatsToLeave&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    return;
}
case td::td_api::getChatFolderDefaultIconName::ID: {
    auto& typed = static_cast<td::td_api::getChatFolderDefaultIconName&>(value);
    wipe_native_value(typed.folder_, observer);
    return;
}
case td::td_api::getChatFolderInviteLinks::ID: {
    auto& typed = static_cast<td::td_api::getChatFolderInviteLinks&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    return;
}
case td::td_api::getChatFolderNewChats::ID: {
    auto& typed = static_cast<td::td_api::getChatFolderNewChats&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    return;
}
case td::td_api::getChatHistory::ID: {
    auto& typed = static_cast<td::td_api::getChatHistory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.only_local_, observer);
    return;
}
case td::td_api::getChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::getChatInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::getChatInviteLinkCounts::ID: {
    auto& typed = static_cast<td::td_api::getChatInviteLinkCounts&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatInviteLinkMembers::ID: {
    auto& typed = static_cast<td::td_api::getChatInviteLinkMembers&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.only_with_expired_subscription_, observer);
    wipe_native_value(typed.offset_member_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatInviteLinks::ID: {
    auto& typed = static_cast<td::td_api::getChatInviteLinks&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.creator_user_id_, observer);
    wipe_native_value(typed.is_revoked_, observer);
    wipe_native_value(typed.offset_date_, observer);
    wipe_native_value(typed.offset_invite_link_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatJoinRequests::ID: {
    auto& typed = static_cast<td::td_api::getChatJoinRequests&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_request_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatListsToAddChat::ID: {
    auto& typed = static_cast<td::td_api::getChatListsToAddChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatMember::ID: {
    auto& typed = static_cast<td::td_api::getChatMember&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.member_id_, observer);
    return;
}
case td::td_api::getChatMessageByDate::ID: {
    auto& typed = static_cast<td::td_api::getChatMessageByDate&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::getChatMessageCalendar::ID: {
    auto& typed = static_cast<td::td_api::getChatMessageCalendar&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.filter_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    return;
}
case td::td_api::getChatMessageCount::ID: {
    auto& typed = static_cast<td::td_api::getChatMessageCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.filter_, observer);
    wipe_native_value(typed.return_local_, observer);
    return;
}
case td::td_api::getChatMessagePosition::ID: {
    auto& typed = static_cast<td::td_api::getChatMessagePosition&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.filter_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getChatNotificationSettingsExceptions::ID: {
    auto& typed = static_cast<td::td_api::getChatNotificationSettingsExceptions&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.compare_sound_, observer);
    return;
}
case td::td_api::getChatOwnerAfterLeaving::ID: {
    auto& typed = static_cast<td::td_api::getChatOwnerAfterLeaving&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatPinnedMessage::ID: {
    auto& typed = static_cast<td::td_api::getChatPinnedMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatPostedToChatPageStories::ID: {
    auto& typed = static_cast<td::td_api::getChatPostedToChatPageStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.from_story_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatRevenueStatistics::ID: {
    auto& typed = static_cast<td::td_api::getChatRevenueStatistics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getChatRevenueTransactions::ID: {
    auto& typed = static_cast<td::td_api::getChatRevenueTransactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatRevenueWithdrawalUrl::ID: {
    auto& typed = static_cast<td::td_api::getChatRevenueWithdrawalUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getChatScheduledMessages::ID: {
    auto& typed = static_cast<td::td_api::getChatScheduledMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatSimilarChatCount::ID: {
    auto& typed = static_cast<td::td_api::getChatSimilarChatCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.return_local_, observer);
    return;
}
case td::td_api::getChatSimilarChats::ID: {
    auto& typed = static_cast<td::td_api::getChatSimilarChats&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatSparseMessagePositions::ID: {
    auto& typed = static_cast<td::td_api::getChatSparseMessagePositions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.filter_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    return;
}
case td::td_api::getChatSponsoredMessages::ID: {
    auto& typed = static_cast<td::td_api::getChatSponsoredMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatStatistics::ID: {
    auto& typed = static_cast<td::td_api::getChatStatistics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getChatStoryAlbums::ID: {
    auto& typed = static_cast<td::td_api::getChatStoryAlbums&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getChatStoryInteractions::ID: {
    auto& typed = static_cast<td::td_api::getChatStoryInteractions&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.reaction_type_, observer);
    wipe_native_value(typed.prefer_forwards_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChats::ID: {
    auto& typed = static_cast<td::td_api::getChats&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getChatsForChatFolderInviteLink::ID: {
    auto& typed = static_cast<td::td_api::getChatsForChatFolderInviteLink&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    return;
}
case td::td_api::getChatsToPostStories::ID: {
    return;
}
case td::td_api::getCloseFriends::ID: {
    return;
}
case td::td_api::getCollectibleItemInfo::ID: {
    auto& typed = static_cast<td::td_api::getCollectibleItemInfo&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::getCommands::ID: {
    auto& typed = static_cast<td::td_api::getCommands&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::getConnectedAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::getConnectedAffiliateProgram&>(value);
    wipe_native_value(typed.affiliate_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::getConnectedAffiliatePrograms::ID: {
    auto& typed = static_cast<td::td_api::getConnectedAffiliatePrograms&>(value);
    wipe_native_value(typed.affiliate_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getConnectedWebsites::ID: {
    return;
}
case td::td_api::getContacts::ID: {
    return;
}
case td::td_api::getCountries::ID: {
    return;
}
case td::td_api::getCountry::ID: {
    auto& typed = static_cast<td::td_api::getCountry&>(value);
    wipe_native_value(typed.country_code_, observer);
    return;
}
case td::td_api::getCountryCode::ID: {
    return;
}
case td::td_api::getCountryFlagEmoji::ID: {
    auto& typed = static_cast<td::td_api::getCountryFlagEmoji&>(value);
    wipe_native_value(typed.country_code_, observer);
    return;
}
case td::td_api::getCreatedPublicChats::ID: {
    auto& typed = static_cast<td::td_api::getCreatedPublicChats&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::getCurrentState::ID: {
    return;
}
case td::td_api::getCurrentWeather::ID: {
    auto& typed = static_cast<td::td_api::getCurrentWeather&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::getCustomEmojiReactionAnimations::ID: {
    return;
}
case td::td_api::getCustomEmojiStickers::ID: {
    auto& typed = static_cast<td::td_api::getCustomEmojiStickers&>(value);
    wipe_native_value(typed.custom_emoji_ids_, observer);
    return;
}
case td::td_api::getDatabaseStatistics::ID: {
    return;
}
case td::td_api::getDeepLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::getDeepLinkInfo&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::getDefaultBackgroundCustomEmojiStickers::ID: {
    return;
}
case td::td_api::getDefaultChatEmojiStatuses::ID: {
    return;
}
case td::td_api::getDefaultChatPhotoCustomEmojiStickers::ID: {
    return;
}
case td::td_api::getDefaultEmojiStatuses::ID: {
    return;
}
case td::td_api::getDefaultMessageAutoDeleteTime::ID: {
    return;
}
case td::td_api::getDefaultProfilePhotoCustomEmojiStickers::ID: {
    return;
}
case td::td_api::getDirectMessagesChatTopic::ID: {
    auto& typed = static_cast<td::td_api::getDirectMessagesChatTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::getDirectMessagesChatTopicHistory::ID: {
    auto& typed = static_cast<td::td_api::getDirectMessagesChatTopicHistory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getDirectMessagesChatTopicMessageByDate::ID: {
    auto& typed = static_cast<td::td_api::getDirectMessagesChatTopicMessageByDate&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::getDirectMessagesChatTopicRevenue::ID: {
    auto& typed = static_cast<td::td_api::getDirectMessagesChatTopicRevenue&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::getDisallowedChatEmojiStatuses::ID: {
    return;
}
case td::td_api::getEmojiCategories::ID: {
    auto& typed = static_cast<td::td_api::getEmojiCategories&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::getEmojiReaction::ID: {
    auto& typed = static_cast<td::td_api::getEmojiReaction&>(value);
    wipe_native_value(typed.emoji_, observer);
    return;
}
case td::td_api::getEmojiSuggestionsUrl::ID: {
    auto& typed = static_cast<td::td_api::getEmojiSuggestionsUrl&>(value);
    wipe_native_value(typed.language_code_, observer);
    return;
}
case td::td_api::getExternalLink::ID: {
    auto& typed = static_cast<td::td_api::getExternalLink&>(value);
    wipe_native_value(typed.link_, observer);
    wipe_native_value(typed.allow_write_access_, observer);
    return;
}
case td::td_api::getExternalLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::getExternalLinkInfo&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::getFavoriteStickers::ID: {
    return;
}
case td::td_api::getFile::ID: {
    auto& typed = static_cast<td::td_api::getFile&>(value);
    wipe_native_value(typed.file_id_, observer);
    return;
}
case td::td_api::getFileDownloadedPrefixSize::ID: {
    auto& typed = static_cast<td::td_api::getFileDownloadedPrefixSize&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.offset_, observer);
    return;
}
case td::td_api::getFileExtension::ID: {
    auto& typed = static_cast<td::td_api::getFileExtension&>(value);
    wipe_native_value(typed.mime_type_, observer);
    return;
}
case td::td_api::getFileMimeType::ID: {
    auto& typed = static_cast<td::td_api::getFileMimeType&>(value);
    wipe_native_value(typed.file_name_, observer);
    return;
}
case td::td_api::getForumTopic::ID: {
    auto& typed = static_cast<td::td_api::getForumTopic&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::getForumTopicDefaultIcons::ID: {
    return;
}
case td::td_api::getForumTopicHistory::ID: {
    auto& typed = static_cast<td::td_api::getForumTopicHistory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getForumTopicLink::ID: {
    auto& typed = static_cast<td::td_api::getForumTopicLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::getForumTopics::ID: {
    auto& typed = static_cast<td::td_api::getForumTopics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_date_, observer);
    wipe_native_value(typed.offset_message_id_, observer);
    wipe_native_value(typed.offset_forum_topic_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getFullRichMessage::ID: {
    auto& typed = static_cast<td::td_api::getFullRichMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getGameHighScores::ID: {
    auto& typed = static_cast<td::td_api::getGameHighScores&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getGiftAuctionAcquiredGifts::ID: {
    auto& typed = static_cast<td::td_api::getGiftAuctionAcquiredGifts&>(value);
    wipe_native_value(typed.gift_id_, observer);
    return;
}
case td::td_api::getGiftAuctionState::ID: {
    auto& typed = static_cast<td::td_api::getGiftAuctionState&>(value);
    wipe_native_value(typed.auction_id_, observer);
    return;
}
case td::td_api::getGiftChatThemes::ID: {
    auto& typed = static_cast<td::td_api::getGiftChatThemes&>(value);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getGiftCollections::ID: {
    auto& typed = static_cast<td::td_api::getGiftCollections&>(value);
    wipe_native_value(typed.owner_id_, observer);
    return;
}
case td::td_api::getGiftUpgradePreview::ID: {
    auto& typed = static_cast<td::td_api::getGiftUpgradePreview&>(value);
    wipe_native_value(typed.regular_gift_id_, observer);
    return;
}
case td::td_api::getGiftsForCrafting::ID: {
    auto& typed = static_cast<td::td_api::getGiftsForCrafting&>(value);
    wipe_native_value(typed.regular_gift_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getGiveawayInfo::ID: {
    auto& typed = static_cast<td::td_api::getGiveawayInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getGreetingStickers::ID: {
    return;
}
case td::td_api::getGrossingWebAppBots::ID: {
    auto& typed = static_cast<td::td_api::getGrossingWebAppBots&>(value);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getGroupCall::ID: {
    auto& typed = static_cast<td::td_api::getGroupCall&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::getGroupCallParticipants::ID: {
    auto& typed = static_cast<td::td_api::getGroupCallParticipants&>(value);
    wipe_native_value(typed.input_group_call_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getGroupCallStreamSegment::ID: {
    auto& typed = static_cast<td::td_api::getGroupCallStreamSegment&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.time_offset_, observer);
    wipe_native_value(typed.scale_, observer);
    wipe_native_value(typed.channel_id_, observer);
    wipe_native_value(typed.video_quality_, observer);
    return;
}
case td::td_api::getGroupCallStreams::ID: {
    auto& typed = static_cast<td::td_api::getGroupCallStreams&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::getGroupsInCommon::ID: {
    auto& typed = static_cast<td::td_api::getGroupsInCommon&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.offset_chat_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getImportedContactCount::ID: {
    return;
}
case td::td_api::getInactiveSupergroupChats::ID: {
    return;
}
case td::td_api::getInlineGameHighScores::ID: {
    auto& typed = static_cast<td::td_api::getInlineGameHighScores&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getInlineQueryResults::ID: {
    auto& typed = static_cast<td::td_api::getInlineQueryResults&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_location_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_, observer);
    return;
}
case td::td_api::getInstalledBackgrounds::ID: {
    auto& typed = static_cast<td::td_api::getInstalledBackgrounds&>(value);
    wipe_native_value(typed.for_dark_theme_, observer);
    return;
}
case td::td_api::getInstalledStickerSets::ID: {
    auto& typed = static_cast<td::td_api::getInstalledStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    return;
}
case td::td_api::getInternalLink::ID: {
    auto& typed = static_cast<td::td_api::getInternalLink&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.is_http_, observer);
    return;
}
case td::td_api::getInternalLinkType::ID: {
    auto& typed = static_cast<td::td_api::getInternalLinkType&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::getJsonString::ID: {
    auto& typed = static_cast<td::td_api::getJsonString&>(value);
    wipe_native_value(typed.json_value_, observer);
    return;
}
case td::td_api::getJsonValue::ID: {
    auto& typed = static_cast<td::td_api::getJsonValue&>(value);
    wipe_native_value(typed.json_, observer);
    return;
}
case td::td_api::getKeywordEmojis::ID: {
    auto& typed = static_cast<td::td_api::getKeywordEmojis&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.input_language_codes_, observer);
    return;
}
case td::td_api::getLanguagePackInfo::ID: {
    auto& typed = static_cast<td::td_api::getLanguagePackInfo&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    return;
}
case td::td_api::getLanguagePackString::ID: {
    auto& typed = static_cast<td::td_api::getLanguagePackString&>(value);
    wipe_native_value(typed.language_pack_database_path_, observer);
    wipe_native_value(typed.localization_target_, observer);
    wipe_native_value(typed.language_pack_id_, observer);
    wipe_native_value(typed.key_, observer);
    return;
}
case td::td_api::getLanguagePackStrings::ID: {
    auto& typed = static_cast<td::td_api::getLanguagePackStrings&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    wipe_native_value(typed.keys_, observer);
    return;
}
case td::td_api::getLinkPreview::ID: {
    auto& typed = static_cast<td::td_api::getLinkPreview&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.link_preview_options_, observer);
    return;
}
case td::td_api::getLinkWebBrowserType::ID: {
    auto& typed = static_cast<td::td_api::getLinkWebBrowserType&>(value);
    wipe_native_value(typed.link_, observer);
    return;
}
case td::td_api::getLiveStoryAvailableMessageSenders::ID: {
    auto& typed = static_cast<td::td_api::getLiveStoryAvailableMessageSenders&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::getLiveStoryRtmpUrl::ID: {
    auto& typed = static_cast<td::td_api::getLiveStoryRtmpUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getLiveStoryStreamer::ID: {
    auto& typed = static_cast<td::td_api::getLiveStoryStreamer&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::getLiveStoryTopDonors::ID: {
    auto& typed = static_cast<td::td_api::getLiveStoryTopDonors&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::getLocalizationTargetInfo::ID: {
    auto& typed = static_cast<td::td_api::getLocalizationTargetInfo&>(value);
    wipe_native_value(typed.only_local_, observer);
    return;
}
case td::td_api::getLogStream::ID: {
    return;
}
case td::td_api::getLogTagVerbosityLevel::ID: {
    auto& typed = static_cast<td::td_api::getLogTagVerbosityLevel&>(value);
    wipe_native_value(typed.tag_, observer);
    return;
}
case td::td_api::getLogTags::ID: {
    return;
}
case td::td_api::getLogVerbosityLevel::ID: {
    return;
}
case td::td_api::getLoginPasskeys::ID: {
    return;
}
case td::td_api::getLoginUrl::ID: {
    auto& typed = static_cast<td::td_api::getLoginUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.button_id_, observer);
    wipe_native_value(typed.allow_write_access_, observer);
    return;
}
case td::td_api::getLoginUrlInfo::ID: {
    auto& typed = static_cast<td::td_api::getLoginUrlInfo&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.button_id_, observer);
    return;
}
case td::td_api::getMainWebApp::ID: {
    auto& typed = static_cast<td::td_api::getMainWebApp&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::getManagedBotAccessSettings::ID: {
    auto& typed = static_cast<td::td_api::getManagedBotAccessSettings&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::getManagedBotToken::ID: {
    auto& typed = static_cast<td::td_api::getManagedBotToken&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.revoke_, observer);
    return;
}
case td::td_api::getMapThumbnailFile::ID: {
    auto& typed = static_cast<td::td_api::getMapThumbnailFile&>(value);
    wipe_native_value(typed.location_, observer);
    wipe_native_value(typed.zoom_, observer);
    wipe_native_value(typed.width_, observer);
    wipe_native_value(typed.height_, observer);
    wipe_native_value(typed.scale_, observer);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getMarkdownText::ID: {
    auto& typed = static_cast<td::td_api::getMarkdownText&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::getMe::ID: {
    return;
}
case td::td_api::getMenuButton::ID: {
    auto& typed = static_cast<td::td_api::getMenuButton&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getMessage::ID: {
    auto& typed = static_cast<td::td_api::getMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessageAddedReactions::ID: {
    auto& typed = static_cast<td::td_api::getMessageAddedReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reaction_type_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getMessageAuthor::ID: {
    auto& typed = static_cast<td::td_api::getMessageAuthor&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessageAvailableReactions::ID: {
    auto& typed = static_cast<td::td_api::getMessageAvailableReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.row_size_, observer);
    return;
}
case td::td_api::getMessageEffect::ID: {
    auto& typed = static_cast<td::td_api::getMessageEffect&>(value);
    wipe_native_value(typed.effect_id_, observer);
    return;
}
case td::td_api::getMessageEmbeddingCode::ID: {
    auto& typed = static_cast<td::td_api::getMessageEmbeddingCode&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.for_album_, observer);
    return;
}
case td::td_api::getMessageFileType::ID: {
    auto& typed = static_cast<td::td_api::getMessageFileType&>(value);
    wipe_native_value(typed.message_file_head_, observer);
    return;
}
case td::td_api::getMessageImportConfirmationText::ID: {
    auto& typed = static_cast<td::td_api::getMessageImportConfirmationText&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getMessageLink::ID: {
    auto& typed = static_cast<td::td_api::getMessageLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.media_timestamp_, observer);
    wipe_native_value(typed.checklist_task_id_, observer);
    wipe_native_value(typed.poll_option_id_, observer);
    wipe_native_value(typed.for_album_, observer);
    wipe_native_value(typed.in_message_thread_, observer);
    return;
}
case td::td_api::getMessageLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::getMessageLinkInfo&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::getMessageLocally::ID: {
    auto& typed = static_cast<td::td_api::getMessageLocally&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessageProperties::ID: {
    auto& typed = static_cast<td::td_api::getMessageProperties&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessagePublicForwards::ID: {
    auto& typed = static_cast<td::td_api::getMessagePublicForwards&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getMessageReadDate::ID: {
    auto& typed = static_cast<td::td_api::getMessageReadDate&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessageStatistics::ID: {
    auto& typed = static_cast<td::td_api::getMessageStatistics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getMessageThread::ID: {
    auto& typed = static_cast<td::td_api::getMessageThread&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessageThreadHistory::ID: {
    auto& typed = static_cast<td::td_api::getMessageThreadHistory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getMessageViewers::ID: {
    auto& typed = static_cast<td::td_api::getMessageViewers&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getMessages::ID: {
    auto& typed = static_cast<td::td_api::getMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::getNetworkStatistics::ID: {
    auto& typed = static_cast<td::td_api::getNetworkStatistics&>(value);
    wipe_native_value(typed.only_current_, observer);
    return;
}
case td::td_api::getNewChatPrivacySettings::ID: {
    return;
}
case td::td_api::getOauthLinkInfo::ID: {
    auto& typed = static_cast<td::td_api::getOauthLinkInfo&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.in_app_origin_, observer);
    return;
}
case td::td_api::getOption::ID: {
    auto& typed = static_cast<td::td_api::getOption&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::getOwnedBots::ID: {
    return;
}
case td::td_api::getOwnedStickerSets::ID: {
    auto& typed = static_cast<td::td_api::getOwnedStickerSets&>(value);
    wipe_native_value(typed.offset_sticker_set_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getPaidMessageRevenue::ID: {
    auto& typed = static_cast<td::td_api::getPaidMessageRevenue&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getPasskeyParameters::ID: {
    return;
}
case td::td_api::getPassportAuthorizationForm::ID: {
    auto& typed = static_cast<td::td_api::getPassportAuthorizationForm&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.public_key_, observer);
    wipe_native_value(typed.nonce_, observer);
    return;
}
case td::td_api::getPassportAuthorizationFormAvailableElements::ID: {
    auto& typed = static_cast<td::td_api::getPassportAuthorizationFormAvailableElements&>(value);
    wipe_native_value(typed.authorization_form_id_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getPassportElement::ID: {
    auto& typed = static_cast<td::td_api::getPassportElement&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getPasswordState::ID: {
    return;
}
case td::td_api::getPaymentForm::ID: {
    auto& typed = static_cast<td::td_api::getPaymentForm&>(value);
    wipe_native_value(typed.input_invoice_, observer);
    wipe_native_value(typed.theme_, observer);
    return;
}
case td::td_api::getPaymentReceipt::ID: {
    auto& typed = static_cast<td::td_api::getPaymentReceipt&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getPersonalChatHistory::ID: {
    auto& typed = static_cast<td::td_api::getPersonalChatHistory&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getPhoneNumberInfo::ID: {
    auto& typed = static_cast<td::td_api::getPhoneNumberInfo&>(value);
    wipe_native_value(typed.phone_number_prefix_, observer);
    return;
}
case td::td_api::getPhoneNumberInfoSync::ID: {
    auto& typed = static_cast<td::td_api::getPhoneNumberInfoSync&>(value);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.phone_number_prefix_, observer);
    return;
}
case td::td_api::getPollOptionProperties::ID: {
    auto& typed = static_cast<td::td_api::getPollOptionProperties&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.poll_option_id_, observer);
    return;
}
case td::td_api::getPollVoteStatistics::ID: {
    auto& typed = static_cast<td::td_api::getPollVoteStatistics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getPollVoters::ID: {
    auto& typed = static_cast<td::td_api::getPollVoters&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getPreferredCountryLanguage::ID: {
    auto& typed = static_cast<td::td_api::getPreferredCountryLanguage&>(value);
    wipe_native_value(typed.country_code_, observer);
    return;
}
case td::td_api::getPremiumFeatures::ID: {
    auto& typed = static_cast<td::td_api::getPremiumFeatures&>(value);
    wipe_native_value(typed.source_, observer);
    return;
}
case td::td_api::getPremiumGiftPaymentOptions::ID: {
    return;
}
case td::td_api::getPremiumGiveawayPaymentOptions::ID: {
    auto& typed = static_cast<td::td_api::getPremiumGiveawayPaymentOptions&>(value);
    wipe_native_value(typed.boosted_chat_id_, observer);
    return;
}
case td::td_api::getPremiumInfoSticker::ID: {
    auto& typed = static_cast<td::td_api::getPremiumInfoSticker&>(value);
    wipe_native_value(typed.month_count_, observer);
    return;
}
case td::td_api::getPremiumLimit::ID: {
    auto& typed = static_cast<td::td_api::getPremiumLimit&>(value);
    wipe_native_value(typed.limit_type_, observer);
    return;
}
case td::td_api::getPremiumState::ID: {
    return;
}
case td::td_api::getPremiumStickerExamples::ID: {
    return;
}
case td::td_api::getPremiumStickers::ID: {
    auto& typed = static_cast<td::td_api::getPremiumStickers&>(value);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getPreparedInlineMessage::ID: {
    auto& typed = static_cast<td::td_api::getPreparedInlineMessage&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.prepared_message_id_, observer);
    return;
}
case td::td_api::getPreparedKeyboardButton::ID: {
    auto& typed = static_cast<td::td_api::getPreparedKeyboardButton&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.prepared_button_id_, observer);
    return;
}
case td::td_api::getProxies::ID: {
    return;
}
case td::td_api::getPublicPostSearchLimits::ID: {
    auto& typed = static_cast<td::td_api::getPublicPostSearchLimits&>(value);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::getPushReceiverId::ID: {
    auto& typed = static_cast<td::td_api::getPushReceiverId&>(value);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::getReadDatePrivacySettings::ID: {
    return;
}
case td::td_api::getReceivedGift::ID: {
    auto& typed = static_cast<td::td_api::getReceivedGift&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    return;
}
case td::td_api::getReceivedGifts::ID: {
    auto& typed = static_cast<td::td_api::getReceivedGifts&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_id_, observer);
    wipe_native_value(typed.exclude_unsaved_, observer);
    wipe_native_value(typed.exclude_saved_, observer);
    wipe_native_value(typed.exclude_unlimited_, observer);
    wipe_native_value(typed.exclude_upgradable_, observer);
    wipe_native_value(typed.exclude_non_upgradable_, observer);
    wipe_native_value(typed.exclude_upgraded_, observer);
    wipe_native_value(typed.exclude_without_colors_, observer);
    wipe_native_value(typed.exclude_hosted_, observer);
    wipe_native_value(typed.sort_by_price_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getRecentEmojiStatuses::ID: {
    return;
}
case td::td_api::getRecentInlineBots::ID: {
    return;
}
case td::td_api::getRecentStickers::ID: {
    auto& typed = static_cast<td::td_api::getRecentStickers&>(value);
    wipe_native_value(typed.is_attached_, observer);
    return;
}
case td::td_api::getRecentlyOpenedChats::ID: {
    auto& typed = static_cast<td::td_api::getRecentlyOpenedChats&>(value);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getRecentlyVisitedTMeUrls::ID: {
    auto& typed = static_cast<td::td_api::getRecentlyVisitedTMeUrls&>(value);
    wipe_native_value(typed.referrer_, observer);
    return;
}
case td::td_api::getRecommendedChatFolders::ID: {
    return;
}
case td::td_api::getRecommendedChats::ID: {
    return;
}
case td::td_api::getRecoveryEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::getRecoveryEmailAddress&>(value);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getRemoteFile::ID: {
    auto& typed = static_cast<td::td_api::getRemoteFile&>(value);
    wipe_native_value(typed.remote_file_id_, observer);
    wipe_native_value(typed.file_type_, observer);
    return;
}
case td::td_api::getRepliedMessage::ID: {
    auto& typed = static_cast<td::td_api::getRepliedMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getSavedAnimations::ID: {
    return;
}
case td::td_api::getSavedMessagesTags::ID: {
    auto& typed = static_cast<td::td_api::getSavedMessagesTags&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    return;
}
case td::td_api::getSavedMessagesTopicHistory::ID: {
    auto& typed = static_cast<td::td_api::getSavedMessagesTopicHistory&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getSavedMessagesTopicMessageByDate::ID: {
    auto& typed = static_cast<td::td_api::getSavedMessagesTopicMessageByDate&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    wipe_native_value(typed.date_, observer);
    return;
}
case td::td_api::getSavedNotificationSound::ID: {
    auto& typed = static_cast<td::td_api::getSavedNotificationSound&>(value);
    wipe_native_value(typed.notification_sound_id_, observer);
    return;
}
case td::td_api::getSavedNotificationSounds::ID: {
    return;
}
case td::td_api::getSavedOrderInfo::ID: {
    return;
}
case td::td_api::getScopeNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::getScopeNotificationSettings&>(value);
    wipe_native_value(typed.scope_, observer);
    return;
}
case td::td_api::getSearchSponsoredChats::ID: {
    auto& typed = static_cast<td::td_api::getSearchSponsoredChats&>(value);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::getSearchedForTags::ID: {
    auto& typed = static_cast<td::td_api::getSearchedForTags&>(value);
    wipe_native_value(typed.tag_prefix_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getSecretChat::ID: {
    auto& typed = static_cast<td::td_api::getSecretChat&>(value);
    wipe_native_value(typed.secret_chat_id_, observer);
    return;
}
case td::td_api::getStakeDiceState::ID: {
    return;
}
case td::td_api::getStarAdAccountUrl::ID: {
    auto& typed = static_cast<td::td_api::getStarAdAccountUrl&>(value);
    wipe_native_value(typed.owner_id_, observer);
    return;
}
case td::td_api::getStarGiftPaymentOptions::ID: {
    auto& typed = static_cast<td::td_api::getStarGiftPaymentOptions&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getStarGiveawayPaymentOptions::ID: {
    return;
}
case td::td_api::getStarPaymentOptions::ID: {
    return;
}
case td::td_api::getStarRevenueStatistics::ID: {
    auto& typed = static_cast<td::td_api::getStarRevenueStatistics&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getStarSubscriptions::ID: {
    auto& typed = static_cast<td::td_api::getStarSubscriptions&>(value);
    wipe_native_value(typed.only_expiring_, observer);
    wipe_native_value(typed.offset_, observer);
    return;
}
case td::td_api::getStarTransactions::ID: {
    auto& typed = static_cast<td::td_api::getStarTransactions&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.subscription_id_, observer);
    wipe_native_value(typed.direction_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getStarWithdrawalUrl::ID: {
    auto& typed = static_cast<td::td_api::getStarWithdrawalUrl&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getStatisticalGraph::ID: {
    auto& typed = static_cast<td::td_api::getStatisticalGraph&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.token_, observer);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::getStickerEmojis::ID: {
    auto& typed = static_cast<td::td_api::getStickerEmojis&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::getStickerOutline::ID: {
    auto& typed = static_cast<td::td_api::getStickerOutline&>(value);
    wipe_native_value(typed.sticker_file_id_, observer);
    wipe_native_value(typed.for_animated_emoji_, observer);
    wipe_native_value(typed.for_clicked_animated_emoji_message_, observer);
    return;
}
case td::td_api::getStickerOutlineSvgPath::ID: {
    auto& typed = static_cast<td::td_api::getStickerOutlineSvgPath&>(value);
    wipe_native_value(typed.sticker_file_id_, observer);
    wipe_native_value(typed.for_animated_emoji_, observer);
    wipe_native_value(typed.for_clicked_animated_emoji_message_, observer);
    return;
}
case td::td_api::getStickerSet::ID: {
    auto& typed = static_cast<td::td_api::getStickerSet&>(value);
    wipe_native_value(typed.set_id_, observer);
    return;
}
case td::td_api::getStickerSetName::ID: {
    auto& typed = static_cast<td::td_api::getStickerSetName&>(value);
    wipe_native_value(typed.set_id_, observer);
    return;
}
case td::td_api::getStickers::ID: {
    auto& typed = static_cast<td::td_api::getStickers&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getStorageStatistics::ID: {
    auto& typed = static_cast<td::td_api::getStorageStatistics&>(value);
    wipe_native_value(typed.chat_limit_, observer);
    return;
}
case td::td_api::getStorageStatisticsFast::ID: {
    return;
}
case td::td_api::getStory::ID: {
    auto& typed = static_cast<td::td_api::getStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.only_local_, observer);
    return;
}
case td::td_api::getStoryAlbumStories::ID: {
    auto& typed = static_cast<td::td_api::getStoryAlbumStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getStoryAvailableReactions::ID: {
    auto& typed = static_cast<td::td_api::getStoryAvailableReactions&>(value);
    wipe_native_value(typed.row_size_, observer);
    return;
}
case td::td_api::getStoryInteractions::ID: {
    auto& typed = static_cast<td::td_api::getStoryInteractions&>(value);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.only_contacts_, observer);
    wipe_native_value(typed.prefer_forwards_, observer);
    wipe_native_value(typed.prefer_with_reaction_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getStoryNotificationSettingsExceptions::ID: {
    return;
}
case td::td_api::getStoryPublicForwards::ID: {
    auto& typed = static_cast<td::td_api::getStoryPublicForwards&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getStoryStatistics::ID: {
    auto& typed = static_cast<td::td_api::getStoryStatistics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getSuggestedFileName::ID: {
    auto& typed = static_cast<td::td_api::getSuggestedFileName&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.directory_, observer);
    return;
}
case td::td_api::getSuggestedStickerSetName::ID: {
    auto& typed = static_cast<td::td_api::getSuggestedStickerSetName&>(value);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::getSuitableDiscussionChats::ID: {
    return;
}
case td::td_api::getSuitablePersonalChats::ID: {
    return;
}
case td::td_api::getSupergroup::ID: {
    auto& typed = static_cast<td::td_api::getSupergroup&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::getSupergroupFullInfo::ID: {
    auto& typed = static_cast<td::td_api::getSupergroupFullInfo&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::getSupergroupMembers::ID: {
    auto& typed = static_cast<td::td_api::getSupergroupMembers&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.filter_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getSupportName::ID: {
    return;
}
case td::td_api::getSupportUser::ID: {
    return;
}
case td::td_api::getTemporaryPasswordState::ID: {
    return;
}
case td::td_api::getTextCompositionStyleExample::ID: {
    auto& typed = static_cast<td::td_api::getTextCompositionStyleExample&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.example_number_, observer);
    return;
}
case td::td_api::getTextEntities::ID: {
    auto& typed = static_cast<td::td_api::getTextEntities&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::getThemeParametersJsonString::ID: {
    auto& typed = static_cast<td::td_api::getThemeParametersJsonString&>(value);
    wipe_native_value(typed.theme_, observer);
    return;
}
case td::td_api::getThemedChatEmojiStatuses::ID: {
    return;
}
case td::td_api::getThemedEmojiStatuses::ID: {
    return;
}
case td::td_api::getTimeZones::ID: {
    return;
}
case td::td_api::getTonRevenueStatistics::ID: {
    auto& typed = static_cast<td::td_api::getTonRevenueStatistics&>(value);
    wipe_native_value(typed.is_dark_, observer);
    return;
}
case td::td_api::getTonTransactions::ID: {
    auto& typed = static_cast<td::td_api::getTonTransactions&>(value);
    wipe_native_value(typed.direction_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getTonWithdrawalUrl::ID: {
    auto& typed = static_cast<td::td_api::getTonWithdrawalUrl&>(value);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getTopChats::ID: {
    auto& typed = static_cast<td::td_api::getTopChats&>(value);
    wipe_native_value(typed.category_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getTrendingStickerSets::ID: {
    auto& typed = static_cast<td::td_api::getTrendingStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getUpgradedGift::ID: {
    auto& typed = static_cast<td::td_api::getUpgradedGift&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::getUpgradedGiftEmojiStatuses::ID: {
    return;
}
case td::td_api::getUpgradedGiftValueInfo::ID: {
    auto& typed = static_cast<td::td_api::getUpgradedGiftValueInfo&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::getUpgradedGiftVariants::ID: {
    auto& typed = static_cast<td::td_api::getUpgradedGiftVariants&>(value);
    wipe_native_value(typed.regular_gift_id_, observer);
    wipe_native_value(typed.return_upgrade_models_, observer);
    wipe_native_value(typed.return_craft_models_, observer);
    return;
}
case td::td_api::getUpgradedGiftWithdrawalUrl::ID: {
    auto& typed = static_cast<td::td_api::getUpgradedGiftWithdrawalUrl&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::getUpgradedGiftsPromotionalAnimation::ID: {
    return;
}
case td::td_api::getUser::ID: {
    auto& typed = static_cast<td::td_api::getUser&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getUserChatBoosts::ID: {
    auto& typed = static_cast<td::td_api::getUserChatBoosts&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getUserFullInfo::ID: {
    auto& typed = static_cast<td::td_api::getUserFullInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getUserLink::ID: {
    return;
}
case td::td_api::getUserPrivacySettingRules::ID: {
    auto& typed = static_cast<td::td_api::getUserPrivacySettingRules&>(value);
    wipe_native_value(typed.setting_, observer);
    return;
}
case td::td_api::getUserProfileAudios::ID: {
    auto& typed = static_cast<td::td_api::getUserProfileAudios&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getUserProfilePhotos::ID: {
    auto& typed = static_cast<td::td_api::getUserProfilePhotos&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::getUserSupportInfo::ID: {
    auto& typed = static_cast<td::td_api::getUserSupportInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::getVideoChatAvailableParticipants::ID: {
    auto& typed = static_cast<td::td_api::getVideoChatAvailableParticipants&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getVideoChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::getVideoChatInviteLink&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.can_self_unmute_, observer);
    return;
}
case td::td_api::getVideoChatRtmpUrl::ID: {
    auto& typed = static_cast<td::td_api::getVideoChatRtmpUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::getVideoMessageAdvertisements::ID: {
    auto& typed = static_cast<td::td_api::getVideoMessageAdvertisements&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::getWebAppLinkUrl::ID: {
    auto& typed = static_cast<td::td_api::getWebAppLinkUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.web_app_short_name_, observer);
    wipe_native_value(typed.start_parameter_, observer);
    wipe_native_value(typed.allow_write_access_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::getWebAppPlaceholder::ID: {
    auto& typed = static_cast<td::td_api::getWebAppPlaceholder&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    return;
}
case td::td_api::getWebAppUrl::ID: {
    auto& typed = static_cast<td::td_api::getWebAppUrl&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::getWebPageInstantView::ID: {
    auto& typed = static_cast<td::td_api::getWebPageInstantView&>(value);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.only_local_, observer);
    return;
}
case td::td_api::giftPremiumWithStars::ID: {
    auto& typed = static_cast<td::td_api::giftPremiumWithStars&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.month_count_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::hideContactCloseBirthdays::ID: {
    return;
}
case td::td_api::hideSuggestedAction::ID: {
    auto& typed = static_cast<td::td_api::hideSuggestedAction&>(value);
    wipe_native_value(typed.action_, observer);
    return;
}
case td::td_api::importContacts::ID: {
    auto& typed = static_cast<td::td_api::importContacts&>(value);
    wipe_native_value(typed.contacts_, observer);
    return;
}
case td::td_api::importMessages::ID: {
    auto& typed = static_cast<td::td_api::importMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_file_, observer);
    wipe_native_value(typed.attached_files_, observer);
    return;
}
case td::td_api::increaseGiftAuctionBid::ID: {
    auto& typed = static_cast<td::td_api::increaseGiftAuctionBid&>(value);
    wipe_native_value(typed.gift_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::inviteGroupCallParticipant::ID: {
    auto& typed = static_cast<td::td_api::inviteGroupCallParticipant&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.is_video_, observer);
    return;
}
case td::td_api::inviteVideoChatParticipants::ID: {
    auto& typed = static_cast<td::td_api::inviteVideoChatParticipants&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::isLoginEmailAddressRequired::ID: {
    return;
}
case td::td_api::isProfileAudio::ID: {
    auto& typed = static_cast<td::td_api::isProfileAudio&>(value);
    wipe_native_value(typed.file_id_, observer);
    return;
}
case td::td_api::joinChat::ID: {
    auto& typed = static_cast<td::td_api::joinChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::joinChatByInviteLink::ID: {
    auto& typed = static_cast<td::td_api::joinChatByInviteLink&>(value);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::joinGroupCall::ID: {
    auto& typed = static_cast<td::td_api::joinGroupCall&>(value);
    wipe_native_value(typed.input_group_call_, observer);
    wipe_native_value(typed.join_parameters_, observer);
    return;
}
case td::td_api::joinLiveStory::ID: {
    auto& typed = static_cast<td::td_api::joinLiveStory&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.join_parameters_, observer);
    return;
}
case td::td_api::joinVideoChat::ID: {
    auto& typed = static_cast<td::td_api::joinVideoChat&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.join_parameters_, observer);
    wipe_native_value(typed.invite_hash_, observer);
    return;
}
case td::td_api::launchPrepaidGiveaway::ID: {
    auto& typed = static_cast<td::td_api::launchPrepaidGiveaway&>(value);
    wipe_native_value(typed.giveaway_id_, observer);
    wipe_native_value(typed.parameters_, observer);
    wipe_native_value(typed.winner_count_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::leaveChat::ID: {
    auto& typed = static_cast<td::td_api::leaveChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::leaveGroupCall::ID: {
    auto& typed = static_cast<td::td_api::leaveGroupCall&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::listenToAudio::ID: {
    auto& typed = static_cast<td::td_api::listenToAudio&>(value);
    wipe_native_value(typed.audio_file_id_, observer);
    wipe_native_value(typed.duration_, observer);
    return;
}
case td::td_api::loadActiveStories::ID: {
    auto& typed = static_cast<td::td_api::loadActiveStories&>(value);
    wipe_native_value(typed.story_list_, observer);
    return;
}
case td::td_api::loadChats::ID: {
    auto& typed = static_cast<td::td_api::loadChats&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::loadDirectMessagesChatTopics::ID: {
    auto& typed = static_cast<td::td_api::loadDirectMessagesChatTopics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::loadGroupCallParticipants::ID: {
    auto& typed = static_cast<td::td_api::loadGroupCallParticipants&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::loadQuickReplyShortcutMessages::ID: {
    auto& typed = static_cast<td::td_api::loadQuickReplyShortcutMessages&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    return;
}
case td::td_api::loadQuickReplyShortcuts::ID: {
    return;
}
case td::td_api::loadSavedMessagesTopics::ID: {
    auto& typed = static_cast<td::td_api::loadSavedMessagesTopics&>(value);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::logOut::ID: {
    return;
}
case td::td_api::markChecklistTasksAsDone::ID: {
    auto& typed = static_cast<td::td_api::markChecklistTasksAsDone&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.marked_as_done_task_ids_, observer);
    wipe_native_value(typed.marked_as_not_done_task_ids_, observer);
    return;
}
case td::td_api::openBotSimilarBot::ID: {
    auto& typed = static_cast<td::td_api::openBotSimilarBot&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.opened_bot_user_id_, observer);
    return;
}
case td::td_api::openChat::ID: {
    auto& typed = static_cast<td::td_api::openChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::openChatSimilarChat::ID: {
    auto& typed = static_cast<td::td_api::openChatSimilarChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.opened_chat_id_, observer);
    return;
}
case td::td_api::openGiftAuction::ID: {
    auto& typed = static_cast<td::td_api::openGiftAuction&>(value);
    wipe_native_value(typed.gift_id_, observer);
    return;
}
case td::td_api::openMessageContent::ID: {
    auto& typed = static_cast<td::td_api::openMessageContent&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::openSponsoredChat::ID: {
    auto& typed = static_cast<td::td_api::openSponsoredChat&>(value);
    wipe_native_value(typed.sponsored_chat_unique_id_, observer);
    return;
}
case td::td_api::openStory::ID: {
    auto& typed = static_cast<td::td_api::openStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    return;
}
case td::td_api::openWebApp::ID: {
    auto& typed = static_cast<td::td_api::openWebApp&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.url_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::optimizeStorage::ID: {
    auto& typed = static_cast<td::td_api::optimizeStorage&>(value);
    wipe_native_value(typed.size_, observer);
    wipe_native_value(typed.ttl_, observer);
    wipe_native_value(typed.count_, observer);
    wipe_native_value(typed.immunity_delay_, observer);
    wipe_native_value(typed.file_types_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    wipe_native_value(typed.exclude_chat_ids_, observer);
    wipe_native_value(typed.return_deleted_file_statistics_, observer);
    wipe_native_value(typed.chat_limit_, observer);
    return;
}
case td::td_api::parseMarkdown::ID: {
    auto& typed = static_cast<td::td_api::parseMarkdown&>(value);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::parseTextEntities::ID: {
    auto& typed = static_cast<td::td_api::parseTextEntities&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.parse_mode_, observer);
    return;
}
case td::td_api::pinChatMessage::ID: {
    auto& typed = static_cast<td::td_api::pinChatMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.disable_notification_, observer);
    wipe_native_value(typed.only_for_self_, observer);
    return;
}
case td::td_api::pingProxy::ID: {
    auto& typed = static_cast<td::td_api::pingProxy&>(value);
    wipe_native_value(typed.proxy_, observer);
    return;
}
case td::td_api::placeGiftAuctionBid::ID: {
    auto& typed = static_cast<td::td_api::placeGiftAuctionBid&>(value);
    wipe_native_value(typed.gift_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.is_private_, observer);
    return;
}
case td::td_api::postStory::ID: {
    auto& typed = static_cast<td::td_api::postStory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.content_, observer);
    wipe_native_value(typed.areas_, observer);
    wipe_native_value(typed.caption_, observer);
    wipe_native_value(typed.privacy_settings_, observer);
    wipe_native_value(typed.album_ids_, observer);
    wipe_native_value(typed.active_period_, observer);
    wipe_native_value(typed.from_story_full_id_, observer);
    wipe_native_value(typed.is_posted_to_chat_page_, observer);
    wipe_native_value(typed.protect_content_, observer);
    return;
}
case td::td_api::preliminaryUploadFile::ID: {
    auto& typed = static_cast<td::td_api::preliminaryUploadFile&>(value);
    wipe_native_value(typed.file_, observer);
    wipe_native_value(typed.file_type_, observer);
    wipe_native_value(typed.priority_, observer);
    return;
}
case td::td_api::processChatFolderNewChats::ID: {
    auto& typed = static_cast<td::td_api::processChatFolderNewChats&>(value);
    wipe_native_value(typed.chat_folder_id_, observer);
    wipe_native_value(typed.added_chat_ids_, observer);
    return;
}
case td::td_api::processChatHasProtectedContentDisableRequest::ID: {
    auto& typed = static_cast<td::td_api::processChatHasProtectedContentDisableRequest&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.request_message_id_, observer);
    wipe_native_value(typed.approve_, observer);
    return;
}
case td::td_api::processChatJoinRequest::ID: {
    auto& typed = static_cast<td::td_api::processChatJoinRequest&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.approve_, observer);
    return;
}
case td::td_api::processChatJoinRequests::ID: {
    auto& typed = static_cast<td::td_api::processChatJoinRequests&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    wipe_native_value(typed.approve_, observer);
    return;
}
case td::td_api::processGiftPurchaseOffer::ID: {
    auto& typed = static_cast<td::td_api::processGiftPurchaseOffer&>(value);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.accept_, observer);
    return;
}
case td::td_api::processPushNotification::ID: {
    auto& typed = static_cast<td::td_api::processPushNotification&>(value);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::rateSpeechRecognition::ID: {
    auto& typed = static_cast<td::td_api::rateSpeechRecognition&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_good_, observer);
    return;
}
case td::td_api::readAllChatMentions::ID: {
    auto& typed = static_cast<td::td_api::readAllChatMentions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::readAllChatPollVotes::ID: {
    auto& typed = static_cast<td::td_api::readAllChatPollVotes&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::readAllChatReactions::ID: {
    auto& typed = static_cast<td::td_api::readAllChatReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::readAllDirectMessagesChatTopicReactions::ID: {
    auto& typed = static_cast<td::td_api::readAllDirectMessagesChatTopicReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::readAllForumTopicMentions::ID: {
    auto& typed = static_cast<td::td_api::readAllForumTopicMentions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::readAllForumTopicPollVotes::ID: {
    auto& typed = static_cast<td::td_api::readAllForumTopicPollVotes&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::readAllForumTopicReactions::ID: {
    auto& typed = static_cast<td::td_api::readAllForumTopicReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::readBusinessMessage::ID: {
    auto& typed = static_cast<td::td_api::readBusinessMessage&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::readChatList::ID: {
    auto& typed = static_cast<td::td_api::readChatList&>(value);
    wipe_native_value(typed.chat_list_, observer);
    return;
}
case td::td_api::readFilePart::ID: {
    auto& typed = static_cast<td::td_api::readFilePart&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.count_, observer);
    return;
}
case td::td_api::readdQuickReplyShortcutMessages::ID: {
    auto& typed = static_cast<td::td_api::readdQuickReplyShortcutMessages&>(value);
    wipe_native_value(typed.shortcut_name_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::recognizeSpeech::ID: {
    auto& typed = static_cast<td::td_api::recognizeSpeech&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::recoverAuthenticationPassword::ID: {
    auto& typed = static_cast<td::td_api::recoverAuthenticationPassword&>(value);
    wipe_native_value(typed.recovery_code_, observer);
    wipe_native_value(typed.new_password_, observer);
    wipe_native_value(typed.new_hint_, observer);
    return;
}
case td::td_api::recoverPassword::ID: {
    auto& typed = static_cast<td::td_api::recoverPassword&>(value);
    wipe_native_value(typed.recovery_code_, observer);
    wipe_native_value(typed.new_password_, observer);
    wipe_native_value(typed.new_hint_, observer);
    return;
}
case td::td_api::refundStarPayment::ID: {
    auto& typed = static_cast<td::td_api::refundStarPayment&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.telegram_payment_charge_id_, observer);
    return;
}
case td::td_api::registerDevice::ID: {
    auto& typed = static_cast<td::td_api::registerDevice&>(value);
    wipe_native_value(typed.device_token_, observer);
    wipe_native_value(typed.other_user_ids_, observer);
    return;
}
case td::td_api::registerUser::ID: {
    auto& typed = static_cast<td::td_api::registerUser&>(value);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    wipe_native_value(typed.disable_notification_, observer);
    return;
}
case td::td_api::removeAllFilesFromDownloads::ID: {
    auto& typed = static_cast<td::td_api::removeAllFilesFromDownloads&>(value);
    wipe_native_value(typed.only_active_, observer);
    wipe_native_value(typed.only_completed_, observer);
    wipe_native_value(typed.delete_from_cache_, observer);
    return;
}
case td::td_api::removeAllWebBrowserSettingsExceptions::ID: {
    return;
}
case td::td_api::removeBusinessConnectedBotFromChat::ID: {
    auto& typed = static_cast<td::td_api::removeBusinessConnectedBotFromChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::removeChatActionBar::ID: {
    auto& typed = static_cast<td::td_api::removeChatActionBar&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::removeContacts::ID: {
    auto& typed = static_cast<td::td_api::removeContacts&>(value);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::removeFavoriteSticker::ID: {
    auto& typed = static_cast<td::td_api::removeFavoriteSticker&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::removeFileFromDownloads::ID: {
    auto& typed = static_cast<td::td_api::removeFileFromDownloads&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.delete_from_cache_, observer);
    return;
}
case td::td_api::removeGiftCollectionGifts::ID: {
    auto& typed = static_cast<td::td_api::removeGiftCollectionGifts&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_id_, observer);
    wipe_native_value(typed.received_gift_ids_, observer);
    return;
}
case td::td_api::removeInstalledBackground::ID: {
    auto& typed = static_cast<td::td_api::removeInstalledBackground&>(value);
    wipe_native_value(typed.background_id_, observer);
    return;
}
case td::td_api::removeLoginPasskey::ID: {
    auto& typed = static_cast<td::td_api::removeLoginPasskey&>(value);
    wipe_native_value(typed.passkey_id_, observer);
    return;
}
case td::td_api::removeMessageReaction::ID: {
    auto& typed = static_cast<td::td_api::removeMessageReaction&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reaction_type_, observer);
    return;
}
case td::td_api::removeMessageSenderBotVerification::ID: {
    auto& typed = static_cast<td::td_api::removeMessageSenderBotVerification&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.verified_id_, observer);
    return;
}
case td::td_api::removeNotification::ID: {
    auto& typed = static_cast<td::td_api::removeNotification&>(value);
    wipe_native_value(typed.notification_group_id_, observer);
    wipe_native_value(typed.notification_id_, observer);
    return;
}
case td::td_api::removeNotificationGroup::ID: {
    auto& typed = static_cast<td::td_api::removeNotificationGroup&>(value);
    wipe_native_value(typed.notification_group_id_, observer);
    wipe_native_value(typed.max_notification_id_, observer);
    return;
}
case td::td_api::removePendingLiveStoryReactions::ID: {
    auto& typed = static_cast<td::td_api::removePendingLiveStoryReactions&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::removePendingPaidMessageReactions::ID: {
    auto& typed = static_cast<td::td_api::removePendingPaidMessageReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::removeProfileAudio::ID: {
    auto& typed = static_cast<td::td_api::removeProfileAudio&>(value);
    wipe_native_value(typed.file_id_, observer);
    return;
}
case td::td_api::removeProxy::ID: {
    auto& typed = static_cast<td::td_api::removeProxy&>(value);
    wipe_native_value(typed.proxy_id_, observer);
    return;
}
case td::td_api::removeRecentHashtag::ID: {
    auto& typed = static_cast<td::td_api::removeRecentHashtag&>(value);
    wipe_native_value(typed.hashtag_, observer);
    return;
}
case td::td_api::removeRecentSticker::ID: {
    auto& typed = static_cast<td::td_api::removeRecentSticker&>(value);
    wipe_native_value(typed.is_attached_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::removeRecentlyFoundChat::ID: {
    auto& typed = static_cast<td::td_api::removeRecentlyFoundChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::removeSavedAnimation::ID: {
    auto& typed = static_cast<td::td_api::removeSavedAnimation&>(value);
    wipe_native_value(typed.animation_, observer);
    return;
}
case td::td_api::removeSavedNotificationSound::ID: {
    auto& typed = static_cast<td::td_api::removeSavedNotificationSound&>(value);
    wipe_native_value(typed.notification_sound_id_, observer);
    return;
}
case td::td_api::removeSearchedForTag::ID: {
    auto& typed = static_cast<td::td_api::removeSearchedForTag&>(value);
    wipe_native_value(typed.tag_, observer);
    return;
}
case td::td_api::removeStickerFromSet::ID: {
    auto& typed = static_cast<td::td_api::removeStickerFromSet&>(value);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::removeStoryAlbumStories::ID: {
    auto& typed = static_cast<td::td_api::removeStoryAlbumStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    wipe_native_value(typed.story_ids_, observer);
    return;
}
case td::td_api::removeTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::removeTextCompositionStyle&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::removeTopChat::ID: {
    auto& typed = static_cast<td::td_api::removeTopChat&>(value);
    wipe_native_value(typed.category_, observer);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::removeWebBrowserSettingsException::ID: {
    auto& typed = static_cast<td::td_api::removeWebBrowserSettingsException&>(value);
    wipe_native_value(typed.url_, observer);
    return;
}
case td::td_api::reorderActiveUsernames::ID: {
    auto& typed = static_cast<td::td_api::reorderActiveUsernames&>(value);
    wipe_native_value(typed.usernames_, observer);
    return;
}
case td::td_api::reorderBotActiveUsernames::ID: {
    auto& typed = static_cast<td::td_api::reorderBotActiveUsernames&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.usernames_, observer);
    return;
}
case td::td_api::reorderBotMediaPreviews::ID: {
    auto& typed = static_cast<td::td_api::reorderBotMediaPreviews&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.file_ids_, observer);
    return;
}
case td::td_api::reorderChatFolders::ID: {
    auto& typed = static_cast<td::td_api::reorderChatFolders&>(value);
    wipe_native_value(typed.chat_folder_ids_, observer);
    wipe_native_value(typed.main_chat_list_position_, observer);
    return;
}
case td::td_api::reorderGiftCollectionGifts::ID: {
    auto& typed = static_cast<td::td_api::reorderGiftCollectionGifts&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_id_, observer);
    wipe_native_value(typed.received_gift_ids_, observer);
    return;
}
case td::td_api::reorderGiftCollections::ID: {
    auto& typed = static_cast<td::td_api::reorderGiftCollections&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_ids_, observer);
    return;
}
case td::td_api::reorderInstalledStickerSets::ID: {
    auto& typed = static_cast<td::td_api::reorderInstalledStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.sticker_set_ids_, observer);
    return;
}
case td::td_api::reorderQuickReplyShortcuts::ID: {
    auto& typed = static_cast<td::td_api::reorderQuickReplyShortcuts&>(value);
    wipe_native_value(typed.shortcut_ids_, observer);
    return;
}
case td::td_api::reorderStoryAlbumStories::ID: {
    auto& typed = static_cast<td::td_api::reorderStoryAlbumStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    wipe_native_value(typed.story_ids_, observer);
    return;
}
case td::td_api::reorderStoryAlbums::ID: {
    auto& typed = static_cast<td::td_api::reorderStoryAlbums&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_ids_, observer);
    return;
}
case td::td_api::reorderSupergroupActiveUsernames::ID: {
    auto& typed = static_cast<td::td_api::reorderSupergroupActiveUsernames&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.usernames_, observer);
    return;
}
case td::td_api::replaceLiveStoryRtmpUrl::ID: {
    auto& typed = static_cast<td::td_api::replaceLiveStoryRtmpUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::replacePrimaryChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::replacePrimaryChatInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::replaceStickerInSet::ID: {
    auto& typed = static_cast<td::td_api::replaceStickerInSet&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.old_sticker_, observer);
    wipe_native_value(typed.new_sticker_, observer);
    return;
}
case td::td_api::replaceVideoChatRtmpUrl::ID: {
    auto& typed = static_cast<td::td_api::replaceVideoChatRtmpUrl&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::reportAuthenticationCodeMissing::ID: {
    auto& typed = static_cast<td::td_api::reportAuthenticationCodeMissing&>(value);
    wipe_native_value(typed.mobile_network_code_, observer);
    return;
}
case td::td_api::reportChat::ID: {
    auto& typed = static_cast<td::td_api::reportChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::reportChatPhoto::ID: {
    auto& typed = static_cast<td::td_api::reportChatPhoto&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.reason_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::reportChatSponsoredMessage::ID: {
    auto& typed = static_cast<td::td_api::reportChatSponsoredMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    return;
}
case td::td_api::reportMessageReactions::ID: {
    auto& typed = static_cast<td::td_api::reportMessageReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.sender_id_, observer);
    return;
}
case td::td_api::reportPhoneNumberCodeMissing::ID: {
    auto& typed = static_cast<td::td_api::reportPhoneNumberCodeMissing&>(value);
    wipe_native_value(typed.mobile_network_code_, observer);
    return;
}
case td::td_api::reportSponsoredChat::ID: {
    auto& typed = static_cast<td::td_api::reportSponsoredChat&>(value);
    wipe_native_value(typed.sponsored_chat_unique_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    return;
}
case td::td_api::reportStory::ID: {
    auto& typed = static_cast<td::td_api::reportStory&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::reportSupergroupAntiSpamFalsePositive::ID: {
    auto& typed = static_cast<td::td_api::reportSupergroupAntiSpamFalsePositive&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::reportSupergroupSpam::ID: {
    auto& typed = static_cast<td::td_api::reportSupergroupSpam&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    return;
}
case td::td_api::reportVideoMessageAdvertisement::ID: {
    auto& typed = static_cast<td::td_api::reportVideoMessageAdvertisement&>(value);
    wipe_native_value(typed.advertisement_unique_id_, observer);
    wipe_native_value(typed.option_id_, observer);
    return;
}
case td::td_api::requestAuthenticationPasswordRecovery::ID: {
    return;
}
case td::td_api::requestPasswordRecovery::ID: {
    return;
}
case td::td_api::requestQrCodeAuthentication::ID: {
    auto& typed = static_cast<td::td_api::requestQrCodeAuthentication&>(value);
    wipe_native_value(typed.other_user_ids_, observer);
    return;
}
case td::td_api::resendAuthenticationCode::ID: {
    auto& typed = static_cast<td::td_api::resendAuthenticationCode&>(value);
    wipe_native_value(typed.reason_, observer);
    return;
}
case td::td_api::resendEmailAddressVerificationCode::ID: {
    return;
}
case td::td_api::resendLoginEmailAddressCode::ID: {
    return;
}
case td::td_api::resendMessages::ID: {
    auto& typed = static_cast<td::td_api::resendMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.quote_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::resendPhoneNumberCode::ID: {
    auto& typed = static_cast<td::td_api::resendPhoneNumberCode&>(value);
    wipe_native_value(typed.reason_, observer);
    return;
}
case td::td_api::resendRecoveryEmailAddressCode::ID: {
    return;
}
case td::td_api::resetAllNotificationSettings::ID: {
    return;
}
case td::td_api::resetAuthenticationEmailAddress::ID: {
    return;
}
case td::td_api::resetInstalledBackgrounds::ID: {
    return;
}
case td::td_api::resetNetworkStatistics::ID: {
    return;
}
case td::td_api::resetPassword::ID: {
    return;
}
case td::td_api::reuseStarSubscription::ID: {
    auto& typed = static_cast<td::td_api::reuseStarSubscription&>(value);
    wipe_native_value(typed.subscription_id_, observer);
    return;
}
case td::td_api::revokeChatInviteLink::ID: {
    auto& typed = static_cast<td::td_api::revokeChatInviteLink&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.invite_link_, observer);
    return;
}
case td::td_api::revokeGroupCallInviteLink::ID: {
    auto& typed = static_cast<td::td_api::revokeGroupCallInviteLink&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::saveApplicationLogEvent::ID: {
    auto& typed = static_cast<td::td_api::saveApplicationLogEvent&>(value);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::savePreparedInlineMessage::ID: {
    auto& typed = static_cast<td::td_api::savePreparedInlineMessage&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.result_, observer);
    wipe_native_value(typed.chat_types_, observer);
    return;
}
case td::td_api::savePreparedKeyboardButton::ID: {
    auto& typed = static_cast<td::td_api::savePreparedKeyboardButton&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.button_, observer);
    return;
}
case td::td_api::searchAffiliatePrograms::ID: {
    auto& typed = static_cast<td::td_api::searchAffiliatePrograms&>(value);
    wipe_native_value(typed.affiliate_, observer);
    wipe_native_value(typed.sort_order_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchBackground::ID: {
    auto& typed = static_cast<td::td_api::searchBackground&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::searchCallMessages::ID: {
    auto& typed = static_cast<td::td_api::searchCallMessages&>(value);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.only_missed_, observer);
    return;
}
case td::td_api::searchChatAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::searchChatAffiliateProgram&>(value);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.referrer_, observer);
    return;
}
case td::td_api::searchChatMembers::ID: {
    auto& typed = static_cast<td::td_api::searchChatMembers&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.filter_, observer);
    return;
}
case td::td_api::searchChatMessages::ID: {
    auto& typed = static_cast<td::td_api::searchChatMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.filter_, observer);
    return;
}
case td::td_api::searchChatRecentLocationMessages::ID: {
    auto& typed = static_cast<td::td_api::searchChatRecentLocationMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchChats::ID: {
    auto& typed = static_cast<td::td_api::searchChats&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.type_filter_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchChatsOnServer::ID: {
    auto& typed = static_cast<td::td_api::searchChatsOnServer&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.type_filter_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchContacts::ID: {
    auto& typed = static_cast<td::td_api::searchContacts&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchEmojis::ID: {
    auto& typed = static_cast<td::td_api::searchEmojis&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.input_language_codes_, observer);
    return;
}
case td::td_api::searchFileDownloads::ID: {
    auto& typed = static_cast<td::td_api::searchFileDownloads&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.only_active_, observer);
    wipe_native_value(typed.only_completed_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchGiftsForResale::ID: {
    auto& typed = static_cast<td::td_api::searchGiftsForResale&>(value);
    wipe_native_value(typed.gift_id_, observer);
    wipe_native_value(typed.order_, observer);
    wipe_native_value(typed.for_crafting_, observer);
    wipe_native_value(typed.for_stars_, observer);
    wipe_native_value(typed.attributes_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchHashtags::ID: {
    auto& typed = static_cast<td::td_api::searchHashtags&>(value);
    wipe_native_value(typed.prefix_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchInstalledStickerSets::ID: {
    auto& typed = static_cast<td::td_api::searchInstalledStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchMessages::ID: {
    auto& typed = static_cast<td::td_api::searchMessages&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.filter_, observer);
    wipe_native_value(typed.chat_type_filter_, observer);
    wipe_native_value(typed.min_date_, observer);
    wipe_native_value(typed.max_date_, observer);
    return;
}
case td::td_api::searchOutgoingDocumentMessages::ID: {
    auto& typed = static_cast<td::td_api::searchOutgoingDocumentMessages&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchPublicChat::ID: {
    auto& typed = static_cast<td::td_api::searchPublicChat&>(value);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::searchPublicChats::ID: {
    auto& typed = static_cast<td::td_api::searchPublicChats&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.type_filter_, observer);
    return;
}
case td::td_api::searchPublicMessagesByTag::ID: {
    auto& typed = static_cast<td::td_api::searchPublicMessagesByTag&>(value);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchPublicPosts::ID: {
    auto& typed = static_cast<td::td_api::searchPublicPosts&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::searchPublicStoriesByLocation::ID: {
    auto& typed = static_cast<td::td_api::searchPublicStoriesByLocation&>(value);
    wipe_native_value(typed.address_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchPublicStoriesByTag::ID: {
    auto& typed = static_cast<td::td_api::searchPublicStoriesByTag&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchPublicStoriesByVenue::ID: {
    auto& typed = static_cast<td::td_api::searchPublicStoriesByVenue&>(value);
    wipe_native_value(typed.venue_provider_, observer);
    wipe_native_value(typed.venue_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchQuote::ID: {
    auto& typed = static_cast<td::td_api::searchQuote&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.quote_, observer);
    wipe_native_value(typed.quote_position_, observer);
    return;
}
case td::td_api::searchRecentlyFoundChats::ID: {
    auto& typed = static_cast<td::td_api::searchRecentlyFoundChats&>(value);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.type_filter_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchSavedMessages::ID: {
    auto& typed = static_cast<td::td_api::searchSavedMessages&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.from_message_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchSecretMessages::ID: {
    auto& typed = static_cast<td::td_api::searchSecretMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.filter_, observer);
    return;
}
case td::td_api::searchStickerSet::ID: {
    auto& typed = static_cast<td::td_api::searchStickerSet&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.ignore_cache_, observer);
    return;
}
case td::td_api::searchStickerSets::ID: {
    auto& typed = static_cast<td::td_api::searchStickerSets&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.query_, observer);
    return;
}
case td::td_api::searchStickers::ID: {
    auto& typed = static_cast<td::td_api::searchStickers&>(value);
    wipe_native_value(typed.sticker_type_, observer);
    wipe_native_value(typed.emojis_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.input_language_codes_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.limit_, observer);
    return;
}
case td::td_api::searchStringsByPrefix::ID: {
    auto& typed = static_cast<td::td_api::searchStringsByPrefix&>(value);
    wipe_native_value(typed.strings_, observer);
    wipe_native_value(typed.query_, observer);
    wipe_native_value(typed.limit_, observer);
    wipe_native_value(typed.return_none_for_empty_query_, observer);
    return;
}
case td::td_api::searchTextCompositionStyle::ID: {
    auto& typed = static_cast<td::td_api::searchTextCompositionStyle&>(value);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::searchUserByPhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::searchUserByPhoneNumber&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.only_local_, observer);
    return;
}
case td::td_api::searchUserByToken::ID: {
    auto& typed = static_cast<td::td_api::searchUserByToken&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::searchWebApp::ID: {
    auto& typed = static_cast<td::td_api::searchWebApp&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.web_app_short_name_, observer);
    return;
}
case td::td_api::sellGift::ID: {
    auto& typed = static_cast<td::td_api::sellGift&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    return;
}
case td::td_api::sendAuthenticationFirebaseSms::ID: {
    auto& typed = static_cast<td::td_api::sendAuthenticationFirebaseSms&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::sendBotStartMessage::ID: {
    auto& typed = static_cast<td::td_api::sendBotStartMessage&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.parameter_, observer);
    return;
}
case td::td_api::sendBusinessMessage::ID: {
    auto& typed = static_cast<td::td_api::sendBusinessMessage&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.disable_notification_, observer);
    wipe_native_value(typed.protect_content_, observer);
    wipe_native_value(typed.effect_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::sendBusinessMessageAlbum::ID: {
    auto& typed = static_cast<td::td_api::sendBusinessMessageAlbum&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.disable_notification_, observer);
    wipe_native_value(typed.protect_content_, observer);
    wipe_native_value(typed.effect_id_, observer);
    wipe_native_value(typed.input_message_contents_, observer);
    return;
}
case td::td_api::sendCallDebugInformation::ID: {
    auto& typed = static_cast<td::td_api::sendCallDebugInformation&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.debug_information_, observer);
    return;
}
case td::td_api::sendCallLog::ID: {
    auto& typed = static_cast<td::td_api::sendCallLog&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.log_file_, observer);
    return;
}
case td::td_api::sendCallRating::ID: {
    auto& typed = static_cast<td::td_api::sendCallRating&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.rating_, observer);
    wipe_native_value(typed.comment_, observer);
    wipe_native_value(typed.problems_, observer);
    return;
}
case td::td_api::sendCallSignalingData::ID: {
    auto& typed = static_cast<td::td_api::sendCallSignalingData&>(value);
    wipe_native_value(typed.call_id_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::sendChatAction::ID: {
    auto& typed = static_cast<td::td_api::sendChatAction&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.action_, observer);
    return;
}
case td::td_api::sendCustomRequest::ID: {
    auto& typed = static_cast<td::td_api::sendCustomRequest&>(value);
    wipe_native_value(typed.method_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::sendEmailAddressVerificationCode::ID: {
    auto& typed = static_cast<td::td_api::sendEmailAddressVerificationCode&>(value);
    wipe_native_value(typed.email_address_, observer);
    return;
}
case td::td_api::sendGift::ID: {
    auto& typed = static_cast<td::td_api::sendGift&>(value);
    wipe_native_value(typed.gift_id_, observer);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.is_private_, observer);
    wipe_native_value(typed.pay_for_upgrade_, observer);
    return;
}
case td::td_api::sendGiftPurchaseOffer::ID: {
    auto& typed = static_cast<td::td_api::sendGiftPurchaseOffer&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.gift_name_, observer);
    wipe_native_value(typed.price_, observer);
    wipe_native_value(typed.duration_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::sendGroupCallMessage::ID: {
    auto& typed = static_cast<td::td_api::sendGroupCallMessage&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::sendInlineQueryResultMessage::ID: {
    auto& typed = static_cast<td::td_api::sendInlineQueryResultMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.options_, observer);
    wipe_native_value(typed.query_id_, observer);
    wipe_native_value(typed.result_id_, observer);
    wipe_native_value(typed.hide_via_bot_, observer);
    return;
}
case td::td_api::sendMessage::ID: {
    auto& typed = static_cast<td::td_api::sendMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.options_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    wipe_native_value(typed.input_message_content_, observer);
    return;
}
case td::td_api::sendMessageAlbum::ID: {
    auto& typed = static_cast<td::td_api::sendMessageAlbum&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.reply_to_, observer);
    wipe_native_value(typed.options_, observer);
    wipe_native_value(typed.input_message_contents_, observer);
    return;
}
case td::td_api::sendMessageViewMetrics::ID: {
    auto& typed = static_cast<td::td_api::sendMessageViewMetrics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.time_in_view_ms_, observer);
    wipe_native_value(typed.active_time_in_view_ms_, observer);
    wipe_native_value(typed.height_to_viewport_ratio_per_mille_, observer);
    wipe_native_value(typed.seen_range_ratio_per_mille_, observer);
    return;
}
case td::td_api::sendPassportAuthorizationForm::ID: {
    auto& typed = static_cast<td::td_api::sendPassportAuthorizationForm&>(value);
    wipe_native_value(typed.authorization_form_id_, observer);
    wipe_native_value(typed.types_, observer);
    return;
}
case td::td_api::sendPaymentForm::ID: {
    auto& typed = static_cast<td::td_api::sendPaymentForm&>(value);
    wipe_native_value(typed.input_invoice_, observer);
    wipe_native_value(typed.payment_form_id_, observer);
    wipe_native_value(typed.order_info_id_, observer);
    wipe_native_value(typed.shipping_option_id_, observer);
    wipe_native_value(typed.credentials_, observer);
    wipe_native_value(typed.tip_amount_, observer);
    return;
}
case td::td_api::sendPhoneNumberCode::ID: {
    auto& typed = static_cast<td::td_api::sendPhoneNumberCode&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.settings_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::sendPhoneNumberFirebaseSms::ID: {
    auto& typed = static_cast<td::td_api::sendPhoneNumberFirebaseSms&>(value);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::sendQuickReplyShortcutMessages::ID: {
    auto& typed = static_cast<td::td_api::sendQuickReplyShortcutMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.sending_id_, observer);
    return;
}
case td::td_api::sendResoldGift::ID: {
    auto& typed = static_cast<td::td_api::sendResoldGift&>(value);
    wipe_native_value(typed.gift_name_, observer);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.price_, observer);
    return;
}
case td::td_api::sendRichMessageDraft::ID: {
    auto& typed = static_cast<td::td_api::sendRichMessageDraft&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.draft_id_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::sendTextMessageDraft::ID: {
    auto& typed = static_cast<td::td_api::sendTextMessageDraft&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.draft_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::sendWebAppCustomRequest::ID: {
    auto& typed = static_cast<td::td_api::sendWebAppCustomRequest&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.method_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::sendWebAppData::ID: {
    auto& typed = static_cast<td::td_api::sendWebAppData&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.button_text_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
case td::td_api::setAccentColor::ID: {
    auto& typed = static_cast<td::td_api::setAccentColor&>(value);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.background_custom_emoji_id_, observer);
    return;
}
case td::td_api::setAccountTtl::ID: {
    auto& typed = static_cast<td::td_api::setAccountTtl&>(value);
    wipe_native_value(typed.ttl_, observer);
    return;
}
case td::td_api::setAlarm::ID: {
    auto& typed = static_cast<td::td_api::setAlarm&>(value);
    wipe_native_value(typed.seconds_, observer);
    return;
}
case td::td_api::setApplicationVerificationToken::ID: {
    auto& typed = static_cast<td::td_api::setApplicationVerificationToken&>(value);
    wipe_native_value(typed.verification_id_, observer);
    wipe_native_value(typed.token_, observer);
    return;
}
case td::td_api::setArchiveChatListSettings::ID: {
    auto& typed = static_cast<td::td_api::setArchiveChatListSettings&>(value);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setAuthenticationEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::setAuthenticationEmailAddress&>(value);
    wipe_native_value(typed.email_address_, observer);
    return;
}
case td::td_api::setAuthenticationPhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::setAuthenticationPhoneNumber&>(value);
    wipe_native_value(typed.phone_number_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setAuthenticationPremiumPurchaseTransaction::ID: {
    auto& typed = static_cast<td::td_api::setAuthenticationPremiumPurchaseTransaction&>(value);
    wipe_native_value(typed.transaction_, observer);
    wipe_native_value(typed.is_restore_, observer);
    wipe_native_value(typed.premium_day_count_, observer);
    wipe_native_value(typed.currency_, observer);
    wipe_native_value(typed.amount_, observer);
    return;
}
case td::td_api::setAutoDownloadSettings::ID: {
    auto& typed = static_cast<td::td_api::setAutoDownloadSettings&>(value);
    wipe_native_value(typed.settings_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::setAutosaveSettings::ID: {
    auto& typed = static_cast<td::td_api::setAutosaveSettings&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setBio::ID: {
    auto& typed = static_cast<td::td_api::setBio&>(value);
    wipe_native_value(typed.bio_, observer);
    return;
}
case td::td_api::setBirthdate::ID: {
    auto& typed = static_cast<td::td_api::setBirthdate&>(value);
    wipe_native_value(typed.birthdate_, observer);
    return;
}
case td::td_api::setBotInfoDescription::ID: {
    auto& typed = static_cast<td::td_api::setBotInfoDescription&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::setBotInfoShortDescription::ID: {
    auto& typed = static_cast<td::td_api::setBotInfoShortDescription&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.short_description_, observer);
    return;
}
case td::td_api::setBotName::ID: {
    auto& typed = static_cast<td::td_api::setBotName&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::setBotProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::setBotProfilePhoto&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::setBotUpdatesStatus::ID: {
    auto& typed = static_cast<td::td_api::setBotUpdatesStatus&>(value);
    wipe_native_value(typed.pending_update_count_, observer);
    wipe_native_value(typed.error_message_, observer);
    return;
}
case td::td_api::setBusinessAccountBio::ID: {
    auto& typed = static_cast<td::td_api::setBusinessAccountBio&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.bio_, observer);
    return;
}
case td::td_api::setBusinessAccountGiftSettings::ID: {
    auto& typed = static_cast<td::td_api::setBusinessAccountGiftSettings&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setBusinessAccountName::ID: {
    auto& typed = static_cast<td::td_api::setBusinessAccountName&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    return;
}
case td::td_api::setBusinessAccountProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::setBusinessAccountProfilePhoto&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.is_public_, observer);
    return;
}
case td::td_api::setBusinessAccountUsername::ID: {
    auto& typed = static_cast<td::td_api::setBusinessAccountUsername&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::setBusinessAwayMessageSettings::ID: {
    auto& typed = static_cast<td::td_api::setBusinessAwayMessageSettings&>(value);
    wipe_native_value(typed.away_message_settings_, observer);
    return;
}
case td::td_api::setBusinessConnectedBot::ID: {
    auto& typed = static_cast<td::td_api::setBusinessConnectedBot&>(value);
    wipe_native_value(typed.bot_, observer);
    return;
}
case td::td_api::setBusinessGreetingMessageSettings::ID: {
    auto& typed = static_cast<td::td_api::setBusinessGreetingMessageSettings&>(value);
    wipe_native_value(typed.greeting_message_settings_, observer);
    return;
}
case td::td_api::setBusinessLocation::ID: {
    auto& typed = static_cast<td::td_api::setBusinessLocation&>(value);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::setBusinessMessageIsPinned::ID: {
    auto& typed = static_cast<td::td_api::setBusinessMessageIsPinned&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::setBusinessOpeningHours::ID: {
    auto& typed = static_cast<td::td_api::setBusinessOpeningHours&>(value);
    wipe_native_value(typed.opening_hours_, observer);
    return;
}
case td::td_api::setBusinessStartPage::ID: {
    auto& typed = static_cast<td::td_api::setBusinessStartPage&>(value);
    wipe_native_value(typed.start_page_, observer);
    return;
}
case td::td_api::setChatAccentColor::ID: {
    auto& typed = static_cast<td::td_api::setChatAccentColor&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.accent_color_id_, observer);
    wipe_native_value(typed.background_custom_emoji_id_, observer);
    return;
}
case td::td_api::setChatActiveStoriesList::ID: {
    auto& typed = static_cast<td::td_api::setChatActiveStoriesList&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_list_, observer);
    return;
}
case td::td_api::setChatAffiliateProgram::ID: {
    auto& typed = static_cast<td::td_api::setChatAffiliateProgram&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.parameters_, observer);
    return;
}
case td::td_api::setChatAvailableReactions::ID: {
    auto& typed = static_cast<td::td_api::setChatAvailableReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.available_reactions_, observer);
    return;
}
case td::td_api::setChatBackground::ID: {
    auto& typed = static_cast<td::td_api::setChatBackground&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.dark_theme_dimming_, observer);
    wipe_native_value(typed.only_for_self_, observer);
    return;
}
case td::td_api::setChatClientData::ID: {
    auto& typed = static_cast<td::td_api::setChatClientData&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.client_data_, observer);
    return;
}
case td::td_api::setChatDescription::ID: {
    auto& typed = static_cast<td::td_api::setChatDescription&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.description_, observer);
    return;
}
case td::td_api::setChatDirectMessagesGroup::ID: {
    auto& typed = static_cast<td::td_api::setChatDirectMessagesGroup&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_enabled_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::setChatDiscussionGroup::ID: {
    auto& typed = static_cast<td::td_api::setChatDiscussionGroup&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.discussion_chat_id_, observer);
    return;
}
case td::td_api::setChatDraftMessage::ID: {
    auto& typed = static_cast<td::td_api::setChatDraftMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.draft_message_, observer);
    return;
}
case td::td_api::setChatEmojiStatus::ID: {
    auto& typed = static_cast<td::td_api::setChatEmojiStatus&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.emoji_status_, observer);
    return;
}
case td::td_api::setChatLocation::ID: {
    auto& typed = static_cast<td::td_api::setChatLocation&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.location_, observer);
    return;
}
case td::td_api::setChatMemberStatus::ID: {
    auto& typed = static_cast<td::td_api::setChatMemberStatus&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.member_id_, observer);
    wipe_native_value(typed.status_, observer);
    return;
}
case td::td_api::setChatMemberTag::ID: {
    auto& typed = static_cast<td::td_api::setChatMemberTag&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.tag_, observer);
    return;
}
case td::td_api::setChatMessageAutoDeleteTime::ID: {
    auto& typed = static_cast<td::td_api::setChatMessageAutoDeleteTime&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    return;
}
case td::td_api::setChatMessageSender::ID: {
    auto& typed = static_cast<td::td_api::setChatMessageSender&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_sender_id_, observer);
    return;
}
case td::td_api::setChatNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::setChatNotificationSettings&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::setChatPaidMessageStarCount::ID: {
    auto& typed = static_cast<td::td_api::setChatPaidMessageStarCount&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::setChatPermissions::ID: {
    auto& typed = static_cast<td::td_api::setChatPermissions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.permissions_, observer);
    return;
}
case td::td_api::setChatPhoto::ID: {
    auto& typed = static_cast<td::td_api::setChatPhoto&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::setChatPinnedStories::ID: {
    auto& typed = static_cast<td::td_api::setChatPinnedStories&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_ids_, observer);
    return;
}
case td::td_api::setChatProfileAccentColor::ID: {
    auto& typed = static_cast<td::td_api::setChatProfileAccentColor&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.profile_accent_color_id_, observer);
    wipe_native_value(typed.profile_background_custom_emoji_id_, observer);
    return;
}
case td::td_api::setChatSlowModeDelay::ID: {
    auto& typed = static_cast<td::td_api::setChatSlowModeDelay&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.slow_mode_delay_, observer);
    return;
}
case td::td_api::setChatTheme::ID: {
    auto& typed = static_cast<td::td_api::setChatTheme&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.theme_, observer);
    return;
}
case td::td_api::setChatTitle::ID: {
    auto& typed = static_cast<td::td_api::setChatTitle&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::setCloseFriends::ID: {
    auto& typed = static_cast<td::td_api::setCloseFriends&>(value);
    wipe_native_value(typed.user_ids_, observer);
    return;
}
case td::td_api::setCommands::ID: {
    auto& typed = static_cast<td::td_api::setCommands&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.language_code_, observer);
    wipe_native_value(typed.commands_, observer);
    return;
}
case td::td_api::setCustomEmojiStickerSetThumbnail::ID: {
    auto& typed = static_cast<td::td_api::setCustomEmojiStickerSetThumbnail&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.custom_emoji_id_, observer);
    return;
}
case td::td_api::setCustomLanguagePack::ID: {
    auto& typed = static_cast<td::td_api::setCustomLanguagePack&>(value);
    wipe_native_value(typed.info_, observer);
    wipe_native_value(typed.strings_, observer);
    return;
}
case td::td_api::setCustomLanguagePackString::ID: {
    auto& typed = static_cast<td::td_api::setCustomLanguagePackString&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    wipe_native_value(typed.new_string_, observer);
    return;
}
case td::td_api::setDatabaseEncryptionKey::ID: {
    auto& typed = static_cast<td::td_api::setDatabaseEncryptionKey&>(value);
    wipe_native_value(typed.new_encryption_key_, observer);
    return;
}
case td::td_api::setDefaultBackground::ID: {
    auto& typed = static_cast<td::td_api::setDefaultBackground&>(value);
    wipe_native_value(typed.background_, observer);
    wipe_native_value(typed.type_, observer);
    wipe_native_value(typed.for_dark_theme_, observer);
    return;
}
case td::td_api::setDefaultChannelAdministratorRights::ID: {
    auto& typed = static_cast<td::td_api::setDefaultChannelAdministratorRights&>(value);
    wipe_native_value(typed.default_channel_administrator_rights_, observer);
    return;
}
case td::td_api::setDefaultGroupAdministratorRights::ID: {
    auto& typed = static_cast<td::td_api::setDefaultGroupAdministratorRights&>(value);
    wipe_native_value(typed.default_group_administrator_rights_, observer);
    return;
}
case td::td_api::setDefaultMessageAutoDeleteTime::ID: {
    auto& typed = static_cast<td::td_api::setDefaultMessageAutoDeleteTime&>(value);
    wipe_native_value(typed.message_auto_delete_time_, observer);
    return;
}
case td::td_api::setDefaultReactionType::ID: {
    auto& typed = static_cast<td::td_api::setDefaultReactionType&>(value);
    wipe_native_value(typed.reaction_type_, observer);
    return;
}
case td::td_api::setDirectMessagesChatTopicIsMarkedAsUnread::ID: {
    auto& typed = static_cast<td::td_api::setDirectMessagesChatTopicIsMarkedAsUnread&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.is_marked_as_unread_, observer);
    return;
}
case td::td_api::setEmojiStatus::ID: {
    auto& typed = static_cast<td::td_api::setEmojiStatus&>(value);
    wipe_native_value(typed.emoji_status_, observer);
    return;
}
case td::td_api::setFileGenerationProgress::ID: {
    auto& typed = static_cast<td::td_api::setFileGenerationProgress&>(value);
    wipe_native_value(typed.generation_id_, observer);
    wipe_native_value(typed.expected_size_, observer);
    wipe_native_value(typed.local_prefix_size_, observer);
    return;
}
case td::td_api::setForumTopicNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::setForumTopicNotificationSettings&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::setGameScore::ID: {
    auto& typed = static_cast<td::td_api::setGameScore&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.edit_message_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.score_, observer);
    wipe_native_value(typed.force_, observer);
    return;
}
case td::td_api::setGiftCollectionName::ID: {
    auto& typed = static_cast<td::td_api::setGiftCollectionName&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.collection_id_, observer);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::setGiftResalePrice::ID: {
    auto& typed = static_cast<td::td_api::setGiftResalePrice&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.price_, observer);
    return;
}
case td::td_api::setGiftSettings::ID: {
    auto& typed = static_cast<td::td_api::setGiftSettings&>(value);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setGroupCallPaidMessageStarCount::ID: {
    auto& typed = static_cast<td::td_api::setGroupCallPaidMessageStarCount&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::setGroupCallParticipantIsSpeaking::ID: {
    auto& typed = static_cast<td::td_api::setGroupCallParticipantIsSpeaking&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.audio_source_, observer);
    wipe_native_value(typed.is_speaking_, observer);
    return;
}
case td::td_api::setGroupCallParticipantVolumeLevel::ID: {
    auto& typed = static_cast<td::td_api::setGroupCallParticipantVolumeLevel&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.volume_level_, observer);
    return;
}
case td::td_api::setInactiveSessionTtl::ID: {
    auto& typed = static_cast<td::td_api::setInactiveSessionTtl&>(value);
    wipe_native_value(typed.inactive_session_ttl_days_, observer);
    return;
}
case td::td_api::setInlineGameScore::ID: {
    auto& typed = static_cast<td::td_api::setInlineGameScore&>(value);
    wipe_native_value(typed.inline_message_id_, observer);
    wipe_native_value(typed.edit_message_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.score_, observer);
    wipe_native_value(typed.force_, observer);
    return;
}
case td::td_api::setLiveStoryMessageSender::ID: {
    auto& typed = static_cast<td::td_api::setLiveStoryMessageSender&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.message_sender_id_, observer);
    return;
}
case td::td_api::setLogStream::ID: {
    auto& typed = static_cast<td::td_api::setLogStream&>(value);
    wipe_native_value(typed.log_stream_, observer);
    return;
}
case td::td_api::setLogTagVerbosityLevel::ID: {
    auto& typed = static_cast<td::td_api::setLogTagVerbosityLevel&>(value);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.new_verbosity_level_, observer);
    return;
}
case td::td_api::setLogVerbosityLevel::ID: {
    auto& typed = static_cast<td::td_api::setLogVerbosityLevel&>(value);
    wipe_native_value(typed.new_verbosity_level_, observer);
    return;
}
case td::td_api::setLoginEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::setLoginEmailAddress&>(value);
    wipe_native_value(typed.new_login_email_address_, observer);
    return;
}
case td::td_api::setMainProfileTab::ID: {
    auto& typed = static_cast<td::td_api::setMainProfileTab&>(value);
    wipe_native_value(typed.main_profile_tab_, observer);
    return;
}
case td::td_api::setManagedBotAccessSettings::ID: {
    auto& typed = static_cast<td::td_api::setManagedBotAccessSettings&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setMenuButton::ID: {
    auto& typed = static_cast<td::td_api::setMenuButton&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.menu_button_, observer);
    return;
}
case td::td_api::setMessageFactCheck::ID: {
    auto& typed = static_cast<td::td_api::setMessageFactCheck&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.text_, observer);
    return;
}
case td::td_api::setMessageReactions::ID: {
    auto& typed = static_cast<td::td_api::setMessageReactions&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reaction_types_, observer);
    wipe_native_value(typed.is_big_, observer);
    return;
}
case td::td_api::setMessageSenderBlockList::ID: {
    auto& typed = static_cast<td::td_api::setMessageSenderBlockList&>(value);
    wipe_native_value(typed.sender_id_, observer);
    wipe_native_value(typed.block_list_, observer);
    return;
}
case td::td_api::setMessageSenderBotVerification::ID: {
    auto& typed = static_cast<td::td_api::setMessageSenderBotVerification&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.verified_id_, observer);
    wipe_native_value(typed.custom_description_, observer);
    return;
}
case td::td_api::setName::ID: {
    auto& typed = static_cast<td::td_api::setName&>(value);
    wipe_native_value(typed.first_name_, observer);
    wipe_native_value(typed.last_name_, observer);
    return;
}
case td::td_api::setNetworkType::ID: {
    auto& typed = static_cast<td::td_api::setNetworkType&>(value);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::setNewChatPrivacySettings::ID: {
    auto& typed = static_cast<td::td_api::setNewChatPrivacySettings&>(value);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setOption::ID: {
    auto& typed = static_cast<td::td_api::setOption&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.value_, observer);
    return;
}
case td::td_api::setPaidMessageReactionType::ID: {
    auto& typed = static_cast<td::td_api::setPaidMessageReactionType&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.type_, observer);
    return;
}
case td::td_api::setPassportElement::ID: {
    auto& typed = static_cast<td::td_api::setPassportElement&>(value);
    wipe_native_value(typed.element_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::setPassportElementErrors::ID: {
    auto& typed = static_cast<td::td_api::setPassportElementErrors&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.errors_, observer);
    return;
}
case td::td_api::setPassword::ID: {
    auto& typed = static_cast<td::td_api::setPassword&>(value);
    wipe_native_value(typed.old_password_, observer);
    wipe_native_value(typed.new_password_, observer);
    wipe_native_value(typed.new_hint_, observer);
    wipe_native_value(typed.set_recovery_email_address_, observer);
    wipe_native_value(typed.new_recovery_email_address_, observer);
    return;
}
case td::td_api::setPersonalChat::ID: {
    auto& typed = static_cast<td::td_api::setPersonalChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::setPinnedChats::ID: {
    auto& typed = static_cast<td::td_api::setPinnedChats&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.chat_ids_, observer);
    return;
}
case td::td_api::setPinnedForumTopics::ID: {
    auto& typed = static_cast<td::td_api::setPinnedForumTopics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_ids_, observer);
    return;
}
case td::td_api::setPinnedGifts::ID: {
    auto& typed = static_cast<td::td_api::setPinnedGifts&>(value);
    wipe_native_value(typed.owner_id_, observer);
    wipe_native_value(typed.received_gift_ids_, observer);
    return;
}
case td::td_api::setPinnedSavedMessagesTopics::ID: {
    auto& typed = static_cast<td::td_api::setPinnedSavedMessagesTopics&>(value);
    wipe_native_value(typed.saved_messages_topic_ids_, observer);
    return;
}
case td::td_api::setPollAnswer::ID: {
    auto& typed = static_cast<td::td_api::setPollAnswer&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.option_ids_, observer);
    return;
}
case td::td_api::setProfileAccentColor::ID: {
    auto& typed = static_cast<td::td_api::setProfileAccentColor&>(value);
    wipe_native_value(typed.profile_accent_color_id_, observer);
    wipe_native_value(typed.profile_background_custom_emoji_id_, observer);
    return;
}
case td::td_api::setProfileAudioPosition::ID: {
    auto& typed = static_cast<td::td_api::setProfileAudioPosition&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.after_file_id_, observer);
    return;
}
case td::td_api::setProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::setProfilePhoto&>(value);
    wipe_native_value(typed.photo_, observer);
    wipe_native_value(typed.is_public_, observer);
    return;
}
case td::td_api::setQuickReplyShortcutName::ID: {
    auto& typed = static_cast<td::td_api::setQuickReplyShortcutName&>(value);
    wipe_native_value(typed.shortcut_id_, observer);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::setReactionNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::setReactionNotificationSettings&>(value);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::setReadDatePrivacySettings::ID: {
    auto& typed = static_cast<td::td_api::setReadDatePrivacySettings&>(value);
    wipe_native_value(typed.settings_, observer);
    return;
}
case td::td_api::setRecoveryEmailAddress::ID: {
    auto& typed = static_cast<td::td_api::setRecoveryEmailAddress&>(value);
    wipe_native_value(typed.password_, observer);
    wipe_native_value(typed.new_recovery_email_address_, observer);
    return;
}
case td::td_api::setSavedMessagesTagLabel::ID: {
    auto& typed = static_cast<td::td_api::setSavedMessagesTagLabel&>(value);
    wipe_native_value(typed.tag_, observer);
    wipe_native_value(typed.label_, observer);
    return;
}
case td::td_api::setScopeNotificationSettings::ID: {
    auto& typed = static_cast<td::td_api::setScopeNotificationSettings&>(value);
    wipe_native_value(typed.scope_, observer);
    wipe_native_value(typed.notification_settings_, observer);
    return;
}
case td::td_api::setStickerEmojis::ID: {
    auto& typed = static_cast<td::td_api::setStickerEmojis&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.emojis_, observer);
    return;
}
case td::td_api::setStickerKeywords::ID: {
    auto& typed = static_cast<td::td_api::setStickerKeywords&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.keywords_, observer);
    return;
}
case td::td_api::setStickerMaskPosition::ID: {
    auto& typed = static_cast<td::td_api::setStickerMaskPosition&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.mask_position_, observer);
    return;
}
case td::td_api::setStickerPositionInSet::ID: {
    auto& typed = static_cast<td::td_api::setStickerPositionInSet&>(value);
    wipe_native_value(typed.sticker_, observer);
    wipe_native_value(typed.position_, observer);
    return;
}
case td::td_api::setStickerSetThumbnail::ID: {
    auto& typed = static_cast<td::td_api::setStickerSetThumbnail&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.thumbnail_, observer);
    wipe_native_value(typed.format_, observer);
    return;
}
case td::td_api::setStickerSetTitle::ID: {
    auto& typed = static_cast<td::td_api::setStickerSetTitle&>(value);
    wipe_native_value(typed.name_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::setStoryAlbumName::ID: {
    auto& typed = static_cast<td::td_api::setStoryAlbumName&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.story_album_id_, observer);
    wipe_native_value(typed.name_, observer);
    return;
}
case td::td_api::setStoryPrivacySettings::ID: {
    auto& typed = static_cast<td::td_api::setStoryPrivacySettings&>(value);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.privacy_settings_, observer);
    return;
}
case td::td_api::setStoryReaction::ID: {
    auto& typed = static_cast<td::td_api::setStoryReaction&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.reaction_type_, observer);
    wipe_native_value(typed.update_recent_reactions_, observer);
    return;
}
case td::td_api::setSupergroupCustomEmojiStickerSet::ID: {
    auto& typed = static_cast<td::td_api::setSupergroupCustomEmojiStickerSet&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.custom_emoji_sticker_set_id_, observer);
    return;
}
case td::td_api::setSupergroupMainProfileTab::ID: {
    auto& typed = static_cast<td::td_api::setSupergroupMainProfileTab&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.main_profile_tab_, observer);
    return;
}
case td::td_api::setSupergroupStickerSet::ID: {
    auto& typed = static_cast<td::td_api::setSupergroupStickerSet&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.sticker_set_id_, observer);
    return;
}
case td::td_api::setSupergroupUnrestrictBoostCount::ID: {
    auto& typed = static_cast<td::td_api::setSupergroupUnrestrictBoostCount&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.unrestrict_boost_count_, observer);
    return;
}
case td::td_api::setSupergroupUsername::ID: {
    auto& typed = static_cast<td::td_api::setSupergroupUsername&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::setTdlibParameters::ID: {
    auto& typed = static_cast<td::td_api::setTdlibParameters&>(value);
    wipe_native_value(typed.use_test_dc_, observer);
    wipe_native_value(typed.database_directory_, observer);
    wipe_native_value(typed.files_directory_, observer);
    wipe_native_value(typed.database_encryption_key_, observer);
    wipe_native_value(typed.use_file_database_, observer);
    wipe_native_value(typed.use_chat_info_database_, observer);
    wipe_native_value(typed.use_message_database_, observer);
    wipe_native_value(typed.use_secret_chats_, observer);
    wipe_native_value(typed.api_id_, observer);
    wipe_native_value(typed.api_hash_, observer);
    wipe_native_value(typed.system_language_code_, observer);
    wipe_native_value(typed.device_model_, observer);
    wipe_native_value(typed.system_version_, observer);
    wipe_native_value(typed.application_version_, observer);
    return;
}
case td::td_api::setUpgradedGiftColors::ID: {
    auto& typed = static_cast<td::td_api::setUpgradedGiftColors&>(value);
    wipe_native_value(typed.upgraded_gift_colors_id_, observer);
    return;
}
case td::td_api::setUserEmojiStatus::ID: {
    auto& typed = static_cast<td::td_api::setUserEmojiStatus&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.emoji_status_, observer);
    return;
}
case td::td_api::setUserNote::ID: {
    auto& typed = static_cast<td::td_api::setUserNote&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.note_, observer);
    return;
}
case td::td_api::setUserPersonalProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::setUserPersonalProfilePhoto&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::setUserPrivacySettingRules::ID: {
    auto& typed = static_cast<td::td_api::setUserPrivacySettingRules&>(value);
    wipe_native_value(typed.setting_, observer);
    wipe_native_value(typed.rules_, observer);
    return;
}
case td::td_api::setUserSupportInfo::ID: {
    auto& typed = static_cast<td::td_api::setUserSupportInfo&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.message_, observer);
    return;
}
case td::td_api::setUsername::ID: {
    auto& typed = static_cast<td::td_api::setUsername&>(value);
    wipe_native_value(typed.username_, observer);
    return;
}
case td::td_api::setVideoChatDefaultParticipant::ID: {
    auto& typed = static_cast<td::td_api::setVideoChatDefaultParticipant&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.default_participant_id_, observer);
    return;
}
case td::td_api::setVideoChatTitle::ID: {
    auto& typed = static_cast<td::td_api::setVideoChatTitle&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.title_, observer);
    return;
}
case td::td_api::shareChatWithBot::ID: {
    auto& typed = static_cast<td::td_api::shareChatWithBot&>(value);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.button_id_, observer);
    wipe_native_value(typed.shared_chat_id_, observer);
    wipe_native_value(typed.only_check_, observer);
    return;
}
case td::td_api::sharePhoneNumber::ID: {
    auto& typed = static_cast<td::td_api::sharePhoneNumber&>(value);
    wipe_native_value(typed.user_id_, observer);
    return;
}
case td::td_api::shareUsersWithBot::ID: {
    auto& typed = static_cast<td::td_api::shareUsersWithBot&>(value);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.button_id_, observer);
    wipe_native_value(typed.shared_user_ids_, observer);
    wipe_native_value(typed.only_check_, observer);
    return;
}
case td::td_api::startGroupCallRecording::ID: {
    auto& typed = static_cast<td::td_api::startGroupCallRecording&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.title_, observer);
    wipe_native_value(typed.record_video_, observer);
    wipe_native_value(typed.use_portrait_orientation_, observer);
    return;
}
case td::td_api::startGroupCallScreenSharing::ID: {
    auto& typed = static_cast<td::td_api::startGroupCallScreenSharing&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.audio_source_id_, observer);
    wipe_native_value(typed.payload_, observer);
    return;
}
case td::td_api::startLiveStory::ID: {
    auto& typed = static_cast<td::td_api::startLiveStory&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.privacy_settings_, observer);
    wipe_native_value(typed.protect_content_, observer);
    wipe_native_value(typed.is_rtmp_stream_, observer);
    wipe_native_value(typed.enable_messages_, observer);
    wipe_native_value(typed.paid_message_star_count_, observer);
    return;
}
case td::td_api::startScheduledVideoChat::ID: {
    auto& typed = static_cast<td::td_api::startScheduledVideoChat&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    return;
}
case td::td_api::stopBusinessPoll::ID: {
    auto& typed = static_cast<td::td_api::stopBusinessPoll&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::stopPoll::ID: {
    auto& typed = static_cast<td::td_api::stopPoll&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.reply_markup_, observer);
    return;
}
case td::td_api::suggestUserBirthdate::ID: {
    auto& typed = static_cast<td::td_api::suggestUserBirthdate&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.birthdate_, observer);
    return;
}
case td::td_api::suggestUserProfilePhoto::ID: {
    auto& typed = static_cast<td::td_api::suggestUserProfilePhoto&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.photo_, observer);
    return;
}
case td::td_api::summarizeMessage::ID: {
    auto& typed = static_cast<td::td_api::summarizeMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.translate_to_language_code_, observer);
    wipe_native_value(typed.tone_, observer);
    return;
}
case td::td_api::synchronizeLanguagePack::ID: {
    auto& typed = static_cast<td::td_api::synchronizeLanguagePack&>(value);
    wipe_native_value(typed.language_pack_id_, observer);
    return;
}
case td::td_api::terminateAllOtherSessions::ID: {
    return;
}
case td::td_api::terminateSession::ID: {
    auto& typed = static_cast<td::td_api::terminateSession&>(value);
    wipe_native_value(typed.session_id_, observer);
    return;
}
case td::td_api::testCallBytes::ID: {
    auto& typed = static_cast<td::td_api::testCallBytes&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testCallEmpty::ID: {
    return;
}
case td::td_api::testCallString::ID: {
    auto& typed = static_cast<td::td_api::testCallString&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testCallVectorInt::ID: {
    auto& typed = static_cast<td::td_api::testCallVectorInt&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testCallVectorIntObject::ID: {
    auto& typed = static_cast<td::td_api::testCallVectorIntObject&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testCallVectorString::ID: {
    auto& typed = static_cast<td::td_api::testCallVectorString&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testCallVectorStringObject::ID: {
    auto& typed = static_cast<td::td_api::testCallVectorStringObject&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testGetDifference::ID: {
    return;
}
case td::td_api::testNetwork::ID: {
    return;
}
case td::td_api::testProxy::ID: {
    auto& typed = static_cast<td::td_api::testProxy&>(value);
    wipe_native_value(typed.proxy_, observer);
    wipe_native_value(typed.dc_id_, observer);
    wipe_native_value(typed.timeout_, observer);
    return;
}
case td::td_api::testReturnError::ID: {
    auto& typed = static_cast<td::td_api::testReturnError&>(value);
    wipe_native_value(typed.error_, observer);
    return;
}
case td::td_api::testSquareInt::ID: {
    auto& typed = static_cast<td::td_api::testSquareInt&>(value);
    wipe_native_value(typed.x_, observer);
    return;
}
case td::td_api::testUseUpdate::ID: {
    return;
}
case td::td_api::toggleAllDownloadsArePaused::ID: {
    auto& typed = static_cast<td::td_api::toggleAllDownloadsArePaused&>(value);
    wipe_native_value(typed.are_paused_, observer);
    return;
}
case td::td_api::toggleBotCanManageEmojiStatus::ID: {
    auto& typed = static_cast<td::td_api::toggleBotCanManageEmojiStatus&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.can_manage_emoji_status_, observer);
    return;
}
case td::td_api::toggleBotIsAddedToAttachmentMenu::ID: {
    auto& typed = static_cast<td::td_api::toggleBotIsAddedToAttachmentMenu&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.is_added_, observer);
    wipe_native_value(typed.allow_write_access_, observer);
    return;
}
case td::td_api::toggleBotUsernameIsActive::ID: {
    auto& typed = static_cast<td::td_api::toggleBotUsernameIsActive&>(value);
    wipe_native_value(typed.bot_user_id_, observer);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.is_active_, observer);
    return;
}
case td::td_api::toggleBusinessConnectedBotChatIsPaused::ID: {
    auto& typed = static_cast<td::td_api::toggleBusinessConnectedBotChatIsPaused&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_paused_, observer);
    return;
}
case td::td_api::toggleChatDefaultDisableNotification::ID: {
    auto& typed = static_cast<td::td_api::toggleChatDefaultDisableNotification&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.default_disable_notification_, observer);
    return;
}
case td::td_api::toggleChatFolderTags::ID: {
    auto& typed = static_cast<td::td_api::toggleChatFolderTags&>(value);
    wipe_native_value(typed.are_tags_enabled_, observer);
    return;
}
case td::td_api::toggleChatGiftNotifications::ID: {
    auto& typed = static_cast<td::td_api::toggleChatGiftNotifications&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.are_enabled_, observer);
    return;
}
case td::td_api::toggleChatHasProtectedContent::ID: {
    auto& typed = static_cast<td::td_api::toggleChatHasProtectedContent&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.has_protected_content_, observer);
    return;
}
case td::td_api::toggleChatIsMarkedAsUnread::ID: {
    auto& typed = static_cast<td::td_api::toggleChatIsMarkedAsUnread&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_marked_as_unread_, observer);
    return;
}
case td::td_api::toggleChatIsPinned::ID: {
    auto& typed = static_cast<td::td_api::toggleChatIsPinned&>(value);
    wipe_native_value(typed.chat_list_, observer);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::toggleChatIsTranslatable::ID: {
    auto& typed = static_cast<td::td_api::toggleChatIsTranslatable&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_translatable_, observer);
    return;
}
case td::td_api::toggleChatViewAsTopics::ID: {
    auto& typed = static_cast<td::td_api::toggleChatViewAsTopics&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.view_as_topics_, observer);
    return;
}
case td::td_api::toggleDirectMessagesChatTopicCanSendUnpaidMessages::ID: {
    auto& typed = static_cast<td::td_api::toggleDirectMessagesChatTopicCanSendUnpaidMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    wipe_native_value(typed.can_send_unpaid_messages_, observer);
    wipe_native_value(typed.refund_payments_, observer);
    return;
}
case td::td_api::toggleDownloadIsPaused::ID: {
    auto& typed = static_cast<td::td_api::toggleDownloadIsPaused&>(value);
    wipe_native_value(typed.file_id_, observer);
    wipe_native_value(typed.is_paused_, observer);
    return;
}
case td::td_api::toggleForumTopicIsClosed::ID: {
    auto& typed = static_cast<td::td_api::toggleForumTopicIsClosed&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.is_closed_, observer);
    return;
}
case td::td_api::toggleForumTopicIsPinned::ID: {
    auto& typed = static_cast<td::td_api::toggleForumTopicIsPinned&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::toggleGeneralForumTopicIsHidden::ID: {
    auto& typed = static_cast<td::td_api::toggleGeneralForumTopicIsHidden&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.is_hidden_, observer);
    return;
}
case td::td_api::toggleGiftIsSaved::ID: {
    auto& typed = static_cast<td::td_api::toggleGiftIsSaved&>(value);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.is_saved_, observer);
    return;
}
case td::td_api::toggleGroupCallAreMessagesAllowed::ID: {
    auto& typed = static_cast<td::td_api::toggleGroupCallAreMessagesAllowed&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.are_messages_allowed_, observer);
    return;
}
case td::td_api::toggleGroupCallIsMyVideoEnabled::ID: {
    auto& typed = static_cast<td::td_api::toggleGroupCallIsMyVideoEnabled&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.is_my_video_enabled_, observer);
    return;
}
case td::td_api::toggleGroupCallIsMyVideoPaused::ID: {
    auto& typed = static_cast<td::td_api::toggleGroupCallIsMyVideoPaused&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.is_my_video_paused_, observer);
    return;
}
case td::td_api::toggleGroupCallParticipantIsHandRaised::ID: {
    auto& typed = static_cast<td::td_api::toggleGroupCallParticipantIsHandRaised&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.is_hand_raised_, observer);
    return;
}
case td::td_api::toggleGroupCallParticipantIsMuted::ID: {
    auto& typed = static_cast<td::td_api::toggleGroupCallParticipantIsMuted&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.participant_id_, observer);
    wipe_native_value(typed.is_muted_, observer);
    return;
}
case td::td_api::toggleGroupCallScreenSharingIsPaused::ID: {
    auto& typed = static_cast<td::td_api::toggleGroupCallScreenSharingIsPaused&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.is_paused_, observer);
    return;
}
case td::td_api::toggleHasSponsoredMessagesEnabled::ID: {
    auto& typed = static_cast<td::td_api::toggleHasSponsoredMessagesEnabled&>(value);
    wipe_native_value(typed.has_sponsored_messages_enabled_, observer);
    return;
}
case td::td_api::toggleSavedMessagesTopicIsPinned::ID: {
    auto& typed = static_cast<td::td_api::toggleSavedMessagesTopicIsPinned&>(value);
    wipe_native_value(typed.saved_messages_topic_id_, observer);
    wipe_native_value(typed.is_pinned_, observer);
    return;
}
case td::td_api::toggleSessionCanAcceptCalls::ID: {
    auto& typed = static_cast<td::td_api::toggleSessionCanAcceptCalls&>(value);
    wipe_native_value(typed.session_id_, observer);
    wipe_native_value(typed.can_accept_calls_, observer);
    return;
}
case td::td_api::toggleSessionCanAcceptSecretChats::ID: {
    auto& typed = static_cast<td::td_api::toggleSessionCanAcceptSecretChats&>(value);
    wipe_native_value(typed.session_id_, observer);
    wipe_native_value(typed.can_accept_secret_chats_, observer);
    return;
}
case td::td_api::toggleStoryIsPostedToChatPage::ID: {
    auto& typed = static_cast<td::td_api::toggleStoryIsPostedToChatPage&>(value);
    wipe_native_value(typed.story_poster_chat_id_, observer);
    wipe_native_value(typed.story_id_, observer);
    wipe_native_value(typed.is_posted_to_chat_page_, observer);
    return;
}
case td::td_api::toggleSupergroupCanHaveSponsoredMessages::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupCanHaveSponsoredMessages&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.can_have_sponsored_messages_, observer);
    return;
}
case td::td_api::toggleSupergroupHasAggressiveAntiSpamEnabled::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupHasAggressiveAntiSpamEnabled&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.has_aggressive_anti_spam_enabled_, observer);
    return;
}
case td::td_api::toggleSupergroupHasAutomaticTranslation::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupHasAutomaticTranslation&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.has_automatic_translation_, observer);
    return;
}
case td::td_api::toggleSupergroupHasHiddenMembers::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupHasHiddenMembers&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.has_hidden_members_, observer);
    return;
}
case td::td_api::toggleSupergroupIsAllHistoryAvailable::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupIsAllHistoryAvailable&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.is_all_history_available_, observer);
    return;
}
case td::td_api::toggleSupergroupIsBroadcastGroup::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupIsBroadcastGroup&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    return;
}
case td::td_api::toggleSupergroupIsForum::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupIsForum&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.is_forum_, observer);
    wipe_native_value(typed.has_forum_tabs_, observer);
    return;
}
case td::td_api::toggleSupergroupJoinByRequest::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupJoinByRequest&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.join_by_request_, observer);
    wipe_native_value(typed.guard_bot_user_id_, observer);
    wipe_native_value(typed.apply_to_invite_links_, observer);
    return;
}
case td::td_api::toggleSupergroupJoinToSendMessages::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupJoinToSendMessages&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.join_to_send_messages_, observer);
    return;
}
case td::td_api::toggleSupergroupSignMessages::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupSignMessages&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.sign_messages_, observer);
    wipe_native_value(typed.show_message_sender_, observer);
    return;
}
case td::td_api::toggleSupergroupUsernameIsActive::ID: {
    auto& typed = static_cast<td::td_api::toggleSupergroupUsernameIsActive&>(value);
    wipe_native_value(typed.supergroup_id_, observer);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.is_active_, observer);
    return;
}
case td::td_api::toggleUsernameIsActive::ID: {
    auto& typed = static_cast<td::td_api::toggleUsernameIsActive&>(value);
    wipe_native_value(typed.username_, observer);
    wipe_native_value(typed.is_active_, observer);
    return;
}
case td::td_api::toggleVideoChatEnabledStartNotification::ID: {
    auto& typed = static_cast<td::td_api::toggleVideoChatEnabledStartNotification&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.enabled_start_notification_, observer);
    return;
}
case td::td_api::toggleVideoChatMuteNewParticipants::ID: {
    auto& typed = static_cast<td::td_api::toggleVideoChatMuteNewParticipants&>(value);
    wipe_native_value(typed.group_call_id_, observer);
    wipe_native_value(typed.mute_new_participants_, observer);
    return;
}
case td::td_api::transferBusinessAccountStars::ID: {
    auto& typed = static_cast<td::td_api::transferBusinessAccountStars&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::transferChatOwnership::ID: {
    auto& typed = static_cast<td::td_api::transferChatOwnership&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.password_, observer);
    return;
}
case td::td_api::transferGift::ID: {
    auto& typed = static_cast<td::td_api::transferGift&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.new_owner_id_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::translateMessageText::ID: {
    auto& typed = static_cast<td::td_api::translateMessageText&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    wipe_native_value(typed.to_language_code_, observer);
    wipe_native_value(typed.tone_, observer);
    return;
}
case td::td_api::translateText::ID: {
    auto& typed = static_cast<td::td_api::translateText&>(value);
    wipe_native_value(typed.text_, observer);
    wipe_native_value(typed.to_language_code_, observer);
    wipe_native_value(typed.tone_, observer);
    return;
}
case td::td_api::unpinAllChatMessages::ID: {
    auto& typed = static_cast<td::td_api::unpinAllChatMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::unpinAllDirectMessagesChatTopicMessages::ID: {
    auto& typed = static_cast<td::td_api::unpinAllDirectMessagesChatTopicMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.topic_id_, observer);
    return;
}
case td::td_api::unpinAllForumTopicMessages::ID: {
    auto& typed = static_cast<td::td_api::unpinAllForumTopicMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.forum_topic_id_, observer);
    return;
}
case td::td_api::unpinChatMessage::ID: {
    auto& typed = static_cast<td::td_api::unpinChatMessage&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_id_, observer);
    return;
}
case td::td_api::upgradeBasicGroupChatToSupergroupChat::ID: {
    auto& typed = static_cast<td::td_api::upgradeBasicGroupChatToSupergroupChat&>(value);
    wipe_native_value(typed.chat_id_, observer);
    return;
}
case td::td_api::upgradeGift::ID: {
    auto& typed = static_cast<td::td_api::upgradeGift&>(value);
    wipe_native_value(typed.business_connection_id_, observer);
    wipe_native_value(typed.received_gift_id_, observer);
    wipe_native_value(typed.keep_original_details_, observer);
    wipe_native_value(typed.star_count_, observer);
    return;
}
case td::td_api::uploadStickerFile::ID: {
    auto& typed = static_cast<td::td_api::uploadStickerFile&>(value);
    wipe_native_value(typed.user_id_, observer);
    wipe_native_value(typed.sticker_format_, observer);
    wipe_native_value(typed.sticker_, observer);
    return;
}
case td::td_api::validateOrderInfo::ID: {
    auto& typed = static_cast<td::td_api::validateOrderInfo&>(value);
    wipe_native_value(typed.input_invoice_, observer);
    wipe_native_value(typed.order_info_, observer);
    wipe_native_value(typed.allow_save_, observer);
    return;
}
case td::td_api::viewMessages::ID: {
    auto& typed = static_cast<td::td_api::viewMessages&>(value);
    wipe_native_value(typed.chat_id_, observer);
    wipe_native_value(typed.message_ids_, observer);
    wipe_native_value(typed.source_, observer);
    wipe_native_value(typed.force_read_, observer);
    return;
}
case td::td_api::viewPremiumFeature::ID: {
    auto& typed = static_cast<td::td_api::viewPremiumFeature&>(value);
    wipe_native_value(typed.feature_, observer);
    return;
}
case td::td_api::viewSponsoredChat::ID: {
    auto& typed = static_cast<td::td_api::viewSponsoredChat&>(value);
    wipe_native_value(typed.sponsored_chat_unique_id_, observer);
    return;
}
case td::td_api::viewTrendingStickerSets::ID: {
    auto& typed = static_cast<td::td_api::viewTrendingStickerSets&>(value);
    wipe_native_value(typed.sticker_set_ids_, observer);
    return;
}
case td::td_api::viewVideoMessageAdvertisement::ID: {
    auto& typed = static_cast<td::td_api::viewVideoMessageAdvertisement&>(value);
    wipe_native_value(typed.advertisement_unique_id_, observer);
    return;
}
case td::td_api::writeGeneratedFilePart::ID: {
    auto& typed = static_cast<td::td_api::writeGeneratedFilePart&>(value);
    wipe_native_value(typed.generation_id_, observer);
    wipe_native_value(typed.offset_, observer);
    wipe_native_value(typed.data_, observer);
    return;
}
    default:
        return;
    }
}
