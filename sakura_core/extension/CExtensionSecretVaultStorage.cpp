/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionSecretVaultStorage.h"

#include "platform/controlipc/ControlPlatformEndpointDiscoveryReader.h"
#include <sakura/controlipc/ProfileAuthorityIdentity.h>

#include <array>
#include <limits>
#include <utility>

#include <bcrypt.h>
#include <windows.h>

namespace {

using platform::controlipc::CEditorSecretVaultClient;
using platform::controlipc::CEditorSecretVaultEndpointReaderAdapter;
using platform::controlipc::CEditorSecretVaultNamedPipeChannel;
using platform::controlipc::EditorSecretVaultApplyRequest;
using platform::controlipc::EditorSecretVaultApplyResult;
using platform::controlipc::EditorSecretVaultCallerIdentity;
using platform::controlipc::EditorSecretVaultClientOptions;
using platform::controlipc::EditorSecretVaultGetRequest;
using platform::controlipc::EditorSecretVaultGetResult;
using platform::controlipc::EEditorSecretVaultOutcome;
using platform::controlipc::IControlPlatformEndpointReader;
using platform::controlipc::IEditorSecretVaultChannel;
using platform::controlipc::IEditorSecretVaultClient;
using platform::secrets::ESecretGetStatus;
using platform::secrets::ESecretMutationKind;
using platform::secrets::ESecretMutationStatus;
using platform::secrets::SecretAddress;
using platform::secrets::SecretMutationRequest;

SExtensionSecretStorageResult Success()
{
	return { true, EExtensionSecretStorageStatus::Success, ERROR_SUCCESS, {} };
}

SExtensionSecretStorageResult Failure(
	EExtensionSecretStorageStatus status,
	DWORD error,
	std::wstring diagnostic)
{
	return { false, status, error, std::move(diagnostic) };
}

void Wipe(std::string& value) noexcept
{
	volatile char* bytes = value.data();
	for (std::size_t index = 0; index != value.size(); ++index) bytes[index] = '\0';
	value.clear();
}

bool ToUtf8(std::wstring_view value, std::string& result)
{
	result.clear();
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return false;
	result.resize(static_cast<std::size_t>(bytes));
	if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr) != bytes) {
		Wipe(result);
		return false;
	}
	return true;
}

bool FromUtf8(std::string_view value, std::wstring& result)
{
	result.clear();
	if (value.empty()) return true;
	if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const int characters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (characters <= 0) return false;
	result.resize(static_cast<std::size_t>(characters));
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), result.data(), characters) == characters;
}

bool CreateOperationId(std::string& operationId)
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return false;
	}
	static constexpr char digits[] = "0123456789abcdef";
	operationId.clear();
	operationId.reserve(random.size() * 2);
	for (const auto byte : random) {
		operationId.push_back(digits[byte >> 4]);
		operationId.push_back(digits[byte & 0x0f]);
	}
	::SecureZeroMemory(random.data(), random.size());
	return true;
}

SExtensionSecretStorageResult FromGetFailure(const EditorSecretVaultGetResult& result)
{
	switch (result.outcome) {
	case EEditorSecretVaultOutcome::Stopped:
		return Failure(EExtensionSecretStorageStatus::Stopped, ERROR_OPERATION_ABORTED,
			L"Secret Vault is stopped");
	case EEditorSecretVaultOutcome::Unavailable:
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_SERVICE_NOT_ACTIVE,
			L"Secret Vault is unavailable");
	case EEditorSecretVaultOutcome::OperationInFlight:
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_BUSY,
			L"Secret Vault operation is already in flight");
	default:
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_GEN_FAILURE,
			L"Secret Vault read did not reach a successful terminal state");
	}
}

SExtensionSecretStorageResult FromApplyResult(const EditorSecretVaultApplyResult& result)
{
	switch (result.outcome) {
	case EEditorSecretVaultOutcome::Succeeded:
	case EEditorSecretVaultOutcome::NotApplicable:
		return Success();
	case EEditorSecretVaultOutcome::Conflict:
		return Failure(EExtensionSecretStorageStatus::Conflict, ERROR_REVISION_MISMATCH,
			L"Secret Vault revision conflict");
	case EEditorSecretVaultOutcome::RetryWithSameOperationId:
		return Failure(EExtensionSecretStorageStatus::RetryWithSameOperationId, ERROR_RETRY,
			L"Secret Vault mutation outcome remains ambiguous");
	case EEditorSecretVaultOutcome::Stopped:
		return Failure(EExtensionSecretStorageStatus::Stopped, ERROR_OPERATION_ABORTED,
			L"Secret Vault is stopped");
	case EEditorSecretVaultOutcome::Unavailable:
	case EEditorSecretVaultOutcome::OperationInFlight:
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_SERVICE_NOT_ACTIVE,
			L"Secret Vault mutation is unavailable");
	default:
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_GEN_FAILURE,
			L"Secret Vault mutation did not reach a successful terminal state");
	}
}

class CProductionEditorSecretVaultClient final : public IEditorSecretVaultClient {
public:
	explicit CProductionEditorSecretVaultClient(ExtensionSecretVaultStorageProductionOptions options)
		: m_reader(std::make_unique<platform::controlipc::CControlPlatformEndpointDiscoveryReader>(
			options.profileDirectory, options.profileHash))
		, m_readerAdapter(*m_reader)
		, m_client(EditorSecretVaultClientOptions{
			.profileId = std::move(options.profileId),
			.profileHash = std::move(options.profileHash),
			.pinnedControlGeneration = options.pinnedControlGeneration,
			.exchangeDeadline = options.exchangeDeadline,
			.capabilityLifetime = options.capabilityLifetime,
			.channelFactory = [] {
				return std::unique_ptr<IEditorSecretVaultChannel>(
					std::make_unique<CEditorSecretVaultNamedPipeChannel>());
			},
		}, m_readerAdapter)
	{
	}

	EditorSecretVaultGetResult Get(const EditorSecretVaultGetRequest& request) override
	{
		return m_client.Get(request);
	}
	EditorSecretVaultApplyResult Store(const EditorSecretVaultApplyRequest& request) override
	{
		return m_client.Store(request);
	}
	EditorSecretVaultApplyResult Delete(const EditorSecretVaultApplyRequest& request) override
	{
		return m_client.Delete(request);
	}
	platform::controlipc::EditorSecretVaultRevokeResult RevokeSession() override
	{
		return m_client.RevokeSession();
	}
	void Stop() noexcept override { m_client.Stop(); }
	bool IsStopped() const noexcept override { return m_client.IsStopped(); }

private:
	std::unique_ptr<IControlPlatformEndpointReader> m_reader;
	CEditorSecretVaultEndpointReaderAdapter m_readerAdapter;
	CEditorSecretVaultClient m_client;
};

} // namespace

CExtensionSecretVaultStorage::CExtensionSecretVaultStorage(
	std::unique_ptr<IEditorSecretVaultClient> client)
	: m_client(std::move(client))
{
}

CExtensionSecretVaultStorage::~CExtensionSecretVaultStorage()
{
	Stop();
}

SExtensionSecretStorageResult CExtensionSecretVaultStorage::BindSession(
	std::string_view extensionHostSessionId,
	std::uint64_t hostGeneration)
{
	if (!m_client || hostGeneration == 0
		|| !platform::secrets::IsValidSecretVaultIdentifier(
			extensionHostSessionId, platform::secrets::kMaximumSecretVaultCapabilitySessionIdBytes)) {
		return Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER,
			L"Secret Vault session identity is invalid");
	}
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return Failure(EExtensionSecretStorageStatus::Stopped, ERROR_OPERATION_ABORTED,
			L"Secret Vault adapter is stopped");
	}
	if (!m_extensionHostSessionId.empty()) {
		if (m_extensionHostSessionId == extensionHostSessionId && m_hostGeneration == hostGeneration) return Success();
		return Failure(EExtensionSecretStorageStatus::Conflict, ERROR_INVALID_STATE,
			L"Secret Vault adapter is already bound to another host session");
	}
	m_extensionHostSessionId.assign(extensionHostSessionId);
	m_hostGeneration = hostGeneration;
	return Success();
}

std::optional<EditorSecretVaultCallerIdentity> CExtensionSecretVaultStorage::CallerFor(
	std::wstring_view extensionId) const
{
	std::string extensionUtf8;
	std::string canonicalExtensionId;
	if (!ToUtf8(extensionId, extensionUtf8)
		|| !platform::secrets::CanonicalizeSecretVaultExtensionId(extensionUtf8, canonicalExtensionId)) {
		return std::nullopt;
	}
	std::scoped_lock lock(m_mutex);
	if (m_stopped || m_extensionHostSessionId.empty() || m_hostGeneration == 0) return std::nullopt;
	return EditorSecretVaultCallerIdentity{
		m_extensionHostSessionId,
		m_hostGeneration,
		std::move(canonicalExtensionId),
	};
}

SExtensionSecretReadResult CExtensionSecretVaultStorage::Get(
	std::wstring_view extensionId,
	std::wstring_view key)
{
	SExtensionSecretReadResult result;
	const auto caller = CallerFor(extensionId);
	std::string keyUtf8;
	if (!m_client || !caller || !ToUtf8(key, keyUtf8)
		|| !SecretAddress{ caller ? caller->canonicalExtensionId : std::string{}, keyUtf8 }.IsValid()) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER,
				L"Secret Vault read arguments or session are invalid");
		return result;
	}
	auto read = m_client->Get({ *caller, std::move(keyUtf8) });
	if (read.outcome == EEditorSecretVaultOutcome::NotFound) {
		static_cast<SExtensionSecretStorageResult&>(result) = Success();
		return result;
	}
	if (read.outcome != EEditorSecretVaultOutcome::Succeeded
		|| read.result.status != ESecretGetStatus::Found || !read.result.value) {
		static_cast<SExtensionSecretStorageResult&>(result) = FromGetFailure(read);
		if (read.result.value) Wipe(*read.result.value);
		return result;
	}
	std::wstring value;
	const bool converted = FromUtf8(*read.result.value, value);
	Wipe(*read.result.value);
	if (!converted) {
		static_cast<SExtensionSecretStorageResult&>(result) =
			Failure(EExtensionSecretStorageStatus::CorruptData, ERROR_NO_UNICODE_TRANSLATION,
				L"Secret Vault returned invalid UTF-8");
		return result;
	}
	static_cast<SExtensionSecretStorageResult&>(result) = Success();
	result.value = std::move(value);
	return result;
}

SExtensionSecretStorageResult CExtensionSecretVaultStorage::Store(
	std::wstring_view extensionId,
	std::wstring_view key,
	std::wstring_view value)
{
	return Mutate(ESecretMutationKind::Set, extensionId, key, value);
}

SExtensionSecretStorageResult CExtensionSecretVaultStorage::Delete(
	std::wstring_view extensionId,
	std::wstring_view key)
{
	return Mutate(ESecretMutationKind::Delete, extensionId, key, {});
}

SExtensionSecretStorageResult CExtensionSecretVaultStorage::Mutate(
	ESecretMutationKind kind,
	std::wstring_view extensionId,
	std::wstring_view key,
	std::wstring_view value)
{
	const auto caller = CallerFor(extensionId);
	std::string keyUtf8;
	std::string valueUtf8;
	if (!m_client || !caller || !ToUtf8(key, keyUtf8)
		|| (kind == ESecretMutationKind::Set && !ToUtf8(value, valueUtf8))
		|| !SecretAddress{ caller ? caller->canonicalExtensionId : std::string{}, keyUtf8 }.IsValid()
		|| valueUtf8.size() > platform::secrets::kMaximumSecretVaultValueBytes) {
		Wipe(valueUtf8);
		return Failure(EExtensionSecretStorageStatus::InvalidArgument, ERROR_INVALID_PARAMETER,
			L"Secret Vault mutation arguments or session are invalid");
	}

	auto current = m_client->Get({ *caller, keyUtf8 });
	if (current.outcome != EEditorSecretVaultOutcome::Succeeded
		&& current.outcome != EEditorSecretVaultOutcome::NotFound) {
		Wipe(valueUtf8);
		if (current.result.value) Wipe(*current.result.value);
		return FromGetFailure(current);
	}
	if (current.result.value) Wipe(*current.result.value);

	std::string operationId;
	if (!CreateOperationId(operationId)) {
		Wipe(valueUtf8);
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_GEN_FAILURE,
			L"Secret Vault operation identity generation failed");
	}
	EditorSecretVaultApplyRequest request{
		*caller,
		SecretMutationRequest{
			.kind = kind,
			.extensionId = caller->canonicalExtensionId,
			.key = std::move(keyUtf8),
			.value = std::move(valueUtf8),
			.operationId = std::move(operationId),
			.expectedRevision = current.result.revision,
		},
	};
	auto applied = kind == ESecretMutationKind::Set
		? m_client->Store(request) : m_client->Delete(request);
	if (applied.outcome == EEditorSecretVaultOutcome::RetryWithSameOperationId) {
		applied = kind == ESecretMutationKind::Set
			? m_client->Store(request) : m_client->Delete(request);
	}
	Wipe(request.mutation.value);
	request.mutation.operationId.clear();
	return FromApplyResult(applied);
}

SExtensionSecretStorageResult CExtensionSecretVaultStorage::ClearSession() noexcept
{
	bool revoke = false;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopped) {
			return Failure(EExtensionSecretStorageStatus::Stopped, ERROR_OPERATION_ABORTED,
				L"Secret Vault adapter is stopped");
		}
		revoke = !m_extensionHostSessionId.empty();
		m_extensionHostSessionId.clear();
		m_hostGeneration = 0;
	}
	if (!revoke || !m_client) return Success();
	try {
		const auto result = m_client->RevokeSession();
		if (result.outcome == EEditorSecretVaultOutcome::Succeeded) return Success();
		if (result.outcome == EEditorSecretVaultOutcome::Stopped) {
			return Failure(EExtensionSecretStorageStatus::Stopped, ERROR_OPERATION_ABORTED,
				L"Secret Vault session revocation was stopped");
		}
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_SERVICE_NOT_ACTIVE,
			L"Secret Vault session revocation did not reach a successful terminal state");
	}
	catch (...) {
		return Failure(EExtensionSecretStorageStatus::Unavailable, ERROR_GEN_FAILURE,
			L"Secret Vault session revocation failed");
	}
}

void CExtensionSecretVaultStorage::Stop() noexcept
{
	bool revoke = false;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopped) return;
		m_stopped = true;
		revoke = !m_extensionHostSessionId.empty();
		m_extensionHostSessionId.clear();
		m_hostGeneration = 0;
	}
	if (!m_client) return;
	if (revoke) {
		try {
			(void)m_client->RevokeSession();
		}
		catch (...) {
		}
	}
	m_client->Stop();
}

std::unique_ptr<IExtensionSecretSessionStorage>
CreateProductionExtensionSecretVaultStorage(
	ExtensionSecretVaultStorageProductionOptions options)
{
	if (options.profileDirectory.empty()
		|| !platform::profiles::IsCanonicalProfileAuthorityId(options.profileId)
		|| options.profileHash.empty() || options.pinnedControlGeneration == 0
		|| options.exchangeDeadline.count() <= 0 || options.capabilityLifetime.count() <= 0) {
		return {};
	}
	try {
		auto client = std::make_unique<CProductionEditorSecretVaultClient>(std::move(options));
		return std::make_unique<CExtensionSecretVaultStorage>(std::move(client));
	}
	catch (...) {
		return {};
	}
}
