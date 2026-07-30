/*! @file
	@brief VS Code SecretStorage 互換の Windows DPAPI 永続層
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONSECRETSTORAGE_9C3DD165_523F_41D3_B74C_C8CF4E6AA5AE_H_
#define SAKURA_CEXTENSIONSECRETSTORAGE_9C3DD165_523F_41D3_B74C_C8CF4E6AA5AE_H_
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class EExtensionSecretStorageStatus {
	Success,
	InvalidArgument,
	IoError,
	CryptoError,
	CorruptData,
};

struct SExtensionSecretStorageResult {
	bool success = false;
	EExtensionSecretStorageStatus status = EExtensionSecretStorageStatus::IoError;
	std::uint32_t errorCode = 0;
	std::wstring diagnostic;
};

struct SExtensionSecretReadResult : SExtensionSecretStorageResult {
	std::optional<std::wstring> value;
};

struct SExtensionSecretKeysResult : SExtensionSecretStorageResult {
	std::vector<std::wstring> keys;
};

class CExtensionSecretStorage final {
public:
	explicit CExtensionSecretStorage(std::filesystem::path rootDirectory);

	SExtensionSecretStorageResult Store(
		std::wstring_view extensionId,
		std::wstring_view key,
		std::wstring_view value);
	SExtensionSecretReadResult Get(std::wstring_view extensionId, std::wstring_view key);
	SExtensionSecretStorageResult Delete(std::wstring_view extensionId, std::wstring_view key);
	SExtensionSecretKeysResult Keys(std::wstring_view extensionId);

	const std::filesystem::path& GetRootDirectory() const noexcept { return m_rootDirectory; }

private:
	std::filesystem::path m_rootDirectory;
	std::mutex m_mutex;
};

#endif /* SAKURA_CEXTENSIONSECRETSTORAGE_9C3DD165_523F_41D3_B74C_C8CF4E6AA5AE_H_ */
