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

#include "extension/IExtensionSecretStorage.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

struct SExtensionSecretKeysResult : SExtensionSecretStorageResult {
	std::vector<std::wstring> keys;
};

class CExtensionSecretStorage final : public IExtensionSecretStorage {
public:
	explicit CExtensionSecretStorage(std::filesystem::path rootDirectory);

	SExtensionSecretStorageResult Store(
		std::wstring_view extensionId,
		std::wstring_view key,
		std::wstring_view value) override;
	SExtensionSecretReadResult Get(std::wstring_view extensionId, std::wstring_view key) override;
	SExtensionSecretStorageResult Delete(std::wstring_view extensionId, std::wstring_view key) override;
	SExtensionSecretKeysResult Keys(std::wstring_view extensionId);

	const std::filesystem::path& GetRootDirectory() const noexcept { return m_rootDirectory; }

private:
	std::filesystem::path m_rootDirectory;
	std::mutex m_mutex;
};

#endif /* SAKURA_CEXTENSIONSECRETSTORAGE_9C3DD165_523F_41D3_B74C_C8CF4E6AA5AE_H_ */
