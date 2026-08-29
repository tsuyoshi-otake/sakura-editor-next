/*! @file */
#include <sakura/harnessbridge/HarnessBridgeCapability.h>

#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace platform::harnessbridge {
namespace {

bool ValidContext(const HarnessCapabilityContext& context) noexcept
{
	return context.bridgeEpoch != 0 && context.runtimeGeneration != 0 && context.sessionId != 0
		&& context.paneId != 0
		&& ((context.processId == 0 && context.processCreationTime == 0)
			|| (context.processId != 0 && context.processCreationTime != 0));
}

bool FillRandom(std::span<std::uint8_t> bytes) noexcept
{
	return BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool ConstantTimeEqual(const std::span<const std::uint8_t> left, const std::span<const std::uint8_t> right) noexcept
{
	if (left.size() != right.size()) return false;
	std::uint8_t difference = 0;
	for (std::size_t i = 0; i < left.size(); ++i) difference |= left[i] ^ right[i];
	return difference == 0;
}

bool HasNonzero(const std::array<std::uint8_t, 16>& bytes) noexcept
{
	return std::any_of(bytes.begin(), bytes.end(), [](const std::uint8_t value) { return value != 0; });
}

constexpr std::uint32_t kKnownGrants = static_cast<std::uint32_t>(EHarnessGrant::Message)
	| static_cast<std::uint32_t>(EHarnessGrant::ConsoleRead)
	| static_cast<std::uint32_t>(EHarnessGrant::SendInput)
	| static_cast<std::uint32_t>(EHarnessGrant::ManageTerminal);

bool SameOptionalTarget(const HarnessCapabilityContext& left,
	const HarnessCapabilityContext& right) noexcept
{
	if (!left.profileId.empty() && left.profileId != right.profileId) return false;
	if (left.profileGeneration != 0 && left.profileGeneration != right.profileGeneration) return false;
	if (HasNonzero(left.editorId) && left.editorId != right.editorId) return false;
	if (left.instanceGeneration != 0 && left.instanceGeneration != right.instanceGeneration) return false;
	if (left.windowId != 0 && left.windowId != right.windowId) return false;
	if (left.instanceId != 0 && left.instanceId != right.instanceId) return false;
	return true;
}

std::optional<std::array<std::uint8_t, 32>> ComputeHmac(
	const std::array<std::uint8_t, 32>& secret, const std::span<const std::uint8_t> transcript) noexcept
{
	try {
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
			nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return std::nullopt;
		struct AlgorithmCloser final {
			BCRYPT_ALG_HANDLE value;
			~AlgorithmCloser() { if (value != nullptr) (void)::BCryptCloseAlgorithmProvider(value, 0); }
		} algorithmCloser{ algorithm };

		DWORD objectBytes = 0;
		DWORD resultBytes = 0;
		if (!BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
			reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &resultBytes, 0))) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> object(objectBytes);
		BCRYPT_HASH_HANDLE hash = nullptr;
		if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
			const_cast<PUCHAR>(secret.data()), static_cast<ULONG>(secret.size()), 0))) return std::nullopt;
		struct HashCloser final {
			BCRYPT_HASH_HANDLE value;
			~HashCloser() { if (value != nullptr) (void)::BCryptDestroyHash(value); }
		} hashCloser{ hash };
		if (!transcript.empty() && !BCRYPT_SUCCESS(::BCryptHashData(hash,
			const_cast<PUCHAR>(transcript.data()), static_cast<ULONG>(transcript.size()), 0))) return std::nullopt;
		std::array<std::uint8_t, 32> digest{};
		if (!BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
			return std::nullopt;
		}
		return digest;
	} catch (...) {
		return std::nullopt;
	}
}

} // namespace

std::optional<HarnessCapabilityCredential> CHarnessBridgeCapabilityStore::Issue(
	const HarnessCapabilityContext& context, const EHarnessGrant grants,
	const std::chrono::steady_clock::time_point expiresAt)
{
	if (!ValidContext(context) || grants == EHarnessGrant::None
		|| (static_cast<std::uint32_t>(grants) & ~kKnownGrants) != 0) return std::nullopt;
	std::lock_guard lock(m_mutex);
	if (m_records.size() >= 1024) return std::nullopt;
	HarnessCapabilityCredential credential;
	if (!FillRandom(credential.id.value) || !FillRandom(credential.secret)) return std::nullopt;
	credential.grants = grants;
	Record record;
	record.credential = credential;
	record.context = context;
	record.expiresAt = expiresAt == std::chrono::steady_clock::time_point{}
		? std::chrono::steady_clock::now() + std::chrono::minutes(5) : expiresAt;
	m_records.emplace(credential.id, std::move(record));
	return credential;
}

void CHarnessBridgeCapabilityStore::Revoke(const HarnessOpaqueId& id) noexcept
{
	std::lock_guard lock(m_mutex);
	const auto it = m_records.find(id);
	if (it == m_records.end()) return;
	std::fill(it->second.credential.secret.begin(), it->second.credential.secret.end(), std::uint8_t{ 0 });
	m_records.erase(it);
}

void CHarnessBridgeCapabilityStore::RevokeAll() noexcept
{
	std::lock_guard lock(m_mutex);
	for (auto& [id, record] : m_records) {
		(void)id;
		record.revoked = true;
		std::fill(record.credential.secret.begin(), record.credential.secret.end(), std::uint8_t{ 0 });
	}
	m_records.clear();
}

EHarnessCapabilityCheck CHarnessBridgeCapabilityStore::Check(
	const HarnessCapabilityCredential& credential, const HarnessCapabilityContext& context,
	const EHarnessGrant required, const std::chrono::steady_clock::time_point now) const noexcept
{
	std::lock_guard lock(m_mutex);
	const auto it = m_records.find(credential.id);
	if (it == m_records.end() || !credential.id.IsValid()) return EHarnessCapabilityCheck::Invalid;
	const auto& record = it->second;
	if (record.revoked) return EHarnessCapabilityCheck::Revoked;
	if (record.expiresAt != std::chrono::steady_clock::time_point{} && now >= record.expiresAt) return EHarnessCapabilityCheck::Expired;
	if (record.context.bridgeEpoch != context.bridgeEpoch || record.context.runtimeGeneration != context.runtimeGeneration) return EHarnessCapabilityCheck::WrongEpoch;
	if (record.context.sessionId != context.sessionId || record.context.paneId != context.paneId
		|| !SameOptionalTarget(record.context, context)) return EHarnessCapabilityCheck::WrongTarget;
	// The direct credential check has no OS job-membership proof and therefore
	// must never authorize a terminal-job-scoped record.
	if (record.context.processId == 0 || record.context.processCreationTime == 0) {
		return EHarnessCapabilityCheck::WrongProcess;
	}
	if (record.context.processId != context.processId || record.context.processCreationTime != context.processCreationTime) return EHarnessCapabilityCheck::WrongProcess;
	if (!HasGrant(record.credential.grants, required) || !HasGrant(credential.grants, required)) return EHarnessCapabilityCheck::MissingGrant;
	if (!ConstantTimeEqual(record.credential.secret, credential.secret)) return EHarnessCapabilityCheck::Invalid;
	return EHarnessCapabilityCheck::Granted;
}

std::optional<std::array<std::uint8_t, 32>> CHarnessBridgeCapabilityStore::ComputeAuthenticationDigest(
	const HarnessOpaqueId& id, const std::span<const std::uint8_t> transcript,
	const std::chrono::steady_clock::time_point now) const noexcept
{
	std::lock_guard lock(m_mutex);
	const auto it = m_records.find(id);
	if (it == m_records.end() || !id.IsValid() || it->second.revoked
		|| (it->second.expiresAt != std::chrono::steady_clock::time_point{} && now >= it->second.expiresAt)) {
		return std::nullopt;
	}
	return ComputeHmac(it->second.credential.secret, transcript);
}

EHarnessCapabilityCheck CHarnessBridgeCapabilityStore::CheckAuthentication(
	const HarnessOpaqueId& id, const EHarnessGrant presentedGrants,
	const HarnessCapabilityContext& context, const EHarnessGrant required,
	const std::span<const std::uint8_t> transcript, const std::span<const std::uint8_t> digest,
	const std::chrono::steady_clock::time_point now) const noexcept
{
	std::lock_guard lock(m_mutex);
	const auto it = m_records.find(id);
	if (it == m_records.end() || !id.IsValid()) return EHarnessCapabilityCheck::Invalid;
	const auto& record = it->second;
	if (record.revoked) return EHarnessCapabilityCheck::Revoked;
	if (record.expiresAt != std::chrono::steady_clock::time_point{} && now >= record.expiresAt) return EHarnessCapabilityCheck::Expired;
	if (record.context.bridgeEpoch != context.bridgeEpoch || record.context.runtimeGeneration != context.runtimeGeneration) return EHarnessCapabilityCheck::WrongEpoch;
	if (record.context.sessionId != context.sessionId || record.context.paneId != context.paneId
		|| !SameOptionalTarget(record.context, context)) return EHarnessCapabilityCheck::WrongTarget;
	const bool terminalJobScoped = record.context.processId == 0
		&& record.context.processCreationTime == 0;
	if (!terminalJobScoped && (record.context.processId != context.processId
		|| record.context.processCreationTime != context.processCreationTime)) {
		return EHarnessCapabilityCheck::WrongProcess;
	}
	if (context.processId == 0 || context.processCreationTime == 0) {
		return EHarnessCapabilityCheck::WrongProcess;
	}
	if (!HasGrant(record.credential.grants, required) || !HasGrant(record.credential.grants, presentedGrants)) return EHarnessCapabilityCheck::MissingGrant;
	if (digest.size() != 32) return EHarnessCapabilityCheck::Invalid;
	const auto expected = ComputeHmac(record.credential.secret, transcript);
	if (!expected || !ConstantTimeEqual(*expected, digest)) return EHarnessCapabilityCheck::Invalid;
	return EHarnessCapabilityCheck::Granted;
}

std::size_t CHarnessBridgeCapabilityStore::Size() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_records.size();
}

} // namespace platform::harnessbridge
