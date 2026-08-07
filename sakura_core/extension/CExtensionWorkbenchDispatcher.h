/*! @file
	@brief 拡張ホスト RPC を所有権付きワークベンチモデルへ反映する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionCommandPalette.h"
#include "extension/CExtensionContributionRegistry.h"
#include "extension/CExtensionNotificationCenter.h"
#include "extension/CExtensionRpcProtocol.h"
#include "extension/IExtensionSecretStorage.h"
#include "extension/CExtensionStatusBar.h"
#include "extension/CExtensionViewRegistry.h"
#include "extension/CExtensionWorkbenchUi.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

class CExtensionWorkbenchServiceBridge;

enum class EExtensionWorkbenchChange : std::uint32_t {
	None = 0,
	Commands = 1u << 0,
	StatusBar = 1u << 1,
	Views = 1u << 2,
	Notifications = 1u << 3,
	Diagnostics = 1u << 4,
	Output = 1u << 5,
	Progress = 1u << 6,
	QuickInput = 1u << 7,
	Scm = 1u << 8,
	//! menus / keybindings / viewsContainers / languages / snippets の集合が変わった
	Contributions = 1u << 9,
	//! `CExtensionHoverCenter` に新しい Hover 結果が Publish/Clear された（`CExtensionService::RequestHover`
	//! の応答、またはその取り消し）。Hover は `Dispatch()` を経由しない outbound request/response なので、
	//! ここは他の inbound capability の bit と違い `CExtensionService` 側だけが立てる。
	Hover = 1u << 10,
	/*!
		@brief context key が変わった（`setContext`）

		`when` を投影時に評価する面 — メニュー・コマンドパレット・キーバインド・
		ビューコンテナ — は、宣言が 1 つも変わらなくても context key が変われば
		見せる集合が変わる。`Commands` と分けているのは、コマンド登録のたびに
		コンテナ投影をやり直さないため。
	*/
	ContextKeys = 1u << 11,
};

constexpr EExtensionWorkbenchChange operator|(EExtensionWorkbenchChange left, EExtensionWorkbenchChange right) noexcept
{
	return static_cast<EExtensionWorkbenchChange>(
		static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

struct SExtensionWorkbenchDispatchResult {
	bool handled = false;
	bool success = false;
	//! The request is waiting for a later UI action and must not be answered yet.
	bool responseDeferred = false;
	std::string resultJson = "{}";
	int errorCode = -32602;
	std::string errorMessage;
	EExtensionWorkbenchChange changes = EExtensionWorkbenchChange::None;
};

/*!
	@brief JSON-RPC の application method を純粋なモデル更新へ変換する

	Transport と HWND を所有しない。I/O owner が Dispatch を直列に呼び、changes を
	UI thread へ通知する。SecretStorage の平文は resultJson 以外へ保持しない。
*/
class CExtensionWorkbenchDispatcher final {
public:
	using NotificationHandler = std::function<std::optional<std::size_t>(const SExtensionNotification&)>;
	using DeferredNotificationHandler = std::function<bool(
		const SExtensionNotification&, const SExtensionRpcMessage&)>;
	using QuickInputHandler = std::function<SExtensionQuickInputCompletion(const SExtensionQuickInputRequest&)>;

	CExtensionWorkbenchDispatcher(
		CExtensionContextKeys& contextKeys,
		CExtensionCommandPalette& commands,
		CExtensionStatusBar& statusBar,
		CExtensionNotificationCenter& notifications,
		CExtensionViewRegistry& views,
		IExtensionSecretStorage& secrets,
		CExtensionDiagnostics& diagnostics,
		CExtensionQuickInput& quickInput,
		CExtensionOutputChannel& output,
		CExtensionProgressCenter& progress,
		CExtensionWorkbenchServiceBridge* serviceBridge = nullptr,
		/*
			contribution レジストリはディスパッチャより長生きする。ディスパッチャは
			拡張ホストの再接続ごとに作り直される（CExtensionService::ResetDispatcherWorker）
			ので、ここで所有すると、その度にアクティビティバーやキーマップが消えてしまう。
			所有は CExtensionService に置き、ここは参照だけを持つ。
		*/
		CExtensionContributionRegistry* contributions = nullptr);

	void SetNotificationHandler(NotificationHandler handler);
	void SetDeferredNotificationHandler(DeferredNotificationHandler handler);
	void SetQuickInputHandler(QuickInputHandler handler);
	[[nodiscard]] SExtensionWorkbenchDispatchResult Dispatch(const SExtensionRpcMessage& message);
	[[nodiscard]] SExtensionWorkbenchDispatchResult ApplyTreeChildrenResult(
		std::wstring_view viewHandle,
		std::wstring_view parentHandle,
		std::wstring_view extensionId,
		std::uint64_t generation,
		std::string_view resultJson);

private:
	struct CommandState {
		std::wstring extensionId;
		std::uint64_t generation = 0;
		bool contributed = false;
		bool hasHandler = false;
	};

	SExtensionWorkbenchDispatchResult DispatchExtensionRegistration(std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchRemoveGeneration(std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchActivationFailure(std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchCommandHandler(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchContextSet(std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchCommandList(std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchStatusBar(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchView(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchSecret(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchNotification(const SExtensionRpcMessage& message);
	SExtensionWorkbenchDispatchResult DispatchDiagnostics(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchQuickInput(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchOutput(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchProgress(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchScm(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchLanguageStatus(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchCapabilityRegistration(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchUnsupportedCapability(std::string_view method, std::string_view paramsJson);
	SExtensionWorkbenchDispatchResult DispatchConfigurationUpdate(std::string_view paramsJson);

	CExtensionContextKeys& m_contextKeys;
	CExtensionCommandPalette& m_commands;
	CExtensionStatusBar& m_statusBar;
	CExtensionNotificationCenter& m_notifications;
	CExtensionViewRegistry& m_views;
	IExtensionSecretStorage& m_secrets;
	CExtensionDiagnostics& m_diagnostics;
	CExtensionQuickInput& m_quickInput;
	CExtensionOutputChannel& m_output;
	CExtensionProgressCenter& m_progress;
	CExtensionWorkbenchServiceBridge* m_serviceBridge = nullptr;
	CExtensionContributionRegistry* m_contributions = nullptr;
	NotificationHandler m_notificationHandler;
	DeferredNotificationHandler m_deferredNotificationHandler;
	QuickInputHandler m_quickInputHandler;
	std::unordered_map<std::wstring, CommandState> m_commandStates;
	std::unordered_map<std::wstring, SExtensionViewDescriptor> m_viewDescriptors;
	std::unordered_set<std::wstring> m_reportedUnsupportedCapabilities;
};
