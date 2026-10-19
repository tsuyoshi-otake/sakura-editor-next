/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include <sakura/security/AuthenticodeSignatureVerifier.h>

#include <Windows.h>
#include <Softpub.h>
#include <wintrust.h>
#include <wincrypt.h>

namespace platform::security {

std::optional<std::wstring> VerifiedSignerOrganization( const std::wstring_view filePath ) noexcept
{
	if( filePath.empty() || filePath.size() >= 32768 ) return std::nullopt;
	const std::wstring path(filePath);

	WINTRUST_FILE_INFO fileInfo{};
	fileInfo.cbStruct = sizeof(fileInfo);
	fileInfo.pcwszFilePath = path.c_str();

	WINTRUST_DATA data{};
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.pFile = &fileInfo;
	data.dwStateAction = WTD_STATEACTION_VERIFY;
	data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	const LONG status = ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

	std::optional<std::wstring> result;
	if( status == ERROR_SUCCESS && data.hWVTStateData != nullptr ) {
		if( auto* provider = ::WTHelperProvDataFromStateData(data.hWVTStateData) ) {
			CRYPT_PROVIDER_SGNR* signer = ::WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
			const CRYPT_PROVIDER_CERT* chain = signer != nullptr
				? ::WTHelperGetProvCertFromChain(signer, 0)
				: nullptr;
			const CERT_CONTEXT* leaf = chain != nullptr ? chain->pCert : nullptr;
			if( leaf != nullptr ) {
				const DWORD length = ::CertGetNameStringW(leaf, CERT_NAME_ATTR_TYPE, 0,
					const_cast<char*>(szOID_ORGANIZATION_NAME), nullptr, 0);
				if( length > 1 ) {
					std::wstring organization(length, L'\0');
					const DWORD written = ::CertGetNameStringW(leaf, CERT_NAME_ATTR_TYPE, 0,
						const_cast<char*>(szOID_ORGANIZATION_NAME), organization.data(),
						static_cast<DWORD>(organization.size()));
					if( written == length ) {
						organization.resize(length - 1);
						result = std::move(organization);
					}
				}
			}
		}
	}
	if( data.hWVTStateData != nullptr ) {
		WINTRUST_DATA close{};
		close.cbStruct = sizeof(close);
		close.dwUIChoice = WTD_UI_NONE;
		close.dwStateAction = WTD_STATEACTION_CLOSE;
		close.hWVTStateData = data.hWVTStateData;
		(void)::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &close);
	}
	return result;
}

bool VerifyGitHubCliTrust( const std::wstring_view filePath ) noexcept
{
	const auto organization = VerifiedSignerOrganization(filePath);
	return organization.has_value() && *organization == L"GitHub, Inc.";
}

} // namespace platform::security
