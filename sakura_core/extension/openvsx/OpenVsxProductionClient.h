/*! @file
 * @brief Open VSX の production request composition.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "config/CConfigurationNetworkPolicy.h"
#include "extension/openvsx/OpenVsxRequestServiceAdapter.h"

#include <cstdint>
#include <memory>
#include <string>

namespace extension::openvsx {

//! Factory は未構成・不正設定・内部構成失敗を、network request 前に区別する。
enum class EOpenVsxProductionClientOutcome : std::uint8_t {
	Ready,
	InvalidProfileId,
	ConfigurationUnavailable,
	ConfigurationInvalid,
	InternalFailure,
};

//! 診断は常に固定の非機密メッセージで、profile/configuration 値は保持しない。
struct OpenVsxProductionClientResult final {
	EOpenVsxProductionClientOutcome outcome = EOpenVsxProductionClientOutcome::InternalFailure;
	std::shared_ptr<IOpenVsxRegistryClient> client;
	std::string diagnostic;

	explicit operator bool() const noexcept
	{
		return outcome == EOpenVsxProductionClientOutcome::Ready && static_cast<bool>(client);
	}
};

//! One immutable network-policy snapshot becomes bounded, endpoint-specific
//! request limits.  This is pure policy construction; it never contacts a proxy
//! or registry and is intentionally available for deterministic contract tests.
[[nodiscard]] OpenVsxRequestPolicy BuildOpenVsxProductionRequestPolicy(
	const config::ConfigurationNetworkPolicySnapshot& snapshot
);

/*! 
 * @brief 一つの profile 設定 snapshot から、自己完結した Open VSX client を作る。
 *
 * 成功時に返す client は IConfigurationService も CConfigurationNetworkPolicy も参照
 * しない。設定の変更は既存 client に反映されず、次の factory call でのみ採用される。
 */
[[nodiscard]] OpenVsxProductionClientResult CreateOpenVsxProductionClient(
	config::IConfigurationService& configurationService,
	std::wstring canonicalProfileId
) noexcept;

} // namespace extension::openvsx
