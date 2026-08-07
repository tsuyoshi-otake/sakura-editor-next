/*! @file
	@brief エディタプロセス側の VS Code 互換拡張サービス
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionCommandPalette.h"
#include "extension/CExtensionClientReconnectPolicy.h"
#include "extension/CExtensionDocumentBridge.h"
#include "extension/CExtensionHostSharedState.h"
#include "extension/CExtensionNotificationCenter.h"
#include "extension/CExtensionPipeTransport.h"
#include "extension/CExtensionProfileState.h"
#include "extension/CExtensionRpcProtocol.h"
#include "extension/IExtensionSecretStorage.h"
#include "extension/CExtensionStatusBar.h"
#include "extension/CExtensionViewRegistry.h"

#include "config/WorkspaceContextTypes.h"

class CExtensionWorkbenchServiceBridge;
namespace workbench { class IWorkbenchRuntime; }
namespace workbench::output { class OutputService; }
namespace workbench::problems { class MarkerService; }
#include "extension/CExtensionWorkbenchDispatcher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SExtensionNativeEditorOptions {
	SExtensionDocumentId documentId;
	std::optional<std::uint32_t> tabSize;
	std::optional<bool> insertSpaces;
};

/*!
	@brief 共有 Node host と 1 editor window の workbench model を接続する

	Named pipe、JSON-RPC protocol、dispatcher は専用 worker thread だけが触る。
	pipe read thread と UI thread は bounded queue へ入力を積み、UI model の変更は
	MYWM_EXTENSION_WORKBENCH_CHANGED で editor HWND へ戻す。
*/
class CExtensionService final : private IExtensionPipeTransportSink {
public:
	struct NativeApplyEditResult {
		SExtensionApplyEditResult result;
		std::vector<SExtensionDocumentSnapshot> snapshots;
	};
	using ApplyEditHandler = std::function<NativeApplyEditResult(const std::vector<SExtensionDocumentEdit>&)>;
	using EditorOptionsHandler = std::function<bool(const SExtensionNativeEditorOptions&)>;
	CExtensionService(
		HWND editorWindow,
		HWND brokerWindow,
		std::filesystem::path profileDirectory,
		std::shared_ptr<CExtensionViewRegistry> views,
		std::unique_ptr<IExtensionSecretSessionStorage> secrets = {},
		workbench::problems::MarkerService* markerService = nullptr,
		workbench::output::OutputService* outputService = nullptr,
		workbench::IWorkbenchRuntime* workbenchRuntime = nullptr,
		std::filesystem::path extensionSelectionPath = {},
		bool defaultProfileExtensionsWhenMissing = true);
	~CExtensionService();
	CExtensionService(const CExtensionService&) = delete;
	CExtensionService& operator=(const CExtensionService&) = delete;

	//! 初回 idle または最初の拡張 UI 操作から呼ぶ。二重起動はしない。
	void Start();
	void Shutdown() noexcept;

	//! 導入済み拡張の集合が変わった可能性がある（インストール完了など）ときに UI スレッドから呼ぶ。
	//! 他の Notify* 系と同じく、ワーカースレッドへ投げて即座に戻る（UI スレッドをブロックしない）。
	//! 新しく見つかったルートだけを登録し、既に登録済みの拡張は二重登録・二重アクティベートしない。
	//! まだ一度も接続していなければ（導入済み拡張が無く接続を見送っていた場合を含め）、この呼び出しが
	//! 接続を確立する起点になる。
	void RequestInstalledExtensionRescan();

	void ExecuteCommand(std::wstring_view command, std::string_view argumentsJson = "[]");
	void RequestTreeChildren(std::wstring_view viewHandle, std::wstring_view parentHandle);
	void NotifyTreeSelection(std::wstring_view viewHandle, const std::vector<std::wstring>& itemHandles);
	void NotifyTreeCheckbox(std::wstring_view viewHandle, std::wstring_view itemHandle, bool checked);
	//! Native SCM input edits are requests so the extension host can apply the value and fire
	//! SourceControlInputBox.onDidChange on its owning generation.
	void RequestScmInputChange(std::string_view handle, std::string_view value, bool global = false);
	void NotifyViewVisibility(bool visible);
	//! Native document lifecycle forwarded to the shared extension host.
	void OpenDocument(SExtensionDocumentSnapshot snapshot);
	void ChangeDocument(SExtensionDocumentSnapshot snapshot);
	void SaveDocument(SExtensionDocumentSnapshot snapshot);
	void CloseDocument(SExtensionDocumentId id);
	void SetActiveEditor(SExtensionDocumentId id, SExtensionTextPosition caret);
	void SetWindowState(bool focused);
	//! Projects the resolved workspace trust onto the extension host. Only a
	//! transition to trusted is an event upstream can express; a downgrade updates
	//! the reported value without one.
	void SetWorkspaceTrusted(bool trusted);
	void SetApplyEditHandler(ApplyEditHandler handler);
	void SetEditorOptionsHandler(EditorOptionsHandler handler);

	/*!
		@brief カーソル位置の Hover を要求する

		UI スレッド（マウス dwell タイマー等）から直接呼べる。呼ぶたびに内部の
		sequence を進めてから worker スレッドへ投げるので、応答が届く前に次の
		`RequestHover`/`CancelHover` が来れば古い方の応答は
		`HandleHoverResponseWorker` が sequence 不一致で無視する -- 新しい結果の上に
		古い結果が描かれることはない。`id` の文書がまだ開いていない・スナップショットが
		無い場合は何も送らず Hover を隠す。
	*/
	void RequestHover(SExtensionDocumentId id, SExtensionTextPosition position);
	//! 進行中の Hover 要求を無効化し、表示中の Hover 結果を隠す（マウスが離れた等）。
	void CancelHover();
	//! UI スレッドが `EExtensionWorkbenchChange::Hover` を受けてから読む。
	[[nodiscard]] std::optional<SExtensionHoverResult> HoverResult() const { return m_hover.Snapshot(); }

	/*!
		@brief マニフェスト由来の contribution points を読む

		アクティビティバー・メニュー・キーマップは、ディスパッチャや RPC を知らずに
		このレジストリだけを見る。レジストリ自身が thread-safe なので参照を渡す。
	*/
	[[nodiscard]] const CExtensionContributionRegistry& Contributions() const noexcept { return m_contributions; }

	/*!
		@brief `when` 節 1 本を評価する

		メニュー項目の可視判定とキーバインドの適用判定は同じ context key ストアを
		見なければならない。ストアそのものを公開すると呼び出し側が書き込めてしまうので、
		読む口だけを出す。空の節は真（VS Code の「節が無ければ常に有効」と同じ）。
	*/
	[[nodiscard]] bool EvaluateWhenClause(std::wstring_view clause) const { return m_contextKeys.Evaluate(clause); }

	[[nodiscard]] std::vector<SExtensionStatusBarItem> StatusBarItems() const;
	[[nodiscard]] std::vector<SExtensionCommandPaletteItem> SearchCommands(
		std::wstring_view query, std::size_t maximumResults = 200) const;
	[[nodiscard]] std::vector<SExtensionDiagnostic> DiagnosticsForUri(std::wstring_view uri) const;
	[[nodiscard]] std::vector<SExtensionProblem> Problems() const;
	[[nodiscard]] std::vector<SExtensionOutputChannel> OutputChannels() const;
	[[nodiscard]] std::vector<SExtensionProgress> ProgressItems() const;
	[[nodiscard]] std::vector<SExtensionNotification> PendingNotifications() const;
	//! Queues a non-modal notification completion back to the extension-host worker.
	void ResolveNotification(std::uint64_t id, std::optional<std::size_t> selectedAction);

	//! MYWM_EXTENSION_NOTIFICATION_PROMPT を受けた UI thread だけが呼ぶ。
	LRESULT HandleNotificationPrompt(LPARAM promptPointer) noexcept;
	//! MYWM_EXTENSION_QUICK_INPUT_PROMPT を受けた UI thread だけが呼ぶ。
	LRESULT HandleQuickInputPrompt(LPARAM promptPointer) noexcept;
	//! MYWM_EXTENSION_APPLY_EDIT_PROMPT を受けた UI thread だけが呼ぶ。
	LRESULT HandleApplyEditPrompt(LPARAM promptPointer) noexcept;
	//! MYWM_EXTENSION_EDITOR_OPTIONS_PROMPT を受けた UI thread だけが呼ぶ。
	LRESULT HandleEditorOptionsPrompt(LPARAM promptPointer) noexcept;

private:
	enum class ClientActionKind {
		ExecuteCommand,
		TreeChildren,
		TreeSelection,
		TreeCheckbox,
		ScmInputChange,
	};
	struct ClientAction {
		ClientActionKind kind = ClientActionKind::ExecuteCommand;
		std::wstring first;
		std::wstring second;
		std::vector<std::wstring> handles;
		std::string json;
		bool checked = false;
	};
	enum class PendingKind {
		RegisterExtensions,
		ActivateEvent,
		ExecuteCommand,
		FormatDocument,
		TreeChildren,
		ViewEvent,
		DocumentEvent,
		//! Fire-and-forget acknowledgement for a source-control input update.
		ScmInputChange,
		//! `extension/languages/provide` with kind="hover", answered by `HandleHoverResponseWorker`.
		Hover,
	};
	struct PendingRequest {
		PendingKind kind = PendingKind::ViewEvent;
		std::wstring viewHandle;
		std::wstring parentHandle;
		std::wstring extensionId;
		std::uint64_t generation = 0;
		SExtensionDocumentId documentId;
		std::uint64_t expectedVersion = 0;
		//! Hover only: the requested caret/mouse position, echoed back into the
		//! published `SExtensionHoverResult` so a UI-thread consumer knows where
		//! to anchor the popup without keeping its own side table.
		SExtensionTextPosition hoverPosition;
		//! Hover only: the `m_hoverSequence` value in effect when this request was
		//! sent. A response is applied only while it still matches the service's
		//! current sequence; `RequestHover`/`CancelHover` advance the sequence, so
		//! a superseded response is recognized and discarded instead of racing a
		//! newer one onto the screen.
		std::uint64_t hoverSequence = 0;
	};
	struct NotificationPrompt {
		const SExtensionNotification* notification = nullptr;
		std::optional<std::size_t> selected;
	};
	struct QuickInputPrompt {
		const SExtensionQuickInputRequest* request = nullptr;
		SExtensionQuickInputCompletion completion;
	};
	struct ApplyEditPrompt {
		const std::vector<SExtensionDocumentEdit>* edits = nullptr;
		NativeApplyEditResult completion;
	};
	struct EditorOptionsPrompt {
		const SExtensionNativeEditorOptions* options = nullptr;
		bool applied = false;
	};

	void OnExtensionPipeBytes(std::vector<std::uint8_t> bytes) noexcept override;
	void OnExtensionPipeClosed(std::uint32_t errorCode, std::wstring diagnostic) noexcept override;

	void Enqueue(std::function<void()> task);
	void WorkerMain() noexcept;
	void WorkerInitialize();
	[[nodiscard]] std::vector<std::filesystem::path> LoadInstalledExtensionRootsWorker();
	//! Reads the resolved workspace trust exactly the way `SendRegisterExtensionsWorker`
	//! already does: through the bridge's projection of
	//! `CWorkbenchRuntime::ResolveAndApplyWorkspaceTrust`, never by re-deriving trust here.
	//! A null bridge means trust can never be read, and that must read as untrusted -- fail
	//! closed, never fail open.
	[[nodiscard]] bool CurrentWorkspaceTrustedWorker() const;
	//! Writes one withheld-extension line to the same "Extension Compatibility" output
	//! channel `CExtensionWorkbenchDispatcher::DispatchUnsupportedCapability` owns the
	//! identity/message conventions for. No-op beyond the first report for a given ID while
	//! it remains withheld; see `m_reportedWithheldExtensions`.
	void ReportWithheldExtensionWorker(const std::wstring& uniqueId);
	void RequestReconnectWorker();
	void RescanInstalledExtensionsWorker();
	void EnsureConnectedWorker(std::uint64_t attemptToken);
	void ProcessReconnectDeadlineWorker(CExtensionClientReconnectPolicy::TimePoint now);
	[[nodiscard]] double NextReconnectJitter() noexcept;
	void HandlePipeBytesWorker(
		std::uint64_t attemptToken,
		std::uint64_t connectionGeneration,
		std::vector<std::uint8_t> bytes);
	void HandlePipeClosedWorker(
		std::uint64_t attemptToken,
		std::uint64_t connectionGeneration,
		std::uint32_t errorCode,
		std::wstring diagnostic);
	void HandleMessageWorker(const SExtensionRpcMessage& message);
	bool HandleEnvironmentClipboardRequestWorker(const SExtensionRpcMessage& message);
	bool QueueNotificationRequestWorker(
		const SExtensionNotification& notification, const SExtensionRpcMessage& request);
	void ResolveNotificationWorker(std::uint64_t id, std::optional<std::size_t> selectedAction);
	bool HandleDocumentVersionGapWorker(const SExtensionRpcMessage& message);
	bool HandleEditorOptionsNotificationWorker(const SExtensionRpcMessage& message);
	bool HandleApplyEditRequestWorker(const SExtensionRpcMessage& message);
	bool HandleBuiltInCommandRequestWorker(const SExtensionRpcMessage& message);
	bool ParseApplyEditWorker(std::string_view paramsJson, std::vector<SExtensionDocumentEdit>& edits,
		std::string& error) const;
	void HandleResponseWorker(const SExtensionRpcMessage& message);
	void HandleFormatDocumentResponseWorker(
		const PendingRequest& pending, std::string_view resultJson);
	void RequestHoverWorker(SExtensionDocumentId id, SExtensionTextPosition position, std::uint64_t sequence);
	void CancelHoverWorker(std::uint64_t sequence);
	void HandleHoverResponseWorker(const PendingRequest& pending, std::string_view resultJson);
	bool HandleHelloWorker(const SExtensionRpcMessage& message);
	void SendRegisterExtensionsWorker();
	void HandleRegistrationResultWorker(std::string_view resultJson);
	void SendActivateEventWorker(std::wstring_view event);
	void ActivateContributedViewsWorker();
	void NotifyViewVisibilityWorker(std::wstring_view handle);
	void NotifyRegisteredViewsVisibilityWorker();
	void SubmitClientAction(ClientAction action);
	void RunClientActionWorker(ClientAction action);
	void DrainDeferredActionsWorker();
	void RequestFormatDocumentWorker();
	void RegisterBuiltInCommands();
	void OpenDocumentWorker(SExtensionDocumentSnapshot snapshot);
	void ChangeDocumentWorker(SExtensionDocumentSnapshot snapshot);
	void SaveDocumentWorker(SExtensionDocumentSnapshot snapshot);
	void CloseDocumentWorker(SExtensionDocumentId id);
	void DrainDocumentChangesWorker(CExtensionEventAggregator::Clock::time_point now);
	void SendDocumentSnapshotWorker(std::string_view method, const SExtensionDocumentSnapshot& snapshot,
		bool snapshotOnly = false, std::size_t coalescedChanges = 1);
	void SendDocumentCloseWorker(const SExtensionDocumentId& id, bool notification = false);
	void SendActiveEditorWorker();
	bool SendRequestWorker(
		std::string_view method,
		std::string_view paramsJson,
		PendingRequest pending);
	bool SendNotificationWorker(std::string_view method, std::string_view paramsJson);
	bool SendResponseWorker(const SExtensionRpcMessage& request, const SExtensionWorkbenchDispatchResult& result);
	bool SendOutboundWorker(const SExtensionRpcOutbound& outbound);
	void FailConnectionWorker(std::uint32_t errorCode, std::wstring diagnostic, bool notifyBroker);
	void ReleaseHostLeaseWorker() noexcept;
	void ClearWorkbenchWorker();
	void ResetDispatcherWorker();
	void PostWorkbenchChanges(EExtensionWorkbenchChange changes) const noexcept;
	std::optional<std::size_t> ShowNotificationOnUi(const SExtensionNotification& notification);
	SExtensionQuickInputCompletion ShowQuickInputOnUi(const SExtensionQuickInputRequest& request);
	NativeApplyEditResult ApplyEditOnUi(const std::vector<SExtensionDocumentEdit>& edits);
	bool ApplyEditorOptionsOnUi(const SExtensionNativeEditorOptions& options);
	[[nodiscard]] std::optional<SExtensionViewDescriptor> FindView(std::wstring_view handle) const;

	HWND m_editorWindow = nullptr;
	HWND m_brokerWindow = nullptr;
	std::filesystem::path m_profileDirectory;
	CExtensionProfileState m_extensionProfileState;
	bool m_defaultProfileExtensionsWhenMissing = true;
	std::shared_ptr<CExtensionViewRegistry> m_views;
	CExtensionContextKeys m_contextKeys;
	CExtensionCommandPalette m_commands;
	CExtensionStatusBar m_statusBar;
	CExtensionNotificationCenter m_notifications;
	std::unique_ptr<IExtensionSecretSessionStorage> m_secrets;
	CExtensionDiagnostics m_diagnostics;
	CExtensionQuickInput m_quickInput;
	CExtensionOutputChannel m_output;
	CExtensionProgressCenter m_progress;
	//! Thread-safe like the other *Center members above; `Publish`/`Clear` come
	//! from the worker thread, `Snapshot()` from the UI thread.
	CExtensionHoverCenter m_hover;
	/*
		contribution points はディスパッチャより長生きする。ディスパッチャは拡張ホストの
		再接続ごとに作り直されるので、そこに持たせるとアクティビティバーやキーマップが
		再接続の度に消える。所有はここに置き、ディスパッチャへは参照だけ渡す。
	*/
	CExtensionContributionRegistry m_contributions;
	std::unique_ptr<CExtensionWorkbenchServiceBridge> m_workbenchServiceBridge;
	ApplyEditHandler m_applyEditHandler;
	EditorOptionsHandler m_editorOptionsHandler;

	std::atomic_bool m_started = false;
	std::atomic_bool m_stopping = false;
	std::mutex m_taskMutex;
	std::condition_variable m_taskReady;
	std::deque<std::function<void()>> m_tasks;
	std::atomic_bool m_taskQueueOverloaded = false;
	std::atomic_uint64_t m_pipeCallbackAttemptToken = 0;
	std::atomic_uint64_t m_pipeCallbackGeneration = 0;
	//! Advanced synchronously by `RequestHover`/`CancelHover` on whichever thread
	//! calls them (normally the UI thread), and read by the worker thread to
	//! decide whether an in-flight or just-arrived hover response is still the
	//! most recent one. Atomic because it is the cross-thread fence itself, not
	//! state that Enqueue's task-queue ordering already protects.
	std::atomic_uint64_t m_hoverSequence = 0;
	std::thread m_worker;

	// worker-thread-only state
	std::vector<std::filesystem::path> m_installedRoots;
	//! Extension unique IDs already reported to "Extension Compatibility" for being
	//! withheld by Workspace Trust in the most recently computed installed set.
	//! `LoadInstalledExtensionRootsWorker` drops an ID from here as soon as it leaves the
	//! withheld set (trust granted, disabled, uninstalled), so a later re-withholding is
	//! reported again instead of staying silent forever after the first report.
	std::unordered_set<std::wstring> m_reportedWithheldExtensions;
	std::vector<std::wstring> m_contributedViewIds;
	std::unordered_set<std::wstring> m_requestedViewActivations;
	std::deque<ClientAction> m_deferredActions;
	std::unordered_map<std::string, PendingRequest> m_pendingRequests;
	std::unordered_map<std::uint64_t, SExtensionRpcMessage> m_pendingNotificationRequests;
	CExtensionDocumentSync m_documents;
	CExtensionEventAggregator m_documentEvents;
	std::optional<SExtensionDocumentId> m_activeDocument;
	SExtensionTextPosition m_activeCaret;
	bool m_windowFocused = false;
	//! Non-owning. The runtime outlives this service, which the window destroys first.
	workbench::IWorkbenchRuntime* m_workbenchRuntime = nullptr;
	//! Keeps a workspace-context delivery that is already in flight from reaching a
	//! service that has begun shutting down. Releasing the subscription only stops
	//! deliveries that have not started: the context service copies its listeners and
	//! invokes them outside its own lock, so one can still be running when `Reset`
	//! returns. Holding this mutex across the call is what closes that window.
	struct WorkspaceTrustListenerGate final {
		std::mutex mutex;
		CExtensionService* owner = nullptr;
	};
	std::shared_ptr<WorkspaceTrustListenerGate> m_workspaceTrustGate;
	config::WorkspaceContextSubscription m_workspaceTrustSubscription;
	//! The last value sent to the host, so a resend is not mistaken for a grant. Only ever
	//! advanced while `m_registered`; the RPC it guards is never even attempted before the
	//! first host session registers.
	std::optional<bool> m_sentWorkspaceTrusted;
	//! The last resolved trust value the installed-set filter reacted to, tracked
	//! unconditionally regardless of `m_registered`. `m_sentWorkspaceTrusted` cannot serve
	//! this second purpose: because it only advances once registered, using it here would
	//! make every trust change before the first host session look like a no-op and silently
	//! skip the rescan that is supposed to withhold/admit extensions from the very first
	//! registration. `WorkerInitialize` seeds this explicitly (the WorkspaceContext
	//! subscription in `Start()` is installed only after that task is already queued, so
	//! there is no change notification to wait on for the initial value); `SetWorkspaceTrusted`
	//! keeps it current afterward.
	std::optional<bool> m_filterWorkspaceTrusted;
	CExtensionHostSharedState m_sharedState;
	SExtensionHostBrokerSnapshot m_connectionSnapshot;
	std::unique_ptr<CExtensionPipeTransport> m_transport;
	std::unique_ptr<CExtensionRpcProtocol> m_protocol;
	std::unique_ptr<CExtensionWorkbenchDispatcher> m_dispatcher;
	bool m_leaseAcquired = false;
	bool m_awaitingHello = false;
	bool m_connected = false;
	bool m_registered = false;
	bool m_sidebarVisible = false;
	CExtensionClientReconnectPolicy m_reconnectPolicy;
	std::uint64_t m_connectionAttemptToken = 0;
	std::uint32_t m_reconnectJitterState = 0x9e3779b9u;
};
