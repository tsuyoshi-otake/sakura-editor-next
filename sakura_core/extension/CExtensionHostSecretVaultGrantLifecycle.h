/*! @file
	@brief Extension Host の native session と Secret Vault grant を同期する境界
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/*! @brief Secret Vault が host capability を識別するための、native editor 専用 session。 */
struct SExtensionHostSecretVaultGrantSession {
	std::uint64_t generation = 0;
	std::wstring extensionHostSessionId;

	[[nodiscard]] bool IsValid() const noexcept
	{
		return generation != 0 && !extensionHostSessionId.empty();
	}
	[[nodiscard]] bool operator==(const SExtensionHostSecretVaultGrantSession&) const noexcept = default;
};

enum class EExtensionHostSecretVaultGrantStatus : std::uint8_t {
	Succeeded,
	Conflict,
	Inactive,
	Invalid,
	Failed,
};

/*! @brief Grant mutation の compare-and-swap 結果。成功時 revision は expectedRevision より大きい。 */
struct SExtensionHostSecretVaultGrantResult {
	EExtensionHostSecretVaultGrantStatus status = EExtensionHostSecretVaultGrantStatus::Invalid;
	std::uint64_t revision = 0;
};

/*! 
	@brief Control-owned Secret Vault の host grant lifecycle に対する狭い契約。

	すべての mutation は (generation, extensionHostSessionId, expectedRevision) を比較し、
	別 generation や古い callback が現在の capability を更新できないようにする。
	実装は値や SecretStorage の API を露出してはならない。
*/
class IExtensionHostSecretVaultGrantLifecycle {
public:
	virtual ~IExtensionHostSecretVaultGrantLifecycle() = default;

	virtual SExtensionHostSecretVaultGrantResult Activate(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) = 0;
	virtual SExtensionHostSecretVaultGrantResult RegisterEditorProcess(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint32_t editorProcessId,
		std::uint64_t expectedRevision) = 0;
	virtual SExtensionHostSecretVaultGrantResult UnregisterEditorProcess(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint32_t editorProcessId,
		std::uint64_t expectedRevision) = 0;
	virtual SExtensionHostSecretVaultGrantResult ReplaceInstalledExtensionInventory(
		const SExtensionHostSecretVaultGrantSession& session,
		const std::vector<std::string>& canonicalExtensionIds,
		std::uint64_t expectedRevision) = 0;
	virtual SExtensionHostSecretVaultGrantResult RevokeIssuedCapabilities(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) = 0;
	virtual SExtensionHostSecretVaultGrantResult Deactivate(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) = 0;
};

enum class EExtensionHostSecretVaultLeaseAcquireResult : std::uint8_t {
	//! Session is active and this PID is registered (or already had a nested lease).
	Registered,
	//! The broker has not materialized the session yet; Activate() will register this PID.
	Deferred,
	//! A lifecycle failure fenced the active grant. The caller must reject the editor lease.
	Rejected,
};

/*! 
	@brief Host lifecycle callbacks と Editor PID lease を Secret Vault grant mutation に変換する。

	外部 lifecycle callback は mutex を保持せずに呼ぶ。結果を適用する直前に
	generation/session/revision を再確認し、競合・例外・不正な revision は revoke →
	deactivate して fail closed にする。
*/
class CExtensionHostSecretVaultGrantCoordinator final {
public:
	explicit CExtensionHostSecretVaultGrantCoordinator(
		std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle> lifecycle);

	CExtensionHostSecretVaultGrantCoordinator(const CExtensionHostSecretVaultGrantCoordinator&) = delete;
	CExtensionHostSecretVaultGrantCoordinator& operator=(const CExtensionHostSecretVaultGrantCoordinator&) = delete;

	//! StartHost 中に呼ぶ。既存 lease と inventory を新 generation へ再発行する。
	[[nodiscard]] bool Activate(const SExtensionHostSecretVaultGrantSession& session) noexcept;
	//! Broker へ lease を渡す直前に呼ぶ。PID の多重 lease は 1 回だけ登録する。
	[[nodiscard]] EExtensionHostSecretVaultLeaseAcquireResult AcquireEditorLease(
		std::uint32_t editorProcessId) noexcept;
	//! Broker から lease を外すときに呼ぶ。最後の lease だけを unregister する。
	[[nodiscard]] bool ReleaseEditorLease(std::uint32_t editorProcessId) noexcept;
	//! OpenVSX の installed set を canonical ID の置換 snapshot として同期する。
	[[nodiscard]] bool ReplaceInstalledExtensionInventory(
		const std::vector<std::string>& extensionIds) noexcept;
	//! 既発行 capability を先に revoke してから grant を deactivate する。冪等。
	void RevokeAndDeactivate() noexcept;
	//! 完全 shutdown。PID lease は次の controller lifetime に持ち越さない。
	void Shutdown() noexcept;

	[[nodiscard]] bool IsActiveForGeneration(std::uint64_t generation) const noexcept;

private:
	struct ActiveSession {
		SExtensionHostSecretVaultGrantSession session;
		std::atomic<std::uint64_t> revision = 0;
	};

	[[nodiscard]] static bool IsSuccessfulAdvance(
		const SExtensionHostSecretVaultGrantResult& result,
		std::uint64_t expectedRevision) noexcept;
	[[nodiscard]] bool ReissueInventoryAndLeases();
	[[nodiscard]] bool RegisterActiveEditorProcess(std::uint32_t editorProcessId) noexcept;
	[[nodiscard]] bool ReplaceActiveInventory(const std::vector<std::string>& canonicalExtensionIds) noexcept;
	[[nodiscard]] bool EndSession(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t revision) noexcept;
	void FailClosed(const SExtensionHostSecretVaultGrantSession& session, std::uint64_t revision) noexcept;
	void ClearActiveIfMatches(const SExtensionHostSecretVaultGrantSession& session) noexcept;
	[[nodiscard]] bool UpdateActiveRevision(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision,
		std::uint64_t nextRevision) noexcept;
	[[nodiscard]] bool GetActive(std::shared_ptr<ActiveSession>& active) const noexcept;

	std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle> m_lifecycle;
	mutable std::mutex m_mutex;
	std::map<std::uint32_t, std::uint32_t> m_editorLeases;
	std::vector<std::string> m_canonicalExtensionInventory;
	bool m_inventoryKnown = false;
	std::shared_ptr<ActiveSession> m_active;
};
