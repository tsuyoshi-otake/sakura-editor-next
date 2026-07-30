/*! @file
	@brief 制御プロセス側の拡張ホスト所有コントローラー
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionHostBroker.h"
#include "extension/CExtensionHostSharedState.h"

#include <filesystem>
#include <memory>
#include <string>

/*!
	@brief 1 プロファイル 1 Node host の起動・監視・lease を所有する

	通常の拡張 RPC は中継しない。エディタは共有 snapshot から接続情報を読み、
	Node host の named pipe へ直接接続する。
*/
class CExtensionHostController final : private IExtensionHostBrokerObserver {
public:
	CExtensionHostController() = default;
	~CExtensionHostController();
	CExtensionHostController(const CExtensionHostController&) = delete;
	CExtensionHostController& operator=(const CExtensionHostController&) = delete;

	bool Initialize(const std::filesystem::path& profileDirectory, std::wstring& diagnostic);
	void Shutdown() noexcept;
	void Tick() noexcept;

	bool AcquireLease(std::uint32_t editorProcessId) noexcept;
	void ReleaseLease(std::uint32_t editorProcessId) noexcept;
	bool AcceptHandshake(std::uint64_t generation, std::uint32_t serverProcessId) noexcept;
	void NotifyHostLost(std::uint64_t generation, std::uint32_t errorCode) noexcept;
	[[nodiscard]] SExtensionHostBrokerSnapshot Snapshot() const;

private:
	void OnExtensionHostLifecycleAction(
		const SExtensionHostLifecycleAction& action,
		const SExtensionHostBrokerSnapshot& snapshot) noexcept override;
	void PublishSnapshot() noexcept;
	void PublishUnavailable(std::string diagnostic) noexcept;

	CExtensionHostSharedState m_sharedState;
	std::unique_ptr<CExtensionHostBroker> m_broker;
	SExtensionHostBrokerSnapshot m_unavailableSnapshot;
	bool m_shutdown = false;
};
