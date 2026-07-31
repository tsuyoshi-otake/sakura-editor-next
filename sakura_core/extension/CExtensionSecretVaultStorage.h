/*! @file
	@brief Control-owned Secret Vault adapter for the extension-host SecretStorage API.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/IExtensionSecretStorage.h"
#include "platform/controlipc/EditorSecretVaultClient.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

struct ExtensionSecretVaultStorageProductionOptions {
	std::filesystem::path profileDirectory;
	std::string profileId;
	std::wstring profileHash;
	std::uint64_t pinnedControlGeneration = 0;
	std::chrono::milliseconds exchangeDeadline = std::chrono::seconds(5);
	std::chrono::milliseconds capabilityLifetime = std::chrono::seconds(30);
};

/*!
	@brief Session-fenced SecretStorage adapter with no legacy-file fallback.

	The adapter obtains one current Secret Vault revision before each mutation,
	generates one durable operation ID, and performs at most one replay after an
	ambiguous post-Apply disconnect. A conflict is always returned to the caller.
*/
class CExtensionSecretVaultStorage final : public IExtensionSecretSessionStorage {
public:
	explicit CExtensionSecretVaultStorage(
		std::unique_ptr<platform::controlipc::IEditorSecretVaultClient> client);
	~CExtensionSecretVaultStorage() override;
	CExtensionSecretVaultStorage(const CExtensionSecretVaultStorage&) = delete;
	CExtensionSecretVaultStorage& operator=(const CExtensionSecretVaultStorage&) = delete;

	SExtensionSecretStorageResult Store(
		std::wstring_view extensionId,
		std::wstring_view key,
		std::wstring_view value) override;
	SExtensionSecretReadResult Get(
		std::wstring_view extensionId,
		std::wstring_view key) override;
	SExtensionSecretStorageResult Delete(
		std::wstring_view extensionId,
		std::wstring_view key) override;

	SExtensionSecretStorageResult BindSession(
		std::string_view extensionHostSessionId,
		std::uint64_t hostGeneration) override;
	SExtensionSecretStorageResult ClearSession() noexcept override;
	void Stop() noexcept override;

private:
	[[nodiscard]] std::optional<platform::controlipc::EditorSecretVaultCallerIdentity> CallerFor(
		std::wstring_view extensionId) const;
	[[nodiscard]] SExtensionSecretStorageResult Mutate(
		platform::secrets::ESecretMutationKind kind,
		std::wstring_view extensionId,
		std::wstring_view key,
		std::wstring_view value);

	std::unique_ptr<platform::controlipc::IEditorSecretVaultClient> m_client;
	mutable std::mutex m_mutex;
	std::string m_extensionHostSessionId;
	std::uint64_t m_hostGeneration = 0;
	bool m_stopped = false;
};

//! Returns null when immutable profile/generation composition is invalid.
[[nodiscard]] std::unique_ptr<IExtensionSecretSessionStorage>
CreateProductionExtensionSecretVaultStorage(
	ExtensionSecretVaultStorageProductionOptions options);
