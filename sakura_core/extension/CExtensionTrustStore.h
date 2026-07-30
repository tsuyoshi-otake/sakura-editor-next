/*! @file
	@brief Fail-closed persistent approval store for executable extensions
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <filesystem>
#include <mutex>
#include <string_view>

class CExtensionTrustStore final {
public:
	explicit CExtensionTrustStore(std::filesystem::path storageFile);

	[[nodiscard]] bool IsTrusted(
		std::wstring_view extensionId,
		std::wstring_view version,
		std::wstring_view extensionPath) const;
	[[nodiscard]] bool Grant(
		std::wstring_view extensionId,
		std::wstring_view version,
		std::wstring_view extensionPath);
	[[nodiscard]] bool RevokeAll() noexcept;

private:
	std::filesystem::path m_storageFile;
	mutable std::mutex m_mutex;
};
