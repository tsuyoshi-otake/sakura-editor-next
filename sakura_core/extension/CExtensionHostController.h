/*! @file
	@brief 制御プロセス側の拡張ホスト所有コントローラー
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionHostBroker.h"
#include "extension/CExtensionHostSecretVaultGrantLifecycle.h"
#include "extension/CExtensionHostSharedState.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

/*!
	@brief 1 プロファイル 1 Node host の起動・監視・lease を所有する

	通常の拡張 RPC は中継しない。エディタは共有 snapshot から接続情報を読み、
	Node host の named pipe へ直接接続する。
*/
class CExtensionHostController final : private IExtensionHostBrokerObserver {
public:
	CExtensionHostController() = default;
	explicit CExtensionHostController(
		std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle> secretVaultGrantLifecycle);
	~CExtensionHostController();
	CExtensionHostController(const CExtensionHostController&) = delete;
	CExtensionHostController& operator=(const CExtensionHostController&) = delete;

	bool Initialize(const std::filesystem::path& profileDirectory, std::wstring& diagnostic);
	void Shutdown() noexcept;
	void Tick() noexcept;

	bool AcquireLease(std::uint32_t editorProcessId) noexcept;
	void ReleaseLease(std::uint32_t editorProcessId) noexcept;
	//! Installed extension identities are canonicalized and atomically replaced in the active grant.
	bool RefreshSecretVaultExtensionInventory(const std::vector<std::string>& extensionIds) noexcept;
	bool AcceptHandshake(std::uint64_t generation, std::uint32_t serverProcessId) noexcept;
	void NotifyHostLost(std::uint64_t generation, std::uint32_t errorCode) noexcept;
	[[nodiscard]] SExtensionHostBrokerSnapshot Snapshot() const;

private:
	void OnExtensionHostLifecycleAction(
		const SExtensionHostLifecycleAction& action,
		const SExtensionHostBrokerSnapshot& snapshot) noexcept override;
	void PublishSnapshot() noexcept;
	void PublishUnavailable(std::string diagnostic) noexcept;
	void FailClosedSecretVaultGrant(std::string diagnostic) noexcept;
	void RevokeSecretVaultGrant() noexcept;
	void RollbackAcquiredLease(
		std::uint32_t editorProcessId,
		bool releaseBrokerLease,
		bool releaseSecretVaultLease) noexcept;
	void ReleaseTrackedLease(std::uint32_t editorProcessId) noexcept;
	void ReleaseLeaseComponents(
		std::uint32_t editorProcessId,
		bool releaseBrokerLease,
		bool releaseSecretVaultLease) noexcept;
	void ReclaimTerminatedEditorLeases() noexcept;
	[[nodiscard]] static bool IsEditorProcessTerminated(HANDLE process) noexcept;
	static void CloseLeaseProcessHandle(HANDLE process) noexcept;

	struct SEditorLeaseOwner {
		std::uint32_t leaseCount = 0;
		//! Opened for SYNCHRONIZE on first acquisition. It pins this PID generation.
		HANDLE process = nullptr;
	};

	CExtensionHostSharedState m_sharedState;
	std::unique_ptr<CExtensionHostBroker> m_broker;
	std::unique_ptr<CExtensionHostSecretVaultGrantCoordinator> m_secretVaultGrantCoordinator;
	SExtensionHostBrokerSnapshot m_unavailableSnapshot;
	std::map<std::uint32_t, SEditorLeaseOwner> m_editorLeases;
	ULONGLONG m_nextEditorLeaseHealthCheckTick = 0;
	bool m_shutdown = false;
};
