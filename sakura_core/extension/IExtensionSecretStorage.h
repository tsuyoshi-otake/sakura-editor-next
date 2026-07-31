/*! @file
	@brief Extension-host SecretStorage boundary independent of durable ownership.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class EExtensionSecretStorageStatus {
	Success,
	InvalidArgument,
	Unsupported,
	Unavailable,
	Conflict,
	RetryWithSameOperationId,
	Stopped,
	IoError,
	CryptoError,
	CorruptData,
};

struct SExtensionSecretStorageResult {
	bool success = false;
	EExtensionSecretStorageStatus status = EExtensionSecretStorageStatus::Unavailable;
	std::uint32_t errorCode = 0;
	//! Must never contain an extension secret, key, capability, or operation ID.
	std::wstring diagnostic;
};

struct SExtensionSecretReadResult : SExtensionSecretStorageResult {
	std::optional<std::wstring> value;
};

/*!
	@brief Narrow SecretStorage operations exposed to the workbench dispatcher.

	The production implementation is backed only by the control-owned Secret
	Vault. The legacy per-editor DPAPI implementation implements this interface
	solely for migration compatibility and isolated legacy tests.
*/
class IExtensionSecretStorage {
public:
	virtual ~IExtensionSecretStorage() = default;
	virtual SExtensionSecretStorageResult Store(
		std::wstring_view extensionId,
		std::wstring_view key,
		std::wstring_view value) = 0;
	virtual SExtensionSecretReadResult Get(
		std::wstring_view extensionId,
		std::wstring_view key) = 0;
	virtual SExtensionSecretStorageResult Delete(
		std::wstring_view extensionId,
		std::wstring_view key) = 0;
};

/*!
	@brief Extension-host connection lifecycle around SecretStorage operations.

	BindSession is called only after the native host Hello is authenticated.
	ClearSession revokes the editor capability session before the host lease is
	released. Stop is terminal and must interrupt any in-flight transport.
*/
class IExtensionSecretSessionStorage : public IExtensionSecretStorage {
public:
	~IExtensionSecretSessionStorage() override = default;
	virtual SExtensionSecretStorageResult BindSession(
		std::string_view extensionHostSessionId,
		std::uint64_t hostGeneration) = 0;
	virtual SExtensionSecretStorageResult ClearSession() noexcept = 0;
	virtual void Stop() noexcept = 0;
};
