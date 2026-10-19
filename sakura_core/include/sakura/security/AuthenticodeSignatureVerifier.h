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

//! True only when the file carries a valid embedded Authenticode signature
//! whose leaf signer subject organization is exactly "GitHub, Inc." -- the
//! publisher gate every gh.exe launch boundary applies before any
//! credential-adjacent invocation (the SENP tool policy probe and the
//! workbench account `gh auth status` runner share it). The match is exact
//! and case-sensitive: the organization is the CA-verified legal name, so a
//! differently cased value names a different subject.
[[nodiscard]] bool VerifyGitHubCliTrust( std::wstring_view filePath ) noexcept;

} // namespace platform::security
