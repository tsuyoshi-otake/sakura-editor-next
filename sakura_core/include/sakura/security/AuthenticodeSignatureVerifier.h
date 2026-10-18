/*! @file
	@brief Offline Authenticode publisher verification for executable files.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace platform::security {

//! Returns the leaf signer's subject organization (the O= attribute) when the
//! file carries a valid embedded Authenticode signature, or nullopt when the
//! signature is absent, invalid, or its chain cannot be built. Verification
//! never touches the network and cannot hang offline: WinVerifyTrust runs with
//! WTD_UI_NONE, WTD_REVOKE_NONE and WTD_CACHE_ONLY_URL_RETRIEVAL.
[[nodiscard]] std::optional<std::wstring> VerifiedSignerOrganization(
	std::wstring_view filePath ) noexcept;

} // namespace platform::security
