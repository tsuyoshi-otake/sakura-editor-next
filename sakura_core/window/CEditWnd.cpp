/*!	@file
	@brief 編集ウィンドウ（外枠）管理クラス

	@author Norio Nakatani
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000-2001, genta, jepro, ao
	Copyright (C) 2001, MIK, Stonee, Misaka, hor, YAZAKI
	Copyright (C) 2002, YAZAKI, genta, hor, aroka, minfu, 鬼, MIK, ai
	Copyright (C) 2003, genta, MIK, Moca, wmlhq, ryoji, KEITA
	Copyright (C) 2004, genta, Moca, yasu, MIK, novice, Kazika
	Copyright (C) 2005, genta, MIK, Moca, aroka, ryoji
	Copyright (C) 2006, genta, ryoji, aroka, fon, yukihane
	Copyright (C) 2007, ryoji
	Copyright (C) 2008, ryoji, nasukoji
	Copyright (C) 2009, ryoji, nasukoji, Hidetaka Sakai
	Copyright (C) 2010, ryoji, Moca、Uchi
	Copyright (C) 2011, ryoji
	Copyright (C) 2013, Uchi
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include <ShlObj.h>

#include "window/CEditWnd.h"
#include "_main/CControlTray.h"
#include "_main/FailedEditorProcessShutdown.h"
#include "_main/CCommandLine.h"	/// 2003/1/26 aroka
#include "_main/CAppMode.h"
#include "basis/CErrorInfo.h"
#include "func/CKeyBind.h"
#include "dlg/CDlgAbout.h"
#include "dlg/CDlgOpenFile.h"
#include "dlg/CDlgPrintSetting.h"
#include "env/CShareData.h"
#include "env/CSakuraEnvironment.h"
#include "charset/CCodeFactory.h"
#include "charset/CCodeBase.h"
#include "charset/charset.h"
#include "CSelectLang.h"
#include "CEditApp.h"
#include "prop/CPropCommon.h"
#include "recent/CMRUFile.h"
#include "recent/CMRUFolder.h"
#include "util/module.h"
#include "util/os.h"		//WM_MOUSEWHEEL,WM_THEMECHANGED
#include "util/window.h"
#include "util/shell.h"
#include "util/file.h"
#include "util/string_ex2.h"
#include "plugin/CJackManager.h"
#include "agent/CGrepAgent.h"
#include "env/CMarkMgr.h"
#include "doc/logic/CDocLine.h"
#include "doc/CDocFileOperation.h"
#include "doc/layout/CLayout.h"
#include "debug/CRunningTimer.h"
#include "debug/StartupTrace.h"
#include "apiwrap/StdApi.h"
#include "apiwrap/CommonControl.h"
#include "sakura_rc.h"
#include "config/system_constants.h"
#include "config/app_constants.h"
#include "recent/CRecentEditNode.h"
#include "recent/CRecentFile.h"
#include "recent/CRecentFolder.h"
#include "apiwrap/DarkMode.h"
#include "window/CCustomFrameController.h"
#include "window/DocumentBreadcrumbs.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include "markdown/MarkdownRemoteImageFetcher.h"
#include "markdown/MarkdownPreviewLayout.h"
#include "terminal/window/CTerminalTool.h"
#include "_main/CProcess.h"
#include "terminal/model/TerminalModel.h"
#include "theme/CThemeService.h"
#include "theme/CColorThemeRegistry.h"
#include "senp/SenpRuntimeService.h"
#include "update/UpdateComposition.h"
#include "update/IUpdateService.h"
#include "config/ConfigurationTypes.h"
#include "config/CConfigurationNetworkPolicy.h"
#include "config/CWorkspaceContextService.h"
#include "config/SettingsWritebackCoordinator.h"
#include "config/editing/CJsoncConfigurationEditor.h"
#include "workbench/CWorkbenchPanelHost.h"
#include "workbench/CWorkspaceContext.h"
#include "workbench/IWorkbenchRuntime.h"
#include "workbench/projects/ProjectCatalogService.h"
#include "workbench/recent/RecentlyOpenedWorkspaceMenuProjection.h"
#include "workbench/workspace/WorkspaceEditingService.h"
#include "workbench/workspace/WorkspaceWindowTransitionService.h"
#include "workbench/workspace/WorkspaceWindowTransitionPlanner.h"
#include "workbench/IconMetrics.h"
#include "workbench/WorkbenchLayout.h"
#include "workbench/account/AccountDiscovery.h"
#include "workbench/activity/ActivityBarEntryProjection.h"
#include "workbench/activity/CActivityBar.h"
#include "workbench/projects/CProjectsPage.h"
#include "workbench/panel/CBottomPanelTool.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/explorer/ExplorerDeleteConfirmation.h"
#include "workbench/explorer/ExplorerResourcePath.h"
#include "workbench/statusbar/IStatusbarVisibilityMementoStore.h"
#include "workbench/statusbar/StatusbarViewModel.h"
#include "workbench/viewcontainer/CViewContainerHost.h"
#include "workbench/search/CSearchWorkbenchTool.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/rendering/FrameCoordinatorRuntime.h"
#include "workbench/rendering/FrameCadenceSource.h"
#include "workbench/rendering/FrameRuntimeRetirementService.h"
#include "workbench/rendering/FrameWindowTransaction.h"
#include "workbench/scm/ScmNativeSurfaceAdapter.h"
#include "workbench/WorkbenchZoom.h"
#include "workbench/commands/ApiCommandArguments.h"
#include "workbench/commands/ExplorerCommandArguments.h"
#include "workbench/commands/ExplorerCommandIds.h"
#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/commands/WorkbenchContextKeyService.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/editor/CEditDocLegacyEditorBackend.h"
#include "workbench/editor/CEditorServiceLegacyAdapter.h"
#include "workbench/editor/CDiffSurface.h"
#include "workbench/editor/CEmptyEditorSurface.h"
#include "workbench/editor/EditorCommandIds.h"
#include "workbench/editor/WorkbenchCommandPaletteModel.h"
#include "workbench/editor/EditorWorkingCopyCoordinator.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/scm/GitBranchCommands.h"
#include "workbench/scm/GitCommitCommands.h"
#include "workbench/scm/GitDiffModel.h"
#include "workbench/scm/GitFailureText.h"
#include "workbench/scm/GitInitCloneCommands.h"
#include "workbench/scm/GitLineStaging.h"
#include "workbench/scm/GitHistoryModel.h"
#include "workbench/scm/GitOutputChannel.h"
#include "workbench/scm/GitScmPublisher.h"
#include "workbench/scm/GitStageCommands.h"
#include "workbench/scm/GitSyncCommands.h"
#include "workbench/problems/MarkerPositionAdapter.h"
#include "workbench/layout/WorkbenchLayoutStateService.h"
#include "workbench/win32/PaneCompositeProjectionService.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"
#include <sakura/filesystem/FileSystemFactory.h>
#include <sakura/uri/UriIdentity.h>
#include <sakura/editor/win32/Win32EditorFrameAdapter.h>

#include "macro/CMacroFactory.h"
#include "view/colors/CColorStrategy.h"
#include "view/figures/CFigureManager.h"
#include "workbench/quickinput/CCommandPaletteOverlay.h"
#include "workbench/quickinput/CQuickInputDialog.h"
#include "_os/CClipboard.h"
#include "cmd/COpeBlk.h"
#include "cmd/CViewCommander_inline.h"

#include <array>
#include <atomic>
#include <cwctype>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

namespace
{
class CWorkspaceWindowTransitionCallbackHost final
	: public workbench::workspace::IWorkspaceWindowTransitionHost {
public:
	std::function<EWorkspaceWindowTransitionResult()> prepare;
	std::function<EWorkspaceWindowTransitionResult()> launch;
	std::function<EWorkspaceWindowTransitionResult()> close;
	std::function<EWorkspaceWindowTransitionResult()> cleanup;

	workbench::workspace::EWorkspaceWindowTransitionOutcome PrepareReplacement() override
	{
		return Convert(prepare ? prepare() : EWorkspaceWindowTransitionResult::Failed);
	}
	workbench::workspace::EWorkspaceWindowTransitionOutcome LaunchAndWaitForReady() override
	{
		return Convert(launch ? launch() : EWorkspaceWindowTransitionResult::Failed);
	}
	workbench::workspace::EWorkspaceWindowTransitionOutcome CloseCurrentWindowOnce() override
	{
		return Convert(close ? close() : EWorkspaceWindowTransitionResult::Failed);
	}
	workbench::workspace::EWorkspaceWindowTransitionOutcome DeleteStagedTarget() override
	{
		return Convert(cleanup ? cleanup() : EWorkspaceWindowTransitionResult::Failed);
	}

private:
	static workbench::workspace::EWorkspaceWindowTransitionOutcome Convert(EWorkspaceWindowTransitionResult result) noexcept
	{
		switch (result) {
		case EWorkspaceWindowTransitionResult::Succeeded:
			return workbench::workspace::EWorkspaceWindowTransitionOutcome::Succeeded;
		case EWorkspaceWindowTransitionResult::Cancelled:
			return workbench::workspace::EWorkspaceWindowTransitionOutcome::Cancelled;
		case EWorkspaceWindowTransitionResult::Failed:
			return workbench::workspace::EWorkspaceWindowTransitionOutcome::Failed;
		}
		return workbench::workspace::EWorkspaceWindowTransitionOutcome::Failed;
	}
};

EWorkspaceWindowTransitionResult ToWindowTransitionResult(
	workbench::workspace::EWorkspaceWindowTransitionOutcome result) noexcept
{
	switch (result) {
	case workbench::workspace::EWorkspaceWindowTransitionOutcome::Succeeded:
		return EWorkspaceWindowTransitionResult::Succeeded;
	case workbench::workspace::EWorkspaceWindowTransitionOutcome::Cancelled:
		return EWorkspaceWindowTransitionResult::Cancelled;
	case workbench::workspace::EWorkspaceWindowTransitionOutcome::Failed:
		return EWorkspaceWindowTransitionResult::Failed;
	}
	return EWorkspaceWindowTransitionResult::Failed;
}

std::optional<std::wstring> CreateManagedWorkspacePath()
{
	std::array<WCHAR, MAX_PATH> temporaryDirectory{};
	if (::GetTempPathW(static_cast<DWORD>(temporaryDirectory.size()), temporaryDirectory.data()) == 0) return std::nullopt;
	std::array<WCHAR, MAX_PATH> reservation{};
	if (::GetTempFileNameW(temporaryDirectory.data(), L"skr", 0, reservation.data()) == 0) return std::nullopt;
	// GetTempFileName atomically reserves uniqueness.  The workspace service
	// subsequently owns the actual document creation through Missing-CAS.
	if (::DeleteFileW(reservation.data()) == FALSE) return std::nullopt;
	return std::wstring(reservation.data()) + L".code-workspace";
}

class CStartupDocumentSubphaseTimer final
{
public:
	explicit CStartupDocumentSubphaseTimer(CStartupTrace::StartupDocumentSubphase subphase) noexcept
		: m_subphase(subphase)
		, m_enabled(CStartupTrace::IsCollectingStartupDocumentMetrics())
	{
		if (m_enabled) {
			::QueryPerformanceCounter(&m_start);
		}
	}

	~CStartupDocumentSubphaseTimer()
	{
		Finish();
	}

	void Finish() noexcept
	{
		if (!m_enabled) {
			return;
		}
		LARGE_INTEGER end{};
		::QueryPerformanceCounter(&end);
		CStartupTrace::AccumulateStartupDocumentSubphase(
			m_subphase, end.QuadPart - m_start.QuadPart);
		m_enabled = false;
	}

	CStartupDocumentSubphaseTimer(const CStartupDocumentSubphaseTimer&) = delete;
	CStartupDocumentSubphaseTimer& operator=(const CStartupDocumentSubphaseTimer&) = delete;

private:
	CStartupTrace::StartupDocumentSubphase m_subphase;
	LARGE_INTEGER m_start{};
	bool m_enabled{};
};
}

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたので
//	定義を削除

#define		YOHAKU_X		4		/* ウィンドウ内の枠と紙の隙間最小値 */
#define		YOHAKU_Y		4		/* ウィンドウ内の枠と紙の隙間最小値 */
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたので
//	定義を削除

//	状況によりメニューの表示を変えるコマンドリスト(SetMenuFuncSelで使用)
//		2010/5/19	Uchi
//		2012/10/19	syat	各国語対応のため定数化
struct SFuncMenuName {
	EFunctionCode	eFunc;
	int				nNameId[2];		// 選択文字列ID
};

static const SFuncMenuName	sFuncMenuName[] = {
	{F_RECKEYMACRO,			{F_RECKEYMACRO_REC,				F_RECKEYMACRO_APPE}},
	{F_SAVEKEYMACRO,		{F_SAVEKEYMACRO_REC,			F_SAVEKEYMACRO_APPE}},
	{F_LOADKEYMACRO,		{F_LOADKEYMACRO_REC,			F_LOADKEYMACRO_APPE}},
	{F_EXECKEYMACRO,		{F_EXECKEYMACRO_REC,			F_EXECKEYMACRO_APPE}},
	{F_SPLIT_V,				{F_SPLIT_V_ON,					F_SPLIT_V_OFF}},
	{F_SPLIT_H,				{F_SPLIT_H_ON,					F_SPLIT_H_OFF}},
	{F_SPLIT_VH,			{F_SPLIT_VH_ON,					F_SPLIT_VH_OFF}},
	{F_TAB_CLOSEOTHER,		{F_TAB_CLOSEOTHER_TAB,			F_TAB_CLOSEOTHER_WINDOW}},
	{F_TOPMOST,				{F_TOPMOST_SET,					F_TOPMOST_REL}},
	{F_BIND_WINDOW,			{F_TAB_GROUPIZE,				F_TAB_GROUPDEL}},
	{F_SHOWTOOLBAR,			{F_SHOWTOOLBAR_ON,				F_SHOWTOOLBAR_OFF}},
	{F_SHOWFUNCKEY,			{F_SHOWFUNCKEY_ON,				F_SHOWFUNCKEY_OFF}},
	{F_SHOWTAB,				{F_SHOWTAB_ON,					F_SHOWTAB_OFF}},
	{F_SHOWSTATUSBAR,		{F_SHOWSTATUSBAR_ON,			F_SHOWSTATUSBAR_OFF}},
	{F_SHOWMINIMAP,			{F_SHOWMINIMAP_ON,				F_SHOWMINIMAP_OFF}},
	{F_TOGGLE_KEY_SEARCH,	{F_TOGGLE_KEY_SEARCH_ON,		F_TOGGLE_KEY_SEARCH_OFF}},
};

//! Owns the runtime lease while callbacks are in flight.
//!
//! The high bit of m_admission is the close fence and the remaining bits count
//! admitted callback scopes.  Admission and closing are one CAS/fetch_or
//! protocol, so a callback that wins the race with Close keeps the lease alive
//! until its RuntimeCall leaves.  Retirement is still non-blocking: the last
//! callback, or the close path when the count is already zero, hands the lease
//! to FrameRuntimeRetirementService.
struct CEditWndFrameRuntimeLeaseHolder final {
	using Runtime = workbench::rendering::FrameCoordinatorRuntime;
	using Lease = workbench::rendering::FrameRuntimeRetirementLease;

	explicit CEditWndFrameRuntimeLeaseHolder(std::unique_ptr<Lease> lease) noexcept
		: m_lease(std::move(lease))
	{
	}

	CEditWndFrameRuntimeLeaseHolder(const CEditWndFrameRuntimeLeaseHolder&) = delete;
	CEditWndFrameRuntimeLeaseHolder& operator=(const CEditWndFrameRuntimeLeaseHolder&) = delete;

	[[nodiscard]] Runtime* RuntimePointer() const noexcept
	{
		return m_lease != nullptr ? m_lease->Runtime() : nullptr;
	}

	//! Closes admission without waiting for already admitted callbacks.
	void CloseIngress() noexcept
	{
		m_admission.fetch_or(kClosedBit, std::memory_order_acq_rel);
	}

	//! Requests bounded asynchronous retirement. A callback may complete this.
	void RequestRetirement() noexcept
	{
		CloseIngress();
		m_retirementRequested.store(true, std::memory_order_release);
		TryRetire();
	}

	[[nodiscard]] std::shared_ptr<workbench::rendering::FrameRuntimeRetirementObservation>
		Observation() const noexcept
	{
		return m_observation.load(std::memory_order_acquire);
	}

	private:
	friend struct CEditWndFrameRuntimeCallbackState;

	static constexpr std::uint64_t kClosedBit = std::uint64_t{1} << 63;
	static constexpr std::uint64_t kActiveMask = ~kClosedBit;

	//! Returns a runtime only while this holder owns one active admission.
	[[nodiscard]] Runtime* TryEnter() noexcept
	{
		auto admission = m_admission.load(std::memory_order_acquire);
		for (;;) {
			if ((admission & kClosedBit) != 0
				|| (admission & kActiveMask) == kActiveMask) {
				return nullptr;
			}
			if (m_admission.compare_exchange_weak(admission, admission + 1,
				std::memory_order_acq_rel, std::memory_order_acquire)) {
				break;
			}
		}
		Runtime* const runtime = RuntimePointer();
		if (runtime == nullptr) {
			Leave();
			return nullptr;
		}
		return runtime;
	}

	public:
	void Leave() noexcept
	{
		const auto previous = m_admission.fetch_sub(1, std::memory_order_acq_rel);
		if ((previous & kActiveMask) == 1) TryRetire();
	}

	private:
	void TryRetire() noexcept
	{
		if (!m_retirementRequested.load(std::memory_order_acquire)) return;
		const auto admission = m_admission.load(std::memory_order_acquire);
		if ((admission & kClosedBit) == 0 || (admission & kActiveMask) != 0) return;
		if (m_retirementStarted.exchange(true, std::memory_order_acq_rel)) return;
		if (m_lease == nullptr) return;
		const auto result = m_lease->RetireNow();
		m_observation.store(result.observation, std::memory_order_release);
		if (!result.Accepted()) {
			::OutputDebugStringW(L"Sakura Editor NEXT: frame runtime retirement handoff failed.\n");
		}
	}

	std::unique_ptr<Lease> m_lease;
	std::atomic<std::uint64_t> m_admission{ 0 };
	std::atomic_bool m_retirementRequested{ false };
	std::atomic_bool m_retirementStarted{ false };
	std::atomic<std::shared_ptr<workbench::rendering::FrameRuntimeRetirementObservation>>
		m_observation;
};

//! Shared callback gate for the per-window presentation runtime.
//!
//! Terminal callbacks are delivered by the terminal HWND message path and the
//! SCM callbacks are delivered by its retained-list WM_PAINT path.  The lambdas
//! retain this state, never CEditWnd or a raw runtime pointer.  Close fences new
//! admissions, child sinks are detached, and only then is retirement requested.
struct CEditWndFrameRuntimeCallbackState final {
	using Runtime = workbench::rendering::FrameCoordinatorRuntime;
	using RuntimeRegistration = workbench::rendering::FrameNativeSurfaceRegistration;
	using RuntimeFrame = workbench::rendering::FrameNativeSurfaceFrame;
	using ScmRegistration = workbench::scm::ScmNativeSurfaceRegistration;
	using ScmFrame = workbench::scm::ScmNativeSurfaceFrame;

	struct RuntimeCall final {
		RuntimeCall(std::shared_ptr<CEditWndFrameRuntimeLeaseHolder> owner,
			Runtime* runtime) noexcept
			: owner(std::move(owner)), runtime(runtime)
		{
		}
		RuntimeCall(const RuntimeCall&) = delete;
		RuntimeCall& operator=(const RuntimeCall&) = delete;
		RuntimeCall(RuntimeCall&& other) noexcept
			: owner(std::move(other.owner)), runtime(std::exchange(other.runtime, nullptr))
		{
		}
		RuntimeCall& operator=(RuntimeCall&&) = delete;
		~RuntimeCall() noexcept
		{
			if (owner != nullptr) owner->Leave();
		}
		std::shared_ptr<CEditWndFrameRuntimeLeaseHolder> owner;
		Runtime* runtime = nullptr;
	};

	void Bind(std::shared_ptr<CEditWndFrameRuntimeLeaseHolder> owner) noexcept
	{
		m_owner = std::move(owner);
		m_closed.store(m_owner == nullptr, std::memory_order_release);
	}

	//! Fences future calls without joining a worker or taking a UI mutex.
	void Close() noexcept
	{
		m_closed.store(true, std::memory_order_release);
		if (m_owner != nullptr) m_owner->CloseIngress();
	}

	//! Transfers the lease to the bounded reaper after all child sinks are gone.
	void RequestRetirement() noexcept
	{
		if (m_owner != nullptr) m_owner->RequestRetirement();
	}

	void SetDisplayEpoch(const std::uint64_t epoch) noexcept
	{
		if (epoch != 0) m_displayEpoch.store(epoch, std::memory_order_release);
	}

	[[nodiscard]] std::uint64_t DisplayEpoch() const noexcept
	{
		const auto epoch = m_displayEpoch.load(std::memory_order_acquire);
		return epoch == 0 ? 1 : epoch;
	}

	[[nodiscard]] bool Register(const RuntimeRegistration& registration) noexcept
	{
		if (auto call = BeginCall()) {
			return call->runtime->RegisterPresentedSurface(registration).Accepted();
		}
		return false;
	}

	[[nodiscard]] bool Update(const RuntimeRegistration& registration) noexcept
	{
		if (auto call = BeginCall()) {
			return call->runtime->UpdatePresentedSurface(registration).Accepted();
		}
		return false;
	}

	void CloseSurface(const workbench::rendering::FrameSurfaceId surfaceId,
		const std::uint64_t lifetime) noexcept
	{
		if (auto call = BeginCall()) {
			(void)call->runtime->CloseSurface(surfaceId, lifetime);
		}
	}

	[[nodiscard]] bool SubmitAccepted(
		std::shared_ptr<const RuntimeFrame> frame) noexcept
	{
		if (frame == nullptr) return false;
		if (auto call = BeginCall()) {
			return call->runtime->SubmitNativeSurfaceFrame(std::move(frame)).Accepted();
		}
		return false;
	}

	void Submit(std::shared_ptr<const RuntimeFrame> frame) noexcept
	{
		(void)SubmitAccepted(std::move(frame));
	}

	[[nodiscard]] bool RegisterScm(const ScmRegistration& registration) noexcept
	{
		return Register(ToRuntimeRegistration(registration));
	}

	[[nodiscard]] bool UpdateScm(const ScmRegistration& registration) noexcept
	{
		return Update(ToRuntimeRegistration(registration));
	}

	void CloseScm(const workbench::rendering::FrameSurfaceId surfaceId,
		const std::uint64_t lifetime) noexcept
	{
		CloseSurface(surfaceId, lifetime);
	}

	[[nodiscard]] bool SubmitScm(std::shared_ptr<const ScmFrame> frame) noexcept
	{
		if (frame == nullptr || !frame->IsValid()) return false;
		try {
			auto runtimeFrame = std::make_shared<const RuntimeFrame>(RuntimeFrame{
				.surfaceId = frame->surfaceId,
				.surfaceLifetimeEpoch = frame->surfaceLifetimeEpoch,
				.deviceEpoch = frame->deviceEpoch,
				.displayEpoch = DisplayEpoch(),
				.layoutEpoch = frame->layoutEpoch,
				.requestId = frame->requestId,
				.width = frame->width,
				.height = frame->height,
				.pitch = frame->payloadPitch,
				.dirtyRect = frame->dirtyRect,
				.compactDirtyPayload = true,
				.pixels = frame->pixels,
			});
			if (!runtimeFrame->IsValid()) return false;
			if (auto call = BeginCall()) {
				return call->runtime->SubmitNativeSurfaceFrame(std::move(runtimeFrame)).Accepted();
			}
		} catch (...) {
		}
		return false;
	}

private:
	[[nodiscard]] std::optional<RuntimeCall> BeginCall() noexcept
	{
		if (m_closed.load(std::memory_order_acquire)) return std::nullopt;
		auto owner = m_owner;
		if (owner == nullptr) return std::nullopt;
		Runtime* const runtime = owner->TryEnter();
		if (runtime == nullptr || m_closed.load(std::memory_order_acquire)) {
			if (runtime != nullptr) owner->Leave();
			return std::nullopt;
		}
		return RuntimeCall(std::move(owner), runtime);
	}

	[[nodiscard]] static RuntimeRegistration ToRuntimeRegistration(
		const ScmRegistration& registration) noexcept
	{
		return RuntimeRegistration{
			.presentation = workbench::rendering::FramePresentationSurfaceSpec{
				.surfaceId = registration.surfaceId,
				.surfaceLifetimeEpoch = registration.surfaceLifetimeEpoch,
				.deviceEpoch = registration.deviceEpoch,
				.layoutEpoch = registration.layoutEpoch,
				.width = registration.width,
				.height = registration.height,
				.visible = registration.visible,
			},
			.targetWindow = registration.targetWindow,
			.x = registration.x,
			.y = registration.y,
		};
	}

	std::shared_ptr<CEditWndFrameRuntimeLeaseHolder> m_owner;
	std::atomic_bool m_closed{ true };
	std::atomic<std::uint64_t> m_displayEpoch{ 1 };
};

struct CEditWndNativeSurfaceBinding final {
	const void* owner = nullptr;
	HWND window = nullptr;
	std::uint64_t lifetimeEpoch = 0;
	workbench::rendering::FrameNativeSurfacePayloadTarget target{};
	bool targetInitialized = false;
};

struct CEditWndFrameRuntimeState final {
	workbench::rendering::FrameCadenceSource cadenceSource;
	std::shared_ptr<CEditWndFrameRuntimeLeaseHolder> leaseHolder;
	std::unique_ptr<workbench::rendering::FrameWindowTransaction> windowTransaction;
	std::shared_ptr<workbench::rendering::FrameRuntimeRetirementObservation> retirementObservation;
	std::shared_ptr<CEditWndFrameRuntimeCallbackState> callbackState;
	terminal::TerminalNativeFrameBridgePtr terminalFrameBridge;
	std::uint64_t scmSurfaceLifetimeEpoch = 1;
	std::uint64_t scmDeviceEpoch = 1;
	std::uint64_t scmLayoutEpoch = 1;
	HWND scmTargetWindow = nullptr;
	LONG scmTargetX = 0;
	LONG scmTargetY = 0;
	std::uint64_t scmTargetDeviceEpoch = 1;
	bool scmTargetVisible = false;
	bool scmTargetInitialized = false;
	std::array<CEditWndNativeSurfaceBinding, 4> editorBindings{};
	CEditWndNativeSurfaceBinding minimapBinding{};
	CEditWndNativeSurfaceBinding markdownBinding{};
	CEditWndNativeSurfaceBinding explorerBinding{};
	CEditWndNativeSurfaceBinding searchBinding{};
	CEditWndNativeSurfaceBinding outlineBinding{};
};

namespace {

constexpr std::string_view kLegacyEditorInputId = "legacy.editor.1";
// Stable only within the frame-runtime surface namespace. SCM's retained
// Changes list is one logical surface even though its source is a child HWND.
constexpr workbench::rendering::FrameSurfaceId kScmNativeSurfaceId =
	0x53434d0000000001ULL;
constexpr workbench::rendering::FrameSurfaceId kExplorerNativeSurfaceId =
	0x4558500000000001ULL;
constexpr workbench::rendering::FrameSurfaceId kSearchNativeSurfaceId =
	0x5345410000000001ULL;
constexpr workbench::rendering::FrameSurfaceId kOutlineNativeSurfaceId =
	0x4f55540000000001ULL;

//! Width of the frame-edge drop zone that stands in for a hidden side bar during a
//! composite drag. VS Code accepts an edge drop to reveal the Secondary Side Bar; the
//! exact strip width is a native detail with no upstream counterpart.
constexpr int kSideBarDropEdgeDip = 48;

//! Maps the legacy shared-memory active tool onto a Primary Side Bar ViewContainer.
//! Outline and Terminal are not Primary Side Bar containers, so both fall back to
//! Explorer: Outline is a View nested in Explorer, and Terminal lives in the Panel.
[[nodiscard]] std::string_view SidebarPageForLegacyTool(EWorkbenchActiveTool tool) noexcept
{
	namespace pageIds = workbench::viewcontainer::pageIds;
	switch (tool) {
	case WORKBENCH_TOOL_SCM: return pageIds::SourceControl;
	case WORKBENCH_TOOL_EXPLORER:
	case WORKBENCH_TOOL_OUTLINE:
	case WORKBENCH_TOOL_TERMINAL:
		break;
	}
	return pageIds::Explorer;
}

//! Projects the runtime workspace kind into the SCM provider's welcome-state
//! vocabulary.  SCM distinguishes an empty workbench, a folder, and a
//! multi-folder workspace even when the native Explorer currently exposes only
//! one semantic root.
[[nodiscard]] workbench::scm::EGitScmWelcomeWorkspaceState ScmWelcomeWorkspaceState(
	const config::WorkspaceContextSnapshot& snapshot) noexcept
{
	switch (snapshot.kind) {
	case config::EWorkspaceKind::Folder:
		return snapshot.folders.size() == 1
			? workbench::scm::EGitScmWelcomeWorkspaceState::Folder
			: (snapshot.folders.empty()
				? workbench::scm::EGitScmWelcomeWorkspaceState::Empty
				: workbench::scm::EGitScmWelcomeWorkspaceState::WorkspaceWithFolders);
	case config::EWorkspaceKind::Workspace:
		return snapshot.folders.empty()
			? workbench::scm::EGitScmWelcomeWorkspaceState::WorkspaceWithoutFolders
			: workbench::scm::EGitScmWelcomeWorkspaceState::WorkspaceWithFolders;
	case config::EWorkspaceKind::Empty:
	default:
		return workbench::scm::EGitScmWelcomeWorkspaceState::Empty;
	}
}

/*!
	@brief Native ViewContainers available before the page registry is composed.

	The normal path derives renderable identities from `CViewContainerPages`, including pages
	projected from extension contributions. This fallback is used only before that registry exists.
*/
constexpr std::array kFallbackRenderableContainers{
	std::string_view(workbench::layout::ids::viewContainer::Explorer),
	std::string_view(workbench::layout::ids::viewContainer::Search),
	std::string_view(workbench::layout::ids::viewContainer::SourceControl),
	std::string_view(workbench::layout::ids::viewContainer::Extensions),
};

[[nodiscard]] std::wstring LocalizedWorkbenchString(UINT resourceId)
{
	if (resourceId == 0) return {};
	return std::wstring(CSelectLang::LoadStringW(resourceId));
}

[[nodiscard]] std::wstring LocalizedWorkbenchCommandTitle(
	std::string_view commandId, std::wstring_view fallback)
{
	const auto resourceId = workbench::commands::ResolveBuiltinWorkbenchCommandTitleResourceId(commandId);
	if (const auto localized = LocalizedWorkbenchString(resourceId); !localized.empty()) return localized;
	return std::wstring(fallback);
}

[[nodiscard]] std::wstring ResolveLocalizedWorkbenchCommandTitle(
	const workbench::commands::WorkbenchCommandDescriptor& descriptor)
{
	return LocalizedWorkbenchCommandTitle(descriptor.id, u8stowcs(descriptor.title));
}

void ReplaceLocalizedArgument(std::wstring& text, std::wstring_view argument)
{
	constexpr std::wstring_view kPlaceholder = L"{0}";
	for (std::size_t position = 0;;) {
		position = text.find(kPlaceholder, position);
		if (position == std::wstring::npos) return;
		text.replace(position, kPlaceholder.size(), argument);
		position += argument.size();
	}
}

[[nodiscard]] std::wstring ResolveLocalizedScmText(
	workbench::scm::EScmTextKey key, std::wstring_view argument)
{
	UINT resourceId = 0;
	switch (key) {
	case workbench::scm::EScmTextKey::SourceControlTitle:
		resourceId = STR_WORKBENCH_SOURCE_CONTROL_TITLE; break;
	case workbench::scm::EScmTextKey::RepositoriesTitle:
		resourceId = STR_WORKBENCH_SCM_REPOSITORIES_TITLE; break;
	case workbench::scm::EScmTextKey::ChangesTitle:
		resourceId = STR_WORKBENCH_SCM_CHANGES_TITLE; break;
	case workbench::scm::EScmTextKey::GraphTitle:
		resourceId = STR_WORKBENCH_SCM_GRAPH_TITLE; break;
	case workbench::scm::EScmTextKey::GraphUnavailable:
		resourceId = STR_WORKBENCH_SCM_GRAPH_UNAVAILABLE; break;
	case workbench::scm::EScmTextKey::GitFolderNoRepository:
		resourceId = STR_WORKBENCH_GIT_FOLDER_NO_REPOSITORY; break;
	case workbench::scm::EScmTextKey::GitEmptyWorkbench:
		resourceId = STR_WORKBENCH_GIT_EMPTY_WORKBENCH; break;
	case workbench::scm::EScmTextKey::GitWorkspaceNoRepository:
		resourceId = STR_WORKBENCH_GIT_WORKSPACE_NO_REPOSITORY; break;
	case workbench::scm::EScmTextKey::GitEmptyWorkspace:
		resourceId = STR_WORKBENCH_GIT_EMPTY_WORKSPACE; break;
	case workbench::scm::EScmTextKey::GitInitializeRepository:
		resourceId = STR_WORKBENCH_GIT_INITIALIZE_REPOSITORY; break;
	case workbench::scm::EScmTextKey::GitCloneRepository:
		resourceId = STR_WORKBENCH_GIT_CLONE_REPOSITORY; break;
	case workbench::scm::EScmTextKey::GitAddFolderToWorkspace:
		resourceId = STR_WORKBENCH_GIT_ADD_FOLDER_TO_WORKSPACE; break;
	case workbench::scm::EScmTextKey::GitChooseFolder:
		resourceId = STR_WORKBENCH_GIT_CHOOSE_FOLDER; break;
	case workbench::scm::EScmTextKey::GitOpenFolder:
		resourceId = STR_WORKBENCH_EXPLORER_OPEN_FOLDER; break;
	case workbench::scm::EScmTextKey::GitOpen:
		resourceId = STR_WORKBENCH_GIT_OPEN; break;
	case workbench::scm::EScmTextKey::GitInitHomeWarning:
		resourceId = STR_WORKBENCH_GIT_INIT_HOME_WARNING; break;
	case workbench::scm::EScmTextKey::GitInitOpenPrompt:
		resourceId = STR_WORKBENCH_GIT_INIT_OPEN_PROMPT; break;
	case workbench::scm::EScmTextKey::GitInitFolderPicker:
		resourceId = STR_WORKBENCH_GIT_INIT_FOLDER_PICKER; break;
	case workbench::scm::EScmTextKey::GitInitSuccess:
		resourceId = STR_WORKBENCH_GIT_INIT_SUCCESS; break;
	case workbench::scm::EScmTextKey::GitInitCancelledHome:
		resourceId = STR_WORKBENCH_GIT_INIT_CANCELLED_HOME; break;
	case workbench::scm::EScmTextKey::GitCloneOverwriteWarning:
		resourceId = STR_WORKBENCH_GIT_CLONE_OVERWRITE_WARNING; break;
	case workbench::scm::EScmTextKey::GitOverwrite:
		resourceId = STR_WORKBENCH_GIT_OVERWRITE; break;
	case workbench::scm::EScmTextKey::GitRepositoryUrl:
		resourceId = STR_WORKBENCH_GIT_REPOSITORY_URL; break;
	case workbench::scm::EScmTextKey::GitInvalidUrlFolder:
		resourceId = STR_WORKBENCH_GIT_INVALID_URL_FOLDER; break;
	case workbench::scm::EScmTextKey::GitRepositoryLocation:
		resourceId = STR_WORKBENCH_GIT_REPOSITORY_LOCATION; break;
	case workbench::scm::EScmTextKey::GitCloneNonEmpty:
		resourceId = STR_WORKBENCH_GIT_CLONE_NONEMPTY; break;
	case workbench::scm::EScmTextKey::GitCloneCancelled:
		resourceId = STR_WORKBENCH_GIT_CLONE_CANCELLED; break;
	case workbench::scm::EScmTextKey::GitOpenChanges:
		resourceId = STR_WORKBENCH_GIT_OPEN_CHANGES; break;
	case workbench::scm::EScmTextKey::GitOpenFile:
		resourceId = STR_WORKBENCH_GIT_OPEN_FILE; break;
	case workbench::scm::EScmTextKey::GitStageChanges:
		resourceId = STR_WORKBENCH_GIT_STAGE_CHANGES; break;
	case workbench::scm::EScmTextKey::GitUnstageChanges:
		resourceId = STR_WORKBENCH_GIT_UNSTAGE_CHANGES; break;
	case workbench::scm::EScmTextKey::GitDiscardChanges:
		resourceId = STR_WORKBENCH_GIT_DISCARD_CHANGES; break;
	case workbench::scm::EScmTextKey::GitStageAllChanges:
		resourceId = STR_WORKBENCH_GIT_STAGE_ALL_CHANGES; break;
	case workbench::scm::EScmTextKey::GitUnstageAllChanges:
		resourceId = STR_WORKBENCH_GIT_UNSTAGE_ALL_CHANGES; break;
	case workbench::scm::EScmTextKey::GitDiscardAllChanges:
		resourceId = STR_WORKBENCH_GIT_DISCARD_ALL_CHANGES; break;
	case workbench::scm::EScmTextKey::GitPublishBranch:
		resourceId = STR_WORKBENCH_GIT_PUBLISH_BRANCH; break;
	case workbench::scm::EScmTextKey::GitSynchronizeChanges:
		resourceId = STR_WORKBENCH_GIT_SYNCHRONIZE_CHANGES; break;
	case workbench::scm::EScmTextKey::GitCheckoutBranchTag:
		resourceId = STR_WORKBENCH_GIT_CHECKOUT_BRANCH_TAG; break;
	case workbench::scm::EScmTextKey::GitCommitMessage:
		resourceId = STR_WORKBENCH_GIT_COMMIT_MESSAGE; break;
	case workbench::scm::EScmTextKey::GitCommitAction:
		resourceId = STR_WORKBENCH_GIT_COMMIT_ACTION; break;
	case workbench::scm::EScmTextKey::GitCommitAmendAction:
		resourceId = STR_WORKBENCH_GIT_COMMIT_AMEND_ACTION; break;
	case workbench::scm::EScmTextKey::GitCommitAndPushAction:
		resourceId = STR_WORKBENCH_GIT_COMMIT_AND_PUSH_ACTION; break;
	case workbench::scm::EScmTextKey::GitCommitAndSyncAction:
		resourceId = STR_WORKBENCH_GIT_COMMIT_AND_SYNC_ACTION; break;
	}
	std::wstring localized = LocalizedWorkbenchString(resourceId);
	if (!localized.empty() && !argument.empty()) ReplaceLocalizedArgument(localized, argument);
	return localized;
}

//! The SCM orchestration models intentionally remain HWND- and resource-free.
//! Their stable text keys are resolved only at this composition boundary, where
//! Sakura's selected language DLL is available.  That keeps headless tests on
//! their English fallback while the live workbench follows language changes.
[[nodiscard]] std::wstring ResolveLocalizedScmTextKey(
	std::string_view key, std::wstring_view argument)
{
	struct TextEntry {
		const std::string_view key{};
		const UINT resourceId{};
	};
	constexpr std::array<TextEntry, 105> kEntries{{
		{ "GitRefBranches", STR_WORKBENCH_GIT_REF_BRANCHES },
		{ "GitRefRemoteBranches", STR_WORKBENCH_GIT_REF_REMOTE_BRANCHES },
		{ "GitRefTags", STR_WORKBENCH_GIT_REF_TAGS },
		{ "GitRefRemoteBranchAt", STR_WORKBENCH_GIT_REF_REMOTE_BRANCH_AT },
		{ "GitRefTagAt", STR_WORKBENCH_GIT_REF_TAG_AT },
		{ "GitCreateBranch", STR_WORKBENCH_GIT_CREATE_BRANCH },
		{ "GitCreateBranchFrom", STR_WORKBENCH_GIT_CREATE_BRANCH_FROM },
		{ "GitCheckoutDetached", STR_WORKBENCH_GIT_CHECKOUT_DETACHED },
		{ "GitCheckoutPlaceholder", STR_WORKBENCH_GIT_CHECKOUT_PLACEHOLDER },
		{ "GitCheckoutDetachedPlaceholder", STR_WORKBENCH_GIT_CHECKOUT_DETACHED_PLACEHOLDER },
		{ "GitBranchAlreadyExists", STR_WORKBENCH_GIT_BRANCH_ALREADY_EXISTS },
		{ "GitNewBranchWillBe", STR_WORKBENCH_GIT_NEW_BRANCH_WILL_BE },
		{ "GitBranchNamePrompt", STR_WORKBENCH_GIT_BRANCH_NAME_PROMPT },
		{ "GitBranchNamePlaceholder", STR_WORKBENCH_GIT_BRANCH_NAME_PLACEHOLDER },
		{ "GitBranchFromPlaceholder", STR_WORKBENCH_GIT_BRANCH_FROM_PLACEHOLDER },
		{ "GitCheckoutNoPresenter", STR_WORKBENCH_GIT_CHECKOUT_NO_PRESENTER },
		{ "GitBranchNoPresenter", STR_WORKBENCH_GIT_BRANCH_NO_PRESENTER },
		{ "GitUnsavedOne", STR_WORKBENCH_GIT_UNSAVED_ONE },
		{ "GitUnsavedMany", STR_WORKBENCH_GIT_UNSAVED_MANY },
		{ "GitSaveAllCommitChanges", STR_WORKBENCH_GIT_SAVE_ALL_COMMIT_CHANGES },
		{ "GitCommitChanges", STR_WORKBENCH_GIT_COMMIT_CHANGES },
		{ "GitNoStagedChanges", STR_WORKBENCH_GIT_NO_STAGED_CHANGES },
		{ "GitYes", STR_WORKBENCH_GIT_YES },
		{ "GitNoChanges", STR_WORKBENCH_GIT_NO_CHANGES },
		{ "GitCreateEmptyCommit", STR_WORKBENCH_GIT_CREATE_EMPTY_COMMIT },
		{ "GitNoVerifyWarning", STR_WORKBENCH_GIT_NO_VERIFY_WARNING },
		{ "GitOk", STR_WORKBENCH_GIT_OK },
		{ "GitUndoMergeWarning", STR_WORKBENCH_GIT_UNDO_MERGE_WARNING },
		{ "GitUndoMergeCommit", STR_WORKBENCH_GIT_UNDO_MERGE_COMMIT },
		{ "GitRebaseUnsupported", STR_WORKBENCH_GIT_REBASE_UNSUPPORTED },
		{ "GitNoVerifyDisabled", STR_WORKBENCH_GIT_NO_VERIFY_DISABLED },
		{ "GitCommitMessage", STR_WORKBENCH_GIT_COMMIT_MESSAGE },
		{ "GitCommitMessageOnBranch", STR_WORKBENCH_GIT_COMMIT_MESSAGE_ON_BRANCH },
		{ "GitCommitMessagePrompt", STR_WORKBENCH_GIT_COMMIT_MESSAGE_PROMPT },
		{ "GitCommitNoInvoker", STR_WORKBENCH_GIT_COMMIT_NO_INVOKER },
		{ "GitCommitNoConfirmation", STR_WORKBENCH_GIT_COMMIT_NO_CONFIRMATION },
		{ "GitUnsavedSaveFailed", STR_WORKBENCH_GIT_UNSAVED_SAVE_FAILED },
		{ "GitCommitNoMessagePrompt", STR_WORKBENCH_GIT_COMMIT_NO_MESSAGE_PROMPT },
		{ "GitUndoNoInvoker", STR_WORKBENCH_GIT_UNDO_NO_INVOKER },
		{ "GitIrreversibleOne", STR_WORKBENCH_GIT_IRREVERSIBLE_ONE },
		{ "GitIrreversibleMany", STR_WORKBENCH_GIT_IRREVERSIBLE_MANY },
		{ "GitDeleteUntrackedOne", STR_WORKBENCH_GIT_DELETE_UNTRACKED_ONE },
		{ "GitDeleteUntrackedMany", STR_WORKBENCH_GIT_DELETE_UNTRACKED_MANY },
		{ "GitRestoreOneRecycleBin", STR_WORKBENCH_GIT_RESTORE_ONE_RECYCLE_BIN },
		{ "GitRestoreManyRecycleBin", STR_WORKBENCH_GIT_RESTORE_MANY_RECYCLE_BIN },
		{ "GitMoveToRecycleBin", STR_WORKBENCH_GIT_MOVE_TO_RECYCLE_BIN },
		{ "GitDeleteFile", STR_WORKBENCH_GIT_DELETE_FILE },
		{ "GitDeleteAllFiles", STR_WORKBENCH_GIT_DELETE_ALL_FILES },
		{ "GitRestoreOne", STR_WORKBENCH_GIT_RESTORE_ONE },
		{ "GitRestoreMany", STR_WORKBENCH_GIT_RESTORE_MANY },
		{ "GitRestoreFile", STR_WORKBENCH_GIT_RESTORE_FILE },
		{ "GitRestoreAllFiles", STR_WORKBENCH_GIT_RESTORE_ALL_FILES },
		{ "GitDiscardOne", STR_WORKBENCH_GIT_DISCARD_ONE },
		{ "GitDiscardMany", STR_WORKBENCH_GIT_DISCARD_MANY },
		{ "GitDiscardFile", STR_WORKBENCH_GIT_DISCARD_FILE },
		{ "GitDiscardAllFiles", STR_WORKBENCH_GIT_DISCARD_ALL_FILES },
		{ "GitDiscardTrackedOne", STR_WORKBENCH_GIT_DISCARD_TRACKED_ONE },
		{ "GitDiscardTrackedMany", STR_WORKBENCH_GIT_DISCARD_TRACKED_MANY },
		{ "GitDiscardTrackedFile", STR_WORKBENCH_GIT_DISCARD_TRACKED_FILE },
		{ "GitDiscardAllTrackedFiles", STR_WORKBENCH_GIT_DISCARD_ALL_TRACKED_FILES },
		{ "GitRecycleBinFailed", STR_WORKBENCH_GIT_RECYCLE_BIN_FAILED },
		{ "GitStageNoInvoker", STR_WORKBENCH_GIT_STAGE_NO_INVOKER },
		{ "GitStageMergeUnsupported", STR_WORKBENCH_GIT_STAGE_MERGE_UNSUPPORTED },
		{ "GitUnstageNoInvoker", STR_WORKBENCH_GIT_UNSTAGE_NO_INVOKER },
		{ "GitDiscardNoInvoker", STR_WORKBENCH_GIT_DISCARD_NO_INVOKER },
		{ "GitDiscardNoConfirmation", STR_WORKBENCH_GIT_DISCARD_NO_CONFIRMATION },
		{ "GitSyncConfirmation", STR_WORKBENCH_GIT_SYNC_CONFIRMATION },
		{ "GitPublishBranchPrompt", STR_WORKBENCH_GIT_PUBLISH_BRANCH_PROMPT },
		{ "GitMaybeRebasedNoName", STR_WORKBENCH_GIT_MAYBE_REBASED_NO_NAME },
		{ "GitMaybeRebased", STR_WORKBENCH_GIT_MAYBE_REBASED },
		{ "GitPull", STR_WORKBENCH_GIT_PULL },
		{ "GitDontPull", STR_WORKBENCH_GIT_DONT_PULL },
		{ "GitFetchPlaceholder", STR_WORKBENCH_GIT_FETCH_PLACEHOLDER },
		{ "GitSyncNoPresenter", STR_WORKBENCH_GIT_SYNC_NO_PRESENTER },
		{ "GitNoFetchRemotes", STR_WORKBENCH_GIT_NO_FETCH_REMOTES },
		{ "GitNoPullRemotes", STR_WORKBENCH_GIT_NO_PULL_REMOTES },
		{ "GitNoPushRemotes", STR_WORKBENCH_GIT_NO_PUSH_REMOTES },
		{ "GitNoBranchToPush", STR_WORKBENCH_GIT_NO_BRANCH_TO_PUSH },
		{ "GitNoPublishRemotes", STR_WORKBENCH_GIT_NO_PUBLISH_REMOTES },
		{ "GitSyncDismissed", STR_WORKBENCH_GIT_SYNC_DISMISSED },
		{ "GitSyncNoHead", STR_WORKBENCH_GIT_SYNC_NO_HEAD },
		{ "GitSyncReadOnlyRemote", STR_WORKBENCH_GIT_SYNC_READ_ONLY_REMOTE },
		{ "GitSyncNothingToPush", STR_WORKBENCH_GIT_SYNC_NOTHING_TO_PUSH },
		{ "GitDirtyWorkTree", STR_WORKBENCH_GIT_DIRTY_WORK_TREE },
		{ "GitPushRejected", STR_WORKBENCH_GIT_PUSH_REJECTED },
		{ "GitForcePushRejected", STR_WORKBENCH_GIT_FORCE_PUSH_REJECTED },
		{ "GitConflict", STR_WORKBENCH_GIT_CONFLICT },
		{ "GitAuthenticationFailed", STR_WORKBENCH_GIT_AUTHENTICATION_FAILED },
		{ "GitAuthenticationFailedTarget", STR_WORKBENCH_GIT_AUTHENTICATION_FAILED_TARGET },
		{ "GitNoUserNameConfigured", STR_WORKBENCH_GIT_NO_USER_NAME_CONFIGURED },
		{ "GitFetchAllRemotes", STR_WORKBENCH_GIT_FETCH_ALL_REMOTES },
		{ "GitPublishRemotePicker", STR_WORKBENCH_GIT_PUBLISH_REMOTE_PICKER },
		{ "GitDiffIndex", STR_WORKBENCH_GIT_DIFF_TITLE_INDEX },
		{ "GitDiffWorkingTree", STR_WORKBENCH_GIT_DIFF_TITLE_WORKING_TREE },
		{ "GitDiffDeleted", STR_WORKBENCH_GIT_DIFF_TITLE_DELETED },
		{ "GitDiffTheirs", STR_WORKBENCH_GIT_DIFF_TITLE_THEIRS },
		{ "GitDiffOurs", STR_WORKBENCH_GIT_DIFF_TITLE_OURS },
		{ "GitDiffUntracked", STR_WORKBENCH_GIT_DIFF_TITLE_UNTRACKED },
		{ "GitDiffIntentToAdd", STR_WORKBENCH_GIT_DIFF_TITLE_INTENT_TO_ADD },
		{ "GitDiffTypeChanged", STR_WORKBENCH_GIT_DIFF_TITLE_TYPE_CHANGED },
		{ "GitScmMergeChanges", STR_WORKBENCH_GIT_SCM_MERGE_CHANGES },
		{ "GitScmStagedChanges", STR_WORKBENCH_GIT_SCM_STAGED_CHANGES },
		{ "GitScmChanges", STR_WORKBENCH_GIT_SCM_CHANGES },
		{ "GitScmUntrackedChanges", STR_WORKBENCH_GIT_SCM_UNTRACKED_CHANGES },
		{ "GitScmOpen", STR_WORKBENCH_GIT_OPEN },
	}};
	for (const auto& entry : kEntries) {
		if (entry.key != key) continue;
		auto localized = LocalizedWorkbenchString(entry.resourceId);
		if (!localized.empty() && !argument.empty()) ReplaceLocalizedArgument(localized, argument);
		return localized;
	}
	return {};
}

[[nodiscard]] std::wstring ResolveLocalizedActivityBarTitle(
	std::string_view containerId, std::wstring_view fallback)
{
	const auto resourceId = workbench::activity::ResolveActivityTitleResourceId(containerId);
	if (const auto localized = LocalizedWorkbenchString(resourceId); !localized.empty()) return localized;
	return std::wstring(fallback);
}

[[nodiscard]] std::wstring AccountDetail(const int labelResourceId, std::wstring_view value)
{
	auto label = LocalizedWorkbenchString(labelResourceId);
	if (label.empty()) return std::wstring(value);
	label.append(L": ");
	label.append(value);
	return label;
}

[[nodiscard]] CustomFrameAccountMenuModel ProjectAccountMenuModel(
	const workbench::account::AccountDiscoverySnapshot& snapshot)
{
	using workbench::account::EAccountDiscoveryState;
	using workbench::account::EAccountSourceState;
	using workbench::account::EGitHubAccountState;

	CustomFrameAccountMenuModel model;
	model.loadingFallback = LocalizedWorkbenchString(STR_WORKBENCH_ACCOUNT_LOADING);
	model.unavailableFallback = LocalizedWorkbenchString(STR_WORKBENCH_ACCOUNT_DISCOVERY_FAILED);
	model.absentFallback = LocalizedWorkbenchString(STR_WORKBENCH_ACCOUNT_NO_PROVIDER);
	if (snapshot.state == EAccountDiscoveryState::Loading) {
		model.state = CustomFrameAccountMenuState::Loading;
		return model;
	}
	if (snapshot.state == EAccountDiscoveryState::Stopped) {
		model.state = CustomFrameAccountMenuState::Absent;
		return model;
	}

	model.state = CustomFrameAccountMenuState::Available;
	if (snapshot.gitIdentity) {
		const auto& identity = *snapshot.gitIdentity;
		CustomFrameAccountMenuParent parent;
		if (!identity.userName.empty()) parent.label = identity.userName;
		if (!identity.userEmail.empty()) {
			if (!parent.label.empty()) {
				parent.label.append(L" <").append(identity.userEmail).append(L">");
			} else {
				parent.label = identity.userEmail;
			}
		}
		if (parent.label.empty()) parent.label = L"Git";
		else parent.label.append(L" (Git)");
		if (!identity.userName.empty()) parent.detailRows.push_back(L"user.name: " + identity.userName);
		if (!identity.userEmail.empty()) parent.detailRows.push_back(L"user.email: " + identity.userEmail);
		model.parents.push_back(std::move(parent));
	} else {
		CustomFrameAccountMenuParent parent{ .label = L"Git" };
		parent.detailRows.push_back(LocalizedWorkbenchString(
			snapshot.gitState == EAccountSourceState::Unconfigured
				? STR_WORKBENCH_ACCOUNT_GIT_IDENTITY_NOT_CONFIGURED
				: snapshot.gitState == EAccountSourceState::Unavailable
					? STR_WORKBENCH_ACCOUNT_GIT_UNAVAILABLE
					: STR_WORKBENCH_ACCOUNT_DISCOVERY_FAILED));
		model.parents.push_back(std::move(parent));
	}

	for (const auto& account : snapshot.githubAccounts) {
		CustomFrameAccountMenuParent parent;
		parent.label = account.login;
		parent.label.append(L" (GitHub CLI)");
		parent.detailRows.push_back(AccountDetail(STR_WORKBENCH_ACCOUNT_HOST, account.host));
		parent.detailRows.push_back(AccountDetail(
			STR_WORKBENCH_ACCOUNT_GIT_PROTOCOL, account.gitProtocol));
		const int statusResourceId = account.state != EGitHubAccountState::Success
			? STR_WORKBENCH_ACCOUNT_NEEDS_ATTENTION
			: account.active
				? STR_WORKBENCH_ACCOUNT_ACTIVE
				: STR_WORKBENCH_ACCOUNT_INACTIVE;
		parent.detailRows.push_back(AccountDetail(STR_WORKBENCH_ACCOUNT_STATUS,
			LocalizedWorkbenchString(statusResourceId)));
		model.parents.push_back(std::move(parent));
	}
	if (snapshot.githubAccounts.empty()) {
		CustomFrameAccountMenuParent parent{ .label = L"GitHub CLI" };
		parent.detailRows.push_back(LocalizedWorkbenchString(
			snapshot.githubState == EAccountSourceState::Ready
				? STR_WORKBENCH_ACCOUNT_GITHUB_CLI_NO_ACCOUNTS
				: snapshot.githubState == EAccountSourceState::Unavailable
					? STR_WORKBENCH_ACCOUNT_GITHUB_CLI_UNAVAILABLE
					: STR_WORKBENCH_ACCOUNT_DISCOVERY_FAILED));
		model.parents.push_back(std::move(parent));
	}
	return model;
}

[[nodiscard]] workbench::ActivityBarLocationMenuLabels ActivityBarLocationMenuLabels()
{
	const WORD language = PRIMARYLANGID(CSelectLang::getDefaultLangId());
	if (language == LANG_JAPANESE) {
		return { L"アクティビティ バーの位置", L"既定値", L"上部", L"下部" };
	}
	if (language == LANG_CHINESE) {
		return { L"活动栏位置", L"默认", L"顶部", L"底部" };
	}
	return {};
}

class ScopedWorkingCopyBackendEffect final {
public:
	explicit ScopedWorkingCopyBackendEffect(bool& value) noexcept
		: m_value(value)
		, m_previous(value)
	{
		m_value = true;
	}

	~ScopedWorkingCopyBackendEffect()
	{
		m_value = m_previous;
	}

	ScopedWorkingCopyBackendEffect(const ScopedWorkingCopyBackendEffect&) = delete;
	ScopedWorkingCopyBackendEffect& operator=(const ScopedWorkingCopyBackendEffect&) = delete;

private:
	bool& m_value;
	const bool m_previous;
};

class ScopedMouseWheelForward final {
public:
	explicit ScopedMouseWheelForward(bool& value) noexcept
		: m_value(value)
		, m_previous(value)
	{
		m_value = true;
	}

	~ScopedMouseWheelForward()
	{
		m_value = m_previous;
	}

	ScopedMouseWheelForward(const ScopedMouseWheelForward&) = delete;
	ScopedMouseWheelForward& operator=(const ScopedMouseWheelForward&) = delete;

private:
	bool& m_value;
	const bool m_previous;
};

class ScopedBooleanState final {
public:
	explicit ScopedBooleanState(bool& value, bool next = true) noexcept
		: m_value(value)
		, m_previous(value)
	{
		m_value = next;
	}

	~ScopedBooleanState()
	{
		m_value = m_previous;
	}

	ScopedBooleanState(const ScopedBooleanState&) = delete;
	ScopedBooleanState& operator=(const ScopedBooleanState&) = delete;

private:
	bool& m_value;
	const bool m_previous;
};

std::optional<std::wstring> PreviewProjectWorkspaceIdentity(
	config::EWorkspaceKind kind, platform::uri::Uri uri)
{
	config::CWorkspaceContextService preview(L"project-switch-preview");
	const auto before = preview.Snapshot();
	config::WorkspaceContextResult result;
	if (kind == config::EWorkspaceKind::Folder) {
		result = preview.SetFolder({
			.operation = { .operationId = "project-switch-folder-preview", .expectedRevision = before.revision },
			.folderUri = std::move(uri),
			.displayName = L"preview",
		});
	} else if (kind == config::EWorkspaceKind::Workspace) {
		result = preview.SetWorkspace({
			.operation = { .operationId = "project-switch-workspace-preview", .expectedRevision = before.revision },
			.workspaceConfigUri = std::move(uri),
		});
	} else {
		return std::nullopt;
	}
	if (result.outcome != config::EWorkspaceContextOutcome::Succeeded
		|| result.snapshot.workspaceIdentityKey.empty()) return std::nullopt;
	return result.snapshot.workspaceIdentityKey;
}

[[nodiscard]] std::wstring GetProcessCurrentDirectory()
{
	const DWORD required = ::GetCurrentDirectoryW(0, nullptr);
	if (required == 0) return {};
	std::wstring directory(required, L'\0');
	const DWORD copied = ::GetCurrentDirectoryW(required, directory.data());
	if (copied == 0 || copied >= required) return {};
	directory.resize(copied);
	return directory;
}

[[nodiscard]] std::wstring MakeAbsolutePath(std::wstring_view path)
{
	if (path.empty()) return {};
	const DWORD required = ::GetFullPathNameW(std::wstring(path).c_str(), 0, nullptr, nullptr);
	if (required == 0) return std::wstring(path);
	std::wstring absolute(required, L'\0');
	const DWORD copied = ::GetFullPathNameW(std::wstring(path).c_str(), required, absolute.data(), nullptr);
	if (copied == 0 || copied >= required) return std::wstring(path);
	absolute.resize(copied);
	return absolute;
}

[[nodiscard]] bool WideToUtf8Bounded(const wchar_t* value, std::size_t maximumBytes, std::string& converted) noexcept
{
	converted.clear();
	if (value == nullptr) return true;
	constexpr std::size_t kMaximumInputCharacters = 32767;
	const auto length = ::wcsnlen_s(value, kMaximumInputCharacters + 1);
	if (length > kMaximumInputCharacters) return false;
	if (length == 0) return true;
	if (length > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const auto required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
		static_cast<int>(length), nullptr, 0, nullptr, nullptr);
	if (required <= 0 || static_cast<std::size_t>(required) > maximumBytes) return false;
	try {
		converted.resize(static_cast<std::size_t>(required));
	}
	catch (const std::exception&) {
		return false;
	}
	return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
		static_cast<int>(length), converted.data(), required, nullptr, nullptr) == required;
}

[[nodiscard]] bool TryCanonicalEncodingId(ECodeType encoding, std::optional<std::string>& canonicalId)
{
	canonicalId.reset();
	switch (encoding) {
	case CODE_NONE:
		return true;
	case CODE_SJIS:
		canonicalId = "shift_jis";
		return true;
	case CODE_JIS:
		canonicalId = "iso-2022-jp";
		return true;
	case CODE_EUC:
		canonicalId = "euc-jp";
		return true;
	case CODE_UTF16LE:
		canonicalId = "utf-16le";
		return true;
	case CODE_UTF16BE:
		canonicalId = "utf-16be";
		return true;
	case CODE_UTF8:
		canonicalId = "utf-8";
		return true;
	case CODE_UTF7:
		canonicalId = "utf-7";
		return true;
	case CODE_CESU8:
		canonicalId = "cesu-8";
		return true;
	case CODE_LATIN1:
		canonicalId = "windows-1252";
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool TryWorkingCopyLineEnding(
	EEolType lineEnding, workbench::editor::EEditorWorkingCopyLineEnding& portable) noexcept
{
	using workbench::editor::EEditorWorkingCopyLineEnding;
	switch (lineEnding) {
	case EEolType::none:
		portable = EEditorWorkingCopyLineEnding::Preserve;
		return true;
	case EEolType::cr_and_lf:
		portable = EEditorWorkingCopyLineEnding::CrLf;
		return true;
	case EEolType::line_feed:
		portable = EEditorWorkingCopyLineEnding::Lf;
		return true;
	case EEolType::carriage_return:
		portable = EEditorWorkingCopyLineEnding::Cr;
		return true;
	default:
		return false;
	}
}

[[nodiscard]] std::optional<workbench::editor::EditorDocumentIdentity> FileIdentityFromLegacyPath(
	const wchar_t* value)
{
	if (value == nullptr) return std::nullopt;
	constexpr std::size_t kMaximumPathCharacters = 32767;
	const auto length = ::wcsnlen_s(value, kMaximumPathCharacters + 1);
	if (length == 0 || length > kMaximumPathCharacters) return std::nullopt;
	const auto absolute = MakeAbsolutePath(std::wstring_view(value, length));
	const auto uri = platform::uri::Uri::FromWindowsPath(absolute);
	if (!uri) return std::nullopt;
	return workbench::editor::EditorDocumentIdentity{ .resource = std::move(*uri.value) };
}

[[nodiscard]] RECT ToWinRect(const workbench::WorkbenchRect& source) noexcept
{
	return { source.left, source.top, source.right, source.bottom };
}

[[nodiscard]] bool ContainsPoint(const RECT& rect, POINT point) noexcept
{
	return rect.right > rect.left && rect.bottom > rect.top && ::PtInRect(&rect, point) != FALSE;
}

[[nodiscard]] int PixelsToDip(int pixels, unsigned int dpi) noexcept
{
	return ::MulDiv(pixels, 96, dpi == 0 ? 96 : static_cast<int>(dpi));
}

//! Repositions a child as part of a retained frame layout transaction.
//!
//! MoveWindow's repaint parameter is deceptively expensive here: it causes a
//! synchronous WM_PAINT for every sibling while the remaining siblings still
//! describe the previous geometry.  That exposes an erased/copy-bits frame in
//! the middle of a resize.  The parent commits all child pixels together with
//! one no-erase RedrawWindow after the complete layout has been projected.
[[nodiscard]] bool PositionChildForFrame(
	HWND window, int left, int top, int width, int height) noexcept
{
	if (window == nullptr || !::IsWindow(window)) return false;
	return ::SetWindowPos(window, nullptr, left, top,
		std::max(0, width), std::max(0, height),
		SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW) != FALSE;
}

//! The Git Output channel sink for this window. VS Code's Git extension
//! logs every command it runs into a "Git" log channel, so each native git
//! invocation below mirrors its header and stderr there. Logging is
//! best-effort: a null service or an exhausted operation-id sequence skips
//! the mirror and never changes the command's own result.
[[nodiscard]] workbench::scm::GitOutputSink MakeGitOutputSink(
	workbench::output::IOutputService* service)
{
	workbench::scm::GitOutputSink sink;
	sink.service = service;
	sink.owner = { .ownerId = "workbench.scm.git", .generation = 1 };
	sink.createOperationId = "sakura.scm.git.output/create";
	sink.nextAppendOperationId = []() -> std::optional<std::string> {
		// Process-local and thread-safe: these commands run on the UI thread,
		// but the output provider is shared with other producers.
		static std::atomic<std::uint64_t> sequence{ 0 };
		const auto next = ++sequence;
		if (next == (std::numeric_limits<std::uint64_t>::max)()) return std::nullopt;
		std::string operationId = "sakura.scm.git.output/";
		operationId += std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
		operationId += '/';
		operationId += std::to_string(static_cast<unsigned long long>(next));
		if (!workbench::output::IsValidOutputOperationId(operationId)) return std::nullopt;
		return operationId;
	};
	return sink;
}

} // namespace

struct CEditWnd::WorkbenchServiceProjectionGate final {
	std::mutex mutex;
	HWND window{};
	bool connected{ true };
	bool messageQueued{};
	//! A ChannelShown must survive coalescing with content-only notifications.
	bool outputRevealPending{};

	static void Notify(const std::shared_ptr<WorkbenchServiceProjectionGate>& gate,
		const bool outputChannelShown) noexcept
	{
		std::lock_guard lock(gate->mutex);
		if (!gate->connected || gate->window == nullptr) return;
		if (outputChannelShown) gate->outputRevealPending = true;
		if (gate->messageQueued) return;
		gate->messageQueued = true;
		if (::PostMessageW(gate->window, MYWM_WORKBENCH_SERVICE_PROJECTION_CHANGED, 0, 0)) return;

		// A failed post must not leave the gate permanently coalesced. A later
		// service change can retry after a transient queue/window failure.
		gate->messageQueued = false;
	}
};

struct CEditWnd::ThemeConfigurationGate final {
	std::mutex mutex;
	HWND window{};
	bool connected{ true };
	bool messageQueued{};

	static void Notify(const std::shared_ptr<ThemeConfigurationGate>& gate,
		const std::vector<config::ConfigurationChange>& changes) noexcept
	{
		const bool relevant = std::ranges::any_of(changes, [](const config::ConfigurationChange& change) {
			// `scm.countBadge` joins the two theme keys because it decides what the
			// Activity Bar paints, and VS Code applies it the moment it changes
			// rather than at the next Source Control publication.
			return change.key == "workbench.colorTheme" || change.key == "workbench.iconTheme"
				|| change.key == "workbench.activityBar.location"
				|| change.key.starts_with("editor.minimap.")
				|| change.key == "editor.guides.indentation"
				|| change.key == "scm.countBadge"
				|| change.key == "terminal.integrated.tabs.title"
				|| change.key == "terminal.integrated.tabs.description"
				|| change.key == "terminal.integrated.tabs.separator"
				|| change.key == "terminal.integrated.tabs.allowAgentCliTitle"
				|| change.key == "terminal.integrated.tabs.enabled"
				|| change.key == "terminal.integrated.tabs.hideCondition"
				|| change.key == "terminal.integrated.tabs.showActiveTerminal"
				|| change.key == "terminal.integrated.tabs.showActions"
				|| change.key == "terminal.integrated.tabs.location"
				|| change.key == "terminal.integrated.scrollback";
		});
		if (!relevant) return;
		std::lock_guard lock(gate->mutex);
		if (!gate->connected || gate->window == nullptr || gate->messageQueued) return;
		gate->messageQueued = true;
		if (::PostMessageW(gate->window, MYWM_WORKBENCH_THEME_CHANGED, 0, 0)) return;
		gate->messageQueued = false;
	}
};

struct CEditWnd::UpdateStateGate final {
	std::mutex mutex;
	HWND window{};
	bool connected{ true };
	bool messageQueued{};

	//! The state itself is deliberately not carried through the message. The UI
	//! thread re-reads `IUpdateService::State()`, so coalescing several worker
	//! transitions into one post can never show an intermediate state that the
	//! service has already left.
	static void Notify(const std::shared_ptr<UpdateStateGate>& gate) noexcept
	{
		std::lock_guard lock(gate->mutex);
		if (!gate->connected || gate->window == nullptr || gate->messageQueued) return;
		gate->messageQueued = true;
		if (::PostMessageW(gate->window, MYWM_WORKBENCH_UPDATE_STATE_CHANGED, 0, 0)) return;
		gate->messageQueued = false;
	}
};

static void ShowCodeBox( CEditDoc* pcEditDoc, const CEditView& cActiveView )
{
	// カーソル位置の文字列を取得
	const CLayout*	pcLayout;
	CLogicInt		nLineLen;
	const CEditView* pcView = &cActiveView;
	const CCaret* pcCaret = &pcView->GetCaret();
	const CLayoutMgr* pLayoutMgr = &pcEditDoc->m_cLayoutMgr;
	const wchar_t*	pLine = pLayoutMgr->GetLineStr( pcCaret->GetCaretLayoutPos().GetY2(), &nLineLen, &pcLayout );

	// -- -- -- -- キャレット位置の文字情報 -> szCaretChar -- -- -- -- //
	//
	if( pLine ){
		// 指定された桁に対応する行のデータ内の位置を調べる
		CLogicInt nIdx = pcView->LineColumnToIndex( pcLayout, pcCaret->GetCaretLayoutPos().GetX2() );
		if( nIdx < nLineLen ){
			if( nIdx < nLineLen - (pcLayout->GetLayoutEol().GetLen()?1:0) ){
				// 一時的に表示方法の設定を変更する
				CommonSetting_Statusbar sStatusbar;
				sStatusbar.m_bDispUniInSjis		= false;
				sStatusbar.m_bDispUniInJis		= false;
				sStatusbar.m_bDispUniInEuc		= false;
				sStatusbar.m_bDispUtf8Codepoint	= false;
				sStatusbar.m_bDispSPCodepoint	= false;

				WCHAR szMsg[128];
				WCHAR szCode[CODE_CODEMAX][32];
				wchar_t szChar[3];
				int nCharChars = CNativeW::GetSizeOfChar( pLine, nLineLen, nIdx );
				memcpy(szChar, &pLine[nIdx], nCharChars * sizeof(wchar_t));
				szChar[nCharChars] = L'\0';
				for( int i = 0; i < CODE_CODEMAX; i++ ){
					if( i == CODE_SJIS || i == CODE_JIS || i == CODE_EUC || i == CODE_LATIN1 || i == CODE_UNICODE || i == CODE_UTF8 || i == CODE_CESU8 ){
						//auto_sprintf( szCaretChar, L"%04x", );
						//任意の文字コードからUnicodeへ変換する		2008/6/9 Uchi
						CCodeBase* pCode = CCodeFactory::CreateCodeBase((ECodeType)i, false);
						EConvertResult ret = pCode->UnicodeToHex(&pLine[nIdx], nLineLen - nIdx, szCode[i], &sStatusbar);
						delete pCode;
						if (ret != RESULT_COMPLETE) {
							// うまくコードが取れなかった
							wcscpy(szCode[i], L"-");
						}
					}
				}
				// コードポイント部（サロゲートペアも）
				WCHAR szCodeCP[32];
				sStatusbar.m_bDispSPCodepoint = true;
				CCodeBase* pCode = CCodeFactory::CreateCodeBase(CODE_UNICODE, false);
				EConvertResult ret = pCode->UnicodeToHex(&pLine[nIdx], nLineLen - nIdx, szCodeCP, &sStatusbar);
				delete pCode;
				if (ret != RESULT_COMPLETE) {
					// うまくコードが取れなかった
					wcscpy(szCodeCP, L"-");
				}

				// メッセージボックス表示
				auto_sprintf(szMsg, LS(STR_ERR_DLGEDITWND13),
					szChar, szCodeCP, szCode[CODE_SJIS], szCode[CODE_JIS], szCode[CODE_EUC], szCode[CODE_LATIN1], szCode[CODE_UNICODE], szCode[CODE_UTF8], szCode[CODE_CESU8]);
				::MessageBox( cActiveView.GetHwnd(), szMsg, GSTR_APPNAME, MB_OK );
			}
		}
	}
}

/*!
 * 編集ウインドウのアドレスを取得します。
 */
CEditWnd* GetEditWndPtr() noexcept
{
	return CEditWnd::getInstance();
}

/*!
 * 編集ウインドウの参照を取得します。
 *
 * 編集ウインドウの生存期間ははエディタプロセスと同じなので、
 * ほとんどの場合、このグローバル関数を使ってアクセスできます。
 *
 * @throws CEditWndが生成されていない
 */
CEditWnd& GetEditWnd()
{
	auto pcEditWnd = GetEditWndPtr();
	if (!pcEditWnd) {
		throw std::domain_error("CEditWnd is not initialized");
	}
	return *pcEditWnd;
}

CViewFont* GetViewFont(bool isMiniMap)
{
	return GetEditWnd().GetViewFont(isMiniMap);
}

//	/* メッセージループ */
//	DWORD MessageLoop_Thread( DWORD pCEditWndObject );

LRESULT CALLBACK CEditWndProc(
	HWND	hwnd,	// handle of window
	UINT	uMsg,	// message identifier
	WPARAM	wParam,	// first message parameter
	LPARAM	lParam 	// second message parameter
)
{
	auto* pcWnd = reinterpret_cast<CEditWnd*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (uMsg == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		pcWnd = create == nullptr ? nullptr : static_cast<CEditWnd*>(create->lpCreateParams);
		if (pcWnd != nullptr) {
			::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pcWnd));
			pcWnd->AttachMainWindowEarly(hwnd);
		}
	}

	if (pcWnd == nullptr) {
		return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
	}

	const LRESULT result = pcWnd->IsDispatchReady()
		? pcWnd->DispatchEvent(hwnd, uMsg, wParam, lParam)
		: pcWnd->DispatchBootstrapEvent(hwnd, uMsg, wParam, lParam);
	if (uMsg == WM_NCDESTROY) {
		::SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
	}
	return result;
}

CEditWnd::CEditWnd()
	: m_customFrame(std::make_unique<CCustomFrameController>())
{
	const auto& cTypeConfig = GetEditDoc().m_cDocType.GetDocumentAttribute();
	auto& cLayoutMgr = GetEditDoc().m_cLayoutMgr;
	cLayoutMgr.SetLayoutInfo( true, false, cTypeConfig,
		cLayoutMgr.GetTabSpaceKetas(), cLayoutMgr.m_tsvInfo.m_nTsvMode,
		cLayoutMgr.GetMaxLineKetas(), CLayoutXInt(-1), &GetLogfont() );

	// [0] - [3] まで作成・初期化していたものを[0]だけ作る。ほかは分割されるまで何もしない
	m_pcEditViewArr[0] = std::make_unique<CEditView>();
	m_pcEditView = m_pcEditViewArr[0].get();
}

CEditWnd::CEditWnd(
	workbench::editor::CEditorServiceLegacyAdapter& editorServiceAdapter,
	workbench::editor::CEditDocLegacyEditorBackend& legacyEditorBackend,
	workbench::editor::EditorWorkingCopyCoordinator& workingCopyCoordinator,
	workbench::editor::persistence::EditorWorkingCopyLifecycleBridge& workingCopyLifecycleBridge,
	workbench::IWorkbenchRuntime& workbenchRuntime)
	: CEditWnd()
{
	m_editorServiceAdapter = &editorServiceAdapter;
	m_legacyEditorBackend = &legacyEditorBackend;
	m_workingCopyCoordinator = &workingCopyCoordinator;
	m_workingCopyLifecycleBridge = &workingCopyLifecycleBridge;
	m_workbenchRuntime = &workbenchRuntime;
}

void CEditWnd::AttachMainWindowEarly(HWND hWnd)
{
	m_hWnd = hWnd;
	if (!m_customFrame) {
		m_customFrame = std::make_unique<CCustomFrameController>();
	}
	const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark
		: theme::ThemeMode::Light;
	m_customFrame->Attach(hWnd, mode);
}

LRESULT CEditWnd::DispatchBootstrapEvent(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if (Msg == WM_WINDOWPOSCHANGING && IsStartupDrawSuppressed()) {
		// Some child/control initialization paths can request that the top-level
		// window be shown.  Keep the window genuinely hidden until the document,
		// final geometry, caption, and scroll ranges are ready to commit together.
		if (auto* position = reinterpret_cast<WINDOWPOS*>(lParam)) {
			position->flags &= ~SWP_SHOWWINDOW;
		}
	}
	LRESULT result = 0;
	const bool handled = m_customFrame
		&& m_customFrame->HandleWindowMessage(Msg, wParam, lParam, result);
	if (!handled) {
		result = ::DefWindowProc(hWnd, Msg, wParam, lParam);
	}
	if (Msg == WM_NCDESTROY) {
		m_dispatchReady = false;
		if (m_hWnd == hWnd) {
			m_hWnd = nullptr;
		}
	}
	return result;
}

CEditWnd::~CEditWnd()
{
	AbortStartupDrawTransaction();
	CloseWorkbench();
	CMacroFactory::resetInstance();
	CColorStrategyPool::resetInstance();
	CFigureManager::resetInstance();

	delete[] m_pszLastCaption;

	m_hWnd = nullptr;
}

std::string CEditWnd::NextEditorOperationId(std::string_view prefix)
{
	return std::string(prefix) + "." + std::to_string(::GetCurrentProcessId()) + "."
		+ std::to_string(++m_editorOperationSequence);
}

bool CEditWnd::CloseActiveEditorInput()
{
	if (m_editorServiceAdapter == nullptr || m_legacyEditorBackend == nullptr) return false;
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	if (!snapshot.group.activeInputId) {
		m_legacyEditorBackend->ClearInput();
		ApplyEditorCoreSnapshot(snapshot);
		return true;
	}
	const auto result = m_editorServiceAdapter->CloseInput({
		.operation = {
			.operationId = NextEditorOperationId("legacy.close"),
			.expectedModelRevision = snapshot.revision,
		},
		.inputId = *snapshot.group.activeInputId,
	});
	if (result.status != workbench::editor::EEditorOperationStatus::Succeeded) return false;
	m_legacyEditorBackend->ClearInput();
	ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
	return true;
}

bool CEditWnd::AdoptLoadedLegacyFile()
{
	if (m_editorServiceAdapter == nullptr || m_legacyEditorBackend == nullptr) return false;
	if (!m_legacyEditorBackend->PrepareFileInput()) {
		m_legacyEditorBackend->ClearInput();
		ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
		return false;
	}
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	workbench::editor::EditorOperationResult result;
	if (snapshot.group.activeInputId) {
		if (ActiveInputMatchesCurrentFile()) {
			// Reload keeps the canonical identity but publishes a new clean content version.
			return SynchronizeLegacyDocumentState(false, true);
		}
		result = m_editorServiceAdapter->ReplaceInputDocumentWithCurrent({
			.operationId = NextEditorOperationId("legacy.replace.file"),
			.expectedModelRevision = snapshot.revision,
		}, *snapshot.group.activeInputId);
	}
	else {
		result = m_editorServiceAdapter->AdoptCurrentDocument({
			.operationId = NextEditorOperationId("legacy.adopt.file"),
			.expectedModelRevision = snapshot.revision,
		}, std::string(kLegacyEditorInputId));
	}
	if (result.status != workbench::editor::EEditorOperationStatus::Succeeded) {
		m_legacyEditorBackend->ClearInput();
		ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
		return false;
	}
	ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
	return true;
}

bool CEditWnd::AdoptLegacyUntitledInput(std::string_view kind)
{
	if (m_editorServiceAdapter == nullptr || m_legacyEditorBackend == nullptr
		|| kind.empty() || kind.size() > 64) return false;
	if (m_editorServiceAdapter->Snapshot().group.activeInputId && !CloseActiveEditorInput()) return false;
	const std::string opaqueId = "sakura-legacy-" + std::string(kind) + ":"
		+ std::to_string(::GetCurrentProcessId()) + ":" + std::to_string(++m_editorOperationSequence);
	if (!m_legacyEditorBackend->PrepareUntitledInput(opaqueId)) {
		m_legacyEditorBackend->ClearInput();
		ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
		return false;
	}
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	const auto result = m_editorServiceAdapter->AdoptCurrentDocument({
		.operationId = NextEditorOperationId("legacy.adopt.untitled"),
		.expectedModelRevision = snapshot.revision,
	}, std::string(kLegacyEditorInputId));
	if (result.status != workbench::editor::EEditorOperationStatus::Succeeded) {
		m_legacyEditorBackend->ClearInput();
		ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
		return false;
	}
	ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
	return true;
}

ERecoveredEditorProjectionResult CEditWnd::ReconcileRecoveredEditorInput(
	std::string_view recoveredInputId, std::string_view effectiveActiveInputId)
{
	using namespace workbench::editor;
	if (m_editorServiceAdapter == nullptr || recoveredInputId.empty()
		|| effectiveActiveInputId.empty() || recoveredInputId != effectiveActiveInputId) {
		return ERecoveredEditorProjectionResult::InvalidRecovery;
	}

	try {
		const auto before = m_editorServiceAdapter->Snapshot();
		// The currently supported native bridge has exactly one recovered input.
		// Do not make a similarly named native CEditDoc authoritative by guessing
		// from it: prove the exact core input and its inactive starting state.
		if (before.group.inputs.size() != 1 || before.group.activeInputId) {
			return ERecoveredEditorProjectionResult::InvalidRecovery;
		}
		const auto recovered = std::ranges::find_if(before.group.inputs,
			[recoveredInputId](const auto& candidate) {
				return candidate.descriptor.inputId == recoveredInputId;
			});
		if (recovered == before.group.inputs.end()) {
			return ERecoveredEditorProjectionResult::InvalidRecovery;
		}

		const auto activated = m_editorServiceAdapter->ShowInput({
			.operation = {
				.operationId = NextEditorOperationId("recovery.activate"),
				.expectedModelRevision = before.revision,
			},
			.inputId = std::string(effectiveActiveInputId),
		});
		if (activated.status != EEditorOperationStatus::Succeeded) {
			return ERecoveredEditorProjectionResult::CoreActivationFailed;
		}

		const auto after = m_editorServiceAdapter->Snapshot();
		if (!after.group.activeInputId || *after.group.activeInputId != effectiveActiveInputId) {
			return ERecoveredEditorProjectionResult::CoreActivationFailed;
		}
		ApplyEditorCoreSnapshot(after, false);

		const HWND emptySurface = m_emptyEditorSurface ? m_emptyEditorSurface->GetHwnd() : nullptr;
		if (emptySurface == nullptr || !::IsWindow(emptySurface)) {
			return ERecoveredEditorProjectionResult::NativeProjectionFailed;
		}
		const bool emptySurfaceShown = ::IsWindow(emptySurface)
			&& (::GetWindowLongPtrW(emptySurface, GWL_STYLE) & WS_VISIBLE) != 0;
		if (!HasActiveEditorInput() || emptySurfaceShown) {
			return ERecoveredEditorProjectionResult::NativeProjectionFailed;
		}
		return ERecoveredEditorProjectionResult::Succeeded;
	}
	catch (...) {
		return ERecoveredEditorProjectionResult::NativeProjectionFailed;
	}
}

bool CEditWnd::CreateUntitledEditorInput()
{
	if (HasActiveEditorInput()) return false;
	GetDocument()->InitDoc();
	GetDocument()->InitAllView();
	GetDocument()->SetCurDirNotitle();
	CAppNodeManager::getInstance()->GetNoNameNumber(GetHwnd());
	return AdoptLegacyUntitledInput("untitled");
}

bool CEditWnd::ExecuteWorkbenchEditorCommand(std::string_view commandId)
{
	using namespace workbench::editor;
	if (commandId == command_ids::NewUntitledFile) {
		if (!HasActiveEditorInput()) return CreateUntitledEditorInput();
		// The established native command owns the multi-document/new-buffer path
		// once an editor is active.  Do not turn a recognized stable command into
		// a no-op simply because the empty-editor helper is not applicable.
		GetDocument()->HandleCommand(F_FILENEW);
		return true;
	}
	if (commandId == command_ids::OpenFile) {
		GetDocument()->HandleCommand(F_FILEOPEN);
		return true;
	}
	if (commandId == command_ids::OpenFolder) {
		return OpenWorkspaceFolder() == EOpenWorkspaceFolderResult::Succeeded;
	}
	if (commandId == command_ids::OpenRecent) {
		return ShowRecentlyOpenedWorkspaceMenu() == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::OpenWorkspace) {
		return OpenWorkspaceConfiguration() == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::AddRootFolder) {
		return AddFolderToWorkspace() == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::SaveWorkspaceAs) {
		return SaveWorkspaceAs() == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::DuplicateWorkspaceInNewWindow) {
		return DuplicateWorkspaceInNewWindow() == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::CloseFolder) {
		return CloseWorkspaceWindow() == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::NewWindow) {
		return LaunchWorkspaceTarget({}, false) == EWorkspaceWindowTransitionResult::Succeeded;
	}
	if (commandId == command_ids::Save || commandId == command_ids::SaveAs
		|| commandId == command_ids::Revert || commandId == command_ids::CloseActiveEditor) {
		return ExecuteActiveWorkingCopyCommand(commandId);
	}
	if (commandId == command_ids::SaveAll) {
		// The existing control-process fan-out remains the implementation, but it
		// is reachable only through this one stable registry executor.
		GetDocument()->HandleCommand(F_FILESAVEALL);
		return true;
	}
	if (commandId == command_ids::CloseWindow) {
		GetDocument()->HandleCommand(F_WINCLOSE);
		return true;
	}
	if (commandId == command_ids::Quit) {
		GetDocument()->HandleCommand(F_EXITALL);
		return true;
	}
	if (commandId == command_ids::ShowCommands) {
		return ShowCommandPalette();
	}
	if (commandId == command_ids::OpenSettings) {
		return CEditApp::getInstance()->OpenPropertySheet(-1);
	}
	return false;
}

void CEditWnd::ConfigureCustomFrameActions()
{
	if (!m_customFrame) return;
	m_customFrame->SetManageMenuActionCallback([this](CustomFrameManageAction action) {
		using namespace workbench::editor;
		std::string_view commandId;
		switch (action) {
		case CustomFrameManageAction::ShowCommandPalette:
			commandId = command_ids::ShowCommands;
			break;
		case CustomFrameManageAction::OpenSettings:
			commandId = command_ids::OpenSettings;
			break;
		case CustomFrameManageAction::OpenKeyboardShortcuts:
			commandId = command_ids::OpenGlobalKeybindings;
			break;
		case CustomFrameManageAction::SelectColorTheme:
			commandId = command_ids::SelectTheme;
			break;
		// Upstream's `7_update` group. Only the four actionable entries reach here;
		// the in-progress ones are contributed with `precondition: false` and the
		// menu never returns them.
		case CustomFrameManageAction::CheckForUpdates:
			commandId = "update.check";
			break;
		case CustomFrameManageAction::DownloadUpdate:
			commandId = "update.downloadNow";
			break;
		case CustomFrameManageAction::InstallUpdate:
			commandId = "update.install";
			break;
		case CustomFrameManageAction::RestartToUpdate:
			commandId = "update.restart";
			break;
		case CustomFrameManageAction::None:
			return;
		}
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand(commandId, handled);
	});
	// The title-bar button is one command whose meaning depends on the state it is
	// showing; the registry resolves which one from the same context snapshot the
	// button's own visibility clause was evaluated against, so the frame never
	// holds a second copy of the update state.
	m_customFrame->SetUpdateIndicatorCallback([this]() {
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand(workbench::commands::kUpdateIndicatorCommandId, handled);
	});
}

bool CEditWnd::ExecuteActiveWorkingCopyCommand(
	std::string_view commandId, bool suppressCloseConfirmation, bool disposeWindow)
{
	const auto result = ExecuteActiveWorkingCopyOperation(
		commandId, {}, std::nullopt, suppressCloseConfirmation, disposeWindow);
	if (result.status == workbench::editor::EEditorWorkingCopyOperationStatus::Succeeded) return true;
	return result.status == workbench::editor::EEditorWorkingCopyOperationStatus::NotApplicable
		&& commandId == workbench::editor::command_ids::Save
		&& result.workingCopy && result.workingCopy->identity.resource.has_value();
}

workbench::editor::EditorWorkingCopyOperationResult CEditWnd::ExecuteActiveWorkingCopyOperation(
	std::string_view commandId,
	const workbench::editor::EditorWorkingCopySaveOptions& saveOptions,
	std::optional<workbench::editor::EditorDocumentIdentity> targetIdentity,
	bool suppressCloseConfirmation,
	bool disposeWindow)
{
	using namespace workbench::editor;
	using namespace workbench::editor::persistence;
	if (m_workingCopyCoordinator == nullptr || m_editorServiceAdapter == nullptr) {
		return {
			.status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InvalidInput,
		};
	}
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	if (!snapshot.group.activeInputId) {
		return {
			.status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InputNotFound,
			.coreRevision = snapshot.revision,
		};
	}
	const auto completionToken = m_workingCopyLifecycleBridge
		? m_workingCopyLifecycleBridge->CaptureCurrentCompletionToken() : std::nullopt;

	const auto operation = EditorWorkingCopyOperationMetadata{
		.operationId = NextEditorOperationId("workbench.working-copy"),
		.expectedModelRevision = snapshot.revision,
	};
	const auto activeInput = std::ranges::find_if(snapshot.group.inputs, [&snapshot](const auto& candidate) {
		return candidate.descriptor.inputId == *snapshot.group.activeInputId;
	});
	if (activeInput == snapshot.group.inputs.end()) {
		return {
			.status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InputNotFound,
			.coreRevision = snapshot.revision,
		};
	}
	// An Untitled/opaque input has no native file target.  Stable Save therefore
	// takes the same target-acquisition route as Save As even when it is clean.
	const bool saveRequiresTarget = !activeInput->descriptor.documentIdentity.resource.has_value();
	const bool saveAsOperation = commandId == command_ids::SaveAs
		|| (commandId == command_ids::Save && saveRequiresTarget
			&& saveOptions.targetPolicy == EEditorWorkingCopySaveTargetPolicy::AcquireIfMissing);
	if (commandId == command_ids::CloseActiveEditor && m_workingCopyLifecycleBridge) {
		// Persist the latest accepted dirty generation before the native prompt can
		// synchronously save, discard, cancel, or destroy the backing document.
		(void)m_workingCopyLifecycleBridge->Flush(::GetTickCount64(), true);
	}
	EditorWorkingCopyOperationResult result;
	{
		// Save and close may synchronously notify the native listener.  The core is
		// committed only after that backend effect returns, so OnAfterSave must not
		// adopt/synchronize a speculative legacy identity in the interim.
		ScopedWorkingCopyBackendEffect backendEffect(m_workingCopyBackendEffectInProgress);
		if (commandId == command_ids::Save && !saveAsOperation) {
			result = m_workingCopyCoordinator->Save({
				.operation = operation,
				.inputId = *snapshot.group.activeInputId,
				.options = saveOptions,
			});
		}
		else if (saveAsOperation) {
			result = m_workingCopyCoordinator->SaveAs({
				.operation = operation,
				.inputId = *snapshot.group.activeInputId,
				.targetIdentity = std::move(targetIdentity),
				.options = saveOptions,
			});
		}
		else if (commandId == command_ids::Revert) {
			// The current backend returns Unsupported without touching the live document.
			result = m_workingCopyCoordinator->Revert({ .operation = operation, .inputId = *snapshot.group.activeInputId });
		}
		else if (commandId == command_ids::CloseActiveEditor) {
			result = m_workingCopyCoordinator->Close({
				.operation = operation,
				.inputId = *snapshot.group.activeInputId,
				.suppressConfirmation = suppressCloseConfirmation,
				.disposition = disposeWindow
					? EEditorWorkingCopyCloseDisposition::DisposeWindow
					: EEditorWorkingCopyCloseDisposition::InitializeEmptyDocument,
			});
		}
		else {
			return {
				.status = EEditorWorkingCopyOperationStatus::Unsupported,
				.reason = EEditorWorkingCopyOperationReason::BackendUnsupported,
				.coreRevision = snapshot.revision,
			};
		}
	}

	if (result.status == EEditorWorkingCopyOperationStatus::Succeeded) {
		if (commandId == command_ids::CloseActiveEditor && m_legacyEditorBackend != nullptr) {
			// CommitClose has reset the legacy document.  Its old read-only adoption
			// candidate must not survive the authoritative core input removal.
			m_legacyEditorBackend->ClearInput();
		}
		ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
		if (completionToken && m_workingCopyLifecycleBridge) {
			if (commandId == command_ids::CloseActiveEditor) {
				(void)m_workingCopyLifecycleBridge->CompletePreClose(*completionToken);
			}
			else if (commandId == command_ids::Save || commandId == command_ids::SaveAs) {
				(void)m_workingCopyLifecycleBridge->CompleteCurrentSave(*completionToken,
					saveAsOperation
						? EEditorWorkingCopySaveCompletionMode::AllowIdentityReplacement
						: EEditorWorkingCopySaveCompletionMode::PreserveIdentity);
			}
		}
		return result;
	}
	// A clean Save is a successfully handled no-op.  Cancellation, failure,
	// conflict, and the intentionally unsupported Revert remain observable to
	// the caller as false and do not trigger legacy fallback work.
	const bool cleanSave = result.status == EEditorWorkingCopyOperationStatus::NotApplicable
		&& commandId == command_ids::Save && !saveRequiresTarget;
	if (cleanSave && completionToken && m_workingCopyLifecycleBridge) {
		(void)m_workingCopyLifecycleBridge->CompleteCurrentSave(*completionToken);
	}
	return result;
}

SWorkingCopyFunctionDispatchResult CEditWnd::TryExecuteWorkingCopyFileCommand(
	const SLegacyEditorFunctionCommand& request)
{
	using namespace workbench::editor;
	SWorkingCopyFunctionDispatchResult dispatch;
	if (m_workingCopyCoordinator == nullptr || m_editorServiceAdapter == nullptr) return dispatch;

	const auto baseCode = static_cast<EFunctionCode>(static_cast<int>(request.FunctionCode()) & 0xffff);
	switch (baseCode) {
	case F_FILESAVE:
	case F_FILESAVEAS_DIALOG:
	case F_FILESAVEAS:
	case F_FILESAVE_QUIET:
	case F_FILESAVECLOSE:
	case F_FILECLOSE:
		break;
	default:
		return dispatch;
	}
	dispatch.handled = true;

	const auto invalidInput = [this]() {
		return EditorWorkingCopyOperationResult{
			.status = EEditorWorkingCopyOperationStatus::Failed,
			.reason = EEditorWorkingCopyOperationReason::InvalidInput,
			.coreRevision = m_editorServiceAdapter ? m_editorServiceAdapter->Snapshot().revision : 0,
		};
	};
	const auto isSuccessfulSave = [](const EditorWorkingCopyOperationResult& result) {
		if (result.status == EEditorWorkingCopyOperationStatus::Succeeded) return true;
		return result.status == EEditorWorkingCopyOperationStatus::NotApplicable
			&& result.workingCopy && result.workingCopy->identity.resource.has_value();
	};
	const auto postWindowClose = [this]() {
		const HWND window = GetHwnd();
		if (window == nullptr) return false;
		return ::PostMessage(window, MYWM_CLOSE, 0,
			reinterpret_cast<LPARAM>(CAppNodeManager::getInstance()->GetNextTab(window))) != FALSE;
	};

	EditorWorkingCopySaveOptions options;
	switch (baseCode) {
	case F_FILESAVE:
		dispatch.operation = ExecuteActiveWorkingCopyOperation(command_ids::Save, options);
		dispatch.legacyResult = isSuccessfulSave(*dispatch.operation) ? TRUE : FALSE;
		break;

	case F_FILESAVE_QUIET:
		options.targetPolicy = EEditorWorkingCopySaveTargetPolicy::ExistingOnly;
		options.suppressFeedback = true;
		dispatch.operation = ExecuteActiveWorkingCopyOperation(command_ids::Save, options);
		dispatch.legacyResult = isSuccessfulSave(*dispatch.operation) ? TRUE : FALSE;
		break;

	case F_FILESAVEAS_DIALOG:
		if (!WideToUtf8Bounded(reinterpret_cast<const wchar_t*>(request.Parameter1()), 4096, options.suggestedTarget)
			|| !TryCanonicalEncodingId(static_cast<ECodeType>(request.Parameter2()), options.encodingId)
			|| !TryWorkingCopyLineEnding(static_cast<EEolType>(request.Parameter3()), options.lineEnding)) {
			dispatch.operation = invalidInput();
			dispatch.legacyResult = FALSE;
			break;
		}
		dispatch.operation = ExecuteActiveWorkingCopyOperation(command_ids::SaveAs, options);
		dispatch.legacyResult = dispatch.operation->status == EEditorWorkingCopyOperationStatus::Succeeded
			? TRUE : FALSE;
		break;

	case F_FILESAVEAS:
		if (!TryWorkingCopyLineEnding(static_cast<EEolType>(request.Parameter3()), options.lineEnding)) {
			dispatch.operation = invalidInput();
			dispatch.legacyResult = FALSE;
			break;
		}
		if (auto target = FileIdentityFromLegacyPath(reinterpret_cast<const wchar_t*>(request.Parameter1()))) {
			dispatch.operation = ExecuteActiveWorkingCopyOperation(
				command_ids::SaveAs, options, std::move(target));
			dispatch.legacyResult = dispatch.operation->status == EEditorWorkingCopyOperationStatus::Succeeded
				? TRUE : FALSE;
		}
		else {
			dispatch.operation = invalidInput();
			dispatch.legacyResult = FALSE;
		}
		break;

	case F_FILESAVECLOSE:
		if (!GetDllShareData().m_Common.m_sFile.m_bEnableUnmodifiedOverwrite
			&& !GetDocument()->m_cDocEditor.IsModified()) {
			dispatch.legacyResult = postWindowClose() ? TRUE : FALSE;
			break;
		}
		options.suppressFeedback = true;
		options.forceWrite = GetDllShareData().m_Common.m_sFile.m_bEnableUnmodifiedOverwrite;
		dispatch.operation = ExecuteActiveWorkingCopyOperation(command_ids::Save, options);
		if (isSuccessfulSave(*dispatch.operation)) {
			dispatch.legacyResult = postWindowClose() ? TRUE : FALSE;
		}
		else {
			dispatch.legacyResult = FALSE;
		}
		break;

	case F_FILECLOSE:
		dispatch.operation = ExecuteActiveWorkingCopyOperation(command_ids::CloseActiveEditor);
		dispatch.legacyResult = dispatch.operation->status == EEditorWorkingCopyOperationStatus::Succeeded
			? TRUE : FALSE;
		break;

	default:
		// The first switch owns command recognition; this is an explicit terminal guard.
		dispatch.handled = false;
		break;
	}
	return dispatch;
}

void CEditWnd::DispatchEditorFunction(EFunctionCode functionCode)
{
	const auto baseCode = static_cast<EFunctionCode>(static_cast<int>(functionCode) & 0xffff);
	// Menu and key dispatch retain their source/high-bit flags up to this point.
	// Route only the base legacy alias through the stable workbench command; a
	// registered command's terminal failure must not fall through as success.
	if (m_workbenchRuntime != nullptr) {
		using namespace workbench::editor;
		std::string_view commandId;
		switch (baseCode) {
		case F_FILENEW: commandId = command_ids::NewUntitledFile; break;
		case F_FILENEW_NEWWINDOW: commandId = command_ids::NewWindow; break;
		case F_FILEOPEN: commandId = command_ids::OpenFile; break;
		case F_OPEN_WORKSPACE_FOLDER: commandId = command_ids::OpenFolder; break;
		case F_OPEN_WORKSPACE: commandId = command_ids::OpenWorkspace; break;
		case F_RECENT_WORKSPACE_LIST: commandId = command_ids::OpenRecent; break;
		case F_CLEAR_RECENT_WORKSPACES: commandId = command_ids::ClearRecentFiles; break;
		case F_ADD_FOLDER_TO_WORKSPACE: commandId = command_ids::AddRootFolder; break;
		case F_SAVE_WORKSPACE_AS: commandId = command_ids::SaveWorkspaceAs; break;
		case F_DUPLICATE_WORKSPACE: commandId = command_ids::DuplicateWorkspaceInNewWindow; break;
		case F_FILESAVE: commandId = command_ids::Save; break;
		case F_FILESAVEAS_DIALOG: commandId = command_ids::SaveAs; break;
		case F_FILESAVEALL: commandId = command_ids::SaveAll; break;
		case F_CLOSE_WORKSPACE: commandId = command_ids::CloseFolder; break;
		case F_CLOSE_ACTIVE_EDITOR: commandId = command_ids::CloseActiveEditor; break;
		case F_WINCLOSE: commandId = command_ids::CloseWindow; break;
		case F_EXITALL: commandId = command_ids::Quit; break;
		default: break;
		}
		if (!commandId.empty()) {
			bool handled = false;
			(void)TryExecuteWorkbenchStableCommand(commandId, handled);
			// Recognition is terminal. Disabled, cancelled, unsupported, and failed
			// stable commands must never select a different legacy behavior.
			if (!handled) return;
			return;
		}
	}
	if (baseCode == F_TOGGLE_LEFT_EXPLORER && m_workbenchRuntime != nullptr) {
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand("workbench.action.toggleSidebarVisibility", handled);
		if (handled) return;
	}
	if (baseCode == F_FILENEW && !HasActiveEditorInput()) {
		(void)CreateUntitledEditorInput();
		return;
	}
	if (!HasActiveEditorInput()) {
		const int code = static_cast<int>(baseCode);
		const bool isDocumentFileCommand = code >= static_cast<int>(F_FILESAVE)
			&& code <= static_cast<int>(F_PROPERTY_FILE)
			&& baseCode != F_FILENEW_NEWWINDOW
			&& baseCode != F_FILEOPEN_DROPDOWN;
		const bool isEditorCommand = code >= static_cast<int>(F_WCHAR)
			&& code <= static_cast<int>(F_FUNCLIST_PREV);
		const bool isDocumentModeCommand = code >= static_cast<int>(F_CHGMOD_INS)
			&& code <= static_cast<int>(F_CANCEL_MODE);
		if (isDocumentFileCommand || isEditorCommand || isDocumentModeCommand) return;
	}
	GetDocument()->HandleCommand(functionCode);
}

void CEditWnd::ExecuteWorkbenchFileFunction(EFunctionCode functionCode)
{
	DispatchEditorFunction(functionCode);
}

senp::ISenpRuntimeService* CEditWnd::GetSenpRuntime() const noexcept
{
	return m_workbenchRuntime == nullptr ? nullptr : m_workbenchRuntime->ExtensionRuntime();
}

senp::ISenpLanguageService* CEditWnd::GetSenpLanguageService() const noexcept
{
	return m_workbenchRuntime == nullptr ? nullptr : m_workbenchRuntime->ExtensionLanguages();
}

void CEditWnd::RefreshEditorCorePresentation()
{
	if (m_editorServiceAdapter == nullptr) return;
	ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot());
}

void CEditWnd::ApplyEditorCoreSnapshot(
	const workbench::editor::EditorCoreSnapshot& snapshot, bool restoreFocus)
{
	if (m_editorServiceAdapter == nullptr) return;

	const bool hasActiveInput = snapshot.group.activeInputId.has_value();
	// Editor tabs belong to this editor group.  Other native editor processes
	// may exist while this group is genuinely empty; their global node count
	// must not manufacture an Untitled tab above the empty-editor surface.
	const bool previousShowDocumentTabs =
		m_editorCorePresentationInitialized && m_hasActiveEditorInput;
	const bool showDocumentTabs = hasActiveInput;
	const bool documentTabVisibilityChanged = previousShowDocumentTabs != showDocumentTabs;
	const bool presentationChanged = !m_editorCorePresentationInitialized
		|| m_hasActiveEditorInput != hasActiveInput;
	m_hasActiveEditorInput = hasActiveInput;
	m_editorCorePresentationInitialized = true;
	UpdateWorkbenchWelcomeState();
	if (!presentationChanged) return;

	const HWND splitter = m_cSplitterWnd.GetHwnd();
	const HWND emptySurface = m_emptyEditorSurface ? m_emptyEditorSurface->GetHwnd() : nullptr;
	const HWND diffSurface = m_diffSurface ? m_diffSurface->GetHwnd() : nullptr;
	const HWND focused = ::GetFocus();
	const bool editorOwnedFocus = focused != nullptr
		&& ((splitter != nullptr && (focused == splitter || ::IsChild(splitter, focused)))
			|| (emptySurface != nullptr && (focused == emptySurface || ::IsChild(emptySurface, focused)))
			|| (diffSurface != nullptr && (focused == diffSurface || ::IsChild(diffSurface, focused))));

	if (hasActiveInput) {
		if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
		// A document input outranks every composition-layer projection, so the
		// comparison is retracted outright rather than merely hidden: it would
		// otherwise reappear the next time the group became empty, showing a diff
		// the user never asked for again.
		if (m_diffSurface) {
			m_diffSurface->ClearDiff();
			m_diffSurface->Hide();
		}
		if (splitter != nullptr && !m_pPrintPreview) ::ShowWindow(splitter, SW_SHOWNA);
		if (const HWND minimap = m_cMiniMapView.GetHwnd(); minimap != nullptr && !m_pPrintPreview) {
			::ShowWindow(minimap, SW_SHOWNA);
		}
	} else {
		m_markdownPreviewCommandState.Reset();
		m_markdownPreviewVisible = false;
		m_markdownPreviewDirty = false;
		m_markdownPreviewRevision = -1;
		m_markdownPreviewDivider = {};
		if (m_markdownPreview) m_markdownPreview->Show(false);
		if (splitter != nullptr) ::ShowWindow(splitter, SW_HIDE);
		if (const HWND minimap = m_cMiniMapView.GetHwnd(); minimap != nullptr) {
			::ShowWindow(minimap, SW_HIDE);
		}
		// Exactly one projection is visible: comparison or watermark.
		if (m_diffSurface && m_diffSurface->HasDiff() && !m_pPrintPreview) {
			m_diffSurface->Show();
		} else if (m_emptyEditorSurface && !m_pPrintPreview) {
			m_emptyEditorSurface->Show();
		}
		ClearDocumentStatus();
	}

	UpdateCaption();
	m_cTabWnd.RefreshDocumentActionState();
	if (documentTabVisibilityChanged) {
		if (const HWND window = GetHwnd(); ::IsWindow(window)) {
			RECT client{};
			::GetClientRect(window, &client);
			(void)OnSize2(m_nWinSizeType,
				MAKELONG(client.right - client.left, client.bottom - client.top), false);
		}
	}

	if (!restoreFocus || !editorOwnedFocus || m_pPrintPreview) return;
	if (hasActiveInput) {
		if (const HWND view = GetActiveView().GetHwnd(); ::IsWindowVisible(view)) ::SetFocus(view);
	} else if (m_diffSurface && m_diffSurface->HasDiff()) {
		m_diffSurface->Focus();
	} else if (m_emptyEditorSurface) {
		m_emptyEditorSurface->Focus();
	}
}

bool CEditWnd::SynchronizeLegacyDocumentState(bool dirty, bool contentChanged)
{
	if (m_editorServiceAdapter == nullptr) return false;
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	if (!snapshot.group.activeInputId) return false;

	const auto input = std::ranges::find_if(snapshot.group.inputs, [&snapshot](const auto& candidate) {
		return candidate.descriptor.inputId == *snapshot.group.activeInputId;
	});
	if (input == snapshot.group.inputs.end()) return false;
	const auto document = std::ranges::find_if(snapshot.documents, [&input](const auto& candidate) {
		return candidate.documentKey == input->documentKey;
	});
	if (document == snapshot.documents.end()) return false;

	std::uint64_t documentRevision = document->documentRevision;
	if (contentChanged) {
		if (documentRevision == (std::numeric_limits<std::uint64_t>::max)()) return false;
		++documentRevision;
	}
	const auto result = m_editorServiceAdapter->SetDocumentState({
		.operation = {
			.operationId = NextEditorOperationId("legacy.document.state"),
			.expectedModelRevision = snapshot.revision,
		},
		.inputId = *snapshot.group.activeInputId,
		.dirty = dirty,
		.documentRevision = documentRevision,
	});
	return result.status == workbench::editor::EEditorOperationStatus::Succeeded
		|| result.status == workbench::editor::EEditorOperationStatus::NotApplicable;
}

void CEditWnd::ClearDocumentStatus()
{
	if (m_cStatusBar.GetStatusHwnd() == nullptr) return;
	for (int part = 1; part < 8; ++part) {
		m_cStatusBar.SetStatusText(part, 0, L"");
	}
	ClearViewCaretPosInfo();
	LayoutStatusBarParts();
}

bool CEditWnd::ActiveInputMatchesCurrentFile() const
{
	if (m_editorServiceAdapter == nullptr) return true;
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	if (!snapshot.group.activeInputId) return false;
	const auto input = std::ranges::find_if(snapshot.group.inputs, [&snapshot](const auto& candidate) {
		return candidate.descriptor.inputId == *snapshot.group.activeInputId;
	});
	if (input == snapshot.group.inputs.end() || !input->descriptor.documentIdentity.resource) return false;
	const auto& file = GetDocument()->m_cDocFile;
	if (!file.GetFilePathClass().IsValidPath()) return false;
	const auto current = platform::uri::Uri::FromWindowsPath(file.GetFilePath());
	return current && platform::uri::UriIdentityService::IsEqual(
		*input->descriptor.documentIdentity.resource, *current.value);
}

void CEditWnd::BeginStartupDrawTransaction() noexcept
{
	if (m_startupDrawState != StartupDrawState::Inactive) {
		assert_warning(false);
		return;
	}
	m_startupSavedDrawSwitch = SetDrawSwitchOfAllViews(false);
	m_startupShowCommand = SW_SHOW;
	m_startupPreviousTabWindow = nullptr;
	m_startupFirstContentPainted = false;
	m_startupCommitLayoutAllowed = false;
	m_startupMiniMapSummaryEmitted = false;
	m_startupMiniMapPaintQpcTicks = 0;
	m_startupMiniMapPaintCount = 0;
	m_startupMiniMapImmediateUpdateCount = 0;
	m_startupDrawState = StartupDrawState::Suppressing;
	CStartupTrace::ArmStartupDocument();
}

bool CEditWnd::IsStartupDrawSuppressed() const noexcept
{
	return m_startupDrawState == StartupDrawState::Suppressing;
}

bool CEditWnd::IsStartupDrawCommitting() const noexcept
{
	return m_startupDrawState == StartupDrawState::Committing;
}

bool CEditWnd::ShouldDeferStartupLayout() const noexcept
{
	return !m_startupCommitLayoutAllowed
		&& (m_startupDrawState == StartupDrawState::Suppressing
			|| m_startupDrawState == StartupDrawState::Committing);
}

void CEditWnd::AbortStartupDrawTransaction() noexcept
{
	const bool wasCommitting = m_startupDrawState == StartupDrawState::Committing;
	if (m_startupDrawState == StartupDrawState::Suppressing
		|| m_startupDrawState == StartupDrawState::Committing) {
		SetDrawSwitchOfAllViews(m_startupSavedDrawSwitch);
		m_startupCommitLayoutAllowed = false;
		m_startupPreviousTabWindow = nullptr;
		m_startupDrawState = StartupDrawState::Aborted;
		CStartupTrace::AbortStartupDocument();
		if (wasCommitting) {
			EmitStartupMiniMapSummary();
		}
	}
}

void CEditWnd::EmitStartupMiniMapSummary() noexcept
{
	if (m_startupMiniMapSummaryEmitted) {
		return;
	}
	m_startupMiniMapSummaryEmitted = true;
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawMiniMapPaintSummary,
		m_startupMiniMapPaintQpcTicks, m_startupMiniMapPaintCount);
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawMiniMapUpdateSummary,
		m_startupMiniMapImmediateUpdateCount);
	CStartupTrace::FlushStartupDocumentMetrics();
}

void CEditWnd::RecordStartupMiniMapImmediateUpdate() noexcept
{
	if (m_startupDrawState == StartupDrawState::Committing) {
		++m_startupMiniMapImmediateUpdateCount;
	}
}

void CEditWnd::RecordStartupMiniMapPaint(std::int64_t qpcTicks) noexcept
{
	if (m_startupDrawState != StartupDrawState::Committing || qpcTicks < 0) {
		return;
	}
	m_startupMiniMapPaintQpcTicks += qpcTicks;
	++m_startupMiniMapPaintCount;
}

void CEditWnd::FinishStartupTabSwap() noexcept
{
	if (!m_startupFirstContentPainted) {
		return;
	}

	const HWND previousTab = m_startupPreviousTabWindow;
	if (::IsWindow(previousTab)) {
		if (const HWND hwnd = GetHwnd(); ::IsWindow(hwnd)) {
			::BringWindowToTop(hwnd);
		}
		::ShowWindowAsync(previousTab, SW_HIDE);
	}
	m_startupPreviousTabWindow = nullptr;
}

void CEditWnd::CommitStartupDrawTransaction()
{
	if (m_startupDrawState == StartupDrawState::Committed
		|| m_startupDrawState == StartupDrawState::Aborted) {
		return;
	}
	if (m_startupDrawState != StartupDrawState::Suppressing) {
		assert_warning(false);
		return;
	}

	CStartupDocumentSubphaseTimer drawCommitTimer{
		CStartupTrace::StartupDocumentSubphase::DrawCommit };
	m_startupDrawState = StartupDrawState::Committing;
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawCommitBegin);
	const HWND hwnd = GetHwnd();
	if (!::IsWindow(hwnd)) {
		drawCommitTimer.Finish();
		AbortStartupDrawTransaction();
		CStartupTrace::Mark(CStartupTrace::Event::StartupDrawCommitEnd, 0, ERROR_INVALID_WINDOW_HANDLE);
		return;
	}

	// Finalize child geometry while the top-level window is still hidden. Drawing
	// remains disabled here, so controls cannot expose intermediate bootstrap state.
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawLayoutBegin);
	RECT client{};
	::GetClientRect(hwnd, &client);
	const WPARAM sizeType = ::IsIconic(hwnd)
		? SIZE_MINIMIZED
		: (::IsZoomed(hwnd) ? SIZE_MAXIMIZED : SIZE_RESTORED);
	m_startupCommitLayoutAllowed = true;
	(void)OnSize2(sizeType,
		MAKELONG(client.right - client.left, client.bottom - client.top), true);
	m_startupCommitLayoutAllowed = false;
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawLayoutEnd,
		m_startupDrawState == StartupDrawState::Committing && ::IsWindow(hwnd) ? 1 : 0);
	if (m_startupDrawState != StartupDrawState::Committing || !::IsWindow(hwnd)) {
		drawCommitTimer.Finish();
		AbortStartupDrawTransaction();
		CStartupTrace::Mark(CStartupTrace::Event::StartupDrawCommitEnd, 0, ERROR_OPERATION_ABORTED);
		return;
	}

	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawScrollBegin);
	SetDrawSwitchOfAllViews(m_startupSavedDrawSwitch);
	// The load-completion path attempts to update the scroll bars while startup
	// drawing is suppressed.  CEditView::AdjustScrollBars deliberately ignores
	// that request, so publish the final layout range after restoring drawing and
	// before the first visible paint.  Otherwise a later incidental message is
	// required to replace the one-line bootstrap range.
	for (int i = 0; i < GetAllViewCount(); ++i) {
		GetView(i).AdjustScrollBars(FALSE);
	}
	m_cMiniMapView.AdjustScrollBars(FALSE);
	CStartupTrace::CompleteStartupDocument();
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawScrollEnd);

	// Allow a maximizing/minimizing ShowWindow to deliver its final WM_SIZE with
	// drawing enabled. The first composited frame can therefore contain the real
	// document instead of a visible empty shell.
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawShowBegin);
	m_startupCommitLayoutAllowed = true;
	::ShowWindow(hwnd, m_startupShowCommand);
	m_startupCommitLayoutAllowed = false;
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawShowEnd,
		m_startupDrawState == StartupDrawState::Committing && ::IsWindow(hwnd) ? 1 : 0);
	if (m_startupDrawState != StartupDrawState::Committing || !::IsWindow(hwnd)) {
		drawCommitTimer.Finish();
		AbortStartupDrawTransaction();
		CStartupTrace::Mark(CStartupTrace::Event::StartupDrawCommitEnd, 0, ERROR_OPERATION_ABORTED);
		return;
	}

	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawRedrawBegin);
	const HWND previousTab = m_startupPreviousTabWindow;
	if (::IsWindow(previousTab)) {
		// Keep the fully painted previous tab in front until the new view is ready.
		::SetWindowPos(hwnd, previousTab, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	const BOOL redrawResult = ::RedrawWindow(hwnd, nullptr, nullptr,
		RDW_FRAME | RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	if (redrawResult) {
		(void)::GdiFlush();
		PublishCommittedGdiFrame();
	}
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawRedrawEnd, redrawResult ? 1 : 0);
	if (redrawResult && !HasActiveEditorInput()) {
		RecordFirstStartupContentPaint();
	}

	drawCommitTimer.Finish();
	EmitStartupMiniMapSummary();
	m_startupDrawState = StartupDrawState::Committed;
	FinishStartupTabSwap();
	CStartupTrace::Mark(CStartupTrace::Event::StartupDrawCommitEnd, redrawResult ? 1 : 0);
	PostDeferredStartupWorkbenchIfReady();
}

void CEditWnd::RecordFirstStartupContentPaint() noexcept
{
	if (m_startupFirstContentPainted
		|| (m_startupDrawState != StartupDrawState::Committing
			&& m_startupDrawState != StartupDrawState::Committed)) {
		return;
	}
	m_startupFirstContentPainted = true;
	CStartupTrace::MarkFirstContentPainted();
	if (m_startupDrawState == StartupDrawState::Committed) {
		FinishStartupTabSwap();
	}
	PostDeferredStartupWorkbenchIfReady();
}

std::wstring CEditWnd::BuildExplorerLaunchOptions(bool preview) const
{
	std::wstring options;
	const auto append = [&options](std::wstring option) {
		if (option.empty()) return;
		if (!options.empty()) options.push_back(L' ');
		options += std::move(option);
	};
	if (preview) append(L"-EXPLORERPREVIEW");

	if (m_workbenchRuntime != nullptr) {
		const auto workspace = m_workbenchRuntime->WorkspaceContext().Snapshot();
		if (workspace.kind == config::EWorkspaceKind::Workspace && workspace.workspaceConfigUri) {
			if (const auto path = workspace.workspaceConfigUri->ToWindowsPath(); path.value) {
				append(L"-WORKSPACE=\"" + *path.value + L"\"");
				return options;
			}
		}
		if (workspace.kind == config::EWorkspaceKind::Folder && workspace.folders.size() == 1) {
			if (const auto path = workspace.folders.front().uri.ToWindowsPath(); path.value) {
				append(L"-FOLDER=\"" + *path.value + L"\"");
				return options;
			}
		}
	}

	const auto* commandLine = CCommandLine::getInstance();
	if (commandLine->IsSetWorkspaceConfig()) {
		append(L"-WORKSPACE=\"" + std::wstring(commandLine->GetWorkspaceConfig()) + L"\"");
	} else if (commandLine->IsSetWorkspaceFolder()) {
		append(L"-FOLDER=\"" + std::wstring(commandLine->GetWorkspaceFolder()) + L"\"");
	}
	return options;
}

//! Opens one file at one UTF-16 marker position.  The open places the file in
//! exactly one frame, which is this one only when the file was not already open
//! elsewhere, so the owner is asked the way the tag jump does rather than
//! assumed.  Where this frame owns the document the position is converted
//! against the real line contents -- that is what corrects a column landing
//! inside a surrogate pair and what clamps a position computed against an older
//! revision.  Another frame's line contents are not readable from here, so the
//! position crosses unconverted, exactly as the tag jump sends it; the receiving
//! frame's MYWM_SETCARETPOS handler still keeps the caret off an EOL.
//!
//! Both the Problems panel and the Search view activate a result through this
//! one path, so a marker and a search match can never land differently.
void CEditWnd::OpenDocumentAtMarkerPosition(std::wstring_view path,
	std::uint32_t zeroBasedLine, std::uint32_t zeroBasedColumn)
{
	if (path.empty()) return;
	const std::wstring ownedPath(path);
	GetActiveView().GetCommander().Command_FILEOPEN(ownedPath.c_str());
	HWND owner = nullptr;
	if (!CShareData::getInstance()->IsPathOpened(ownedPath.c_str(), &owner) || owner == nullptr) {
		return;
	}
	CLogicPoint caret;
	if (owner == GetHwnd()) {
		const CDocLineMgr& lines = GetDocument()->m_cDocLineMgr;
		const auto lineCount = static_cast<std::uint32_t>(
			(std::max)(CLogicInt(0), lines.GetLineCount()));
		const auto converted = workbench::problems::ConvertMarkerPositionToLogicPoint(
			zeroBasedLine, zeroBasedColumn, lineCount,
			[&lines](std::uint32_t index) -> std::wstring_view {
				const CDocLine* docLine = lines.GetLine(CLogicInt(static_cast<int>(index)));
				if (docLine == nullptr) return {};
				const auto length = (std::max)(CLogicInt(0), docLine->GetLengthWithoutEOL());
				return { docLine->GetPtr(), static_cast<std::size_t>(length) };
			});
		caret = converted.position;
	} else {
		caret.Set(CLogicInt(static_cast<int>(zeroBasedColumn)),
			CLogicInt(static_cast<int>(zeroBasedLine)));
	}
	GetDllShareData().m_sWorkBuffer.m_LogicPoint = caret;
	::SendMessageAny(owner, MYWM_SETCARETPOS, 0, 0);
	ActivateFrameWindow(owner);
}

//! Opens one Search result.  `line` and `column` are 1-based UTF-16 positions,
//! which is how `SearchMatch` records them.
void CEditWnd::OpenSearchMatch(std::wstring_view path, std::int64_t line, int column)
{
	if (line <= 0) return;
	OpenDocumentAtMarkerPosition(path, static_cast<std::uint32_t>(line - 1),
		static_cast<std::uint32_t>((std::max)(0, column - 1)));
}

//! A replace pass rewrites files on disk. This frame reloads its own document
//! when the pass changed it and nothing local would be lost; a modified
//! document and every other frame keep the existing external-change detection,
//! which is the same prompt any outside editor would trigger.
void CEditWnd::ReloadReplacedFiles(const std::vector<std::wstring>& paths)
{
	if (paths.empty()) return;
	CEditDoc* document = GetDocument();
	if (document == nullptr || document->m_cDocEditor.IsModified()) return;
	const std::wstring current = document->m_cDocFile.GetFilePath();
	if (current.empty()) return;
	const bool touched = std::ranges::any_of(paths, [&current](const std::wstring& path) {
		return ::_wcsicmp(path.c_str(), current.c_str()) == 0;
	});
	if (!touched) return;
	document->m_cDocFileOperation.ReloadCurrentFile(document->m_cDocFile.GetCodeSet());
}

void CEditWnd::OpenExplorerFile(std::wstring_view path,
	workbench::explorer::ExplorerFileActivationKind kind)
{
	if (path.empty()) return;
	const std::wstring ownedPath(path);
	SLoadInfo loadInfo(ownedPath.c_str(), CODE_AUTODETECT, false);
	const auto currentPath = GetDocument()->m_cDocFile.GetFilePath();
	const auto plan = workbench::explorer::PlanExplorerEditorActivation(
		m_explorerPreviewEditor, loadInfo.IsSamePath(currentPath), kind);
	if (plan.action == workbench::explorer::ExplorerEditorActivationAction::ActivateCurrent) {
		m_explorerPreviewEditor = plan.nextEditorIsPreview;
		return;
	}

	if (plan.action == workbench::explorer::ExplorerEditorActivationAction::ReplaceCurrentPreview) {
		loadInfo.eWindowDisposition = ELoadWindowDisposition::ReplaceCurrentEditor;
		if (GetDocument()->m_cDocFileOperation.FileCloseOpen(loadInfo)) {
			m_explorerPreviewEditor = plan.nextEditorIsPreview;
		}
		return;
	}

	if (GetDocument()->IsAcceptLoad()) {
		if (GetDocument()->m_cDocFileOperation.FileLoad(&loadInfo)) {
			m_explorerPreviewEditor = plan.nextEditorIsPreview;
		}
		return;
	}

	const auto options = BuildExplorerLaunchOptions(plan.nextEditorIsPreview);
	(void)CControlTray::OpenNewEditor(
		G_AppInstance(), GetHwnd(), loadInfo,
		options.empty() ? nullptr : options.c_str(), false, nullptr, false);
}

bool CEditWnd::InitializeWorkbench()
{
	if (m_workspaceContext != nullptr) return true;
	m_explorerPreviewEditor = CCommandLine::getInstance()->IsExplorerPreview();

	std::wstring terminalLaunchDirectory = GetProcessCurrentDirectory();
	if (m_workbenchRuntime != nullptr) {
		if (const auto& terminalUri = m_workbenchRuntime->Bootstrap().TerminalLaunchDirectoryUri()) {
			if (auto path = terminalUri->ToWindowsPath(); path) terminalLaunchDirectory = std::move(*path.value);
		}
	}
	m_workspaceContext = std::make_unique<workbench::CWorkspaceContext>(std::move(terminalLaunchDirectory));
	if (GetDocument()->m_cDocFile.GetFilePathClass().IsValidPath()) {
		m_workspaceContext->SetSelectedFile(GetDocument()->m_cDocFile.GetFilePath());
	}
	if (m_workbenchRuntime == nullptr) {
		const auto* commandLine = CCommandLine::getInstance();
		if (commandLine->IsSetWorkspaceFolder()) {
			m_workspaceContext->SetExplicitRoot(MakeAbsolutePath(commandLine->GetWorkspaceFolder()));
		}
	} else {
		ApplySemanticWorkspaceContext();
	}
	m_accountDiscoveryService = std::make_unique<workbench::account::AccountDiscoveryService>();
	(void)m_accountDiscoveryService->RequestRefresh(
		m_workspaceContext->GetNewTerminalWorkingDirectory());
	if (m_customFrame) {
		m_customFrame->SetAccountMenuModelCallback([this] {
			if (!m_accountDiscoveryService) return CustomFrameAccountMenuModel{};
			const auto snapshot = m_accountDiscoveryService->Snapshot();
			auto model = ProjectAccountMenuModel(snapshot);
			// Return the stable snapshot captured above, then refresh in the
			// background for the next open. The popup never waits for git or gh.
			if (workbench::account::IsTerminalAccountDiscoveryState(snapshot.state)
				&& m_workspaceContext) {
				try {
					(void)m_accountDiscoveryService->RequestRefresh(
						m_workspaceContext->GetNewTerminalWorkingDirectory());
				}
				catch (...) {
					// The already-built snapshot remains a complete terminal view.
				}
			}
			return model;
		});
	}

	const auto commitExtent = [this](workbench::WorkbenchEdge edge, int extentDip) {
		if (m_workbenchRuntime == nullptr) {
			PersistWorkbenchExtent(edge, extentDip);
			return true;
		}
		switch (edge) {
		case workbench::WorkbenchEdge::Left:
			return SetBuiltinPartExtent(workbench::layout::ids::part::Sidebar, extentDip);
		case workbench::WorkbenchEdge::Bottom:
			return SetBuiltinPartExtent(workbench::layout::ids::part::Panel, extentDip);
		case workbench::WorkbenchEdge::Right:
			return SetBuiltinPartExtent(workbench::layout::ids::part::Auxiliarybar, extentDip);
		}
		return false;
	};
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	m_viewContainerPages = std::make_shared<workbench::viewcontainer::CViewContainerPages>(m_cDlgFuncList);
	const auto contributionSnapshot = m_workbenchRuntime->Contributions().Snapshot();
	const std::array hostViewProviders{
		workbench::viewcontainer::HostViewProviderDescriptor{
			.id = "sakura.projects",
			.factory = [this]() {
				workbench::projects::ProjectsPageOptions options;
				options.projects = [this]()
					-> std::optional<std::vector<workbench::projects::ProjectEntry>> {
					if (m_workbenchRuntime == nullptr) {
						return std::nullopt;
					}
					const auto projects = m_workbenchRuntime->Projects();
					return projects == nullptr
						|| projects->State() != workbench::projects::EProjectCatalogState::Ready
						? std::nullopt
						: std::optional(projects->Snapshot());
				};
				options.workspace = [this]() {
					return m_workbenchRuntime == nullptr
						? config::WorkspaceContextSnapshot{}
						: m_workbenchRuntime->WorkspaceContext().Snapshot();
				};
				options.workspaceRoot = [this]() { return GetSemanticWorkspaceRoot(); };
				options.repositoryRoots = [this](const workbench::projects::ProjectEntry& project)
					-> std::optional<std::vector<std::wstring>> {
					if (project.kind == workbench::projects::EProjectKind::Folder) {
						const auto path = project.uri.ToWindowsPath();
						if (!path.value || path.value->empty()) return std::nullopt;
						return std::vector<std::wstring>{ *path.value };
					}
					if (m_workbenchRuntime == nullptr) return std::nullopt;
					const auto inspected = m_workbenchRuntime->InspectWorkspaceConfiguration(project.uri);
					if (!inspected.Succeeded()) return std::nullopt;
					std::vector<std::wstring> roots;
					roots.reserve(inspected.document->folders.size());
					for (const auto& folder : inspected.document->folders) {
						const auto path = folder.uri.ToWindowsPath();
						if (path.value && !path.value->empty()) roots.push_back(*path.value);
					}
					return roots;
				};
				options.activateProject = [this](
					const workbench::projects::ProjectEntry& project, const bool thisWindow) {
					if (GetHwnd() == nullptr) return workbench::projects::EProjectsActivationStatus::Failed;
					if (thisWindow) {
						if (::IsIconic(GetHwnd())) ::ShowWindow(GetHwnd(), SW_RESTORE);
						::SetForegroundWindow(GetHwnd());
						return workbench::projects::EProjectsActivationStatus::FocusedCurrentWindow;
					}
					// A Project switch must never detach Terminal state or commit a new
					// workspace while the only native Editor document has unsaved data.
					// The explicit new-window command is a separate callback and remains
					// available because it does not replace this window's document.
					if (HasActiveEditorInput() && GetDocument()->m_cDocEditor.IsModified()) {
						return workbench::projects::EProjectsActivationStatus::Failed;
					}
					const auto path = project.uri.ToWindowsPath();
					if (!path.value || path.value->empty()) {
						return workbench::projects::EProjectsActivationStatus::Failed;
					}
					// Folder Projects are switched in the existing window.  This keeps
					// the process-owned workbench (layout, panel state, and extensions)
					// alive; only the workspace-dependent projections are refreshed.
					if (project.kind == workbench::projects::EProjectKind::Folder) {
						return ApplyFolderWorkspace(*path.value, true) == EOpenWorkspaceFolderResult::Succeeded
							? workbench::projects::EProjectsActivationStatus::FocusedCurrentWindow
							: workbench::projects::EProjectsActivationStatus::Failed;
					}
					if (project.kind == workbench::projects::EProjectKind::Workspace
						&& m_workbenchRuntime != nullptr) {
						if (m_projectWorkspaceTransitionInProgress) {
							return workbench::projects::EProjectsActivationStatus::Failed;
						}
						ScopedBooleanState transition(m_projectWorkspaceTransitionInProgress);
						const auto before = m_workbenchRuntime->WorkspaceContext().Snapshot();
						const auto targetIdentity = PreviewProjectWorkspaceIdentity(
							config::EWorkspaceKind::Workspace, project.uri);
						if (!targetIdentity) {
							return workbench::projects::EProjectsActivationStatus::Failed;
						}
						const bool terminalWasVisible = IsWorkbenchPanelVisible(workbench::WorkbenchEdge::Bottom)
							&& IsBuiltinWorkbenchViewActive(workbench::layout::ids::view::Terminal);
						const auto oldTerminalDirectory = m_workspaceContext != nullptr
							? m_workspaceContext->GetNewTerminalWorkingDirectory() : std::wstring{};
						const auto targetTerminalDirectory = std::filesystem::path(*path.value).parent_path().wstring();
						const bool targetProjectionExisted = m_terminalTool != nullptr
							&& m_terminalTool->HasWorkspaceProjection(*targetIdentity);
						bool terminalPrepared = false;
						terminal::TerminalWorkspaceSwitchOutcome terminalOutcome =
							terminal::TerminalWorkspaceSwitchOutcome::Unchanged;
						if (m_terminalTool != nullptr && before.workspaceIdentityKey != *targetIdentity) {
							const auto prepared = m_terminalTool->SwitchWorkspace({
								before.workspaceIdentityKey, *targetIdentity, targetTerminalDirectory, false });
							if (!prepared.Succeeded()) {
								if (!targetProjectionExisted) m_terminalTool->DiscardWorkspaceProjection(*targetIdentity);
								return workbench::projects::EProjectsActivationStatus::Failed;
							}
							terminalPrepared = true;
							terminalOutcome = prepared.outcome;
						}
						const auto rollbackTerminal = [&] {
							if (!terminalPrepared || m_terminalTool == nullptr) return;
							const auto restored = m_terminalTool->SwitchWorkspace({
								*targetIdentity, before.workspaceIdentityKey, oldTerminalDirectory, false });
							if (!restored.Succeeded()) {
								::OutputDebugStringW(L"Sakura Editor NEXT: Project terminal rollback failed.\n");
							}
							if (!targetProjectionExisted) m_terminalTool->DiscardWorkspaceProjection(*targetIdentity);
						};
						const auto accepted = m_workbenchRuntime->SwitchToWorkspaceConfiguration(project.uri);
						if (accepted.outcome != config::EWorkspaceContextOutcome::Succeeded) {
							rollbackTerminal();
							return workbench::projects::EProjectsActivationStatus::Failed;
						}
						ApplySemanticWorkspaceContext();
						RevealExplorerAfterWorkspaceCommit();
						if (m_terminalTool != nullptr && terminalPrepared) {
							if (terminalOutcome == terminal::TerminalWorkspaceSwitchOutcome::Detached) {
								m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
							}
							if (terminalWasVisible && !m_terminalTool->EnsureSessionStarted()) {
								::OutputDebugStringW(L"Sakura Editor NEXT: Project terminal activation failed after workspace commit.\n");
							}
						}
						if (!RefreshWorkbenchCommandContext()) {
							// The workspace CAS is already committed. Context-key refresh is an
							// advisory projection and must not misreport a successful switch as a
							// failed transition whose old workspace is supposedly still active.
							::OutputDebugStringW(L"Sakura Editor NEXT: Project workspace context refresh failed.\n");
						}
						return workbench::projects::EProjectsActivationStatus::FocusedCurrentWindow;
					}
					const std::wstring option = project.kind == workbench::projects::EProjectKind::Folder
						? L"-FOLDER=\"" + *path.value + L"\""
						: L"-WORKSPACE=\"" + *path.value + L"\"";
					return LaunchWorkspaceTarget(option, false) == EWorkspaceWindowTransitionResult::Succeeded
						? workbench::projects::EProjectsActivationStatus::OpenedNewWindow
						: workbench::projects::EProjectsActivationStatus::Failed;
				};
				options.activateProjectInNewWindow = [this](
					const workbench::projects::ProjectEntry& project) {
					if (GetHwnd() == nullptr) return workbench::projects::EProjectsActivationStatus::Failed;
					const auto path = project.uri.ToWindowsPath();
					if (!path.value || path.value->empty()) {
						return workbench::projects::EProjectsActivationStatus::Failed;
					}
					const std::wstring option = project.kind == workbench::projects::EProjectKind::Folder
						? L"-FOLDER=\"" + *path.value + L"\""
						: L"-WORKSPACE=\"" + *path.value + L"\"";
					return LaunchWorkspaceTarget(option, false) == EWorkspaceWindowTransitionResult::Succeeded
						? workbench::projects::EProjectsActivationStatus::OpenedNewWindow
						: workbench::projects::EProjectsActivationStatus::Failed;
				};
				options.activateWorktree = [this](const std::wstring_view path, const bool thisWindow) {
					if (path.empty() || GetHwnd() == nullptr) {
						return workbench::projects::EProjectsActivationStatus::Failed;
					}
					if (thisWindow) {
						if (::IsIconic(GetHwnd())) ::ShowWindow(GetHwnd(), SW_RESTORE);
						::SetForegroundWindow(GetHwnd());
						return workbench::projects::EProjectsActivationStatus::FocusedCurrentWindow;
					}
					if (HasActiveEditorInput() && GetDocument()->m_cDocEditor.IsModified()) {
						return workbench::projects::EProjectsActivationStatus::Failed;
					}
					const auto result = ApplyFolderWorkspace(std::wstring(path), true);
					return result == EOpenWorkspaceFolderResult::Succeeded
						? workbench::projects::EProjectsActivationStatus::FocusedCurrentWindow
						: workbench::projects::EProjectsActivationStatus::Failed;
				};
				options.removeProject = [this](const workbench::projects::ProjectEntry& project) {
					if (m_workbenchRuntime == nullptr) {
						return workbench::projects::EProjectsRemovalStatus::Failed;
					}
					const auto projects = m_workbenchRuntime->Projects();
					if (projects == nullptr) {
						return workbench::projects::EProjectsRemovalStatus::Failed;
					}
					const auto removed = projects->Remove(project.uri);
					return removed.outcome == workbench::projects::EProjectCatalogOutcome::Succeeded
						? workbench::projects::EProjectsRemovalStatus::Removed
						: workbench::projects::EProjectsRemovalStatus::Failed;
				};
				return workbench::projects::CreateProjectsPage(GetHwnd(), std::move(options));
			},
		},
	};
	auto projectedHostViews = workbench::viewcontainer::ProjectHostViewPages(
		contributionSnapshot, hostViewProviders);
	if (!projectedHostViews.Succeeded()
		|| !m_viewContainerPages->RegisterContributedPages(
			std::move(projectedHostViews.descriptors)).Succeeded()) {
		m_viewContainerPages.reset();
		CloseWorkbench();
		return false;
	}
	if (!m_viewContainerPages->Create(GetHwnd())) {
		m_viewContainerPages.reset();
		CloseWorkbench();
		return false;
	}
	m_explorerTool = m_viewContainerPages->Explorer();
	m_outlineWorkbenchTool = m_viewContainerPages->Outline();
	m_scmTool = m_viewContainerPages->SourceControl();
	m_searchTool = m_viewContainerPages->Search();
	m_extensionsTool = m_viewContainerPages->Extensions();
	if (m_extensionsTool != nullptr && m_workbenchRuntime != nullptr) {
		m_extensionsTool->SetExtensionsChangedCallback([this] {
			// Runtime and language projections can refresh in-place. Native View pages
			// are a startup batch, so workbench contribution changes apply to the next window.
			if (auto* runtime = GetSenpRuntime()) runtime->NotifyExtensionsChanged();
			if (auto* languages = GetSenpLanguageService()) languages->NotifyExtensionsChanged();
			Views_Redraw();
		});
		m_extensionsTool->SetManagementService(m_workbenchRuntime->Extensions());
	}
	const auto workspaceRoot = GetSemanticWorkspaceRoot();
	m_explorerTool->SetRoot(workspaceRoot);
	m_scmTool->SetRoot(workspaceRoot);
	m_searchTool->SetRoot(workspaceRoot);
	m_searchTool->SetMatchActivationCallback([this](std::wstring_view path, std::int64_t line,
		int column, int) { OpenSearchMatch(path, line, column); });
	m_searchTool->SetFilesChangedCallback([this](const std::vector<std::wstring>& paths) {
		ReloadReplacedFiles(paths);
	});
	m_scmTool->SetTextResolver([](workbench::scm::EScmTextKey key, std::wstring_view argument) {
		return ResolveLocalizedScmText(key, argument);
	});
	m_scmTool->SetPublicationTextResolver([](std::string_view key, std::wstring_view argument) {
		return ResolveLocalizedScmTextKey(key, argument);
	});
	UpdateWorkbenchWelcomeState();
	m_explorerTool->SetFileActivationCallback([this](std::wstring_view path,
		workbench::explorer::ExplorerFileActivationKind kind) {
		OpenExplorerFile(path, kind);
	});
	// The Explorer's context menu and keybindings dispatch the same stable
	// command IDs through the same registry the SCM view uses; recognition is
	// terminal exactly as it is there.
	m_explorerTool->SetCommandCallback([this](std::string_view command, std::string_view argumentsJson) {
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand(command, handled, argumentsJson);
		return handled;
	});
	// Menu titles are the registry's facts. A command the registry does not know
	// resolves to the empty string and the Explorer menu fails closed rather than
	// rendering a partial menu with an invented label.
	m_explorerTool->SetMenuTitleResolver([this](std::string_view commandId) -> std::wstring {
		if (!m_workbenchCommandRegistry) {
			return {};
		}
		const auto descriptor = m_workbenchCommandRegistry->Find(commandId);
		if (!descriptor) {
			return {};
		}
		return ResolveLocalizedWorkbenchCommandTitle(*descriptor);
	});
	m_explorerTool->SetRenameCommitCallback([this](std::wstring_view path, std::wstring_view newName) {
		CommitExplorerRename(path, newName);
	});
	m_explorerTool->SetCreateCommitCallback([this](std::wstring_view parentDirectory,
		std::wstring_view name, bool directory) {
		CommitExplorerCreate(parentDirectory, name, directory);
	});
	m_scmTool->SetFileActivationCallback([this](std::wstring_view path) {
		const std::wstring ownedPath(path);
		GetActiveView().GetCommander().Command_FILEOPEN(ownedPath.c_str());
	});
	// The status bar renders the provider's own `statusBarCommands`, the same way
	// VS Code's SCMStatusBarController does. It must not build a second label out
	// of the parse result: the branch shown beside the file list and the branch
	// shown in the status bar would then be able to disagree.
	m_scmTool->SetStatusBarCommandsCallback([this](const std::vector<workbench::scm::ScmCommand>& commands) {
		m_cStatusBar.SetScmStatusCommands(commands);
		// The same publication the status bar renders is what the Activity Bar
		// badge counts, so both follow one notification instead of two.
		SyncScmActivityBadge();
	});
	// The repository row's own toolbar runs the same published commands the status
	// bar does, through the same registry, so the branch item and the row cannot
	// mean different things. `handled` is the registry's recognition, which is
	// terminal; an unrecognized id lets the view fall back to opening the file,
	// which is what `git.openFile` means while no diff editor exists.
	// A published `ScmCommand` carries its own `arguments`, and a resource-scoped
	// one is meaningless without them, so the payload travels with the id instead
	// of being dropped here.
	m_scmTool->SetCommandCallback([this](std::string_view command, std::string_view argumentsJson) {
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand(command, handled, argumentsJson);
		return handled;
	});
	// The built-in Git repository is published through the same SCM authority an
	// extension-contributed provider uses, so the view has one truth to render
	// instead of a private Git model beside the service. A window with no runtime
	// has no service to borrow and keeps rendering its own publication locally.
	m_scmTool->SetSourceControlService(m_workbenchRuntime != nullptr ? m_workbenchRuntime->Scm() : nullptr);
	// VS Code 1.18's Git status in the File Explorer: the Git extension publishes
	// file decorations and the Explorer renders them. The two views never talk to
	// each other upstream either -- a decorations service sits between them -- so
	// the composition root is what joins the provider to its consumer here.
	m_scmTool->SetFileDecorationsCallback(
		[this](std::vector<workbench::decorations::FileDecorationEntry> entries) {
			if (m_explorerTool == nullptr) return;
			workbench::decorations::FileDecorationTable table;
			if (m_gitDecorationsEnabled) table.Replace(std::move(entries));
			m_explorerTool->SetFileDecorations(std::move(table));
		});
	ApplyExplorerDecorationSettings();
	ApplyScmInputLineCountSetting();

	const auto requestOutlineExpanded = [this](bool expanded) {
		if (m_workbenchRuntime != nullptr) {
			return SetBuiltinViewVisibility(workbench::layout::ids::view::Outline, expanded);
		}
		auto& workbenchSettings = m_pShareData->m_Common.m_sWorkbench;
		const BOOL value = expanded ? TRUE : FALSE;
		if (workbenchSettings.m_bRightPanelVisible != value) {
			workbenchSettings.m_bRightPanelVisible = value;
			BroadcastWorkbenchSettings();
		}
		return true;
	};
	const auto refreshOutlineAfterReveal = [this]() {
		if (m_dispatchReady) ReloadWorkbenchOutlineAndRelayout();
	};
	const auto onHeaderDrag = [this](workbench::WorkbenchEdge edge, POINT screenPoint) {
		const auto* source = edge == workbench::WorkbenchEdge::Right ? m_auxiliaryBarHost : m_sidebarHost;
		if (source == nullptr) return;
		const auto page = source->ActivePage();
		if (page.empty()) return;
		if (const auto target = HitTestSideBarEdge(screenPoint); target && *target != edge) {
			MoveViewContainerToEdge(page, *target);
		}
	};

	m_leftWorkbenchPanel = std::make_unique<workbench::CWorkbenchPanelHost>(
		workbench::WorkbenchEdge::Left, settings.m_nLeftPanelExtent96, commitExtent);
	auto sidebarHost = std::make_unique<workbench::viewcontainer::CViewContainerHost>(
		m_viewContainerPages, "workbench.parts.sidebar",
		requestOutlineExpanded, refreshOutlineAfterReveal);
	m_sidebarHost = sidebarHost.get();
	m_leftWorkbenchPanel->SetHeaderDragCallback(onHeaderDrag);
	if (!m_leftWorkbenchPanel->Create(GetHwnd(), G_AppInstance(), std::move(sidebarHost))) {
		m_sidebarHost = nullptr;
		m_leftWorkbenchPanel.reset();
	}

	// VS Code's Secondary Side Bar is empty until a ViewContainer is moved into it, but it
	// is the very same composite host, so it renders a borrowed page once one arrives.
	m_rightWorkbenchPanel = std::make_unique<workbench::CWorkbenchPanelHost>(
		workbench::WorkbenchEdge::Right, settings.m_nAuxiliaryBarExtent96, commitExtent);
	m_rightWorkbenchPanel->SetTitle(LocalizedWorkbenchString(STR_WORKBENCH_SECONDARY_SIDEBAR_TITLE));
	auto auxiliaryHost = std::make_unique<workbench::viewcontainer::CViewContainerHost>(
		m_viewContainerPages, "workbench.parts.auxiliarybar",
		requestOutlineExpanded, refreshOutlineAfterReveal);
	m_auxiliaryBarHost = auxiliaryHost.get();
	m_rightWorkbenchPanel->SetHeaderDragCallback(onHeaderDrag);
	if (!m_rightWorkbenchPanel->Create(GetHwnd(), G_AppInstance(), std::move(auxiliaryHost))) {
		m_auxiliaryBarHost = nullptr;
		m_rightWorkbenchPanel.reset();
	}
	if (m_sidebarHost != nullptr) {
		// Explorer is VS Code's default Primary Side Bar container.
		m_sidebarHost->ShowPage(workbench::viewcontainer::pageIds::Explorer);
	}
	RefreshSidebarTitles();

	m_bottomWorkbenchPanel = std::make_unique<workbench::CWorkbenchPanelHost>(
		workbench::WorkbenchEdge::Bottom, settings.m_nBottomPanelExtent96, commitExtent);
	terminal::TerminalTabManagerDependencies terminalDependencies;
	if (auto* process = CProcess::getInstance()) {
		terminalDependencies.runtimeService = process->GetTerminalRuntimeService();
		terminalDependencies.launchProfiles = process->GetTerminalLaunchProfiles();
	}
	auto bottomPanelTool = std::make_unique<workbench::panel::CBottomPanelTool>(
		std::move(terminalDependencies), m_viewContainerPages);
	m_bottomPanelTool = bottomPanelTool.get();
	m_terminalTool = bottomPanelTool->Terminal();
	bottomPanelTool->SetContainerSelectionCallback(
		[this](const std::string_view containerId) {
			return m_workbenchRuntime == nullptr
				|| ActivateWorkbenchViewContainer(containerId, false);
		});
	bottomPanelTool->SetPanelActions({
		.closePanel = [this]() {
			SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Bottom, false, false);
		},
		.toggleMaximize = [this]() {
			ToggleBottomWorkbenchMaximized();
		},
		.isMaximized = [this]() {
			return m_bottomWorkbenchMaximized;
		},
	});
	m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
	m_terminalTool->SetShortcutPresetSink([this](terminal::TerminalShortcutPreset preset) {
		(void)PersistTerminalShortcutPresetSelection(preset);
	});
	ApplyTerminalShortcutPresetSetting();
	ApplyTerminalScrollbackSetting();
	ApplyTerminalTabPresentationSettings();
		m_terminalTool->SetPanelActions({
			.renderPanelActions = false,
			.renderHeader = false,
		});
	if (!m_bottomWorkbenchPanel->Create(GetHwnd(), G_AppInstance(), std::move(bottomPanelTool))) {
		m_bottomPanelTool = nullptr;
		m_terminalTool = nullptr;
		m_bottomWorkbenchPanel.reset();
	}
	if (!InitializePaneCompositeProjection()) {
		CloseWorkbench();
		return false;
	}

	m_activityBar = std::make_unique<workbench::CActivityBar>([this](std::string_view containerId) {
		// A container the page pool cannot render would toggle a side bar that then shows
		// nothing, so the click is ignored rather than producing an empty Part.
		if (m_viewContainerPages == nullptr || !m_viewContainerPages->Contains(containerId)) return;
		// VS Code's default vertical Activity Bar hides the Primary Side Bar when the clicked
		// ViewContainer already is the visible active one. Its top/bottom composite bars instead
		// focus that view and stay visible. Hiding therefore belongs only to the vertical click
		// gesture, never to `workbench.view.*`, which only ever reveals a container.
		if (m_workbenchRuntime != nullptr) {
			const bool toggleIfActive = workbench::ResolveActivityBarActiveIconClickBehavior(
				m_activityBarLocation)
				== workbench::EActivityBarActiveIconClickBehavior::TogglePrimarySideBar;
			const std::string_view commandId = toggleIfActive
				&& IsSidebarViewContainerActive(containerId)
				? std::string_view("workbench.action.toggleSidebarVisibility")
				: containerId == workbench::viewcontainer::pageIds::Explorer
					? std::string_view("workbench.view.explorer")
					: containerId == workbench::viewcontainer::pageIds::Extensions
						? std::string_view("workbench.view.extensions")
					: std::string_view();
			if (!commandId.empty()) {
				bool handled = false;
				(void)TryExecuteWorkbenchStableCommand(commandId, handled);
				if (handled) return;
			}
			ActivateSidebarPage(containerId, toggleIfActive);
			return;
		}
		ActivateSidebarPage(containerId, true);
	});
	m_auxiliaryActivityBar = std::make_unique<workbench::CActivityBar>(
		[this](std::string_view containerId) {
			if (m_workbenchRuntime == nullptr || m_viewContainerPages == nullptr
				|| !m_viewContainerPages->Contains(containerId)) return;
			if (IsAuxiliaryViewContainerActive(containerId)) {
				SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Right, false, false);
				return;
			}
			static_cast<void>(ActivateWorkbenchViewContainer(containerId, true));
		});
	const auto showGlobalAction = [this](std::string_view actionId, POINT screenPoint) {
		if (m_customFrame == nullptr) return;
		if (actionId == workbench::activity::kAccountsActivityId) {
			m_customFrame->ShowAccountMenuAt(screenPoint);
		} else if (actionId == workbench::activity::kManageActivityId) {
			m_customFrame->ShowManageMenuAt(screenPoint);
		}
	};
	m_activityBar->SetGlobalActionCallback(showGlobalAction);
	m_auxiliaryActivityBar->SetGlobalActionCallback(showGlobalAction);
	const auto changeLocation = [this](workbench::ActivityBarLocation location) {
		std::string_view commandId;
		switch (location) {
		case workbench::ActivityBarLocation::Default:
			commandId = "workbench.action.activityBarLocation.default"; break;
		case workbench::ActivityBarLocation::Top:
			commandId = "workbench.action.activityBarLocation.top"; break;
		case workbench::ActivityBarLocation::Bottom:
			commandId = "workbench.action.activityBarLocation.bottom"; break;
		default:
			return;
		}
		bool handled = false;
		if (m_workbenchCommandRegistry != nullptr) {
			static_cast<void>(TryExecuteWorkbenchStableCommand(commandId, handled));
			return;
		}
		static_cast<void>(SetActivityBarLocation(location, true));
	};
	m_activityBar->SetLocationRequestCallback(changeLocation);
	m_auxiliaryActivityBar->SetLocationRequestCallback(changeLocation);
	const auto locationLabels = ActivityBarLocationMenuLabels();
	m_activityBar->SetLocationMenuLabels(locationLabels);
	m_auxiliaryActivityBar->SetLocationMenuLabels(locationLabels);
	// VS Code's Activity Bar icon is a composite drag handle: dropping it on another side
	// bar runs the same `moveViewContainerToLocation` the Command Palette move uses.
	m_activityBar->SetContainerDragCallback([this](std::string_view containerId, POINT screenPoint) {
		if (m_viewContainerPages == nullptr || !m_viewContainerPages->Contains(containerId)) return;
		if (m_activityBar != nullptr && ReorderViewContainerInActivityBar(containerId,
			workbench::layout::EWorkbenchViewContainerLocation::SideBar,
			*m_activityBar, screenPoint)) return;
		if (const auto target = HitTestSideBarEdge(screenPoint);
			target && *target != workbench::WorkbenchEdge::Left) {
			MoveViewContainerToEdge(containerId, *target);
		}
	});
	m_auxiliaryActivityBar->SetContainerDragCallback(
		[this](std::string_view containerId, POINT screenPoint) {
			if (m_viewContainerPages == nullptr || !m_viewContainerPages->Contains(containerId)) return;
			if (m_auxiliaryActivityBar != nullptr && ReorderViewContainerInActivityBar(containerId,
				workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar,
				*m_auxiliaryActivityBar, screenPoint)) return;
			if (const auto target = HitTestSideBarEdge(screenPoint);
				target && *target != workbench::WorkbenchEdge::Right) {
				MoveViewContainerToEdge(containerId, *target);
			}
		});
	if (!m_activityBar->Create(GetHwnd(), G_AppInstance())) m_activityBar.reset();
	if (!m_auxiliaryActivityBar->Create(GetHwnd(), G_AppInstance())) {
		m_auxiliaryActivityBar.reset();
	}
	ApplyActivityBarLocationSetting();
	ApplyMiniMapSettings();
	ApplyIndentGuideSettings();
	// The strip is empty until it is projected, and the first layout commit can be far away
	// (or never arrive at all on the legacy path), so seed it as soon as the window exists.
	SyncViewContainers(nullptr);

	const bool hasEditorAdapter = m_editorServiceAdapter != nullptr;
	const bool hasLegacyBackend = m_legacyEditorBackend != nullptr;
	if (hasEditorAdapter != hasLegacyBackend) {
		CloseWorkbench();
		return false;
	}
	const bool editorBridgeEnabled = hasEditorAdapter && hasLegacyBackend;
	if (editorBridgeEnabled) {
		m_emptyEditorSurface = std::make_unique<workbench::editor::CEmptyEditorSurface>(
			[this](std::string_view commandId) {
				// Watermark actions share the same stable command authority as the
				// native menu and keybinding.  A recognized failure is terminal and
				// must not fall through to a legacy implementation.
				bool handled = false;
				(void)TryExecuteWorkbenchStableCommand(commandId, handled);
				if (!handled) (void)ExecuteWorkbenchEditorCommand(commandId);
			});
		if (!m_emptyEditorSurface->Create(GetHwnd(), G_AppInstance())) {
			m_emptyEditorSurface.reset();
		}
		m_diffSurface = std::make_unique<CDiffSurface>();
		if (m_diffSurface->Open(G_AppInstance(), GetHwnd()) == nullptr) {
			m_diffSurface.reset();
		} else {
			m_diffSurface->Hide();
			m_diffSurface->SetOnCloseRequested([this]() { ClearDiffSurface(); });
		}
	}

	const bool initialized = m_leftWorkbenchPanel != nullptr
		&& m_rightWorkbenchPanel != nullptr
		&& m_bottomWorkbenchPanel != nullptr
		&& m_activityBar != nullptr
		&& m_auxiliaryActivityBar != nullptr
		&& (!editorBridgeEnabled
			|| (m_emptyEditorSurface != nullptr && m_diffSurface != nullptr));
	if (!initialized) {
		// Workbench initialization is all-or-nothing. Do not leave an editor in
		// an unobservable partial state where a configured tool has no HWND.
		CloseWorkbench();
		return false;
	}
	if (m_workbenchRuntime != nullptr) {
		m_workbenchContextKeyService = std::make_unique<workbench::commands::WorkbenchContextKeyService>();
		m_workbenchCommandRegistry = std::make_unique<workbench::commands::WorkbenchCommandRegistry>();
		const auto registration = m_workbenchCommandRegistry->RegisterBuiltinCommands({
			.showCommands = [this]() {
				return ShowCommandPalette()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "command palette is unavailable" };
			},
			.openSettings = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::OpenSettings)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "settings dialog could not be opened" };
			},
			.openFolder = [this]() {
				return ExecuteOpenWorkspaceFolderCommand();
			},
			.newUntitledFile = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::NewUntitledFile)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "new untitled editor could not be created" };
			},
			.newWindow = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::NewWindow)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "new window could not be started" };
			},
			.openFile = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::OpenFile)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "file picker could not be opened" };
			},
			.openWorkspace = [this]() {
				switch (OpenWorkspaceConfiguration()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "workspace selection was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "workspace transition failed" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "workspace command returned an invalid terminal status" };
			},
			.openRecent = [this]() {
				switch (ShowRecentlyOpenedWorkspaceMenu()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "recent selection was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "recent workspace transition failed" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "recent workspace command returned an invalid terminal status" };
			},
			.clearRecentFiles = [this]() {
				switch (ClearRecentlyOpenedHistory()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "clearing recently opened items was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "recent workspace history could not be cleared" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "clear recently opened returned an invalid terminal status" };
			},
			.addRootFolder = [this]() {
				switch (AddFolderToWorkspace()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "folder selection was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "workspace update transition failed" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "add folder returned an invalid terminal status" };
			},
			.saveWorkspaceAs = [this]() {
				switch (SaveWorkspaceAs()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "workspace save target selection was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "workspace save transition failed" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "save workspace returned an invalid terminal status" };
			},
			.duplicateWorkspaceInNewWindow = [this]() {
				switch (DuplicateWorkspaceInNewWindow()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "workspace duplication was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "workspace duplication transition failed" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "duplicate workspace returned an invalid terminal status" };
			},
			.save = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::Save)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "active editor save failed" };
			},
			.saveAs = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::SaveAs)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "active editor save as failed" };
			},
			.saveAll = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::SaveAll)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "save all dispatch failed" };
			},
			.closeActiveEditor = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::CloseActiveEditor)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "active editor close failed" };
			},
			.closeFolder = [this]() {
				switch (CloseWorkspaceWindow()) {
				case EWorkspaceWindowTransitionResult::Succeeded:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
				case EWorkspaceWindowTransitionResult::Cancelled:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable, "workspace close was cancelled" };
				case EWorkspaceWindowTransitionResult::Failed:
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "workspace close transition failed" };
				}
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "close workspace returned an invalid terminal status" };
			},
			.closeWindow = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::CloseWindow)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "window close dispatch failed" };
			},
			.quit = [this]() {
				return ExecuteWorkbenchEditorCommand(workbench::editor::command_ids::Quit)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "quit dispatch failed" };
			},
			.openGlobalKeybindings = [this]() {
				return CEditApp::getInstance()->OpenPropertySheet(ID_PROPCOM_PAGENUM_KEYBOARD)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed,
						"keyboard shortcuts settings could not be opened" };
			},
			.toggleSidebarVisibility = [this]() {
				return ExecuteToggleSidebarVisibilityCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "sidebar layout command failed" };
			},
			.activityBarLocationDefault = [this]() {
				return SetActivityBarLocation(workbench::ActivityBarLocation::Default, true)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed,
						"activity bar location could not be saved" };
			},
			.activityBarLocationTop = [this]() {
				return SetActivityBarLocation(workbench::ActivityBarLocation::Top, true)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed,
						"activity bar location could not be saved" };
			},
			.activityBarLocationBottom = [this]() {
				return SetActivityBarLocation(workbench::ActivityBarLocation::Bottom, true)
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed,
						"activity bar location could not be saved" };
			},
			.showExplorer = [this]() {
				return ExecuteShowExplorerCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "explorer layout command failed" };
			},
			.showExtensions = [this]() {
				return ExecuteShowExtensionsCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "extensions layout command failed" };
			},
			.showProblems = [this]() {
				return ExecuteShowProblemsCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "problems layout command failed" };
			},
			.toggleOutput = [this]() {
				return ExecuteToggleOutputCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "output layout command failed" };
			},
			.selectTheme = [this]() {
				return ShowColorThemePicker()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "no color theme is available" };
			},
			.showNotifications = []() {
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Unsupported,
					"notification center is unavailable" };
			},
			.hideNotifications = []() {
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Unsupported,
					"notification center is unavailable" };
			},
			.toggleStatusbarVisibility = [this]() {
				DispatchEditorFunction(F_SHOWSTATUSBAR);
				return workbench::commands::WorkbenchCommandExecutionResult{
					workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
			},
			.markdownShowPreview = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ShowPreview);
			},
			.markdownShowPreviewToSide = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ShowPreviewToSide);
			},
			.markdownShowLockedPreviewToSide = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ShowLockedPreviewToSide);
			},
			.markdownShowSource = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ShowSource);
			},
			.markdownShowPreviewSecuritySelector = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ShowPreviewSecuritySelector);
			},
			.markdownPreviewRefresh = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::Refresh);
			},
			.markdownPreviewToggleLock = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ToggleLock);
			},
			.markdownReopenAsPreview = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ReopenAsPreview);
			},
			.markdownReopenAsSource = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::ReopenAsSource);
			},
			.markdownTogglePreview = [this]() {
				return ExecuteMarkdownPreviewCommand(markdown::MarkdownPreviewCommand::TogglePreview);
			},
			// VS Code's two API commands. They are registered by the workbench, not
			// by the Git provider, because upstream registers them in
			// `workbench/api/common/apiCommands.ts` — any caller may issue them, and
			// the Git provider is only their best-known one.
			.vscodeDiff = [this](std::string_view argumentsJson) {
				return ExecuteVsCodeDiffCommand(argumentsJson);
			},
			.vscodeOpen = [this](std::string_view argumentsJson) {
				return ExecuteVsCodeOpenCommand(argumentsJson);
			},
			.vscodeOpenFolder = [this](std::string_view argumentsJson) {
				if (!argumentsJson.empty() && argumentsJson != "[]") {
					return workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Unsupported,
						"URI-based vscode.openFolder is not supported" };
				}
				return ExecuteOpenWorkspaceFolderCommand();
			},
		});
		if (!registration.Succeeded()) {
			CloseWorkbench();
			return false;
		}
		// The built-in Git provider contributes its own commands, exactly as
		// `vscode.git` does, rather than being folded into the workbench shell's
		// list. Every command in this batch has a real executor: a registered id
		// with an empty executor would turn a missing route into a silent no-op
		// instead of the typed `Unsupported` the registry returns for an id it
		// does not know at all. See `workbench/scm/CLAUDE.md`.
		const auto gitRegistration = m_workbenchCommandRegistry->RegisterGitCommands({
			.checkout = [this]() { return ExecuteGitBranchCommand(EGitBranchCommand::Checkout); },
			.checkoutDetached = [this]() { return ExecuteGitBranchCommand(EGitBranchCommand::CheckoutDetached); },
			.branch = [this]() { return ExecuteGitBranchCommand(EGitBranchCommand::Branch); },
			.branchFrom = [this]() { return ExecuteGitBranchCommand(EGitBranchCommand::BranchFrom); },
			.init = [this](std::string_view argumentsJson) {
				return ExecuteGitInitCommand(argumentsJson);
			},
			.clone = [this]() { return ExecuteGitCloneCommand(false); },
			.cloneRecursive = [this]() { return ExecuteGitCloneCommand(true); },
			.stage = [this](std::string_view argumentsJson) {
				return ExecuteGitStageCommand(EGitStageCommand::Stage, argumentsJson);
			},
			.unstage = [this](std::string_view argumentsJson) {
				return ExecuteGitStageCommand(EGitStageCommand::Unstage, argumentsJson);
			},
			.clean = [this](std::string_view argumentsJson) {
				return ExecuteGitStageCommand(EGitStageCommand::Clean, argumentsJson);
			},
			.openChange = [this](std::string_view argumentsJson) {
				return ExecuteGitOpenChangeCommand(argumentsJson);
			},
			.stageAll = [this]() { return ExecuteGitStageCommand(EGitStageCommand::StageAll, {}); },
			.unstageAll = [this]() { return ExecuteGitStageCommand(EGitStageCommand::UnstageAll, {}); },
			.cleanAll = [this]() { return ExecuteGitStageCommand(EGitStageCommand::CleanAll, {}); },
			.commit = [this](std::string_view argumentsJson) {
				return ExecuteGitCommitCommand(EGitCommitCommand::Commit, argumentsJson);
			},
			.commitAmend = [this]() { return ExecuteGitCommitCommand(EGitCommitCommand::CommitAmend); },
			.undoCommit = [this]() { return ExecuteGitCommitCommand(EGitCommitCommand::UndoCommit); },
			.stageSelectedRanges = [this]() { return ExecuteGitSelectedRangesCommand(true); },
			.unstageSelectedRanges = [this]() { return ExecuteGitSelectedRangesCommand(false); },
			.fetch = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::Fetch); },
			.fetchPrune = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::FetchPrune); },
			.fetchAll = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::FetchAll); },
			.pull = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::Pull); },
			.pullRebase = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::PullRebase); },
			.push = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::Push); },
			.sync = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::Sync); },
			.syncRebase = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::SyncRebase); },
			.publish = [this]() { return ExecuteGitSyncCommand(EGitSyncCommand::Publish); },
			.refresh = [this]() {
				using workbench::commands::EWorkbenchCommandExecutionStatus;
				if (m_scmTool == nullptr) {
					return workbench::commands::WorkbenchCommandExecutionResult{
						EWorkbenchCommandExecutionStatus::NotApplicable,
						"no source control view exists in this window" };
				}
				m_scmTool->Refresh();
				return workbench::commands::WorkbenchCommandExecutionResult{
					EWorkbenchCommandExecutionStatus::Succeeded, {} };
			},
			.showOutput = [this]() {
				using workbench::commands::EWorkbenchCommandExecutionStatus;
				if (m_bottomPanelTool == nullptr) {
					return workbench::commands::WorkbenchCommandExecutionResult{
						EWorkbenchCommandExecutionStatus::NotApplicable,
						"no panel exists in this window" };
				}
				// Upstream reveals the Git extension's own output channel. This
				// product publishes Git's output into the shared Output view, so
				// revealing that view is the same destination, not an approximation.
				if (!IsWorkbenchPanelVisible(workbench::WorkbenchEdge::Bottom)) {
					ToggleWorkbenchPanel(workbench::WorkbenchEdge::Bottom, true);
				}
				m_bottomPanelTool->ShowOutput();
				return workbench::commands::WorkbenchCommandExecutionResult{
					EWorkbenchCommandExecutionStatus::Succeeded, {} };
			},
			.copyCommitId = [this](std::string_view argumentsJson) {
				return ExecuteGitCopyCommitCommand(argumentsJson, false);
			},
			.copyCommitMessage = [this](std::string_view argumentsJson) {
				return ExecuteGitCopyCommitCommand(argumentsJson, true);
			},
		});
		if (!gitRegistration.Succeeded()) {
			CloseWorkbench();
			return false;
		}
		// The Files Explorer's eight resource-scoped file-operation commands, in
		// upstream's own IDs and surface shapes; the Explorer surface behavior and
		// its recorded divergences live in `workbench/explorer/CLAUDE.md`.
		const auto explorerRegistration = m_workbenchCommandRegistry->RegisterExplorerCommands({
			.createFileFromExplorer = [this]() -> workbench::commands::WorkbenchCommandExecutionResult {
				using workbench::commands::EWorkbenchCommandExecutionStatus;
				if (m_explorerTool == nullptr) {
					return workbench::commands::WorkbenchCommandExecutionResult{
						EWorkbenchCommandExecutionStatus::NotApplicable,
						"no explorer view exists in this window" };
				}
				return { m_explorerTool->CreateEntryFromSelection(false)
					? EWorkbenchCommandExecutionStatus::Succeeded
					: EWorkbenchCommandExecutionStatus::NotApplicable, {} };
			},
			.createFolderFromExplorer = [this]() -> workbench::commands::WorkbenchCommandExecutionResult {
				using workbench::commands::EWorkbenchCommandExecutionStatus;
				if (m_explorerTool == nullptr) {
					return workbench::commands::WorkbenchCommandExecutionResult{
						EWorkbenchCommandExecutionStatus::NotApplicable,
						"no explorer view exists in this window" };
				}
				return { m_explorerTool->CreateEntryFromSelection(true)
					? EWorkbenchCommandExecutionStatus::Succeeded
					: EWorkbenchCommandExecutionStatus::NotApplicable, {} };
			},
			.refreshFilesExplorer = [this]() {
				using workbench::commands::EWorkbenchCommandExecutionStatus;
				if (m_explorerTool == nullptr) {
					return workbench::commands::WorkbenchCommandExecutionResult{
						EWorkbenchCommandExecutionStatus::NotApplicable,
						"no explorer view exists in this window" };
				}
				m_explorerTool->Refresh();
				return workbench::commands::WorkbenchCommandExecutionResult{
					EWorkbenchCommandExecutionStatus::Succeeded, {} };
			},
			.collapseExplorerFolders = [this]() {
				using workbench::commands::EWorkbenchCommandExecutionStatus;
				if (m_explorerTool == nullptr) {
					return workbench::commands::WorkbenchCommandExecutionResult{
						EWorkbenchCommandExecutionStatus::NotApplicable,
						"no explorer view exists in this window" };
				}
				m_explorerTool->CollapseAllFolders();
				return workbench::commands::WorkbenchCommandExecutionResult{
					EWorkbenchCommandExecutionStatus::Succeeded, {} };
			},
			.newFile = [this](std::string_view argumentsJson) { return ExecuteExplorerNewEntry(argumentsJson, false); },
			.newFolder = [this](std::string_view argumentsJson) { return ExecuteExplorerNewEntry(argumentsJson, true); },
			.renameFile = [this](std::string_view argumentsJson) { return ExecuteExplorerRenameFile(argumentsJson); },
			.moveFileToTrash = [this](std::string_view argumentsJson) { return ExecuteExplorerDelete(argumentsJson, true); },
			.deleteFile = [this](std::string_view argumentsJson) { return ExecuteExplorerDelete(argumentsJson, false); },
			.copyFilePath = [this](std::string_view argumentsJson) { return ExecuteExplorerCopyPath(argumentsJson, false); },
			.copyRelativeFilePath = [this](std::string_view argumentsJson) { return ExecuteExplorerCopyPath(argumentsJson, true); },
			.revealFileInOS = [this](std::string_view argumentsJson) { return ExecuteExplorerRevealInOS(argumentsJson); },
		});
		if (!explorerRegistration.Succeeded()) {
			CloseWorkbench();
			return false;
		}
		// Registered before the stack exists, because the stack is optional and the
		// commands are not: a machine with no writable staging root must still
		// answer `update.check` with a typed `Unsupported`, not with the
		// `UnknownCommand` an unregistered id would produce.
		const auto updateRegistration = m_workbenchCommandRegistry->RegisterUpdateCommands({
			.checkForUpdates = [this]() { return ExecuteUpdateCommand(EUpdateCommand::CheckForUpdates); },
			.downloadUpdate = [this]() { return ExecuteUpdateCommand(EUpdateCommand::DownloadUpdate); },
			.applyUpdate = [this]() { return ExecuteUpdateCommand(EUpdateCommand::ApplyUpdate); },
			.quitAndInstall = [this]() { return ExecuteUpdateCommand(EUpdateCommand::QuitAndInstall); },
			.showUpdateInfo = [this]() { return ExecuteUpdateCommand(EUpdateCommand::ShowUpdateInfo); },
		});
		if (!updateRegistration.Succeeded()) {
			CloseWorkbench();
			return false;
		}
		ConfigureCustomFrameActions();
		// After the frame callbacks exist, so the first committed state can paint
		// the indicator instead of waiting for the next transition.
		InitializeUpdateProjection();
	}
	if (m_workbenchRuntime != nullptr && !InitializeWorkbenchServiceProjection()) {
		CloseWorkbench();
		return false;
	}
	m_colorThemeRegistry = std::make_unique<theme::CColorThemeRegistry>();
	RefreshColorThemes();
	if (m_workbenchRuntime != nullptr && GetHwnd() != nullptr) {
		try {
			auto gate = std::make_shared<ThemeConfigurationGate>();
			gate->window = GetHwnd();
			auto subscription = m_workbenchRuntime->Configuration().Subscribe(
				[gate](const std::vector<config::ConfigurationChange>& changes) {
					ThemeConfigurationGate::Notify(gate, changes);
				});
			m_themeConfigurationGate = gate;
			m_themeConfigurationSubscription =
				std::make_unique<config::ConfigurationSubscription>(std::move(subscription));
		}
		catch (...) {
			// The selected setting is still read during startup. If the advisory
			// watcher cannot be subscribed, a later window reopen remains the safe
			// terminal fallback instead of keeping a dangling callback.
			m_themeConfigurationGate.reset();
			m_themeConfigurationSubscription.reset();
		}
	}

	m_bottomPanelTool->SetProblemActivationCallback(
		[this](const workbench::win32::ProblemsPanelEntry& problem) {
			const auto uri = platform::uri::Uri::Parse(problem.resourceUri);
			if (!uri.value) return;
			const auto path = uri.value->ToWindowsPath();
			if (!path.value) return;
			// The marker's range start is already a zero-based UTF-16 position,
			// which is what the shared activation path takes.
			OpenDocumentAtMarkerPosition(*path.value, problem.range.startLine,
				problem.range.startColumn);
		});
	m_bottomPanelTool->SetOutputChannelSelectionCallback([this](const std::string& channelId) {
		if (m_outputService == nullptr) return false;
		try {
			const auto snapshot = m_outputService->Snapshot();
			const auto channel = std::ranges::find(snapshot.channels, channelId,
				&workbench::output::OutputChannelSnapshot::channelId);
			if (channel == snapshot.channels.end()) return false;
			auto operationId = NextOutputPanelOperationId();
			if (!operationId) return false;
			const auto result = m_outputService->Show({
				.operation = {
					.operationId = std::move(*operationId),
					.expectedRevision = snapshot.revision,
				},
				.owner = channel->owner,
				.channelId = channelId,
				.preserveFocus = true,
			});
			return result.Succeeded()
				|| (result.status == workbench::output::EOutputOperationStatus::NotApplicable
					&& result.reason == workbench::output::EOutputOperationReason::None);
		}
		catch (...) {
			return false;
		}
	});
	m_cStatusBar.SetWorkbenchCommandCallback([this](std::string_view command) {
		// Remote Development is not a supported authority in this product. Keep the
		// VS Code `status.host` affordance visible, but fail closed instead of
	// approximating SSH/WSL/container windows with local state.
	if (command == "workbench.action.remote.showMenu") {
		const auto unsupportedMessage = LocalizedWorkbenchString(STR_WORKBENCH_REMOTE_UNSUPPORTED);
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS,
			unsupportedMessage.c_str());
		return;
	}
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand(command, handled);
	});
	m_cStatusBar.SetStatusbarVisibilityCallback([this](std::string_view id, bool hidden) {
		SetStatusbarEntryHidden(id, hidden);
	});
	RefreshStatusbarPresentation();

	if (m_workbenchRuntime != nullptr) {
		const HWND editorWindow = GetHwnd();
		try {
			m_layoutStateSubscription = m_workbenchRuntime->LayoutState().Subscribe(
				[editorWindow](const workbench::layout::WorkbenchLayoutChangeBatch&) {
					if (::IsWindow(editorWindow)) {
						::PostMessageW(editorWindow, MYWM_WORKBENCH_LAYOUT_CHANGED, 0, 0);
					}
				});
		}
		catch (...) {
			m_layoutStateSubscription.reset();
		}
		if (!m_layoutStateSubscription) {
			CloseWorkbench();
			return false;
		}
	}

	(void)ApplyWorkbenchTheme();
	ApplyWorkbenchSettingsFromSharedData(false);
	if (!ApplyInitialWorkbenchLayoutState()) {
		CloseWorkbench();
		return false;
	}
	if (editorBridgeEnabled) {
		const HWND editorWindow = GetHwnd();
		m_editorCoreSubscription = m_editorServiceAdapter->Subscribe(
			[editorWindow](const workbench::editor::EditorCoreChangeBatch&) {
				if (::IsWindow(editorWindow)) {
					::PostMessageW(editorWindow, MYWM_EDITOR_CORE_CHANGED, 0, 0);
				}
			});
		if (!m_editorCoreSubscription) {
			CloseWorkbench();
			return false;
		}
		ApplyEditorCoreSnapshot(m_editorServiceAdapter->Snapshot(), false);
	}
	m_startupOutlineReloadPending = m_workbenchRuntime == nullptr && settings.m_bRightPanelVisible != FALSE;
	return true;
}

void CEditWnd::PostDeferredStartupWorkbenchIfReady()
{
	if (m_startupWorkbenchCompletionPosted
		|| m_startupDrawState != StartupDrawState::Committed
		|| !m_startupFirstContentPainted
		|| !m_cDlgFuncList.m_bEditWndReady
		|| !m_startupOutlineReloadPending) {
		return;
	}

	m_startupWorkbenchCompletionPosted = true;
	if (!::PostMessageW(GetHwnd(), MYWM_COMPLETE_STARTUP_WORKBENCH, 0, 0)) {
		m_startupWorkbenchCompletionPosted = false;
		// If the queue cannot accept the internal message, still give every
		// pending branch an explicit terminal state while the window is valid.
		CompleteDeferredStartupWorkbench();
	}
}

void CEditWnd::CompleteDeferredStartupWorkbench()
{
	// Clear each request before doing any callback-driven work.  A load that is
	// triggered reentrantly can set a fresh request which belongs to the next
	// completion rather than being accidentally consumed here.
	const bool reloadOutline = std::exchange(m_startupOutlineReloadPending, false);
	if (!reloadOutline) return;
	const auto& settings = m_pShareData->m_Common.m_sWorkbench;
	if (reloadOutline
		&& settings.m_bRightPanelVisible != FALSE
		&& m_outlineWorkbenchTool != nullptr
		&& IsOutlineViewExpanded()) {
		ReloadWorkbenchOutlineAndRelayout();
	}
}

std::vector<std::wstring> CEditWnd::BuildActiveDocumentBreadcrumbSegments() const
{
	if (!HasActiveEditorInput() || m_workbenchRuntime == nullptr || GetDocument() == nullptr) return {};
	const auto& file = GetDocument()->m_cDocFile;
	if (!file.GetFilePathClass().IsValidPath()) return {};

	std::vector<std::wstring> workspaceRoots;
	for (const auto& folder : m_workbenchRuntime->WorkspaceContext().Snapshot().folders) {
		const auto windowsPath = folder.uri.ToWindowsPath();
		if (windowsPath && windowsPath.value) workspaceRoots.push_back(*windowsPath.value);
	}
	return breadcrumbs::BuildDocumentBreadcrumbs(file.GetFilePath(), workspaceRoots).segments;
}

void CEditWnd::RefreshColorThemes()
{
	if (!m_colorThemeRegistry) return;
	m_colorThemeRegistry->Clear();
	(void)m_colorThemeRegistry->RegisterBuiltinThemes();
}

bool CEditWnd::PersistColorThemeSelection(std::wstring_view themeId)
{
	if (m_workbenchRuntime == nullptr || themeId.empty()) return false;
	try {
		const auto& profile = m_workbenchRuntime->Bootstrap().UserDataProfile();
		config::ConfigurationTarget sourceTarget;
		sourceTarget.profileId = profile.SelectedProfileId();
		const config::ConfigurationSource source {
			config::EConfigurationScope::Profile,
			sourceTarget,
			"profile.settings",
			0,
		};
		config::editing::ConfigurationDocumentEditTarget editTarget;
		editTarget.scope = config::editing::EConfigurationDocumentScope::Profile;
		editTarget.target = sourceTarget;
		editTarget.resource = profile.Resources().Settings();
		const config::SettingsWritebackRequest request {
			.edit = {
				.target = std::move(editTarget),
				.key = "workbench.colorTheme",
				.value = config::ConfigurationValue(std::wstring(themeId)),
			},
			.documentKey = "profile.settings",
			.source = source,
		};
		const auto result = m_workbenchRuntime->WriteSetting(request);
		if (result.Succeeded()) return true;
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS,
			LocalizedWorkbenchString(STR_WORKBENCH_COLOR_THEME_SAVE_FAILED).c_str());
		return false;
	}
	catch (...) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS,
			LocalizedWorkbenchString(STR_WORKBENCH_COLOR_THEME_APPLY_FAILED).c_str());
		return false;
	}
}

std::wstring CEditWnd::ConfiguredColorThemeLabel() const
{
	if (!m_colorThemeRegistry) return {};
	const auto fallbackMode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark : theme::ThemeMode::Light;
	try {
		if (m_workbenchRuntime != nullptr) {
			const auto target = BuildWorkbenchConfigurationTarget();
			const auto lookup = m_workbenchRuntime->Configuration().GetValue("workbench.colorTheme", target);
			if (lookup.value) {
				if (const auto* selected = std::get_if<std::wstring>(&lookup.value->Value());
					selected != nullptr && !selected->empty()) {
					const auto loaded = m_colorThemeRegistry->Load(*selected);
					if (loaded.Succeeded()) return loaded.theme->info.label;
				}
			}
		}
		const auto fallback = m_colorThemeRegistry->Load(
			theme::CColorThemeRegistry::BuiltinThemeId(fallbackMode));
		return fallback.Succeeded() ? fallback.theme->info.label : std::wstring{};
	}
	catch (...) {
		return {};
	}
}

bool CEditWnd::ShowColorThemePicker()
{
	if (!m_colorThemeRegistry || !GetHwnd()) return false;
	const auto themes = m_colorThemeRegistry->Themes();
	if (themes.empty()) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS,
			LocalizedWorkbenchString(STR_WORKBENCH_COLOR_THEME_NONE).c_str());
		return false;
	}
	if (!EnsureQuickInputOverlay()) return false;

	const auto makeItems = [](const std::vector<theme::ColorThemeInfo>& source) {
		std::vector<workbench::quickinput::CommandPaletteItem> items;
		items.reserve(source.size());
		for (const auto& colorTheme : source) {
			items.push_back({
				.id = colorTheme.label,
				.label = colorTheme.label,
				.enabled = true,
			});
		}
		return items;
	};
	const auto lowercase = [](std::wstring_view value) {
		std::wstring result(value);
		for (auto& character : result) {
			character = static_cast<wchar_t>(std::towlower(character));
		}
		return result;
	};
	m_commandPaletteOverlay->SetStringsCallback([] {
		return workbench::quickinput::QuickInputStrings {
			.placeholder = LocalizedWorkbenchString(STR_WORKBENCH_COLOR_THEME_PICKER_PLACEHOLDER),
			.noResults = LocalizedWorkbenchString(STR_WORKBENCH_COMMAND_PALETTE_NO_RESULTS),
		};
	});
	m_commandPaletteOverlay->SetSearchCallback([themes, makeItems, lowercase](std::wstring_view query) {
		if (query.empty()) return makeItems(themes);
		const auto loweredQuery = lowercase(query);
		std::vector<theme::ColorThemeInfo> filtered;
		filtered.reserve(themes.size());
		for (const auto& colorTheme : themes) {
			if (lowercase(colorTheme.label).find(loweredQuery) != std::wstring::npos
				|| lowercase(colorTheme.id).find(loweredQuery) != std::wstring::npos) {
				filtered.push_back(colorTheme);
			}
		}
		return makeItems(filtered);
	});
	m_commandPaletteOverlay->SetSelectionCallback([this](std::wstring themeLabel) {
		(void)ApplyWorkbenchTheme(themeLabel);
	});
	m_commandPaletteOverlay->SetAcceptCallback([this](std::wstring themeLabel) {
		// VS Code persists the display label for workbench.colorTheme. The registry
		// also accepts the stable id, so settings authored by either ecosystem work.
		if (PersistColorThemeSelection(themeLabel)) {
			// The visible commit must not depend on an advisory configuration
			// notification. This also covers accept without a preceding preview.
			(void)ApplyWorkbenchTheme(themeLabel);
		} else {
			// A failed save owns no durable new theme. Restore the committed setting
			// so a preview cannot become an accidental terminal state.
			(void)ApplyWorkbenchTheme();
		}
	});
	m_commandPaletteOverlay->SetCancelCallback([this] {
		(void)ApplyWorkbenchTheme();
	});
	return m_commandPaletteOverlay->Show(makeItems(themes), ConfiguredColorThemeLabel());
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteOpenWorkspaceFolderCommand()
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	switch (OpenWorkspaceFolder()) {
	case EOpenWorkspaceFolderResult::Succeeded:
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	case EOpenWorkspaceFolderResult::Cancelled:
	case EOpenWorkspaceFolderResult::InvalidSelection:
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "folder selection was cancelled or invalid" };
	case EOpenWorkspaceFolderResult::PickerFailed:
		return { EWorkbenchCommandExecutionStatus::Failed, "folder picker failed" };
	case EOpenWorkspaceFolderResult::WorkspaceContextFailed:
		return { EWorkbenchCommandExecutionStatus::Failed, "workspace context could not accept the selected folder" };
	case EOpenWorkspaceFolderResult::ExplorerProjectionFailed:
		return { EWorkbenchCommandExecutionStatus::Failed, "selected folder could not be projected into the Explorer Part" };
	}
	return { EWorkbenchCommandExecutionStatus::Failed, "folder command returned an invalid terminal status" };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitBranchCommand(EGitBranchCommand command)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}

	// Quick Input is non-modal, so the session gate survives after this command
	// returns and is invalidated before the window's workbench is torn down.
	auto session = std::make_shared<std::atomic_bool>(true);
	if (m_gitBranchCommandSession) m_gitBranchCommandSession->store(false);
	m_gitBranchCommandSession = session;

	workbench::scm::GitBranchCommandContext context;
	context.text = [](std::string_view key, std::wstring_view argument) {
		return ResolveLocalizedScmTextKey(key, argument);
	};
	context.run = [root, session, sink = MakeGitOutputSink(m_outputService)]
		(const std::vector<std::wstring>& arguments) {
		if (!session->load()) return workbench::scm::GitExecutionResult{};
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = root;
		request.arguments = arguments;
		return workbench::scm::RunGitLogged(request, nullptr, sink);
	};
	context.message = [this, session](std::wstring_view message) {
		if (!session->load() || GetHwnd() == nullptr) return;
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, std::wstring(message).c_str());
	};
	context.quickPickAsync = [this, session](
		const std::vector<workbench::scm::GitCheckoutItem>& initialItems,
		std::wstring_view placeholder,
		std::function<std::vector<workbench::scm::GitCheckoutItem>(std::wstring_view)> search,
		std::function<void(std::optional<workbench::scm::GitCheckoutItem>)> completion) {
		if (!session->load() || GetHwnd() == nullptr || !EnsureQuickInputOverlay()) {
			if (completion) completion(std::nullopt);
			return;
		}
		struct QuickPickState {
			std::vector<workbench::scm::GitCheckoutItem> visible;
			std::function<std::vector<workbench::scm::GitCheckoutItem>(std::wstring_view)> search;
		};
		auto state = std::make_shared<QuickPickState>();
		state->visible = initialItems;
		state->search = std::move(search);
		const auto lowercase = [](std::wstring_view value) {
			std::wstring result(value);
			for (auto& character : result) character = static_cast<wchar_t>(std::towlower(character));
			return result;
		};
		const auto itemId = [](const workbench::scm::GitCheckoutItem& item) {
			const auto kind = std::to_wstring(static_cast<int>(item.kind));
			return L"git-quickpick:" + kind + L":" + item.refName + L":" + item.label;
		};
		const auto convert = [state, itemId](std::vector<workbench::scm::GitCheckoutItem> items) {
			state->visible = std::move(items);
			std::vector<workbench::quickinput::CommandPaletteItem> converted;
			converted.reserve(state->visible.size());
			for (const auto& item : state->visible) {
				const bool separator = item.kind == workbench::scm::EGitCheckoutItemKind::Separator;
				converted.push_back({
					.id = itemId(item),
					.label = item.label,
					.description = item.description,
					.detail = {},
					.enabled = !separator,
					.separator = separator,
				});
			}
			return converted;
		};
		m_commandPaletteOverlay->SetStringsCallback([placeholder = std::wstring(placeholder)] {
			return workbench::quickinput::QuickInputStrings {
				.placeholder = placeholder,
				.noResults = LocalizedWorkbenchString(STR_WORKBENCH_COMMAND_PALETTE_NO_RESULTS),
			};
		});
		m_commandPaletteOverlay->SetSearchCallback([state, convert, lowercase](std::wstring_view query) {
			std::vector<workbench::scm::GitCheckoutItem> rows = state->search
				? state->search(query) : state->visible;
			if (!query.empty()) {
				const auto lowered = lowercase(query);
				std::vector<workbench::scm::GitCheckoutItem> filtered;
				std::optional<workbench::scm::GitCheckoutItem> pendingSeparator;
				for (const auto& item : rows) {
					if (item.kind == workbench::scm::EGitCheckoutItemKind::Separator) {
						pendingSeparator = item;
						continue;
					}
					const auto haystack = lowercase(item.label + L" " + item.description);
					if (haystack.find(lowered) == std::wstring::npos) continue;
					if (pendingSeparator) {
						filtered.push_back(std::move(*pendingSeparator));
						pendingSeparator.reset();
					}
					filtered.push_back(item);
				}
				rows = std::move(filtered);
			}
			return convert(std::move(rows));
		});
		m_commandPaletteOverlay->SetSelectionCallback({});
		m_commandPaletteOverlay->SetAcceptCallback([state, itemId, completion, session](std::wstring id) {
			if (!session->load()) return;
			for (const auto& item : state->visible) {
				if (itemId(item) == id && item.kind != workbench::scm::EGitCheckoutItemKind::Separator) {
					if (completion) completion(item);
					return;
				}
			}
			if (completion) completion(std::nullopt);
		});
		m_commandPaletteOverlay->SetCancelCallback([completion, session] {
			if (!session->load()) return;
			if (completion) completion(std::nullopt);
		});
		if (!m_commandPaletteOverlay->Show(convert(initialItems))) {
			if (completion) completion(std::nullopt);
		}
	};
	context.inputBoxAsync = [this, session](std::wstring_view prompt, std::wstring_view placeholder,
		std::wstring_view value, std::function<void(std::optional<std::wstring>)> completion) {
		if (!session->load() || GetHwnd() == nullptr || !EnsureQuickInputOverlay()) {
			if (completion) completion(std::nullopt);
			return;
		}
		m_commandPaletteOverlay->SetStringsCallback({});
		m_commandPaletteOverlay->SetSearchCallback({});
		m_commandPaletteOverlay->SetSelectionCallback({});
		m_commandPaletteOverlay->SetAcceptCallback([completion, session](std::wstring typed) {
			if (!session->load()) return;
			if (completion) completion(std::move(typed));
		});
		m_commandPaletteOverlay->SetCancelCallback([completion, session] {
			if (!session->load()) return;
			if (completion) completion(std::nullopt);
		});
		if (!m_commandPaletteOverlay->ShowInput(prompt, placeholder, value)) {
			if (completion) completion(std::nullopt);
		}
	};
	const auto completion = [this, session](workbench::scm::GitBranchCommandResult result) {
	if (!session->exchange(false) || GetHwnd() == nullptr) return;
	if (m_commandPaletteOverlay) {
		m_commandPaletteOverlay->SetStringsCallback({});
		m_commandPaletteOverlay->SetSearchCallback({});
		m_commandPaletteOverlay->SetSelectionCallback({});
		m_commandPaletteOverlay->SetAcceptCallback({});
		m_commandPaletteOverlay->SetCancelCallback({});
	}
	m_gitBranchCommandSession.reset();
		switch (result.status) {
		case workbench::scm::EGitBranchCommandStatus::Succeeded:
			// HEAD moved, so every published SCM fact is stale. Refreshing here is
			// what upstream's repository status refresh does after an operation.
			if (m_scmTool != nullptr) m_scmTool->Refresh();
			break;
		case workbench::scm::EGitBranchCommandStatus::Cancelled:
			break;
		case workbench::scm::EGitBranchCommandStatus::Failed:
		default:
			if (!result.message.empty()) {
				m_cStatusBar.SetStatusText(0, SBT_NOBORDERS,
					std::wstring(result.message).c_str());
			}
			break;
		}
	};
	switch (command) {
	case EGitBranchCommand::Checkout:
		workbench::scm::RunGitCheckoutAsync(context, false, completion);
		break;
	case EGitBranchCommand::CheckoutDetached:
		workbench::scm::RunGitCheckoutAsync(context, true, completion);
		break;
	case EGitBranchCommand::Branch:
		workbench::scm::RunGitCreateBranchAsync(context, false, completion);
		break;
	case EGitBranchCommand::BranchFrom:
		workbench::scm::RunGitCreateBranchAsync(context, true, completion);
		break;
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitInitCommand(
	std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;
	if (GetHwnd() == nullptr) return { EWorkbenchCommandExecutionStatus::NotApplicable, "window is unavailable" };

	workbench::scm::GitInitCommandContext context;
	context.text = [](workbench::scm::EScmTextKey key, std::wstring_view argument) {
		return ResolveLocalizedScmText(key, argument);
	};
	if (m_workbenchRuntime != nullptr) {
		const auto snapshot = m_workbenchRuntime->WorkspaceContext().Snapshot();
		for (const auto& folder : snapshot.folders) {
			const auto path = folder.uri.ToWindowsPath();
			if (!path.value || path.value->empty()) continue;
			context.openFolders.push_back({ folder.displayName, *path.value });
		}
	}
	context.homeDirectory = [] {
		std::array<wchar_t, MAX_PATH> value{};
		const DWORD length = ::GetEnvironmentVariableW(L"USERPROFILE", value.data(), static_cast<DWORD>(value.size()));
		return length == 0 || length >= value.size() ? std::wstring{} : std::wstring(value.data(), length);
	}();
	context.run = [sink = MakeGitOutputSink(m_outputService)]
		(std::wstring_view workingDirectory, const std::vector<std::wstring>& arguments) {
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = std::wstring(workingDirectory);
		request.arguments = arguments;
		return workbench::scm::RunGitLogged(request, nullptr, sink);
	};
	context.folderPick = [this](const auto& items, std::wstring_view placeholder) -> std::optional<std::size_t> {
		SQuickInputRequest request;
		request.kind = EQuickInputKind::QuickPick;
		request.title = LocalizedWorkbenchString(STR_WORKBENCH_REPOSITORY_INIT_QUICK_PICK_TITLE);
		request.placeholder = placeholder;
		for (std::size_t i = 0; i < items.size(); ++i) {
			request.items.push_back({ i, items[i].label, items[i].description, items[i].path });
		}
		CQuickInputDialog dialog(request);
		const auto completion = dialog.DoModal(GetHwnd());
		if (completion.state != EQuickInputState::Accepted || completion.selectedIndices.size() != 1) return std::nullopt;
		return completion.selectedIndices.front();
	};
	context.browseForFolder = [this](std::wstring_view label, std::wstring_view initial) -> std::optional<std::wstring> {
		std::array<WCHAR, 32768> selected{};
		const auto result = SelectDirWithResult(GetHwnd(), std::wstring(label), std::filesystem::path(initial), selected);
		if (result != ESelectDirResult::Succeeded) return std::nullopt;
		const auto absolute = MakeAbsolutePath(selected.data());
		if (absolute.empty()) return std::nullopt;
		const DWORD attributes = ::GetFileAttributesW(absolute.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return std::nullopt;
		return absolute;
	};
	context.confirm = [this](const workbench::scm::GitPrompt& prompt) -> std::optional<std::size_t> {
		TASKDIALOGCONFIG config{};
		config.cbSize = sizeof(config); config.hwndParent = GetHwnd();
		config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
		config.pszWindowTitle = L"Sakura Editor NEXT";
		config.pszMainIcon = prompt.warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
		config.pszMainInstruction = prompt.message.c_str();
		config.pszContent = prompt.detail.empty() ? nullptr : prompt.detail.c_str();
		config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
		std::vector<TASKDIALOG_BUTTON> buttons;
		for (std::size_t i = 0; i < prompt.choices.size(); ++i) buttons.push_back({ static_cast<int>(1000 + i), prompt.choices[i].c_str() });
		config.cButtons = static_cast<UINT>(buttons.size()); config.pButtons = buttons.data();
		int selected = 0;
		if (buttons.empty() || FAILED(::TaskDialogIndirect(&config, &selected, nullptr, nullptr))) return std::nullopt;
		if (selected < 1000 || static_cast<std::size_t>(selected - 1000) >= prompt.choices.size()) return std::nullopt;
		return static_cast<std::size_t>(selected - 1000);
	};
	context.message = [this](std::wstring_view message) { m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, std::wstring(message).c_str()); };

	const auto result = workbench::scm::RunGitInit(
		context, workbench::scm::ParseGitInitSkipFolderPromptArgument(argumentsJson));
	if (result.status == workbench::scm::EGitInitCommandStatus::Cancelled)
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "the user dismissed repository initialization" };
	if (!result.Succeeded()) return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(result.message) };
	if (result.postAction == workbench::scm::EGitInitPostAction::OfferToOpen && context.confirm) {
		const auto choice = context.confirm(workbench::scm::BuildGitInitOpenPrompt(context.text));
		if (choice && *choice == 0) {
			if (ApplyFolderWorkspace(result.repositoryPath, true) != EOpenWorkspaceFolderResult::Succeeded)
				return { EWorkbenchCommandExecutionStatus::Failed, "initialized repository could not be opened" };
		}
	}
	if (m_scmTool != nullptr) m_scmTool->Refresh();
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitCloneCommand(bool recurseSubmodules)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;
	if (GetHwnd() == nullptr) return { EWorkbenchCommandExecutionStatus::NotApplicable, "window is unavailable" };
	workbench::scm::GitCloneCommandContext context;
	context.text = [](workbench::scm::EScmTextKey key, std::wstring_view argument) {
		return ResolveLocalizedScmText(key, argument);
	};
	context.homeDirectory = [] {
		std::array<wchar_t, MAX_PATH> value{};
		const DWORD length = ::GetEnvironmentVariableW(L"USERPROFILE", value.data(), static_cast<DWORD>(value.size()));
		return length == 0 || length >= value.size() ? std::wstring{} : std::wstring(value.data(), length);
	}();
	context.promptForUrl = [this](std::wstring_view prompt, std::wstring_view placeholder, std::wstring_view value) -> std::optional<std::wstring> {
		SQuickInputRequest request; request.kind = EQuickInputKind::InputBox; request.title = std::wstring(prompt); request.placeholder = placeholder; request.value = value;
		CQuickInputDialog dialog(request); const auto completion = dialog.DoModal(GetHwnd());
		if (completion.state != EQuickInputState::Accepted) return std::nullopt;
		return completion.value.value_or(std::wstring{});
	};
	context.browseForParentDirectory = [this](std::wstring_view label, std::wstring_view initial) -> std::optional<std::wstring> {
		std::array<WCHAR, 32768> selected{};
		if (SelectDirWithResult(GetHwnd(), std::wstring(label), std::filesystem::path(initial), selected) != ESelectDirResult::Succeeded) return std::nullopt;
		return MakeAbsolutePath(selected.data());
	};
	context.pathState = [](std::wstring_view path) {
		const DWORD attributes = ::GetFileAttributesW(std::wstring(path).c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return workbench::scm::EGitPathState::Absent;
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return workbench::scm::EGitPathState::NonEmpty;
		WIN32_FIND_DATAW data{}; const auto pattern = std::filesystem::path(path) / L"*";
		const HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) return workbench::scm::EGitPathState::EmptyDirectory;
		bool empty = true;
		do { if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) { empty = false; break; } } while (::FindNextFileW(find, &data));
		::FindClose(find); return empty ? workbench::scm::EGitPathState::EmptyDirectory : workbench::scm::EGitPathState::NonEmpty;
	};
	context.message = [this](std::wstring_view message) { m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, std::wstring(message).c_str()); };
	const auto request = workbench::scm::RunGitClonePrepare(context);
	if (!request) return { EWorkbenchCommandExecutionStatus::NotApplicable, "clone was cancelled or the destination is unavailable" };
	const auto cloneWorkingDirectory = std::filesystem::path(request->destinationPath).parent_path().wstring();
	const auto raw = workbench::scm::RunGitCloneExecute(*request, { recurseSubmodules },
		[cloneWorkingDirectory, sink = MakeGitOutputSink(m_outputService)]
			(const std::vector<std::wstring>& arguments, HANDLE stop) {
			workbench::scm::GitExecutionRequest r;
			r.workingDirectory = cloneWorkingDirectory;
			r.arguments = arguments;
			return workbench::scm::RunGitLogged(r, stop, sink);
		}, nullptr);
	const auto result = workbench::scm::RunGitCloneComplete(*request, raw);
	if (!result.Succeeded()) return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(result.message) };
	context.message(LocalizedWorkbenchString(STR_WORKBENCH_GIT_CLONE_SUCCESS));
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}


namespace {

//! Decodes one Explorer command payload to the Windows path it names. Every
//! failure — malformed payload, unparseable URI, non-file scheme — collapses to
//! `nullopt`; the executors report it as one malformed-arguments terminal.
std::optional<std::wstring> ResolveExplorerArgumentPath(std::string_view argumentsJson)
{
	const auto parsed = workbench::commands::ParseExplorerResourceArguments(argumentsJson);
	if (!parsed) {
		return std::nullopt;
	}
	const auto uri = platform::uri::Uri::Parse(parsed->resourceUri);
	if (!uri.value) {
		return std::nullopt;
	}
	auto windowsPath = uri.value->ToWindowsPath();
	if (!windowsPath.value || windowsPath.value->empty()) {
		return std::nullopt;
	}
	return std::move(*windowsPath.value);
}

//! Last path segment with trailing separators trimmed; the delete confirmation
//! names the resource the way upstream's dialog does, not by its full path.
std::wstring ExplorerResourceDisplayName(std::wstring_view path)
{
	while (!path.empty() && workbench::explorer::IsExplorerPathSeparator(path.back())) {
		path.remove_suffix(1);
	}
	const auto separator = path.find_last_of(L"\\/");
	if (separator != std::wstring_view::npos) {
		path.remove_prefix(separator + 1);
	}
	return std::wstring(path);
}

//! Shows one Explorer delete confirmation in the shape upstream's dialog has:
//! instruction, detail, a single accented primary button, and Cancel. Returns
//! true only on an explicit primary-button accept; a failed dialog and a
//! decline take the same fail-closed path.
bool ShowExplorerDeleteConfirmationDialog(
	HWND owner, const workbench::explorer::ExplorerDeleteConfirmation& confirmation)
{
	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof(config);
	config.hwndParent = owner;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW
		| TDF_SIZE_TO_CONTENT;
	config.pszWindowTitle = L"Sakura Editor NEXT";
	if (confirmation.isWarning) {
		config.pszMainIcon = TD_WARNING_ICON;
	}
	config.pszMainInstruction = confirmation.instruction.c_str();
	config.pszContent = confirmation.detail.empty() ? nullptr : confirmation.detail.c_str();
	config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
	constexpr int kPrimaryButtonId = 1000;
	TASKDIALOG_BUTTON button{ kPrimaryButtonId, confirmation.primaryButton.c_str() };
	config.cButtons = 1;
	config.pButtons = &button;
	int selected = 0;
	if (FAILED(::TaskDialogIndirect(&config, &selected, nullptr, nullptr))
		|| selected != kPrimaryButtonId) {
		return false;
	}
	return true;
}

//! Surfaces a failed Explorer file operation. Recorded divergence in
//! `workbench/explorer/CLAUDE.md`: upstream reports these through the
//! notification center; this product's notification surface cannot yet carry
//! them, so the report is a modal error dialog.
void ShowExplorerOperationError(HWND owner, std::wstring instruction, const std::wstring& detail)
{
	TASKDIALOGCONFIG config{};
	config.cbSize = sizeof(config);
	config.hwndParent = owner;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW
		| TDF_SIZE_TO_CONTENT;
	config.pszWindowTitle = L"Sakura Editor NEXT";
	config.pszMainIcon = TD_ERROR_ICON;
	config.pszMainInstruction = instruction.c_str();
	config.pszContent = detail.empty() ? nullptr : detail.c_str();
	config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
	(void)::TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

} // namespace

platform::filesystem::IFileService* CEditWnd::EnsureExplorerFileService()
{
	if (!m_explorerFileService) {
		auto files = platform::filesystem::CreateWin32FileService();
		if (!files.Succeeded() || !files.value) {
			return nullptr;
		}
		m_explorerFileService = std::move(*files.value);
	}
	return m_explorerFileService.get();
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteExplorerNewEntry(
	std::string_view argumentsJson, bool directory)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto path = ResolveExplorerArgumentPath(argumentsJson);
	if (!path) {
		return { EWorkbenchCommandExecutionStatus::Failed, "explorer command arguments are malformed" };
	}
	if (m_explorerTool == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no explorer view exists in this window" };
	}
	if (!m_explorerTool->BeginCreateEntry(*path, directory)) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable,
			"the resource cannot host a new entry" };
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteExplorerRenameFile(
	std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto path = ResolveExplorerArgumentPath(argumentsJson);
	if (!path) {
		return { EWorkbenchCommandExecutionStatus::Failed, "explorer command arguments are malformed" };
	}
	if (m_explorerTool == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no explorer view exists in this window" };
	}
	if (!m_explorerTool->BeginRenameEntry(*path)) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "the resource is not renameable" };
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteExplorerDelete(
	std::string_view argumentsJson, bool useTrash)
{
	using platform::filesystem::EFileResultStatus;
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto path = ResolveExplorerArgumentPath(argumentsJson);
	if (!path) {
		return { EWorkbenchCommandExecutionStatus::Failed, "explorer command arguments are malformed" };
	}
	auto* const files = EnsureExplorerFileService();
	if (files == nullptr) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the file service is unavailable" };
	}
	const auto stat = files->Stat(platform::uri::Uri::FromWindowsPath(*path));
	if (!stat.Succeeded() || !stat.value) {
		if (stat.status == EFileResultStatus::NotFound) {
			return { EWorkbenchCommandExecutionStatus::NotApplicable, "the resource does not exist" };
		}
		return { EWorkbenchCommandExecutionStatus::Failed, "the resource cannot be inspected" };
	}
	const bool isDirectory = stat.value->type == platform::filesystem::EFileEntryType::Directory;

	const auto confirmation = workbench::explorer::BuildExplorerDeleteConfirmation(
		ExplorerResourceDisplayName(*path), isDirectory, useTrash);
	if (!ShowExplorerDeleteConfirmationDialog(GetHwnd(), confirmation)) {
		// Upstream resolves a declined confirmation without error; cancelling
		// the dialog is the command completing, not the command failing.
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	}

	auto deletion = files->Delete(platform::uri::Uri::FromWindowsPath(*path),
		{ .recursive = true, .useTrash = useTrash });
	if (!deletion.Succeeded() && useTrash) {
		// Upstream's Recycle Bin fallback: offer the permanent deletion the
		// trash could not perform, behind its own warning confirmation.
		if (!ShowExplorerDeleteConfirmationDialog(GetHwnd(),
				workbench::explorer::BuildExplorerTrashFailedConfirmation())) {
			return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
		}
		deletion = files->Delete(platform::uri::Uri::FromWindowsPath(*path),
			{ .recursive = true, .useTrash = false });
	}
	if (!deletion.Succeeded()) {
		ShowExplorerOperationError(GetHwnd(), LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_DELETE_FAILED),
			deletion.diagnostic);
		return { EWorkbenchCommandExecutionStatus::Failed, "the resource could not be deleted" };
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitCopyCommitCommand(
	std::string_view argumentsJson, bool message)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto id = workbench::scm::ParseGitHistoryItemArguments(argumentsJson);
	if (!id) {
		return { EWorkbenchCommandExecutionStatus::Failed, "git history item arguments are malformed" };
	}
	if (m_scmTool == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable,
			"no source control view exists in this window" };
	}
	// A commit the Graph no longer holds cannot have its text copied from a
	// history that no longer describes it; refreshing behind an open menu is
	// exactly how that happens.
	const auto item = m_scmTool->HistoryItem(*id);
	if (!item) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable,
			"the graph no longer holds that commit" };
	}
	const std::wstring& text = message ? item->message : item->id;
	CClipboard clipboard(GetHwnd());
	if (!clipboard) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the clipboard could not be opened" };
	}
	clipboard.Empty();
	if (!clipboard.SetText(text.c_str(), text.size(), false, false)) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the clipboard text could not be written" };
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteExplorerCopyPath(
	std::string_view argumentsJson, bool relative)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto path = ResolveExplorerArgumentPath(argumentsJson);
	if (!path) {
		return { EWorkbenchCommandExecutionStatus::Failed, "explorer command arguments are malformed" };
	}
	const std::wstring text = relative
		? workbench::explorer::BuildExplorerRelativePathLabel(GetSemanticWorkspaceRoot(), *path)
		: *path;
	CClipboard clipboard(GetHwnd());
	if (!clipboard) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the clipboard could not be opened" };
	}
	clipboard.Empty();
	if (!clipboard.SetText(text.c_str(), text.size(), false, false)) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the clipboard text could not be written" };
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteExplorerRevealInOS(
	std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto path = ResolveExplorerArgumentPath(argumentsJson);
	if (!path) {
		return { EWorkbenchCommandExecutionStatus::Failed, "explorer command arguments are malformed" };
	}
	PIDLIST_ABSOLUTE item = ::ILCreateFromPathW(path->c_str());
	if (item == nullptr) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the resource could not be located" };
	}
	const HRESULT revealed = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
	::ILFree(item);
	if (FAILED(revealed)) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the resource could not be revealed" };
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

void CEditWnd::CommitExplorerRename(std::wstring_view path, std::wstring_view newName)
{
	auto* const files = EnsureExplorerFileService();
	if (files == nullptr) {
		ShowExplorerOperationError(GetHwnd(), LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_RENAME_FAILED), {});
		return;
	}
	std::wstring source(path);
	while (!source.empty() && workbench::explorer::IsExplorerPathSeparator(source.back())) {
		source.pop_back();
	}
	const auto separator = source.find_last_of(L"\\/");
	if (separator == std::wstring::npos) {
		ShowExplorerOperationError(GetHwnd(), LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_RENAME_FAILED), {});
		return;
	}
	std::wstring target = source.substr(0, separator + 1);
	target += newName;
	const auto renamed = files->Rename(platform::uri::Uri::FromWindowsPath(source),
		platform::uri::Uri::FromWindowsPath(target), { .overwrite = false });
	if (!renamed.Succeeded()) {
		ShowExplorerOperationError(GetHwnd(), LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_RENAME_FAILED),
			renamed.diagnostic);
	}
}

void CEditWnd::CommitExplorerCreate(
	std::wstring_view parentDirectory, std::wstring_view name, bool directory)
{
	auto* const files = EnsureExplorerFileService();
	if (files == nullptr) {
		ShowExplorerOperationError(GetHwnd(), directory
			? LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_CREATE_FOLDER_FAILED)
			: LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_CREATE_FILE_FAILED), {});
		return;
	}
	std::wstring target(parentDirectory);
	if (!target.empty() && !workbench::explorer::IsExplorerPathSeparator(target.back())) {
		target += L'\\';
	}
	target += name;
	if (directory) {
		const auto made = files->MakeDirectory(platform::uri::Uri::FromWindowsPath(target));
		if (!made.Succeeded()) {
			ShowExplorerOperationError(GetHwnd(), LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_CREATE_FOLDER_FAILED),
				made.diagnostic);
		}
		return;
	}
	// Create-if-missing keeps an existing file's content untouched: racing an
	// existing name is a conflict, never a truncation.
	const auto created = files->ConditionalAtomicReplace(
		platform::uri::Uri::FromWindowsPath(target), platform::filesystem::FileBytes{},
		platform::filesystem::FileConditionalReplaceOptions::ForMissing());
	if (!created.Succeeded()) {
		ShowExplorerOperationError(GetHwnd(), LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_CREATE_FILE_FAILED),
			created.diagnostic);
		return;
	}
	// Upstream opens a newly created file pinned; a folder only appears in the tree.
	OpenExplorerFile(target, workbench::explorer::ExplorerFileActivationKind::Pinned);
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitStageCommand(
	EGitStageCommand command, std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}

	workbench::scm::GitStageCommandContext context;
	context.text = [](std::string_view key, std::wstring_view argument) {
		return ResolveLocalizedScmTextKey(key, argument);
	};
	context.repositoryRoot = root;
	context.run = [&root, sink = MakeGitOutputSink(m_outputService)]
		(const std::vector<std::wstring>& arguments) {
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = root;
		request.arguments = arguments;
		return workbench::scm::RunGitLogged(request, nullptr, sink);
	};
	context.message = [this](std::wstring_view message) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, std::wstring(message).c_str());
	};
	// Upstream's discard confirmation is `window.showWarningMessage(..., { modal:
	// true }, ...buttons)`, whose buttons are the two different discard sets. A
	// task dialog is the native modal that can carry that many labelled choices,
	// and dismissing it is a cancel, never the primary action.
	context.confirm = [this](const workbench::scm::GitDiscardPrompt& prompt) -> std::optional<std::size_t> {
		if (prompt.choices.empty()) return std::nullopt;
		std::vector<TASKDIALOG_BUTTON> buttons;
		buttons.reserve(prompt.choices.size());
		for (std::size_t index = 0; index < prompt.choices.size(); ++index) {
			buttons.push_back({ 1000 + static_cast<int>(index), prompt.choices[index].label.c_str() });
		}
		TASKDIALOGCONFIG config{};
		config.cbSize = sizeof(config);
		config.hwndParent = GetHwnd();
		config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
		config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
		config.pszWindowTitle = L"Sakura Editor NEXT";
		config.pszMainIcon = TD_WARNING_ICON;
		config.pszMainInstruction = prompt.message.c_str();
		config.pszContent = prompt.detail.empty() ? nullptr : prompt.detail.c_str();
		config.cButtons = static_cast<UINT>(buttons.size());
		config.pButtons = buttons.data();
		int selected = 0;
		if (FAILED(::TaskDialogIndirect(&config, &selected, nullptr, nullptr))) return std::nullopt;
		if (selected < 1000) return std::nullopt;
		const auto index = static_cast<std::size_t>(selected - 1000);
		if (index >= prompt.choices.size()) return std::nullopt;
		return index;
	};
	// `git.discardUntrackedChangesToTrash` defaults to true, so an untracked file
	// goes to the Recycle Bin rather than being erased. The returned list is the
	// paths the shell refused, so the permanent fallback re-deletes only those.
	context.trash = [](const std::vector<std::wstring>& absolutePaths) {
		std::vector<std::wstring> failed;
		for (const auto& path : absolutePaths) {
			// SHFileOperationW wants a double-null-terminated list; one call per
			// path is what makes a single refusal attributable to that path.
			std::wstring buffer = path;
			buffer.push_back(L'\0');
			buffer.push_back(L'\0');
			SHFILEOPSTRUCTW operation{};
			operation.wFunc = FO_DELETE;
			operation.pFrom = buffer.c_str();
			operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
			const int status = ::SHFileOperationW(&operation);
			if (status != 0 || operation.fAnyOperationsAborted) failed.push_back(path);
		}
		return failed;
	};

	std::vector<workbench::scm::GitStageResource> resources;
	switch (command) {
	case EGitStageCommand::Stage:
	case EGitStageCommand::Unstage:
	case EGitStageCommand::Clean: {
		// The invocation names its own rows. A payload that does not parse is a
		// hard failure rather than an empty selection, because an empty selection
		// would silently look like "nothing to do".
		auto parsed = workbench::scm::ParseGitStageArguments(argumentsJson);
		if (!parsed) return { EWorkbenchCommandExecutionStatus::Failed, "malformed command arguments" };
		resources = std::move(*parsed);
		break;
	}
	case EGitStageCommand::StageAll:
	case EGitStageCommand::CleanAll:
		// Upstream's group-scoped handlers read the repository's own resource
		// groups. Ours derives them from the state the publication was built from,
		// through the same classification, so this operand list is exactly the set
		// of rows the view is showing.
		if (m_scmTool == nullptr) {
			return { EWorkbenchCommandExecutionStatus::NotApplicable, "no source control view is open" };
		}
		resources = workbench::scm::CollectGitStageResources(m_scmTool->State());
		break;
	case EGitStageCommand::UnstageAll:
		// `revert([])`: a whole-index reset, not a listing of every staged path.
		break;
	}

	workbench::scm::GitStageCommandResult result{};
	switch (command) {
	case EGitStageCommand::Stage:
	case EGitStageCommand::StageAll:
		result = workbench::scm::RunGitStage(context, resources);
		break;
	case EGitStageCommand::Unstage:
		result = workbench::scm::RunGitUnstage(context, resources);
		break;
	case EGitStageCommand::UnstageAll:
		result = workbench::scm::RunGitUnstageAll(context);
		break;
	case EGitStageCommand::Clean:
	case EGitStageCommand::CleanAll:
		result = workbench::scm::RunGitDiscard(context, resources);
		break;
	}

	switch (result.status) {
	case workbench::scm::EGitStageCommandStatus::Succeeded:
		// The index or the worktree moved, so every published SCM fact is stale.
		if (m_scmTool != nullptr) m_scmTool->Refresh();
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	case workbench::scm::EGitStageCommandStatus::NotApplicable:
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no resource of this command's groups was named" };
	case workbench::scm::EGitStageCommandStatus::Cancelled:
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "the user dismissed the confirmation" };
	case workbench::scm::EGitStageCommandStatus::UnsupportedMergeConflict:
		return { EWorkbenchCommandExecutionStatus::Unsupported, wcstou8s(result.message) };
	case workbench::scm::EGitStageCommandStatus::Failed:
	default:
		break;
	}
	return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(result.message) };
}

namespace {

//!
//! @brief The bound on one side of a comparison, in bytes.
//!
//! `GitExecutionRequest::maximumOutputBytes`' own default, applied to the
//! working-tree side as well. One side read through git and the other read from
//! disk must refuse the same file for the same reason; two different bounds
//! would make "too large to compare" depend on which half of the comparison the
//! file happened to occupy.
//!
constexpr std::size_t kMaximumDiffSideBytes = 4u * 1024u * 1024u;

//! The last path segment, over the forward slashes a repository-relative path uses.
[[nodiscard]] std::wstring_view DiffPathBasename(std::wstring_view path)
{
	const auto separator = path.find_last_of(L"/\\");
	return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

//!
//! @brief The column caption one side renders under.
//!
//! Upstream labels a diff editor's sides from its URIs, where the repository
//! side reads `<name> (<ref>)`. The surface already carries the file name in its
//! own title, so each column names only the **area** its text came from, which
//! is the fact a user checking what they are about to commit needs.
//!
[[nodiscard]] std::wstring DiffEndpointLabel(const workbench::scm::GitDiffEndpoint& endpoint)
{
	if (endpoint.source == workbench::scm::EGitDiffSource::WorkingTree) {
		return LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_WORKING_TREE);
	}
	// An empty ref is the index, not an absent one: `git show :path`.
	return endpoint.ref.empty() ? LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_INDEX) : endpoint.ref;
}

//! Read one side's raw bytes. `failure` carries a sentence only when this fails.
[[nodiscard]] bool ReadDiffEndpointBytes(
	const workbench::scm::GitDiffEndpoint& endpoint, std::wstring_view repositoryRoot,
	std::vector<std::uint8_t>& bytes, std::wstring& failure,
	const workbench::scm::GitOutputSink& sink)
{
	bytes.clear();
	if (endpoint.source == workbench::scm::EGitDiffSource::Repository) {
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = std::wstring(repositoryRoot);
		request.arguments = workbench::scm::BuildGitShowArguments(endpoint);
		request.maximumOutputBytes = kMaximumDiffSideBytes;
		auto result = workbench::scm::RunGitLogged(request, nullptr, sink);
		if (!result.Succeeded()) {
			failure = workbench::scm::DescribeGitFailure(result);
			return false;
		}
		bytes = std::move(result.standardOutput);
		return true;
	}

	// The working-tree side is the file on disk and must never be read through
	// `git show`: it is the only side the user can edit, and git knows nothing
	// about the edit that has not been staged yet.
	const auto absolute = workbench::scm::JoinRepositoryPath(repositoryRoot, endpoint.path);
	if (absolute.empty()) {
		failure = LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_PATH_UNRESOLVED);
		return false;
	}
	const HANDLE file = ::CreateFileW(absolute.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		// A refresh is periodic, so the file can legitimately vanish between the
		// row being rendered and the row being clicked. Reporting that is honest;
		// substituting an empty side would render the whole file as deleted.
		failure = LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_FILE_OPEN_FAILED);
		return false;
	}
	LARGE_INTEGER size{};
	if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
		::CloseHandle(file);
		failure = LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_FILE_READ_FAILED);
		return false;
	}
	if (static_cast<unsigned long long>(size.QuadPart) > kMaximumDiffSideBytes) {
		::CloseHandle(file);
		failure = LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_FILE_TOO_LARGE);
		return false;
	}
	bytes.resize(static_cast<std::size_t>(size.QuadPart));
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const std::size_t remaining = bytes.size() - offset;
		const DWORD wanted = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
		DWORD read = 0;
		if (!::ReadFile(file, bytes.data() + offset, wanted, &read, nullptr) || read == 0) {
			::CloseHandle(file);
			failure = LocalizedWorkbenchString(STR_WORKBENCH_GIT_DIFF_FILE_READ_FAILED);
			return false;
		}
		offset += read;
	}
	::CloseHandle(file);
	return true;
}

//! `DecodeGitOutput` over a byte vector, without forming a view over a null pointer.
[[nodiscard]] std::wstring DecodeDiffSide(const std::vector<std::uint8_t>& bytes)
{
	if (bytes.empty()) return {};
	return workbench::scm::DecodeGitOutput(
		std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

} // namespace

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteVsCodeDiffCommand(std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}
	const auto parsed = workbench::commands::ParseApiDiffArguments(argumentsJson);
	if (!parsed) {
		return { EWorkbenchCommandExecutionStatus::Failed, "malformed command arguments" };
	}
	// A URI naming a file outside this repository is refused rather than read.
	// The only producer of these URIs is the built-in Git provider for the
	// repository this window has open, and a path that escapes that root is
	// exactly what `ResolveGitDiffEndpointUri` exists to reject.
	const auto original = workbench::scm::ResolveGitDiffEndpointUri(parsed->originalUri, root);
	const auto modified = workbench::scm::ResolveGitDiffEndpointUri(parsed->modifiedUri, root);
	if (!original || !modified) {
		return { EWorkbenchCommandExecutionStatus::Unsupported,
			"only files inside this window's repository can be compared" };
	}

	std::vector<std::uint8_t> originalBytes;
	std::vector<std::uint8_t> modifiedBytes;
	std::wstring failure;
	const auto gitOutputSink = MakeGitOutputSink(m_outputService);
	if (!ReadDiffEndpointBytes(*original, root, originalBytes, failure, gitOutputSink)
		|| !ReadDiffEndpointBytes(*modified, root, modifiedBytes, failure, gitOutputSink)) {
		return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(failure) };
	}

	SDiffSurfaceContent content;
	content.originalLabel = DiffEndpointLabel(*original);
	content.modifiedLabel = DiffEndpointLabel(*modified);
	content.title = parsed->title;
	if (content.title.empty()) {
		// Upstream derives a label from the two URIs when the caller passes no
		// title. `git.openChange` always passes one, so this is the shape an
		// extension-issued `vscode.diff` lands on.
		content.title.append(DiffPathBasename(modified->path))
			.append(L" (").append(content.originalLabel)
			.append(L" ↔ ").append(content.modifiedLabel).append(L")");
	}
	// Both sides go through one decoder. Two decoders that disagreed by a single
	// character would render identical text as a change, which is precisely the
	// lie this surface exists to prevent.
	content.originalLines = workbench::scm::SplitGitDiffLines(DecodeDiffSide(originalBytes));
	content.modifiedLines = workbench::scm::SplitGitDiffLines(DecodeDiffSide(modifiedBytes));
	const auto diff = workbench::scm::ComputeGitLineDiff(content.originalLines, content.modifiedLines);
	content.truncated = diff.hitTimeout;
	// The composition root is the only place that may translate between the SCM
	// subtree's row and the editor subtree's row; neither subtree may name the
	// other's type.
	const auto viewRows = workbench::scm::BuildGitDiffViewRows(
		static_cast<int>(content.originalLines.size()),
		static_cast<int>(content.modifiedLines.size()), diff);
	content.rows.reserve(viewRows.size());
	for (const auto& row : viewRows) {
		content.rows.push_back(
			SDiffSurfaceRow{ row.changed, row.originalLineNumber, row.modifiedLineNumber });
	}

	if (!ShowDiffSurface(std::move(content))) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable,
			"a document is open in this editor group" };
	}
	// Recorded only after the surface accepted the comparison, so the retained
	// endpoints can never name a comparison that is not on screen.
	m_diffRepositoryRoot = root;
	m_diffOriginalUri = parsed->originalUri;
	m_diffModifiedUri = parsed->modifiedUri;
	// `isInDiffEditor` just became true, and the selected-range commands are
	// gated on it. A stale snapshot would leave them listed as out of scope.
	(void)RefreshWorkbenchCommandContext();
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteVsCodeOpenCommand(std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}
	const auto parsed = workbench::commands::ParseApiOpenArguments(argumentsJson);
	if (!parsed) {
		return { EWorkbenchCommandExecutionStatus::Failed, "malformed command arguments" };
	}
	const auto resource = workbench::scm::ResolveGitDiffEndpointUri(parsed->resourceUri, root);
	if (!resource) {
		return { EWorkbenchCommandExecutionStatus::Unsupported,
			"only files inside this window's repository can be opened here" };
	}
	if (resource->source == workbench::scm::EGitDiffSource::Repository) {
		// Upstream opens a `git:` URI as a read-only document through its
		// `GitFileSystemProvider`. There is no read-only document input here, and
		// the diff surface is a *comparison*: showing one side of it with the other
		// left empty would draw a whole-file insertion git never reported.
		return { EWorkbenchCommandExecutionStatus::Unsupported,
			"opening committed or staged content needs a read-only editor, which is not implemented" };
	}
	// `TextDocumentShowOptions.override` chooses between a registered custom
	// editor and the default text editor. There are no custom editors here, so
	// both of its values already resolve to this one route; it is carried through
	// the arguments so a caller's request is not silently rewritten.
	const auto absolute = workbench::scm::JoinRepositoryPath(root, resource->path);
	if (absolute.empty()) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the named path could not be resolved" };
	}
	GetActiveView().GetCommander().Command_FILEOPEN(absolute.c_str());
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitOpenChangeCommand(std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}
	if (m_scmTool == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no source control view is open" };
	}
	const auto parsed = workbench::scm::ParseGitStageArguments(argumentsJson);
	if (!parsed) {
		return { EWorkbenchCommandExecutionStatus::Failed, "malformed command arguments" };
	}
	if (parsed->empty()) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no resource was named" };
	}

	// Upstream's handler calls `openChange()` on the resource **object**, so the
	// comparison it opens always belongs to the live row. A native menu can only
	// name a row by `(path, group)`, so the row is re-derived from the current
	// state rather than carried across from the snapshot the menu was built from.
	const auto rows = workbench::scm::CollectGitDiffRows(m_scmTool->State());
	WorkbenchCommandExecutionResult result{ EWorkbenchCommandExecutionStatus::NotApplicable, "no resource was named" };
	for (const auto& resource : *parsed) {
		const workbench::scm::GitDiffRow* found = nullptr;
		for (const auto& entry : rows) {
			if (entry.group == resource.group && entry.row.path == resource.path) {
				found = &entry.row;
				break;
			}
		}
		if (found == nullptr) {
			return { EWorkbenchCommandExecutionStatus::NotApplicable,
				"that row is no longer in the Source Control view" };
		}
		const auto input = workbench::scm::ResolveGitDiffInput(*found,
			[](std::string_view key, std::wstring_view argument) {
				return ResolveLocalizedScmTextKey(key, argument);
			});
		switch (input.kind) {
		case workbench::scm::EGitDiffCommandKind::Diff: {
			workbench::commands::ApiDiffArguments arguments;
			arguments.originalUri = workbench::scm::BuildGitDiffEndpointUri(*input.original, root);
			arguments.modifiedUri = workbench::scm::BuildGitDiffEndpointUri(*input.modified, root);
			arguments.title = input.title;
			if (arguments.originalUri.empty() || arguments.modifiedUri.empty()) {
				return { EWorkbenchCommandExecutionStatus::Failed, "the compared path could not be resolved" };
			}
			result = ExecuteVsCodeDiffCommand(workbench::commands::BuildApiDiffArguments(arguments));
			break;
		}
		case workbench::scm::EGitDiffCommandKind::Open: {
			workbench::commands::ApiOpenArguments arguments;
			arguments.resourceUri = workbench::scm::BuildGitDiffEndpointUri(*input.modified, root);
			// Upstream passes `override: false` for a both-modified conflict and
			// leaves it undefined otherwise. Absent and `false` are different
			// requests, so the distinction is carried rather than flattened.
			if (found->status == workbench::scm::EGitFileStatus::BothModified) arguments.overrideEditor = false;
			arguments.label = input.title;
			if (arguments.resourceUri.empty()) {
				return { EWorkbenchCommandExecutionStatus::Failed, "the named path could not be resolved" };
			}
			result = ExecuteVsCodeOpenCommand(workbench::commands::BuildApiOpenArguments(arguments));
			break;
		}
		case workbench::scm::EGitDiffCommandKind::None:
		default:
			// Upstream reaches its `vscode.open` branch with an undefined URI here.
			// There is no text on either side, so refusing is the honest form of it.
			return { EWorkbenchCommandExecutionStatus::NotApplicable, "this row has no content to compare" };
		}
		if (result.status != EWorkbenchCommandExecutionStatus::Succeeded) return result;
	}
	return result;
}

namespace {

//!
//! @brief The 0-based modified-side line span a run of rendered rows names.
//!
//! The surface selects **rows**, and a row is a position in the alignment, not a
//! line of either text. A row that carries a modified line number names that
//! line. A row that does not is a line the original side has and the modified
//! side does not, and upstream's `getModifiedRange` places such a deletion on
//! the seam between the two modified lines that surround it —
//! `intersectDiffWithRange` reaches that seam from either side, so the preceding
//! modified line names it. A deletion at the very top has no preceding line, and
//! there the following line is the one that reaches the seam.
//!
//! False when the row range is empty or lies outside the rows, and then neither
//! output is written.
//!
[[nodiscard]] bool SelectedModifiedLineSpan(
	const std::vector<SDiffSurfaceRow>& rows, int firstRow, int lastRow, int& startLine, int& endLine)
{
	int precedingModifiedLines = 0;
	int first = -1;
	int last = -1;
	for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
		const int modifiedLineNumber = rows[index].modifiedLineNumber;
		if (index >= firstRow && index <= lastRow) {
			const int line = modifiedLineNumber > 0
				? modifiedLineNumber - 1
				: (precedingModifiedLines > 0 ? precedingModifiedLines - 1 : 0);
			if (first < 0 || line < first) first = line;
			if (last < 0 || line > last) last = line;
		}
		if (modifiedLineNumber > 0) ++precedingModifiedLines;
	}
	if (first < 0) return false;
	startLine = first;
	endLine = last;
	return true;
}

//! `ClassifyGitTextEncoding` over a byte vector, without forming a view over a null pointer.
[[nodiscard]] workbench::scm::EGitTextEncoding ClassifyDiffSide(const std::vector<std::uint8_t>& bytes) noexcept
{
	if (bytes.empty()) return workbench::scm::EGitTextEncoding::Utf8;
	return workbench::scm::ClassifyGitTextEncoding(
		std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

} // namespace

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitSelectedRangesCommand(bool stage)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	if (GetHwnd() == nullptr || m_diffSurface == nullptr || !m_diffSurface->HasDiff()
		|| m_diffRepositoryRoot.empty() || m_diffModifiedUri.empty()) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no comparison is open in this window" };
	}
	int firstRow = 0;
	int lastRow = 0;
	if (!m_diffSurface->SelectedRowRange(firstRow, lastRow)) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no lines are selected in the comparison" };
	}

	// Copied, because refreshing the comparison at the end of a successful stage
	// rewrites these members.
	const std::wstring root = m_diffRepositoryRoot;
	const std::wstring originalUri = m_diffOriginalUri;
	const std::wstring modifiedUri = m_diffModifiedUri;
	const auto original = workbench::scm::ResolveGitDiffEndpointUri(originalUri, root);
	const auto modified = workbench::scm::ResolveGitDiffEndpointUri(modifiedUri, root);
	if (!original || !modified) {
		return { EWorkbenchCommandExecutionStatus::Failed, "the compared path could not be resolved" };
	}

	// Upstream gates each command on what the open comparison *is*, not on which
	// button was pressed. `stageSelectedRanges` requires the right-hand side to be
	// the working tree, and `unstageSelectedRanges` requires it to be the index
	// (`fromGitUri(...).ref === ''`) with HEAD on the left. A comparison that is
	// neither has no index entry these lines could be written into, so refusing is
	// the honest answer rather than picking a path and writing somewhere.
	if (stage) {
		if (original->source != workbench::scm::EGitDiffSource::Repository
			|| modified->source != workbench::scm::EGitDiffSource::WorkingTree) {
			return { EWorkbenchCommandExecutionStatus::Unsupported,
				"staging selected ranges needs a comparison whose right-hand side is the working tree" };
		}
	} else if (modified->source != workbench::scm::EGitDiffSource::Repository || !modified->ref.empty()
		|| original->source != workbench::scm::EGitDiffSource::Repository || original->ref.empty()) {
		return { EWorkbenchCommandExecutionStatus::Unsupported,
			"unstaging selected ranges needs a comparison whose right-hand side is the index" };
	}

	std::vector<std::uint8_t> originalBytes;
	std::vector<std::uint8_t> modifiedBytes;
	std::wstring failure;
	const auto gitOutputSink = MakeGitOutputSink(m_outputService);
	if (!ReadDiffEndpointBytes(*original, root, originalBytes, failure, gitOutputSink)
		|| !ReadDiffEndpointBytes(*modified, root, modifiedBytes, failure, gitOutputSink)) {
		return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(failure) };
	}
	const auto originalText = DecodeDiffSide(originalBytes);
	const auto modifiedText = DecodeDiffSide(modifiedBytes);

	// Both sides are re-read and compared against what is on screen, because the
	// rows the user selected are the only description of their intent and a row
	// index means nothing against text that has moved. Upstream never faces this:
	// its diff editor holds live documents and recomputes the diff as they change.
	// Here a stale comparison would stage a region the user never looked at, so
	// this fails closed rather than staging against the newer text.
	const auto& content = m_diffSurface->Content();
	if (workbench::scm::SplitGitDiffLines(originalText) != content.originalLines
		|| workbench::scm::SplitGitDiffLines(modifiedText) != content.modifiedLines) {
		return { EWorkbenchCommandExecutionStatus::Failed,
			"the compared files changed after this comparison was opened; open the comparison again" };
	}

	const auto originalStaging = workbench::scm::MakeGitStagingText(originalText);
	const auto modifiedStaging = workbench::scm::MakeGitStagingText(modifiedText);
	int startLine = 0;
	int endLine = 0;
	if (!SelectedModifiedLineSpan(content.rows, firstRow, lastRow, startLine, endLine)) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable,
			"the selection names no line of the right-hand side" };
	}
	const auto selections = workbench::scm::NormalizeGitSelectedLines(
		{ workbench::scm::GitSelectedLines{ startLine, endLine } }, modifiedStaging);
	const auto diff = workbench::scm::ComputeGitLineDiff(content.originalLines, content.modifiedLines);
	if (diff.hitTimeout) {
		// A bounded alignment is not a complete description of the change, so the
		// changes it names are not the changes the selection covers. The surface
		// already tells the user the comparison is truncated; staging from it would
		// write an index entry built from a partial diff.
		return { EWorkbenchCommandExecutionStatus::Unsupported,
			"this comparison is too large to be diffed completely, so part of it cannot be staged" };
	}
	auto selected = workbench::scm::SelectGitLineChanges(
		modifiedStaging, workbench::scm::ToGitLineChanges(diff), selections);
	if (selected.empty()) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "the selected lines contain no change" };
	}

	// Upstream builds no patch. It assembles the complete new content of the index
	// entry and writes that, and unstaging is the same assembly with the two sides
	// exchanged and every selected change inverted.
	std::wstring staged;
	if (stage) {
		staged = workbench::scm::ApplyGitLineChanges(originalStaging, modifiedStaging, selected);
	} else {
		for (auto& change : selected) change = workbench::scm::InvertGitLineChange(change);
		staged = workbench::scm::ApplyGitLineChanges(modifiedStaging, originalStaging, selected);
	}

	// The assembled content carries text from both sides, so it can be written
	// back exactly only in an encoding both sides round-trip. UTF-8 is used when
	// both decoded as UTF-8; otherwise the byte-wise fallback applies, and
	// `EncodeGitText` refuses anything it cannot represent rather than
	// substituting a replacement character into a durable blob.
	const auto encoding =
		(ClassifyDiffSide(originalBytes) == workbench::scm::EGitTextEncoding::Utf8
			&& ClassifyDiffSide(modifiedBytes) == workbench::scm::EGitTextEncoding::Utf8)
		? workbench::scm::EGitTextEncoding::Utf8
		: workbench::scm::EGitTextEncoding::Latin1Fallback;
	auto encoded = workbench::scm::EncodeGitText(staged, encoding);
	if (!encoded) {
		return { EWorkbenchCommandExecutionStatus::Failed,
			"the selected text cannot be written back in the encoding it was read with" };
	}

	const auto runGit = [&root, sink = MakeGitOutputSink(m_outputService)]
		(const std::vector<std::wstring>& arguments, std::string standardInput) {
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = root;
		request.arguments = arguments;
		request.standardInput = std::move(standardInput);
		return workbench::scm::RunGitLogged(request, nullptr, sink);
	};

	const auto& path = modified->path;
	const auto hashed = runGit(workbench::scm::BuildGitHashObjectArguments(path), std::move(*encoded));
	if (!hashed.Succeeded()) {
		return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(workbench::scm::DescribeGitFailure(hashed)) };
	}
	const auto object = workbench::scm::ParseGitHashObjectName(DecodeDiffSide(hashed.standardOutput));
	if (!object) {
		return { EWorkbenchCommandExecutionStatus::Failed, "git wrote no object name for the staged content" };
	}

	// Upstream reads the mode from HEAD rather than from the index, so staging
	// part of a change cannot silently change the file's mode, and falls back to
	// the index for a repository that has no HEAD commit yet.
	std::wstring mode;
	auto details = runGit(workbench::scm::BuildGitHeadObjectDetailsArguments(path), {});
	if (!details.Succeeded()) {
		details = runGit(workbench::scm::BuildGitStagedObjectDetailsArguments(path), {});
	}
	if (details.Succeeded()) {
		if (auto parsed = workbench::scm::ParseGitObjectMode(DecodeDiffSide(details.standardOutput))) {
			mode = std::move(*parsed);
		}
	}
	// Upstream's `UnknownPath`: git knows nothing about this path, so the entry is
	// created at the default mode instead of replacing one that does not exist.
	const bool add = mode.empty();
	if (add) mode = L"100644";

	const auto updated = runGit(workbench::scm::BuildGitUpdateIndexArguments(mode, *object, path, add), {});
	if (!updated.Succeeded()) {
		return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(workbench::scm::DescribeGitFailure(updated)) };
	}

	// The index moved, so every published SCM fact is stale.
	if (m_scmTool != nullptr) m_scmTool->Refresh();
	// So is the comparison: at least one of its two sides is now different text.
	// Upstream's diff editor recomputes itself from its live documents; the
	// nearest thing here is to open the same comparison again, which re-reads both
	// sides. A comparison that can no longer be opened is retracted rather than
	// left showing text that no longer exists.
	workbench::commands::ApiDiffArguments arguments;
	arguments.originalUri = originalUri;
	arguments.modifiedUri = modifiedUri;
	arguments.title = m_diffSurface->Title();
	if (ExecuteVsCodeDiffCommand(workbench::commands::BuildApiDiffArguments(arguments)).status
		!= EWorkbenchCommandExecutionStatus::Succeeded) {
		ClearDiffSurface();
	}
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitCommitCommand(
	EGitCommitCommand command, std::string_view argumentsJson)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	const auto postCommit = workbench::scm::ParseGitCommitPostCommandArguments(argumentsJson);
	if (!postCommit) {
		return { EWorkbenchCommandExecutionStatus::Failed,
			"git.commit received an unsupported post-commit arguments payload" };
	}

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}
	// Upstream's `commitWithAnyInput` reads the message off the repository's own
	// SCM input box. With no view there is no box, and treating its absence as an
	// empty message would commit without the text the user believes they typed.
	if (m_scmTool == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no source control view is open" };
	}

	workbench::scm::GitCommitCommandContext context;
	context.text = [](std::string_view key, std::wstring_view argument) {
		return ResolveLocalizedScmTextKey(key, argument);
	};
	context.repositoryRoot = root;
	context.run = [&root, sink = MakeGitOutputSink(m_outputService)]
		(const std::vector<std::wstring>& arguments, std::string_view standardInput) {
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = root;
		request.arguments = arguments;
		// The message always travels through `--file -`, so it reaches git on the
		// runner's stdin thread. An argument would be length-bounded and could be
		// reread as an option.
		request.standardInput = std::string(standardInput);
		return workbench::scm::RunGitLogged(request, nullptr, sink);
	};
	context.message = [this](std::wstring_view message) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, std::wstring(message).c_str());
	};
	// Upstream's gates are `showWarningMessage(..., { modal: true }, ...buttons)`
	// plus one `showInformationMessage`. A task dialog is the native modal that
	// carries that many labelled choices; index 0 is the primary action, and
	// dismissal is a cancel rather than an implicit yes.
	context.confirm = [this](const workbench::scm::GitCommitPrompt& prompt) -> std::optional<std::size_t> {
		if (prompt.choices.empty()) return std::nullopt;
		std::vector<TASKDIALOG_BUTTON> buttons;
		buttons.reserve(prompt.choices.size());
		for (std::size_t index = 0; index < prompt.choices.size(); ++index) {
			buttons.push_back({ 1000 + static_cast<int>(index), prompt.choices[index].c_str() });
		}
		TASKDIALOGCONFIG config{};
		config.cbSize = sizeof(config);
		config.hwndParent = GetHwnd();
		config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
		config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
		config.pszWindowTitle = L"Sakura Editor NEXT";
		// `warning` carries which of the two upstream message functions produced
		// this prompt, so "there are no changes to commit" stays informational
		// instead of being escalated into a warning it never was.
		config.pszMainIcon = prompt.warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
		config.pszMainInstruction = prompt.message.c_str();
		config.pszContent = prompt.detail.empty() ? nullptr : prompt.detail.c_str();
		config.cButtons = static_cast<UINT>(buttons.size());
		config.pButtons = buttons.data();
		int selected = 0;
		if (FAILED(::TaskDialogIndirect(&config, &selected, nullptr, nullptr))) return std::nullopt;
		if (selected < 1000) return std::nullopt;
		const auto index = static_cast<std::size_t>(selected - 1000);
		if (index >= prompt.choices.size()) return std::nullopt;
		return index;
	};
	context.promptForMessage = [this](std::wstring_view placeholder,
		std::wstring_view prompt) -> std::optional<std::wstring> {
		SQuickInputRequest request;
		request.kind = EQuickInputKind::InputBox;
		// Upstream's box carries a title and a separate prompt line. This dialog
		// paints only a caption, so the prompt takes it and the placeholder - the
		// half that names the branch being committed on - stays in the field.
		request.title = prompt;
		request.placeholder = placeholder;
		CQuickInputDialog dialog(request);
		const auto completion = dialog.DoModal(GetHwnd());
		if (completion.state != EQuickInputState::Accepted) return std::nullopt;
		return completion.value.value_or(std::wstring{});
	};
	// Upstream enumerates every dirty `workspace.textDocument` inside the
	// repository. An editor process owns exactly one document, so this can only
	// report that one; the divergence is recorded in `workbench/scm/CLAUDE.md`.
	std::wstring rootPrefix = root;
	while (!rootPrefix.empty() && (rootPrefix.back() == L'\\' || rootPrefix.back() == L'/')) {
		rootPrefix.pop_back();
	}
	context.dirtyDocuments = [this, rootPrefix]() {
		std::vector<std::wstring> documents;
		if (!HasActiveEditorInput() || !GetDocument()->m_cDocEditor.IsModified()) return documents;
		if (!GetDocument()->m_cDocFile.GetFilePathClass().IsValidPath()) return documents;
		std::wstring path = GetDocument()->m_cDocFile.GetFilePath();
		// `isDescendant`: a dirty document outside this repository is not this
		// commit's business, and prompting about it would name an unrelated file.
		if (rootPrefix.empty() || path.size() <= rootPrefix.size()) return documents;
		if (::_wcsnicmp(path.c_str(), rootPrefix.c_str(), rootPrefix.size()) != 0) return documents;
		const auto boundary = path[rootPrefix.size()];
		if (boundary != L'\\' && boundary != L'/') return documents;
		documents.push_back(std::move(path));
		return documents;
	};
	// The prompt's primary action is upstream's `Save All & Commit`, and the one
	// document above is the whole set this process can save.
	context.saveDocuments = [this]() { return GetDocument()->m_cDocFileOperation.FileSave(); };

	const auto& scmState = m_scmTool->State();
	const auto state = workbench::scm::BuildGitCommitRepositoryState(root,
		workbench::scm::CollectGitStageResources(scmState),
		u8stowcs(workbench::scm::GitHeadShortName(scmState)),
		!scmState.commit.empty());

	workbench::scm::GitCommitCommandResult result{};
	if (command == EGitCommitCommand::UndoCommit) {
		result = workbench::scm::RunGitUndoCommit(context, state);
	}
	else {
		workbench::scm::GitCommitOptions options;
		options.amend = command == EGitCommitCommand::CommitAmend;
		const auto typed = m_scmTool->CommitMessage();
		result = workbench::scm::RunGitCommit(context, state, typed, options);
	}

	// Upstream assigns `repository.inputBox.value` inside the command itself, so
	// the box changes independently of what the command then reports. An absent
	// value means "leave it alone", which is what keeps a failed commit from
	// discarding the message that failed.
	if (result.inputBoxValue) m_scmTool->SetCommitMessage(*result.inputBoxValue);

	switch (result.status) {
	case workbench::scm::EGitCommitCommandStatus::Succeeded:
		// HEAD and the index both moved, so every published SCM fact is stale.
		m_scmTool->Refresh();
		if (*postCommit == workbench::scm::EGitPostCommitCommand::None) {
			return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
		}
		{
			const auto followUp = ExecuteGitSyncCommand(
				*postCommit == workbench::scm::EGitPostCommitCommand::Push
					? EGitSyncCommand::Push : EGitSyncCommand::Sync,
				true);
			if (followUp.Succeeded()) return followUp;
			std::string detail = "commit succeeded, but the post-commit operation did not complete";
			if (!followUp.detail.empty()) {
				detail += ": ";
				detail += followUp.detail;
			}
			return { followUp.status, std::move(detail) };
		}
	case workbench::scm::EGitCommitCommandStatus::NotApplicable:
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "there was nothing to commit" };
	case workbench::scm::EGitCommitCommandStatus::Cancelled:
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "the user dismissed the prompt" };
	case workbench::scm::EGitCommitCommandStatus::UnsupportedRebaseInProgress:
		return { EWorkbenchCommandExecutionStatus::Unsupported, wcstou8s(result.message) };
	case workbench::scm::EGitCommitCommandStatus::Failed:
	default:
		break;
	}
	return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(result.message) };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteGitSyncCommand(
	EGitSyncCommand command, bool commitJustSucceeded)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	const auto root = GetSemanticWorkspaceRoot();
	if (root.empty() || GetHwnd() == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no repository is open in this window" };
	}
	// Upstream's handlers read HEAD's name, its upstream, and the ahead/behind
	// counts off the repository. Those are exactly the facts the published SCM
	// state carries, so a window with no Source Control view has no repository
	// state to act on; inventing one would push against a branch nobody read.
	if (m_scmTool == nullptr) {
		return { EWorkbenchCommandExecutionStatus::NotApplicable, "no source control view is open" };
	}

	// Every command captions its own dialogs with the title upstream publishes
	// for it, for the same reason the branch commands do: a framed dialog must
	// caption itself with something, and upstream's own string is not an
	// invented label.
	std::string_view commandId = "git.fetch";
	std::wstring_view fallback = L"Fetch";
	switch (command) {
	case EGitSyncCommand::FetchPrune: commandId = "git.fetchPrune"; fallback = L"Fetch (Prune)"; break;
	case EGitSyncCommand::FetchAll: commandId = "git.fetchAll"; fallback = L"Fetch From All Remotes"; break;
	case EGitSyncCommand::Pull: commandId = "git.pull"; fallback = L"Pull"; break;
	case EGitSyncCommand::PullRebase: commandId = "git.pullRebase"; fallback = L"Pull (Rebase)"; break;
	case EGitSyncCommand::Push: commandId = "git.push"; fallback = L"Push"; break;
	case EGitSyncCommand::Sync: commandId = "git.sync"; fallback = L"Sync"; break;
	case EGitSyncCommand::SyncRebase: commandId = "git.syncRebase"; fallback = L"Sync (Rebase)"; break;
	case EGitSyncCommand::Publish: commandId = "git.publish"; fallback = L"Publish Branch..."; break;
	case EGitSyncCommand::Fetch:
	default:
		break;
	}
	const std::wstring caption = LocalizedWorkbenchCommandTitle(commandId, fallback);

	workbench::scm::GitSyncCommandContext context;
	context.text = [](std::string_view key, std::wstring_view argument) {
		return ResolveLocalizedScmTextKey(key, argument);
	};
	context.run = [&root, sink = MakeGitOutputSink(m_outputService)]
		(const std::vector<std::wstring>& arguments) {
		workbench::scm::GitExecutionRequest request;
		request.workingDirectory = root;
		request.arguments = arguments;
		return workbench::scm::RunGitLogged(request, nullptr, sink);
	};
	context.message = [this](std::wstring_view message) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, std::wstring(message).c_str());
	};
	// The same native modal the commit gates use. Index 0 is upstream's primary
	// action and dismissal is a cancel, never an implicit yes - which matters
	// most here, where the primary action pushes commits to a remote.
	context.confirm = [this](const workbench::scm::GitPrompt& prompt) -> std::optional<std::size_t> {
		if (prompt.choices.empty()) return std::nullopt;
		std::vector<TASKDIALOG_BUTTON> buttons;
		buttons.reserve(prompt.choices.size());
		for (std::size_t index = 0; index < prompt.choices.size(); ++index) {
			buttons.push_back({ 1000 + static_cast<int>(index), prompt.choices[index].c_str() });
		}
		TASKDIALOGCONFIG config{};
		config.cbSize = sizeof(config);
		config.hwndParent = GetHwnd();
		config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
		config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
		config.pszWindowTitle = L"Sakura Editor NEXT";
		config.pszMainIcon = prompt.warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
		config.pszMainInstruction = prompt.message.c_str();
		config.pszContent = prompt.detail.empty() ? nullptr : prompt.detail.c_str();
		config.cButtons = static_cast<UINT>(buttons.size());
		config.pButtons = buttons.data();
		int selected = 0;
		if (FAILED(::TaskDialogIndirect(&config, &selected, nullptr, nullptr))) return std::nullopt;
		if (selected < 1000) return std::nullopt;
		const auto index = static_cast<std::size_t>(selected - 1000);
		if (index >= prompt.choices.size()) return std::nullopt;
		return index;
	};
	context.pickRemote = [this, caption](const std::vector<workbench::scm::GitRemotePickItem>& items,
		std::wstring_view placeholder) -> std::optional<std::size_t> {
		if (items.empty()) return std::nullopt;
		SQuickInputRequest request;
		request.kind = EQuickInputKind::QuickPick;
		request.title = caption;
		request.placeholder = placeholder;
		request.items.reserve(items.size());
		for (std::size_t index = 0; index < items.size(); ++index) {
			// The native list renders label text literally, so upstream's leading
			// `$(cloud)` / `$(cloud-download)` codicon markup is removed here
			// rather than drawn as the characters `$(cloud) origin`. The model
			// keeps upstream's own label, exactly as it keeps the checkout
			// picker's separator rows, so a list that can draw a codicon needs no
			// model change.
			std::wstring label = items[index].label;
			if (label.starts_with(L"$(")) {
				if (const auto close = label.find(L')'); close != std::wstring::npos) {
					label.erase(0, close + 1);
					while (!label.empty() && label.front() == L' ') label.erase(0, 1);
				}
			}
			request.items.push_back({
				.sourceIndex = index,
				.label = std::move(label),
				.description = items[index].description,
			});
		}
		CQuickInputDialog dialog(request);
		const auto completion = dialog.DoModal(GetHwnd());
		if (completion.state != EQuickInputState::Accepted
			|| completion.selectedIndices.size() != 1) {
			return std::nullopt;
		}
		const auto selected = completion.selectedIndices.front();
		if (selected >= items.size()) return std::nullopt;
		return selected;
	};

	// Upstream's `getRemotes` asks git rather than deriving the remote set from
	// the branch state, so an unreadable remote list is reported as the failure
	// it is instead of silently becoming "this repository has no remotes".
	const auto remotesResult = context.run(workbench::scm::BuildGitRemoteArguments());
	if (!remotesResult.Succeeded() || remotesResult.exitCode != 0) {
		return { EWorkbenchCommandExecutionStatus::Failed,
			wcstou8s(workbench::scm::DescribeGitFailure(remotesResult)) };
	}
	auto remotes = workbench::scm::ParseGitRemotes({
		reinterpret_cast<const char*>(remotesResult.standardOutput.data()),
		remotesResult.standardOutput.size() });
	auto state = workbench::scm::BuildGitSyncRepositoryState(m_scmTool->State(), std::move(remotes));
	if (commitJustSucceeded && state.HasUpstream()) {
		// Refresh is intentionally asynchronous. The successful commit itself is
		// proof that this branch is at least one commit ahead, so do not let the
		// pre-commit snapshot make Commit & Sync skip its push half.
		state.ahead = std::max(state.ahead, 1);
	}

	workbench::scm::GitSyncCommandResult result{};
	switch (command) {
	case EGitSyncCommand::Fetch:
		result = workbench::scm::RunGitFetch(context, state, workbench::scm::EGitFetchScope::Default);
		break;
	case EGitSyncCommand::FetchPrune:
		result = workbench::scm::RunGitFetch(context, state, workbench::scm::EGitFetchScope::Prune);
		break;
	case EGitSyncCommand::FetchAll:
		result = workbench::scm::RunGitFetch(context, state, workbench::scm::EGitFetchScope::All);
		break;
	case EGitSyncCommand::Pull:
		result = workbench::scm::RunGitPull(context, state, false);
		break;
	case EGitSyncCommand::PullRebase:
		result = workbench::scm::RunGitPull(context, state, true);
		break;
	case EGitSyncCommand::Push:
		result = workbench::scm::RunGitPush(context, state);
		break;
	case EGitSyncCommand::Sync:
		// Upstream's `_sync` computes `rebase || rebaseWhenSync`, and `git.sync`
		// passes `false`, so the setting is what decides here. `git.syncRebase`
		// passes `true` and the setting cannot turn it back off.
		result = workbench::scm::RunGitSync(context, state, context.configuration.rebaseWhenSync);
		break;
	case EGitSyncCommand::SyncRebase:
		result = workbench::scm::RunGitSync(context, state, true);
		break;
	case EGitSyncCommand::Publish:
		result = workbench::scm::RunGitPublish(context, state);
		break;
	}

	switch (result.status) {
	case workbench::scm::EGitSyncCommandStatus::Succeeded:
		// Remote-tracking refs, the ahead/behind counts, and possibly HEAD's own
		// upstream all moved, so every published SCM fact is stale.
		m_scmTool->Refresh();
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	case workbench::scm::EGitSyncCommandStatus::NotApplicable:
	case workbench::scm::EGitSyncCommandStatus::Cancelled:
		// Both are upstream's own early returns rather than errors, and each one
		// already says which gate it was, so the reason is carried through rather
		// than replaced with a sentence that names the status instead of the gate.
		return { EWorkbenchCommandExecutionStatus::NotApplicable, wcstou8s(result.message) };
	case workbench::scm::EGitSyncCommandStatus::Failed:
	default:
		break;
	}
	return { EWorkbenchCommandExecutionStatus::Failed, wcstou8s(result.message) };
}

void CEditWnd::RelayoutEditorProjections()
{
	RECT client{};
	if (GetHwnd() == nullptr || !::GetClientRect(GetHwnd(), &client)) return;
	(void)OnSize2(m_nWinSizeType,
		MAKELONG(client.right - client.left, client.bottom - client.top), false);
}

bool CEditWnd::ShowDiffSurface(SDiffSurfaceContent content)
{
	if (!m_diffSurface) return false;
	// The surface is a composition-layer projection, not an `EditorInput`. It has
	// no document model and no tab, so showing it over an open document would
	// hide a document the user can no longer reach. Refusing is the honest
	// boundary until a real diff `EditorInput` exists.
	if (HasActiveEditorInput()) return false;
	m_diffSurface->ShowDiff(std::move(content));
	// Opening a comparison replaces whatever the group was showing, exactly as it
	// does in VS Code.
	if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
	if (!m_pPrintPreview) m_diffSurface->Show();
	RelayoutEditorProjections();
	return true;
}

void CEditWnd::ClearDiffSurface()
{
	if (!m_diffSurface) return;
	m_diffSurface->ClearDiff();
	m_diffSurface->Hide();
	m_diffRepositoryRoot.clear();
	m_diffOriginalUri.clear();
	m_diffModifiedUri.clear();
	if (m_emptyEditorSurface && !HasActiveEditorInput() && !m_pPrintPreview) {
		m_emptyEditorSurface->Show();
	}
	RelayoutEditorProjections();
	(void)RefreshWorkbenchCommandContext();
}

void CEditWnd::RefreshStatusbarPresentation()
{
	if (m_workbenchRuntime == nullptr) return;
	using workbench::statusbar::EStatusbarEntryAlignment;
	using workbench::statusbar::StatusbarEntry;
	std::vector<StatusbarEntry> entries{
		{ "status.host", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_REMOTE_HOST), EStatusbarEntryAlignment::Left, true },
		{ "status.scm", LocalizedWorkbenchString(STR_WORKBENCH_SOURCE_CONTROL_TITLE), EStatusbarEntryAlignment::Left, true },
		{ "status.editor.selection", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_SELECTION), EStatusbarEntryAlignment::Right, true },
		{ "status.editor.eol", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_LINE_ENDING), EStatusbarEntryAlignment::Right, true },
		{ "sakura.status.editor.characterCode", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_CHARACTER_CODE), EStatusbarEntryAlignment::Right, true },
		{ "status.editor.encoding", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_ENCODING), EStatusbarEntryAlignment::Right, true },
		{ "sakura.status.macroRecording", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_MACRO_RECORDING), EStatusbarEntryAlignment::Right, true },
		{ "status.editor.inputMode", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_INPUT_MODE), EStatusbarEntryAlignment::Right, true },
		{ "status.editor.zoom", LocalizedWorkbenchString(STR_WORKBENCH_STATUS_ZOOM), EStatusbarEntryAlignment::Right, true },
	};
	if (!m_workbenchRuntime->StatusbarState().SetEntries(std::move(entries))) return;
	m_cStatusBar.SetStatusbarViewSnapshot(m_workbenchRuntime->StatusbarState().Snapshot());
	LayoutStatusBarParts();
}

void CEditWnd::SetStatusbarEntryHidden(std::string_view id, bool hidden)
{
	if (m_workbenchRuntime == nullptr) return;
	const bool changed = m_workbenchRuntime->StatusbarState().SetHidden(id, hidden);
	if (changed) (void)m_workbenchRuntime->PersistStatusbarVisibility();
	m_cStatusBar.SetStatusbarViewSnapshot(m_workbenchRuntime->StatusbarState().Snapshot());
	LayoutStatusBarParts();
}

void CEditWnd::CloseWorkbench() noexcept
{
	if (m_gitBranchCommandSession) {
		m_gitBranchCommandSession->store(false);
		m_gitBranchCommandSession.reset();
	}
	// Fence every surface lifetime before any page HWND or callback gate can be
	// destroyed. This only requests close and transfers thread ownership; it
	// never waits on the UI thread.
	CloseFrameRuntime();
	// Model callbacks own only the shared gate. Disconnect it before removing
	// subscriptions so an in-flight callback cannot post into torn-down panels.
	if (m_themeConfigurationGate) {
		std::lock_guard lock(m_themeConfigurationGate->mutex);
		m_themeConfigurationGate->connected = false;
		m_themeConfigurationGate->window = nullptr;
	}
	m_themeConfigurationSubscription.reset();
	m_themeConfigurationGate.reset();
	CloseWorkbenchServiceProjection();
	// The update worker can be mid-download. Closing its gate and joining the
	// worker here is what makes the registry executors below safe to destroy: an
	// update notification after this point has nowhere to arrive.
	CloseUpdateProjection();
	// The Quick Input surface owns callbacks into this composition root. Detach and
	// destroy it before the command registry/service it queries can disappear.
	if (m_commandPaletteOverlay) {
		m_commandPaletteOverlay->SetStringsCallback({});
		m_commandPaletteOverlay->SetSearchCallback({});
		m_commandPaletteOverlay->SetSelectionCallback({});
		m_commandPaletteOverlay->SetAcceptCallback({});
		m_commandPaletteOverlay->SetCancelCallback({});
		m_commandPaletteOverlay->Destroy();
		m_commandPaletteOverlay.reset();
	}
	if (m_layoutStateSubscription) m_layoutStateSubscription->Unsubscribe();
	m_layoutStateSubscription.reset();
	// Registry executors capture this window and must be gone before any host they
	// can project is closed. Context state has no external owner and is window-local.
	if (m_customFrame) {
		m_customFrame->SetManageMenuActionCallback({});
		m_customFrame->SetAccountMenuModelCallback({});
	}
	if (m_accountDiscoveryService) m_accountDiscoveryService->Stop();
	m_accountDiscoveryService.reset();
	ClearWorkbenchKeybindingChord();
	m_workbenchCommandRegistry.reset();
	m_workbenchContextKeyService.reset();
	if (m_editorCoreSubscription) m_editorCoreSubscription->Unsubscribe();
	m_editorCoreSubscription.reset();
	if (m_diffSurface) {
		m_diffSurface->SetOnCloseRequested({});
		m_diffSurface->ClearDiff();
		m_diffSurface->Destroy();
	}
	m_diffSurface.reset();
	if (m_emptyEditorSurface) m_emptyEditorSurface->Destroy();
	m_emptyEditorSurface.reset();
	m_startupOutlineReloadPending = false;
	m_startupWorkbenchCompletionPosted = false;
	if (m_bottomPanelTool) {
		m_bottomPanelTool->SetProblemActivationCallback({});
		m_bottomPanelTool->SetOutputChannelSelectionCallback({});
		m_bottomPanelTool->SetContainerSelectionCallback({});
	}
	m_cStatusBar.SetWorkbenchCommandCallback({});
	m_cStatusBar.SetStatusbarVisibilityCallback({});
	m_colorThemeRegistry.reset();
	theme::CThemeService::ClearActiveColorThemePalette();
	if (m_activityBar) m_activityBar->Close();
	if (m_auxiliaryActivityBar) m_auxiliaryActivityBar->Close();
	// Stop staged projection and detach its pages before the page pool or any
	// physical host is closed. The service borrows all three hosts and is gone
	// before their destruction begins.
	if (m_paneCompositeProjection) (void)m_paneCompositeProjection->Close();
	m_paneCompositeProjection.reset();
	// The shared pages are borrowed by both side-bar hosts, so they must be destroyed
	// before either host window that may still be their parent.
	if (m_viewContainerPages) m_viewContainerPages->Close();
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->Close();
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->Close();
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->Close();
	m_activityBar.reset();
	m_auxiliaryActivityBar.reset();
	m_leftWorkbenchPanel.reset();
	m_rightWorkbenchPanel.reset();
	m_bottomWorkbenchPanel.reset();
	m_sidebarHost = nullptr;
	m_auxiliaryBarHost = nullptr;
	m_viewContainerPages.reset();
	m_bottomPanelTool = nullptr;
	m_explorerTool = nullptr;
	m_outlineWorkbenchTool = nullptr;
	m_scmTool = nullptr;
	m_searchTool = nullptr;
	m_extensionsTool = nullptr;
	m_terminalTool = nullptr;
	m_bottomWorkbenchMaximized = false;
	m_workspaceContext.reset();
}

bool CEditWnd::ApplyWorkbenchTheme(std::wstring_view previewTheme)
{
	auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark
		: theme::ThemeMode::Light;
	// The Explorer's file icon theme needs the ColorThemeKind, not the Dark/Light
	// mode: VS Code emits an icon theme's `light` section under the `.vs` body class
	// alone, so High Contrast Light keeps the base section even though its mode is
	// Light.  Until a color theme is actually loaded below, the saved mode is the
	// only evidence available.
	bool lightColorTheme = mode == theme::ThemeMode::Light;
	// The persisted VS Code setting is resolved through the same profile,
	// workspace, and single-folder target used by the configuration service.
	// An empty/invalid setting first resolves to Sakura's built-in theme for the
	// saved mode; the constexpr palette remains the final fail-closed fallback.
	std::optional<theme::ColorThemeSnapshot> resolvedTheme;
	if (m_colorThemeRegistry != nullptr && m_workbenchRuntime != nullptr) {
		try {
			std::wstring selectedTheme;
			if (!previewTheme.empty()) {
				selectedTheme = previewTheme;
			} else {
				const auto target = BuildWorkbenchConfigurationTarget();
				const auto lookup = m_workbenchRuntime->Configuration().GetValue("workbench.colorTheme", target);
				if (lookup.value) {
					if (const auto* selected = std::get_if<std::wstring>(&lookup.value->Value());
						selected != nullptr) {
						selectedTheme = *selected;
					}
				}
			}
			const auto resolveTheme = [this, &resolvedTheme](std::wstring_view idOrLabel) {
				const auto loaded = m_colorThemeRegistry->Load(idOrLabel);
				if (!loaded.Succeeded()) return false;
				resolvedTheme = std::move(*loaded.theme);
				return true;
			};
			if (!selectedTheme.empty() && resolveTheme(selectedTheme)) {
				// The explicit VS Code setting won.
			} else if (!previewTheme.empty()) {
				// A Quick Pick preview is advisory and must not replace the currently
				// valid palette when its item can no longer be resolved.
				return false;
			} else {
				// Empty settings and unreadable third-party settings both resolve to
				// the built-in theme matching Sakura's saved Dark/Light preference.
				(void)resolveTheme(theme::CColorThemeRegistry::BuiltinThemeId(mode));
			}
		}
		catch (...) {
			if (!previewTheme.empty()) return false;
			// Keep the legacy saved mode when a third-party theme cannot be read.
		}
	} else if (!previewTheme.empty()) {
		return false;
	}

	theme::CThemeService::ClearActiveColorThemePalette();
	if (resolvedTheme) {
		if (!theme::CThemeService::IsHighContrastActive()) {
			theme::CThemeService::SetActiveColorThemePalette(resolvedTheme->palette);
			theme::CThemeService::SetActiveColorThemeSyntaxPalette(resolvedTheme->syntaxPalette);
			theme::CThemeService::SetActiveColorThemeTokenColors(resolvedTheme->tokenColors);
		}
		mode = theme::CColorThemeRegistry::ModeForKind(resolvedTheme->info.kind);
		lightColorTheme = resolvedTheme->info.kind == theme::ColorThemeKind::Light;
	}
	// Published as process-local active-theme state beside the palette and syntax
	// overlays above. The Explorer reads it on its paint path to select the file
	// icon theme's `light` section, the way VS Code's `.vs` body class does, so no
	// part host in between has to carry an icon-theme argument it never reads.
	theme::CThemeService::SetActiveColorThemeLightKind(lightColorTheme);
	// Native popup menus are painted by darkmodelib rather than the workbench
	// palette. Synchronize that process-wide renderer with the resolved color
	// theme (including previews), then refresh the menu subclass before any menu
	// is opened. The legacy shared dark-mode preference remains only the fallback
	// used when no workbench.colorTheme can be resolved.
	const BOOL nativeDark = mode == theme::ThemeMode::Dark ? TRUE : FALSE;
	// darkmodelib caches popup-menu theme handles separately from the top-level
	// mode flag. Reapplying the resolved mode flushes that cache even when the
	// boolean did not change (for example after previewing Dark and accepting
	// Light), so every native menu follows the selected color theme.
	ApplyDarkModeSetting(nativeDark);
	DarkMode::setDarkTitleBarEx(GetHwnd(), true);
	DarkMode::setWindowMenuBarSubclass(GetHwnd());
	DarkMode::setChildCtrlsTheme(GetHwnd());
	::DrawMenuBar(GetHwnd());
	if (m_customFrame) m_customFrame->SetThemeMode(mode);
	const auto palette = theme::CThemeService::EffectivePalette(mode);
	m_cStatusBar.SetPalette(palette);
	if (m_commandPaletteOverlay) m_commandPaletteOverlay->SetPalette(palette);
	if (m_cTabWnd.GetHwnd()) m_cTabWnd.UpdateTheme();
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->SetPalette(palette);
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->SetPalette(palette);
	if (m_viewContainerPages) m_viewContainerPages->SetPalette(palette);
	if (m_sidebarHost) m_sidebarHost->SetPalette(palette);
	if (m_auxiliaryBarHost) m_auxiliaryBarHost->SetPalette(palette);
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->SetPalette(palette);
	if (m_bottomPanelTool) m_bottomPanelTool->SetPalette(palette);
	if (m_terminalTool) m_terminalTool->SetPalette(palette);
	if (m_markdownPreview) m_markdownPreview->SetPalette(palette);
	if (m_emptyEditorSurface) m_emptyEditorSurface->SetPalette(palette);
	if (m_diffSurface) m_diffSurface->SetPalette(palette);
	for (int index = 0; index < GetAllViewCount(); ++index) {
		GetView(index).UpdateWorkbenchTheme();
	}
	m_cMiniMapView.UpdateWorkbenchTheme();
	if (m_activityBar) {
		workbench::ActivityBarPalette activityPalette;
		activityPalette.background = palette.activityBar.ToColorRef();
		activityPalette.hoverBackground = palette.raised.ToColorRef();
		activityPalette.pressedBackground = palette.border.ToColorRef();
		activityPalette.selectedBackground = palette.panel.ToColorRef();
		activityPalette.activeIndicator = palette.accent.ToColorRef();
		activityPalette.icon = palette.secondaryText.ToColorRef();
		activityPalette.activeIcon = palette.primaryText.ToColorRef();
		activityPalette.disabledIcon = palette.secondaryText.ToColorRef();
		activityPalette.focusBorder = palette.accent.ToColorRef();
		activityPalette.border = palette.border.ToColorRef();
		// The badge is its own theme role; see theme/CLAUDE.md for why it is
		// neither `accent` nor one of the `button.*` roles.
		activityPalette.badgeBackground = palette.activityBarBadgeBackground.ToColorRef();
		activityPalette.badgeForeground = palette.activityBarBadgeForeground.ToColorRef();
		activityPalette.highContrast = theme::CThemeService::IsHighContrastActive();
		m_activityBar->SetPalette(activityPalette);
		if (m_auxiliaryActivityBar) m_auxiliaryActivityBar->SetPalette(activityPalette);
	}
	return true;
}

void CEditWnd::ApplyWorkbenchSettingsFromSharedData(bool finalizeProjection)
{
	const auto& settings = m_pShareData->m_Common.m_sWorkbench;
	auto applyPanel = [](workbench::CWorkbenchPanelHost* host, BOOL visible, int extentDip) {
		if (host == nullptr) return;
		host->ApplyExtentDip(extentDip);
		if (visible != FALSE) host->Show(); else host->Hide();
	};
	if (m_workbenchRuntime == nullptr) {
		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		applyPanel(m_leftWorkbenchPanel.get(), settings.m_bLeftPanelVisible, settings.m_nLeftPanelExtent96);
		applyPanel(m_rightWorkbenchPanel.get(), settings.m_bAuxiliaryBarVisible,
			settings.m_nAuxiliaryBarExtent96);
		applyPanel(m_bottomWorkbenchPanel.get(), settings.m_bBottomPanelVisible,
			settings.m_nBottomPanelExtent96);
		if (settings.m_bBottomPanelVisible == FALSE) m_bottomWorkbenchMaximized = false;
		if (m_viewContainerPages) {
			ApplySidebarPage(SidebarPageForLegacyTool(settings.m_eActiveTool));
			SetOutlineExpandedInHosts(settings.m_bRightPanelVisible != FALSE);
		}
	}
	if (finalizeProjection) FinalizeWorkbenchPanelProjection();
}

bool CEditWnd::ApplyInitialWorkbenchLayoutState()
{
	if (m_workbenchRuntime == nullptr) {
		FinalizeWorkbenchPanelProjection();
		return true;
	}
	return ApplyCurrentWorkbenchLayoutState(true, true);
}

bool CEditWnd::InitializePaneCompositeProjection()
{
	if (m_paneCompositeProjection != nullptr) return true;
	if (m_leftWorkbenchPanel == nullptr || m_bottomWorkbenchPanel == nullptr
		|| m_rightWorkbenchPanel == nullptr || m_sidebarHost == nullptr
		|| m_auxiliaryBarHost == nullptr || m_bottomPanelTool == nullptr
		|| m_viewContainerPages == nullptr) {
		return false;
	}
	using Location = workbench::layout::EWorkbenchViewContainerLocation;
	using Binding = workbench::win32::PaneCompositeHostBinding;
	const auto makeBinding = [this](const Location location) {
		Binding binding;
		binding.location = location;
		binding.supportsContainer = [this, location](const std::string_view containerId) {
			if (location == Location::Panel) {
				return m_bottomPanelTool != nullptr
					&& m_bottomPanelTool->SupportsContainer(containerId);
			}
			if (m_viewContainerPages == nullptr) return false;
			const auto pageLocation = location == Location::SideBar
				? workbench::layout::EViewContainerLocation::Sidebar
				: workbench::layout::EViewContainerLocation::AuxiliaryBar;
			return m_viewContainerPages->SupportsLocation(containerId, pageLocation);
		};
		binding.canApply = [this](const workbench::win32::PaneCompositeHostState& state) {
			return CanApplyPaneCompositeHostState(state);
		};
		binding.readState = [this, location] {
			return ReadPaneCompositeHostState(location);
		};
		binding.applyState = [this](const workbench::win32::PaneCompositeHostState& state) {
			return ApplyPaneCompositeHostState(state);
		};
		binding.closeProjection = [this, location] {
			return ClosePaneCompositeHostProjection(location);
		};
		return binding;
	};
	try {
		m_paneCompositeProjection =
			std::make_unique<workbench::win32::PaneCompositeProjectionService>(
				std::array<Binding, workbench::win32::kPaneCompositeHostCount>{
					makeBinding(Location::SideBar),
					makeBinding(Location::Panel),
					makeBinding(Location::AuxiliaryBar),
				});
		return true;
	} catch (...) {
		m_paneCompositeProjection.reset();
		return false;
	}
}

std::optional<workbench::win32::PaneCompositeHostState>
CEditWnd::ReadPaneCompositeHostState(
	const workbench::layout::EWorkbenchViewContainerLocation location) const noexcept
{
	using Location = workbench::layout::EWorkbenchViewContainerLocation;
	workbench::CWorkbenchPanelHost* part = nullptr;
	workbench::viewcontainer::CViewContainerHost* sideBar = nullptr;
	std::string_view partId;
	switch (location) {
	case Location::SideBar:
		part = m_leftWorkbenchPanel.get();
		sideBar = m_sidebarHost;
		partId = workbench::layout::ids::part::Sidebar;
		break;
	case Location::Panel:
		part = m_bottomWorkbenchPanel.get();
		partId = workbench::layout::ids::part::Panel;
		break;
	case Location::AuxiliaryBar:
		part = m_rightWorkbenchPanel.get();
		sideBar = m_auxiliaryBarHost;
		partId = workbench::layout::ids::part::Auxiliarybar;
		break;
	}
	if (part == nullptr) return std::nullopt;
	workbench::win32::PaneCompositeHostState state;
	state.location = location;
	try {
		state.partId = partId;
		state.visible = part->GetState() != workbench::WorkbenchPanelState::Hidden;
		if (part->GetExtentDip() > 0) {
			state.committedExtentDip = static_cast<std::uint32_t>(part->GetExtentDip());
		}
		if (location == Location::Panel) {
			if (m_bottomPanelTool == nullptr) return std::nullopt;
			const auto active = m_bottomPanelTool->ActiveContainerId();
			if (!active.empty()) state.activeContainerId = std::string(active);
			if (const auto attached = m_bottomPanelTool->AttachedContainerId()) {
				state.attachedContainerId = std::string(*attached);
			}
		} else {
			if (sideBar == nullptr || m_viewContainerPages == nullptr) return std::nullopt;
			const auto active = sideBar->ActivePage();
			if (!active.empty()) {
				state.activeContainerId = std::string(active);
				if (m_viewContainerPages->AttachedHost(active) == sideBar->GetHwnd()) {
					state.attachedContainerId = std::string(active);
				}
			}
		}
		return state;
	} catch (...) {
		return std::nullopt;
	}
}

bool CEditWnd::CanApplyPaneCompositeHostState(
	const workbench::win32::PaneCompositeHostState& state) const noexcept
{
	using Location = workbench::layout::EWorkbenchViewContainerLocation;
	if (state.committedExtentDip
		&& (*state.committedExtentDip == 0
			|| *state.committedExtentDip
				> workbench::layout::kMaximumWorkbenchLayoutCommittedExtentDip)) {
		return false;
	}
	const auto expectedPart = state.location == Location::SideBar
		? workbench::layout::ids::part::Sidebar
		: state.location == Location::Panel
			? workbench::layout::ids::part::Panel
			: workbench::layout::ids::part::Auxiliarybar;
	if (state.partId != expectedPart) return false;
	if (state.attachedContainerId && state.activeContainerId != state.attachedContainerId) {
		return false;
	}
	if (state.location == Location::Panel) {
		return m_bottomWorkbenchPanel != nullptr && m_bottomPanelTool != nullptr
			&& state.activeContainerId
			&& m_bottomPanelTool->SupportsContainer(*state.activeContainerId);
	}
	if (m_viewContainerPages == nullptr
		|| (state.location == Location::SideBar && (m_leftWorkbenchPanel == nullptr
			|| m_sidebarHost == nullptr))
		|| (state.location == Location::AuxiliaryBar && (m_rightWorkbenchPanel == nullptr
			|| m_auxiliaryBarHost == nullptr))) {
		return false;
	}
	if (!state.activeContainerId) return !state.attachedContainerId;
	const auto pageLocation = state.location == Location::SideBar
		? workbench::layout::EViewContainerLocation::Sidebar
		: workbench::layout::EViewContainerLocation::AuxiliaryBar;
	return m_viewContainerPages->SupportsLocation(*state.activeContainerId, pageLocation);
}

workbench::win32::EPaneCompositeHostApplyStatus CEditWnd::ApplyPaneCompositeHostState(
	const workbench::win32::PaneCompositeHostState& state) noexcept
{
	using ApplyStatus = workbench::win32::EPaneCompositeHostApplyStatus;
	using Location = workbench::layout::EWorkbenchViewContainerLocation;
	if (!CanApplyPaneCompositeHostState(state)) return ApplyStatus::Failed;
	try {
		workbench::CWorkbenchPanelHost* part = nullptr;
		if (state.location == Location::Panel) {
			part = m_bottomWorkbenchPanel.get();
			if (!m_bottomPanelTool->ApplyActiveContainer(*state.activeContainerId)) {
				return ApplyStatus::Failed;
			}
			if (state.attachedContainerId) {
				const auto attached = m_bottomPanelTool->AttachActivePage();
				if (attached == workbench::panel::EBottomPanelPageAttachStatus::Closed
					|| attached == workbench::panel::EBottomPanelPageAttachStatus::Failed) {
					return ApplyStatus::Failed;
				}
			} else {
				const auto detached = m_bottomPanelTool->DetachActivePage();
				if (detached == workbench::panel::EBottomPanelPageDetachStatus::Closed
					|| detached == workbench::panel::EBottomPanelPageDetachStatus::Failed) {
					return ApplyStatus::Failed;
				}
			}
		} else {
			auto* host = state.location == Location::SideBar
				? m_sidebarHost : m_auxiliaryBarHost;
			part = state.location == Location::SideBar
				? m_leftWorkbenchPanel.get() : m_rightWorkbenchPanel.get();
			if (host == nullptr || part == nullptr) return ApplyStatus::Failed;
			const auto pageStatus = host->ShowPage(
				state.attachedContainerId ? std::string_view(*state.attachedContainerId)
					: std::string_view{});
			if (pageStatus != workbench::viewcontainer::EViewContainerHostPageStatus::Applied
				&& pageStatus != workbench::viewcontainer::EViewContainerHostPageStatus::AlreadyApplied
				&& pageStatus != workbench::viewcontainer::EViewContainerHostPageStatus::Cleared) {
				return ApplyStatus::Failed;
			}
		}
		if (part == nullptr) return ApplyStatus::Failed;
		if (state.committedExtentDip) {
			part->ApplyExtentDip(static_cast<int>(*state.committedExtentDip));
		}
		if (state.visible) part->Show(); else part->Hide();
		return ApplyStatus::Applied;
	} catch (...) {
		return ApplyStatus::Failed;
	}
}

bool CEditWnd::ClosePaneCompositeHostProjection(
	const workbench::layout::EWorkbenchViewContainerLocation location) noexcept
{
	using Location = workbench::layout::EWorkbenchViewContainerLocation;
	try {
		if (location == Location::Panel) {
			if (m_bottomPanelTool == nullptr) return true;
			const auto status = m_bottomPanelTool->DetachActivePage();
			return status == workbench::panel::EBottomPanelPageDetachStatus::Detached
				|| status == workbench::panel::EBottomPanelPageDetachStatus::AlreadyDetached;
		}
		auto* host = location == Location::SideBar ? m_sidebarHost : m_auxiliaryBarHost;
		if (host == nullptr) return true;
		const auto status = host->ShowPage({});
		return status == workbench::viewcontainer::EViewContainerHostPageStatus::Cleared
			|| status == workbench::viewcontainer::EViewContainerHostPageStatus::AlreadyApplied;
	} catch (...) {
		return false;
	}
}

bool CEditWnd::ApplyCurrentWorkbenchLayoutState(bool finalizeProjection,
	bool broadcastMirrorChanges, bool* mirrorChanged)
{
	if (mirrorChanged != nullptr) *mirrorChanged = false;
	if (m_workbenchRuntime == nullptr || m_leftWorkbenchPanel == nullptr
		|| m_bottomWorkbenchPanel == nullptr || m_rightWorkbenchPanel == nullptr) {
		return false;
	}

	workbench::layout::WorkbenchLayoutStateSnapshot snapshot;
	try {
		snapshot = m_workbenchRuntime->LayoutState().Snapshot();
	}
	catch (...) {
		return false;
	}
	if (m_paneCompositeProjection == nullptr) return false;
	const auto prepared = m_paneCompositeProjection->Prepare(snapshot);
	if (!prepared.Succeeded()) return false;
	if (m_workbenchContextKeyService != nullptr) {
		const bool recentlyOpenedAvailable = HasRecentlyOpenedItems();
		const auto contextResult = m_workbenchContextKeyService->SetCoreProjection(
			snapshot, m_workbenchRuntime->WorkspaceContext().Snapshot(), BuildWorkbenchEditorCommandContext(),
			recentlyOpenedAvailable, BuildWorkbenchScmCommandContext(), BuildWorkbenchUpdateCommandContext());
		if (!contextResult.Succeeded()
			&& contextResult.status != workbench::commands::EWorkbenchContextMutationStatus::NotApplicable) {
			(void)m_paneCompositeProjection->Cancel(*prepared.token);
			return false;
		}
	}
	const auto committed = m_paneCompositeProjection->Commit(*prepared.token);
	if (!committed.Succeeded()) return false;
	const auto* projection = m_paneCompositeProjection->LastCommittedProjection();
	if (projection == nullptr) return false;

	const auto outline = std::ranges::find(snapshot.views,
		workbench::layout::ids::view::Outline, &workbench::layout::WorkbenchViewState::viewId);
	if (outline != snapshot.views.end()) SetOutlineExpandedInHosts(outline->visible);
	SyncViewContainers(&snapshot);
	RefreshSidebarTitles();
	if (!projection->hosts[1].visible) m_bottomWorkbenchMaximized = false;
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	bool changed = false;
	const auto updateVisible = [&changed](BOOL& destination, bool visible) {
		const BOOL value = visible ? TRUE : FALSE;
		if (destination == value) return;
		destination = value;
		changed = true;
	};
	const auto updateExtent = [&changed](int& destination, int extentDip) {
		if (destination == extentDip) return;
		destination = extentDip;
		changed = true;
	};
	updateVisible(settings.m_bLeftPanelVisible, projection->hosts[0].visible);
	updateExtent(settings.m_nLeftPanelExtent96, m_leftWorkbenchPanel->GetExtentDip());
	updateVisible(settings.m_bBottomPanelVisible, projection->hosts[1].visible);
	updateExtent(settings.m_nBottomPanelExtent96, m_bottomWorkbenchPanel->GetExtentDip());
	updateVisible(settings.m_bAuxiliaryBarVisible, projection->hosts[2].visible);
	updateExtent(settings.m_nAuxiliaryBarExtent96, m_rightWorkbenchPanel->GetExtentDip());
	if (mirrorChanged != nullptr) *mirrorChanged = changed;
	std::string_view activeActivityContainer;
	if (projection->hosts[0].visible && projection->hosts[0].activeContainerId) {
		activeActivityContainer = *projection->hosts[0].activeContainerId;
	}
	if (m_activityBar) m_activityBar->SetSelectedItem(activeActivityContainer);
	if (finalizeProjection) FinalizeWorkbenchPanelProjection();
	if (finalizeProjection && projection->focus) {
		ApplyPaneCompositeFocus(*projection->focus);
		// Focus is intentionally applied only after final native bounds exist, but
		// WM_SETFOCUS can invalidate button, caret, and selection pixels after the
		// geometry commit has already painted. Publish that terminal visual state
		// before returning the atomic projection to its caller.
		RedrawWorkbenchFrameForCommittedLayout(true);
	}
	if (changed && broadcastMirrorChanges) BroadcastWorkbenchSettings();
	return true;
}

void CEditWnd::ApplyPaneCompositeFocus(
	const workbench::win32::PaneCompositeFocusProjection& focus)
{
	using Location = workbench::layout::EWorkbenchViewContainerLocation;
	if (focus.partId == workbench::layout::ids::part::Editor) {
		if (m_emptyEditorSurface != nullptr && m_emptyEditorSurface->IsVisible()) {
			m_emptyEditorSurface->Focus();
		} else if (HasActiveEditorInput() && GetActiveView().GetHwnd() != nullptr
			&& ::IsWindowVisible(GetActiveView().GetHwnd())) {
			::SetFocus(GetActiveView().GetHwnd());
		}
		return;
	}
	if (!focus.location) return;
	if (*focus.location == Location::Panel) {
		if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->ActivateTool();
		return;
	}
	if (focus.viewId == workbench::layout::ids::view::Outline
		&& focus.containerId == workbench::layout::ids::viewContainer::Explorer) {
		if (auto* explorerHost = HostShowingPage(*focus.containerId)) {
			explorerHost->FocusOutline();
		}
		return;
	}
	if (focus.containerId) {
		if (auto* host = PanelHostFor(HostShowingPage(*focus.containerId))) {
			host->ActivateTool();
		}
		return;
	}
	auto* host = *focus.location == Location::SideBar
		? m_leftWorkbenchPanel.get() : m_rightWorkbenchPanel.get();
	if (host != nullptr) host->ActivateTool();
}

void CEditWnd::OnWorkbenchLayoutStateChanged()
{
	if (!m_layoutStateSubscription) return;
	if (m_resizingWorkbenchPanel != nullptr) {
		// RedrawWindow(RDW_UPDATENOW) can dispatch an older posted notification
		// inside the active pointer sample. Cancelling here revives the previous
		// committed extent and turns mouse-up into an unrelated window drag. Keep
		// the gesture authoritative and project the newest snapshot at its terminal
		// commit/cancel path instead.
		m_workbenchLayoutProjectionDeferred = true;
		return;
	}
	if (m_resizingMarkdownPreview) CancelMarkdownPreviewResize();
	if (!ApplyCurrentWorkbenchLayoutState(true, true)) {
		::OutputDebugStringW(L"Sakura Editor NEXT: committed workbench layout projection failed.\n");
	}
}

bool CEditWnd::InitializeWorkbenchServiceProjection()
{
	if (m_workbenchRuntime == nullptr || m_bottomPanelTool == nullptr
		|| m_workbenchServiceProjectionGate != nullptr || m_markerSubscriptionId || m_outputSubscriptionId) {
		return false;
	}

	m_markerService = m_workbenchRuntime->Markers();
	m_outputService = m_workbenchRuntime->Output();
	if (m_markerService == nullptr || m_outputService == nullptr) {
		m_markerService = nullptr;
		m_outputService = nullptr;
		return false;
	}

	try {
		auto gate = std::make_shared<WorkbenchServiceProjectionGate>();
		gate->window = GetHwnd();
		if (gate->window == nullptr) return false;

		const auto markerSubscription = m_markerService->Subscribe(
			[gate](const workbench::problems::MarkerChange&) {
				WorkbenchServiceProjectionGate::Notify(gate, false);
			});
		if (markerSubscription.status != workbench::problems::EMarkerSubscriptionStatus::Subscribed
			|| !markerSubscription.subscriptionId) {
			m_markerService = nullptr;
			m_outputService = nullptr;
			return false;
		}
		// Publish the gate and marker ID before the second subscription. Every
		// subsequent failure can therefore take the single close terminal.
		m_workbenchServiceProjectionGate = gate;
		m_markerSubscriptionId = *markerSubscription.subscriptionId;

		const auto outputSubscription = m_outputService->Subscribe(
			[gate](const workbench::output::OutputServiceChange& change) {
				WorkbenchServiceProjectionGate::Notify(gate,
					change.kind == workbench::output::EOutputChangeKind::ChannelShown);
			});
		if (!outputSubscription) {
			CloseWorkbenchServiceProjection();
			return false;
		}

		m_outputSubscriptionId = *outputSubscription;
		OnWorkbenchServiceProjectionChanged();
		return true;
	}
	catch (...) {
		CloseWorkbenchServiceProjection();
		return false;
	}
}

void CEditWnd::CloseWorkbenchServiceProjection() noexcept
{
	const auto gate = std::move(m_workbenchServiceProjectionGate);
	if (gate) {
		std::lock_guard lock(gate->mutex);
		gate->connected = false;
		gate->window = nullptr;
		gate->messageQueued = false;
		gate->outputRevealPending = false;
	}
	if (m_markerService != nullptr && m_markerSubscriptionId) {
		m_markerService->Unsubscribe(*m_markerSubscriptionId);
	}
	m_markerSubscriptionId.reset();
	if (m_outputService != nullptr && m_outputSubscriptionId) {
		m_outputService->Unsubscribe(*m_outputSubscriptionId);
	}
	m_outputSubscriptionId.reset();
	m_markerService = nullptr;
	m_outputService = nullptr;
}

namespace {

//! Upstream contributes exactly one `7_update` item per state, and contributes
//! nothing at all for `uninitialized`, `disabled`, `overwriting`, and
//! `restarting`. Those four therefore map to `None` rather than to whichever
//! neighbouring label looks closest.
[[nodiscard]] CustomFrameUpdateMenuEntry UpdateMenuEntryFor(update::EUpdateStateType state) noexcept
{
	switch (state) {
	case update::EUpdateStateType::Idle:                 return CustomFrameUpdateMenuEntry::Check;
	case update::EUpdateStateType::CheckingForUpdates:   return CustomFrameUpdateMenuEntry::Checking;
	case update::EUpdateStateType::AvailableForDownload: return CustomFrameUpdateMenuEntry::DownloadNow;
	case update::EUpdateStateType::Downloading:          return CustomFrameUpdateMenuEntry::Downloading;
	case update::EUpdateStateType::Downloaded:           return CustomFrameUpdateMenuEntry::Install;
	case update::EUpdateStateType::Updating:             return CustomFrameUpdateMenuEntry::Updating;
	case update::EUpdateStateType::Cancelling:           return CustomFrameUpdateMenuEntry::Cancelling;
	case update::EUpdateStateType::Ready:                return CustomFrameUpdateMenuEntry::Restart;
	case update::EUpdateStateType::Uninitialized:
	case update::EUpdateStateType::Disabled:
	case update::EUpdateStateType::Overwriting:
	case update::EUpdateStateType::Restarting:
		break;
	}
	return CustomFrameUpdateMenuEntry::None;
}

} // namespace

void CEditWnd::InitializeUpdateProjection() noexcept
{
	if (m_workbenchRuntime == nullptr || m_updateComposition || m_updateStateGate) return;
	const HWND window = GetHwnd();
	if (window == nullptr) return;

	try {
		// Same profile authority every other profile-scoped service in this window
		// uses. See `config/CLAUDE.md`: the control authority id would read as a
		// valid target and silently return descriptor defaults.
		auto result = update::CreateUpdateComposition(
			m_workbenchRuntime->Configuration(),
			m_workbenchRuntime->Bootstrap().UserDataProfile().SelectedProfileId());
		if (!result) {
			// A machine with no writable staging root, or an installation whose
			// network policy cannot be read, simply has no update surfaces. That is
			// an absence, not a startup failure, and the window must still open.
			return;
		}

		auto gate = std::make_shared<UpdateStateGate>();
		gate->window = window;
		const auto subscription = result.composition->Service().Subscribe(
			[gate](const update::UpdateState&) { UpdateStateGate::Notify(gate); });
		if (!subscription) {
			result.composition->Shutdown();
			return;
		}

		m_updateComposition = std::move(result.composition);
		m_updateStateGate = gate;
		m_updateSubscriptionId = *subscription;
		OnWorkbenchUpdateStateChanged();
	}
	catch (...) {
		CloseUpdateProjection();
	}
}

void CEditWnd::CloseUpdateProjection() noexcept
{
	const auto gate = std::move(m_updateStateGate);
	if (gate) {
		std::lock_guard lock(gate->mutex);
		gate->connected = false;
		gate->window = nullptr;
		gate->messageQueued = false;
	}
	if (m_updateComposition && m_updateSubscriptionId) {
		m_updateComposition->Service().Unsubscribe(*m_updateSubscriptionId);
	}
	m_updateSubscriptionId.reset();
	// Stop the worker before releasing the stack: a posted download still holds a
	// weak reference to the service, and joining here is what makes the window's
	// teardown order observable rather than incidental.
	if (m_updateComposition) m_updateComposition->Shutdown();
	m_updateComposition.reset();
	m_updateStateId = "uninitialized";
	if (m_customFrame) {
		m_customFrame->SetUpdateIndicatorCallback({});
		m_customFrame->SetUpdateIndicatorVisible(false);
		m_customFrame->SetUpdateMenuEntry(CustomFrameUpdateMenuEntry::None);
	}
}

void CEditWnd::OnWorkbenchUpdateStateChanged()
{
	if (m_updateStateGate) {
		std::lock_guard lock(m_updateStateGate->mutex);
		m_updateStateGate->messageQueued = false;
	}
	if (!m_updateComposition) return;

	const auto state = m_updateComposition->Service().State();
	m_updateStateId = std::string(update::UpdateStateTypeId(state.type));
	// The context projection is refreshed before the surfaces are, so a click
	// arriving in the same message batch is evaluated against the state the
	// button is about to show rather than the one it is leaving.
	(void)RefreshWorkbenchCommandContext();

	if (!m_customFrame) return;
	const bool actionable = update::IsActionableUpdateState(state.type)
		&& m_updateComposition->Configuration().titleBar;
	m_customFrame->SetUpdateIndicatorVisible(actionable);
	m_customFrame->SetUpdateMenuEntry(UpdateMenuEntryFor(state.type));
	m_customFrame->InvalidateTitle();
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteUpdateCommand(EUpdateCommand command)
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;
	using workbench::commands::WorkbenchCommandExecutionResult;

	if (!m_updateComposition) {
		return { EWorkbenchCommandExecutionStatus::Unsupported,
			"This installation has no update stack: it could not resolve a staging location or an update policy." };
	}

	auto& service = m_updateComposition->Service();
	switch (command) {
	case EUpdateCommand::CheckForUpdates:
		// `true`: this path is only ever reached from a user gesture. The periodic
		// poll calls the service directly and is what `update.mode` = `manual`
		// suppresses.
		service.CheckForUpdates(true);
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	case EUpdateCommand::DownloadUpdate:
		service.DownloadUpdate();
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	case EUpdateCommand::ApplyUpdate:
		service.ApplyUpdate();
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	case EUpdateCommand::QuitAndInstall:
		return ExecuteUpdateQuitAndInstall();
	case EUpdateCommand::ShowUpdateInfo: {
		const auto state = service.State();
		if (!state.update || state.update->releaseUrl.empty()) {
			return { EWorkbenchCommandExecutionStatus::NotApplicable,
				"No update is currently known, so there is no release to show." };
		}
		if (!m_updateComposition->Launcher().OpenReleasePage(state.update->releaseUrl)) {
			return { EWorkbenchCommandExecutionStatus::Failed, "The release page could not be opened." };
		}
		return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	}
	}
	return { EWorkbenchCommandExecutionStatus::Failed, "Unknown update command." };
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteUpdateQuitAndInstall()
{
	using workbench::commands::EWorkbenchCommandExecutionStatus;

	// Arm first, then quit: the last process to exit reads the armed manifest, so
	// arming after the quit has begun would race the very exit it is meant to
	// describe.
	m_updateComposition->Service().QuitAndInstall();

	// The quit runs synchronously and may destroy this window before it returns,
	// so nothing below may touch `this` unconditionally. The gate is the one piece
	// of this object's state that outlives it: `CloseUpdateProjection` clears
	// `connected` during teardown, so a still-connected gate afterwards is proof
	// that the window survived — which can only mean the user cancelled the quit.
	const std::weak_ptr<UpdateStateGate> weakGate = m_updateStateGate;
	GetDocument()->HandleCommand(F_EXITALL);

	const auto gate = weakGate.lock();
	if (!gate) return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
	bool survived = false;
	{
		std::lock_guard lock(gate->mutex);
		survived = gate->connected;
	}
	if (!survived) return { EWorkbenchCommandExecutionStatus::Succeeded, {} };

	// Upstream's `update.restart` leaves the update staged but unarmed when the
	// quit does not happen, so the next `Ready` gesture starts from the same place
	// rather than silently updating at some unrelated later exit.
	m_updateComposition->Service().AbortQuitAndInstall();
	return { EWorkbenchCommandExecutionStatus::NotApplicable, "The restart was cancelled." };
}

workbench::commands::WorkbenchUpdateCommandContext CEditWnd::BuildWorkbenchUpdateCommandContext() const
{
	return workbench::commands::WorkbenchUpdateCommandContext{ m_updateStateId };
}

void CEditWnd::OnWorkbenchServiceProjectionChanged()
{
	if (m_bottomPanelTool == nullptr || m_markerService == nullptr || m_outputService == nullptr
		|| !m_workbenchServiceProjectionGate) {
		return;
	}

	bool revealOutput = false;
	{
		std::lock_guard lock(m_workbenchServiceProjectionGate->mutex);
		if (!m_workbenchServiceProjectionGate->connected) return;
		revealOutput = m_workbenchServiceProjectionGate->outputRevealPending;
		m_workbenchServiceProjectionGate->outputRevealPending = false;
		m_workbenchServiceProjectionGate->messageQueued = false;
	}

	try {
		const auto problems = workbench::win32::ProjectProblemsPanel(m_markerService->Snapshot());
		const auto output = workbench::win32::ProjectOutputPanel(m_outputService->Snapshot());
		m_bottomPanelTool->SetProblemsSnapshot(problems);
		m_bottomPanelTool->SetOutputSnapshot(output);

		if (!revealOutput || !output.activeChannelId) return;
		const auto active = std::ranges::find(output.channels, *output.activeChannelId,
			&workbench::win32::OutputPanelChannel::channelId);
		if (active == output.channels.end() || !active->visible) return;
		if (!ExecuteShowOutputCommand(!active->lastShowPreservedFocus)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: Output service reveal projection failed.\n");
		}
	}
	catch (...) {
		::OutputDebugStringW(L"Sakura Editor NEXT: Problems/Output service projection failed.\n");
	}
}

void CEditWnd::FinalizeWorkbenchPanelProjection()
{
	const auto& settings = m_pShareData->m_Common.m_sWorkbench;
	const bool leftVisible = m_leftWorkbenchPanel != nullptr
		&& m_leftWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;
	const bool rightVisible = m_rightWorkbenchPanel != nullptr
		&& m_rightWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;
	const bool bottomVisible = m_bottomWorkbenchPanel != nullptr
		&& m_bottomWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;

	if (m_workbenchRuntime == nullptr) {
		using workbench::layout::ids::viewContainer::Explorer;
		using workbench::layout::ids::viewContainer::SourceControl;
		std::string_view activeContainer;
		switch (settings.m_eActiveTool) {
		case WORKBENCH_TOOL_EXPLORER:
			if (leftVisible) activeContainer = Explorer;
			break;
		case WORKBENCH_TOOL_OUTLINE:
			// Outline now lives inside Explorer.
			if (leftVisible) activeContainer = Explorer;
			break;
		case WORKBENCH_TOOL_TERMINAL:
			// Terminal is reached from the bottom panel/title-bar controls rather
			// than occupying a dedicated Activity Bar button.
			break;
		case WORKBENCH_TOOL_SCM:
			if (leftVisible) activeContainer = SourceControl;
			break;
		}
		// Secondary Side Bar visibility owns no Activity Bar item; every Activity Bar
		// ViewContainer lives in the Primary Side Bar, exactly like VS Code.
		(void)rightVisible;
		if (m_activityBar) m_activityBar->SetSelectedItem(activeContainer);
	}
	(void)ApplyWorkbenchTheme();
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
		RedrawWorkbenchFrameForCommittedLayout(false);
	}
	const bool terminalSelected = m_workbenchRuntime == nullptr
		? settings.m_eActiveTool == WORKBENCH_TOOL_TERMINAL
		: m_bottomPanelTool != nullptr
			&& m_bottomPanelTool->ActiveContainerId()
				== workbench::layout::ids::viewContainer::Terminal;
	if (bottomVisible && terminalSelected && m_terminalTool != nullptr) {
		// Restoring a visible terminal must produce its first prompt without the
		// user pressing '+'. Keep focus where startup/shared-setting propagation
		// found it; explicit user activation remains the focus-owning path.
		(void)m_terminalTool->EnsureSessionStarted();
	}
	const bool outlineVisible = m_workbenchRuntime == nullptr
		? settings.m_bRightPanelVisible != FALSE
		: IsOutlineViewExpanded();
	if (leftVisible && outlineVisible && m_outlineWorkbenchTool != nullptr
		&& m_cDlgFuncList.GetHwnd() == nullptr && m_dispatchReady) {
		ReloadWorkbenchOutlineAndRelayout();
	}
}

bool CEditWnd::InitializeFrameRuntime() noexcept
{
	if (m_frameRuntimeState != nullptr) return true;
	if (m_viewContainerPages == nullptr || GetHwnd() == nullptr) return false;
	try {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		workbench::rendering::FrameCoordinatorRuntimeOptions options;
		options.presentationOwner.targetWindow = nullptr;
		options.presentationOwner.width = static_cast<std::uint32_t>(
			std::max<LONG>(1, client.right - client.left));
		options.presentationOwner.height = static_cast<std::uint32_t>(
			std::max<LONG>(1, client.bottom - client.top));
		auto state = std::make_unique<CEditWndFrameRuntimeState>();
		options.cadence = state->cadenceSource.Observe(GetHwnd()).input;
		auto lease = workbench::rendering::FrameRuntimeRetirementLease::TryCreate(options);
		if (lease == nullptr) return false;
		state->leaseHolder = std::make_shared<CEditWndFrameRuntimeLeaseHolder>(std::move(lease));
		auto* const runtime = state->leaseHolder->RuntimePointer();
		if (runtime == nullptr) return false;
		state->callbackState = std::make_shared<CEditWndFrameRuntimeCallbackState>();
		state->callbackState->Bind(state->leaseHolder);
		const auto callbackState = state->callbackState;
		state->terminalFrameBridge = std::make_shared<terminal::TerminalNativeFrameBridge>(
			[callbackState](const workbench::rendering::FrameNativeSurfaceRegistration& registration) {
				if (callbackState != nullptr) (void)callbackState->Register(registration);
			},
			[callbackState](const workbench::rendering::FrameNativeSurfaceRegistration& registration) {
				if (callbackState != nullptr) (void)callbackState->Update(registration);
			},
			[callbackState](const workbench::rendering::FrameSurfaceId surfaceId,
				const std::uint64_t surfaceLifetimeEpoch) {
				if (callbackState != nullptr) callbackState->CloseSurface(
					surfaceId, surfaceLifetimeEpoch);
			},
			[callbackState](std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame> frame) {
				if (callbackState != nullptr) callbackState->Submit(std::move(frame));
			});
		state->callbackState->SetDisplayEpoch(options.cadence.displayEpoch);
		state->terminalFrameBridge->SetDisplayEpoch(options.cadence.displayEpoch);
		state->windowTransaction =
			std::make_unique<workbench::rendering::FrameWindowTransaction>(16);
		using WindowRole = workbench::rendering::EFrameWindowSurfaceRole;
		const std::array<workbench::rendering::FrameWindowSurfaceSpec, 10> windowSurfaces{
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::ActivityBar),
				"workbench.parts.activitybar", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::PrimarySideBar),
				"workbench.parts.sidebar", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::SecondarySideBar),
				"workbench.parts.auxiliarybar", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::Panel),
				"workbench.parts.panel", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::TitleAndMenu),
				"workbench.parts.titlebar", true},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::Tabs),
				"workbench.parts.editor", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::StatusBar),
				"workbench.parts.statusbar", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::Editor),
				"workbench.parts.editor", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::MarkdownPreview),
				"workbench.parts.editor", false},
			workbench::rendering::FrameWindowSurfaceSpec{
				workbench::rendering::FrameWindowSurfaceId(WindowRole::Terminal),
				"workbench.parts.panel", false},
		};
		for (const auto& spec : windowSurfaces) {
			if (!state->windowTransaction->OpenSurface(spec).Accepted()) return false;
		}
		for (const auto& projection : m_viewContainerPages->FrameSurfaceProjections()) {
			const auto& surface = projection.surface;
			const auto result = state->leaseHolder->RuntimePointer()->RegisterPresentedSurface({
				.surfaceId = surface.surfaceId,
				.surfaceLifetimeEpoch = surface.surfaceLifetimeEpoch,
				.deviceEpoch = surface.deviceEpoch,
				.layoutEpoch = surface.layoutEpoch,
				.width = projection.width,
				.height = projection.height,
				.visible = surface.visible,
			});
			if (!result.Accepted()) return false;
		}
		for (const auto& surface : state->windowTransaction->Snapshots()) {
			const auto result = state->leaseHolder->RuntimePointer()->RegisterPresentedSurface({
				.surfaceId = surface.surfaceId,
				.surfaceLifetimeEpoch = surface.surfaceLifetimeEpoch,
				.deviceEpoch = surface.deviceEpoch,
				.layoutEpoch = surface.layoutEpoch,
				.width = static_cast<std::uint32_t>(
					std::max<LONG>(1, client.right - client.left)),
				.height = static_cast<std::uint32_t>(
					std::max<LONG>(1, client.bottom - client.top)),
				.visible = surface.visible,
			});
			if (!result.Accepted()) return false;
		}
		if (!state->windowTransaction->BeginLayout().Accepted()) return false;
		m_frameRuntimeState = std::move(state);
		if (m_scmTool != nullptr && m_frameRuntimeState->callbackState != nullptr) {
			const auto scmCallbacks = m_frameRuntimeState->callbackState;
			m_scmTool->SetNativeSurfaceSink({
				.registerSurface = [scmCallbacks](
					const workbench::scm::ScmNativeSurfaceRegistration& registration) {
					return scmCallbacks != nullptr && scmCallbacks->RegisterScm(registration);
				},
				.updateSurface = [scmCallbacks](
					const workbench::scm::ScmNativeSurfaceRegistration& registration) {
					return scmCallbacks != nullptr && scmCallbacks->UpdateScm(registration);
				},
				.closeSurface = [scmCallbacks](
					const workbench::rendering::FrameSurfaceId surfaceId,
					const std::uint64_t surfaceLifetimeEpoch) {
					if (scmCallbacks != nullptr) scmCallbacks->CloseScm(
						surfaceId, surfaceLifetimeEpoch);
				},
				.submitFrame = [scmCallbacks](
					std::shared_ptr<const workbench::scm::ScmNativeSurfaceFrame> frame) {
					return scmCallbacks != nullptr && scmCallbacks->SubmitScm(std::move(frame));
				},
			});
		}
		UpdateFrameRuntimeNativeSurfaces();
		if (m_terminalTool != nullptr && m_frameRuntimeState->terminalFrameBridge != nullptr) {
			m_terminalTool->SetNativeFrameRuntimeBridge(
				m_frameRuntimeState->terminalFrameBridge);
		}
		return true;
	} catch (...) {
		return false;
	}
}

void CEditWnd::UpdateFrameRuntimeNativeSurfaces() noexcept
{
	if (m_frameRuntimeState == nullptr || m_frameRuntimeState->callbackState == nullptr
		|| m_frameRuntimeState->leaseHolder == nullptr) {
		return;
	}
	const auto* const runtime = m_frameRuntimeState->leaseHolder->RuntimePointer();
	if (runtime == nullptr) return;
	// The owner is the only writer of presentationDeviceEpoch. Snapshot is a
	// bounded observation; only an epoch transition changes native targets.
	const auto runtimeSnapshot = runtime->Snapshot();
	const bool deviceEpochChanged = runtimeSnapshot.presentationDeviceEpoch != 0
		&& runtimeSnapshot.presentationDeviceEpoch != m_frameRuntimeState->scmDeviceEpoch;
	if (deviceEpochChanged) {
		m_frameRuntimeState->scmDeviceEpoch = runtimeSnapshot.presentationDeviceEpoch;
		if (m_terminalTool != nullptr) {
			m_terminalTool->NotifyFrameDeviceEpoch(runtimeSnapshot.presentationDeviceEpoch);
		}
	}
	const auto deviceEpoch = std::max<std::uint64_t>(1, m_frameRuntimeState->scmDeviceEpoch);
	const bool minimized = GetHwnd() != nullptr && ::IsIconic(GetHwnd()) != FALSE;

	if (m_scmTool != nullptr) {
		const HWND targetWindow = m_scmTool->GetHwnd();
		const bool visible = targetWindow != nullptr && ::IsWindow(targetWindow)
			&& ::IsWindowVisible(targetWindow) != FALSE && !minimized;
		const bool targetChanged = !m_frameRuntimeState->scmTargetInitialized
			|| targetWindow != m_frameRuntimeState->scmTargetWindow
			|| visible != m_frameRuntimeState->scmTargetVisible
			|| m_frameRuntimeState->scmTargetX != 0
			|| m_frameRuntimeState->scmTargetY != 0
			|| deviceEpoch != m_frameRuntimeState->scmTargetDeviceEpoch;
		if (targetChanged) {
			if (m_frameRuntimeState->scmLayoutEpoch
				!= (std::numeric_limits<std::uint64_t>::max)()) {
				++m_frameRuntimeState->scmLayoutEpoch;
			}
			const workbench::scm::ScmNativeSurfaceTarget target{
				.surfaceId = kScmNativeSurfaceId,
				.surfaceLifetimeEpoch = m_frameRuntimeState->scmSurfaceLifetimeEpoch,
				.deviceEpoch = deviceEpoch,
				.layoutEpoch = std::max<std::uint64_t>(1, m_frameRuntimeState->scmLayoutEpoch),
				.targetWindow = targetWindow,
				.x = 0,
				.y = 0,
				.visible = visible,
			};
			(void)m_scmTool->SetNativeSurfaceTarget(target);
			m_frameRuntimeState->scmTargetWindow = targetWindow;
			m_frameRuntimeState->scmTargetX = target.x;
			m_frameRuntimeState->scmTargetY = target.y;
			m_frameRuntimeState->scmTargetDeviceEpoch = target.deviceEpoch;
			m_frameRuntimeState->scmTargetVisible = target.visible;
			m_frameRuntimeState->scmTargetInitialized = true;
		}
	}

	const auto callbackState = m_frameRuntimeState->callbackState;
	const workbench::rendering::FrameNativeSurfacePayloadSink sink{
		.registerSurface = [callbackState](
			const workbench::rendering::FrameNativeSurfaceRegistration& registration) {
			return callbackState != nullptr && callbackState->Register(registration);
		},
		.updateSurface = [callbackState](
			const workbench::rendering::FrameNativeSurfaceRegistration& registration) {
			return callbackState != nullptr && callbackState->Update(registration);
		},
		.closeSurface = [callbackState](
			const workbench::rendering::FrameSurfaceId surfaceId,
			const std::uint64_t lifetimeEpoch) {
			if (callbackState != nullptr) callbackState->CloseSurface(surfaceId, lifetimeEpoch);
		},
		.submitFrame = [callbackState](
			std::shared_ptr<const workbench::rendering::FrameNativeSurfaceFrame> frame) {
			return callbackState != nullptr
				&& callbackState->SubmitAccepted(std::move(frame));
		},
	};
	const auto displayEpoch = callbackState->DisplayEpoch();
	const auto advanceLifetime = [](std::uint64_t& epoch) noexcept {
		if (epoch == (std::numeric_limits<std::uint64_t>::max)()) return false;
		++epoch;
		return true;
	};
	const auto sameTarget = [](const auto& left, const auto& right) noexcept {
		return left.surfaceId == right.surfaceId
			&& left.surfaceLifetimeEpoch == right.surfaceLifetimeEpoch
			&& left.deviceEpoch == right.deviceEpoch
			&& left.displayEpoch == right.displayEpoch
			&& left.layoutEpoch == right.layoutEpoch
			&& left.width == right.width && left.height == right.height
			&& left.targetWindow == right.targetWindow
			&& left.x == right.x && left.y == right.y
			&& left.visible == right.visible && left.minimized == right.minimized;
	};
	const auto configureSurface = [&](auto* surface,
		CEditWndNativeSurfaceBinding& binding,
		const workbench::rendering::FrameSurfaceId surfaceId) noexcept {
		if (surface == nullptr) return;
		const void* const owner = static_cast<const void*>(surface);
		if (binding.owner != owner) {
			binding.owner = owner;
			binding.window = nullptr;
			binding.target = {};
			binding.targetInitialized = false;
			surface->SetNativeSurfaceSink(sink);
		}
		const HWND window = surface->GetHwnd();
		if (window == nullptr || ::IsWindow(window) == FALSE) {
			if (binding.window != nullptr) surface->ClearNativeSurfaceTarget();
			binding.window = nullptr;
			return;
		}
		if (binding.window != window) {
			surface->ClearNativeSurfaceTarget();
			if (!advanceLifetime(binding.lifetimeEpoch)) return;
			binding.window = window;
			binding.target = {};
			binding.targetInitialized = false;
		}
		RECT client{};
		if (::GetClientRect(window, &client) == FALSE) return;
		const auto width = static_cast<std::uint32_t>(
			std::max<LONG>(0, client.right - client.left));
		const auto height = static_cast<std::uint32_t>(
			std::max<LONG>(0, client.bottom - client.top));
		const bool visible = ::IsWindowVisible(window) != FALSE && !minimized;
		using Target = workbench::rendering::FrameNativeSurfacePayloadTarget;
		const auto& current = surface->NativeSurfaceTargetSnapshot();
		Target target = current.has_value() ? *current : Target{};
		if (!current.has_value()
			|| target.surfaceId != surfaceId
			|| target.surfaceLifetimeEpoch != binding.lifetimeEpoch) {
			target = {};
			target.surfaceId = surfaceId;
			target.surfaceLifetimeEpoch = binding.lifetimeEpoch;
			target.layoutEpoch = 1;
		}
		const bool projectionChanged = current.has_value()
			&& (target.deviceEpoch != deviceEpoch
				|| target.targetWindow != window
				|| target.width != width || target.height != height
				|| target.visible != visible || target.minimized != minimized);
		target.deviceEpoch = deviceEpoch;
		target.displayEpoch = displayEpoch;
		target.targetWindow = window;
		target.x = 0;
		target.y = 0;
		target.width = width;
		target.height = height;
		target.visible = visible;
		target.minimized = minimized;
		if (projectionChanged) {
			if (target.layoutEpoch == (std::numeric_limits<std::uint64_t>::max)()) return;
			++target.layoutEpoch;
		}
		if (!current.has_value() || !sameTarget(*current, target)) {
			(void)surface->SetNativeSurfaceTarget(target);
			if (projectionChanged && (current->deviceEpoch != target.deviceEpoch
				|| current->displayEpoch != target.displayEpoch)) {
				::InvalidateRect(window, nullptr, FALSE);
			}
		}
	};
	const auto configureWorkbenchTool = [&](auto* tool,
		CEditWndNativeSurfaceBinding& binding,
		const workbench::rendering::FrameSurfaceId surfaceId) noexcept {
		if (tool == nullptr) return;
		const void* const owner = static_cast<const void*>(tool);
		if (binding.owner != owner) {
			binding.owner = owner;
			binding.window = nullptr;
			binding.target = {};
			binding.targetInitialized = false;
			tool->SetNativeSurfaceSink(sink);
		}
		const HWND window = tool->GetHwnd();
		if (window == nullptr || ::IsWindow(window) == FALSE) {
			if (binding.targetInitialized) (void)tool->CloseNativeSurface();
			binding.window = nullptr;
			binding.target = {};
			binding.targetInitialized = false;
			return;
		}
		if (binding.window != window) {
			if (binding.targetInitialized) (void)tool->CloseNativeSurface();
			if (!advanceLifetime(binding.lifetimeEpoch)) return;
			binding.window = window;
			binding.target = {};
			binding.targetInitialized = false;
		}
		RECT client{};
		if (::GetClientRect(window, &client) == FALSE) return;
		using Target = workbench::rendering::FrameNativeSurfacePayloadTarget;
		Target target{
			.surfaceId = surfaceId,
			.surfaceLifetimeEpoch = binding.lifetimeEpoch,
			.deviceEpoch = deviceEpoch,
			.displayEpoch = displayEpoch,
			.layoutEpoch = binding.targetInitialized
				? binding.target.layoutEpoch : std::uint64_t{1},
			.width = static_cast<std::uint32_t>(
				std::max<LONG>(0, client.right - client.left)),
			.height = static_cast<std::uint32_t>(
				std::max<LONG>(0, client.bottom - client.top)),
			.targetWindow = window,
			.x = 0,
			.y = 0,
			.visible = ::IsWindowVisible(window) != FALSE && !minimized,
			.minimized = minimized,
		};
		const bool changed = !binding.targetInitialized || !sameTarget(binding.target, target);
		if (!changed) return;
		if (binding.targetInitialized) {
			if (target.layoutEpoch == (std::numeric_limits<std::uint64_t>::max)()) return;
			++target.layoutEpoch;
		}
		const auto result = binding.targetInitialized
			? tool->UpdateNativeSurface(target) : tool->RegisterNativeSurface(target);
		if (!result.Accepted()) return;
		const bool epochChanged = binding.targetInitialized
			&& (binding.target.deviceEpoch != target.deviceEpoch
				|| binding.target.displayEpoch != target.displayEpoch);
		binding.target = target;
		binding.targetInitialized = true;
		if (epochChanged) ::InvalidateRect(window, nullptr, FALSE);
	};

	for (std::size_t index = 0; index < m_pcEditViewArr.size(); ++index) {
		configureSurface(m_pcEditViewArr[index].get(),
			m_frameRuntimeState->editorBindings[index],
			editor::rendering::EditorViewSurfaceId(
				static_cast<std::uint32_t>(index), false));
	}
	configureSurface(&m_cMiniMapView, m_frameRuntimeState->minimapBinding,
		editor::rendering::EditorViewSurfaceId(0, true));
	configureSurface(m_markdownPreview.get(), m_frameRuntimeState->markdownBinding,
		markdown::kMarkdownPreviewSurfaceId);
	configureWorkbenchTool(m_explorerTool, m_frameRuntimeState->explorerBinding,
		kExplorerNativeSurfaceId);
	configureWorkbenchTool(m_searchTool, m_frameRuntimeState->searchBinding,
		kSearchNativeSurfaceId);
	configureWorkbenchTool(m_outlineWorkbenchTool, m_frameRuntimeState->outlineBinding,
		kOutlineNativeSurfaceId);
}

void CEditWnd::UpdateFrameRuntimeCadence(const bool invalidateSource) noexcept
{
	if (m_frameRuntimeState == nullptr || m_frameRuntimeState->leaseHolder == nullptr
		|| m_frameRuntimeState->leaseHolder->RuntimePointer() == nullptr || GetHwnd() == nullptr) {
		return;
	}
	if (invalidateSource) m_frameRuntimeState->cadenceSource.Invalidate();
	const auto observation = m_frameRuntimeState->cadenceSource.Observe(GetHwnd());
	if (m_frameRuntimeState->callbackState != nullptr) {
		m_frameRuntimeState->callbackState->SetDisplayEpoch(
			observation.input.displayEpoch);
	}
	if (m_frameRuntimeState->terminalFrameBridge != nullptr) {
		m_frameRuntimeState->terminalFrameBridge->SetDisplayEpoch(
			observation.input.displayEpoch);
	}
	UpdateFrameRuntimeNativeSurfaces();
	(void)m_frameRuntimeState->leaseHolder->RuntimePointer()->UpdateCadence(observation.input);
}

void CEditWnd::PublishCommittedGdiFrame() noexcept
{
	// The caller reaches this function only after the complete parent/child GDI
	// frame has flushed. Markdown keeps its own lifetime/content fence, so make
	// that same physical boundary authoritative for its last-good publication.
	if (m_markdownPreview != nullptr) {
		(void)m_markdownPreview->CommitGdiFrame();
	}
	if (m_terminalTool != nullptr) {
		m_terminalTool->CommitGdiFrames();
	}
	if (m_viewContainerPages == nullptr && (m_frameRuntimeState == nullptr
		|| m_frameRuntimeState->windowTransaction == nullptr)) return;
	if (m_frameRuntimeState == nullptr || m_frameRuntimeState->leaseHolder == nullptr
		|| m_frameRuntimeState->leaseHolder->RuntimePointer() == nullptr) return;
	auto* const runtime = m_frameRuntimeState->leaseHolder->RuntimePointer();
	UpdateFrameRuntimeNativeSurfaces();
	const auto publishNativeSurfaces = [this]() noexcept {
		for (const auto& view : m_pcEditViewArr) {
			if (view != nullptr) (void)view->PublishNativeSurface();
		}
		(void)m_cMiniMapView.PublishNativeSurface();
		if (m_markdownPreview != nullptr) {
			(void)m_markdownPreview->PublishNativeSurface();
		}
	};
	if (m_viewContainerPages != nullptr) {
		for (const auto& projection : m_viewContainerPages->CommitGdiFrame()) {
			const auto& surface = projection.surface;
			if (surface.committedRequestId == 0) continue;
			(void)runtime->RecordGdiFallback({
				.surfaceId = surface.surfaceId,
				.surfaceLifetimeEpoch = surface.surfaceLifetimeEpoch,
				.deviceEpoch = surface.deviceEpoch,
				.layoutEpoch = surface.layoutEpoch,
				.requestId = surface.committedRequestId,
				.width = projection.width,
				.height = projection.height,
				.visible = surface.visible,
			});
		}
	}
	if (m_frameRuntimeState->windowTransaction == nullptr) {
		publishNativeSurfaces();
		return;
	}

	const auto extent = [](const HWND hwnd) noexcept {
		RECT rect{};
		if (hwnd == nullptr || !::GetClientRect(hwnd, &rect)) {
			return std::pair<std::uint32_t, std::uint32_t>{1, 1};
		}
		return std::pair<std::uint32_t, std::uint32_t>{
			static_cast<std::uint32_t>(std::max<LONG>(1, rect.right - rect.left)),
			static_cast<std::uint32_t>(std::max<LONG>(1, rect.bottom - rect.top))};
	};
	const auto hwndForRole = [this](
		const workbench::rendering::EFrameWindowSurfaceRole role) noexcept -> HWND {
		switch (role) {
		case workbench::rendering::EFrameWindowSurfaceRole::ActivityBar:
			return m_activityBar ? m_activityBar->GetHwnd() : nullptr;
		case workbench::rendering::EFrameWindowSurfaceRole::PrimarySideBar:
			return m_leftWorkbenchPanel ? m_leftWorkbenchPanel->GetHwnd() : nullptr;
		case workbench::rendering::EFrameWindowSurfaceRole::SecondarySideBar:
			return m_rightWorkbenchPanel ? m_rightWorkbenchPanel->GetHwnd() : nullptr;
		case workbench::rendering::EFrameWindowSurfaceRole::Panel:
			return m_bottomWorkbenchPanel ? m_bottomWorkbenchPanel->GetHwnd() : nullptr;
		case workbench::rendering::EFrameWindowSurfaceRole::TitleAndMenu:
			return GetHwnd();
		case workbench::rendering::EFrameWindowSurfaceRole::Tabs:
			return m_cTabWnd.GetHwnd();
		case workbench::rendering::EFrameWindowSurfaceRole::StatusBar:
			return m_cStatusBar.GetStatusHwnd();
		case workbench::rendering::EFrameWindowSurfaceRole::Editor:
			return HasActiveEditorInput() ? GetActiveView().GetHwnd() : nullptr;
		case workbench::rendering::EFrameWindowSurfaceRole::MarkdownPreview:
			return m_markdownPreview ? m_markdownPreview->GetHwnd() : nullptr;
		case workbench::rendering::EFrameWindowSurfaceRole::Terminal:
			return m_terminalTool ? m_terminalTool->GetHwnd() : nullptr;
		}
		return nullptr;
	};
	for (const auto& surface :
		m_frameRuntimeState->windowTransaction->CommitGdiBoundary()) {
		if (surface.committedRequestId == 0) continue;
		const auto role = static_cast<workbench::rendering::EFrameWindowSurfaceRole>(
			surface.surfaceId - workbench::rendering::FrameWindowSurfaceId(
				workbench::rendering::EFrameWindowSurfaceRole::ActivityBar));
		const auto [width, height] = extent(hwndForRole(role));
		(void)runtime->RecordGdiFallback({
			.surfaceId = surface.surfaceId,
			.surfaceLifetimeEpoch = surface.surfaceLifetimeEpoch,
			.deviceEpoch = surface.deviceEpoch,
			.layoutEpoch = surface.layoutEpoch,
			.requestId = surface.committedRequestId,
			.width = width,
			.height = height,
			.visible = surface.visible,
		});
	}
	publishNativeSurfaces();
}

void CEditWnd::CloseFrameRuntime() noexcept
{
	if (m_frameRuntimeState == nullptr) return;
	// Raise the shared ingress fence before withdrawing child sinks. No callback
	// owns a UI wait or a runtime mutex, and any child that retained a bridge/sink
	// now sees a closed gate instead of a borrowed CEditWnd pointer.
	if (m_frameRuntimeState->callbackState != nullptr) {
		m_frameRuntimeState->callbackState->Close();
	}
	if (m_terminalTool != nullptr) m_terminalTool->DetachNativeFrameRuntime();
	if (m_scmTool != nullptr) {
		m_scmTool->ClearNativeSurfaceTarget();
		m_scmTool->SetNativeSurfaceSink({});
	}
	if (m_explorerTool != nullptr) {
		(void)m_explorerTool->CloseNativeSurface();
		m_explorerTool->SetNativeSurfaceSink({});
	}
	if (m_searchTool != nullptr) {
		(void)m_searchTool->CloseNativeSurface();
		m_searchTool->SetNativeSurfaceSink({});
	}
	if (m_outlineWorkbenchTool != nullptr) {
		(void)m_outlineWorkbenchTool->CloseNativeSurface();
		m_outlineWorkbenchTool->SetNativeSurfaceSink({});
	}
	for (const auto& view : m_pcEditViewArr) {
		if (view == nullptr) continue;
		view->ClearNativeSurfaceTarget();
		view->SetNativeSurfaceSink({});
	}
	m_cMiniMapView.ClearNativeSurfaceTarget();
	m_cMiniMapView.SetNativeSurfaceSink({});
	if (m_markdownPreview != nullptr) {
		m_markdownPreview->ClearNativeSurfaceTarget();
		m_markdownPreview->SetNativeSurfaceSink({});
	}
	if (m_frameRuntimeState->terminalFrameBridge != nullptr) {
		// DetachNativeFrameRuntime fences every pane bridge before this runtime
		// retirement. Keep this idempotent fallback for a window with no tool.
		m_frameRuntimeState->terminalFrameBridge->Close();
	}
	if (m_frameRuntimeState->windowTransaction != nullptr) {
		(void)m_frameRuntimeState->windowTransaction->Close();
	}
	if (m_frameRuntimeState->callbackState != nullptr) {
		m_frameRuntimeState->callbackState->RequestRetirement();
	} else if (m_frameRuntimeState->leaseHolder != nullptr) {
		m_frameRuntimeState->leaseHolder->RequestRetirement();
	}
	m_frameRuntimeState->retirementObservation =
		m_frameRuntimeState->leaseHolder != nullptr
		? m_frameRuntimeState->leaseHolder->Observation() : nullptr;
	m_frameRuntimeState.reset();
}

void CEditWnd::RedrawWorkbenchFrameForCommittedLayout(bool immediate)
{
	if (GetHwnd() == nullptr) return;
	// The startup draw transaction suppresses painting on purpose and commits
	// one complete frame itself, so invalidating here would only add work that
	// the commit repeats.
	if (m_startupDrawState == StartupDrawState::Suppressing
		|| m_startupDrawState == StartupDrawState::Committing) {
		m_appliedWorkbenchHostGeometry.reset();
		return;
	}
	// Children reposition through SetWindowPos/MoveWindow, which copies the old
	// client bits into the new rectangle and invalidates only what became newly
	// visible. Every other pixel — the area a sibling vacated and the copied
	// bits themselves — stays valid and stale, so a committed geometry change
	// must invalidate the whole frame once. Comparing the applied rectangles
	// keeps geometry-neutral projections (active-view switches, mirror updates)
	// from repainting the window.
	const auto hostRect = [this](const workbench::CWorkbenchPanelHost* host) {
		RECT rect{};
		if (host != nullptr && host->GetHwnd() != nullptr
			&& host->GetState() != workbench::WorkbenchPanelState::Hidden
			&& ::GetWindowRect(host->GetHwnd(), &rect)) {
			(void)::MapWindowPoints(nullptr, GetHwnd(), reinterpret_cast<POINT*>(&rect), 2);
			return rect;
		}
		return RECT{};
	};
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const std::array<RECT, 4> geometry{ client, hostRect(m_leftWorkbenchPanel.get()),
		hostRect(m_bottomWorkbenchPanel.get()), hostRect(m_rightWorkbenchPanel.get()) };
	bool partVisibilityChanged = false;
	if (m_appliedWorkbenchHostGeometry) {
		for (std::size_t index = 1; index < geometry.size(); ++index) {
			partVisibilityChanged =
				(::IsRectEmpty(&(*m_appliedWorkbenchHostGeometry)[index]) != FALSE)
				!= (::IsRectEmpty(&geometry[index]) != FALSE);
			if (partVisibilityChanged) break;
		}
	}
	const bool changed = immediate || !m_appliedWorkbenchHostGeometry
		|| !std::ranges::equal(*m_appliedWorkbenchHostGeometry, geometry,
			[](const RECT& lhs, const RECT& rhs) { return ::EqualRect(&lhs, &rhs) != FALSE; });
	m_appliedWorkbenchHostGeometry = geometry;
	if (!changed) return;
	// Retained surfaces normally keep their last complete pixels until the new
	// geometry cohort is ready, so geometry-only commits deliberately skip the
	// background erase. A physical Part visibility transition is different: the
	// hidden HWND cannot repaint the pixels previously composed at that boundary,
	// and SWP_NOREDRAW leaves the parent/child clipping change without a complete
	// update region. Dual CopyFromScreen/PrintWindow captures measured a whole
	// Panel remaining stale until a frame-and-erase redraw. All three Part states
	// and child bounds are already committed here, so this one synchronous strict
	// redraw cannot expose an intermediate layout.
	UINT redrawFlags = RDW_INVALIDATE | RDW_ALLCHILDREN
		| (immediate ? RDW_UPDATENOW : 0);
	redrawFlags |= partVisibilityChanged ? (RDW_FRAME | RDW_ERASE) : RDW_NOERASE;
	::RedrawWindow(GetHwnd(), nullptr, nullptr,
		redrawFlags);
	if (immediate) {
		// RDW_UPDATENOW completes WM_PAINT dispatch, but child controls can leave
		// their final GDI commands in this UI thread's batch. Publish those commands
		// before the geometry handler returns so a native tab/list intermediate does
		// not become the next composed frame. This does not wait for DWM or a worker.
		(void)::GdiFlush();
		PublishCommittedGdiFrame();
	}
}

void CEditWnd::ReloadWorkbenchOutlineAndRelayout()
{
	if (m_outlineWorkbenchTool == nullptr) return;
	m_cDlgFuncList.ChangeView(reinterpret_cast<LPARAM>(&GetActiveView()));
	// Command_FUNCLIST now delegates workbench parsing to CDlgFuncList's snapshot
	// worker.  That dialog owns request deduplication and every async completion;
	// the tool adapter only owns HWND lifetime, layout, and appearance.
	const bool commandSucceeded = GetActiveView().GetCommander().Command_FUNCLIST(
		SHOW_RELOAD, OUTLINE_DEFAULT ) != FALSE;
	if( !commandSucceeded ) return;
	const bool rightPanelVisible = IsOutlineViewExpanded();
	const bool dialogCreated = m_cDlgFuncList.GetHwnd() != nullptr;
	if( !workbench::outline::ShouldRelayoutOutlineAfterReload(
		commandSucceeded, rightPanelVisible, dialogCreated ) || GetHwnd() == nullptr ) {
		return;
	}

	// DoModeless deliberately creates the workbench child with SW_HIDE.  The previous
	// layout may already have completed, so make the right host lay it out now.  OnSize2
	// only positions children; it neither activates the host nor changes editor focus.
	RECT client{};
	::GetClientRect( GetHwnd(), &client );
	(void)OnSize2( m_nWinSizeType,
		MAKELONG( client.right - client.left, client.bottom - client.top ), false );
}

void CEditWnd::BroadcastWorkbenchSettings()
{
	if (GetHwnd() == nullptr) return;
	CAppNodeGroupHandle(0).SendMessageToAllEditors(
		MYWM_CHANGESETTING, 0, PM_CHANGESETTING_WORKBENCH, GetHwnd());
}

void CEditWnd::UpdateWorkbenchWelcomeState()
{
	if (m_explorerTool == nullptr && m_scmTool == nullptr) return;

	const auto explorerState = [&]() {
		if (m_workbenchRuntime == nullptr) {
			return m_hasActiveEditorInput
				? workbench::explorer::ExplorerWelcomeState::NoFolderWithEditors
				: workbench::explorer::ExplorerWelcomeState::NoFolder;
		}
		const auto snapshot = m_workbenchRuntime->WorkspaceContext().Snapshot();
		if (snapshot.kind == config::EWorkspaceKind::Workspace && snapshot.folders.empty()) {
			return workbench::explorer::ExplorerWelcomeState::EmptyWorkspace;
		}
		if (snapshot.kind == config::EWorkspaceKind::Workspace && !snapshot.folders.empty()) {
			return workbench::explorer::ExplorerWelcomeState::WorkspaceWithFoldersUnsupported;
		}
		if (snapshot.kind == config::EWorkspaceKind::Empty) {
			return m_hasActiveEditorInput
				? workbench::explorer::ExplorerWelcomeState::NoFolderWithEditors
				: workbench::explorer::ExplorerWelcomeState::NoFolder;
		}
		return workbench::explorer::ExplorerWelcomeState::NoFolder;
	}();
	if (m_explorerTool != nullptr) m_explorerTool->SetWelcomeState(explorerState);

	if (m_scmTool != nullptr) {
		const auto scmState = m_workbenchRuntime == nullptr
			? (GetSemanticWorkspaceRoot().empty()
				? workbench::scm::EGitScmWelcomeWorkspaceState::Empty
				: workbench::scm::EGitScmWelcomeWorkspaceState::Folder)
			: ScmWelcomeWorkspaceState(m_workbenchRuntime->WorkspaceContext().Snapshot());
		m_scmTool->SetWelcomeWorkspaceState(scmState);
	}
}

std::wstring CEditWnd::GetSemanticWorkspaceRoot() const
{
	if (m_workbenchRuntime == nullptr) {
		return m_workspaceContext == nullptr ? std::wstring{} : m_workspaceContext->GetRoot();
	}

	const auto snapshot = m_workbenchRuntime->WorkspaceContext().Snapshot();
	// The native Explorer currently has one root. Do not silently collapse a
	// multi-root workspace to its first folder; that receives a real tree model
	// in the later view-container slice.
	if ((snapshot.kind != config::EWorkspaceKind::Folder
		&& snapshot.kind != config::EWorkspaceKind::Workspace)
		|| snapshot.folders.size() != 1) return {};
	const auto path = snapshot.folders.front().uri.ToWindowsPath();
	return path ? std::move(*path.value) : std::wstring{};
}

void CEditWnd::ApplySemanticWorkspaceContext()
{
	if (m_workspaceContext == nullptr) return;
	const auto root = GetSemanticWorkspaceRoot();
	if (m_workbenchRuntime != nullptr) {
		if (root.empty()) m_workspaceContext->ClearExplicitRoot();
		else m_workspaceContext->SetExplicitRoot(root);
	}
	if (m_explorerTool) m_explorerTool->SetRoot(root);
	if (m_scmTool) {
		m_scmTool->SetRoot(root);
	}
	UpdateWorkbenchWelcomeState();
	// The caption carries the folder's name, so opening or closing one changes it.
	UpdateCaption();
	if (m_terminalTool && !m_projectWorkspaceTransitionInProgress) {
		m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
	}
	if (m_viewContainerPages != nullptr) {
		m_viewContainerPages->RefreshPageContent(
			workbench::layout::ids::viewContainer::Projects);
	}
}


void CEditWnd::UpdateWorkspaceFromDocument()
{
	if (!m_workspaceContext) return;
	if (GetDocument()->m_cDocFile.GetFilePathClass().IsValidPath()) {
		m_workspaceContext->SetSelectedFile(GetDocument()->m_cDocFile.GetFilePath());
	} else {
		m_workspaceContext->ClearSelectedFile();
	}
	// A document path is only a terminal-launch fallback. It never promotes its
	// parent to workspace identity, Explorer/SCM root, or .vscode authority.
	if (m_terminalTool) m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
}

std::optional<std::string> CEditWnd::NextWorkbenchLayoutOperationId(std::string_view action)
{
	if (action.empty() || action.find('\0') != std::string_view::npos
		|| m_workbenchLayoutOperationSequence == (std::numeric_limits<std::uint64_t>::max)()) {
		return std::nullopt;
	}
	const auto sequence = ++m_workbenchLayoutOperationSequence;
	std::string operationId = "sakura.native-layout.v1/";
	operationId += std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
	operationId += '/';
	operationId += std::to_string(static_cast<unsigned long long>(
		reinterpret_cast<std::uintptr_t>(GetHwnd())));
	operationId += '/';
	operationId += std::to_string(static_cast<unsigned long long>(sequence));
	operationId += '/';
	operationId.append(action);
	if (operationId.size() > workbench::layout::kMaxWorkbenchLayoutOperationIdLength) {
		return std::nullopt;
	}
	return operationId;
}

std::optional<std::string> CEditWnd::NextOutputPanelOperationId()
{
	if (m_outputPanelOperationSequence == (std::numeric_limits<std::uint64_t>::max)()) {
		return std::nullopt;
	}
	const auto sequence = ++m_outputPanelOperationSequence;
	std::string operationId = "sakura.native-output.v1/";
	operationId += std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
	operationId += '/';
	operationId += std::to_string(static_cast<unsigned long long>(
		reinterpret_cast<std::uintptr_t>(GetHwnd())));
	operationId += '/';
	operationId += std::to_string(static_cast<unsigned long long>(sequence));
	if (!workbench::output::IsValidOutputOperationId(operationId)) return std::nullopt;
	return operationId;
}

bool CEditWnd::SetBuiltinPartVisibility(std::string_view partId, bool visible)
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		auto operationId = NextWorkbenchLayoutOperationId("set-part-visibility");
		if (!operationId) return false;
		const auto result = m_workbenchRuntime->LayoutState().SetPartVisibility({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.partId = std::string(partId),
			.visible = visible,
		});
		return result.status == workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			|| result.status == workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::SetBuiltinPartExtent(std::string_view partId, int extentDip)
{
	if (m_workbenchRuntime == nullptr || extentDip <= 0
		|| static_cast<std::uint64_t>(extentDip)
			> workbench::layout::kMaximumWorkbenchLayoutCommittedExtentDip) {
		return false;
	}
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		auto operationId = NextWorkbenchLayoutOperationId("set-part-extent");
		if (!operationId) return false;
		const auto result = m_workbenchRuntime->LayoutState().SetPartExtent({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.partId = std::string(partId),
			.committedExtentDip = static_cast<std::uint32_t>(extentDip),
		});
		return result.status == workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			|| result.status == workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::SetBuiltinViewVisibility(std::string_view viewId, bool visible)
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		auto operationId = NextWorkbenchLayoutOperationId("set-view-visibility");
		if (!operationId) return false;
		const auto result = m_workbenchRuntime->LayoutState().SetViewVisibility({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.viewId = std::string(viewId),
			.visible = visible,
		});
		return result.status == workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			|| result.status == workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::ActivateBuiltinWorkbenchView(std::string_view viewId, bool requestFocus)
{
	if (m_workbenchRuntime == nullptr) return false;
	const bool supported = viewId == workbench::layout::ids::view::Explorer
		|| viewId == workbench::layout::ids::view::Outline
		|| viewId == workbench::layout::ids::view::Search
		|| viewId == workbench::layout::ids::view::SourceControl
		|| viewId == workbench::layout::ids::view::ExtensionsInstalled
		|| viewId == workbench::layout::ids::view::Terminal
		|| viewId == workbench::layout::ids::view::Problems
		|| viewId == workbench::layout::ids::view::Output
		;
	if (!supported) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto view = std::ranges::find(snapshot.views, viewId,
			&workbench::layout::WorkbenchViewState::viewId);
		if (view == snapshot.views.end()) return false;
		const auto container = std::ranges::find(snapshot.containers, view->containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end()) return false;
		if (m_paneCompositeProjection == nullptr) return false;
		const auto supportedLocations =
			m_paneCompositeProjection->SupportedLocations(container->containerId);
		if (!supportedLocations.complete || !supportedLocations.Contains(container->location)) {
			return false;
		}

		std::string_view partId;
		switch (container->location) {
		case workbench::layout::EWorkbenchViewContainerLocation::SideBar:
			partId = workbench::layout::ids::part::Sidebar;
			break;
		case workbench::layout::EWorkbenchViewContainerLocation::Panel:
			partId = workbench::layout::ids::part::Panel;
			break;
		case workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar:
			partId = workbench::layout::ids::part::Auxiliarybar;
			break;
		}
		if (partId.empty()) return false;
		const auto part = std::ranges::find(snapshot.parts, partId,
			&workbench::layout::WorkbenchPartState::partId);
		if (part == snapshot.parts.end()) return false;
		auto operationId = NextWorkbenchLayoutOperationId("activate-view");
		if (!operationId) return false;
		std::vector<workbench::layout::WorkbenchLayoutTransactionChange> changes;
		changes.emplace_back(workbench::layout::WorkbenchLayoutActivateViewChange{
			.viewId = std::string(viewId),
		});
		if (!part->visible) {
			changes.emplace_back(workbench::layout::WorkbenchLayoutSetPartVisibilityChange{
				.partId = std::string(partId),
				.visible = true,
			});
		}
		if (requestFocus) {
			changes.emplace_back(workbench::layout::WorkbenchLayoutSetFocusChange{
				.focus = {
					.partId = std::string(partId),
					.containerId = container->containerId,
					.viewId = view->viewId,
				},
			});
		}
		const auto result = m_workbenchRuntime->LayoutState().ApplyTransaction({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.changes = std::move(changes),
		});
		return result.status == workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			|| result.status == workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::ActivateWorkbenchViewContainer(
	const std::string_view containerId, const bool requestFocus)
{
	if (m_workbenchRuntime == nullptr || m_paneCompositeProjection == nullptr
		|| containerId.empty()) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto container = std::ranges::find(snapshot.containers, containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end()) return false;
		const auto supportedLocations =
			m_paneCompositeProjection->SupportedLocations(containerId);
		if (!supportedLocations.complete || !supportedLocations.Contains(container->location)) {
			return false;
		}
		std::string_view partId;
		switch (container->location) {
		case workbench::layout::EWorkbenchViewContainerLocation::SideBar:
			partId = workbench::layout::ids::part::Sidebar;
			break;
		case workbench::layout::EWorkbenchViewContainerLocation::Panel:
			partId = workbench::layout::ids::part::Panel;
			break;
		case workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar:
			partId = workbench::layout::ids::part::Auxiliarybar;
			break;
		}
		const auto part = std::ranges::find(snapshot.parts, partId,
			&workbench::layout::WorkbenchPartState::partId);
		if (part == snapshot.parts.end()) return false;
		auto operationId = NextWorkbenchLayoutOperationId("activate-view-container");
		if (!operationId) return false;
		std::vector<workbench::layout::WorkbenchLayoutTransactionChange> changes;
		changes.emplace_back(workbench::layout::WorkbenchLayoutActivateContainerChange{
			.containerId = std::string(containerId),
		});
		if (!part->visible) {
			changes.emplace_back(workbench::layout::WorkbenchLayoutSetPartVisibilityChange{
				.partId = std::string(partId),
				.visible = true,
			});
		}
		if (requestFocus) {
			changes.emplace_back(workbench::layout::WorkbenchLayoutSetFocusChange{
				.focus = {
					.partId = std::string(partId),
					.containerId = std::string(containerId),
					.viewId = container->activeViewId,
				},
			});
		}
		const auto result = m_workbenchRuntime->LayoutState().ApplyTransaction({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.changes = std::move(changes),
		});
		return result.status == workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			|| result.status == workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::IsBuiltinWorkbenchViewActive(std::string_view viewId) const
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto view = std::ranges::find(snapshot.views, viewId,
			&workbench::layout::WorkbenchViewState::viewId);
		if (view == snapshot.views.end() || !view->visible) return false;
		const auto container = std::ranges::find(snapshot.containers, view->containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end() || !container->visible
			|| !container->activeViewId || *container->activeViewId != viewId) {
			return false;
		}

		const std::optional<std::string>* activeContainer = nullptr;
		std::string_view partId;
		switch (container->location) {
		case workbench::layout::EWorkbenchViewContainerLocation::SideBar:
			activeContainer = &snapshot.activeContainers.sideBar;
			partId = workbench::layout::ids::part::Sidebar;
			break;
		case workbench::layout::EWorkbenchViewContainerLocation::Panel:
			activeContainer = &snapshot.activeContainers.panel;
			partId = workbench::layout::ids::part::Panel;
			break;
		case workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar:
			activeContainer = &snapshot.activeContainers.auxiliaryBar;
			partId = workbench::layout::ids::part::Auxiliarybar;
			break;
		}
		if (activeContainer == nullptr || !*activeContainer
			|| **activeContainer != container->containerId) {
			return false;
		}
		const auto part = std::ranges::find(snapshot.parts, partId,
			&workbench::layout::WorkbenchPartState::partId);
		return part != snapshot.parts.end() && part->visible;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::IsSidebarViewContainerActive(std::string_view containerId) const
{
	// VS Code's Activity Bar compares the clicked container with `getActivePaneComposite()`, so
	// a nested View selection such as Outline inside Explorer must not change the answer. Use
	// `IsBuiltinWorkbenchViewActive` only where the active View itself is the question.
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto part = std::ranges::find(snapshot.parts, workbench::layout::ids::part::Sidebar,
			&workbench::layout::WorkbenchPartState::partId);
		if (part == snapshot.parts.end() || !part->visible) return false;
		const auto container = std::ranges::find(snapshot.containers, containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end() || !container->visible
			|| container->location != workbench::layout::EWorkbenchViewContainerLocation::SideBar) {
			return false;
		}
		return snapshot.activeContainers.sideBar
			&& *snapshot.activeContainers.sideBar == containerId;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::RefreshWorkbenchCommandContext()
{
	if (m_workbenchRuntime == nullptr || m_workbenchContextKeyService == nullptr) return false;
	try {
		const bool recentlyOpenedAvailable = HasRecentlyOpenedItems();
		const auto workspace = m_workbenchRuntime->WorkspaceContext().Snapshot();
		const auto result = m_workbenchContextKeyService->SetCoreProjection(
			m_workbenchRuntime->LayoutState().Snapshot(), workspace,
			BuildWorkbenchEditorCommandContext(), recentlyOpenedAvailable, BuildWorkbenchScmCommandContext(),
			BuildWorkbenchUpdateCommandContext());
		return result.Succeeded()
			|| result.status == workbench::commands::EWorkbenchContextMutationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::TryExecuteWorkbenchStableCommand(
	std::string_view commandId, bool& handled, std::string_view argumentsJson)
{
	handled = false;
	if (m_workbenchCommandRegistry == nullptr || m_workbenchContextKeyService == nullptr) return false;
	if (!m_workbenchCommandRegistry->Find(commandId)) return false;
	handled = true;
	if (!RefreshWorkbenchCommandContext()) {
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command context refresh failed.\n");
		return false;
	}

	const auto result = m_workbenchCommandRegistry->Execute(
		commandId, m_workbenchContextKeyService->Snapshot(), argumentsJson);
	switch (result.status) {
	case workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded:
		return true;
	case workbench::commands::EWorkbenchCommandExecutionStatus::NotApplicable:
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command was not applicable.\n");
		return false;
	case workbench::commands::EWorkbenchCommandExecutionStatus::Disabled:
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command was disabled.\n");
		return false;
	case workbench::commands::EWorkbenchCommandExecutionStatus::UnknownCommand:
		// A registry lookup just succeeded, so this is an internal terminal error,
		// never a cue to execute a potentially different legacy action.
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command disappeared.\n");
		return false;
	case workbench::commands::EWorkbenchCommandExecutionStatus::Unsupported:
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command executor is unsupported.\n");
		return false;
	case workbench::commands::EWorkbenchCommandExecutionStatus::Failed:
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command failed.\n");
		return false;
	}
	::OutputDebugStringW(L"Sakura Editor NEXT: workbench command returned an invalid terminal status.\n");
	return false;
}

bool CEditWnd::ArmWorkbenchKeybindingChordTimer() noexcept
{
	if (GetHwnd() == nullptr
		|| ::SetTimer(GetHwnd(), IDT_WORKBENCH_KEYBINDING_CHORD,
			static_cast<UINT>(workbench::editor::CtrlKChordState::TimeoutMs), nullptr) == 0) {
		// Fail closed when a native timer cannot be armed.  The first stroke must
		// never leave a permanently pending chord in that case.
		m_workbenchKeybindingState.Clear();
		return false;
	}
	return true;
}

workbench::commands::WorkbenchScmCommandContext CEditWnd::BuildWorkbenchScmCommandContext() const
{
	// Pulled from the SCM tool's published provider snapshot rather than pushed
	// from its refresh, so the key can never describe a publication the view has
	// not rendered.
	workbench::commands::WorkbenchScmCommandContext scm;
	if (m_scmTool != nullptr) {
		scm.gitOpenRepositoryCount = static_cast<std::int64_t>(m_scmTool->OpenRepositoryCount());
	}
	return scm;
}

workbench::commands::WorkbenchEditorCommandContext CEditWnd::BuildWorkbenchEditorCommandContext() const
{
	workbench::commands::WorkbenchEditorCommandContext editor;
	// The retained endpoints are set only after the surface accepted a
	// comparison and cleared when it is retracted, so they are the one fact that
	// says a comparison is on screen. Set before the early returns below, because
	// a diff is shown precisely when no document input is active.
	editor.inDiffEditor = m_diffSurface != nullptr && m_diffSurface->HasDiff() && !m_diffModifiedUri.empty();
	if (m_editorServiceAdapter != nullptr) {
		const auto editorSnapshot = m_editorServiceAdapter->Snapshot();
		editor.hasActiveEditor = editorSnapshot.group.activeInputId.has_value();
		if (!editorSnapshot.group.activeInputId) return editor;
		const auto input = std::ranges::find_if(editorSnapshot.group.inputs,
			[&editorSnapshot](const auto& candidate) {
				return candidate.descriptor.inputId == *editorSnapshot.group.activeInputId;
			});
		if (input == editorSnapshot.group.inputs.end()) return editor;
		const auto document = std::ranges::find(editorSnapshot.documents, input->documentKey,
			&workbench::editor::EditorDocumentSnapshot::documentKey);
		editor.activeEditorDirty = document != editorSnapshot.documents.end() && document->dirty;
		return editor;
	}
	editor.hasActiveEditor = HasActiveEditorInput();
	editor.activeEditorDirty = editor.hasActiveEditor && GetDocument()->m_cDocEditor.IsModified();
	return editor;
}

void CEditWnd::ClearWorkbenchKeybindingChord() noexcept
{
	if (GetHwnd() != nullptr) ::KillTimer(GetHwnd(), IDT_WORKBENCH_KEYBINDING_CHORD);
	m_workbenchKeybindingState.Clear();
}

void CEditWnd::ExpireWorkbenchKeybindingChord() noexcept
{
	if (m_workbenchKeybindingState.ExpireIfNeeded(::GetTickCount64())) {
		if (GetHwnd() != nullptr) ::KillTimer(GetHwnd(), IDT_WORKBENCH_KEYBINDING_CHORD);
	}
}

bool CEditWnd::ExecuteToggleSidebarVisibilityCommand()
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto part = std::ranges::find(snapshot.parts, workbench::layout::ids::part::Sidebar,
			&workbench::layout::WorkbenchPartState::partId);
		if (part == snapshot.parts.end()) return false;
		if (!SetBuiltinPartVisibility(workbench::layout::ids::part::Sidebar, !part->visible)) return false;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) return false;
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::ExecuteShowExplorerCommand()
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		if (!ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Explorer, true)) return false;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) return false;
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::ExecuteShowProblemsCommand()
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		if (!ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Problems, true)) return false;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) return false;
		if (!RefreshWorkbenchCommandContext()) return false;
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::ExecuteShowOutputCommand(const bool requestFocus)
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		if (!ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Output, requestFocus)) return false;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) return false;
		if (!RefreshWorkbenchCommandContext()) return false;
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::ExecuteToggleOutputCommand()
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		if (!IsBuiltinWorkbenchViewActive(workbench::layout::ids::view::Output)) {
			return ExecuteShowOutputCommand();
		}
		if (!SetBuiltinPartVisibility(workbench::layout::ids::part::Panel, false)) return false;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) return false;
		if (!RefreshWorkbenchCommandContext()) return false;
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}
	catch (...) {
		return false;
	}
}

void CEditWnd::PersistWorkbenchExtent(workbench::WorkbenchEdge edge, int extentDip)
{
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	int* savedExtent = nullptr;
	switch (edge) {
	case workbench::WorkbenchEdge::Left: savedExtent = &settings.m_nLeftPanelExtent96; break;
	case workbench::WorkbenchEdge::Right: savedExtent = &settings.m_nRightPanelExtent96; break;
	case workbench::WorkbenchEdge::Bottom: savedExtent = &settings.m_nBottomPanelExtent96; break;
	}
	if (savedExtent == nullptr || *savedExtent == extentDip) return;
	*savedExtent = extentDip;
	BroadcastWorkbenchSettings();
}

bool CEditWnd::IsWorkbenchPanelVisible(workbench::WorkbenchEdge edge) const noexcept
{
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
			const auto partVisible = [&snapshot](std::string_view partId) {
				const auto part = std::ranges::find(snapshot.parts, partId,
					&workbench::layout::WorkbenchPartState::partId);
				return part != snapshot.parts.end() && part->visible;
			};
			switch (edge) {
			case workbench::WorkbenchEdge::Left:
				return partVisible(workbench::layout::ids::part::Sidebar);
			case workbench::WorkbenchEdge::Right: {
				const auto outline = std::ranges::find(snapshot.views,
					workbench::layout::ids::view::Outline,
					&workbench::layout::WorkbenchViewState::viewId);
				return partVisible(workbench::layout::ids::part::Sidebar)
					&& outline != snapshot.views.end() && outline->visible;
			}
			case workbench::WorkbenchEdge::Bottom:
				return partVisible(workbench::layout::ids::part::Panel);
			}
		}
		catch (...) {
			return false;
		}
		return false;
	}

	const workbench::CWorkbenchPanelHost* host = nullptr;
	switch (edge) {
	case workbench::WorkbenchEdge::Left: host = m_leftWorkbenchPanel.get(); break;
	case workbench::WorkbenchEdge::Right:
		return IsOutlineViewExpanded();
	case workbench::WorkbenchEdge::Bottom: host = m_bottomWorkbenchPanel.get(); break;
	}
	return host != nullptr && host->GetState() != workbench::WorkbenchPanelState::Hidden;
}

bool CEditWnd::SetWorkbenchPanelVisible(workbench::WorkbenchEdge edge, bool visible, bool activate)
{
	if (m_workbenchRuntime != nullptr) {
		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		bool mirrorChanged = false;

		if (edge == workbench::WorkbenchEdge::Right) {
			// This is the legacy Outline nested inside the left Sidebar. It is not
			// the physical VS Code Auxiliary Bar hosted on the right.
			if (m_viewContainerPages == nullptr || m_leftWorkbenchPanel == nullptr) return false;
			// A reload request reaches this path while the Outline view is already
			// visible. Re-projecting an unchanged visibility state synchronously
			// walks the native workbench tree and turns a cheap parser request into
			// a document-size UI stall. Activation still needs the full projection;
			// a non-activating no-op does not.
			if (!activate && IsOutlineViewExpanded() == visible) return true;
			const bool committed = visible && activate
				? ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Outline, true)
				: (!visible || SetBuiltinPartVisibility(workbench::layout::ids::part::Sidebar, true))
					&& SetBuiltinViewVisibility(workbench::layout::ids::view::Outline, visible);
			if (!committed) return false;
			if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
				::OutputDebugStringW(L"Sakura Editor NEXT: Outline reveal projection failed.\n");
				return false;
			}
			if (mirrorChanged) BroadcastWorkbenchSettings();
			return true;
		}

		const std::string_view partId = edge == workbench::WorkbenchEdge::Left
			? workbench::layout::ids::part::Sidebar : workbench::layout::ids::part::Panel;
		const std::string_view viewId = edge == workbench::WorkbenchEdge::Left
			? workbench::layout::ids::view::Explorer : workbench::layout::ids::view::Terminal;
		const bool committed = visible && activate
			? ActivateBuiltinWorkbenchView(viewId, true)
			: SetBuiltinPartVisibility(partId, visible);
		if (!committed) return false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: workbench visibility projection failed.\n");
			return false;
		}
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}

	workbench::CWorkbenchPanelHost* host = nullptr;
	BOOL* savedVisible = nullptr;
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	std::string_view selectedContainer;
	switch (edge) {
	case workbench::WorkbenchEdge::Left:
		host = m_leftWorkbenchPanel.get();
		savedVisible = &settings.m_bLeftPanelVisible;
		selectedContainer = workbench::layout::ids::viewContainer::Explorer;
		break;
	case workbench::WorkbenchEdge::Right:
		host = m_leftWorkbenchPanel.get();
		savedVisible = &settings.m_bRightPanelVisible;
		selectedContainer = workbench::layout::ids::viewContainer::Explorer;
		break;
	case workbench::WorkbenchEdge::Bottom:
		host = m_bottomWorkbenchPanel.get();
		savedVisible = &settings.m_bBottomPanelVisible;
		break;
	}
	if (host == nullptr || savedVisible == nullptr) return false;
	const BOOL requestedVisible = visible ? TRUE : FALSE;
	if (edge == workbench::WorkbenchEdge::Bottom && !visible) {
		m_bottomWorkbenchMaximized = false;
	}
	const bool visibilityChanged = savedVisible != nullptr && *savedVisible != requestedVisible;
	if (savedVisible) *savedVisible = requestedVisible;
	bool leftVisibilityChanged = false;
	const auto oldActiveTool = settings.m_eActiveTool;
	if (visible && activate) {
		switch (edge) {
		case workbench::WorkbenchEdge::Left: settings.m_eActiveTool = WORKBENCH_TOOL_EXPLORER; break;
		case workbench::WorkbenchEdge::Right: settings.m_eActiveTool = WORKBENCH_TOOL_OUTLINE; break;
		case workbench::WorkbenchEdge::Bottom: settings.m_eActiveTool = WORKBENCH_TOOL_TERMINAL; break;
		}
	}
	if (edge == workbench::WorkbenchEdge::Right) {
		if (visible && m_leftWorkbenchPanel) {
			leftVisibilityChanged = settings.m_bLeftPanelVisible == FALSE;
			m_leftWorkbenchPanel->Show();
			settings.m_bLeftPanelVisible = TRUE;
		}
		SetOutlineExpandedInHosts(visible);
	} else if (host != nullptr) {
		if (edge == workbench::WorkbenchEdge::Left && visible && activate && m_viewContainerPages) {
			ApplySidebarPage(workbench::viewcontainer::pageIds::Explorer);
		}
		if (visible) host->Show(); else host->Hide();
	}
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
		RedrawWorkbenchFrameForCommittedLayout(false);
	}
	// Materialize a newly shown tool only after the host has received its real
	// bounds.  In particular, ConPTY must not be started from the host's initial
	// empty rectangle, which would create a 1x1 pseudo console and lose the
	// shell's startup output before the first resize.
	if (host != nullptr && visible && activate) {
		if (edge == workbench::WorkbenchEdge::Right && m_viewContainerPages) {
			if (auto* explorerHost = HostShowingPage(workbench::viewcontainer::pageIds::Explorer)) {
				explorerHost->FocusOutline();
			}
		} else {
			host->ActivateTool();
		}
	}
	if (m_activityBar && visible && activate) m_activityBar->SetSelectedItem(selectedContainer);
	if (visibilityChanged || leftVisibilityChanged || oldActiveTool != settings.m_eActiveTool) {
		BroadcastWorkbenchSettings();
	}
	return true;
}

void CEditWnd::ActivateSidebarPage(const std::string_view containerId, bool toggleIfActive)
{
	if (!m_leftWorkbenchPanel || !m_sidebarHost || containerId.empty()) return;
	namespace pageIds = workbench::viewcontainer::pageIds;
	// Only a built-in container is identified by a View here; a contributed one is activated
	// as a container, so `requestedView` stays empty for it.
	std::string_view requestedView;
	auto legacyTool = WORKBENCH_TOOL_EXPLORER;
	if (containerId == pageIds::Explorer) {
		requestedView = workbench::layout::ids::view::Explorer;
	} else if (containerId == pageIds::Search) {
		requestedView = workbench::layout::ids::view::Search;
		// The legacy `m_eActiveTool` mirror has no Search value. Reusing another
		// tool's value would make a runtime-less window claim a surface it cannot
		// show, so the legacy path declines instead.
		if (m_workbenchRuntime == nullptr) return;
	} else if (containerId == pageIds::SourceControl) {
		requestedView = workbench::layout::ids::view::SourceControl;
		legacyTool = WORKBENCH_TOOL_SCM;
	} else if (containerId == pageIds::Extensions) {
		requestedView = workbench::layout::ids::view::ExtensionsInstalled;
		if (m_workbenchRuntime == nullptr) return;
	} else if (m_workbenchRuntime == nullptr) {
		// The legacy path has no registry to hold a contributed container, so there is nothing
		// it could activate and mirroring it onto a built-in tool would be a lie.
		return;
	}
	// The toggle compares ViewContainers exactly as VS Code does, so an Outline selection inside
	// the Explorer container still counts as "Explorer is already active".
	const bool alreadyActive = m_workbenchRuntime != nullptr
		? IsSidebarViewContainerActive(containerId)
		: IsWorkbenchPanelVisible(workbench::WorkbenchEdge::Left)
			&& m_pShareData->m_Common.m_sWorkbench.m_eActiveTool == legacyTool;
	if (toggleIfActive && alreadyActive) {
		SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Left, false, false);
		if (m_workbenchRuntime == nullptr && m_activityBar) {
			m_activityBar->SetSelectedItem({});
		}
		return;
	}
	if (m_workbenchRuntime != nullptr) {
		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		const bool activated = requestedView.empty()
			? ActivateWorkbenchViewContainer(containerId, true)
			: ActivateBuiltinWorkbenchView(requestedView, true);
		if (!activated) {
			// A click that reaches here has already passed the "this container has a page"
			// check, so a rejected activation is a real model failure, not an ordinary
			// no-op. Leaving no trace at all is what made this class of defect invisible.
			::OutputDebugStringW(L"Sakura Editor NEXT: view container activation was rejected.\n");
			return;
		}
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: left tool projection failed.\n");
			return;
		}
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return;
	}
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	settings.m_bLeftPanelVisible = TRUE;
	settings.m_eActiveTool = legacyTool;
	ApplySidebarPage(containerId);
	m_leftWorkbenchPanel->Show();
	if (GetHwnd()) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
		RedrawWorkbenchFrameForCommittedLayout(false);
	}
	m_leftWorkbenchPanel->ActivateTool();
	if (m_activityBar) m_activityBar->SetSelectedItem(containerId);
	BroadcastWorkbenchSettings();
}

void CEditWnd::ApplySidebarPage(const std::string_view containerId)
{
	if (m_sidebarHost == nullptr) return;
	// A ViewContainer has exactly one location, so the side bar that just gained it takes
	// the page window away from the other one instead of both claiming to render it.
	if (m_auxiliaryBarHost != nullptr && !containerId.empty()
		&& m_auxiliaryBarHost->ActivePage() == containerId) {
		m_auxiliaryBarHost->ShowPage({});
	}
	m_sidebarHost->ShowPage(containerId);
	RefreshSidebarTitles();
}

void CEditWnd::ApplyAuxiliaryBarPage(const std::string_view containerId)
{
	if (m_auxiliaryBarHost == nullptr) return;
	if (!containerId.empty() && m_sidebarHost != nullptr
		&& m_sidebarHost->ActivePage() == containerId) {
		m_sidebarHost->ShowPage({});
	}
	m_auxiliaryBarHost->ShowPage(containerId);
	RefreshSidebarTitles();
}

void CEditWnd::RefreshSidebarTitles()
{
	// Fixed pages own localized titles. Contributed pages take their fallback title from the
	// logical contribution registry so native factories do not create a second title authority.
	const auto titleOf = [this](std::string_view containerId) -> std::wstring {
		if (containerId.empty() || m_viewContainerPages == nullptr) return {};
		if (auto title = m_viewContainerPages->PageTitle(containerId); !title.empty()) return title;
		const auto snapshot = m_workbenchRuntime != nullptr
			? m_workbenchRuntime->Contributions().Snapshot()
			: workbench::layout::WorkbenchContributionRegistry{}.Snapshot();
		const auto found = std::ranges::find_if(snapshot.viewContainers,
			[containerId](const auto& entry) { return entry.descriptor.id == containerId; });
		if (found == snapshot.viewContainers.end()) return {};
		return ResolveLocalizedActivityBarTitle(containerId, u8stowcs(found->descriptor.title));
	};
	const auto menuOf = [this](std::string_view containerId) {
		std::vector<workbench::CWorkbenchPanelHost::HeaderMenuItem> items;
		if (containerId != workbench::viewcontainer::pageIds::Extensions) return items;
		items.push_back({
			.title = LocalizedWorkbenchString(STR_WORKBENCH_EXTENSIONS_INSTALL_FROM_SENP),
			.enabled = m_extensionsTool != nullptr && m_extensionsTool->CanInstallDeveloperPackage(),
			.invoke = [this]() {
				if (m_extensionsTool != nullptr) m_extensionsTool->InstallDeveloperPackage();
			},
		});
		return items;
	};
	if (m_leftWorkbenchPanel && m_sidebarHost != nullptr) {
		const auto page = m_sidebarHost->ActivePage();
		m_leftWorkbenchPanel->SetTitle(titleOf(page));
		m_leftWorkbenchPanel->SetHeaderMenu(menuOf(page));
	}
	if (m_rightWorkbenchPanel && m_auxiliaryBarHost != nullptr) {
		const auto page = m_auxiliaryBarHost->ActivePage();
		m_rightWorkbenchPanel->SetTitle(page.empty()
			? LocalizedWorkbenchString(STR_WORKBENCH_SECONDARY_SIDEBAR_TITLE)
			: titleOf(page));
		m_rightWorkbenchPanel->SetHeaderMenu(menuOf(page));
	}
}

void CEditWnd::RefreshLocalizedWorkbenchText()
{
	if (m_commandPaletteOverlay) m_commandPaletteOverlay->RefreshStrings();
	if (m_viewContainerPages) m_viewContainerPages->RefreshStrings();
	if (m_bottomPanelTool) m_bottomPanelTool->RefreshStrings();
	RefreshSidebarTitles();
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
			SyncViewContainers(&snapshot);
		}
		catch (...) {
			SyncViewContainers(nullptr);
		}
	} else {
		SyncViewContainers(nullptr);
	}
	RefreshStatusbarPresentation();
	if (GetHwnd() != nullptr) {
		::RedrawWindow(GetHwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
	}
}

workbench::viewcontainer::CViewContainerHost* CEditWnd::HostShowingPage(
	const std::string_view containerId) const noexcept
{
	if (containerId.empty()) return nullptr;
	if (m_sidebarHost != nullptr && m_sidebarHost->ActivePage() == containerId) return m_sidebarHost;
	if (m_auxiliaryBarHost != nullptr && m_auxiliaryBarHost->ActivePage() == containerId) {
		return m_auxiliaryBarHost;
	}
	return nullptr;
}

workbench::CWorkbenchPanelHost* CEditWnd::PanelHostFor(
	const workbench::viewcontainer::CViewContainerHost* host) const noexcept
{
	if (host == nullptr) return nullptr;
	if (host == m_sidebarHost) return m_leftWorkbenchPanel.get();
	if (host == m_auxiliaryBarHost) return m_rightWorkbenchPanel.get();
	return nullptr;
}

bool CEditWnd::IsOutlineViewExpanded() const noexcept
{
	// Outline is a View inside the Explorer ViewContainer, so "is it showing" depends on
	// where that container lives now, not on a fixed physical Part.
	const auto* host = HostShowingPage(workbench::viewcontainer::pageIds::Explorer);
	if (host == nullptr) return false;
	const auto* panel = PanelHostFor(host);
	return panel != nullptr
		&& panel->GetState() != workbench::WorkbenchPanelState::Hidden
		&& host->IsOutlineExpanded();
}

void CEditWnd::SetOutlineExpandedInHosts(bool expanded)
{
	if (auto* host = HostShowingPage(workbench::viewcontainer::pageIds::Explorer)) {
		host->SetOutlineExpanded(expanded);
		return;
	}
	// No side bar renders the Explorer container right now, so only the shared model fact
	// changes; the next host to receive that container lays out from it.
	if (m_viewContainerPages) m_viewContainerPages->SetOutlineExpanded(expanded);
}

void CEditWnd::SyncViewContainers(const workbench::layout::WorkbenchLayoutStateSnapshot* layoutState)
{
	if (!m_activityBar && !m_auxiliaryActivityBar && !m_viewContainerPages) return;
	std::vector<std::string> registeredPageIds;
	std::vector<std::string_view> renderableContainers;
	if (m_viewContainerPages) {
		registeredPageIds = m_viewContainerPages->PageIds();
		renderableContainers.reserve(registeredPageIds.size());
		for (const auto& pageId : registeredPageIds) renderableContainers.emplace_back(pageId);
	} else {
		renderableContainers.assign(
			kFallbackRenderableContainers.begin(), kFallbackRenderableContainers.end());
	}
	const workbench::activity::ActivityBarProjectionOptions options{
		.renderableBuiltins = renderableContainers,
		.titleResolver = ResolveLocalizedActivityBarTitle,
		.layoutState = layoutState,
	};
	static const workbench::layout::WorkbenchContributionSnapshot builtinsOnly =
		workbench::layout::WorkbenchContributionRegistry{}.Snapshot();
	const auto contributions =
		m_workbenchRuntime != nullptr ? m_workbenchRuntime->Contributions().Snapshot() : builtinsOnly;
	auto primaryEntries = workbench::activity::ProjectActivityBarEntries(
		contributions, options, workbench::layout::EViewContainerLocation::Sidebar);
	auto auxiliaryEntries = workbench::activity::ProjectActivityBarEntries(
		contributions, options, workbench::layout::EViewContainerLocation::AuxiliaryBar);
	// GlobalCompositeBar: Accounts then Manage are pinned to the vertical bar.
	// Top/bottom placement moves both actions to the native title bar.
	if (m_activityBarLocation == workbench::ActivityBarLocation::Default) {
		workbench::activity::AppendGlobalActivityActions(primaryEntries,
			ResolveLocalizedActivityBarTitle);
	}
	if (m_activityBar) m_activityBar->SetEntries(std::move(primaryEntries));
	if (m_auxiliaryActivityBar) m_auxiliaryActivityBar->SetEntries(std::move(auxiliaryEntries));
	if (layoutState != nullptr) {
		if (m_activityBar) m_activityBar->SetSelectedItem(layoutState->activeContainers.sideBar
			? *layoutState->activeContainers.sideBar : std::string_view{});
		if (m_auxiliaryActivityBar) {
			m_auxiliaryActivityBar->SetSelectedItem(layoutState->activeContainers.auxiliaryBar
				? *layoutState->activeContainers.auxiliaryBar : std::string_view{});
		}
	}
	// The badge is published beside the entry list, so re-projecting the
	// containers has to republish it; SetEntries never carries it.
	SyncScmActivityBadge();
}

void CEditWnd::SyncScmActivityBadge()
{
	// Both the badge and the commit box are sized from Source Control settings,
	// and this runs on every occasion that can have moved them: startup once the
	// configuration files are loaded, a theme/settings change, and each Activity
	// Bar reprojection. Re-applying is idempotent when nothing moved.
	ApplyScmInputLineCountSetting();
	// The Explorer's Git decorations are read from the same settings on the same
	// occasions, so a settings change reaches the tree without its own hook.
	ApplyExplorerDecorationSettings();
	// The terminal keybinding preset is read from the same document too.
	ApplyTerminalShortcutPresetSetting();
	if (!m_activityBar && !m_auxiliaryActivityBar) return;
	const auto publish = [this](std::optional<int> count) {
		if (m_activityBar) m_activityBar->SetViewContainerBadge(
			workbench::layout::ids::viewContainer::SourceControl, count);
		if (m_auxiliaryActivityBar) m_auxiliaryActivityBar->SetViewContainerBadge(
			workbench::layout::ids::viewContainer::SourceControl, count);
	};
	auto* const service = m_workbenchRuntime != nullptr ? m_workbenchRuntime->Scm() : nullptr;
	if (service == nullptr) {
		publish(std::nullopt);
		return;
	}
	// `off` is upstream's own way of turning the badge off entirely, so it clears
	// rather than merely skipping the update.
	if (ReadScmCountBadgeSetting() == L"off") {
		publish(std::nullopt);
		return;
	}
	const auto snapshot = service->Snapshot();
	std::int64_t total = 0;
	for (const auto& provider : snapshot.providers) {
		if (provider.count) {
			total += *provider.count;
			continue;
		}
		// Upstream's `getRepositoryResourceCount`: resources, not files, so one
		// path that is both staged and edited again counts twice.
		for (const auto& group : provider.groups) {
			total += static_cast<std::int64_t>(group.resources.size());
		}
	}
	if (total <= 0) {
		publish(std::nullopt);
		return;
	}
	publish(static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max())));
}

config::ConfigurationTarget CEditWnd::BuildWorkbenchConfigurationTarget() const
{
	config::ConfigurationTarget target;
	if (m_workbenchRuntime == nullptr) return target;
	target.profileId = m_workbenchRuntime->Bootstrap().UserDataProfile().SelectedProfileId();
	const auto workspace = m_workbenchRuntime->WorkspaceContext().Snapshot();
	if (workspace.folders.size() == 1) {
		// A Folder workspace has no .code-workspace file, so its folder identity is
		// also its workspace identity. Leaving `workspaceUri` empty while naming a
		// folder is not a narrower target -- the configuration service rejects that
		// combination outright, and every lookup made with it silently answers with
		// the caller's own fallback instead of the effective setting.
		target.workspaceUri = workspace.kind == config::EWorkspaceKind::Workspace
			? workspace.workspaceConfigUri
			: std::optional<platform::uri::Uri>(workspace.folders.front().uri);
		target.folderUri = workspace.folders.front().uri;
	} else {
		target.workspaceUri = workspace.workspaceConfigUri;
	}
	return target;
}

bool CEditWnd::ExecuteShowExtensionsCommand()
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		if (!ActivateBuiltinWorkbenchView(workbench::layout::ids::view::ExtensionsInstalled, true)) return false;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) return false;
		if (m_extensionsTool != nullptr) m_extensionsTool->Refresh();
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return true;
	}
	catch (...) {
		return false;
	}
}

workbench::ActivityBarLocation CEditWnd::ReadActivityBarLocationSetting() const
{
	if (m_workbenchRuntime == nullptr) return workbench::ActivityBarLocation::Default;
	try {
		const auto lookup = m_workbenchRuntime->Configuration().GetValue(
			"workbench.activityBar.location", BuildWorkbenchConfigurationTarget());
		if (!lookup.value) return workbench::ActivityBarLocation::Default;
		const auto* value = std::get_if<std::wstring>(&lookup.value->Value());
		if (value == nullptr) return workbench::ActivityBarLocation::Default;
		if (*value == L"top") return workbench::ActivityBarLocation::Top;
		if (*value == L"bottom") return workbench::ActivityBarLocation::Bottom;
		return workbench::ActivityBarLocation::Default;
	}
	catch (...) {
		return workbench::ActivityBarLocation::Default;
	}
}

bool CEditWnd::PersistActivityBarLocationSelection(workbench::ActivityBarLocation location)
{
	if (m_workbenchRuntime == nullptr) return false;
	std::wstring_view value;
	switch (location) {
	case workbench::ActivityBarLocation::Default: value = L"default"; break;
	case workbench::ActivityBarLocation::Top: value = L"top"; break;
	case workbench::ActivityBarLocation::Bottom: value = L"bottom"; break;
	default: return false;
	}
	try {
		const auto& profile = m_workbenchRuntime->Bootstrap().UserDataProfile();
		config::ConfigurationTarget sourceTarget;
		sourceTarget.profileId = profile.SelectedProfileId();
		const config::ConfigurationSource source {
			config::EConfigurationScope::Profile, sourceTarget,
			"profile.settings", 0,
		};
		config::editing::ConfigurationDocumentEditTarget editTarget;
		editTarget.scope = config::editing::EConfigurationDocumentScope::Profile;
		editTarget.target = sourceTarget;
		editTarget.resource = profile.Resources().Settings();
		const config::SettingsWritebackRequest request {
			.edit = {
				.target = std::move(editTarget),
				.key = "workbench.activityBar.location",
				.value = config::ConfigurationValue(std::wstring(value)),
			},
			.documentKey = "profile.settings",
			.source = source,
		};
		return m_workbenchRuntime->WriteSetting(request).Succeeded();
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::SetActivityBarLocation(workbench::ActivityBarLocation location, bool persist)
{
	switch (location) {
	case workbench::ActivityBarLocation::Default:
	case workbench::ActivityBarLocation::Top:
	case workbench::ActivityBarLocation::Bottom:
		break;
	default:
		return false;
	}
	if (persist && !PersistActivityBarLocationSelection(location)) return false;
	m_activityBarLocation = location;
	if (m_activityBar) m_activityBar->SetLocation(location);
	if (m_auxiliaryActivityBar) m_auxiliaryActivityBar->SetLocation(location);
	if (m_customFrame) {
		m_customFrame->SetActivityBarLocation(location);
	}
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
			SyncViewContainers(&snapshot);
		}
		catch (...) {
			SyncViewContainers(nullptr);
		}
	} else {
		SyncViewContainers(nullptr);
	}
	if (GetHwnd() != nullptr && m_activityBar && m_auxiliaryActivityBar) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
		RedrawWorkbenchFrameForCommittedLayout(false);
	}
	return true;
}

void CEditWnd::ApplyActivityBarLocationSetting()
{
	static_cast<void>(SetActivityBarLocation(ReadActivityBarLocationSetting(), false));
}

minimap::Options CEditWnd::ReadMiniMapSettings() const
{
	minimap::Options options;
	if (m_workbenchRuntime == nullptr) {
		options.enabled = m_pShareData->m_Common.m_sWindow.m_bDispMiniMap;
		return options;
	}
	try {
		const std::vector<std::string> keys{
			"editor.minimap.enabled",
			"editor.minimap.autohide",
			"editor.minimap.side",
			"editor.minimap.size",
			"editor.minimap.showSlider",
			"editor.minimap.renderCharacters",
			"editor.minimap.maxColumn",
			"editor.minimap.scale",
		};
		const auto read = m_workbenchRuntime->Configuration().ReadSnapshot(
			keys, BuildWorkbenchConfigurationTarget());
		if (!read.snapshot || read.snapshot->values.size() != keys.size()) return options;
		const auto& values = read.snapshot->values;
		const auto boolAt = [&values](std::size_t index, bool fallback) {
			if (const auto* value = std::get_if<bool>(&values[index].Value())) return *value;
			return fallback;
		};
		const auto integerAt = [&values](std::size_t index, int fallback) {
			if (const auto* value = std::get_if<std::int64_t>(&values[index].Value())) {
				return static_cast<int>(*value);
			}
			return fallback;
		};
		const auto stringAt = [&values](std::size_t index) -> std::wstring_view {
			if (const auto* value = std::get_if<std::wstring>(&values[index].Value())) return *value;
			return {};
		};
		options.enabled = boolAt(0, options.enabled);
		const auto autohide = stringAt(1);
		if (autohide == L"mouseover") options.autohide = minimap::AutoHide::MouseOver;
		else if (autohide == L"scroll") options.autohide = minimap::AutoHide::Scroll;
		options.side = stringAt(2) == L"left" ? minimap::Side::Left : minimap::Side::Right;
		const auto size = stringAt(3);
		if (size == L"fill") options.size = minimap::Size::Fill;
		else if (size == L"fit") options.size = minimap::Size::Fit;
		options.showSlider = stringAt(4) == L"always"
			? minimap::ShowSlider::Always : minimap::ShowSlider::MouseOver;
		options.renderCharacters = boolAt(5, options.renderCharacters);
		options.maxColumn = std::clamp(integerAt(6, options.maxColumn), 1, 10000);
		options.scale = std::clamp(integerAt(7, options.scale), 1, 3);
	}
	catch (...) {
		return minimap::Options{};
	}
	return options;
}

void CEditWnd::ApplyMiniMapSettings()
{
	const auto next = ReadMiniMapSettings();
	const bool layoutChanged = next.enabled != m_miniMapOptions.enabled
		|| next.side != m_miniMapOptions.side
		|| next.maxColumn != m_miniMapOptions.maxColumn
		|| next.scale != m_miniMapOptions.scale;
	m_miniMapOptions = next;
	m_pShareData->m_Common.m_sWindow.m_bDispMiniMap = next.enabled;
	LayoutMiniMap();
	if (m_cMiniMapView.GetHwnd() != nullptr) {
		m_cMiniMapView.SetMiniMapOptions(next);
	}
	if (layoutChanged && m_dispatchReady && GetHwnd() != nullptr) {
		EndLayoutBars();
	}
}

void CEditWnd::ApplyIndentGuideSettings()
{
	bool enabled = true;
	if( m_workbenchRuntime != nullptr ){
		try {
			const auto lookup = m_workbenchRuntime->Configuration().GetValue(
				"editor.guides.indentation", BuildWorkbenchConfigurationTarget());
			if( lookup.value ){
				if( const auto* value = std::get_if<bool>(&lookup.value->Value()) ){
					enabled = *value;
				}
			}
		}
		catch (...) {
			enabled = true;
		}
	}
	m_indentGuidesEnabled = enabled;
	for( const auto& view : m_pcEditViewArr ){
		if( view != nullptr ) view->SetIndentGuidesEnabled(enabled);
	}
}

bool CEditWnd::PersistMiniMapContextSelection(
	minimap::ContextCommand command, const minimap::Options& options)
{
	if (m_workbenchRuntime == nullptr) return true;
	std::string key;
	config::ConfigurationValue value;
	switch (command) {
	case minimap::ContextCommand::ToggleEnabled:
		key = "editor.minimap.enabled";
		value = config::ConfigurationValue(options.enabled);
		break;
	case minimap::ContextCommand::ToggleRenderCharacters:
		key = "editor.minimap.renderCharacters";
		value = config::ConfigurationValue(options.renderCharacters);
		break;
	case minimap::ContextCommand::SizeProportional:
		key = "editor.minimap.size";
		value = config::ConfigurationValue(L"proportional");
		break;
	case minimap::ContextCommand::SizeFill:
		key = "editor.minimap.size";
		value = config::ConfigurationValue(L"fill");
		break;
	case minimap::ContextCommand::SizeFit:
		key = "editor.minimap.size";
		value = config::ConfigurationValue(L"fit");
		break;
	case minimap::ContextCommand::SliderMouseOver:
		key = "editor.minimap.showSlider";
		value = config::ConfigurationValue(L"mouseover");
		break;
	case minimap::ContextCommand::SliderAlways:
		key = "editor.minimap.showSlider";
		value = config::ConfigurationValue(L"always");
		break;
	case minimap::ContextCommand::SideRight:
		key = "editor.minimap.side";
		value = config::ConfigurationValue(L"right");
		break;
	case minimap::ContextCommand::SideLeft:
		key = "editor.minimap.side";
		value = config::ConfigurationValue(L"left");
		break;
	}
	if (key.empty()) return false;
	try {
		const auto& profile = m_workbenchRuntime->Bootstrap().UserDataProfile();
		config::ConfigurationTarget sourceTarget;
		sourceTarget.profileId = profile.SelectedProfileId();
		const config::ConfigurationSource source{
			config::EConfigurationScope::Profile, sourceTarget, "profile.settings", 0 };
		config::editing::ConfigurationDocumentEditTarget editTarget;
		editTarget.scope = config::editing::EConfigurationDocumentScope::Profile;
		editTarget.target = sourceTarget;
		editTarget.resource = profile.Resources().Settings();
		const config::SettingsWritebackRequest request{
			.edit = {
				.target = std::move(editTarget),
				.key = std::move(key),
				.value = std::move(value),
			},
			.documentKey = "profile.settings",
			.source = source,
		};
		return m_workbenchRuntime->WriteSetting(request).Succeeded();
	}
	catch (...) {
		return false;
	}
}

void CEditWnd::CommitMiniMapOptions(const minimap::Options& options)
{
	const bool layoutChanged = options.enabled != m_miniMapOptions.enabled
		|| options.side != m_miniMapOptions.side
		|| options.maxColumn != m_miniMapOptions.maxColumn
		|| options.scale != m_miniMapOptions.scale;
	m_miniMapOptions = options;
	m_pShareData->m_Common.m_sWindow.m_bDispMiniMap = options.enabled;
	LayoutMiniMap();
	if (m_cMiniMapView.GetHwnd() != nullptr) {
		m_cMiniMapView.SetMiniMapOptions(m_miniMapOptions);
	}
	if (layoutChanged && m_dispatchReady && GetHwnd() != nullptr) EndLayoutBars();
}

bool CEditWnd::SetMiniMapEnabled(bool enabled, bool persist)
{
	if (m_miniMapOptions.enabled == enabled) return true;
	auto next = m_miniMapOptions;
	next.enabled = enabled;
	if (persist && !PersistMiniMapContextSelection(
		minimap::ContextCommand::ToggleEnabled, next)) return false;
	CommitMiniMapOptions(next);
	return true;
}

bool CEditWnd::ApplyMiniMapContextCommand(
	minimap::ContextCommand command, bool persist)
{
	const auto next = minimap::ApplyContextCommand(m_miniMapOptions, command);
	if (next == m_miniMapOptions) return true;
	if (persist && !PersistMiniMapContextSelection(command, next)) return false;
	CommitMiniMapOptions(next);
	return true;
}

void CEditWnd::ApplyScmInputLineCountSetting()
{
	if (m_scmTool == nullptr) return;
	int minLines = 1;
	int maxLines = 10;
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto target = BuildWorkbenchConfigurationTarget();
			const auto read = [&](const char* key, int fallback) {
				const auto lookup = m_workbenchRuntime->Configuration().GetValue(key, target);
				if (lookup.value) {
					if (const auto* value = std::get_if<std::int64_t>(&lookup.value->Value());
						value != nullptr) {
						return static_cast<int>(std::clamp<std::int64_t>(*value, 1, 50));
					}
				}
				return fallback;
			};
			minLines = read("scm.inputMinLineCount", minLines);
			maxLines = read("scm.inputMaxLineCount", maxLines);
		} catch (...) {
			// Commit-box sizing is presentation, so an unreadable setting takes the
			// registered default rather than failing the view that asked for it.
		}
	}
	m_scmTool->SetInputLineCountRange(minLines, maxLines);
}

void CEditWnd::ApplyTerminalShortcutPresetSetting()
{
	if (m_terminalTool == nullptr) return;
	// Mirrors the registered default of `sakura.terminal.shortcutPreset`.
	auto preset = terminal::TerminalShortcutPreset::Screen;
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto lookup = m_workbenchRuntime->Configuration().GetValue(
				"sakura.terminal.shortcutPreset", BuildWorkbenchConfigurationTarget());
			if (lookup.value) {
				if (const auto* value = std::get_if<std::wstring>(&lookup.value->Value());
					value != nullptr) {
					preset = terminal::ParseTerminalShortcutPreset(*value)
						.value_or(terminal::TerminalShortcutPreset::Screen);
				}
			}
		}
		catch (...) {
			// An unreadable setting takes the registered default rather than
			// whatever this window happened to hold.
			preset = terminal::TerminalShortcutPreset::Screen;
		}
	}
	m_terminalTool->SetShortcutPreset(preset);
}

void CEditWnd::ApplyTerminalScrollbackSetting()
{
	if (m_terminalTool == nullptr) return;
	std::size_t lines = terminal::TerminalModel::kDefaultScrollbackLines;
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto lookup = m_workbenchRuntime->Configuration().GetValue(
				"terminal.integrated.scrollback", BuildWorkbenchConfigurationTarget());
			if (lookup.value) {
				if (const auto* value = std::get_if<std::int64_t>(&lookup.value->Value());
					value != nullptr && *value >= 0) {
					lines = static_cast<std::size_t>(*value);
				}
			}
		} catch (...) {
			lines = terminal::TerminalModel::kDefaultScrollbackLines;
		}
	}
	m_terminalTool->SetScrollbackLimit(lines);
}

bool CEditWnd::IsAuxiliaryViewContainerActive(std::string_view containerId) const
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto part = std::ranges::find(snapshot.parts,
			workbench::layout::ids::part::Auxiliarybar,
			&workbench::layout::WorkbenchPartState::partId);
		if (part == snapshot.parts.end() || !part->visible) return false;
		const auto container = std::ranges::find(snapshot.containers, containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end() || !container->visible
			|| container->location
				!= workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar) {
			return false;
		}
		return snapshot.activeContainers.auxiliaryBar
			&& *snapshot.activeContainers.auxiliaryBar == containerId;
	}
	catch (...) {
		return false;
	}
}

void CEditWnd::ApplyTerminalTabPresentationSettings()
{
	if (m_terminalTool == nullptr) return;
	terminal::TerminalTabPresentationSettings settings;
	if (m_workbenchRuntime != nullptr) {
		try {
			// Keep these keys in one request: a title/description/separator policy
			// must never be assembled from different configuration revisions.
			const std::vector<std::string> keys {
				"terminal.integrated.tabs.title",
				"terminal.integrated.tabs.description",
				"terminal.integrated.tabs.separator",
				"terminal.integrated.tabs.allowAgentCliTitle",
				"terminal.integrated.tabs.enabled",
				"terminal.integrated.tabs.hideCondition",
				"terminal.integrated.tabs.showActiveTerminal",
				"terminal.integrated.tabs.showActions",
				"terminal.integrated.tabs.location",
			};
			const auto read = m_workbenchRuntime->Configuration().ReadSnapshot(
				keys, BuildWorkbenchConfigurationTarget());
			if (read.snapshot && read.snapshot->values.size() == keys.size()) {
				const auto& values = read.snapshot->values;
				const auto stringValue = [&values](std::size_t index, const std::wstring& fallback) {
					if (index < values.size()) {
						if (const auto* value = std::get_if<std::wstring>(&values[index].Value());
							value != nullptr) return *value;
					}
					return fallback;
				};
				const auto boolValue = [&values](std::size_t index, bool fallback) {
					if (index < values.size()) {
						if (const auto* value = std::get_if<bool>(&values[index].Value());
							value != nullptr) return *value;
					}
					return fallback;
				};
				settings.titleTemplate = stringValue(0, settings.titleTemplate);
				settings.descriptionTemplate = stringValue(1, settings.descriptionTemplate);
				settings.separator = stringValue(2, settings.separator);
				settings.allowAgentCliTitle = boolValue(3, settings.allowAgentCliTitle);
				settings.tabsEnabled = boolValue(4, settings.tabsEnabled);
				if (const auto parsed = terminal::ParseTerminalTabsHideCondition(stringValue(5, L"singleTerminal"))) {
					settings.hideCondition = *parsed;
				}
				if (const auto parsed = terminal::ParseTerminalTabsShowCondition(
					stringValue(6, L"singleTerminalOrNarrow"))) {
					settings.showActiveTerminal = *parsed;
				}
				if (const auto parsed = terminal::ParseTerminalTabsShowCondition(
					stringValue(7, L"singleTerminalOrNarrow"))) {
					settings.showActions = *parsed;
				}
				if (const auto parsed = terminal::ParseTerminalTabsLocation(stringValue(8, L"right"))) {
					settings.location = *parsed;
				}
			}
		}
		catch (const std::exception&) {
			// Presentation projection fails closed to the registered defaults. It
			// never tears down or restarts a running PTY.
		}
	}
	m_terminalTool->SetTabPresentationSettings(std::move(settings));
}

bool CEditWnd::PersistTerminalShortcutPresetSelection(terminal::TerminalShortcutPreset preset)
{
	if (m_workbenchRuntime == nullptr) return false;
	try {
		const auto& profile = m_workbenchRuntime->Bootstrap().UserDataProfile();
		config::ConfigurationTarget sourceTarget;
		sourceTarget.profileId = profile.SelectedProfileId();
		const config::ConfigurationSource source {
			config::EConfigurationScope::Profile,
			sourceTarget,
			"profile.settings",
			0,
		};
		config::editing::ConfigurationDocumentEditTarget editTarget;
		editTarget.scope = config::editing::EConfigurationDocumentScope::Profile;
		editTarget.target = sourceTarget;
		editTarget.resource = profile.Resources().Settings();
		const config::SettingsWritebackRequest request {
			.edit = {
				.target = std::move(editTarget),
				.key = "sakura.terminal.shortcutPreset",
				.value = config::ConfigurationValue(
					std::wstring(terminal::TerminalShortcutPresetId(preset))),
			},
			.documentKey = "profile.settings",
			.source = source,
		};
		return m_workbenchRuntime->WriteSetting(request).Succeeded();
	}
	catch (...) {
		// The in-memory selection already took effect; only its persistence failed.
		return false;
	}
}

void CEditWnd::ApplyExplorerDecorationSettings()
{
	bool enabled = true;
	workbench::explorer::ExplorerDecorationOptions options;
	if (m_workbenchRuntime != nullptr) {
		try {
			const auto target = BuildWorkbenchConfigurationTarget();
			const auto read = [&](const char* key, bool fallback) {
				const auto lookup = m_workbenchRuntime->Configuration().GetValue(key, target);
				if (lookup.value) {
					if (const auto* value = std::get_if<bool>(&lookup.value->Value()); value != nullptr) {
						return *value;
					}
				}
				return fallback;
			};
			enabled = read("git.decorations.enabled", enabled);
			options.colors = read("explorer.decorations.colors", options.colors);
			options.badges = read("explorer.decorations.badges", options.badges);
		} catch (...) {
			// Decoration rendering is presentation, so an unreadable setting takes
			// the registered default rather than failing the view that asked for it.
		}
	}
	m_gitDecorationsEnabled = enabled;
	if (m_explorerTool != nullptr) m_explorerTool->SetDecorationOptions(options);
	// Turning the provider off has to retract what it already published; upstream's
	// provider disposal fires a change for every URI it had decorated.
	if (!enabled && m_explorerTool != nullptr) {
		m_explorerTool->SetFileDecorations({});
	} else if (enabled && m_scmTool != nullptr) {
		// Re-publish what git already reported instead of re-running it: the
		// repository did not change, only what this window renders from it.
		m_scmTool->RepublishFileDecorations();
	}
}

std::wstring CEditWnd::ReadScmCountBadgeSetting() const
{
	if (m_workbenchRuntime == nullptr) return L"all";
	try {
		const auto target = BuildWorkbenchConfigurationTarget();
		const auto lookup = m_workbenchRuntime->Configuration().GetValue("scm.countBadge", target);
		if (lookup.value) {
			if (const auto* selected = std::get_if<std::wstring>(&lookup.value->Value());
				selected != nullptr && !selected->empty()) {
				return *selected;
			}
		}
	} catch (...) {
		// The badge is presentation, so an unreadable setting takes the registered
		// default rather than failing the projection that asked for it.
	}
	return L"all";
}

std::optional<workbench::WorkbenchEdge> CEditWnd::HitTestSideBarEdge(POINT screenPoint) const
{
	const auto covers = [&screenPoint](const workbench::CWorkbenchPanelHost* host) {
		if (host == nullptr || host->GetHwnd() == nullptr) return false;
		if (host->GetState() == workbench::WorkbenchPanelState::Hidden) return false;
		RECT bounds{};
		if (::GetWindowRect(host->GetHwnd(), &bounds) == FALSE) return false;
		return ::PtInRect(&bounds, screenPoint) != FALSE;
	};
	if (covers(m_rightWorkbenchPanel.get())) return workbench::WorkbenchEdge::Right;
	if (covers(m_leftWorkbenchPanel.get())) return workbench::WorkbenchEdge::Left;

	// A hidden Secondary Side Bar still has to accept a drop, otherwise a container could
	// never be moved into it. VS Code shows an edge drop zone for exactly that case, so a
	// release inside the strip along either edge of the frame targets that side bar.
	HWND frame = GetHwnd();
	if (frame == nullptr) return std::nullopt;
	RECT client{};
	if (::GetClientRect(frame, &client) == FALSE) return std::nullopt;
	POINT clientPoint = screenPoint;
	if (::ScreenToClient(frame, &clientPoint) == FALSE) return std::nullopt;
	if (::PtInRect(&client, clientPoint) == FALSE) return std::nullopt;
	const unsigned int dpi = m_leftWorkbenchPanel ? m_leftWorkbenchPanel->GetDpi() : 96u;
	const int strip = ::MulDiv(kSideBarDropEdgeDip, static_cast<int>(dpi), 96);
	if (clientPoint.x >= client.right - strip) return workbench::WorkbenchEdge::Right;
	if (clientPoint.x <= client.left + strip) return workbench::WorkbenchEdge::Left;
	return std::nullopt;
}

void CEditWnd::MoveViewContainerToEdge(const std::string_view containerId,
	workbench::WorkbenchEdge edge)
{
	// Only the two side bars are composite drop targets here. Moving a ViewContainer into
	// the Panel is VS Code's separate `workbench.action.movePanelTo*` family and is not
	// approximated by this gesture.
	if (edge != workbench::WorkbenchEdge::Left && edge != workbench::WorkbenchEdge::Right) return;
	if (m_workbenchRuntime == nullptr || containerId.empty()) return;
	const auto location = edge == workbench::WorkbenchEdge::Right
		? workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar
		: workbench::layout::EWorkbenchViewContainerLocation::SideBar;
	try {
		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto container = std::ranges::find(snapshot.containers, containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end() || container->location == location) return;
		if (m_paneCompositeProjection == nullptr) return;
		const auto supportedLocations =
			m_paneCompositeProjection->SupportedLocations(containerId);
		if (!supportedLocations.complete || !supportedLocations.Contains(location)) return;
		const std::string_view partId = location
			== workbench::layout::EWorkbenchViewContainerLocation::SideBar
			? workbench::layout::ids::part::Sidebar
			: workbench::layout::ids::part::Auxiliarybar;
		const auto part = std::ranges::find(snapshot.parts, partId,
			&workbench::layout::WorkbenchPartState::partId);
		if (part == snapshot.parts.end()) return;

		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		auto operationId = NextWorkbenchLayoutOperationId("move-view-container");
		if (!operationId) return;
		std::vector<workbench::layout::WorkbenchLayoutTransactionChange> changes;
		changes.emplace_back(workbench::layout::WorkbenchLayoutMoveContainerChange{
			.containerId = std::string(containerId),
			.location = location,
			.order = container->order,
		});
		changes.emplace_back(workbench::layout::WorkbenchLayoutActivateContainerChange{
			.containerId = std::string(containerId),
		});
		if (!part->visible) {
			changes.emplace_back(workbench::layout::WorkbenchLayoutSetPartVisibilityChange{
				.partId = std::string(partId),
				.visible = true,
			});
		}
		changes.emplace_back(workbench::layout::WorkbenchLayoutSetFocusChange{
			.focus = {
				.partId = std::string(partId),
				.containerId = std::string(containerId),
				.viewId = container->activeViewId,
			},
		});
		const auto result = m_workbenchRuntime->LayoutState().ApplyTransaction({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.changes = std::move(changes),
		});
		if (result.status != workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			&& result.status != workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
			return;
		}

		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: view container move projection failed.\n");
			return;
		}
		if (mirrorChanged) BroadcastWorkbenchSettings();
	}
	catch (...) {
	}
}

bool CEditWnd::IsSecondarySidebarVisible() const noexcept
{
	return m_rightWorkbenchPanel != nullptr
		&& m_rightWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;
}

void CEditWnd::ToggleSecondarySidebar(bool activate)
{
	if (!m_rightWorkbenchPanel) return;
	if (m_workbenchRuntime != nullptr) {
		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		bool visible = false;
		try {
			const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
			const auto part = std::ranges::find(snapshot.parts,
				workbench::layout::ids::part::Auxiliarybar,
				&workbench::layout::WorkbenchPartState::partId);
			if (part == snapshot.parts.end()) return;
			visible = part->visible;
		}
		catch (...) {
			return;
		}
		// The Secondary Side Bar is empty by default, so toggling it is purely a Part
		// visibility change; there is no ViewContainer to activate inside it.
		if (!SetBuiltinPartVisibility(workbench::layout::ids::part::Auxiliarybar, !visible)) return;
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: Auxiliary Bar projection failed.\n");
			return;
		}
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return;
	}
	const bool visible = m_rightWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	settings.m_bAuxiliaryBarVisible = visible ? FALSE : TRUE;
	if (visible) {
		m_rightWorkbenchPanel->Hide();
	} else {
		m_rightWorkbenchPanel->Show();
	}
	// The Secondary Side Bar holds no Activity Bar ViewContainer, so its visibility must
	// never change the Activity Bar selection; that selection belongs to the Primary Side Bar.
	if (GetHwnd()) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}
	if (!visible && activate) m_rightWorkbenchPanel->ActivateTool();
	BroadcastWorkbenchSettings();
}

void CEditWnd::ToggleWorkbenchPanel(workbench::WorkbenchEdge edge, bool activate)
{
	const bool show = !IsWorkbenchPanelVisible(edge);
	SetWorkbenchPanelVisible(edge, show, activate);
}

void CEditWnd::ToggleBottomWorkbenchMaximized()
{
	if (m_bottomWorkbenchPanel == nullptr
		|| m_bottomWorkbenchPanel->GetState() == workbench::WorkbenchPanelState::Hidden) {
		return;
	}
	m_bottomWorkbenchMaximized = !m_bottomWorkbenchMaximized;
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}
}

EWorkspaceWindowTransitionResult CEditWnd::LaunchWorkspaceTarget(
	const std::wstring& commandLineOption, bool closeCurrentWindow, const std::wstring& stagedTargetToDeleteOnFailure)
{
	bool retainStagedTargetForUnresolvedChild = false;
	CWorkspaceWindowTransitionCallbackHost host;
	host.prepare = [this]() { return PrepareWorkspaceReplacement(); };
	host.launch = [this, &commandLineOption, &stagedTargetToDeleteOnFailure, &retainStagedTargetForUnresolvedChild]() {
		SLoadInfo loadInfo;
		loadInfo.cFilePath = L"";
		loadInfo.eCharCode = CODE_NONE;
		loadInfo.bViewMode = false;
		// sync=true owns the existing bounded (15 second) successor-ready IPC
		// observation in CControlTray::OpenNewEditor.
		auto launched = CControlTray::OpenNewEditorWithResult(G_AppInstance(), GetHwnd(), loadInfo,
			commandLineOption.empty() ? nullptr : commandLineOption.c_str(), true, nullptr, true,
			true);
		if (launched.outcome == EOpenNewEditorOutcome::FailedChildOwnershipTransferred) {
			retainStagedTargetForUnresolvedChild = !stagedTargetToDeleteOnFailure.empty();
			CFailedEditorProcessOwner::Retain(launched.transferredProcessHandle);
		}
		return launched.Succeeded()
			? EWorkspaceWindowTransitionResult::Succeeded : EWorkspaceWindowTransitionResult::Failed;
	};
	host.close = [this]() { return CloseWorkspaceWindowOnce(); };
	host.cleanup = [&stagedTargetToDeleteOnFailure, &retainStagedTargetForUnresolvedChild]() {
		if (stagedTargetToDeleteOnFailure.empty()) return EWorkspaceWindowTransitionResult::Succeeded;
		if (retainStagedTargetForUnresolvedChild) {
			// The successor may still be consuming this managed file.  Retention is
			// the safe terminal cleanup state; a later run may reclaim it after the
			// retained process handle becomes signalled.
			return EWorkspaceWindowTransitionResult::Succeeded;
		}
		// OpenNewEditor does not return a timed-out managed launch until the
		// successor has stopped. A CreateProcess failure has no child, so in
		// either failure mode no process can still be opening this resource.
		return (::DeleteFileW(stagedTargetToDeleteOnFailure.c_str()) != FALSE
			|| ::GetLastError() == ERROR_FILE_NOT_FOUND)
			? EWorkspaceWindowTransitionResult::Succeeded : EWorkspaceWindowTransitionResult::Failed;
	};
	const auto transition = !stagedTargetToDeleteOnFailure.empty()
		? (closeCurrentWindow
			? workbench::workspace::CWorkspaceWindowTransitionPlanner::ManagedReplacement()
			: workbench::workspace::CWorkspaceWindowTransitionPlanner::ManagedDuplicate())
		: (closeCurrentWindow
			? workbench::workspace::CWorkspaceWindowTransitionPlanner::SaveAsReplacement()
			: workbench::workspace::WorkspaceWindowTransitionRequest{});
	return ToWindowTransitionResult(workbench::workspace::CWorkspaceWindowTransitionService::Execute(transition, host));
}

EWorkspaceWindowTransitionResult CEditWnd::PrepareWorkspaceReplacement()
{
	if (!HasActiveEditorInput() || !GetDocument()->m_cDocEditor.IsModified()) {
		return EWorkspaceWindowTransitionResult::Succeeded;
	}
	// This is the authoritative legacy save/discard/cancel dialog.  It is a
	// non-destructive preflight: CommitFileClose is deliberately deferred until
	// the successor has reported ready.
	if (GetDocument()->m_cDocFileOperation.PrepareFileClose(false)) {
		return EWorkspaceWindowTransitionResult::Succeeded;
	}
	switch (GetDocument()->m_cDocFileOperation.GetLastCloseResult()) {
	case EDocFileOperationResult::Cancelled: return EWorkspaceWindowTransitionResult::Cancelled;
	case EDocFileOperationResult::Failed: return EWorkspaceWindowTransitionResult::Failed;
	case EDocFileOperationResult::Succeeded: break;
	}
	return EWorkspaceWindowTransitionResult::Failed;
}

EWorkspaceWindowTransitionResult CEditWnd::CloseWorkspaceWindowOnce()
{
	const HWND window = GetHwnd();
	if (window == nullptr) return EWorkspaceWindowTransitionResult::Failed;
	// OnClose consumes this one-shot token after the real preflight above.  This
	// prevents a discarded dirty buffer from producing a second confirmation.
	m_workspaceReplacementClosePreflightAccepted = true;
	if (::PostMessage(window, WM_CLOSE, 0, 0) != FALSE) return EWorkspaceWindowTransitionResult::Succeeded;
	m_workspaceReplacementClosePreflightAccepted = false;
	return EWorkspaceWindowTransitionResult::Failed;
}

EWorkspaceWindowTransitionResult CEditWnd::OpenWorkspaceConfiguration()
{
	std::array<WCHAR, 32768> selected{};
	CDlgOpenFile dialog;
	dialog.Create(G_AppInstance(), GetHwnd(), L"*.code-workspace", L"");
	if (!dialog.DoModal_GetOpenFileName(selected, EFITER_NONE)) return EWorkspaceWindowTransitionResult::Cancelled;
	const auto path = MakeAbsolutePath(selected.data());
	if (path.empty() || _wcsicmp(std::filesystem::path(path).extension().c_str(), L".code-workspace") != 0) {
		return EWorkspaceWindowTransitionResult::Failed;
	}
	const auto target = platform::uri::Uri::FromWindowsPath(path);
	if (!target) return EWorkspaceWindowTransitionResult::Failed;
	return LaunchWorkspaceTarget(L"-WORKSPACE=\"" + path + L"\"", true);
}

EWorkspaceWindowTransitionResult CEditWnd::AddFolderToWorkspace()
{
	if (m_workbenchRuntime == nullptr || m_workbenchRuntime->WorkspaceEditing() == nullptr) {
		return EWorkspaceWindowTransitionResult::Failed;
	}
	const auto current = m_workbenchRuntime->WorkspaceContext().Snapshot();
	std::vector<std::wstring> selectedPaths;
	switch (SelectDirsWithResult(GetHwnd(), LS(F_ADD_FOLDER_TO_WORKSPACE), GetSemanticWorkspaceRoot(), selectedPaths)) {
	case ESelectDirResult::Succeeded: break;
	case ESelectDirResult::Cancelled: return EWorkspaceWindowTransitionResult::Cancelled;
	case ESelectDirResult::Failed: return EWorkspaceWindowTransitionResult::Failed;
	}
	std::vector<std::pair<std::wstring, platform::uri::Uri>> selectedFolders;
	selectedFolders.reserve(selectedPaths.size());
	for (const auto& selectedPath : selectedPaths) {
		const auto path = MakeAbsolutePath(selectedPath);
		const auto folder = platform::uri::Uri::FromWindowsPath(path);
		if (path.empty() || !folder) return EWorkspaceWindowTransitionResult::Failed;
		selectedFolders.emplace_back(path, std::move(*folder.value));
	}
	if (selectedFolders.empty()) return EWorkspaceWindowTransitionResult::Failed;
	std::vector<workbench::workspace::WorkspaceFolderEdit> additionalFolders;
	additionalFolders.reserve(selectedFolders.size());
	for (auto& [path, folder] : selectedFolders) {
		(void)path;
		additionalFolders.push_back({ std::move(folder), std::nullopt });
	}
	// A saved multi-root workspace keeps its identity and is CAS-updated in
	// place, exactly like VS Code.  Empty/Folder states cross the identity
	// boundary into a managed untitled workspace and therefore use a successor.
	if (current.kind == config::EWorkspaceKind::Workspace) {
		if (!current.workspaceConfigUri) return EWorkspaceWindowTransitionResult::Failed;
		auto request = workbench::workspace::CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(
			current, *current.workspaceConfigUri, std::move(additionalFolders));
		const auto edited = m_workbenchRuntime->ReplaceCurrentWorkspaceFolders(request);
		if (edited.outcome != workbench::workspace::EWorkspaceEditingOutcome::Succeeded) {
			return EWorkspaceWindowTransitionResult::Failed;
		}
		ApplySemanticWorkspaceContext();
		if (!RefreshWorkbenchCommandContext()) return EWorkspaceWindowTransitionResult::Failed;
		return EWorkspaceWindowTransitionResult::Succeeded;
	}
	const auto managedPath = CreateManagedWorkspacePath();
	if (!managedPath) return EWorkspaceWindowTransitionResult::Failed;
	const auto managed = platform::uri::Uri::FromWindowsPath(*managedPath);
	if (!managed) return EWorkspaceWindowTransitionResult::Failed;
	auto request = workbench::workspace::CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(
		current, *managed.value, std::move(additionalFolders));
	const auto edited = m_workbenchRuntime->WorkspaceEditing()->ReplaceFolders(request);
	if (edited.outcome != workbench::workspace::EWorkspaceEditingOutcome::Succeeded) {
		(void)::DeleteFileW(managedPath->c_str());
		return EWorkspaceWindowTransitionResult::Failed;
	}
	return LaunchWorkspaceTarget(L"-WORKSPACE=\"" + *managedPath + L"\"", true, *managedPath);
}

EWorkspaceWindowTransitionResult CEditWnd::SaveWorkspaceAs()
{
	if (m_workbenchRuntime == nullptr || m_workbenchRuntime->WorkspaceEditing() == nullptr) {
		return EWorkspaceWindowTransitionResult::Failed;
	}
	const auto current = m_workbenchRuntime->WorkspaceContext().Snapshot();
	std::array<WCHAR, 32768> selected{};
	std::wstring initial = L"workspace.code-workspace";
	if (current.workspaceConfigUri) {
		if (const auto existing = current.workspaceConfigUri->ToWindowsPath(); existing.value) initial = *existing.value;
	}
	::wcsncpy_s(selected.data(), selected.size(), initial.c_str(), _TRUNCATE);
	CDlgOpenFile dialog;
	dialog.Create(G_AppInstance(), GetHwnd(), L"*.code-workspace", initial.c_str());
	if (!dialog.DoModal_GetSaveFileName(selected)) return EWorkspaceWindowTransitionResult::Cancelled;
	auto path = MakeAbsolutePath(selected.data());
	if (path.empty()) return EWorkspaceWindowTransitionResult::Failed;
	if (_wcsicmp(std::filesystem::path(path).extension().c_str(), L".code-workspace") != 0) {
		path += L".code-workspace";
	}
	const auto target = platform::uri::Uri::FromWindowsPath(path);
	if (!target) return EWorkspaceWindowTransitionResult::Failed;
	auto request = workbench::workspace::CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(current, *target.value);
	const auto edited = m_workbenchRuntime->WorkspaceEditing()->ReplaceFolders(request);
	if (edited.outcome != workbench::workspace::EWorkspaceEditingOutcome::Succeeded) {
		return EWorkspaceWindowTransitionResult::Failed;
	}
	return LaunchWorkspaceTarget(L"-WORKSPACE=\"" + path + L"\"", true);
}

EWorkspaceWindowTransitionResult CEditWnd::DuplicateWorkspaceInNewWindow()
{
	if (m_workbenchRuntime == nullptr || m_workbenchRuntime->WorkspaceEditing() == nullptr) return EWorkspaceWindowTransitionResult::Failed;
	const auto current = m_workbenchRuntime->WorkspaceContext().Snapshot();
	const auto managedPath = CreateManagedWorkspacePath();
	if (!managedPath) return EWorkspaceWindowTransitionResult::Failed;
	const auto managed = platform::uri::Uri::FromWindowsPath(*managedPath);
	if (!managed) return EWorkspaceWindowTransitionResult::Failed;
	auto request = workbench::workspace::CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(current, *managed.value);
	const auto edited = m_workbenchRuntime->WorkspaceEditing()->ReplaceFolders(request);
	if (edited.outcome != workbench::workspace::EWorkspaceEditingOutcome::Succeeded) {
		(void)::DeleteFileW(managedPath->c_str());
		return EWorkspaceWindowTransitionResult::Failed;
	}
	return LaunchWorkspaceTarget(L"-WORKSPACE=\"" + *managedPath + L"\"", false, *managedPath);
}

EWorkspaceWindowTransitionResult CEditWnd::CloseWorkspaceWindow()
{
	if (m_workbenchRuntime == nullptr
		|| m_workbenchRuntime->WorkspaceContext().Snapshot().kind == config::EWorkspaceKind::Empty) {
		return EWorkspaceWindowTransitionResult::Cancelled;
	}
	return LaunchWorkspaceTarget({}, true);
}

bool CEditWnd::HasRecentlyOpenedItems() const
{
	std::vector<workbench::recent::RecentlyOpenedWorkspaceEntry> entries;
	if (m_workbenchRuntime != nullptr) {
		if (const auto recent = m_workbenchRuntime->RecentlyOpenedWorkspaces(); recent != nullptr) {
			entries = recent->Snapshot();
		}
	}
	const CMRUFile legacyFiles;
	return workbench::recent::CRecentlyOpenedWorkspaceMenuProjection::HasItems(entries,
		legacyFiles.MenuLength() > 0);
}

bool CEditWnd::AppendRecentlyOpenedWorkspaceMenu(HMENU hMenu, bool hasRecentFiles)
{
	m_recentlyOpenedWorkspaceMenuSnapshot.clear();
	if (m_workbenchRuntime == nullptr || hMenu == nullptr) return false;
	const auto recent = m_workbenchRuntime->RecentlyOpenedWorkspaces();
	if (recent == nullptr) return false;
	m_recentlyOpenedWorkspaceMenuSnapshot = recent->Snapshot();
	const auto rows = workbench::recent::CRecentlyOpenedWorkspaceMenuProjection::Build(
		m_recentlyOpenedWorkspaceMenuSnapshot, hasRecentFiles,
		LocalizedWorkbenchString(STR_WORKBENCH_RECENT_WORKSPACE_LABEL));
	AppendRecentlyOpenedWorkspaceMenuRows(hMenu, rows);
	return !m_recentlyOpenedWorkspaceMenuSnapshot.empty();
}

void CEditWnd::AppendRecentlyOpenedWorkspaceMenuRows(HMENU hMenu,
	const std::vector<workbench::recent::RecentlyOpenedWorkspaceMenuRow>& rows)
{
	for (const auto& row : rows) {
		if (row.kind == workbench::recent::ERecentlyOpenedWorkspaceMenuRowKind::Separator) {
			(void)::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			continue;
		}
		std::wstring label;
		label.reserve(row.label.size());
		for (const auto character : row.label) {
			label.push_back(character);
			if (character == L'&') label.push_back(L'&');
		}
		m_cMenuDrawer.MyAppendMenu(hMenu, MF_BYPOSITION | MF_STRING,
			static_cast<UINT_PTR>(row.commandId), label.c_str(), L"");
	}
}

void CEditWnd::AppendClearRecentlyOpenedMenuItem(HMENU hMenu, bool hasPrecedingRows)
{
	if (hMenu == nullptr) return;
	AppendRecentlyOpenedWorkspaceMenuRows(hMenu,
		workbench::recent::CRecentlyOpenedWorkspaceMenuProjection::BuildTrailing(
			hasPrecedingRows, F_CLEAR_RECENT_WORKSPACES, LS(F_CLEAR_RECENT_WORKSPACES)));
}

EWorkspaceWindowTransitionResult CEditWnd::ClearRecentlyOpenedHistory()
{
	// VS Code confirms once for the whole history. Declining is an explicit
	// cancellation that must leave every history untouched.
	if (IDYES != ::MYMESSAGEBOX(GetHwnd(), MB_YESNO | MB_APPLMODAL | MB_ICONQUESTION,
			GSTR_APPNAME, LocalizedWorkbenchString(STR_WORKBENCH_RECENT_CLEAR_CONFIRM).c_str())) {
		return EWorkspaceWindowTransitionResult::Cancelled;
	}

	// The typed Folder/Workspace history and the Sakura-native file and folder
	// MRUs are separate authorities; upstream clears the whole Open Recent
	// surface, so all three are cleared and only a typed store failure is fatal.
	bool succeeded = true;
	if (m_workbenchRuntime != nullptr) {
		if (const auto recent = m_workbenchRuntime->RecentlyOpenedWorkspaces(); recent != nullptr) {
			succeeded = recent->Clear().outcome == workbench::recent::ERecentlyOpenedWorkspaceOutcome::Succeeded;
		}
	}
	CMRUFile().ClearAll();
	CMRUFolder().ClearAll();
	m_recentlyOpenedWorkspaceMenuSnapshot.clear();
	(void)RefreshWorkbenchCommandContext();
	return succeeded ? EWorkspaceWindowTransitionResult::Succeeded : EWorkspaceWindowTransitionResult::Failed;
}

void CEditWnd::RecordRecentlyOpenedWorkspaceAfterReady(
	workbench::recent::ERecentlyOpenedWorkspaceKind kind, const platform::uri::Uri& uri)
{
	if (m_workbenchRuntime == nullptr) return;
	const auto recent = m_workbenchRuntime->RecentlyOpenedWorkspaces();
	if (recent != nullptr) {
		const auto recorded = recent->RecordSuccessfulOpen({ kind, uri, std::nullopt });
		if (recorded.outcome != workbench::recent::ERecentlyOpenedWorkspaceOutcome::Succeeded) {
			::OutputDebugStringW(L"Sakura Editor NEXT: recently opened workspace history update failed.\n");
		}
	}
	if (const auto projects = m_workbenchRuntime->Projects(); projects != nullptr) {
		// Runtime startup deliberately treats an unavailable control-storage cache
		// as non-fatal.  RecordCurrentWorkspaceAfterReady is the one bounded
		// post-ready retry point: by then the control handshake has completed, and
		// a coherent load must precede the first catalog write.
		if (projects->State() != workbench::projects::EProjectCatalogState::Ready) {
			const auto loaded = projects->Load();
			if (loaded.outcome != workbench::projects::EProjectCatalogOutcome::Succeeded) {
				::OutputDebugStringW(L"Sakura Editor NEXT: post-ready project catalog load failed.\n");
			}
		}
		if (projects->State() == workbench::projects::EProjectCatalogState::Ready) {
			const auto projectKind = kind == workbench::recent::ERecentlyOpenedWorkspaceKind::Folder
				? workbench::projects::EProjectKind::Folder
				: workbench::projects::EProjectKind::Workspace;
			const auto recorded = projects->RecordSuccessfulOpen({ projectKind, uri, std::nullopt });
			if (recorded.outcome != workbench::projects::EProjectCatalogOutcome::Succeeded) {
				::OutputDebugStringW(L"Sakura Editor NEXT: project catalog update failed.\n");
			}
		}
	}
	if (m_viewContainerPages != nullptr) {
		m_viewContainerPages->RefreshPageContent(
			workbench::layout::ids::viewContainer::Projects);
	}
	(void)RefreshWorkbenchCommandContext();
}

bool CEditWnd::ReorderViewContainerInActivityBar(
	const std::string_view containerId,
	const workbench::layout::EWorkbenchViewContainerLocation location,
	const workbench::CActivityBar& activityBar,
	const POINT screenPoint)
{
	const auto insertionIndex = activityBar.ContainerInsertionIndexAtScreenPoint(screenPoint);
	if (!insertionIndex) return false;
	if (m_workbenchRuntime == nullptr || containerId.empty()) return true;
	try {
		const auto reordered = workbench::activity::ReorderActivityBarContainers(
			activityBar.GetEntries(), containerId, *insertionIndex);
		if (!reordered) return true;

		const auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto contributions = m_workbenchRuntime->Contributions().Snapshot();
		std::vector<const workbench::layout::WorkbenchViewContainerState*> locationStates;
		for (const auto& state : snapshot.containers) {
			if (state.location != location) continue;
			const auto contribution = std::ranges::find_if(contributions.viewContainers,
				[&state](const auto& entry) { return entry.descriptor.id == state.containerId; });
			if (contribution != contributions.viewContainers.end()) locationStates.push_back(&state);
		}
		std::ranges::sort(locationStates, [](const auto* left, const auto* right) {
			if (left->order != right->order) return left->order < right->order;
			return left->containerId < right->containerId;
		});
		if (locationStates.size() > workbench::layout::kMaxWorkbenchLayoutTransactionChanges) return true;
		std::vector<std::string> locationOrder;
		locationOrder.reserve(locationStates.size());
		for (const auto* state : locationStates) locationOrder.push_back(state->containerId);
		const auto originalOrder = locationOrder;
		const auto source = std::ranges::find(locationOrder, containerId);
		const auto visibleSource = std::ranges::find(*reordered, containerId);
		if (source == locationOrder.end() || visibleSource == reordered->end()) return true;
		const auto visibleIndex = static_cast<std::size_t>(visibleSource - reordered->begin());
		locationOrder.erase(source);
		if (visibleIndex + 1 < reordered->size()) {
			const auto next = std::ranges::find(locationOrder, (*reordered)[visibleIndex + 1]);
			if (next == locationOrder.end()) return true;
			locationOrder.insert(next, std::string(containerId));
		} else if (visibleIndex > 0) {
			const auto previous = std::ranges::find(locationOrder, (*reordered)[visibleIndex - 1]);
			if (previous == locationOrder.end()) return true;
			locationOrder.insert(previous + 1, std::string(containerId));
		} else {
			locationOrder.insert(locationOrder.begin(), std::string(containerId));
		}
		if (locationOrder == originalOrder) return true;

		std::vector<workbench::layout::WorkbenchLayoutTransactionChange> changes;
		changes.reserve(locationOrder.size());
		constexpr std::int32_t kOrderStep = 10;
		for (std::size_t index = 0; index < locationOrder.size(); ++index) {
			const auto state = std::ranges::find(snapshot.containers, locationOrder[index],
				&workbench::layout::WorkbenchViewContainerState::containerId);
			if (state == snapshot.containers.end() || state->location != location) return true;
			const auto order = static_cast<std::int32_t>(index) * kOrderStep;
			if (state->order == order) continue;
			changes.emplace_back(workbench::layout::WorkbenchLayoutMoveContainerChange{
				.containerId = locationOrder[index],
				.location = location,
				.order = order,
			});
		}
		if (changes.empty()) return true;
		auto operationId = NextWorkbenchLayoutOperationId("reorder-view-container");
		if (!operationId) return true;
		const auto result = m_workbenchRuntime->LayoutState().ApplyTransaction({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.changes = std::move(changes),
		});
		if (result.status != workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			&& result.status != workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
			return true;
		}
		bool mirrorChanged = false;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: Activity Bar reorder projection failed.\n");
			return true;
		}
		if (mirrorChanged) BroadcastWorkbenchSettings();
	} catch (...) {
	}
	return true;
}

void CEditWnd::RecordCurrentWorkspaceAfterReady()
{
	if (m_workbenchRuntime == nullptr) return;
	const auto current = m_workbenchRuntime->WorkspaceContext().Snapshot();
	if (current.kind == config::EWorkspaceKind::Workspace && current.workspaceConfigUri) {
		RecordRecentlyOpenedWorkspaceAfterReady(
			workbench::recent::ERecentlyOpenedWorkspaceKind::Workspace, *current.workspaceConfigUri);
	} else if (current.kind == config::EWorkspaceKind::Folder && !current.folders.empty()) {
		RecordRecentlyOpenedWorkspaceAfterReady(
			workbench::recent::ERecentlyOpenedWorkspaceKind::Folder, current.folders.front().uri);
	}
}

void CEditWnd::RevealExplorerAfterWorkspaceCommit()
{
	if (m_explorerTool != nullptr) m_explorerTool->SetFilesPaneExpanded(true);
	if (!SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Left, true, true)) {
		// WorkspaceContext is already committed. Revealing the Explorer is an
		// advisory presentation step, so a native reveal failure cannot be exposed
		// as a rejected switch that purportedly kept the previous workspace.
		::OutputDebugStringW(L"Sakura Editor NEXT: Project Explorer reveal failed after workspace commit.\n");
	}
}

EOpenWorkspaceFolderResult CEditWnd::ApplyFolderWorkspace(
	const std::wstring& absoluteRoot, bool revealExplorer)
{
	if (m_projectWorkspaceTransitionInProgress) {
		return EOpenWorkspaceFolderResult::WorkspaceContextFailed;
	}
	ScopedBooleanState transition(m_projectWorkspaceTransitionInProgress);
	if (m_workspaceContext == nullptr || absoluteRoot.empty()) {
		return EOpenWorkspaceFolderResult::WorkspaceContextFailed;
	}
	auto folderUri = platform::uri::Uri::FromWindowsPath(absoluteRoot);
	if (!folderUri) return EOpenWorkspaceFolderResult::WorkspaceContextFailed;

	const bool terminalWasVisible = IsWorkbenchPanelVisible(workbench::WorkbenchEdge::Bottom)
		&& (m_workbenchRuntime != nullptr
			? IsBuiltinWorkbenchViewActive(workbench::layout::ids::view::Terminal)
			: m_pShareData->m_Common.m_sWorkbench.m_eActiveTool == WORKBENCH_TOOL_TERMINAL);
	const auto previousRoot = GetSemanticWorkspaceRoot();
	config::WorkspaceContextSnapshot before;
	std::optional<std::wstring> targetIdentity;
	std::wstring oldTerminalDirectory;
	bool targetProjectionExisted = false;
	bool terminalPrepared = false;
	terminal::TerminalWorkspaceSwitchOutcome terminalOutcome =
		terminal::TerminalWorkspaceSwitchOutcome::Unchanged;
	if (m_workbenchRuntime != nullptr) {
		before = m_workbenchRuntime->WorkspaceContext().Snapshot();
		targetIdentity = PreviewProjectWorkspaceIdentity(config::EWorkspaceKind::Folder, *folderUri.value);
		if (!targetIdentity) return EOpenWorkspaceFolderResult::WorkspaceContextFailed;
		oldTerminalDirectory = m_workspaceContext->GetNewTerminalWorkingDirectory();
		targetProjectionExisted = m_terminalTool != nullptr
			&& m_terminalTool->HasWorkspaceProjection(*targetIdentity);
		if (m_terminalTool != nullptr && before.workspaceIdentityKey != *targetIdentity) {
			const auto prepared = m_terminalTool->SwitchWorkspace({
				before.workspaceIdentityKey, *targetIdentity, absoluteRoot, false });
			if (!prepared.Succeeded()) {
				if (!targetProjectionExisted) m_terminalTool->DiscardWorkspaceProjection(*targetIdentity);
				return EOpenWorkspaceFolderResult::WorkspaceContextFailed;
			}
			terminalPrepared = true;
			terminalOutcome = prepared.outcome;
		}
	}
	const auto rollbackTerminal = [&] {
		if (!terminalPrepared || m_terminalTool == nullptr || !targetIdentity) return;
		const auto restored = m_terminalTool->SwitchWorkspace({
			*targetIdentity, before.workspaceIdentityKey, oldTerminalDirectory, false });
		if (!restored.Succeeded()) {
			::OutputDebugStringW(L"Sakura Editor NEXT: Folder terminal rollback failed.\n");
		}
		if (!targetProjectionExisted) m_terminalTool->DiscardWorkspaceProjection(*targetIdentity);
	};

	if (m_workbenchRuntime != nullptr) {
		auto displayName = std::filesystem::path(absoluteRoot).filename().wstring();
		if (displayName.empty()) displayName = std::filesystem::path(absoluteRoot).root_name().wstring();
		if (displayName.empty() || displayName.size() > 256) {
			displayName = LocalizedWorkbenchString(STR_WORKBENCH_EXPLORER_FOLDER);
		}
		const auto accepted = m_workbenchRuntime->SwitchToFolderWorkspace(
			std::move(*folderUri.value), std::move(displayName));
		if (accepted.outcome != config::EWorkspaceContextOutcome::Succeeded
			&& accepted.outcome != config::EWorkspaceContextOutcome::NotApplicable) {
			rollbackTerminal();
			return EOpenWorkspaceFolderResult::WorkspaceContextFailed;
		}
	} else {
		// Unit-only/legacy construction keeps the pre-runtime projection.
		m_workspaceContext->SetExplicitRoot(absoluteRoot);
	}

	ApplySemanticWorkspaceContext();
	const auto acceptedRoot = GetSemanticWorkspaceRoot();
	if (acceptedRoot.empty()) return EOpenWorkspaceFolderResult::WorkspaceContextFailed;
	if (m_terminalTool != nullptr && terminalPrepared) {
		if (terminalOutcome == terminal::TerminalWorkspaceSwitchOutcome::Detached) {
			m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
		}
		if (terminalWasVisible && !m_terminalTool->EnsureSessionStarted()) {
			::OutputDebugStringW(L"Sakura Editor NEXT: Folder terminal activation failed after workspace commit.\n");
		}
	} else if (m_terminalTool != nullptr && m_workbenchRuntime == nullptr
		&& ::CompareStringOrdinal(previousRoot.c_str(), -1, acceptedRoot.c_str(), -1, TRUE) != CSTR_EQUAL) {
		m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
		if (terminalWasVisible) static_cast<void>(m_terminalTool->EnsureSessionStarted());
	}
	if (revealExplorer) RevealExplorerAfterWorkspaceCommit();
	if (m_workbenchRuntime != nullptr) {
		const auto accepted = m_workbenchRuntime->WorkspaceContext().Snapshot();
		if (accepted.kind == config::EWorkspaceKind::Folder && accepted.folders.size() == 1) {
			RecordRecentlyOpenedWorkspaceAfterReady(
				workbench::recent::ERecentlyOpenedWorkspaceKind::Folder,
				accepted.folders.front().uri);
		}
		(void)RefreshWorkbenchCommandContext();
	}
	return EOpenWorkspaceFolderResult::Succeeded;
}

EWorkspaceWindowTransitionResult CEditWnd::OpenRecentlyOpenedWorkspace(
	const workbench::recent::RecentlyOpenedWorkspaceEntry& entry)
{
	if (m_workbenchRuntime == nullptr) return EWorkspaceWindowTransitionResult::Failed;
	const auto recent = m_workbenchRuntime->RecentlyOpenedWorkspaces();
	const auto normalized = workbench::recent::CRecentlyOpenedWorkspaceService::Normalize(entry);
	if (recent == nullptr || !normalized) return EWorkspaceWindowTransitionResult::Failed;
	const auto localPath = normalized->uri.ToWindowsPath();
	if (!localPath.value) return EWorkspaceWindowTransitionResult::Failed;
	const DWORD attributes = ::GetFileAttributesW(localPath.value->c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		const DWORD error = ::GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
			(void)recent->RemoveConfirmedNotFound(normalized->uri);
			(void)RefreshWorkbenchCommandContext();
		}
		return EWorkspaceWindowTransitionResult::Failed;
	}
	if ((normalized->kind == workbench::recent::ERecentlyOpenedWorkspaceKind::Folder
		&& (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
		|| (normalized->kind == workbench::recent::ERecentlyOpenedWorkspaceKind::Workspace
			&& (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
		return EWorkspaceWindowTransitionResult::Failed;
	}
	if (normalized->kind == workbench::recent::ERecentlyOpenedWorkspaceKind::Folder) {
		return ApplyFolderWorkspace(*localPath.value, true) == EOpenWorkspaceFolderResult::Succeeded
			? EWorkspaceWindowTransitionResult::Succeeded
			: EWorkspaceWindowTransitionResult::Failed;
	}
	return LaunchWorkspaceTarget(L"-WORKSPACE=\"" + *localPath.value + L"\"", true);
}

bool CEditWnd::TryExecuteRecentlyOpenedWorkspaceMenuCommand(std::int32_t commandId)
{
	if (commandId < workbench::recent::kRecentlyOpenedWorkspaceDynamicFirst
		|| commandId > workbench::recent::kRecentlyOpenedWorkspaceDynamicLast) return false;
	const auto index = workbench::recent::CRecentlyOpenedWorkspaceMenuProjection::Resolve(
		commandId, m_recentlyOpenedWorkspaceMenuSnapshot);
	if (index) (void)OpenRecentlyOpenedWorkspace(m_recentlyOpenedWorkspaceMenuSnapshot[*index]);
	// This range belongs exclusively to the typed snapshot.  A stale click is
	// consumed rather than being reinterpreted as a legacy function code.
	return true;
}

EWorkspaceWindowTransitionResult CEditWnd::ShowRecentlyOpenedWorkspaceMenu()
{
	HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr) return EWorkspaceWindowTransitionResult::Failed;
	struct MenuHandle final { HMENU value; ~MenuHandle() { if (value != nullptr) ::DestroyMenu(value); } } menuHandle { menu };
	const CMRUFile legacyFiles;
	const bool hasRecentFiles = legacyFiles.MenuLength() > 0;
	const bool hasTypedEntries = AppendRecentlyOpenedWorkspaceMenu(menu, hasRecentFiles);
	if (!hasTypedEntries && !hasRecentFiles) return EWorkspaceWindowTransitionResult::Cancelled;
	if (hasRecentFiles) legacyFiles.CreateMenu(menu, &m_cMenuDrawer);
	POINT point{};
	if (::GetCursorPos(&point) == FALSE) return EWorkspaceWindowTransitionResult::Failed;
	const UINT selected = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
		point.x, point.y, 0, GetHwnd(), nullptr);
	if (selected == 0) return EWorkspaceWindowTransitionResult::Cancelled;
	const auto index = workbench::recent::CRecentlyOpenedWorkspaceMenuProjection::Resolve(
		static_cast<std::int32_t>(selected), m_recentlyOpenedWorkspaceMenuSnapshot);
	if (index) return OpenRecentlyOpenedWorkspace(m_recentlyOpenedWorkspaceMenuSnapshot[*index]);
	if (selected >= IDM_SELMRU && selected < IDM_SELMRU + MAX_MRU) {
		OnCommand(0, static_cast<WORD>(selected), nullptr);
		return EWorkspaceWindowTransitionResult::Succeeded;
	}
	return EWorkspaceWindowTransitionResult::Failed;
}

EOpenWorkspaceFolderResult CEditWnd::OpenWorkspaceFolder()
{
	if (!m_workspaceContext) return EOpenWorkspaceFolderResult::WorkspaceContextFailed;

	// SelectDir uses the native IFileDialog with FOS_PICKFOLDERS and
	// FOS_FORCEFILESYSTEM. Keep all state intact when the user cancels or the
	// dialog cannot return a filesystem path.
	std::array<WCHAR, 32768> selectedDirectory{};
	auto initialDirectory = GetSemanticWorkspaceRoot();
	if (initialDirectory.empty()) initialDirectory = m_workspaceContext->GetNewTerminalWorkingDirectory();
	switch (SelectDirWithResult(GetHwnd(), LS(F_OPEN_WORKSPACE_FOLDER), initialDirectory, selectedDirectory)) {
	case ESelectDirResult::Succeeded:
		break;
	case ESelectDirResult::Cancelled:
		return EOpenWorkspaceFolderResult::Cancelled;
	case ESelectDirResult::Failed:
		return EOpenWorkspaceFolderResult::PickerFailed;
	}

	const auto absoluteRoot = MakeAbsolutePath(selectedDirectory.data());
	const DWORD attributes = ::GetFileAttributesW(absoluteRoot.c_str());
	if (absoluteRoot.empty() || attributes == INVALID_FILE_ATTRIBUTES
		|| (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		return EOpenWorkspaceFolderResult::InvalidSelection;
	}

	return ApplyFolderWorkspace(absoluteRoot, true);
}

void CEditWnd::FocusIntegratedTerminal()
{
	SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Bottom, true, true);
}

void CEditWnd::NewIntegratedTerminal()
{
	// Activating an empty tool creates its first terminal.  Do not immediately
	// append another one when this command also has to reveal the hidden panel.
	const bool hasTerminal = m_terminalTool != nullptr && m_terminalTool->TabCount() != 0;
	SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Bottom, true, true);
	if (hasTerminal && m_terminalTool) (void)m_terminalTool->AddTerminal();
}

void CEditWnd::RedetectPowerShell()
{
	if (m_terminalTool) m_terminalTool->RedetectPowerShell();
}

bool CEditWnd::IsMarkdownPreviewVisible() const noexcept
{
	return m_markdownPreviewVisible;
}

bool CEditWnd::IsMarkdownPreviewAvailable() const
{
	if (!HasActiveEditorInput()) return false;
	const auto filePath = m_pcEditDoc->m_cDocFile.GetFilePath();
	return CheckEXT(filePath, L"md") || CheckEXT(filePath, L"markdown")
		|| CheckEXT(filePath, L"mdown") || CheckEXT(filePath, L"mkd");
}

/*!
	@brief The opened root folder's display name, empty when no folder is open

	This is VS Code's `${rootName}` caption variable. VS Code shows the folder's
	own name there, never its path, and shows nothing when the window has no
	folder open, so a trailing separator is the caller's problem rather than this
	function's.
*/
std::wstring CEditWnd::GetWorkspaceRootName() const
{
	// The workspace model owns the opened folder; the Explorer View merely shows
	// it. Reading the model keeps the caption correct even when that View has
	// never been created.
	const std::wstring root = GetSemanticWorkspaceRoot();
	if (root.empty()) return {};
	std::wstring_view name{ root };
	while (!name.empty() && (name.back() == L'\\' || name.back() == L'/')) {
		name.remove_suffix(1);
	}
	const auto separator = name.find_last_of(L"\\/");
	if (separator != std::wstring_view::npos) {
		name.remove_prefix(separator + 1);
	}
	// A drive root such as "C:" has no leaf, so its own text is the best name.
	return name.empty() ? root : std::wstring{ name };
}

bool CEditWnd::EnsureMarkdownPreview()
{
	if (m_markdownPreview && m_markdownPreview->IsCreated()) {
		return true;
	}
	std::shared_ptr<markdown::IMarkdownRemoteImageFetcher> remoteImageFetcher;
	if (m_workbenchRuntime != nullptr) {
		try {
			config::CConfigurationNetworkPolicy networkPolicy(
				m_workbenchRuntime->Configuration(),
				m_workbenchRuntime->Bootstrap().UserDataProfile().SelectedProfileId());
			const auto snapshot = networkPolicy.Snapshot();
			if (snapshot && snapshot.snapshot) {
				remoteImageFetcher = markdown::CreateMarkdownRemoteImageFetcher(*snapshot.snapshot);
			}
		}
		catch (...) {
			// A missing/invalid network policy disables only remote images. The
			// native preview and its local-resource sandbox remain available.
		}
	}
	m_markdownPreview = std::make_unique<markdown::CMarkdownPreviewWnd>(
		std::move(remoteImageFetcher));
	if (!m_markdownPreview->Create(GetHwnd())) {
		m_markdownPreview.reset();
		return false;
	}
	const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark : theme::ThemeMode::Light;
	m_markdownPreview->SetPalette(theme::CThemeService::EffectivePalette(mode));
	const auto dpi = GetHwnd() == nullptr ? 96U : ::GetDpiForWindow(GetHwnd());
	m_markdownPreview->SetEditorFont(GetLogfont(), dpi);
	m_markdownPreview->SetSourceLineCallback([this](std::size_t sourceLine) {
		if (!HasActiveEditorInput()) return;
		if (m_markdownPreviewCommandState.SourceIdentity()
			!= GetDocument()->m_cDocFile.GetFilePath()) return;
		const auto boundedLine = static_cast<int>((std::min)(sourceLine,
			static_cast<std::size_t>(std::numeric_limits<int>::max())));
		CLayoutPoint layoutPosition;
		GetDocument()->m_cLayoutMgr.LogicToLayout(
			CLogicPoint(CLogicInt(0), CLogicInt(boundedLine)), &layoutPosition);
		auto& activeView = GetActiveView();
		m_markdownPreviewScrollSyncing = true;
		const auto delta = activeView.ScrollAtV(layoutPosition.y);
		activeView.SyncScrollV(delta);
		m_markdownPreviewScrollSyncing = false;
	});
	return true;
}

void CEditWnd::SyncMarkdownPreviewToEditorScroll(const CEditView& view)
{
	if (!m_markdownPreviewVisible || !m_markdownPreview) return;
	// The minimap mirrors the real view; letting it drive would double-apply the scroll.
	if (view.m_bMiniMap) return;
	if (&view != &GetActiveView()) return;
	// The preview is already driving the editor through its own callback.
	if (m_markdownPreviewScrollSyncing) return;
	if (!HasActiveEditorInput()) return;
	if (m_markdownPreviewCommandState.SourceIdentity()
		!= GetDocument()->m_cDocFile.GetFilePath()) return;
	CLogicPoint logicPosition;
	GetDocument()->m_cLayoutMgr.LayoutToLogic(
		CLayoutPoint(CLayoutInt(0), view.GetTextArea().GetViewTopLine()), &logicPosition);
	const auto sourceLine = static_cast<Int>(logicPosition.y);
	if (sourceLine < 0) return;
	// AdjustScrollBars runs for layout changes too; only a real change moves the preview.
	if (m_markdownPreviewSyncedSourceLine == sourceLine) return;
	m_markdownPreviewSyncedSourceLine = sourceLine;
	m_markdownPreviewScrollSyncing = true;
	m_markdownPreview->RevealSourceLine(static_cast<std::size_t>(sourceLine));
	m_markdownPreviewScrollSyncing = false;
}

void CEditWnd::CloseMarkdownPreview() noexcept
{
	m_markdownPreviewCommandState.Reset();
	m_markdownPreviewVisible = false;
	m_markdownPreviewDirty = false;
	m_markdownPreviewRevision = -1;
	m_markdownPreviewGeneration = 0;
	if (m_markdownPreview) m_markdownPreview->Close();
	m_markdownPreview.reset();
}

std::wstring CEditWnd::GetMarkdownPreviewSource(bool* truncated)
{
	// Keep the background refresh bounded even for unusually large generated documents.
	constexpr std::size_t maximumCharacters = 2U * 1024U * 1024U;
	constexpr int maximumLines = 200000;
	bool wasTruncated = false;
	std::wstring source;
	int inspectedLines = 0;
	const auto& lineManager = GetDocument()->m_cDocLineMgr;
	for (CLogicInt line(0); line < lineManager.GetLineCount() && inspectedLines < maximumLines;
		++line, ++inspectedLines) {
		CLogicInt length(0);
		const auto* documentLine = lineManager.GetLine(line);
		const auto* text = CDocLine::GetDocLineStrWithEOL_Safe(documentLine, &length);
		if (text == nullptr || length <= 0) {
			continue;
		}
		if (source.size() >= maximumCharacters) {
			wasTruncated = true;
			break;
		}
		const auto available = maximumCharacters - source.size();
		const auto copied = std::min<std::size_t>(available, static_cast<std::size_t>(length));
		source.append(text, copied);
		if (copied < static_cast<std::size_t>(length)) {
			wasTruncated = true;
			break;
		}
	}
	if (inspectedLines >= maximumLines && CLogicInt(inspectedLines) < lineManager.GetLineCount()) {
		wasTruncated = true;
	}
	if (truncated != nullptr) {
		*truncated = wasTruncated;
	}
	return source;
}

void CEditWnd::RefreshMarkdownPreview()
{
	if (!m_markdownPreviewVisible || !m_markdownPreview) {
		return;
	}
	const auto activeSourceIdentity = GetDocument()->m_cDocFile.GetFilePath();
	if (m_markdownPreviewCommandState.IsLocked()
		&& m_markdownPreviewCommandState.SourceIdentity() != activeSourceIdentity) {
		return;
	}
	bool truncated = false;
	markdown::ParseOptions parseOptions;
	parseOptions.documentPath = GetDocument()->m_cDocFile.GetFilePath();
	parseOptions.workspaceRoot = GetSemanticWorkspaceRoot();
	const auto revision = GetDocument()->m_cDocEditor.m_cOpeBuf.GetCurrentPointer();
	auto source = GetMarkdownPreviewSource(&truncated);
	if (m_markdownPreviewGeneration == std::numeric_limits<std::uint64_t>::max()) {
		// Stop and join the old worker before restarting the generation space, so
		// no pre-wrap completion can compare equal to a new request.
		m_markdownPreview->Close();
		m_markdownPreview.reset();
		m_markdownPreviewGeneration = 0;
		if (!EnsureMarkdownPreview()) {
			m_markdownPreviewCommandState.Reset();
			m_markdownPreviewVisible = false;
			return;
		}
	}
	const markdown::PreviewRenderKey key{ ++m_markdownPreviewGeneration, revision };
	if (m_markdownPreview->QueueDocument(
		std::move(source), std::move(parseOptions), truncated, key)) {
		m_markdownPreviewRevision = revision;
		m_markdownPreviewDirty = false;
		const int caretLine = static_cast<int>(Int(GetActiveView().GetCaret().GetCaretLogicPos().y));
		m_markdownPreview->RevealSourceLine(
			static_cast<std::size_t>((std::max)(0, caretLine)));
	}
}

void CEditWnd::UpdateMarkdownPreviewIfNeeded()
{
	if (!m_markdownPreviewVisible || !m_markdownPreview) {
		return;
	}
	const auto activeSourceIdentity = GetDocument()->m_cDocFile.GetFilePath();
	if (m_markdownPreviewCommandState.IsLocked()
		&& m_markdownPreviewCommandState.SourceIdentity() != activeSourceIdentity) {
		// The locked projection owns its previous immutable render. Never refresh
		// or scroll an unrelated active editor through that retained identity.
		m_markdownPreviewDirty = false;
		return;
	}
	if (!IsMarkdownPreviewAvailable()) {
		m_markdownPreviewCommandState.Reset();
		m_markdownPreviewVisible = false;
		m_markdownPreview->Show(false);
		return;
	}
	const auto revision = GetDocument()->m_cDocEditor.m_cOpeBuf.GetCurrentPointer();
	if (revision != m_markdownPreviewRevision) {
		m_markdownPreviewRevision = revision;
		m_markdownPreviewDirty = true;
		return;
	}
	if (m_markdownPreviewDirty) {
		RefreshMarkdownPreview();
	}
}

RECT CEditWnd::LayoutMarkdownPreview(int left, int top, int right, int bottom, unsigned int dpi,
	int minimapWidth, bool minimapOnLeft)
{
	m_markdownPreviewMinimapWidth = std::max(0, minimapWidth);
	m_markdownPreviewMinimapOnLeft = minimapOnLeft;
	const RECT previousDivider = m_markdownPreviewDivider;
	// Where the minimap ends up unless a sibling preview claims the right side.
	RECT minimapBounds = minimapOnLeft
		? RECT{ left, top, left + minimapWidth, bottom }
		: RECT{ right - minimapWidth, top, right, bottom };
	if (!HasActiveEditorInput()) {
		m_markdownPreviewDivider = {};
		if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), &previousDivider, FALSE);
		if (const HWND splitter = m_cSplitterWnd.GetHwnd(); splitter != nullptr) {
			::ShowWindow(splitter, SW_HIDE);
		}
		if (m_markdownPreview) m_markdownPreview->Show(false);
		// Same precedence as `ApplyEditorCoreSnapshot`: comparison, extension
		// metadata, then the watermark. Only the winner is laid out, so
		// a hidden projection never claims the editor rectangle.
		if (m_diffSurface && m_diffSurface->HasDiff()) {
			m_diffSurface->Layout({ left, top, right, bottom }, dpi);
			if (!m_pPrintPreview) m_diffSurface->Show();
			if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
		} else if (m_emptyEditorSurface) {
			m_emptyEditorSurface->Layout({ left, top, right, bottom }, dpi);
			if (!m_pPrintPreview) m_emptyEditorSurface->Show();
			if (m_diffSurface) m_diffSurface->Hide();
		}
		return minimapBounds;
	}
	if (m_diffSurface) m_diffSurface->Hide();
	if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
	const bool showPreview = m_markdownPreviewVisible && m_markdownPreview != nullptr && !m_pPrintPreview;
	auto paneMode = markdown::PreviewPaneMode::Hidden;
	if (showPreview) {
		switch (m_markdownPreviewCommandState.Placement()) {
		case markdown::MarkdownPreviewPlacement::CurrentEditorGroup:
			paneMode = markdown::PreviewPaneMode::Replacement;
			break;
		case markdown::MarkdownPreviewPlacement::NativeSiblingPane:
			paneMode = markdown::PreviewPaneMode::NativeSibling;
			break;
		case markdown::MarkdownPreviewPlacement::SideEditorGroup:
			// A second EditorGroup is not yet a native capability. Exact to-side
			// commands fail closed before this projection boundary.
			paneMode = markdown::PreviewPaneMode::Hidden;
			break;
		}
	}
	const int previewWidthDip = m_resizingMarkdownPreview
		? m_markdownPreviewResizeWidthDip : m_markdownPreviewWidthDip;
	const auto layout = markdown::CalculateMarkdownPreviewLayout(left, right, dpi, paneMode,
		previewWidthDip);
	m_markdownPreviewRegion = { left, top, right, bottom };
	m_markdownPreviewDivider = { layout.dividerLeft, top, layout.dividerRight, bottom };
	// The minimap travels with the editor half, so the view keeps the region left
	// of it. The split was calculated over the region including the minimap's
	// column, which is why no width is lost when the preview is hidden.
	minimapBounds = minimapOnLeft
		? RECT{ layout.editorLeft, top, layout.editorLeft + minimapWidth, bottom }
		: RECT{ layout.editorRight - minimapWidth, top, layout.editorRight, bottom };
	const int editorViewLeft = minimapOnLeft
		? std::min(layout.editorRight, layout.editorLeft + minimapWidth)
		: layout.editorLeft;
	const int editorViewRight = minimapOnLeft
		? layout.editorRight
		: std::max(layout.editorLeft, layout.editorRight - minimapWidth);
	if (paneMode != markdown::PreviewPaneMode::NativeSibling || layout.PreviewWidth() == 0) {
		m_markdownPreviewDivider = {};
	}
	if (GetHwnd() != nullptr) {
		::InvalidateRect(GetHwnd(), &previousDivider, FALSE);
		::InvalidateRect(GetHwnd(), &m_markdownPreviewDivider, FALSE);
	}
	if (const HWND splitter = m_cSplitterWnd.GetHwnd(); splitter != nullptr) {
		(void)PositionChildForFrame(splitter, editorViewLeft, top,
			std::max(0, editorViewRight - editorViewLeft), std::max(0, bottom - top));
		::ShowWindow(splitter, paneMode == markdown::PreviewPaneMode::Replacement || m_pPrintPreview
			? SW_HIDE : SW_SHOWNA);
	}
	if (!m_markdownPreview) {
		return minimapBounds;
	}
	if (!showPreview || layout.PreviewWidth() == 0) {
		// A hidden preview keeps its last committed render generation. Laying the
		// full document out at zero width immediately before hiding is both useless
		// and pathological for large Markdown documents.
		m_markdownPreview->Show(false);
		return minimapBounds;
	}
	const RECT previewBounds{ layout.previewLeft, top, layout.previewRight, bottom };
	m_markdownPreview->SetEditorFont(GetLogfont(), dpi);
	m_markdownPreview->Layout(previewBounds, dpi, m_resizingMarkdownPreview);
	m_markdownPreview->Show(true);
	return minimapBounds;
}

/*!
	@brief True when the point is on the preview divider

	The painted divider is one device-independent pixel, which is far too thin to
	grab. VS Code widens its sash's hit area beyond the drawn line for the same
	reason, so the same slop the workbench splitters use is applied here.
*/
bool CEditWnd::HitTestMarkdownPreviewDivider(POINT point) const noexcept
{
	RECT rect = m_markdownPreviewDivider;
	if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
	const int hitSize = std::max(1, ::MulDiv(4, static_cast<int>(::GetDpiForWindow(GetHwnd())), 96));
	const int extra = std::max(0, hitSize - static_cast<int>(rect.right - rect.left));
	rect.left -= extra / 2;
	rect.right += extra - extra / 2;
	return ContainsPoint(rect, point);
}

void CEditWnd::RelayoutForMarkdownPreviewDivider()
{
	const RECT region = m_markdownPreviewRegion;
	const bool haveRegion = region.right > region.left && region.bottom > region.top;
	if (!haveRegion) return;

	// A preview sash never changes the outer workbench geometry.  Keep this
	// cohort local for both transient samples and the terminal commit/cancel:
	// LayoutMarkdownPreview moves only the editor splitter and preview, while
	// its non-transient preview Layout() queues width-dependent reflow through
	// CMarkdownPreviewWnd's continuation.  Calling OnSize2 here used to walk
	// every Part and synchronously replay all accessory layout on mouse-up.
	const RECT minimapBounds = LayoutMarkdownPreview(region.left, region.top,
		region.right, region.bottom, ::GetDpiForWindow(GetHwnd()),
		m_markdownPreviewMinimapWidth, m_markdownPreviewMinimapOnLeft);
	if (const HWND minimap = m_cMiniMapView.GetHwnd(); minimap != nullptr) {
		(void)PositionChildForFrame(minimap, minimapBounds.left, minimapBounds.top,
			std::max(0L, minimapBounds.right - minimapBounds.left),
			std::max(0L, minimapBounds.bottom - minimapBounds.top));
	}
	if (!m_resizingMarkdownPreview) {
		// The committed/cancelled width has already been selected above.  The
		// preview owns its asynchronous reflow; publish only this cohort's
		// invalidation now and leave unrelated Parts untouched.
		::RedrawWindow(GetHwnd(), &region, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		return;
	}
	// The editor view, the preview, and the divider are separate windows that
	// share one invalidation cohort. The message queue coalesces repeated pointer
	// samples and each child publishes from its persistent buffer.
	//
	// The commit is bounded to the split region and does not ask for an erase.
	// A whole-window RDW_ERASE would repaint the activity bar, side bars, tabs
	// and status bar on every mouse sample even though the drag cannot move
	// them, and those Parts flashing against their own erased background is what
	// a divider drag looked like before this rectangle was passed.
	// MoveWindow and the preview's transient Layout have already invalidated
	// exactly the vacated strips, new viewport, and old/new divider. Flush those
	// pending regions as one frame without invalidating the whole central area
	// again; repainting every pixel here made drag cost proportional to pane area.
	::RedrawWindow(GetHwnd(), haveRegion ? &region : nullptr, nullptr,
		RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
}

void CEditWnd::CommitMarkdownPreviewResize()
{
	if (!m_resizingMarkdownPreview) return;
	m_markdownPreviewWidthDip = m_markdownPreviewResizeWidthDip;
	m_resizingMarkdownPreview = false;
	if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
	RelayoutForMarkdownPreviewDivider();
}

void CEditWnd::CancelMarkdownPreviewResize()
{
	if (!m_resizingMarkdownPreview) return;
	m_resizingMarkdownPreview = false;
	if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
	RelayoutForMarkdownPreviewDivider();
}

void CEditWnd::AbortMarkdownPreviewResize() noexcept
{
	if (!m_resizingMarkdownPreview) return;
	m_resizingMarkdownPreview = false;
	if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
}

void CEditWnd::ToggleMarkdownPreview()
{
	const auto sourceIdentity = GetDocument()->m_cDocFile.GetFilePath();
	const auto result = m_markdownPreviewCommandState.ToggleNativeSibling(
		sourceIdentity, IsMarkdownPreviewAvailable());
	(void)ApplyMarkdownPreviewCommandResult(result);
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ExecuteMarkdownPreviewCommand(
	markdown::MarkdownPreviewCommand command)
{
	const auto sourceIdentity = GetDocument()->m_cDocFile.GetFilePath();
	return ApplyMarkdownPreviewCommandResult(m_markdownPreviewCommandState.Apply(
		command, sourceIdentity, IsMarkdownPreviewAvailable()));
}

workbench::commands::WorkbenchCommandExecutionResult CEditWnd::ApplyMarkdownPreviewCommandResult(
	const markdown::MarkdownPreviewCommandResult& result)
{
	using Status = workbench::commands::EWorkbenchCommandExecutionStatus;
	switch (result.outcome) {
	case markdown::MarkdownPreviewCommandOutcome::NotApplicable:
		return { Status::NotApplicable, "Markdown preview command is not applicable" };
	case markdown::MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup:
		return { Status::Unsupported, "Markdown preview to side requires a second EditorGroup" };
	case markdown::MarkdownPreviewCommandOutcome::UnsupportedSecuritySelector:
		return { Status::Unsupported, "native Markdown preview has no security selector" };
	case markdown::MarkdownPreviewCommandOutcome::UnavailableLockedSource:
		return { Status::Unsupported, "locked Markdown preview source is no longer active" };
	case markdown::MarkdownPreviewCommandOutcome::RefreshRequested:
		RefreshMarkdownPreview();
		return { Status::Succeeded, {} };
	case markdown::MarkdownPreviewCommandOutcome::Applied:
		break;
	}

	const bool wasPreviewVisible = m_markdownPreviewVisible;
	m_markdownPreviewVisible = m_markdownPreviewCommandState.IsVisible();
	if (m_markdownPreviewVisible) {
		const bool hadReusablePreview = m_markdownPreview && m_markdownPreview->IsCreated();
		if (!EnsureMarkdownPreview()) {
			m_markdownPreviewCommandState.Reset();
			m_markdownPreviewVisible = false;
			return { Status::Failed, "native Markdown preview window could not be created" };
		}
		const auto revision = GetDocument()->m_cDocEditor.m_cOpeBuf.GetCurrentPointer();
		if (!hadReusablePreview || result.identityChanged || m_markdownPreviewDirty
			|| m_markdownPreviewRevision != revision) {
			m_markdownPreviewDirty = true;
			RefreshMarkdownPreview();
		}
	} else if (m_markdownPreview) {
		m_markdownPreview->Show(false);
	}
	m_cTabWnd.RefreshDocumentActionState();
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
		if (wasPreviewVisible && !m_markdownPreviewVisible) {
			// Hiding the preview vacates a child-owned rectangle. The normal layout
			// pass queues no-erase invalidation, but a close is a low-frequency
			// transaction whose visible result must be coherent before the next
			// message-loop turn: flush the parent and its new editor sibling as one
			// repaint cohort without reintroducing an erase frame.
			::RedrawWindow(GetHwnd(), nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		}
	}
	return { Status::Succeeded, {} };
}

bool CEditWnd::PreTranslateWorkbenchMessage(MSG& message)
{
	if (message.message == WM_TIMER && message.wParam == IDT_WORKBENCH_KEYBINDING_CHORD) {
		// A queued timer from an earlier restarted chord must not clear the newer
		// state before its own five-second deadline has elapsed.
		ExpireWorkbenchKeybindingChord();
		return true;
	}
	ExpireWorkbenchKeybindingChord();
	const auto cancelsWorkbenchKeybindingChord = [](UINT messageId) noexcept {
		switch (messageId) {
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_XBUTTONDOWN:
		case WM_NCLBUTTONDOWN:
		case WM_NCRBUTTONDOWN:
		case WM_NCMBUTTONDOWN:
		case WM_NCXBUTTONDOWN:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
		case WM_SYSKEYDOWN:
			return true;
		default:
			return false;
		}
	};
	if (m_workbenchKeybindingState.IsChordPending() && cancelsWorkbenchKeybindingChord(message.message)) {
		ClearWorkbenchKeybindingChord();
	}
	if (m_commandPaletteOverlay && m_commandPaletteOverlay->PreTranslateMessage(message)) return true;
	if (m_emptyEditorSurface && m_emptyEditorSurface->PreTranslateMessage(message)) return true;
	if (message.message == WM_KEYDOWN) {
		const workbench::editor::WorkbenchKeyModifiers modifiers{
			.control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
			.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0,
			.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0,
		};
		const HWND focusWindow = ::GetFocus();
		const HWND chordFocusWindow = focusWindow != nullptr ? focusWindow : message.hwnd;
		const auto input = m_workbenchKeybindingState.HandleKeyDown(
			static_cast<std::uint32_t>(message.wParam), modifiers, ::GetTickCount64(),
			reinterpret_cast<workbench::editor::WorkbenchKeybindingState::FocusToken>(chordFocusWindow),
			[this](std::string_view commandId) {
				return m_workbenchCommandRegistry != nullptr
					&& m_workbenchCommandRegistry->Find(commandId).has_value();
			});
		switch (input.decision) {
		case workbench::editor::EWorkbenchKeyInputDecision::PassThrough:
			break;
		case workbench::editor::EWorkbenchKeyInputDecision::BeginOrRestartChordAndConsume:
			// The first stroke and a repeated Ctrl+K are terminal as soon as the
			// registry recognizes Open Folder. A failed timer arm clears the pure
			// state, but must not revive a conflicting legacy accelerator.
			(void)ArmWorkbenchKeybindingChordTimer();
			return true;
		case workbench::editor::EWorkbenchKeyInputDecision::ExecuteStableCommandAndConsume: {
			ClearWorkbenchKeybindingChord();
			bool handled = false;
			(void)TryExecuteWorkbenchStableCommand(input.commandId, handled);
			// A registry-recognized binding is terminal even when its executor is
			// disabled, not applicable, unsupported, or failed.
			return true;
		}
		case workbench::editor::EWorkbenchKeyInputDecision::CancelChordAndConsume:
			ClearWorkbenchKeybindingChord();
			return true;
		}
	}
	if (message.message == WM_KEYDOWN && (::GetKeyState(VK_CONTROL) & 0x8000) != 0
		&& (::GetKeyState(VK_MENU) & 0x8000) == 0) {
		if (message.wParam == L'P' && (::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
			(void)ShowCommandPalette();
			return true;
		}
		int direction = 2;
		if (message.wParam == VK_OEM_PLUS || message.wParam == VK_ADD) direction = 1;
		else if (message.wParam == VK_OEM_MINUS || message.wParam == VK_SUBTRACT) direction = -1;
		else if (message.wParam == L'0' || message.wParam == VK_NUMPAD0) direction = 0;
		if (direction != 2) {
			SetWorkbenchZoomPercent(workbench::AdjustZoomPercent(m_workbenchZoomPercent, direction));
			return true;
		}
	}
	if (m_resizingMarkdownPreview && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
		// Escape abandons the drag at wherever the divider currently sits; there is
		// no committed-elsewhere state to roll back to, unlike a workbench resize.
		CancelMarkdownPreviewResize();
		return true;
	}
	if (m_resizingWorkbenchPanel != nullptr && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
		CancelWorkbenchResize();
		return true;
	}
	if (m_activityBar && m_activityBar->PreTranslateMessage(message)) return true;
	if (m_auxiliaryActivityBar && m_auxiliaryActivityBar->PreTranslateMessage(message)) return true;
	if (m_bottomWorkbenchPanel && m_bottomWorkbenchPanel->PreTranslateMessage(message)) return true;
	if (m_leftWorkbenchPanel && m_leftWorkbenchPanel->PreTranslateMessage(message)) return true;
	return m_rightWorkbenchPanel && m_rightWorkbenchPanel->PreTranslateMessage(message);
}

bool CEditWnd::EnsureQuickInputOverlay()
{
	if (m_commandPaletteOverlay) return true;
	if (!GetHwnd()) return false;
	m_commandPaletteOverlay = std::make_unique<workbench::quickinput::CCommandPaletteOverlay>();
	if (!m_commandPaletteOverlay->Create(GetHwnd())) {
		m_commandPaletteOverlay.reset();
		return false;
	}
	m_commandPaletteOverlay->SetPalette(theme::CThemeService::EffectivePalette(
		m_pShareData->m_Common.m_sWindow.m_bDarkMode ? theme::ThemeMode::Dark : theme::ThemeMode::Light));
	return true;
}

bool CEditWnd::ShowCommandPalette()
{
	if (!GetHwnd() || !m_workbenchCommandRegistry) return false;
	const auto titleResolver = [](const workbench::commands::WorkbenchCommandDescriptor& descriptor) {
		return ResolveLocalizedWorkbenchCommandTitle(descriptor);
	};
	const auto registeredCommands =
		workbench::editor::SearchRegisteredCommandPalette(*m_workbenchCommandRegistry, L"", titleResolver);
	if (registeredCommands.empty()) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS,
			LocalizedWorkbenchString(STR_WORKBENCH_COMMAND_PALETTE_NO_RESULTS).c_str());
		return false;
	}
	const auto convertItems = [](const auto& source) {
		std::vector<workbench::quickinput::CommandPaletteItem> items;
		items.reserve(source.size());
		for (const auto& command : source) {
			items.push_back({
				.id = std::wstring(command.id.begin(), command.id.end()),
				.label = command.label,
				.enabled = true,
			});
		}
		return items;
	};
	if (!EnsureQuickInputOverlay()) return false;
	m_commandPaletteOverlay->SetStringsCallback([] {
		return workbench::quickinput::QuickInputStrings {
			.placeholder = LocalizedWorkbenchString(STR_WORKBENCH_COMMAND_PALETTE_SEARCH_PLACEHOLDER),
			.noResults = LocalizedWorkbenchString(STR_WORKBENCH_COMMAND_PALETTE_NO_RESULTS),
		};
	});
	m_commandPaletteOverlay->SetSearchCallback([this, convertItems, titleResolver](std::wstring_view query) {
		if (!m_workbenchCommandRegistry) {
			return std::vector<workbench::quickinput::CommandPaletteItem>{};
		}
		return convertItems(workbench::editor::SearchRegisteredCommandPalette(
			*m_workbenchCommandRegistry, query, titleResolver));
	});
	m_commandPaletteOverlay->SetSelectionCallback({});
	m_commandPaletteOverlay->SetAcceptCallback([this](std::wstring commandId) {
		if (!m_workbenchCommandRegistry) return;
		(void)workbench::editor::DispatchRegisteredCommandPaletteSelection(
			*m_workbenchCommandRegistry, commandId, [this](std::string_view stableCommandId) {
				bool handled = false;
				(void)TryExecuteWorkbenchStableCommand(stableCommandId, handled);
			});
	});
	m_commandPaletteOverlay->SetCancelCallback({});
	return m_commandPaletteOverlay->Show(convertItems(registeredCommands));
}

void CEditWnd::SetWorkbenchZoomPercent(int percent)
{
	percent = std::clamp(percent, workbench::kMinimumZoomPercent, workbench::kMaximumZoomPercent);
	if (percent == m_workbenchZoomPercent) return;
	if (m_workbenchZoomBasePointSize <= 0) {
		m_workbenchZoomBasePointSize = GetFontPointSize(false);
	}
	m_workbenchZoomPercent = percent;
	if (m_customFrame) m_customFrame->SetUiScalePercent(percent);
	if (m_workbenchZoomBasePointSize > 0 && m_dispatchReady && HasActiveEditorInput()) {
		const int pointSize = std::max(10, ::MulDiv(m_workbenchZoomBasePointSize, percent, 100));
		GetActiveView().GetCommander().Command_SETFONTSIZE(pointSize, 0, 2);
	}
	if (GetHwnd()) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), true);
		::RedrawWindow(GetHwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
	}
}

//! ドキュメントリスナ：ロード前。NotifyCheckLoad が全て通った後、native mutation より前に呼ばれる。
void CEditWnd::OnBeforeLoad([[maybe_unused]] SLoadInfo* sLoadInfo)
{
	if (m_pendingLoadPrearmed) {
		m_pendingLoadPrearmed = false;
		return;
	}
	m_pendingLoadCompletionToken.reset();
	m_pendingLoadHadActiveInput = false;
	m_pendingLoadReachedAfter = false;
	if (m_editorServiceAdapter == nullptr) return;
	const auto snapshot = m_editorServiceAdapter->Snapshot();
	m_pendingLoadHadActiveInput = snapshot.group.activeInputId.has_value();
	if (!m_pendingLoadHadActiveInput || m_workingCopyLifecycleBridge == nullptr) return;
	(void)m_workingCopyLifecycleBridge->Flush(::GetTickCount64(), true);
	if (auto token = m_workingCopyLifecycleBridge->CaptureCurrentCompletionToken()) {
		m_pendingLoadCompletionToken =
			std::make_unique<workbench::editor::persistence::EditorWorkingCopyCompletionToken>(
				std::move(*token));
	}
}

void CEditWnd::PrepareLegacyLoadReplacement()
{
	OnBeforeLoad(nullptr);
	m_pendingLoadPrearmed = true;
}

//! ドキュメントリスナ：ロード後。Commit は OnFinalLoad の成功 terminal まで延期する。
void CEditWnd::OnAfterLoad([[maybe_unused]] const SLoadInfo& sLoadInfo)
{
	m_pendingLoadReachedAfter = true;
}

bool CEditWnd::FinalizeSuccessfulLegacyLoad()
{
	if (m_editorServiceAdapter != nullptr && !AdoptLoadedLegacyFile()) return false;
	if (!HasActiveEditorInput()) return false;
	UpdateWorkspaceFromDocument();
	if (m_startupDrawState != StartupDrawState::Committed
		|| !m_startupFirstContentPainted
		|| !m_cDlgFuncList.m_bEditWndReady
		|| m_startupWorkbenchCompletionPosted) {
		m_startupOutlineReloadPending =
			m_pShareData->m_Common.m_sWorkbench.m_bRightPanelVisible != FALSE;
	}
	if (m_markdownPreviewVisible) {
		const auto sourceIdentity = GetDocument()->m_cDocFile.GetFilePath();
		const bool sourceIsMarkdown = IsMarkdownPreviewAvailable();
		const bool wasLocked = m_markdownPreviewCommandState.IsLocked();
		const bool sameIdentity = m_markdownPreviewCommandState.SourceIdentity() == sourceIdentity;
		(void)m_markdownPreviewCommandState.ObserveActiveSource(sourceIdentity, sourceIsMarkdown);
		m_markdownPreviewVisible = m_markdownPreviewCommandState.IsVisible();
		if (m_markdownPreviewVisible && sourceIsMarkdown && (!wasLocked || sameIdentity)) {
			m_markdownPreviewDirty = true;
			m_markdownPreviewRevision = -1;
			RefreshMarkdownPreview();
		} else if (!m_markdownPreviewVisible && m_markdownPreview) {
			m_markdownPreview->Show(false);
		}
		if (GetHwnd() != nullptr) {
			RECT client{};
			::GetClientRect(GetHwnd(), &client);
			(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
		}
	}
	m_cTabWnd.RefreshDocumentActionState();
	// Complete the pre-load persistence token only after the authoritative core
	// input and every native projection step reached their success terminal.  A
	// projection failure must leave the durable backup untouched for recovery.
	if (m_pendingLoadCompletionToken && m_workingCopyLifecycleBridge) {
		(void)m_workingCopyLifecycleBridge->CompletePreClose(*m_pendingLoadCompletionToken);
	}
	return true;
}

//! ドキュメントリスナ：ロード terminal。成功以外も必ず所有者を決めて終端する。
ELoadFinalizationStatus CEditWnd::OnFinalLoad(ELoadResult eLoadResult)
{
	const auto clearPendingState = [this]() noexcept {
		m_pendingLoadCompletionToken.reset();
		m_pendingLoadHadActiveInput = false;
		m_pendingLoadReachedAfter = false;
		m_pendingLoadPrearmed = false;
	};
	const bool loaded = eLoadResult == LOADED_OK || eLoadResult == LOADED_LOSESOME;
	try {
		if (loaded && m_pendingLoadReachedAfter && m_editorServiceAdapter != nullptr) {
			if (!FinalizeSuccessfulLegacyLoad()) {
				// Native I/O has already committed, but it cannot be represented as the
				// active core input. Fail closed rather than leaving a backing CEditDoc
				// visible without its workbench owner. The pre-load token is intentionally
				// not completed, so its durable recovery backup remains available.
				(void)CloseActiveEditorInput();
				clearPendingState();
				return ELoadFinalizationStatus::Failed;
			}
		}
		else if (!loaded && m_pendingLoadHadActiveInput) {
			// CLoadAgent has already reset the failed native load to its pathless backing
			// document. Do not expose the old Core input against that new native state;
			// retain its durable backup by deliberately not completing the pre-load token.
			(void)CloseActiveEditorInput();
		}
	}
	catch (...) {
		if (loaded && m_pendingLoadHadActiveInput) {
			try {
				// An exception after native load has the same ownership rule as a
				// failed projection: clear the exact active Core input and retain backup.
				(void)CloseActiveEditorInput();
			}
			catch (...) {
				// The caller receives Failed; finalization state is still cleared below.
			}
		}
		clearPendingState();
		return ELoadFinalizationStatus::Failed;
	}
	clearPendingState();
	return ELoadFinalizationStatus::Succeeded;
}

//! ドキュメントリスナ：セーブ後
// 2008.02.02 kobake
void CEditWnd::OnAfterSave([[maybe_unused]] const SSaveInfo& sSaveInfo)
{
	if (!HasActiveEditorInput()) return;
	if (m_editorServiceAdapter != nullptr && !m_workingCopyBackendEffectInProgress) {
		if (!ActiveInputMatchesCurrentFile()) {
			if (!AdoptLoadedLegacyFile()) return;
		} else {
			(void)SynchronizeLegacyDocumentState(false, false);
		}
	}
	UpdateWorkspaceFromDocument();
	UpdateMarkdownPreviewIfNeeded();
	m_cTabWnd.RefreshDocumentActionState();
	//ビュー再描画
	this->Views_RedrawAll();

	//キャプションの更新を行う
	UpdateCaption();

	/* キャレットの行桁位置を表示する */
	GetActiveView().GetCaret().ShowCaretPosInfo();
}

void CEditWnd::UpdateCaption()
{
	// Startup keeps the client views non-drawing while the document is loaded,
	// but the top-level window is still hidden at this point.  Publish the final
	// caption now so ShowWindow never exposes the class-name bootstrap caption.
	// Outside that bounded transaction, preserve the historical draw-switch gate.
	if( !GetActiveView().GetDrawSwitch() && !IsStartupDrawSuppressed() )return;
	if (!HasActiveEditorInput()) {
		::SetWindowText(GetHwnd(), GSTR_APPNAME);
		return;
	}

	const  CommonSetting& Common = GetDllShareData().m_Common;

	const auto pszWindowCaptionFormat = IsActiveApp()
		? Common.m_sWindow.m_szWindowCaptionActive
		: Common.m_sWindow.m_szWindowCaptionInactive;

	const auto pszTabCaptionFormat = Common.m_sTabBar.m_szTabWndCaption;

	wchar_t	pszCap[1024];

	//キャプション更新
	CSakuraEnvironment::ExpandParameter( pszWindowCaptionFormat, pszCap, int(std::size(pszCap)) );
	::SetWindowText( GetHwnd(), pszCap );

	//タブウインドウのファイル名を通知
	CSakuraEnvironment::ExpandParameter( pszTabCaptionFormat, pszCap, int(std::size(pszCap)) );
	ChangeFileNameNotify( pszCap,
		GetListeningDoc()->m_cDocFile.GetFilePath(),
		CEditApp::getInstance()->GetGrepAgent()->m_bGrepMode ); // 2006.01.28 ryoji ファイル名、Grepモードパラメータを追加
}

//!< ウィンドウ生成用の矩形を取得
void CEditWnd::_GetWindowRectForInit(
	CMyRect* rcResult,
	int nGroup [[maybe_unused]],
	const STabGroupInfo& sTabGroupInfo
) const
{
	/* ウィンドウサイズ継承 */
	int	nWinCX, nWinCY;
	//	2004.05.13 Moca m_Common.m_eSaveWindowSizeをBOOLからenumに変えたため
	if( WINSIZEMODE_DEF != m_pShareData->m_Common.m_sWindow.m_eSaveWindowSize ){
		nWinCX = m_pShareData->m_Common.m_sWindow.m_nWinSizeCX;
		nWinCY = m_pShareData->m_Common.m_sWindow.m_nWinSizeCY;
	}else{
		nWinCX = CW_USEDEFAULT;
		nWinCY = 0;
	}

	/* ウィンドウサイズ指定 */
	EditInfo fi;
	CCommandLine::getInstance()->GetEditInfo(&fi);
	if( fi.m_nWindowSizeX >= 0 ){
		nWinCX = fi.m_nWindowSizeX;
	}
	if( fi.m_nWindowSizeY >= 0 ){
		nWinCY = fi.m_nWindowSizeY;
	}

	/* ウィンドウ位置指定 */
	int nWinOX, nWinOY;
	nWinOX = CW_USEDEFAULT;
	nWinOY = 0;
	// ウィンドウ位置固定
	//	2004.05.13 Moca 保存したウィンドウ位置を使う場合は共有メモリからセット
	if( WINSIZEMODE_DEF != m_pShareData->m_Common.m_sWindow.m_eSaveWindowPos ){
		nWinOX =  m_pShareData->m_Common.m_sWindow.m_nWinPosX;
		nWinOY =  m_pShareData->m_Common.m_sWindow.m_nWinPosY;
	}

	//	2004.05.13 Moca マルチディスプレイでは負の値も有効なので，
	//	未設定の判定方法を変更．(負の値→CW_USEDEFAULT)
	if( fi.m_nWindowOriginX != CW_USEDEFAULT ){
		nWinOX = fi.m_nWindowOriginX;
	}
	if( fi.m_nWindowOriginY != CW_USEDEFAULT ){
		nWinOY = fi.m_nWindowOriginY;
	}

	// 必要なら、タブグループにフィットするよう、変更
	if(sTabGroupInfo.IsValid()){
		RECT rcWork, rcMon;
		GetMonitorWorkRect( sTabGroupInfo.hwndTop, &rcWork, &rcMon );

		const WINDOWPLACEMENT& wpTop = sTabGroupInfo.wpTop;
		nWinCX = wpTop.rcNormalPosition.right  - wpTop.rcNormalPosition.left;
		nWinCY = wpTop.rcNormalPosition.bottom - wpTop.rcNormalPosition.top;
		nWinOX = wpTop.rcNormalPosition.left   + (rcWork.left - rcMon.left);
		nWinOY = wpTop.rcNormalPosition.top    + (rcWork.top - rcMon.top);
	}

	//結果
	rcResult->SetXYWH(nWinOX,nWinOY,nWinCX,nWinCY);
}

HWND CEditWnd::_CreateMainWindow(int nGroup, const STabGroupInfo& sTabGroupInfo)
{
	// -- -- -- -- ウィンドウクラス登録 -- -- -- -- //
	WNDCLASSEX	wc;
	//	Apr. 27, 2000 genta
	//	サイズ変更時のちらつきを抑えるためCS_HREDRAW | CS_VREDRAW を外した
	wc.style			= CS_DBLCLKS | CS_BYTEALIGNCLIENT | CS_BYTEALIGNWINDOW;
	wc.lpfnWndProc		= CEditWndProc;
	wc.cbClsExtra		= 0;
	wc.cbWndExtra		= sizeof(LONG_PTR) * 1;                                  //拡張領域を1個確保。
	wc.hInstance		= G_AppInstance();
	//	Dec, 2, 2002 genta アイコン読み込み方法変更
	wc.hIcon			= GetAppIcon( G_AppInstance(), ICON_DEFAULT_APP, FN_APP_ICON, false );

	wc.hCursor			= nullptr/*LoadCursor( NULL, IDC_ARROW )*/;
	wc.hbrBackground	= (HBRUSH)nullptr/*(COLOR_3DSHADOW + 1)*/;
	wc.lpszMenuName		= nullptr;	// MAKEINTRESOURCE( IDR_MENU1 );	2010/5/16 Uchi
	wc.lpszClassName	= GSTR_EDITWINDOWNAME;

	//	Dec. 6, 2002 genta
	//	small icon指定のため RegisterClassExに変更
	wc.cbSize			= sizeof( wc );
	wc.hIconSm			= GetAppIcon( G_AppInstance(), ICON_DEFAULT_APP, FN_APP_ICON, true );
	ATOM	atom = RegisterClassEx( &wc );
	if( 0 == atom ){
		//	2004.05.13 Moca return NULLを有効にした
		return nullptr;
	}

	//矩形取得
	CMyRect rc;
	_GetWindowRectForInit(&rc, nGroup, sTabGroupInfo);

	//作成
	HWND hwndResult = ::CreateWindowEx(
		0,				 	// extended window style
		GSTR_EDITWINDOWNAME,		// pointer to registered class name
		GSTR_EDITWINDOWNAME,		// pointer to window name
		WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,	// window style
		rc.left,			// horizontal position of window
		rc.top,				// vertical position of window
		rc.Width(),			// window width
		rc.Height(),		// window height
		nullptr,				// handle to parent or owner window
		nullptr,				// handle to menu or child-window identifier
		G_AppInstance(),		// handle to application instance
		this				// pointer to window-creation data
	);
	return hwndResult;
}

void CEditWnd::_GetTabGroupInfo(
	STabGroupInfo* pTabGroupInfo,
	int& nGroup
) const
{
	HWND hwndTop = nullptr;
	WINDOWPLACEMENT	wpTop = {0};

	//From Here @@@ 2003.05.31 MIK
	//タブウインドウの場合は現状値を指定
	if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd && !m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin )
	{
		if( nGroup < 0 )	// 不正なグループID
			nGroup = 0;	// グループ指定無し（最近アクティブのグループに入れる）
		EditNode*	pEditNode = CAppNodeGroupHandle(nGroup).GetEditNodeAt(0);	// グループの先頭ウィンドウ情報を取得	// 2007.06.20 ryoji
		hwndTop = pEditNode? pEditNode->GetHwnd(): nullptr;

		if( hwndTop )
		{
			//	Sep. 11, 2003 MIK 新規TABウィンドウの位置が上にずれないように
			// 2007.06.20 ryoji 非プライマリモニタまたはタスクバーを動かした後でもずれないように

			wpTop.length = sizeof(wpTop);
			if( ::GetWindowPlacement( hwndTop, &wpTop ) ){	// 現在の先頭ウィンドウから位置を取得
				if( wpTop.showCmd == SW_SHOWMINIMIZED )
					wpTop.showCmd = pEditNode->m_showCmdRestore;
			}
			else{
				hwndTop = nullptr;
			}
		}
	}
	//To Here @@@ 2003.05.31 MIK

	//結果
	pTabGroupInfo->hwndTop = hwndTop;
	pTabGroupInfo->wpTop = wpTop;
}

void CEditWnd::_AdjustInMonitor(const STabGroupInfo& sTabGroupInfo)
{
	RECT	rcOrg;
	RECT	rcDesktop;
//	int		nWork;

	//	May 01, 2004 genta マルチモニタ対応
	::GetMonitorWorkRect( GetHwnd(), &rcDesktop );
	::GetWindowRect( GetHwnd(), &rcOrg );

	// 2005.11.23 Moca マルチモニタ等で問題があったため計算方法変更
	/* ウィンドウ位置調整 */
	if( rcOrg.bottom > rcDesktop.bottom ){
		rcOrg.top -= rcOrg.bottom - rcDesktop.bottom;
		rcOrg.bottom = rcDesktop.bottom;	//@@@ 2002.01.08
	}
	if( rcOrg.right > rcDesktop.right ){
		rcOrg.left -= rcOrg.right - rcDesktop.right;
		rcOrg.right = rcDesktop.right;	//@@@ 2002.01.08
	}

	if( rcOrg.top < rcDesktop.top ){
		rcOrg.bottom += rcDesktop.top - rcOrg.top;
		rcOrg.top = rcDesktop.top;
	}
	if( rcOrg.left < rcDesktop.left ){
		rcOrg.right += rcDesktop.left - rcOrg.left;
		rcOrg.left = rcDesktop.left;
	}

	/* ウィンドウサイズ調整 */
	if( rcOrg.bottom > rcDesktop.bottom ){
		//rcOrg.bottom = rcDesktop.bottom - 1;	//@@@ 2002.01.08
		rcOrg.bottom = rcDesktop.bottom;	//@@@ 2002.01.08
	}
	if( rcOrg.right > rcDesktop.right ){
		//rcOrg.right = rcDesktop.right - 1;	//@@@ 2002.01.08
		rcOrg.right = rcDesktop.right;	//@@@ 2002.01.08
	}

	//From Here @@@ 2003.06.13 MIK
	if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd
		&& !m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin
		&& sTabGroupInfo.hwndTop )
	{
		// 現在の先頭ウィンドウから WS_EX_TOPMOST 状態を引き継ぐ	// 2007.05.18 ryoji
		DWORD dwExStyle = (DWORD)::GetWindowLongPtr( sTabGroupInfo.hwndTop, GWL_EXSTYLE );
		::SetWindowPos( GetHwnd(), (dwExStyle & WS_EX_TOPMOST)? HWND_TOPMOST: HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );

		// Keep the old tab visible while the new window loads and lays out off-screen.
		// The startup draw transaction performs one final paint before swapping z-order.
		::SetWindowPos( GetHwnd(), sTabGroupInfo.hwndTop, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
		m_startupPreviousTabWindow = sTabGroupInfo.hwndTop;
		m_startupShowCommand = (sTabGroupInfo.wpTop.showCmd == SW_SHOWMAXIMIZED)
			? SW_SHOWMAXIMIZED
			: SW_SHOWNOACTIVATE;
	}
	else
	{
		::SetWindowPos(
			GetHwnd(), nullptr,
			rcOrg.left, rcOrg.top,
			rcOrg.right - rcOrg.left, rcOrg.bottom - rcOrg.top,
			SWP_NOOWNERZORDER | SWP_NOZORDER
		);

		/* ウィンドウサイズ継承。表示は初期文書の確定後まで遅延する。 */
		if( WINSIZEMODE_DEF != m_pShareData->m_Common.m_sWindow.m_eSaveWindowSize &&
			m_pShareData->m_Common.m_sWindow.m_nWinSizeType == SIZE_MAXIMIZED ){
			m_startupShowCommand = SW_SHOWMAXIMIZED;
		}else
		// 2004.05.14 Moca ウィンドウサイズを直接指定する場合は、最小化表示を受け入れる
		if( WINSIZEMODE_SET == m_pShareData->m_Common.m_sWindow.m_eSaveWindowSize &&
			m_pShareData->m_Common.m_sWindow.m_nWinSizeType == SIZE_MINIMIZED ){
			m_startupShowCommand = SW_SHOWMINIMIZED;
		}
		else{
			m_startupShowCommand = SW_SHOW;
		}
	}
	//To Here @@@ 2003.06.13 MIK
}

/*!
	作成

	@date 2002.03.07 genta nDocumentType追加
	@date 2007.06.26 ryoji nGroup追加
	@date 2008.04.19 ryoji 初回アイドリング検出用ゼロ秒タイマーのセット処理を追加
*/
HWND CEditWnd::Create(
	[[maybe_unused]] const CEditDoc* pcEditDoc,
	CImageListMgr*	pcIcons,	//!< [in] Image List
	int				nGroup		//!< [in] グループID
)
{
	MY_RUNNINGTIMER( cRunningTimer, L"CEditWnd::Create" );

	wmemset( m_pszMenubarMessage, L' ', MENUBAR_MESSAGE_MAX_LEN );	// null終端は不要

	//	Dec. 4, 2002 genta
	InitMenubarMessageFont();

	// 2009.01.17 nasukoji	ホイールスクロール有無状態をクリア
	ClearMouseState();

	// ウィンドウ毎にアクセラレータテーブルを作成する
	CreateAccelTbl();

	//ウィンドウ数制限
	if( m_pShareData->m_sNodes.m_nEditArrNum >= MAX_EDITWINDOWS ){	//最大値修正	//@@@ 2003.05.31 MIK
		OkMessage( nullptr, LS(STR_MAXWINDOW), MAX_EDITWINDOWS );
		return nullptr;
	}

	//タブグループ情報取得
	STabGroupInfo sTabGroupInfo;
	_GetTabGroupInfo(&sTabGroupInfo, nGroup);

	// -- -- -- -- ウィンドウ作成 -- -- -- -- //
	HWND hWnd = _CreateMainWindow(nGroup, sTabGroupInfo);
	if(!hWnd)return nullptr;
	m_hWnd = hWnd;

	// 初回アイドリング検出用のゼロ秒タイマーをセットする	// 2008.04.19 ryoji
	// ゼロ秒タイマーが発動（初回アイドリング検出）したら MYWM_FIRST_IDLE を起動元プロセスにポストする。
	// ※起動元での起動先アイドリング検出については CControlTray::OpenNewEditor を参照
	::SetTimer( GetHwnd(), IDT_FIRST_IDLE, 0, nullptr );

	/* 編集ウィンドウリストへの登録 */
	// 2011.01.12 ryoji この処理は以前はウィンドウ可視化よりも後の位置にあった
	// Vista/7 での初回表示アニメーション抑止（rev1868）とのからみで、ウィンドウが可視化される時点でタブバーに全タブが揃っていないと見苦しいのでここに移動。
	// AddEditWndList() で自ウィンドウにポストされる MYWM_TAB_WINDOW_NOTIFY(TWNT_ADD) はタブバー作成後の初回アイドリング時に処理されるので特に問題は無いはず。
	if( !CAppNodeGroupHandle(nGroup).AddEditWndList( GetHwnd() ) ){	// 2007.06.26 ryoji nGroup引数追加
		OkMessage( GetHwnd(), LS(STR_MAXWINDOW), MAX_EDITWINDOWS );
		::DestroyWindow( GetHwnd() );
		m_hWnd = hWnd = nullptr;
		return hWnd;
	}

	//コモンコントロール初期化
	MyInitCommonControls();

	//イメージ、ヘルパなどの作成
	m_cMenuDrawer.Create( G_AppInstance(), GetHwnd(), pcIcons );
	m_cToolbar.Create( pcIcons );

	// プラグインコマンドを登録する
	RegisterPluginCommand();

	SelectCharWidthCache( CWM_FONT_MINIMAP, CWM_CACHE_LOCAL ); // Init
	InitCharWidthCache( m_pcViewFontMiniMap->GetLogfont(), CWM_FONT_MINIMAP );
	SelectCharWidthCache( CWM_FONT_EDIT, GetLogfontCacheMode() );
	InitCharWidthCache( GetLogfont() );

	// -- -- -- -- 子ウィンドウ作成 -- -- -- -- //

	/* 分割フレーム作成 */
	m_cSplitterWnd.Create( GetHwnd() );

	/* ビュー */
	GetView(0).Create( m_cSplitterWnd.GetHwnd(), GetDocument(), 0, TRUE, false  );
	GetView(0).OnSetFocus();

	/* 子ウィンドウの設定 */
	HWND        hWndArr[2];
	hWndArr[0] = GetView(0).GetHwnd();
	hWndArr[1] = nullptr;
	m_cSplitterWnd.SetChildWndArr( hWndArr );

	MY_TRACETIME( cRunningTimer, L"View created" );

	// -- -- -- -- 各種バー作成 -- -- -- -- //

	// メインメニュー
	LayoutMainMenu();

	/* ツールバー */
	LayoutToolBar();

	/* ステータスバー */
	LayoutStatusBar();

	/* ファンクションキー バー */
	LayoutFuncKey();

	/* タブウインドウ */
	LayoutTabBar();

	// ミニマップ
	LayoutMiniMap();

	/* バーの配置終了 */
	EndLayoutBars( FALSE );
	BeginStartupDrawTransaction();
	bool workbenchInitialized = false;
	{
		CStartupDocumentSubphaseTimer workbenchTimer{
			CStartupTrace::StartupDocumentSubphase::WorkbenchUi };
		workbenchInitialized = InitializeWorkbench();
	}
	if (!workbenchInitialized) {
		const auto message = LocalizedWorkbenchString(STR_WORKBENCH_INITIALIZE_FAILED);
		TopErrorMessage(GetHwnd(), message.c_str());
		AbortStartupDrawTransaction();
		::DestroyWindow(GetHwnd());
		m_hWnd = hWnd = nullptr;
		return hWnd;
	}
	// Rendering admission is deliberately fail-closed per window. The native GDI
	// surfaces remain authoritative if the bounded owner/finalizer capacity is
	// unavailable; workbench startup itself must not fail for that fallback.
	if (!InitializeFrameRuntime()) {
		::OutputDebugStringW(L"Sakura Editor NEXT: frame runtime unavailable; using GDI fallback.\n");
	}

	DarkMode::setChildCtrlsTheme(hWnd);
	DarkMode::setWindowMenuBarSubclass(hWnd);
	DarkMode::setChildCtrlsSubclassAndTheme(hWnd);
	m_cStatusBar.InstallPaletteSubclass();

	// -- -- -- -- その他調整など -- -- -- -- //

	// 画面表示直前にDispatchEventを有効化する
	m_dispatchReady = true;

	// デスクトップからはみ出さないようにする
	_AdjustInMonitor(sTabGroupInfo);

	// ドロップされたファイルを受け入れる
	::DragAcceptFiles( GetHwnd(), TRUE );
	m_pcDropTarget->Register_DropTarget( m_hWnd );	// 右ボタンドロップ用	// 2008.06.20 ryoji

	//アクティブ情報
	m_bIsActiveApp = ( ::GetActiveWindow() == GetHwnd() );	// 2007.03.08 ryoji

	// PeekMessageの結果を受け取る構造体
	MSG msg{};

	// メッセージキューを作成する
	::PeekMessageW(&msg, hWnd, WM_USER, WM_USER, PM_NOREMOVE);

	// エディタ－トレイ間でのUI特権分離の確認（Vista UIPI機能） 2007.06.07 ryoji
	CStartupTrace::Mark(CStartupTrace::Event::UipiCheckBegin);
	if (const auto hWndTray = m_pShareData->m_sHandles.m_hwndTray) {
		// 戻り値取得用変数（成功するとhWndが返って来る）
		DWORD_PTR dwRes = 0;

		// コントロールプロセスにMYWM_UIPI_CHECKを送る
		::SetLastError(ERROR_SUCCESS);
		const LRESULT sendResult = ::SendMessageTimeoutW(
			hWndTray, MYWM_UIPI_CHECK, 0L, LPARAM(hWnd), SMTO_NORMAL, 10000, &dwRes);
		const DWORD sendError = sendResult ? ERROR_SUCCESS : ::GetLastError();

		// メッセージ返送を回収する（とれない場合もあるが問題はない。）
		::PeekMessageW(&msg, hWnd, MYWM_UIPI_CHECK, MYWM_UIPI_CHECK, PM_REMOVE | PM_QS_SENDMESSAGE);
		CStartupTrace::Mark(CStartupTrace::Event::UipiCheckEnd, dwRes ? 1 : 0, sendError);

		if (!dwRes) {	// 送信失敗
			TopErrorMessage( GetHwnd(),
				LS(STR_ERR_DLGEDITWND02)
			);
			AbortStartupDrawTransaction();
			::DestroyWindow( GetHwnd() );
			m_hWnd = hWnd = nullptr;
			return hWnd;
		}
	} else {
		// -1 is an explicit "no IPC HWND" outcome.  Future minimal-ready work must
		// preserve the UIPI contract instead of silently taking this branch.
		CStartupTrace::Mark(CStartupTrace::Event::UipiCheckEnd, -1);
	}

	CShareData::getInstance()->SetTraceOutSource( GetHwnd() );	// TraceOut()起動元ウィンドウの設定	// 2006.06.26 ryoji

	//	Aug. 29, 2003 wmlhq
	m_nTimerCount = 0;
	/* タイマーを起動 */ // タイマーのIDと間隔を変更 20060128 aroka
	if( 0 == ::SetTimer( GetHwnd(), IDT_EDIT, 500, nullptr ) ){
		WarningMessage( GetHwnd(), LS(STR_ERR_DLGEDITWND03) );
	}
	// ツールバーのタイマーを分離した 20060128 aroka
	Timer_ONOFF( true );

	//デフォルトのIMEモード設定
	GetDocument()->m_cDocEditor.SetImeMode( GetDocument()->m_cDocType.GetDocumentAttribute().m_nImeState );

	return GetHwnd();
}

//! 起動時のファイルオープン処理
void CEditWnd::OpenDocumentWhenStart(
	const SLoadInfo& _sLoadInfo		//!< [in]
)
{
	if( _sLoadInfo.cFilePath.Length() ){
		if (!IsStartupDrawSuppressed()) {
			::ShowWindow( GetHwnd(), SW_SHOW );
		}
		//	Oct. 03, 2004 genta コード確認は設定に依存
		SLoadInfo	sLoadInfo = _sLoadInfo;
		bool		bReadResult = GetDocument()->m_cDocFileOperation.FileLoadWithoutAutoMacro(&sLoadInfo);	// 自動実行マクロは後で別の場所で実行される
		if( !bReadResult ){
			/* ファイルが既に開かれている */
			if( sLoadInfo.bOpened ){
				::PostMessageAny( GetHwnd(), WM_CLOSE, 0, 0 );
				// 2004.07.12 Moca return NULLだと、メッセージループを通らずにそのまま破棄されてしまい、タブの終了処理が抜ける
				//	この後は正常ルートでメッセージループに入った後WM_CLOSEを受信して直ちにCLOSE & DESTROYとなる．
				//	その中で編集ウィンドウの削除が行われる．
			}
		}
	}
}

void CEditWnd::SetDocumentTypeWhenCreate(
	ECodeType		nCharCode,		//!< [in] 漢字コード
	bool			bViewMode,		//!< [in] ビューモードで開くかどうか
	CTypeConfig		nDocumentType	//!< [in] 文書タイプ．-1のとき強制指定無し．
)
{
	//	Mar. 7, 2002 genta 文書タイプの強制指定
	//	Jun. 4 ,2004 genta ファイル名指定が無くてもタイプ強制指定を有効にする
	if( nDocumentType.IsValidType() ){
		GetDocument()->m_cDocType.SetDocumentType( nDocumentType, true );
		//	2002/05/07 YAZAKI タイプ別設定一覧の一時適用のコードを流用
		GetDocument()->m_cDocType.LockDocumentType();
	}

	// 文字コードの指定	2008/6/14 Uchi
	if( IsValidCodeType( nCharCode ) || nDocumentType.IsValidType() ){
		const STypeConfig& types = GetDocument()->m_cDocType.GetDocumentAttribute();
		ECodeType eDefaultCharCode = types.m_encoding.m_eDefaultCodetype;
		if( !IsValidCodeType( nCharCode ) ){
			nCharCode = eDefaultCharCode;	// 直接コード指定がなければタイプ指定のデフォルト文字コードを使用
		}
		if( nCharCode == eDefaultCharCode ){	// デフォルト文字コードと同じ文字コードが選択されたとき
			GetDocument()->SetDocumentEncoding( nCharCode, types.m_encoding.m_bDefaultBom );
			GetDocument()->m_cDocEditor.m_cNewLineCode = types.m_encoding.m_eDefaultEoltype;
		}
		else{
			GetDocument()->SetDocumentEncoding( nCharCode, CCodeTypeName( nCharCode ).IsBomDefOn() );
			GetDocument()->m_cDocEditor.m_cNewLineCode = EEolType::cr_and_lf;
		}
	}

	//	Jun. 4 ,2004 genta ファイル名指定が無くてもビューモード強制指定を有効にする
	CAppMode::getInstance()->SetViewMode(bViewMode);

	if( nDocumentType.IsValidType() ){
		/* 設定変更を反映させる */
		GetDocument()->OnChangeSetting();	// <--- 内部に BlockingHook() 呼び出しがあるので溜まった描画がここで実行される
	}
}

/*! メインメニューの配置処理
	@date 2010/05/16 Uchi
	@date 2012/10/18 syat 各国語対応
*/
void CEditWnd::LayoutMainMenu()
{
	WCHAR		szLabel[300];
	WCHAR		szKey[10];

	const auto pcMenu = &m_pShareData->m_Common.m_sMainMenu;

	HWND		hWnd = GetHwnd();
	int 		j;
	int 		nCount;
	LPCWSTR		pszName;

	const auto hMenu = ::CreateMenu();

	for (int i = 0; i < MAX_MAINMENU_TOP && pcMenu->m_nMenuTopIdx[i] >= 0; i++) {
		nCount = ( i >= MAX_MAINMENU_TOP || pcMenu->m_nMenuTopIdx[i+1] < 0 ? pcMenu->m_nMainMenuNum : pcMenu->m_nMenuTopIdx[i+1] )
				- pcMenu->m_nMenuTopIdx[i];		// メニュー項目数
		const auto cMainMenu = &pcMenu->m_cMainMenuTbl[pcMenu->m_nMenuTopIdx[i]];
		switch (cMainMenu->m_nType) {
		case T_NODE:
			// ラベル未設定かつFunctionコードがありならストリングテーブルから取得 2012/10/18 syat 各国語対応
			pszName = ( cMainMenu->m_sName[0] == L'\0' && cMainMenu->m_nFunc != F_NODE )
								? LS( cMainMenu->m_nFunc ) : cMainMenu->m_sName;
			::AppendMenu( hMenu, MF_POPUP | MF_STRING | (nCount<=1 ? MF_GRAYED : 0), (UINT_PTR)CreatePopupMenu(),
				CKeyBind::MakeMenuLabel( pszName, cMainMenu->m_sKey ) );
			break;
		case T_LEAF:
			/* メニューラベルの作成 */
			// 2014.05.04 Moca プラグイン/マクロ等を置けるようにFunccode2Nameを使うように
			GetDocument()->m_cFuncLookup.Funccode2Name( cMainMenu->m_nFunc, szLabel, int(std::size(szLabel)) );
			wcscpy( szKey, cMainMenu->m_sKey );
			if (CKeyBind::GetMenuLabel(
				G_AppInstance(),
				m_pShareData->m_Common.m_sKeyBind.m_nKeyNameArrNum,
				m_pShareData->m_Common.m_sKeyBind.m_pKeyNameArr,
				cMainMenu->m_nFunc,
				szLabel,
				cMainMenu->m_sKey,
				FALSE,
				int(std::size(szLabel))) == nullptr) {
				wcscpy( szLabel, L"?" );
			}
			::AppendMenu( hMenu, MF_STRING, cMainMenu->m_nFunc, szLabel );
			break;
		case T_SEPARATOR:
			::AppendMenu( hMenu, MF_SEPARATOR, 0, nullptr );
			break;
		case T_SPECIAL:
			nCount = 0;
			switch (cMainMenu->m_nFunc) {
			case F_WINDOW_LIST:				// ウィンドウリスト
				EditNode*	pEditNodeArr;
				nCount = CAppNodeManager::getInstance()->GetOpenedWindowArr( &pEditNodeArr, TRUE );
				delete [] pEditNodeArr;
				break;
			case F_FILE_USED_RECENTLY:		// typed workspaces/folders followed by recent files
				{
					CRecentFile	cRecentFile;
				nCount = cRecentFile.GetViewCount() + (HasRecentlyOpenedItems() ? 1 : 0);
				}
				break;
			case F_RECENT_WORKSPACE_LIST:
				// Open Recent always contributes Clear Recently Opened, so the
				// submenu stays enabled exactly as it does in VS Code.
				nCount = 1;
				break;
			case F_FOLDER_USED_RECENTLY:	// 最近使ったフォルダー
				{
					CRecentFolder	cRecentFolder;
					nCount = cRecentFolder.GetViewCount();
				}
				break;
			case F_CUSTMENU_LIST:			// カスタムメニューリスト
				//	右クリックメニュー
				if (m_pShareData->m_Common.m_sCustomMenu.m_nCustMenuItemNumArr[0] > 0) {
					nCount++;
				}
				//	カスタムメニュー
				for (j = 1; j < MAX_CUSTOM_MENU; ++j) {
					if (m_pShareData->m_Common.m_sCustomMenu.m_nCustMenuItemNumArr[j] > 0) {
						nCount++;
					}
				}
				break;
			case F_USERMACRO_LIST:			// 登録済みマクロリスト
				for (j = 0; j < MAX_CUSTMACRO; ++j) {
					MacroRec *mp = &m_pShareData->m_Common.m_sMacro.m_MacroTable[j];
					if (mp->IsEnabled()) {
						nCount++;
					}
				}
				break;
			case F_PLUGIN_LIST:				// プラグインコマンドリスト
				//プラグインコマンドを提供するプラグインを列挙する
				{
					const CJackManager* pcJackManager = CJackManager::getInstance();

					CPlug::Array plugs = pcJackManager->GetPlugs( PP_COMMAND );
					for( CPlug::ArrayIter it = plugs.cbegin(); it != plugs.cend(); it++ ){
						nCount++;
					}
				}
				break;
			default:
				break;
			}
			::AppendMenu( hMenu, MF_POPUP | MF_STRING | (nCount<=0 ? MF_GRAYED : 0), (UINT_PTR)CreatePopupMenu(),
				CKeyBind::MakeMenuLabel( LS(cMainMenu->m_nFunc), cMainMenu->m_sKey ) );
			break;
		}
	}
	HMENU hMenuOld = nullptr;
	if (m_customFrame) {
		hMenuOld = m_customFrame->ReplaceMenu(hMenu);
	} else {
		hMenuOld = ::GetMenu(hWnd);
		::SetMenu(hWnd, hMenu);
	}
	if( hMenuOld ){
		DestroyMenu( hMenuOld );
	}

	if (m_customFrame) {
		m_customFrame->InvalidateTitle();
	} else {
		DarkMode::setWindowMenuBarSubclass(hWnd);
		::DrawMenuBar(hWnd);
	}
}

HMENU CEditWnd::GetMainMenuHandle() const noexcept
{
	return m_customFrame ? m_customFrame->GetMenu() : ::GetMenu(GetHwnd());
}

/*! ツールバーの配置処理
	@date 2006.12.19 ryoji 新規作成
*/
void CEditWnd::LayoutToolBar( void )
{
	if( m_pShareData->m_Common.m_sWindow.m_bDispTOOLBAR ){	/* ツールバーを表示する */
		m_cToolbar.CreateToolBar();
		m_cToolbar.UpdateToolbar();
	}else{
		m_cToolbar.DestroyToolBar();
	}
}

/*! ステータスバーの配置処理
	@date 2006.12.19 ryoji 新規作成
*/
void CEditWnd::LayoutStatusBar( void )
{
	if( m_pShareData->m_Common.m_sWindow.m_bDispSTATUSBAR ){	/* ステータスバーを表示する */
		/* ステータスバー作成 */
		m_cStatusBar.CreateStatusBar();
	}
	else{
		/* ステータスバー破棄 */
		m_cStatusBar.DestroyStatusBar();
	}
}

/*! ファンクションキーの配置処理
	@date 2006.12.19 ryoji 新規作成
*/
void CEditWnd::LayoutFuncKey( void )
{
	if( m_pShareData->m_Common.m_sWindow.m_bDispFUNCKEYWND ){	/* ファンクションキーを表示する */
		if( nullptr == m_cFuncKeyWnd.GetHwnd() ){
			bool	bSizeBox;
			if( m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ){	/* ファンクションキー表示位置／0:上 1:下 */
				bSizeBox = false;
			}else{
				bSizeBox = true;
				/* ステータスバーがあるときはサイズボックスを表示しない */
				if( m_cStatusBar.GetStatusHwnd() ){
					bSizeBox = false;
				}
			}
			m_cFuncKeyWnd.Open( G_AppInstance(), GetHwnd(), GetDocument(), bSizeBox );
		}
	}else{
		m_cFuncKeyWnd.Close();
	}
}

/*! タブバーの配置処理
	@date 2006.12.19 ryoji 新規作成
*/
void CEditWnd::LayoutTabBar( void )
{
	if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd ){	/* タブバーを表示する */
		if( nullptr == m_cTabWnd.GetHwnd() ){
			m_cTabWnd.Open( G_AppInstance(), GetHwnd() );
			// タブバーが後から作成された場合、ダークモードのテーマを適用する
			if( IsDarkModeActive() ){
				DarkMode::setChildCtrlsSubclassAndTheme( m_cTabWnd.GetHwnd() );
			}
		}else{
			m_cTabWnd.UpdateStyle();
		}
	}else{
		m_cTabWnd.Close();
		m_cTabWnd.SizeBox_ONOFF(false);
	}
}

/*! ミニマップの配置処理
	@date 2014.07.14 新規作成
*/
void CEditWnd::LayoutMiniMap( void )
{
	const bool enabled = m_workbenchRuntime != nullptr
		? m_miniMapOptions.enabled
		: m_pShareData->m_Common.m_sWindow.m_bDispMiniMap;
	if( enabled ){
		if( !m_cMiniMapView.GetHwnd() ){
			m_cMiniMapView.Create( GetHwnd() );
		}
		if( m_cMiniMapView.GetHwnd() ) m_cMiniMapView.SetMiniMapOptions(m_miniMapOptions);
	}else{
		if( m_cMiniMapView.GetHwnd() ){
			m_cMiniMapView.Close();
		}
	}
}

/*! バーの配置終了処理
	@date 2006.12.19 ryoji 新規作成
	@date 2007.03.04 ryoji 印刷プレビュー時はバーを隠す
	@date 2011.01.21 ryoji アウトライン画面にゴミが描画されるのを抑止する
*/
void CEditWnd::EndLayoutBars( BOOL bAdjust/* = TRUE*/ )
{
	int nCmdShow = m_pPrintPreview? SW_HIDE: SW_SHOW;
	if (const auto hwndToolBar = (nullptr != m_cToolbar.GetRebarHwnd()) ? m_cToolbar.GetRebarHwnd() : m_cToolbar.GetToolbarHwnd())
		::ShowWindow( hwndToolBar, nCmdShow );
	if( m_cStatusBar.GetStatusHwnd() )
		::ShowWindow( m_cStatusBar.GetStatusHwnd(), nCmdShow );
	if( nullptr != m_cFuncKeyWnd.GetHwnd() )
		::ShowWindow( m_cFuncKeyWnd.GetHwnd(), nCmdShow );
	if( nullptr != m_cTabWnd.GetHwnd() )
		::ShowWindow( m_cTabWnd.GetHwnd(), nCmdShow );
	if( nullptr != m_cDlgFuncList.GetHwnd() && m_cDlgFuncList.IsDocking() ){
		::ShowWindow( m_cDlgFuncList.GetHwnd(), nCmdShow );
		// アウトラインを最背後にしておく（ゴミ描画の抑止策）
		// この対策以前は、アウトラインを下ドッキングしている状態で、
		// メニューから[ファンクションキーを表示]/[ステータスバーを表示]を実行して非表示のバーをアウトライン直下に表示したり、
		// その後、ウィンドウの下部境界を上下ドラッグしてサイズ変更するとゴミが現れることがあった。
		::SetWindowPos( m_cDlgFuncList.GetHwnd(), HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE );
	}
	if( m_cMiniMapView.GetHwnd() ){
		::ShowWindow( m_cMiniMapView.GetHwnd(), nCmdShow );
	}
	if (m_markdownPreview) {
		m_markdownPreview->Show(nCmdShow == SW_SHOW && m_markdownPreviewVisible);
	}

	if( bAdjust )
	{
		RECT		rc;
		m_cSplitterWnd.DoSplit( -1, -1 );
		::GetClientRect( GetHwnd(), &rc );
		auto nWinSizeType = m_nWinSizeType;
		::SendMessage( GetHwnd(), WM_SIZE, 0, 0 ); // ツールバーの表示ON/OFFを行うとちらつきが発生する事への対策
		::SendMessage( GetHwnd(), WM_SIZE, nWinSizeType, MAKELONG( rc.right - rc.left, rc.bottom - rc.top ) );
		// Queue the frame repaint after the bar visibility transaction.  The
		// children own their complete retained pixels, so an erase/update-now
		// pass only exposes an intermediate blank frame and recurses into paint.
		::RedrawWindow( GetHwnd(), nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_NOERASE );

		GetActiveView().SetIMECompFormPos();
	}
}

static inline BOOL MyIsDialogMessage(HWND hwnd, MSG* msg)
{
	if(hwnd==nullptr)return FALSE;
	return ::IsDialogMessage(hwnd, msg);
}

//複数プロセス版
/* メッセージループ */
//2004.02.17 Moca GetMessageのエラーチェック
void CEditWnd::MessageLoop( void )
{
	MSG	msg;
	int ret;

	auto hWndDM = GetHwnd();
	DarkMode::setDarkWndNotifySafeEx(hWndDM, false, true);
	// setDarkWndNotifySafeEx recursively installs darkmodelib's status-bar painter.
	// Sakura owns the workbench status palette, so keep our painter last in the chain.
	m_cStatusBar.InstallPaletteSubclass();

	while(GetHwnd())
	{
		//メッセージ取得
		ret = GetMessage(&msg,nullptr,0,0);
		if(ret== 0)break; //WM_QUIT
		if(ret==-1)break; //GetMessage失敗
		if (m_workbenchKeybindingState.IsChordPending()
			&& m_workbenchKeybindingState.CancelIfFocusChanged(
				reinterpret_cast<workbench::editor::WorkbenchKeybindingState::FocusToken>(::GetFocus()))) {
			ClearWorkbenchKeybindingChord();
		}

		//ダイアログメッセージ
		     if( MyIsDialogMessage( CPrintPreview::GetPrintPreviewBarHANDLE_Safe(m_pPrintPreview.get()),	&msg ) ){}	//!< 印刷プレビュー 操作バー
		else if( MyIsDialogMessage( m_cDlgFind.GetHwnd(),								&msg ) ){}	//!<「検索」ダイアログ
		else if( MyIsDialogMessage( m_cDlgFuncList.GetHwnd(),							&msg ) ){}	//!<「アウトライン」ダイアログ
		else if( MyIsDialogMessage( m_cDlgReplace.GetHwnd(),							&msg ) ){}	//!<「置換」ダイアログ
		else if( MyIsDialogMessage( m_cDlgGrep.GetHwnd(),								&msg ) ){}	//!<「Grep」ダイアログ
		else if( MyIsDialogMessage( m_cHokanMgr.GetHwnd(),								&msg ) ){}	//!<「入力補完」
		else if( m_cToolbar.EatMessage(&msg ) ){ }													//!<ツールバー
		else if( PreTranslateWorkbenchMessage(msg) ){}
		else if( m_customFrame && m_customFrame->PreTranslateMessage(msg) ){}
		//アクセラレータ
		else{
			// 補完ウィンドウが表示されているときはキーボード入力を先に処理させる（カーソル移動／決定／キャンセルの処理）
			if (HasActiveEditorInput() && WM_KEYDOWN == msg.message &&
				GetActiveView().m_bHokan &&
				-1 == m_cHokanMgr.KeyProc(msg.wParam, msg.lParam)) {
						continue;	// 補完ウィンドウが処理を実行した
			}

			if( m_hAccel && TranslateAccelerator( msg.hwnd, m_hAccel, &msg ) ){}
			//通常メッセージ
			else{
				TranslateMessage( &msg );
				DispatchMessage( &msg );
			}
		}
	}
}

LRESULT CEditWnd::DispatchEvent(
	HWND	hwnd,	// handle of window
	UINT	uMsg,	// message identifier
	WPARAM	wParam,	// first message parameter
	LPARAM	lParam 	// second message parameter
)
{
	const auto hWnd = GetHwnd();
	if (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS
		|| (uMsg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
		|| (uMsg == WM_ACTIVATEAPP && wParam == FALSE)) {
		// Focus/activation notifications are delivered synchronously to the native
		// window procedure, so they cannot be relied on by MessageLoop's prefilter.
		ClearWorkbenchKeybindingChord();
	}
	if (uMsg == WM_WINDOWPOSCHANGING && IsStartupDrawSuppressed()) {
		if (auto* position = reinterpret_cast<WINDOWPOS*>(lParam)) {
			position->flags &= ~SWP_SHOWWINDOW;
		}
	}
	LRESULT customFrameResult = 0;
	if (m_customFrame && m_customFrame->HandleWindowMessage(uMsg, wParam, lParam, customFrameResult)) {
		if (uMsg == WM_DISPLAYCHANGE) {
			UpdateFrameRuntimeCadence(true);
			::RedrawWindow(hwnd, nullptr, nullptr,
				RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		}
		return customFrameResult;
	}
	if (uMsg == WM_DISPLAYCHANGE) {
		UpdateFrameRuntimeCadence(true);
		::RedrawWindow(hwnd, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}
	if (const auto event = sakura::editor::win32::Win32EditorFrameAdapter::Translate(
		uMsg, wParam, lParam)) {
		const auto effect = sakura::editor::DispatchEditorFrameEvent(*this, *event);
		return sakura::editor::win32::Win32EditorFrameAdapter::Apply(
			hwnd, uMsg, wParam, lParam, effect);
	}

	int					nRet;
	LPNMHDR				pnmh;
	int					nPane;
	EditInfo*			pfi;

	UINT				idCtl;	/* コントロールのID */
	LPDRAWITEMSTRUCT	lpdis;	/* 項目描画情報 */
	UINT				uItem;
	LRESULT				lRes;
	CTypeConfig			cTypeNew;

	switch( uMsg ){
	case WM_PAINTICON:
		return 0;
	case WM_ICONERASEBKGND:
		return 0;
	case MYWM_EDITOR_CORE_CHANGED:
		RefreshEditorCorePresentation();
		return 0;
	case MYWM_WORKBENCH_LAYOUT_CHANGED:
		OnWorkbenchLayoutStateChanged();
		return 0;
	case MYWM_WORKBENCH_SERVICE_PROJECTION_CHANGED:
		OnWorkbenchServiceProjectionChanged();
		return 0;
	case MYWM_WORKBENCH_UPDATE_STATE_CHANGED:
		OnWorkbenchUpdateStateChanged();
		return 0;
	case MYWM_WORKBENCH_THEME_CHANGED:
		if (m_themeConfigurationGate) {
			std::lock_guard lock(m_themeConfigurationGate->mutex);
			m_themeConfigurationGate->messageQueued = false;
		}
		(void)ApplyWorkbenchTheme();
		ApplyActivityBarLocationSetting();
		ApplyMiniMapSettings();
		ApplyIndentGuideSettings();
		// Re-read `scm.countBadge`; the count itself has not moved, but whether it
		// is shown at all may have.
		SyncScmActivityBadge();
		// The commit box is sized from configuration too, and this is the same
		// notification that tells the window its effective settings moved.
		ApplyScmInputLineCountSetting();
		ApplyTerminalScrollbackSetting();
		ApplyTerminalTabPresentationSettings();
		return 0;
	case MYWM_COMPLETE_STARTUP_WORKBENCH:
		m_startupWorkbenchCompletionPosted = false;
		if (m_startupDrawState == StartupDrawState::Committed
			&& m_startupFirstContentPainted
			&& m_cDlgFuncList.m_bEditWndReady) {
			CompleteDeferredStartupWorkbench();
		}
		return 0;
	case WM_LBUTTONDOWN:
		return OnLButtonDown( wParam, lParam );
	case WM_MOUSEMOVE:
		return OnMouseMove( wParam, lParam );
	case WM_LBUTTONUP:
		return OnLButtonUp( wParam, lParam );
	case WM_SETCURSOR:
		return OnSetCursor( wParam, lParam );
	case WM_CAPTURECHANGED:
		return OnCaptureChanged( lParam );
	case WM_CANCELMODE:
		CancelWorkbenchResize();
		CancelMarkdownPreviewResize();
		return 0;
	case WM_MOUSEWHEEL:
		// A child that does not consume a synchronously forwarded wheel lets
		// DefWindowProc propagate it back to this frame.  End that propagated
		// message here; forwarding it again would recurse until stack overflow.
		if (m_mouseWheelForwarding) return 0L;
		// VS Code scrolls whatever the pointer is over, not whatever holds the
		// keyboard focus.  Win32 delivers WM_MOUSEWHEEL to the focus window, and
		// SPI_GETMOUSEWHEELROUTING's hybrid default only redirects to a hovered
		// *other* application, so inside this frame a hovered Source Control /
		// Explorer list would never see the wheel while the editor has focus.
		// Forward to the hovered descendant instead; it keeps its own scroll
		// authority and answers with its own handler.
		if (const HWND hovered = HoveredScrollTarget(lParam); hovered != nullptr) {
			ScopedMouseWheelForward forwarding(m_mouseWheelForwarding);
			return ::SendMessageW(hovered, WM_MOUSEWHEEL, wParam, lParam);
		}
		if (!HasActiveEditorInput() && !m_pPrintPreview) return 0;
		return OnMouseWheel( wParam, lParam );
	case WM_HSCROLL:
		return OnHScroll( wParam, lParam );
	case WM_VSCROLL:
		return OnVScroll( wParam, lParam );

	case WM_MENUCHAR:
		/* メニューアクセスキー押下時の処理(WM_MENUCHAR処理) */
		return m_cMenuDrawer.OnMenuChar( hwnd, uMsg, wParam, lParam );

	// 2007.09.09 Moca 互換BMPによる画面バッファ
	case WM_SHOWWINDOW: {
		if( !wParam ){
			Views_DeleteCompatibleBitmap();
		}
		const auto result = ::DefWindowProc( hwnd, uMsg, wParam, lParam );
		if (m_commandPaletteOverlay) {
			if (!wParam) m_commandPaletteOverlay->Cancel();
			else m_commandPaletteOverlay->Layout();
		}
		return result;
	}

	case WM_MENUSELECT:
		if( nullptr == m_cStatusBar.GetStatusHwnd() ){
			return 1;
		}
		uItem = (UINT) LOWORD(wParam);		// menu item or submenu index
		{
			/* メニュー機能のテキストをセット */
			CNativeW	cmemWork;

			/* 機能に対応するキー名の取得(複数) */
			CNativeW**	ppcAssignedKeyList;
			int			nAssignedKeyNum;
			int			j;
			nAssignedKeyNum = CKeyBind::GetKeyStrList(
				G_AppInstance(),
				m_pShareData->m_Common.m_sKeyBind.m_nKeyNameArrNum,
				(KEYDATA*)m_pShareData->m_Common.m_sKeyBind.m_pKeyNameArr,
				&ppcAssignedKeyList,
				uItem
			);
			if( 0 < nAssignedKeyNum ){
				for( j = 0; j < nAssignedKeyNum; ++j ){
					if( j > 0 ){
						cmemWork.AppendString(L" , ");
					}
					cmemWork.AppendNativeData( *ppcAssignedKeyList[j] );
					delete ppcAssignedKeyList[j];
				}
				delete [] ppcAssignedKeyList;
			}

			const WCHAR* pszItemStr = cmemWork.GetStringPtr();

			m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, pszItemStr);
		}
		return 0;

	case WM_DRAWITEM:
		idCtl = (UINT) wParam;				/* コントロールのID */
		lpdis = (DRAWITEMSTRUCT*) lParam;	/* 項目描画情報 */
		if( IDW_STATUSBAR == idCtl ){
			if( 5 == lpdis->itemID ){ // 2003.08.26 Moca idがずれて作画されなかった
				int	nColor;
				if( m_pShareData->m_sFlags.m_bRecordingKeyMacro	/* キーボードマクロの記録中 */
				 && m_pShareData->m_sFlags.m_hwndRecordingKeyMacro == GetHwnd()	/* キーボードマクロを記録中のウィンドウ */
				){
					nColor = COLOR_BTNTEXT;
				}else{
					nColor = COLOR_3DSHADOW;
				}
				::SetTextColor(lpdis->hDC, m_cStatusBar.GetTextColor());
				::SetBkMode( lpdis->hDC, TRANSPARENT );

				// 2003.08.26 Moca 上下中央位置に作画
				TEXTMETRIC tm;
				::GetTextMetrics( lpdis->hDC, &tm );
				int y = ( lpdis->rcItem.bottom - lpdis->rcItem.top - tm.tmHeight + 1 ) / 2 + lpdis->rcItem.top;
				::TextOutW(lpdis->hDC, lpdis->rcItem.left, y, PSZ_ARGS(L"REC"));
				if( COLOR_BTNTEXT == nColor ){
					::TextOutW(lpdis->hDC, lpdis->rcItem.left + 1, y, PSZ_ARGS(L"REC"));
				}
			}
			return 0;
		}
		return FALSE;
	case WM_PAINT: {
		const auto result = OnPaint( hwnd, uMsg, wParam, lParam );
		if (m_commandPaletteOverlay) m_commandPaletteOverlay->Layout();
		return result;
	}

	case WM_PASTE:
		if (!HasActiveEditorInput()) return 0;
		return GetActiveView().GetCommander().HandleCommand( F_PASTE, true, 0, 0, 0, 0 );

	case WM_COPY:
		if (!HasActiveEditorInput()) return 0;
		return GetActiveView().GetCommander().HandleCommand( F_COPY, true, 0, 0, 0, 0 );

	case WM_HELP:
		if (const auto lphi = (LPHELPINFO) lParam; lphi && HELPINFO_MENUITEM == lphi->iContextType) {
			MyWinHelp( hwnd, HELP_CONTEXT, FuncID_To_HelpContextID( (EFunctionCode)lphi->iCtrlId ) );
		}
		return TRUE;

	case WM_WINDOWPOSCHANGED: {
		// ポップアップウィンドウの表示切替指示をポストする	// 2007.10.22 ryoji
		// ・WM_SHOWWINDOWはすべての表示切替で呼ばれるわけではないのでWM_WINDOWPOSCHANGEDで処理
		//   （タブグループ解除などの設定変更時はWM_SHOWWINDOWは呼ばれない）
		// ・即時切替だとタブ切替に干渉して元のタブに戻ってしまうことがあるので後で切り替える
		if (const auto pwp = (WINDOWPOS*)lParam;
			pwp->flags & SWP_SHOWWINDOW)
			::PostMessage( hwnd, MYWM_SHOWOWNEDPOPUPS, TRUE, 0 );
		else if( pwp->flags & SWP_HIDEWINDOW )
			::PostMessage( hwnd, MYWM_SHOWOWNEDPOPUPS, FALSE, 0 );

		const auto result = ::DefWindowProc( hwnd, uMsg, wParam, lParam );
		if (m_commandPaletteOverlay) m_commandPaletteOverlay->Layout();
		return result;
	}

	case MYWM_SHOWOWNEDPOPUPS:
		::ShowOwnedPopups( m_hWnd, (BOOL)wParam );	// 2007.10.22 ryoji
		return 0L;

	case WM_SYSCOMMAND:
		// タブまとめ表示では閉じる動作はオプション指定に従う	// 2006.02.13 ryoji
		//	Feb. 11, 2007 genta 動作を選べるように(MDI風と従来動作)
		// 2007.02.22 ryoji Alt+F4 のデフォルト機能でモード毎の動作が得られるようになった
		if( wParam == SC_CLOSE ){
			// 印刷プレビューモードでウィンドウを閉じる操作のときはプレビューを閉じる	// 2007.03.04 ryoji
			if( m_pPrintPreview ){
				PrintPreviewModeONOFF();	// 印刷プレビューモードのオン/オフ
				return 0L;
			}
			OnCommand( 0, (WORD)CKeyBind::GetDefFuncCode( VK_F4, _ALT ), nullptr );
			return 0L;
		}
		return DefWindowProc( hwnd, uMsg, wParam, lParam );
#if 0
	case WM_IME_COMPOSITION:
		if ( lParam & GCS_RESULTSTR ) {
			/* メッセージの配送 */
			return Views_DispatchEvent( hwnd, uMsg, wParam, lParam );
		}else{
			return DefWindowProc( hwnd, uMsg, wParam, lParam );
		}
#endif
	//case WM_KILLFOCUS:
	case WM_CHAR:
	case WM_IME_CHAR:
	case WM_KEYUP:
	case WM_SYSKEYUP:	// 2004.04.28 Moca ALT+キーのキーリピート処理のため追加
	case WM_ENTERMENULOOP:
#if 0
	case MYWM_IME_REQUEST:   /*  再変換対応 by minfu 2002.03.27  */ // 20020331 aroka
#endif
		if (!HasActiveEditorInput()) return 0;
		if( GetActiveView().m_nAutoScrollMode ){
			GetActiveView().AutoScrollExit();
		}
		/* メッセージの配送 */
		return Views_DispatchEvent( hwnd, uMsg, wParam, lParam );

	case WM_EXITMENULOOP:
//		MYTRACE( L"WM_EXITMENULOOP\n" );
		if( nullptr != m_cStatusBar.GetStatusHwnd() ){
			m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, L"");
		}
		if (!HasActiveEditorInput()) return 0;
		/* メッセージの配送 */
		return Views_DispatchEvent( hwnd, uMsg, wParam, lParam );

	case WM_NOTIFY:
		pnmh = (LPNMHDR) lParam;
		//	From Here Feb. 15, 2004 genta
		//	ステータスバーのダブルクリックでモード切替ができるようにする
		if( m_cStatusBar.GetStatusHwnd() && pnmh->hwndFrom == m_cStatusBar.GetStatusHwnd() ){
			if (!HasActiveEditorInput()) return 0L;
			if( pnmh->code == NM_DBLCLK ){
				LPNMMOUSE mp = (LPNMMOUSE) lParam;
				if( mp->dwItemSpec == 6 ){	//	上書き/挿入
					GetDocument()->HandleCommand( F_CHGMOD_INS );
				}
				else if( mp->dwItemSpec == 5 ){	//	マクロの記録開始・終了
					GetDocument()->HandleCommand( F_RECKEYMACRO );
				}
				else if( mp->dwItemSpec == 1 ){	//	桁位置→行番号ジャンプ
					GetDocument()->HandleCommand( F_JUMP_DIALOG );
				}
				else if( mp->dwItemSpec == 3 ){	//	文字コード→各種コード
					ShowCodeBox( GetDocument(), GetActiveView() );
				}
				else if( mp->dwItemSpec == 4 ){	//	文字コードセット→文字コードセット指定
					GetDocument()->HandleCommand( F_CHG_CHARSET );
				}
			}
			else if( pnmh->code == NM_RCLICK ){
				LPNMMOUSE mp = (LPNMMOUSE) lParam;
				if( mp->dwItemSpec == 2 ){	//	入力改行モード
					m_cMenuDrawer.ResetContents();
					HMENU hMenuPopUp = ::CreatePopupMenu();
					m_cMenuDrawer.MyAppendMenu( hMenuPopUp, MF_BYPOSITION | MF_STRING, F_CHGMOD_EOL_CRLF,
						LS( F_CHGMOD_EOL_CRLF ), L"C" ); // 入力改行コード指定(CRLF)
					m_cMenuDrawer.MyAppendMenu( hMenuPopUp, MF_BYPOSITION | MF_STRING, F_CHGMOD_EOL_LF,
						LS( F_CHGMOD_EOL_LF ), L"L" ); // 入力改行コード指定(LF)
					m_cMenuDrawer.MyAppendMenu( hMenuPopUp, MF_BYPOSITION | MF_STRING, F_CHGMOD_EOL_CR,
						LS( F_CHGMOD_EOL_CR ), L"R" ); // 入力改行コード指定(CR)
					// 拡張EOLが有効の時だけ表示
					if( GetDllShareData().m_Common.m_sEdit.m_bEnableExtEol ){
						m_cMenuDrawer.MyAppendMenu( hMenuPopUp, MF_BYPOSITION | MF_STRING, F_CHGMOD_EOL_NEL,
							LS(STR_EDITWND_MENU_NEL), L"", TRUE, -2 ); // 入力改行コード指定(NEL)
						m_cMenuDrawer.MyAppendMenu( hMenuPopUp, MF_BYPOSITION | MF_STRING, F_CHGMOD_EOL_LS,
							LS(STR_EDITWND_MENU_LS), L"", TRUE, -2 ); // 入力改行コード指定(LS)
						m_cMenuDrawer.MyAppendMenu( hMenuPopUp, MF_BYPOSITION | MF_STRING, F_CHGMOD_EOL_PS,
							LS(STR_EDITWND_MENU_PS), L"", TRUE, -2 ); // 入力改行コード指定(PS)
					}

					//	mp->ptはステータスバー内部の座標なので，スクリーン座標への変換が必要
					POINT	po = mp->pt;
					::ClientToScreen( m_cStatusBar.GetStatusHwnd(), &po );
					EFunctionCode nId = (EFunctionCode)::TrackPopupMenu(
						hMenuPopUp,
						TPM_CENTERALIGN
						| TPM_BOTTOMALIGN
						| TPM_RETURNCMD
						| TPM_LEFTBUTTON
						,
						po.x,
						po.y,
						0,
						GetHwnd(),
						nullptr
					);
					::DestroyMenu( hMenuPopUp );
					EEolType nEOLCode;
					switch(nId){
					case F_CHGMOD_EOL_CRLF:	nEOLCode = EEolType::cr_and_lf; break;
					case F_CHGMOD_EOL_CR:	nEOLCode = EEolType::carriage_return; break;
					case F_CHGMOD_EOL_LF:	nEOLCode = EEolType::line_feed; break;
					case F_CHGMOD_EOL_NEL:	nEOLCode = EEolType::next_line; break;
					case F_CHGMOD_EOL_PS:	nEOLCode = EEolType::paragraph_separator; break;
					case F_CHGMOD_EOL_LS:	nEOLCode = EEolType::line_separator; break;
					default:
						nEOLCode = EEolType::none;
					}
					if( !CEol::IsNone( nEOLCode ) ){
						GetActiveView().GetCommander().HandleCommand( F_CHGMOD_EOL, true, static_cast<LPARAM>(nEOLCode), 0, 0, 0 );
					}
				}
			}
			return 0L;
		}
		//	To Here Feb. 15, 2004 genta

		switch( pnmh->code ){
		// 2007.09.08 kobake TTN_NEEDTEXTの処理をA版とW版に分けて明示的に処理するようにしました。
		//                   ※テキストが80文字を超えそうならTOOLTIPTEXT::lpszTextを利用してください。
		// 2008.11.03 syat   矩形範囲選択開始のツールチップで80文字超えていたのでlpszTextに変更。
		case TTN_NEEDTEXT:
			{
				static WCHAR szText[256];
				memset(szText, 0, sizeof(szText));

				//ツールチップテキスト取得、設定
				LPTOOLTIPTEXT lptip = (LPTOOLTIPTEXT)pnmh;
				GetTooltipText(szText, int(std::size(szText)), lptip->hdr.idFrom);
				lptip->lpszText = szText;
			}
			break;

		case TBN_DROPDOWN:
			{
				int	nId;
				nId = CreateFileDropDownMenu( pnmh->hwndFrom );
				if( nId != 0 ) OnCommand( (WORD)0 /*メニュー*/, (WORD)nId, nullptr );
			}
			return FALSE;
		default:
			break;
		}
		return 0L;
	case WM_COMMAND:
		OnCommand( HIWORD(wParam), LOWORD(wParam), (HWND) lParam );
		return 0L;
	case WM_INITMENUPOPUP:
		InitMenu( (HMENU)wParam, (UINT)LOWORD( lParam ), (BOOL)HIWORD( lParam ) );
		return 0L;
	case WM_DROPFILES:
		/* ファイルがドロップされた */
		OnDropFiles( (HDROP) wParam );
		return 0L;
	case WM_QUERYENDSESSION:	//OSの終了
		if( OnClose( nullptr, false ) ){
			::DestroyWindow( hwnd );
			return TRUE;
		}
		else{
			return FALSE;
		}
	case WM_DESTROY:
		AbortStartupDrawTransaction();
		m_dispatchReady = false;
		AbortMarkdownPreviewResize();
		CloseMarkdownPreview();
		CloseWorkbench();
		if (m_customFrame) {
			if (HMENU menu = m_customFrame->ReplaceMenu(nullptr); menu != nullptr) {
				::DestroyMenu(menu);
			}
		}
		if( m_pShareData->m_sFlags.m_bRecordingKeyMacro ){					/* キーボードマクロの記録中 */
			if( m_pShareData->m_sFlags.m_hwndRecordingKeyMacro == GetHwnd() ){	/* キーボードマクロを記録中のウィンドウ */
				m_pShareData->m_sFlags.m_bRecordingKeyMacro = FALSE;			/* キーボードマクロの記録中 */
				m_pShareData->m_sFlags.m_hwndRecordingKeyMacro = nullptr;		/* キーボードマクロを記録中のウィンドウ */
			}
		}

		/* タイマーを削除 */
		::KillTimer( GetHwnd(), IDT_TOOLBAR );

		/* ドロップされたファイルを受け入れるのを解除 */
		::DragAcceptFiles( hwnd, FALSE );
		m_pcDropTarget->Revoke_DropTarget();	// 右ボタンドロップ用	// 2008.06.20 ryoji

		/* 編集ウィンドウリストからの削除 */
		CAppNodeGroupHandle(GetHwnd()).DeleteEditWndList( GetHwnd() );

		if( m_pShareData->m_sHandles.m_hwndDebug == GetHwnd() ){
			m_pShareData->m_sHandles.m_hwndDebug = nullptr;
		}
		m_hWnd = nullptr;

		/* 編集ウィンドウオブジェクトからのオブジェクト削除要求 */
		::PostMessageAny( m_pShareData->m_sHandles.m_hwndTray, MYWM_DELETE_ME, 0, 0 );

		/* Windows にスレッドの終了を要求します */
		::PostQuitMessage( 0 );

		return 0L;

	case WM_THEMECHANGED:
		// 2006.06.17 ryoji
		// ビジュアルスタイル／クラシックスタイルが切り替わったらツールバーを再作成する
		// （ビジュアルスタイル: Rebar 有り、クラシックスタイル: Rebar 無し）
		if( m_cToolbar.GetToolbarHwnd() ){
			if( IsVisualStyle() == (nullptr == m_cToolbar.GetRebarHwnd()) ){
				m_cToolbar.DestroyToolBar();
				LayoutToolBar();
				EndLayoutBars();
			}
		}
		if (m_customFrame) {
			m_customFrame->SetThemeMode(
				m_pShareData->m_Common.m_sWindow.m_bDarkMode
					? theme::ThemeMode::Dark
					: theme::ThemeMode::Light);
		}
		(void)ApplyWorkbenchTheme();
		return 0L;

	case WM_SETTINGCHANGE:
		(void)ApplyWorkbenchTheme();
		return ::DefWindowProc(hwnd, uMsg, wParam, lParam);

	case MYWM_UIPI_CHECK:
		/* エディタ－トレイ間でのUI特権分離の確認メッセージ */	// 2007.06.07 ryoji
		return LRESULT(lParam);

	case MYWM_CLOSE:
		/* エディタへの終了要求 */
		// Closing the last window of a merged-tab group with the retain-empty
		// option used to launch a replacement editor process and destroy this
		// one.  VS Code's workbench.action.closeActiveEditor keeps the window
		// and process alive in the empty-editor state, so when the working-copy
		// coordinator owns the close it runs in place instead; the coordinator's
		// CommitClose fires PP_DOCUMENT_CLOSE and reinitializes the empty
		// document, so no plugin event or window teardown belongs here.
		if( PM_CLOSE_EXIT != (PM_CLOSE_EXIT & wParam) &&
			m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd &&
			!m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin &&
			m_pShareData->m_Common.m_sTabBar.m_bTab_RetainEmptyWin &&
			m_workingCopyCoordinator != nullptr &&
			!m_workspaceReplacementClosePreflightAccepted ){
			EditNode* pEditNode = CAppNodeManager::getInstance()->GetEditNode( GetHwnd() );
			if( pEditNode && 1 == CAppNodeGroupHandle(pEditNode->GetGroup()).GetEditorWindowsNum() ){
				if( !HasActiveEditorInput() ){
					return TRUE;	// 既に空。閉じる対象がないのでウィンドウを維持する
				}
				return ExecuteActiveWorkingCopyCommand(
					workbench::editor::command_ids::CloseActiveEditor,
					PM_CLOSE_GREPNOCONFIRM == (PM_CLOSE_GREPNOCONFIRM & wParam),
					false ) ? TRUE : FALSE;
			}
		}
		if( FALSE != ( nRet = OnClose( (HWND)lParam,
				PM_CLOSE_GREPNOCONFIRM == (PM_CLOSE_GREPNOCONFIRM & wParam) )) ){	// Jan. 23, 2002 genta 警告抑制
			//プラグイン：DocumentCloseイベント実行
			if (HasActiveEditorInput()) {
				CJackManager::getInstance()->InvokePlugins(PP_DOCUMENT_CLOSE, &GetActiveView());
			}

			//プラグイン：EditorEndイベント実行
			CJackManager::getInstance()->InvokePlugins( PP_EDITOR_END, &GetActiveView() );

			// タブまとめ表示では閉じる動作はオプション指定に従う	// 2006.02.13 ryoji
			if (PM_CLOSE_EXIT != (PM_CLOSE_EXIT & wParam) &&	// 全終了要求でない場合
				// タブまとめ表示で(無題)を残す指定の場合、残ウィンドウが１個なら新規エディタを起動して終了する
				m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd &&
				!m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin &&
				m_pShareData->m_Common.m_sTabBar.m_bTab_RetainEmptyWin
					){
					// 自グループ内の残ウィンドウ数を調べる	// 2007.06.20 ryoji
					int nGroup = CAppNodeManager::getInstance()->GetEditNode( GetHwnd() )->GetGroup();
					if( 1 == CAppNodeGroupHandle(nGroup).GetEditorWindowsNum() ){
						EditNode* pEditNode = CAppNodeManager::getInstance()->GetEditNode( GetHwnd() );
						if( pEditNode )
							pEditNode->m_bClosing = TRUE;	// 自分はタブ表示してもらわなくていい
						SLoadInfo sLoadInfo;
						sLoadInfo.cFilePath = L"";
						sLoadInfo.eCharCode = CODE_NONE;
						sLoadInfo.bViewMode = false;
						CControlTray::OpenNewEditor(
							G_AppInstance(),
							GetHwnd(),
							sLoadInfo,
							nullptr,
							true
						);
					}
			}
			::DestroyWindow( hwnd );
		}
		return nRet;
	case MYWM_ALLOWACTIVATE:
		::AllowSetForegroundWindow((DWORD)wParam);
		return 0L;

	case MYWM_GETFILEINFO:
		/* トレイからエディタへの編集ファイル名要求通知 */
		pfi = (EditInfo*)&m_pShareData->m_sWorkBuffer.m_EditInfo_MYWM_GETFILEINFO;

		/* 編集ファイル情報を格納 */
		GetDocument()->GetEditInfo( pfi );
		return 0L;
	case MYWM_CHANGESETTING:
		/* 設定変更の通知 */
		switch( (e_PM_CHANGESETTING_SELECT)lParam ){
		case PM_CHANGESETTING_ALL:
			/* Apply the resolved workbench theme instead of restoring the legacy
			 * dark-mode preference over an explicit workbench.colorTheme. */
			{
				const bool nativeDarkBefore = IsDarkModeActive();
				(void)ApplyWorkbenchTheme();
				if( nativeDarkBefore != IsDarkModeActive() ){
					// エディットビューのWS_EX_STATICEDGEを切り替える
					for( int v = 0; v < GetAllViewCount(); v++ ){
						HWND hwndView = GetView(v).GetHwnd();
						DarkMode::setWindowExStyle(hwndView, !IsDarkModeActive(), WS_EX_STATICEDGE);
						::SetWindowPos(hwndView, nullptr, 0, 0, 0, 0,
							SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
					}

					// ステータスバーを再作成する（サブクラスを更新するため）
					m_cStatusBar.DestroyStatusBar();
				}
			}

			/* 言語を選択する */
			CSelectLang::ChangeLang( GetDllShareData().m_Common.m_sWindow.m_szLanguageDll );
			CShareData::getInstance()->RefreshString();
			RefreshLocalizedWorkbenchText();

			// 2015.08.20 プリントプレビューのとき設定を延期する(戻るとき適用)
			if (!m_pPrintPreview) {
				// メインメニュー	2010/5/16 Uchi
				LayoutMainMenu();
			}

			// Oct 10, 2000 ao
			/* 設定変更時、ツールバーを再作成するようにする（バーの内容変更も反映） */
			m_cToolbar.DestroyToolBar();
			LayoutToolBar();
			// Oct 10, 2000 ao ここまで

			// 2008.10.05 nasukoji	非アクティブなウィンドウのツールバーを更新する
			// アクティブなウィンドウはタイマにより更新されるが、それ以外のウィンドウは
			// タイマを停止させており設定変更すると全部有効となってしまうため、ここで
			// ツールバーを更新する
			if( !m_bIsActiveApp )
				m_cToolbar.UpdateToolbar();

			// ファンクションキーを再作成する（バーの内容、位置、グループボタン数の変更も反映）	// 2006.12.19 ryoji
			m_cFuncKeyWnd.Close();
			LayoutFuncKey();

			// タブバーの表示／非表示切り替え	// 2006.12.19 ryoji
			LayoutTabBar();

			// ステータスバーの表示／非表示切り替え	// 2006.12.19 ryoji
			LayoutStatusBar();

			// 水平スクロールバーの表示／非表示切り替え	// 2006.12.19 ryoji
			{
				int i;
				bool b1;
				bool b2;
				b1 = (m_pShareData->m_Common.m_sWindow.m_bScrollBarHorz == FALSE);
				for( i = 0; i < GetAllViewCount(); i++ )
				{
					b2 = (GetView(i).m_hwndHScrollBar == nullptr);
					if( b1 != b2 )		/* 水平スクロールバーを使う */
					{
						GetView(i).DestroyScrollBar();
						GetView(i).CreateScrollBar();
					}
				}
			}

			LayoutMiniMap();

			// バー変更で画面が乱れないように	// 2006.12.19 ryoji
			EndLayoutBars();
			ApplyWorkbenchSettingsFromSharedData();

			// アクセラレータテーブルを再作成する
			// ウィンドウ毎に作成したアクセラレータテーブルを破棄する
			DeleteAccelTbl();
			// ウィンドウ毎にアクセラレータテーブルを作成する
			CreateAccelTbl();

			if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd )
			{
				// タブ表示のままグループ化する／しないが変更されていたらタブを更新する必要がある
				m_cTabWnd.Refresh( FALSE );
			}
			if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd && !m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin )
			{
				if( CAppNodeManager::getInstance()->GetEditNode( GetHwnd() )->IsTopInGroup() )
				{
					if( !::IsWindowVisible( GetHwnd() ) )
					{
						// ::ShowWindow( GetHwnd(), SW_SHOWNA ) だと非表示から表示に切り替わるときに Z-order がおかしくなることがあるので ::SetWindowPos を使う
						::SetWindowPos( GetHwnd(), nullptr,0,0,0,0,
										SWP_SHOWWINDOW | SWP_NOACTIVATE
										| SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER );

						// このウィンドウの WS_EX_TOPMOST 状態を全ウィンドウに反映する	// 2007.05.18 ryoji
						WindowTopMost( ((DWORD)::GetWindowLongPtr( GetHwnd(), GWL_EXSTYLE ) & WS_EX_TOPMOST)? 1: 2 );
					}
				}
				else
				{
					if( ::IsWindowVisible( GetHwnd() ) )
					{
						::ShowWindow( GetHwnd(), SW_HIDE );
					}
				}
			}
			else
			{
				if( !::IsWindowVisible( GetHwnd() ) )
				{
					// ::ShowWindow( GetHwnd(), SW_SHOWNA ) だと非表示から表示に切り替わるときに Z-order がおかしくなることがあるので ::SetWindowPos を使う
					::SetWindowPos( GetHwnd(), nullptr,0,0,0,0,
									SWP_SHOWWINDOW | SWP_NOACTIVATE
									| SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER );
				}
			}

			//	Aug, 21, 2000 genta
			GetDocument()->m_cAutoSaveAgent.ReloadAutoSaveParam();

			GetDocument()->OnChangeSetting();	// ビューに設定変更を反映させる
			GetDocument()->m_cDocType.SetDocumentIcon();	// Sep. 10, 2002 genta 文書アイコンの再設定

			break;
		case PM_CHANGESETTING_WORKBENCH:
			ApplyWorkbenchSettingsFromSharedData();
			break;
		case PM_CHANGESETTING_FONT:
			GetDocument()->OnChangeSetting( true );	// フォントで文字幅が変わるので、レイアウト再構築
			delete [] m_posSaveAry;
			m_posSaveAry = nullptr;
			break;
		case PM_CHANGESETTING_FONTSIZE:
			if( (-1 == wParam && CWM_CACHE_SHARE == GetLogfontCacheMode())
					|| GetDocument()->m_cDocType.GetDocumentType().GetIndex() == wParam ){
				// 文字幅で幅も変わるので再構築する
				// 変更中にさらに変更されると困るのでBlockingHookは無効
				GetDocument()->OnChangeSetting( true, false );
			}
			delete [] m_posSaveAry;
			m_posSaveAry = nullptr;
			break;
		case PM_CHANGESETTING_TYPE:
			cTypeNew = CDocTypeManager().GetDocumentTypeOfPath(GetDocument()->m_cDocFile.GetFilePath());
			if (GetDocument()->m_cDocType.GetDocumentType().GetIndex() == wParam
				|| cTypeNew.GetIndex() == wParam){
				GetDocument()->OnChangeSetting();

				// アウトライン解析画面処理
				bool bAnalyzed = FALSE;
#if 0
				if( /* 必要なら変更条件をここに記述する（将来用） */ )
				{
					// アウトライン解析画面の位置を現在の設定に合わせる
					bAnalyzed = m_cDlgFuncList.ChangeLayout( OUTLINE_LAYOUT_BACKGROUND );	// 外部からの変更通知と同等の扱い
				}
#endif
				if( m_cDlgFuncList.GetHwnd() && !bAnalyzed ){	// アウトラインを開いていれば再解析
					// SHOW_NORMAL: 解析方法が変化していれば再解析される。そうでなければ描画更新（変更されたカラーの適用）のみ。
					EFunctionCode nFuncCode = m_cDlgFuncList.GetFuncCodeRedraw(m_cDlgFuncList.m_nOutlineType);
					GetActiveView().GetCommander().HandleCommand( nFuncCode, true, SHOW_NORMAL, 0, 0, 0 );
				}
				if( MyGetAncestor( ::GetForegroundWindow(), GA_ROOTOWNER2 ) == GetHwnd() )
					::SetFocus( GetActiveView().GetHwnd() );	// フォーカスを戻す
			}
			break;
		case PM_CHANGESETTING_TYPE2:
			cTypeNew = CDocTypeManager().GetDocumentTypeOfPath(GetDocument()->m_cDocFile.GetFilePath());
			if (GetDocument()->m_cDocType.GetDocumentType().GetIndex() == wParam
				|| cTypeNew.GetIndex() == wParam){
				// indexのみ更新
				GetDocument()->m_cDocType.SetDocumentTypeIdx();
				// タイプが変更になった場合は適用する
				if (GetDocument()->m_cDocType.GetDocumentType().GetIndex() != wParam) {
					::SendMessage(m_hWnd, MYWM_CHANGESETTING, wParam, PM_CHANGESETTING_TYPE);
				}
			}
			break;
		case PM_PRINTSETTING:
			{
				if( m_pPrintPreview ){
					m_pPrintPreview->OnChangeSetting();
				}
			}
			break;
		default:
			break;
		}
		return 0L;
	case MYWM_SAVEEDITSTATE:
		{
			if( m_pPrintPreview ){
				// 一時的に設定を戻す
				SelectCharWidthCache( CWM_FONT_EDIT, CWM_CACHE_NEUTRAL );
			}
			// フォント変更前の座標の保存
			m_posSaveAry = SavePhysPosOfAllView();
			if( m_pPrintPreview ){
				// 設定を戻す
				SelectCharWidthCache( CWM_FONT_PRINT, CWM_CACHE_LOCAL );
			}
		}
		return 0L;
	case MYWM_SETACTIVEPANE:
		if( -1 == (int)wParam ){
			if( 0 == lParam ){
				nPane = m_cSplitterWnd.GetFirstPane();
			}else{
				nPane = m_cSplitterWnd.GetLastPane();
			}
			this->SetActivePane( nPane );
		}
		return 0L;

	case MYWM_SETCARETPOS:	/* カーソル位置変更通知 */
		{
			//	2006.07.09 genta LPARAMに新たな意味を追加
			//	bit 0 (MASK 1): (bit 1==0のとき) 0/選択クリア, 1/選択開始・変更
			//	bit 1 (MASK 2): 0: bit 0の設定に従う．1:現在の選択ロックs状態を継続
			//	既存の実装では どちらも0なので強制解除と解釈される．
			//	呼び出し時はe_PM_SETCARETPOS_SELECTSTATEの値を使うこと．
			bool bSelect = (0!= (lParam & 1));
			if( lParam & 2 ){
				// 現在の状態をKEEP
				bSelect = GetActiveView().GetSelectionInfo().IsSelectionLocked();
			}

			//	2006.07.09 genta 強制解除しない
			/*
			カーソル位置変換
			 物理位置(行頭からのバイト数、折り返し無し行位置)
			→
			 レイアウト位置(行頭からの表示桁位置、折り返しあり行位置)
			*/
			CLogicPoint* ppoCaret = &(m_pShareData->m_sWorkBuffer.m_LogicPoint);
			CLayoutPoint ptCaretPos;
			GetDocument()->m_cLayoutMgr.LogicToLayout(
				*ppoCaret,
				&ptCaretPos
			);
			// 改行の真ん中にカーソルが来ないように	// 2007.08.22 ryoji
			// Note. もとが改行単位の桁位置なのでレイアウト折り返しの桁位置を超えることはない。
			//       選択指定(bSelect==TRUE)の場合にはどうするのが妥当かよくわからないが、
			//       2007.08.22現在ではアウトライン解析ダイアログから桁位置0で呼び出される
			//       パターンしかないので実用上特に問題は無い。
			if( !bSelect ){
				const CDocLine *pTmpDocLine = GetDocument()->m_cDocLineMgr.GetLine( ppoCaret->GetY2() );
				if( pTmpDocLine ){
					if( pTmpDocLine->GetLengthWithoutEOL() < ppoCaret->x ) ptCaretPos.x--;
				}
			}
			//	2006.07.09 genta 選択範囲を考慮して移動
			//	MoveCursorの位置調整機能があるので，最終行以降への
			//	移動指示の調整もMoveCursorにまかせる
			GetActiveView().MoveCursorSelecting( ptCaretPos, bSelect, _CARETMARGINRATE / 3 );
		}
		return 0L;

	case MYWM_GETCARETPOS:	/* カーソル位置取得要求 */
		/*
		カーソル位置変換
		 レイアウト位置(行頭からの表示桁位置、折り返しあり行位置)
		→
		物理位置(行頭からのバイト数、折り返し無し行位置)
		*/
		{
			CLogicPoint* ppoCaret = &(m_pShareData->m_sWorkBuffer.m_LogicPoint);
			GetDocument()->m_cLayoutMgr.LayoutToLogic(
				GetActiveView().GetCaret().GetCaretLayoutPos(),
				ppoCaret
			);
		}
		return 0L;

	case MYWM_GETLINEDATA:	/* 行(改行単位)データの要求 */
	{
		// 共有データ：自分Write→相手Read
		// return 0以上：行データあり。wParamオフセットを除いた行データ長。0はEOFかOffsetがちょうどバッファ長だった
		//       -1以下：エラー
		CLogicInt	nLineNum = CLogicInt(wParam);
		CLogicInt	nLineOffset = CLogicInt(lParam);
		if( nLineNum < 0 || GetDocument()->m_cDocLineMgr.GetLineCount() < nLineNum ){
			return -2; // 行番号不正。LineCount == nLineNum はEOF行として下で処理
		}
		CLogicInt	nLineLen = CLogicInt(0);
		const wchar_t*	pLine = GetDocument()->m_cDocLineMgr.GetLine(nLineNum)->GetDocLineStrWithEOL( &nLineLen );
		if( nLineOffset < 0 || nLineLen < nLineOffset ){
			return -3; // オフセット位置不正
		}
		if( nLineNum == GetDocument()->m_cDocLineMgr.GetLineCount() ){
			return 0; // EOF正常終了
		}
 		if( nullptr == pLine ){
			return -4; // 不明なエラー
		}
		if( nLineLen == nLineOffset ){
 			return 0;
 		}
		pLine = GetDocument()->m_cDocLineMgr.GetLine(CLogicInt(wParam))->GetDocLineStrWithEOL( &nLineLen );
		pLine += nLineOffset;
		nLineLen -= nLineOffset;
		size_t nEnd = t_min<size_t>(nLineLen, m_pShareData->m_sWorkBuffer.GetWorkBufferCount<EDIT_CHAR>());
		wmemcpy( m_pShareData->m_sWorkBuffer.GetWorkBuffer<EDIT_CHAR>(), pLine, nEnd );
		return nLineLen;
	}
	case MYWM_GETLINECOUNT:
	{
		return GetDocument()->m_cDocLineMgr.GetLineCount();
	}

	// 2010.05.11 Moca MYWM_ADDSTRINGLEN_Wを追加 NULセーフ
	case MYWM_ADDSTRINGLEN_W:
		{
			EDIT_CHAR* pWork = m_pShareData->m_sWorkBuffer.GetWorkBuffer<EDIT_CHAR>();
			size_t addSize = t_min((size_t)wParam, m_pShareData->m_sWorkBuffer.GetWorkBufferCount<EDIT_CHAR>() );
			GetActiveView().GetCommander().HandleCommand( F_ADDTAIL_W, true, (LPARAM)pWork, (LPARAM)addSize, 0, 0 );
			GetActiveView().GetCommander().HandleCommand( F_GOFILEEND, true, 0, 0, 0, 0 );
		}
		return 0L;

	//タブウインドウ	//@@@ 2003.05.31 MIK
	case MYWM_TAB_WINDOW_NOTIFY:
		if (m_cTabWnd.TabWindowNotify( wParam, lParam ) == ETabWindowNotifyImpact::WorkbenchLayout) {
			RECT		rc;
			::GetClientRect( GetHwnd(), &rc );
			OnSize2( m_nWinSizeType, MAKELONG( rc.right - rc.left, rc.bottom - rc.top ), false );
		}
		GetActiveView().SetIMECompFormPos();
		return 0L;

	//アウトライン	// 2010.06.06 ryoji
	case MYWM_OUTLINE_NOTIFY:
		m_cDlgFuncList.OnOutlineNotify( wParam, lParam );
		return 0L;

	//バーの表示・非表示	//@@@ 2003.06.10 MIK
	case MYWM_BAR_CHANGE_NOTIFY:
		if( GetHwnd() != (HWND)lParam )
		{
			switch( wParam )
			{
			case MYBCN_TOOLBAR:
				LayoutToolBar();	// 2006.12.19 ryoji
				break;
			case MYBCN_FUNCKEY:
				LayoutFuncKey();	// 2006.12.19 ryoji
				break;
			case MYBCN_TAB:
				LayoutTabBar();		// 2006.12.19 ryoji
				if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd
					&& !m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin )
				{
					::ShowWindow(GetHwnd(), SW_HIDE);
				}
				else
				{
					// ::ShowWindow( hwnd, SW_SHOWNA ) だと非表示から表示に切り替わるときに Z-order がおかしくなることがあるので ::SetWindowPos を使う
					::SetWindowPos( hwnd, nullptr,0,0,0,0,
									SWP_SHOWWINDOW | SWP_NOACTIVATE
									| SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER );
				}
				break;
			case MYBCN_STATUSBAR:
				LayoutStatusBar();		// 2006.12.19 ryoji
				break;
			case MYBCN_MINIMAP:
				LayoutMiniMap();
				break;
			default:
				break;
			}
			EndLayoutBars();	// 2006.12.19 ryoji
		}
		DarkMode::setChildCtrlsSubclassAndTheme(hwnd);
		m_cStatusBar.InstallPaletteSubclass();
		return 0L;

	//by 鬼 (2) MYWM_CHECKSYSMENUDBLCLKは不要に, WM_LBUTTONDBLCLK追加
	case WM_NCLBUTTONDOWN:
		return OnNcLButtonDown(wParam, lParam);

	case WM_NCLBUTTONUP:
		return OnNcLButtonUp(wParam, lParam);

	case WM_LBUTTONDBLCLK:
		return OnLButtonDblClk(wParam, lParam);

#if 0
	case WM_IME_NOTIFY:	// Nov. 26, 2006 genta
		if( wParam == IMN_SETCONVERSIONMODE || wParam == IMN_SETOPENSTATUS){
			GetActiveView().GetCaret().ShowEditCaret();
		}
		return DefWindowProc( hwnd, uMsg, wParam, lParam );
#endif

	case WM_NCPAINT:
		DefWindowProc( hwnd, uMsg, wParam, lParam );
		if( nullptr == m_cStatusBar.GetStatusHwnd() ){
			PrintMenubarMessage( nullptr );
		}
		return 0;

	case WM_NCACTIVATE:
		// 編集ウィンドウ切替中（タブまとめ時）はタイトルバーのアクティブ／非アクティブ状態をできるだけ変更しないように（１）	// 2007.04.03 ryoji
		// 前面にいるのが編集ウィンドウならアクティブ状態を保持する
		if( m_pShareData->m_sFlags.m_bEditWndChanging && IsSakuraMainWindow(::GetForegroundWindow()) ){
			wParam = TRUE;	// アクティブ
		}
		lRes = DefWindowProc( hwnd, uMsg, wParam, lParam );
		if( nullptr == m_cStatusBar.GetStatusHwnd() ){
			PrintMenubarMessage( nullptr );
		}
		return lRes;

	case WM_SETTEXT:
		// 編集ウィンドウ切替中（タブまとめ時）はタイトルバーのアクティブ／非アクティブ状態をできるだけ変更しないように（２）	// 2007.04.03 ryoji
		// タイマーを使用してタイトルの変更を遅延する
		if( m_pShareData->m_sFlags.m_bEditWndChanging ){
			delete[] m_pszLastCaption;
			m_pszLastCaption = new WCHAR[ ::wcslen((LPCWSTR)lParam) + 1 ];
			::wcscpy( m_pszLastCaption, (LPCWSTR)lParam );	// 変更後のタイトルを記憶しておく
			::SetTimer( GetHwnd(), IDT_CAPTION, 50, nullptr );
			return 0L;
		}
		::KillTimer( GetHwnd(), IDT_CAPTION );	// タイマーが残っていたら削除する（遅延タイトルを破棄）
		return DefWindowProc( hwnd, uMsg, wParam, lParam );

	case WM_TIMER:
		if( !OnTimer(wParam, lParam) )
			return 0L;
		return DefWindowProc( hwnd, uMsg, wParam, lParam );

	default:
#if 0
// << 20020331 aroka 再変換対応 for 95/NT
		if( uMsg == m_uMSIMEReconvertMsg || uMsg == m_uATOKReconvertMsg){
			return Views_DispatchEvent( hwnd, uMsg, wParam, lParam );
		}
// >> by aroka
#endif
		return DefWindowProc( hwnd, uMsg, wParam, lParam );
	}
}

sakura::editor::EditorFrameEffect CEditWnd::HandleEditorFrameEvent(
	const sakura::editor::EditorFrameEvent& event)
{
	using sakura::editor::EditorFrameEffect;
	using sakura::editor::EditorFrameEffectKind;
	using sakura::editor::EditorFrameEventKind;

	switch (event.Kind()) {
	case EditorFrameEventKind::Activated:
	case EditorFrameEventKind::Deactivated:
		m_bIsActiveApp = event.Kind() == EditorFrameEventKind::Activated;
		if (m_bIsActiveApp) {
			CAppNodeGroupHandle(0).AddEditWndList(GetHwnd());
			ClearMouseState();
		}
		UpdateCaption();
		m_cFuncKeyWnd.Timer_ONOFF(m_bIsActiveApp);
		Timer_ONOFF(m_bIsActiveApp);
		return { EditorFrameEffectKind::Handled, 0 };

	case EditorFrameEventKind::FocusGained:
		m_nTimerCount = 9;
		if (!m_pPrintPreview) {
			if (HasActiveEditorInput()) {
				::SetFocus(GetActiveView().GetHwnd());
			} else if (m_emptyEditorSurface) {
				m_emptyEditorSurface->Focus();
			}
		}
		if (m_pPrintPreview) m_pPrintPreview->SetFocusToPrintPreviewBar();
		return { EditorFrameEffectKind::Handled, 0 };

	case EditorFrameEventKind::FocusLost:
		return { EditorFrameEffectKind::ForwardToDefault, 0 };

	case EditorFrameEventKind::Enabled:
		m_pcDropTarget->Register_DropTarget(m_hWnd);
		return { EditorFrameEffectKind::Handled, 0 };

	case EditorFrameEventKind::Disabled:
		m_pcDropTarget->Revoke_DropTarget();
		return { EditorFrameEffectKind::Handled, 0 };

	case EditorFrameEventKind::Resized: {
		if (event.Detail() == SIZE_MINIMIZED) UpdateCaption();
		const auto packedSize = MAKELPARAM(event.Size().Width(), event.Size().Height());
		const auto result = OnSize(event.Detail(), packedSize);
		UpdateFrameRuntimeCadence();
		if (m_commandPaletteOverlay) m_commandPaletteOverlay->Layout();
		return { EditorFrameEffectKind::Handled, result };
	}

	case EditorFrameEventKind::Moved:
		if (WINSIZEMODE_SAVE == m_pShareData->m_Common.m_sWindow.m_eSaveWindowPos
			&& !::IsZoomed(GetHwnd()) && !::IsIconic(GetHwnd())) {
			WINDOWPLACEMENT placement{};
			placement.length = sizeof(placement);
			::GetWindowPlacement(GetHwnd(), &placement);
			RECT windowRect = placement.rcNormalPosition;
			RECT workRect{};
			RECT monitorRect{};
			GetMonitorWorkRect(GetHwnd(), &workRect, &monitorRect);
			::OffsetRect(&windowRect, workRect.left - monitorRect.left, workRect.top - monitorRect.top);
			m_pShareData->m_Common.m_sWindow.m_nWinPosX = windowRect.left;
			m_pShareData->m_Common.m_sWindow.m_nWinPosY = windowRect.top;
		}
		UpdateFrameRuntimeCadence();
		return { EditorFrameEffectKind::ForwardToDefault, 0 };

	case EditorFrameEventKind::CloseRequested:
		if (OnClose(nullptr, false)) {
			::DestroyWindow(GetHwnd());
			return { EditorFrameEffectKind::CloseAccepted, 0 };
		}
		return { EditorFrameEffectKind::CloseRefused, 0 };

	case EditorFrameEventKind::DpiChanged:
		UpdateFrameRuntimeCadence();
		return { EditorFrameEffectKind::ForwardToDefault, 0 };
	}
	return EditorFrameEffect{};
}

/*! 終了時の処理

	@param hWndFrom [in] 終了要求の Wimdow Handle	//2013/4/9 Uchi

	@retval TRUE: 終了して良い / FALSE: 終了しない
*/
int	CEditWnd::OnClose(HWND hWndActive, bool bGrepNoConfirm )
{
	/* ファイルを閉じるときのMRU登録 & 保存確認 & 保存実行 */
	int nRet = TRUE;
	const bool workspacePreflightAccepted = std::exchange(m_workspaceReplacementClosePreflightAccepted, false);
	if (workspacePreflightAccepted && HasActiveEditorInput()) {
		// PrepareWorkspaceReplacement already ran CDocFileOperation's real
		// save/discard/cancel flow.  Commit only the non-throwing legacy teardown;
		// invoking the coordinator again would prompt a discarded dirty buffer a
		// second time after the successor is already ready.
		GetDocument()->m_cDocFileOperation.CommitFileClose(false);
	}
	else if (HasActiveEditorInput()) {
		nRet = m_workingCopyCoordinator
			? (ExecuteActiveWorkingCopyCommand(
				workbench::editor::command_ids::CloseActiveEditor, bGrepNoConfirm, true) ? TRUE : FALSE)
			: GetDocument()->OnFileClose(bGrepNoConfirm);
	}
	if( !nRet ) return nRet;

	// パラメータでハンドルを貰う様にしたので検索を削除	2013/4/9 Uchi
	if( hWndActive ){
		// アクティブ化制御ウィンドウをアクティブ化する
		if( IsSakuraMainWindow(hWndActive) ){
			ActivateFrameWindow(hWndActive);	// エディタ
		}else{
			::SetForegroundWindow(hWndActive);	// タスクトレイ
		}
	}

#if 0
	// 2005.09.01 ryoji タブまとめ表示の場合は次のウィンドウを前面に（終了時のウィンドウちらつきを抑制）
	if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd
		&& !m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin )
	{
		int i, j;
		EditNode*	p = NULL;
		int nCount = CAppNodeManager::getInstance()->GetOpenedWindowArr( &p, FALSE );
		if( nCount > 1 )
		{
			for( i = 0; i < nCount; i++ )
			{
				if( p[ i ].GetHwnd() == GetHwnd() )
					break;
			}
			if( i < nCount )
			{
				for( j = i + 1; j < nCount; j++ )
				{
					if( p[ j ].m_nGroup == p[ i ].m_nGroup )
						break;
				}
				if( j >= nCount )
				{
					for( j = 0; j < i; j++ )
					{
						if( p[ j ].m_nGroup == p[ i ].m_nGroup )
							break;
					}
				}
				if( j != i )
				{
					HWND hwnd = p[ j ].GetHwnd();
					{
						// 2006.01.28 ryoji
						// タブまとめ表示でこの画面が非表示から表示に変わってすぐ閉じる場合(タブの中クリック時等)、
						// 以前のウィンドウが消えるよりも先に一気にここまで処理が進んでしまうと
						// あとで画面がちらつくので、以前のウィンドウが消えるのをちょっとだけ待つ
						int iWait = 0;
						while( ::IsWindowVisible( hwnd ) && iWait++ < 20 )
							::Sleep(1);
					}
					if( !::IsWindowVisible( hwnd ) )
					{
						ActivateFrameWindow( hwnd );
					}
				}
			}
		}
		if( p ) delete []p;
	}
#endif	// 0

	return nRet;
}

/*! WM_COMMAND処理
	@date 2000.11.15 JEPRO //ショートカットキーがうまく働かないので殺してあった下の2行(F_HELP_CONTENTS,F_HELP_SEARCH)を修正・復活
	@date 2013.05.09 novice 重複するメッセージ処理削除
*/
void CEditWnd::OnCommand( WORD wNotifyCode, WORD wID , HWND hwndCtl )
{
	// 検索ボックスからの WM_COMMAND はすべてコンボボックス通知
	// ##### 検索ボックス処理はツールバー側の WindowProc に集約するほうがスマートかも
	if( m_cToolbar.GetSearchHwnd() && hwndCtl == m_cToolbar.GetSearchHwnd() ){
		switch( wNotifyCode ){
		case CBN_SETFOCUS:
			m_nCurrentFocus = F_SEARCH_BOX;
			break;
		case CBN_KILLFOCUS:
		{
			m_nCurrentFocus = 0;
			//フォーカスがはずれたときに検索キーにしてしまう。
			//検索キーワードを取得
			std::wstring	strText;
			if( m_cToolbar.GetSearchKey(strText) )	//キー文字列がある
			{
				//検索キーを登録
				if( strText.length() < _MAX_PATH ){
					CSearchKeywordManager().AddToSearchKeyArr( strText.c_str() );
				}
				if (HasActiveEditorInput()) {
					GetActiveView().m_strCurSearchKey = std::move(strText);
					GetActiveView().m_bCurSearchUpdate = true;
					GetActiveView().ChangeCurRegexp();
				}
			}
			break;
		}
		default:
			break;
		}
		return;	// CBN_SELCHANGE(1) がアクセラレータと誤認されないようにここで抜ける（rev1886 の問題の抜本対策）
	}

	switch( wNotifyCode ){
	/* メニューからのメッセージ */
	case 0:
	case CMD_FROM_MOUSE: // 2006.05.19 genta マウスから呼びだされた場合
		//ウィンドウ切り替え
		if( wID - IDM_SELWINDOW >= 0 && wID - IDM_SELWINDOW < m_pShareData->m_sNodes.m_nEditArrNum ){
			ActivateFrameWindow( m_pShareData->m_sNodes.m_pEditArr[wID - IDM_SELWINDOW].GetHwnd() );
		}
		else if (TryExecuteRecentlyOpenedWorkspaceMenuCommand(static_cast<std::int32_t>(wID))) {
			// The 13000 range is a typed, immutable snapshot and cannot fall into
			// either legacy MRU handler below.
		}
		//最近使ったファイル
		else if( wID - IDM_SELMRU >= 0 && wID - IDM_SELMRU < 999){
			/* 指定ファイルが開かれているか調べる */
			const CMRUFile cMRU;
			EditInfo checkEditInfo;
			cMRU.GetEditInfo(wID - IDM_SELMRU, &checkEditInfo);
			SLoadInfo sLoadInfo(checkEditInfo.m_szPath, checkEditInfo.m_nCharCode, false);
			GetDocument()->m_cDocFileOperation.FileLoad( &sLoadInfo );	//	Oct.  9, 2004 genta 共通関数化
		}
		//最近使ったフォルダー
		else if( wID - IDM_SELOPENFOLDER >= 0 && wID - IDM_SELOPENFOLDER < 999){
			//フォルダー取得
			const CMRUFolder cMRUFolder;
			LPCWSTR pszFolderPath = cMRUFolder.GetPath( wID - IDM_SELOPENFOLDER );

			//Stonee, 2001/12/21 UNCであれば接続を試みる
			NetConnect( pszFolderPath );

			//「ファイルを開く」ダイアログ
			SLoadInfo sLoadInfo(L"", CODE_AUTODETECT, false);
			CDocFileOperation& cDocOp = GetDocument()->m_cDocFileOperation;
			std::vector<std::wstring> files;
			if( cDocOp.OpenFileDialog(GetHwnd(), pszFolderPath, &sLoadInfo, files) ){
				sLoadInfo.cFilePath = files[0].c_str();
				//開く
				cDocOp.FileLoad( &sLoadInfo );

				// 新たな編集ウィンドウを起動
				size_t nSize = files.size();
				for( size_t f = 1; f < nSize; f++ ){
					sLoadInfo.cFilePath = files[f].c_str();
					CControlTray::OpenNewEditor( G_AppInstance(), GetHwnd(), sLoadInfo, nullptr, true );
				}
			}
		}
		//その他コマンド
		else{
			//ビューにフォーカスを移動しておく
			if( wID != F_SEARCH_BOX && m_nCurrentFocus == F_SEARCH_BOX ) {
				if (HasActiveEditorInput()) ::SetFocus(GetActiveView().GetHwnd());
				else if (m_emptyEditorSurface) m_emptyEditorSurface->Focus();
			}

			// コマンドコードによる処理振り分け
			//	May 19, 2006 genta 上位ビットを渡す
			//	Jul. 7, 2007 genta 上位ビットを定数に
			DispatchEditorFunction(static_cast<EFunctionCode>(wID | 0));
		}
		break;
	/* アクセラレータからのメッセージ */
	case 1:
		{
			//ビューにフォーカスを移動しておく
			if( wID != F_SEARCH_BOX && m_nCurrentFocus == F_SEARCH_BOX ) {
				if (HasActiveEditorInput()) ::SetFocus(GetActiveView().GetHwnd());
				else if (m_emptyEditorSurface) m_emptyEditorSurface->Focus();
			}

			EFunctionCode nFuncCode = CKeyBind::GetFuncCode(
				wID,
				m_pShareData->m_Common.m_sKeyBind.m_nKeyNameArrNum,
				m_pShareData->m_Common.m_sKeyBind.m_pKeyNameArr
			);
			DispatchEditorFunction(static_cast<EFunctionCode>(nFuncCode | FA_FROMKEYBOARD));
		}
		break;
	default:
		break;
	}

	return;
}

//	キーワード：メニューバー順序
//	Sept.14, 2000 Jepro note: メニューバーの項目のキャプションや順番設定などは以下で行っているらしい
//	Sept.16, 2000 Jepro note: アイコンとの関連付けはCShareData_new2.cppファイルで行っている
//	2010/5/16	Uchi	動的に作成する様に変更
void CEditWnd::InitMenu( HMENU hMenu, UINT uPos, BOOL fSystemMenu )
{
	int			cMenuItems;
	int			nPos;
	UINT		fuFlags;
	int			i;
	HMENU		hMenuPopUp;

	MENUINFO mi = { sizeof(mi) };
	mi.fMask = MIM_STYLE;
	mi.dwStyle = MNS_CHECKORBMP;
	SetMenuInfo(hMenu, &mi);

	if( hMenu == ::GetSubMenu( GetMainMenuHandle(), uPos )
		&& !fSystemMenu ){
		// 情報取得
		const CommonSetting_MainMenu*	pcMenu = &m_pShareData->m_Common.m_sMainMenu;
		const CMainMenu*	cMainMenu;
		int			nIdxStr;
		int			nIdxEnd;
		int			nLv;
		std::vector<HMENU>	hSubMenu;
		wchar_t tmpMenuName[MAX_MAIN_MENU_NAME_LEN+1];
		const wchar_t *pMenuName;

		nIdxStr = pcMenu->m_nMenuTopIdx[uPos];
		nIdxEnd = (uPos < MAX_MAINMENU_TOP) ? pcMenu->m_nMenuTopIdx[uPos+1] : -1;
		if (nIdxEnd < 0) {
			nIdxEnd = pcMenu->m_nMainMenuNum;
		}

		// メニュー 初期化
		m_cMenuDrawer.ResetContents();
		cMenuItems = ::GetMenuItemCount( hMenu );
		for( i = cMenuItems - 1; i >= 0; i-- ){
			::DeleteMenu( hMenu, i, MF_BYPOSITION );
		}

		// メニュー作成
		hSubMenu.push_back( hMenu );
		nLv = 1;
		if (pcMenu->m_cMainMenuTbl[nIdxStr].m_nType == T_SPECIAL) {
			nLv = 0;
			nIdxStr--;
		}
		for (i = nIdxStr + 1; i < nIdxEnd; i++) {
			cMainMenu = &pcMenu->m_cMainMenuTbl[i];
			if (cMainMenu->m_nLevel != nLv) {
				nLv = cMainMenu->m_nLevel;
				if (hSubMenu.size() < (size_t)nLv) {
					// 保護
					break;
				}
				hMenu = hSubMenu[nLv-1];
			}
			switch (cMainMenu->m_nType) {
			case T_NODE:
				hMenuPopUp = ::CreatePopupMenu();
				if (cMainMenu->m_nFunc != 0 && cMainMenu->m_sName[0] == L'\0') {
					// ストリングテーブルから読み込み
					wcsncpy_s(tmpMenuName, std::size(tmpMenuName), LS( cMainMenu->m_nFunc ), _TRUNCATE);
					pMenuName = tmpMenuName;
				}else{
					pMenuName = cMainMenu->m_sName;
				}
				m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hMenuPopUp ,
					pMenuName, cMainMenu->m_sKey );
				if (hSubMenu.size() > (size_t)nLv) {
					hSubMenu[nLv] = hMenuPopUp;
				}
				else {
					hSubMenu.push_back( hMenuPopUp );
				}
				break;
			case T_LEAF:
				InitMenu_Function( hMenu, cMainMenu->m_nFunc, cMainMenu->m_sName, cMainMenu->m_sKey );
				break;
			case T_SEPARATOR:
				m_cMenuDrawer.MyAppendMenuSep( hMenu, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr );
				break;
			case T_SPECIAL:
				bool	bInList;		// リストが1個以上ある
				bInList = InitMenu_Special( hMenu, cMainMenu->m_nFunc );
				// リストが無い場合の処理
				//分割線に囲まれ、かつリストなし ならば 次の分割線をスキップ
				if (!bInList &&
					i + 1 < nIdxEnd &&
					T_SEPARATOR == pcMenu->m_cMainMenuTbl[i + 1].m_nType &&
					cMainMenu->m_nLevel == pcMenu->m_cMainMenuTbl[i + 1].m_nLevel &&
					(i == nIdxStr + 1 || (0 < i && T_SEPARATOR == pcMenu->m_cMainMenuTbl[i - 1].m_nType && cMainMenu->m_nLevel == pcMenu->m_cMainMenuTbl[i - 1].m_nLevel))) {
						i++;		// スキップ
				}
				break;
			}
		}
		if (nLv > 0) {
			// レベルが戻っていない
			hMenu = hSubMenu[0];
		}
		// VS Codeのメニューは空のグループの区切り線を出さない。T_SPECIALのリストが空だった
		// ときに残る先頭・末尾・連続の区切り線をここで取り除いてから空判定を行う。
		RemoveRedundantMenuSeparators( hMenu );
		// 子の無い設定SubMenuのDesable
		CheckFreeSubMenu( GetHwnd(), hMenu, uPos );
	}

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
//	if (m_pPrintPreview)	return;	//	印刷プレビューモードなら排除。（おそらく排除しなくてもいいと思うんだけど、念のため）

	/* 機能が利用可能かどうか、チェック状態かどうかを一括チェック */
	cMenuItems = ::GetMenuItemCount( hMenu );
	for (nPos = 0; nPos < cMenuItems; nPos++) {
		EFunctionCode	id = (EFunctionCode)::GetMenuItemID(hMenu, nPos);
		/* 機能が利用可能か調べる */
		//	Jan.  8, 2006 genta 機能が有効な場合には明示的に再設定しないようにする．
		if( ! IsFuncEnable( GetDocument(), m_pShareData, id ) ){
			fuFlags = MF_BYCOMMAND | MF_DISABLED;
			::EnableMenuItem(hMenu, id, fuFlags);
		}

		/* 機能がチェック状態か調べる */
		if( IsFuncChecked( GetDocument(), m_pShareData, id ) ){
			fuFlags = MF_BYCOMMAND | MF_CHECKED;
			::CheckMenuItem(hMenu, id, fuFlags);
		}
		/* else{
			fuFlags = MF_BYCOMMAND | MF_UNCHECKED;
		}
		*/
	}

	return;
}

/*!	通常コマンド(Special以外)のメニューへの追加
*/
void CEditWnd::InitMenu_Function(HMENU hMenu, EFunctionCode eFunc, const wchar_t* pszName, const wchar_t* pszKey)
{
	const wchar_t* psName = nullptr;
	/* メニューラベルの作成 */
	// カスタムメニュー
	if (eFunc == F_MENU_RBUTTON
	  || (eFunc >= F_CUSTMENU_1 && eFunc <= F_CUSTMENU_24)) {
		int j;
		//	右クリックメニュー
		if (eFunc == F_MENU_RBUTTON) {
			j = CUSTMENU_INDEX_FOR_RBUTTONUP;
		}
		else {
			j = eFunc - F_CUSTMENU_BASE;
		}

		int nFlag = MF_BYPOSITION | MF_STRING | MF_GRAYED;
		if( m_pShareData->m_Common.m_sCustomMenu.m_nCustMenuItemNumArr[j] > 0 ){
			nFlag = MF_BYPOSITION | MF_STRING;
		}
		WCHAR buf[ MAX_CUSTOM_MENU_NAME_LEN + 1 ];
		m_cMenuDrawer.MyAppendMenu( hMenu, nFlag,
			eFunc, GetDocument()->m_cFuncLookup.Custmenu2Name( j, buf, int(std::size(buf)) ), pszKey );
	}
	// マクロ
	else if (eFunc >= F_USERMACRO_0 && eFunc < F_USERMACRO_0 + (int)MAX_CUSTMACRO) {
		MacroRec *mp = &m_pShareData->m_Common.m_sMacro.m_MacroTable[eFunc - F_USERMACRO_0];
		if (mp->IsEnabled()) {
			psName = mp->m_szName[0] ? mp->m_szName : mp->m_szFile;
			m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING,
				eFunc, psName, pszKey );
		}
		else {
			psName = L"-- undefined macro --";
			m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_GRAYED,
				eFunc, psName, pszKey );
		}
	}
	// プラグインコマンド
	else if (eFunc >= F_PLUGCOMMAND_FIRST && eFunc < F_PLUGCOMMAND_LAST) {
		WCHAR szLabel[256];
		if( 0 < CJackManager::getInstance()->GetCommandName( eFunc, szLabel, int(std::size(szLabel)) ) ){
			m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING,
				eFunc, szLabel, pszKey,
				TRUE, eFunc );
		}else{
			// not found
			psName = L"-- undefined plugin command --";
			m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_GRAYED,
				eFunc, psName, pszKey );
		}
	}else{
		switch (eFunc) {
		case F_CLOSE_WORKSPACE:
			if( m_workbenchRuntime == nullptr ) return;
			if( const UINT label = CloseWorkspaceMenuLabelResource(
				m_workbenchRuntime->WorkspaceContext().Snapshot().kind); label != 0 ){
				m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, eFunc, LS(label), pszKey );
			}
			return;
		case F_RECKEYMACRO:
		case F_SAVEKEYMACRO:
		case F_LOADKEYMACRO:
		case F_EXECKEYMACRO:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_sFlags.m_bRecordingKeyMacro);
			break;
		case F_SPLIT_V:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				m_cSplitterWnd.GetAllSplitRows() == 1 );
			break;
		case F_SPLIT_H:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				m_cSplitterWnd.GetAllSplitCols() == 1 );
			break;
		case F_SPLIT_VH:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				m_cSplitterWnd.GetAllSplitRows() == 1 || m_cSplitterWnd.GetAllSplitCols() == 1 );
			break;
		case F_TAB_CLOSEOTHER:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd != 0 );
			break;
		case F_TOPMOST:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				((DWORD)::GetWindowLongPtr( GetHwnd(), GWL_EXSTYLE ) & WS_EX_TOPMOST) == 0 );
			break;
		case F_BIND_WINDOW:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				(!m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd
				|| m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin) );
			break;
		case F_SHOWTOOLBAR:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_Common.m_sWindow.m_bMenuIcon | !m_cToolbar.GetToolbarHwnd() );
			break;
		case F_SHOWFUNCKEY:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_Common.m_sWindow.m_bMenuIcon | !m_cFuncKeyWnd.GetHwnd() );
			break;
		case F_SHOWTAB:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_Common.m_sWindow.m_bMenuIcon | !m_cTabWnd.GetHwnd() );
			break;
		case F_SHOWSTATUSBAR:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_Common.m_sWindow.m_bMenuIcon | !m_cStatusBar.GetStatusHwnd() );
			break;
		case F_SHOWMINIMAP:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_Common.m_sWindow.m_bMenuIcon | !m_cMiniMapView.GetHwnd() );
			break;
		case F_TOGGLE_KEY_SEARCH:
			SetMenuFuncSel( hMenu, eFunc, pszKey,
				!m_pShareData->m_Common.m_sWindow.m_bMenuIcon | !IsFuncChecked( GetDocument(), m_pShareData, F_TOGGLE_KEY_SEARCH ) );
			break;
		case F_WRAPWINDOWWIDTH:
			{
				CKetaXInt ketas;
				WCHAR*	pszLabel;
				CEditView::TOGGLE_WRAP_ACTION mode = GetActiveView().GetWrapMode( &ketas );
				if( mode == CEditView::TGWRAP_NONE ){
					m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_GRAYED, F_WRAPWINDOWWIDTH , L"", pszKey );
				}
				else {
					WCHAR szBuf[60];
					pszLabel = szBuf;
					if( mode == CEditView::TGWRAP_FULL ){
						auto_sprintf(
							szBuf,
							LS( STR_WRAP_WIDTH_FULL ),	//L"折り返し桁数: %d 桁（最大）",
							MAXLINEKETAS
						);
					}
					else if( mode == CEditView::TGWRAP_WINDOW ){
						auto_sprintf(
							szBuf,
							LS( STR_WRAP_WIDTH_WINDOW ),	//L"折り返し桁数: %d 桁（右端）",
							int((Int)GetActiveView().ViewColNumToWrapColNum(
								GetActiveView().GetTextArea().m_nViewColNum
							))
						);
					}
					else {
						auto_sprintf(
							szBuf,
							LS( STR_WRAP_WIDTH_FIXED ),	//L"折り返し桁数: %d 桁（指定）",
							int((Int)GetDocument()->m_cDocType.GetDocumentAttribute().m_nMaxLineKetas)
						);
					}
					m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_WRAPWINDOWWIDTH , pszLabel, pszKey );
				}
			}
			break;
		default:
			m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, eFunc,
				pszName, pszKey );
			break;
		}
	}
}

/*!	Specialコマンドのメニューへの追加
*/
UINT CEditWnd::CloseWorkspaceMenuLabelResource(config::EWorkspaceKind kind) noexcept
{
	switch( kind ){
	case config::EWorkspaceKind::Folder:
		return STR_CLOSE_FOLDER;
	case config::EWorkspaceKind::Workspace:
		return F_CLOSE_WORKSPACE;
	case config::EWorkspaceKind::Empty:
		return 0;
	}
	return 0;
}

bool CEditWnd::InitMenu_Special(HMENU hMenu, EFunctionCode eFunc)
{
	int j;
	bool bInList = false;
	switch (eFunc) {
	case F_WINDOW_LIST:				// ウィンドウリスト
		{
			EditNode*	pEditNodeArr;
			int nRowNum = CAppNodeManager::getInstance()->GetOpenedWindowArr( &pEditNodeArr, TRUE );
			WinListMenu(hMenu, pEditNodeArr, nRowNum, false);
			bInList = (nRowNum > 0);
			delete [] pEditNodeArr;
		}
		break;
	case F_FILE_USED_RECENTLY:		// typed workspaces/folders followed by legacy files
		/* The typed history deliberately does not consult CMRUFolder. */
		{
			const CMRUFile cMRU;
			bInList = AppendRecentlyOpenedWorkspaceMenu(hMenu, cMRU.MenuLength() > 0);
			if (cMRU.MenuLength() > 0) {
				cMRU.CreateMenu(hMenu, &m_cMenuDrawer);
				bInList = true;
			}
		}
		break;
	case F_RECENT_WORKSPACE_LIST:
		// The stable Open Recent submenu is one combined projection.  Typed
		// workspace/folder rows own 13000..13063; legacy recent files retain
		// their established IDs below them.  Upstream closes the submenu with a
		// static Clear Recently Opened entry, so this surface is never empty.
		{
			const CMRUFile cMRU;
			bool hasRows = AppendRecentlyOpenedWorkspaceMenu(hMenu, cMRU.MenuLength() > 0);
			if (cMRU.MenuLength() > 0) {
				cMRU.CreateMenu(hMenu, &m_cMenuDrawer);
				hasRows = true;
			}
			AppendClearRecentlyOpenedMenuItem(hMenu, hasRows);
			bInList = true;
		}
		break;
	case F_FOLDER_USED_RECENTLY:	// 最近使ったフォルダー
		/* 最近使ったフォルダーのメニューを作成 */
		{
			//@@@ 2001.12.26 YAZAKI OPENFOLDERリストは、CMRUFolderにすべて依頼する
			const CMRUFolder cMRUFolder;
			cMRUFolder.CreateMenu( hMenu, &m_cMenuDrawer );
			bInList = (cMRUFolder.MenuLength() > 0);
		}
		break;
	case F_CUSTMENU_LIST:			// カスタムメニューリスト
		WCHAR buf[ MAX_CUSTOM_MENU_NAME_LEN + 1 ];
		//	右クリックメニュー
		if( m_pShareData->m_Common.m_sCustomMenu.m_nCustMenuItemNumArr[0] > 0 ){
			 m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING,
				 F_MENU_RBUTTON, GetDocument()->m_cFuncLookup.Custmenu2Name( 0, buf, int(std::size(buf)) ), L"" );
			bInList = true;
		}
		//	カスタムメニュー
		for( j = 1; j < MAX_CUSTOM_MENU; ++j ){
			if( m_pShareData->m_Common.m_sCustomMenu.m_nCustMenuItemNumArr[j] > 0 ){
				 m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING,
			 		F_CUSTMENU_BASE + j, GetDocument()->m_cFuncLookup.Custmenu2Name( j, buf, int(std::size(buf)) ), L""  );
				bInList = true;
			}
		}
		break;
	case F_USERMACRO_LIST:			// 登録済みマクロリスト
		for( j = 0; j < MAX_CUSTMACRO; ++j ){
			MacroRec *mp = &m_pShareData->m_Common.m_sMacro.m_MacroTable[j];
			if( mp->IsEnabled() ){
				if(  mp->m_szName[0] ){
					m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_USERMACRO_0 + j, mp->m_szName, L"" );
				}
				else {
					m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_USERMACRO_0 + j, mp->m_szFile, L"" );
				}
				bInList = true;
			}
		}
		break;
	case F_PLUGIN_LIST:				// プラグインコマンドリスト
		//プラグインコマンドを提供するプラグインを列挙する
		{
			const CJackManager* pcJackManager = CJackManager::getInstance();
			const CPlugin* prevPlugin = nullptr;
			HMENU hMenuPlugin = nullptr;

			CPlug::Array plugs = pcJackManager->GetPlugs( PP_COMMAND );
			for( CPlug::ArrayIter it = plugs.cbegin(); it != plugs.cend(); it++ ){
				const CPlugin* curPlugin = &(*it)->m_cPlugin;
				if( curPlugin != prevPlugin ){
					//プラグインが変わったらプラグインポップアップメニューを登録
					hMenuPlugin = ::CreatePopupMenu();
					m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hMenuPlugin, curPlugin->m_sName.c_str(), L"" );
					prevPlugin = curPlugin;
				}

				//コマンドを登録
				m_cMenuDrawer.MyAppendMenu( hMenuPlugin, MF_BYPOSITION | MF_STRING,
					(*it)->GetFunctionCode(), (*it)->m_sLabel.c_str(), L"",
					TRUE, (*it)->GetFunctionCode() );
			}
			bInList = (prevPlugin != nullptr);
		}
		break;
	default:
		break;
	}
	return bInList;
}

/*!	先頭・末尾・連続した区切り線の除去

	VS Codeのメニューは区切り線でグループを区切るが、片側のグループが空になった区切り線は
	描画しない。ここでも空のMRUリストなどが残した区切り線を取り除き、結果として空になった
	サブメニューは呼び出し元の CheckFreeSubMenu が無効化する。
*/
void CEditWnd::RemoveRedundantMenuSeparators( HMENU hMenu )
{
	if( hMenu == nullptr ) return;

	// 先に下位レベルを整理する。子が空になった場合でもポップアップ自体は残るので、
	// このレベルでは「区切り線かどうか」だけを見ればよい。
	const int nItems = ::GetMenuItemCount( hMenu );
	if( nItems < 0 ) return;
	for( int nPos = 0; nPos < nItems; nPos++ ){
		RemoveRedundantMenuSeparators( ::GetSubMenu( hMenu, nPos ) );
	}

	// 末尾から走査すると削除で後続位置がずれない。
	bool bNextIsSeparatorOrEnd = true;		// 末尾は「後ろに何もない」と同じ扱い
	for( int nPos = ::GetMenuItemCount( hMenu ) - 1; nPos >= 0; nPos-- ){
		MENUITEMINFO mii = { sizeof(MENUITEMINFO) };
		mii.fMask = MIIM_FTYPE | MIIM_SUBMENU;
		if( !::GetMenuItemInfo( hMenu, nPos, TRUE, &mii ) ){
			bNextIsSeparatorOrEnd = false;
			continue;
		}
		const bool bSeparator = (mii.hSubMenu == nullptr) && ((mii.fType & MFT_SEPARATOR) != 0);
		if( bSeparator && bNextIsSeparatorOrEnd ){
			// 末尾・連続の区切り線。先頭の区切り線もこの走査で最終的に末尾扱いになる。
			::DeleteMenu( hMenu, nPos, MF_BYPOSITION );
			continue;
		}
		bNextIsSeparatorOrEnd = bSeparator;
	}
	// 残った先頭の区切り線を落とす。
	while( ::GetMenuItemCount( hMenu ) > 0 ){
		MENUITEMINFO mii = { sizeof(MENUITEMINFO) };
		mii.fMask = MIIM_FTYPE | MIIM_SUBMENU;
		if( !::GetMenuItemInfo( hMenu, 0, TRUE, &mii ) ) break;
		if( (mii.hSubMenu != nullptr) || ((mii.fType & MFT_SEPARATOR) == 0) ) break;
		::DeleteMenu( hMenu, 0, MF_BYPOSITION );
	}
}

// メニューバーの無効化を検査	2010/6/18 Uchi
void CEditWnd::CheckFreeSubMenu( [[maybe_unused]] HWND hWnd, HMENU hMenu, UINT uPos )
{
	int 	cMenuItems;

	cMenuItems = ::GetMenuItemCount( hMenu );
	if (cMenuItems == 0) {
		// 下が無いので無効化
		::EnableMenuItem( GetMainMenuHandle(), uPos, MF_BYPOSITION | MF_GRAYED );
	}
	else {
		// 下位レベルを検索
		CheckFreeSubMenuSub( hMenu, 1 );
	}
}

// メニューバーの無効化を検査	2010/6/18 Uchi
void CEditWnd::CheckFreeSubMenuSub( HMENU hMenu, int nLv )
{
	HMENU	hSubMenu;
	int 	cMenuItems;
	int 	nPos;

	cMenuItems = ::GetMenuItemCount( hMenu );
	for (nPos = 0; nPos < cMenuItems; nPos++) {
		hSubMenu = ::GetSubMenu( hMenu, nPos );
		if (hSubMenu != nullptr) {
			if ( ::GetMenuItemCount( hSubMenu ) == 0) {
				// 下が無いので無効化
				::EnableMenuItem(hMenu, nPos, MF_BYPOSITION | MF_GRAYED);
			}
			else {
				// 下位レベルを検索
				CheckFreeSubMenuSub( hSubMenu, nLv + 1 );
			}
		}
	}
}

//	フラグにより表示文字列の選択をする。
//		2010/5/19	Uchi
void CEditWnd::SetMenuFuncSel( HMENU hMenu, EFunctionCode nFunc, const WCHAR* sKey, bool flag )
{
	int				i;
	const WCHAR*	sName = L"";
	for (i = 0; i < int(std::size(sFuncMenuName)) ;i++) {
		if (sFuncMenuName[i].eFunc == nFunc) {
			sName = flag ? LS( sFuncMenuName[i].nNameId[0] ) : LS( sFuncMenuName[i].nNameId[1] );
		}
	}
	assert( wcslen(sName) );

	m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, nFunc, sName, sKey );
}

STDMETHODIMP CEditWnd::DragEnter(
	LPDATAOBJECT pDataObject,
	DWORD dwKeyState,
	POINTL pt [[maybe_unused]],
	LPDWORD pdwEffect
) const
{
	if( pDataObject == nullptr || pdwEffect == nullptr ){
		return E_INVALIDARG;
	}

	// 右ボタンファイルドロップの場合だけ処理する
	if( !((MK_RBUTTON & dwKeyState) && IsDataAvailable(pDataObject, CF_HDROP)) ){
		*pdwEffect = DROPEFFECT_NONE;
		return E_INVALIDARG;
	}

	// 印刷プレビューでは受け付けない
	if( m_pPrintPreview ){
		*pdwEffect = DROPEFFECT_NONE;
		return E_INVALIDARG;
	}

	*pdwEffect &= DROPEFFECT_LINK;
	return S_OK;
}

STDMETHODIMP CEditWnd::DragOver(
	DWORD dwKeyState [[maybe_unused]],
	POINTL pt [[maybe_unused]],
	LPDWORD pdwEffect
) const
{
	if( pdwEffect == nullptr )
		return E_INVALIDARG;

	*pdwEffect &= DROPEFFECT_LINK;
	return S_OK;
}

STDMETHODIMP CEditWnd::DragLeave() const
{
	return S_OK;
}

STDMETHODIMP CEditWnd::Drop(LPDATAOBJECT pDataObject, [[maybe_unused]] DWORD dwKeyState, [[maybe_unused]] POINTL pt, LPDWORD pdwEffect)
{
	if( pDataObject == nullptr || pdwEffect == nullptr )
		return E_INVALIDARG;

	// ファイルドロップをアクティブビューで処理する
	*pdwEffect &= DROPEFFECT_LINK;
	return GetActiveView().PostMyDropFiles( pDataObject );
}

/* ファイルがドロップされた */
void CEditWnd::OnDropFiles( HDROP hDrop )
{
	POINT		pt;
	int			cFiles, i;
	EditInfo*	pfi;
	HWND		hWndOwner;

	::DragQueryPoint( hDrop, &pt );
	cFiles = (int)::DragQueryFile( hDrop, 0xFFFFFFFF, nullptr, 0);
	/* ファイルをドロップしたときは閉じて開く */
	if( m_pShareData->m_Common.m_sFile.m_bDropFileAndClose ){
		cFiles = 1;
	}
	/* 一度にドロップ可能なファイル数 */
	if( cFiles > m_pShareData->m_Common.m_sFile.m_nDropFileNumMax ){
		cFiles = m_pShareData->m_Common.m_sFile.m_nDropFileNumMax;
	}

	/* アクティブにする */	// 2009.08.20 ryoji 処理開始前に無条件でアクティブ化
	ActivateFrameWindow( GetHwnd() );

	for( i = 0; i < cFiles; i++ ) {
		//ファイルパス取得、解決。
		WCHAR		szFile[_MAX_PATH + 1];
		::DragQueryFile( hDrop, i, szFile, int(std::size(szFile)) );
		CSakuraEnvironment::ResolvePath(szFile);

		/* 指定ファイルが開かれているか調べる */
		if( CShareData::getInstance()->IsPathOpened( szFile, &hWndOwner ) ){
			::SendMessage( hWndOwner, MYWM_GETFILEINFO, 0, 0 );
			pfi = (EditInfo*)&m_pShareData->m_sWorkBuffer.m_EditInfo_MYWM_GETFILEINFO;
			/* アクティブにする */
			ActivateFrameWindow( hWndOwner );
			/* MRUリストへの登録 */
			CMRUFile cMRU;
			cMRU.Add( pfi );
		}
		else{
			/* 変更フラグがオフで、ファイルを読み込んでいない場合 */
			//	2005.06.24 Moca
			if( GetDocument()->IsAcceptLoad() ){
				/* ファイル読み込み */
				SLoadInfo sLoadInfo(szFile, CODE_AUTODETECT, false);
				GetDocument()->m_cDocFileOperation.FileLoad(&sLoadInfo);
			}
			else{
				/* ファイルをドロップしたときは閉じて開く */
				if( m_pShareData->m_Common.m_sFile.m_bDropFileAndClose ){
					/* ファイル読み込み */
					SLoadInfo sLoadInfo(szFile, CODE_AUTODETECT, false);
					(void)GetDocument()->m_cDocFileOperation.FileCloseOpen(sLoadInfo);
				}
				else{
					/* 編集ウィンドウの上限チェック */
					if( m_pShareData->m_sNodes.m_nEditArrNum >= MAX_EDITWINDOWS ){	//最大値修正	//@@@ 2003.05.31 MIK
						::DragFinish( hDrop );
						OkMessage( nullptr, LS(STR_MAXWINDOW), MAX_EDITWINDOWS );
						return;
					}
					/* 新たな編集ウィンドウを起動 */
					SLoadInfo sLoadInfo;
					sLoadInfo.cFilePath = szFile;
					sLoadInfo.eCharCode = CODE_NONE;
					sLoadInfo.bViewMode = false;
					CControlTray::OpenNewEditor(
						G_AppInstance(),
						GetHwnd(),
						sLoadInfo
					);
				}
			}
		}
	}
	::DragFinish( hDrop );
	return;
}

/*! WM_TIMER 処理
	@date 2007.04.03 ryoji 新規
	@date 2008.04.19 ryoji IDT_FIRST_IDLE での MYWM_FIRST_IDLE ポスト処理を追加
	@date 2013.06.09 novice コントロールプロセスへの MYWM_FIRST_IDLE ポスト処理を追加
*/
LRESULT CEditWnd::OnTimer( WPARAM wParam, [[maybe_unused]] LPARAM lParam )
{
	// タイマー ID で処理を振り分ける
	switch( wParam )
	{
	case IDT_EDIT:
		OnEditTimer();
		break;
	case IDT_TOOLBAR:
		m_cToolbar.OnToolbarTimer();
		break;
	case IDT_CAPTION:
		OnCaptionTimer();
		break;
	case IDT_SYSMENU:
		OnSysMenuTimer();
		break;
	case IDT_FIRST_IDLE:
		m_cDlgFuncList.m_bEditWndReady = true;	// エディタ画面の準備完了
		CAppNodeGroupHandle(0).PostMessageToAllEditors( MYWM_FIRST_IDLE, ::GetCurrentProcessId(), 0, nullptr );	// プロセスの初回アイドリング通知	// 2008.04.19 ryoji
		::PostMessage( m_pShareData->m_sHandles.m_hwndTray, MYWM_FIRST_IDLE, (WPARAM)::GetCurrentProcessId(), (LPARAM)0 );
		::KillTimer( m_hWnd, wParam );
		PostDeferredStartupWorkbenchIfReady();
		break;
	case IDT_WORKBENCH_KEYBINDING_CHORD:
		// SetTimer can leave an already-queued WM_TIMER behind when Ctrl+K is
		// repeated. Only expire the state once its own deadline is actually due.
		ExpireWorkbenchKeybindingChord();
		break;
	default:
		return 1L;
	}

	return 0L;
}

/*! キャプション更新用タイマーの処理
	@date 2007.04.03 ryoji 新規
*/
void CEditWnd::OnCaptionTimer() const
{
	// 編集画面の切替（タブまとめ時）が終わっていたらタイマーを終了してタイトルバーを更新する
	// まだ切替中ならタイマー継続
	if( !m_pShareData->m_sFlags.m_bEditWndChanging ){
		::KillTimer( GetHwnd(), IDT_CAPTION );
		::SetWindowText( GetHwnd(), m_pszLastCaption );
	}
}

/*! システムメニュー表示用タイマーの処理
	@date 2007.04.03 ryoji パラメータ無しにした
	                       以前はコールバック関数でやっていたKillTimer()をここで行うようにした
*/
void CEditWnd::OnSysMenuTimer( void ) //by 鬼(2)
{
	::KillTimer( GetHwnd(), IDT_SYSMENU );	// 2007.04.03 ryoji

	if(m_IconClicked == icClicked)
	{
		ReleaseCapture();

		//システムメニュー表示
		// 2006.04.21 ryoji マルチモニタ対応の修正
		// 2007.05.13 ryoji 0x0313メッセージをポストする方式に変更（TrackPopupMenuだとメニュー項目の有効／無効状態が不正になる問題対策）
		RECT R;
		GetWindowRect(GetHwnd(), &R);
		POINT pt;
		pt.x = R.left + GetSystemMetrics(SM_CXFRAME);
		pt.y = R.top + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
		GetMonitorWorkRect( pt, &R );
		::PostMessageAny(
			GetHwnd(),
			0x0313, //右クリックでシステムメニューを表示する際に送信するモノらしい
			0,
			MAKELPARAM( (pt.x > R.left)? pt.x: R.left, (pt.y < R.bottom)? pt.y: R.bottom )
		);
	}
	m_IconClicked = icNone;
}

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更

/* 印刷プレビューモードのオン/オフ */
void CEditWnd::PrintPreviewModeONOFF( void )
{
	if (!m_pPrintPreview && !HasActiveEditorInput()) return;

	HMENU	hMenu;
	HWND	hwndToolBar;

	// 2006.06.17 ryoji Rebar があればそれをツールバー扱いする
	hwndToolBar = (nullptr != m_cToolbar.GetRebarHwnd())? m_cToolbar.GetRebarHwnd(): m_cToolbar.GetToolbarHwnd();

	/* 印刷プレビューモードか */
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	if( m_pPrintPreview ){
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		/*	印刷プレビューモードを解除します。	*/
		m_pPrintPreview = nullptr;	//	NULLか否かで、プリントプレビューモードか判断するため。

		/*	通常モードに戻す	*/
		::ShowWindow( this->m_cSplitterWnd.GetHwnd(), SW_SHOW );
		::ShowWindow( hwndToolBar, SW_SHOW );	// 2006.06.17 ryoji
		::ShowWindow( m_cStatusBar.GetStatusHwnd(), SW_SHOW );
		::ShowWindow( m_cFuncKeyWnd.GetHwnd(), SW_SHOW );
		::ShowWindow( m_cTabWnd.GetHwnd(), SW_SHOW );	//@@@ 2003.06.25 MIK
		::ShowWindow( m_cDlgFuncList.GetHwnd(), SW_SHOW );	// 2010.06.25 ryoji
		if (m_activityBar) ::ShowWindow(m_activityBar->GetHwnd(), SW_SHOWNA);
		for (const auto* panel : { m_leftWorkbenchPanel.get(), m_rightWorkbenchPanel.get(), m_bottomWorkbenchPanel.get() }) {
			if (panel && panel->GetState() != workbench::WorkbenchPanelState::Hidden) {
				::ShowWindow(panel->GetHwnd(), SW_SHOWNA);
			}
		}
		if( m_cMiniMapView.GetHwnd() ){
			::ShowWindow( m_cMiniMapView.GetHwnd(), SW_SHOW );
		}
		if (!HasActiveEditorInput()) {
			::ShowWindow(m_cSplitterWnd.GetHwnd(), SW_HIDE);
			if (m_cMiniMapView.GetHwnd()) ::ShowWindow(m_cMiniMapView.GetHwnd(), SW_HIDE);
			if (m_emptyEditorSurface) m_emptyEditorSurface->Show();
		}

		// その他のモードレスダイアログも戻す	// 2010.06.25 ryoji
		::ShowWindow( m_cDlgFind.GetHwnd(), SW_SHOW );
		::ShowWindow( m_cDlgReplace.GetHwnd(), SW_SHOW );
		::ShowWindow( m_cDlgGrep.GetHwnd(), SW_SHOW );

		::SetFocus( GetHwnd() );

		// メニューを動的に作成するように変更
		//hMenu = ::LoadMenu( G_AppInstance(), MAKEINTRESOURCE( IDR_MENU1 ) );
		//::SetMenu( GetHwnd(), hMenu );
		//::DrawMenuBar( GetHwnd() );
		LayoutMainMenu();				// 2010/5/16 Uchi

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		::InvalidateRect( GetHwnd(), nullptr, FALSE );
	}else{
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		/*	通常モードを隠す	*/
		hMenu = m_customFrame ? m_customFrame->ReplaceMenu(nullptr) : ::GetMenu(GetHwnd());
		//	Jun. 18, 2001 genta Print Previewではメニューを削除
		if (!m_customFrame) {
			::SetMenu(GetHwnd(), nullptr);
		}
		::DestroyMenu( hMenu );
		if (m_customFrame) {
			m_customFrame->InvalidateTitle();
		} else {
			::DrawMenuBar(GetHwnd());
		}

		::ShowWindow( this->m_cSplitterWnd.GetHwnd(), SW_HIDE );
		::ShowWindow( hwndToolBar, SW_HIDE );	// 2006.06.17 ryoji
		::ShowWindow( m_cStatusBar.GetStatusHwnd(), SW_HIDE );
		::ShowWindow( m_cFuncKeyWnd.GetHwnd(), SW_HIDE );
		::ShowWindow( m_cTabWnd.GetHwnd(), SW_HIDE );	//@@@ 2003.06.25 MIK
		::ShowWindow( m_cDlgFuncList.GetHwnd(), SW_HIDE );	// 2010.06.25 ryoji
		if (m_activityBar) ::ShowWindow(m_activityBar->GetHwnd(), SW_HIDE);
		for (const auto* panel : { m_leftWorkbenchPanel.get(), m_rightWorkbenchPanel.get(), m_bottomWorkbenchPanel.get() }) {
			if (panel) ::ShowWindow(panel->GetHwnd(), SW_HIDE);
		}
		if( m_cMiniMapView.GetHwnd() ){
			::ShowWindow( m_cMiniMapView.GetHwnd(), SW_HIDE );
		}
		if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();

		// その他のモードレスダイアログも隠す	// 2010.06.25 ryoji
		::ShowWindow( m_cDlgFind.GetHwnd(), SW_HIDE );
		::ShowWindow( m_cDlgReplace.GetHwnd(), SW_HIDE );
		::ShowWindow( m_cDlgGrep.GetHwnd(), SW_HIDE );

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		m_pPrintPreview = std::make_unique<CPrintPreview>(this);
		/* 現在の印刷設定 */
		m_pPrintPreview->SetPrintSetting(
			&m_pShareData->m_PrintSettingArr[
				GetDocument()->m_cDocType.GetDocumentAttribute().m_nCurrentPrintSetting]
		);

		//	プリンターの情報を取得。

		/* 現在のデフォルトプリンターの情報を取得 */
		BOOL bRes;
		bRes = m_pPrintPreview->GetDefaultPrinterInfo();
		if( !bRes ){
			TopInfoMessage( GetHwnd(), LS(STR_ERR_DLGEDITWND14) );
			return;
		}

		/* 印刷設定の反映 */
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		m_pPrintPreview->OnChangePrintSetting();
		::InvalidateRect( GetHwnd(), nullptr, FALSE );
	}
	return;
}

/* WM_SIZE 処理 */
void CEditWnd::LayoutStatusBarParts()
{
	const HWND statusBar = m_cStatusBar.GetStatusHwnd();
	if (statusBar == nullptr) return;

	RECT client{};
	::GetClientRect(statusBar, &client);
	constexpr int partCount = 8;
	std::wstring labels[partCount];
	bool hasCurrentText = false;
	for (int part = 1; part < partCount; ++part) {
		const LRESULT textInfo = ::SendMessageW(statusBar, SB_GETTEXTLENGTHW, part, 0);
		const UINT style = HIWORD(textInfo);
		if ((style & SBT_OWNERDRAW) != 0) {
			labels[part] = L"REC";
			hasCurrentText = true;
			continue;
		}

		const UINT textLength = LOWORD(textInfo);
		if (textLength == 0) continue;
		std::vector<wchar_t> text(static_cast<size_t>(textLength) + 1, L'\0');
		::SendMessageW(statusBar, SB_GETTEXTW, part, reinterpret_cast<LPARAM>(text.data()));
		labels[part].assign(text.data(), textLength);
		hasCurrentText = true;
	}

	// Before the caret publishes its first snapshot, retain conservative widths.
	// Subsequent updates use only the visible strings, so an empty character-code
	// item no longer leaves a large hole in the right-aligned group.
	if (!hasCurrentText && HasActiveEditorInput()) {
		labels[1] = L"99999 行 9999 列";
		labels[2] = L"CRLF";
		labels[3] = L"AAAAAAAAAAAA";
		labels[4] = L"UTF-16 BOM付";
		labels[5] = L"REC";
		labels[6] = L"上書";
		labels[7] = L"9999 %";
	}

	int partEdges[partCount]{};
	partEdges[partCount - 1] = client.right - client.left - m_cStatusBar.ReservedRightWidth();
	if (!::IsZoomed(GetHwnd())) {
		partEdges[partCount - 1] -= ::GetSystemMetrics(SM_CXVSCROLL) + ::GetSystemMetrics(SM_CXEDGE);
	}
	partEdges[partCount - 1] = std::max(0, partEdges[partCount - 1]);

	const UINT dpi = static_cast<UINT>(::GetDpiForWindow(statusBar));
	const HDC dc = ::GetDC(statusBar);
	HFONT oldFont = nullptr;
	if (dc != nullptr) {
		const HFONT font = reinterpret_cast<HFONT>(::SendMessageW(statusBar, WM_GETFONT, 0, 0));
		if (font != nullptr) oldFont = reinterpret_cast<HFONT>(::SelectObject(dc, font));
	}
	for (int part = partCount - 1; part > 0; --part) {
		SIZE extent{};
		if (dc != nullptr && !labels[part].empty()) {
			::GetTextExtentPoint32W(dc, labels[part].c_str(), static_cast<int>(labels[part].size()), &extent);
		}
		const auto entryId = CMainStatusBar::LegacyEntryIdForPart(part);
		const int width = labels[part].empty() || !m_cStatusBar.IsStatusbarEntryVisible(entryId)
			? 0 : workbench::icons::StatusItemPartWidthPixels(extent.cx, dpi);
		partEdges[part - 1] = std::max(0, partEdges[part] - width);
	}
	if (dc != nullptr) {
		if (oldFont != nullptr) ::SelectObject(dc, oldFont);
		::ReleaseDC(statusBar, dc);
	}

	ApiWrap::StatusBar_SetParts(statusBar, partCount, partEdges);
}

LRESULT CEditWnd::OnSize( WPARAM wParam, LPARAM lParam )
{
	return OnSize2(wParam, lParam, true);
}

LRESULT CEditWnd::OnSize2( WPARAM wParam, LPARAM lParam, bool bUpdateStatus )
{
	if (ShouldDeferStartupLayout()) {
		return 0L;
	}
	if (m_layoutInProgress) {
		m_layoutPending = true;
		m_pendingLayoutWParam = wParam;
		m_pendingLayoutLParam = lParam;
		m_pendingLayoutUpdateStatus = m_pendingLayoutUpdateStatus || bUpdateStatus;
		return 0L;
	}
	m_layoutInProgress = true;
	// Pointer-driven layout samples are one retained-frame transaction.  Do not
	// synchronously paint an individual toolbar/status child while its siblings
	// still carry the previous geometry; the parent flushes the complete cohort.
	const bool deferChildPaint = m_resizingWorkbenchPanel != nullptr
		|| m_resizingMarkdownPreview;
	auto finishLayout = [this](LRESULT result) {
		m_layoutInProgress = false;
		if (!m_layoutPending) return result;
		const WPARAM pendingWParam = m_pendingLayoutWParam;
		const LPARAM pendingLParam = m_pendingLayoutLParam;
		const bool pendingUpdateStatus = m_pendingLayoutUpdateStatus;
		m_layoutPending = false;
		m_pendingLayoutUpdateStatus = false;
		return OnSize2(pendingWParam, pendingLParam, pendingUpdateStatus);
	};
	HWND		hwndToolBar;
	int			cx;
	int			cy;
	int			nToolBarHeight;
	int			nStatusBarHeight;
	int			nFuncKeyWndHeight;
	int			nTabWndHeight;	//タブウインドウ	//@@@ 2003.05.31 MIK
	RECT		rc, rcClient;
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる
//	変数削除

	RECT		rcWin;

	cx = LOWORD( lParam );
	cy = HIWORD( lParam );
	const int nCustomTitleHeight = m_customFrame ? m_customFrame->TitleHeight() : 0;

	/* ウィンドウサイズ継承 */
	if( wParam != SIZE_MINIMIZED ){						/* 最小化は継承しない */
		//	2004.05.13 Moca m_eSaveWindowSizeの解釈追加のため
		if( WINSIZEMODE_SAVE == m_pShareData->m_Common.m_sWindow.m_eSaveWindowSize ){		/* ウィンドウサイズ継承をするか */
			if( wParam == SIZE_MAXIMIZED ){					/* 最大化はサイズを記録しない */
				if( m_pShareData->m_Common.m_sWindow.m_nWinSizeType != (int)wParam ){
					m_pShareData->m_Common.m_sWindow.m_nWinSizeType = (int)wParam;
				}
			}else{
				// Aero Snapの縦方向最大化状態で終了して次回起動するときは元のサイズにする必要があるので、
				// GetWindowRect()ではなくGetWindowPlacement()で得たワークエリア座標をスクリーン座標に変換して記憶する	// 2009.09.02 ryoji
				WINDOWPLACEMENT wp;
				wp.length = sizeof(wp);
				::GetWindowPlacement( GetHwnd(), &wp );	// ワークエリア座標
				rcWin = wp.rcNormalPosition;
				RECT rcWork, rcMon;
				GetMonitorWorkRect( GetHwnd(), &rcWork, &rcMon );
				::OffsetRect(&rcWin, rcWork.left - rcMon.left, rcWork.top - rcMon.top);	// スクリーン座標に変換
				/* ウィンドウサイズに関するデータが変更されたか */
				if( m_pShareData->m_Common.m_sWindow.m_nWinSizeType != (int)wParam ||
					m_pShareData->m_Common.m_sWindow.m_nWinSizeCX != rcWin.right - rcWin.left ||
					m_pShareData->m_Common.m_sWindow.m_nWinSizeCY != rcWin.bottom - rcWin.top
				){
					m_pShareData->m_Common.m_sWindow.m_nWinSizeType = (int)wParam;
					m_pShareData->m_Common.m_sWindow.m_nWinSizeCX = rcWin.right - rcWin.left;
					m_pShareData->m_Common.m_sWindow.m_nWinSizeCY = rcWin.bottom - rcWin.top;
				}
			}
		}

		// 元に戻すときのサイズ種別を記憶	// 2007.06.20 ryoji
		EditNode *p = CAppNodeManager::getInstance()->GetEditNode( GetHwnd() );
		if( p != nullptr ){
			p->m_showCmdRestore = ::IsZoomed( p->GetHwnd() )? SW_SHOWMAXIMIZED: SW_SHOWNORMAL;
		}
	}

	m_nWinSizeType = (int)wParam;	/* サイズ変更のタイプ */

	// 2006.06.17 ryoji Rebar があればそれをツールバー扱いする
	hwndToolBar = (nullptr != m_cToolbar.GetRebarHwnd())? m_cToolbar.GetRebarHwnd(): m_cToolbar.GetToolbarHwnd();
	nToolBarHeight = 0;
	if( nullptr != hwndToolBar ){
		::SendMessage( hwndToolBar, WM_SIZE, wParam, lParam );
		::GetWindowRect( hwndToolBar, &rc );
		nToolBarHeight = rc.bottom - rc.top;
	}
	nFuncKeyWndHeight = 0;
	if( nullptr != m_cFuncKeyWnd.GetHwnd() ){
		::SendMessage( m_cFuncKeyWnd.GetHwnd(), WM_SIZE, wParam, lParam );
		::GetWindowRect( m_cFuncKeyWnd.GetHwnd(), &rc );
		nFuncKeyWndHeight = rc.bottom - rc.top;
	}
	//@@@ From Here 2003.05.31 MIK
	//@@@ To Here 2003.05.31 MIK
	bool bMiniMapSizeBox = true;
	if( wParam == SIZE_MAXIMIZED ){
		bMiniMapSizeBox = false;
	}
	nStatusBarHeight = 0;
	if( nullptr != m_cStatusBar.GetStatusHwnd() ){
		::SendMessage( m_cStatusBar.GetStatusHwnd(), WM_SIZE, wParam, lParam );
		::GetClientRect( m_cStatusBar.GetStatusHwnd(), &rc );
		//	May 12, 2000 genta
		//	2カラム目に改行コードの表示を挿入
		//	From Here
		// 2003.08.26 Moca CR0LF0廃止に従い、適当に調整
		// 2004-02-28 yasu 文字列を出力時の書式に合わせる
		// 幅を変えた場合にはCEditView::ShowCaretPosInfo()での表示方法を見直す必要あり．
		//	Nov. 8, 2003 genta
		//	初期状態ではすべての部分が「枠あり」だが，メッセージエリアは枠を描画しないようにしている
		//	ため，初期化時の枠が変な風に残ってしまう．初期状態で枠を描画させなくするため，
		//	最初に「枠無し」状態を設定した後でバーの分割を行う．
		if( bUpdateStatus ){
			m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, L"");
		}

		LayoutStatusBarParts();

		if( !deferChildPaint && m_startupDrawState != StartupDrawState::Committing ){
			::UpdateWindow( m_cStatusBar.GetStatusHwnd() );	// 2006.06.17 ryoji 即時描画でちらつきを減らす
		}
		::GetWindowRect( m_cStatusBar.GetStatusHwnd(), &rc );
		nStatusBarHeight = rc.bottom - rc.top;
		bMiniMapSizeBox = false;
	}
	::GetClientRect( GetHwnd(), &rcClient );
	const auto physicalDpi = GetHwnd() == nullptr ? 96 : ::GetDpiForWindow(GetHwnd());
	workbench::WorkbenchLayoutRequest layoutRequest;
	layoutRequest.clientWidth = cx;
	layoutRequest.clientHeight = cy;
	layoutRequest.dpi = workbench::ScaleDpi(physicalDpi, m_workbenchZoomPercent);
	layoutRequest.activityBarLocation = m_activityBarLocation;
	layoutRequest.titleBarHeightPixels = nCustomTitleHeight;
	layoutRequest.topAccessoryHeightPixels = nToolBarHeight
		+ (m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ? nFuncKeyWndHeight : 0);
	layoutRequest.documentTabsHeightPixels = 0;
	layoutRequest.bottomAccessoryHeightPixels =
		m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 1 ? nFuncKeyWndHeight : 0;
	layoutRequest.statusBarHeightPixels = nStatusBarHeight;
	layoutRequest.leftPane = m_leftWorkbenchPanel
		? m_leftWorkbenchPanel->GetState() : workbench::WorkbenchPanelState::Hidden;
	layoutRequest.rightPane = m_rightWorkbenchPanel
		? m_rightWorkbenchPanel->GetState() : workbench::WorkbenchPanelState::Hidden;
	layoutRequest.bottomPane = m_bottomWorkbenchPanel
		? m_bottomWorkbenchPanel->GetState() : workbench::WorkbenchPanelState::Hidden;
	layoutRequest.bottomPaneMaximized = m_bottomWorkbenchMaximized
		&& layoutRequest.bottomPane != workbench::WorkbenchPanelState::Hidden;
	layoutRequest.showMinimap = HasActiveEditorInput() && m_cMiniMapView.GetHwnd() != nullptr;
	layoutRequest.minimapOnLeft = m_miniMapOptions.side == minimap::Side::Left;
	layoutRequest.leftPaneWidthDip = m_leftWorkbenchPanel
		? m_leftWorkbenchPanel->GetPendingExtentDip() : m_pShareData->m_Common.m_sWorkbench.m_nLeftPanelExtent96;
	layoutRequest.rightPaneWidthDip = m_rightWorkbenchPanel
		? m_rightWorkbenchPanel->GetPendingExtentDip() : m_pShareData->m_Common.m_sWorkbench.m_nAuxiliaryBarExtent96;
	layoutRequest.bottomPaneHeightDip = m_bottomWorkbenchPanel
		? m_bottomWorkbenchPanel->GetPendingExtentDip() : m_pShareData->m_Common.m_sWorkbench.m_nBottomPanelExtent96;
	layoutRequest.minimapWidthDip = minimap::PreferredWidthDip(m_miniMapOptions);

	// The chrome bands — title bar and top accessory — and the central
	// column's horizontal extent are all decided above the document tabs, so this
	// early evaluation produces the same rectangles for them as the final one
	// below.  Everything that has to be positioned before the tab height is known
	// reads it, so no code here re-derives a sibling's coordinates by adding
	// heights together, which `window/CLAUDE.md` forbids in this function.
	const auto chromeLayout = workbench::CalculateWorkbenchLayout(layoutRequest);
	if( nullptr != hwndToolBar ){
		(void)PositionChildForFrame(hwndToolBar, chromeLayout.topAccessory.left,
			chromeLayout.topAccessory.top, chromeLayout.topAccessory.Width(), nToolBarHeight);
	}

	//@@@ From 2003.05.31 MIK
	//タブウインドウ追加に伴い，ファンクションキー表示位置も調整

	//タブウインドウ
	int nTabHeightBottom = 0;
	nTabWndHeight = 0;
	const bool showDocumentTabs = HasActiveEditorInput();
	if (m_cTabWnd.GetHwnd()) {
		::ShowWindow(m_cTabWnd.GetHwnd(), showDocumentTabs ? SW_SHOWNA : SW_HIDE);
	}
	if( m_cTabWnd.GetHwnd() && showDocumentTabs )
	{
		// タブ多段はSizeBox/ウィンドウ幅で高さが変わる可能性がある
		ETabPosition tabPosition = m_pShareData->m_Common.m_sTabBar.m_eTabPosition;
		bool bHidden = false;
		if( tabPosition == TabPosition_Top ){
			// 上から下に移動するとゴミが表示されるので一度非表示にする
			if( m_cTabWnd.m_eTabPosition != TabPosition_None && m_cTabWnd.m_eTabPosition != TabPosition_Top ){
				bHidden = true;
				::ShowWindow( m_cTabWnd.GetHwnd(), SW_HIDE );
			}
			m_cTabWnd.SizeBox_ONOFF( false );
			::GetWindowRect( m_cTabWnd.GetHwnd(), &rc );
			nTabWndHeight = rc.bottom - rc.top;
			(void)PositionChildForFrame(m_cTabWnd.GetHwnd(), chromeLayout.documentTabs.left,
				chromeLayout.documentTabs.top, chromeLayout.documentTabs.Width(), nTabWndHeight);
			m_cTabWnd.OnSize();
			::GetWindowRect( m_cTabWnd.GetHwnd(), &rc );
			if( nTabWndHeight != rc.bottom - rc.top ){
				nTabWndHeight = rc.bottom - rc.top;
				(void)PositionChildForFrame(m_cTabWnd.GetHwnd(), chromeLayout.documentTabs.left,
					chromeLayout.documentTabs.top, chromeLayout.documentTabs.Width(), nTabWndHeight);
			}
		}else if( tabPosition == TabPosition_Bottom ){
			// 上から下に移動するとゴミが表示されるので一度非表示にする
			if( m_cTabWnd.m_eTabPosition != TabPosition_None && m_cTabWnd.m_eTabPosition != TabPosition_Bottom ){
				bHidden = true;
				ShowWindow( m_cTabWnd.GetHwnd(), SW_HIDE );
			}
			bool	bSizeBox = true;
			if( nullptr != m_cStatusBar.GetStatusHwnd() ){
				bSizeBox = false;
			}
			if (1 == m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place &&
				m_cFuncKeyWnd.GetHwnd()) {
					bSizeBox = false;
			}
			if( wParam == SIZE_MAXIMIZED ){
				bSizeBox = false;
			}
			m_cTabWnd.SizeBox_ONOFF( bSizeBox );
			::GetWindowRect( m_cTabWnd.GetHwnd(), &rc );
			nTabWndHeight = rc.bottom - rc.top;
			(void)PositionChildForFrame(m_cTabWnd.GetHwnd(), 0,
				cy - nFuncKeyWndHeight - nStatusBarHeight - nTabWndHeight, cx, nTabWndHeight);
			m_cTabWnd.OnSize();
			::GetWindowRect( m_cTabWnd.GetHwnd(), &rc );
			if( nTabWndHeight != rc.bottom - rc.top ){
				nTabWndHeight = rc.bottom - rc.top;
				(void)PositionChildForFrame(m_cTabWnd.GetHwnd(), 0,
					cy - nFuncKeyWndHeight - nStatusBarHeight - nTabWndHeight, cx, nTabWndHeight);
			}
			nTabHeightBottom = rc.bottom - rc.top;
			nTabWndHeight = 0;
			bMiniMapSizeBox = false;
		}
		if( bHidden ){
			::ShowWindow( m_cTabWnd.GetHwnd(), SW_SHOW );
		}
		m_cTabWnd.m_eTabPosition = tabPosition;
	}

	//	2005.04.23 genta ファンクションキー非表示の時は移動しない
	if( m_cFuncKeyWnd.GetHwnd() != nullptr ){
		if( m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 )
		{	/* ファンクションキー表示位置／0:上 1:下 */
			// The top accessory band holds the toolbar and this bar in that order, so
			// this bar starts where the band starts plus the toolbar's own height.
			// `nCustomTitleHeight + nToolBarHeight` would be peer-coordinate
			// inference; use the committed chrome layout instead.
			(void)PositionChildForFrame(
				m_cFuncKeyWnd.GetHwnd(),
				chromeLayout.topAccessory.left,
				chromeLayout.topAccessory.top + nToolBarHeight,
				chromeLayout.topAccessory.Width(),
				nFuncKeyWndHeight );
		}
		else if( m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 1 )
		{	/* ファンクションキー表示位置／0:上 1:下 */
			(void)PositionChildForFrame(
				m_cFuncKeyWnd.GetHwnd(),
				0,
				cy - nFuncKeyWndHeight - nStatusBarHeight,
				cx,
				nFuncKeyWndHeight
			);

			bool	bSizeBox = true;
			if( nullptr != m_cStatusBar.GetStatusHwnd() ){
				bSizeBox = false;
			}
			if( wParam == SIZE_MAXIMIZED ){
				bSizeBox = false;
			}
			m_cFuncKeyWnd.SizeBox_ONOFF( bSizeBox );
			bMiniMapSizeBox = false;
		}
		if( !deferChildPaint && m_startupDrawState != StartupDrawState::Committing ){
			::UpdateWindow( m_cFuncKeyWnd.GetHwnd() );	// 2006.06.17 ryoji 即時描画でちらつきを減らす
		}
	}

	layoutRequest.documentTabsHeightPixels = nTabWndHeight;
	layoutRequest.bottomAccessoryHeightPixels = nTabHeightBottom
		+ (m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 1 ? nFuncKeyWndHeight : 0);
	auto layout = workbench::CalculateWorkbenchLayout(layoutRequest);
	if (m_resizingWorkbenchPanel != nullptr) {
		// Persist the pure layout result, never a child HWND sampled later during
		// mouse-up. Native children can still be between geometry and paint commits
		// at that boundary; reading one back made an intermediate/complement width
		// authoritative and caused the Side Bar to jump after release.
		const auto edge = m_resizingWorkbenchPanel->GetEdge();
		// A physical pointer coordinate is not always exactly representable as an
		// integer DIP. Converge the live frame to the persisted DIP now; otherwise
		// mouse-up reprojects it one pixel away and ClearType text visibly twitches.
		for (int attempt = 0; attempt < 2; ++attempt) {
			const int actualPixels = edge == workbench::WorkbenchEdge::Bottom
				? layout.bottomPane.Height()
				: edge == workbench::WorkbenchEdge::Right
					? layout.rightPane.Width()
					: layout.leftPane.Width();
			const int snappedDip = PixelsToDip(actualPixels, layoutRequest.dpi);
			if (snappedDip == m_resizingWorkbenchPanel->GetPendingExtentDip()) break;
			m_resizingWorkbenchPanel->UpdateResize(snappedDip);
			switch (edge) {
			case workbench::WorkbenchEdge::Left:
				layoutRequest.leftPaneWidthDip = snappedDip; break;
			case workbench::WorkbenchEdge::Right:
				layoutRequest.rightPaneWidthDip = snappedDip; break;
			case workbench::WorkbenchEdge::Bottom:
				layoutRequest.bottomPaneHeightDip = snappedDip; break;
			}
			layout = workbench::CalculateWorkbenchLayout(layoutRequest);
		}
	}
	m_leftWorkbenchSplitter = ToWinRect(layout.leftSplitter);
	m_rightWorkbenchSplitter = ToWinRect(layout.rightSplitter);
	m_bottomWorkbenchSplitter = ToWinRect(layout.bottomSplitter);

	if (m_activityBar) m_activityBar->Layout(ToWinRect(layout.primaryActivityBar), layoutRequest.dpi);
	if (m_auxiliaryActivityBar) {
		m_auxiliaryActivityBar->Layout(ToWinRect(layout.secondaryActivityBar), layoutRequest.dpi);
	}
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->Layout(ToWinRect(layout.leftPane), layoutRequest.dpi);
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->Layout(ToWinRect(layout.rightPane), layoutRequest.dpi);
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->Layout(ToWinRect(layout.bottomPane), layoutRequest.dpi);
	if (m_cTabWnd.GetHwnd() && m_cTabWnd.m_eTabPosition == TabPosition_Top) {
		(void)PositionChildForFrame(m_cTabWnd.GetHwnd(), layout.documentTabs.left,
			layout.documentTabs.top, layout.documentTabs.Width(), layout.documentTabs.Height());
		m_cTabWnd.OnSize();
	}

	auto editorBounds = layout.editor;

	// The minimap belongs to the editor group, so the split is calculated over the
	// editor rectangle plus the minimap's own column and the minimap is placed
	// where that split leaves it. Positioning it from `layout.minimap` here would
	// pin it to the frame's right edge, on the far side of a sibling preview.
	const int minimapWidth = layout.minimap.Width();
	const int editorGroupLeft = layoutRequest.minimapOnLeft
		? editorBounds.left - minimapWidth : editorBounds.left;
	const int editorGroupRight = layoutRequest.minimapOnLeft
		? editorBounds.right : editorBounds.right + minimapWidth;
	const RECT minimapBounds = LayoutMarkdownPreview(editorGroupLeft, editorBounds.top,
		editorGroupRight, editorBounds.bottom, physicalDpi, minimapWidth,
		layoutRequest.minimapOnLeft);

	if( m_cMiniMapView.GetHwnd() ){
		::ShowWindow(m_cMiniMapView.GetHwnd(),
			layoutRequest.showMinimap && !m_pPrintPreview ? SW_SHOWNA : SW_HIDE);
		(void)PositionChildForFrame(m_cMiniMapView.GetHwnd(), minimapBounds.left,
			minimapBounds.top, std::max(0L, minimapBounds.right - minimapBounds.left),
			std::max(0L, minimapBounds.bottom - minimapBounds.top));
		if (layoutRequest.rightPane != workbench::WorkbenchPanelState::Hidden
			|| layoutRequest.bottomPane != workbench::WorkbenchPanelState::Hidden) {
			bMiniMapSizeBox = false;
		}
		m_cMiniMapView.SplitBoxOnOff(FALSE, FALSE, bMiniMapSizeBox);
	}
	// The visible Part boundary is one DIP, while VS Code exposes a four-DIP sash
	// hit target. These sibling overlays sit above adjacent child controls so all
	// four pixels receive the initial press instead of only the parent-owned line.
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->LayoutSash(m_leftWorkbenchSplitter);
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->LayoutSash(m_rightWorkbenchSplitter);
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->LayoutSash(m_bottomWorkbenchSplitter);
	if (m_customFrame) m_customFrame->LayoutResizeOverlays();
	//@@@ To 2003.05.31 MIK

	// A frame resize relocates every child the same way a sash commit does, so
	// it leaves the same stale bits behind: without this the watermark, the
	// welcome actions, and the status bar stay drawn at their previous
	// coordinates until something else happens to invalidate them. The same
	// applies to a part-visibility change, which reaches this function through
	// FinalizeWorkbenchPanelProjection.
	//
	// The invalidation is queued and no-erase. Persistent surface buffers retain
	// the previous complete frame until the replacement cohort is ready, avoiding
	// both an empty intermediate frame and UI-thread paint recursion.
	if (!m_resizingMarkdownPreview) {
		if (m_frameRuntimeState != nullptr
			&& m_frameRuntimeState->windowTransaction != nullptr) {
			auto& transaction = *m_frameRuntimeState->windowTransaction;
			using WindowRole = workbench::rendering::EFrameWindowSurfaceRole;
			const auto setProjection = [&transaction](const WindowRole role,
				const char* const hostId, const bool visible) noexcept {
				(void)transaction.SetProjection(
					workbench::rendering::FrameWindowSurfaceId(role), hostId, visible);
			};
			setProjection(WindowRole::ActivityBar, "workbench.parts.activitybar",
				(layout.primaryActivityBar.Width() > 0 && layout.primaryActivityBar.Height() > 0)
				|| (layout.secondaryActivityBar.Width() > 0
					&& layout.secondaryActivityBar.Height() > 0));
			setProjection(WindowRole::PrimarySideBar, "workbench.parts.sidebar",
				layoutRequest.leftPane != workbench::WorkbenchPanelState::Hidden);
			setProjection(WindowRole::SecondarySideBar, "workbench.parts.auxiliarybar",
				layoutRequest.rightPane != workbench::WorkbenchPanelState::Hidden);
			setProjection(WindowRole::Panel, "workbench.parts.panel",
				layoutRequest.bottomPane != workbench::WorkbenchPanelState::Hidden);
			setProjection(WindowRole::TitleAndMenu, "workbench.parts.titlebar", true);
			setProjection(WindowRole::Tabs, "workbench.parts.editor",
				showDocumentTabs && m_cTabWnd.GetHwnd() != nullptr);
			setProjection(WindowRole::StatusBar, "workbench.parts.statusbar",
				m_cStatusBar.GetStatusHwnd() != nullptr
					&& ::IsWindowVisible(m_cStatusBar.GetStatusHwnd()));
			setProjection(WindowRole::Editor, "workbench.parts.editor",
				HasActiveEditorInput() && !m_pPrintPreview);
			setProjection(WindowRole::MarkdownPreview, "workbench.parts.editor",
				m_markdownPreviewVisible && m_markdownPreview != nullptr
					&& m_markdownPreview->IsCreated());
			const bool terminalActive = layoutRequest.bottomPane
					!= workbench::WorkbenchPanelState::Hidden
				&& (m_workbenchRuntime != nullptr
					? IsBuiltinWorkbenchViewActive(workbench::layout::ids::view::Terminal)
					: m_pShareData->m_Common.m_sWorkbench.m_eActiveTool
						== WORKBENCH_TOOL_TERMINAL);
			setProjection(WindowRole::Terminal, "workbench.parts.panel", terminalActive);
			(void)transaction.BeginLayout();
		}
		RedrawWorkbenchFrameForCommittedLayout(true);
	}

	/* 印刷プレビューモードか */
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	if( !m_pPrintPreview ){
		return finishLayout(0L);
	}
	return finishLayout(m_pPrintPreview->OnSize(wParam, lParam));
}

/* WM_PAINT 描画処理 */
LRESULT CEditWnd::OnPaint(
	HWND			hwnd,	// handle of window
	UINT			uMsg,	// message identifier
	WPARAM			wParam,	// first message parameter
	LPARAM			lParam 	// second message parameter
)
{
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	/* 印刷プレビューモードか */
	if( !m_pPrintPreview ){
		PAINTSTRUCT		ps;
		const HDC dc = ::BeginPaint(hwnd, &ps);
		// The edit window has no class background brush because its client area is
		// normally covered by child Parts.  Live workbench resizing temporarily
		// exposes the old child bounds, though, and leaving those pixels untouched
		// preserves every previous splitter position as a vertical/horizontal trail.
		// Paint only the invalid parent area; WS_CLIPCHILDREN keeps the current Parts
		// out of this fill.
		if (!::IsRectEmpty(&ps.rcPaint)) {
			const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
				? theme::ThemeMode::Dark : theme::ThemeMode::Light;
			const auto backgroundBrush = ::CreateSolidBrush(
				theme::CThemeService::EffectivePalette(mode).canvas.ToColorRef());
			if (backgroundBrush != nullptr) {
				::FillRect(dc, &ps.rcPaint, backgroundBrush);
				::DeleteObject(backgroundBrush);
			}
		}
		if (m_customFrame) {
			m_customFrame->Paint(dc, ps.rcPaint);
		}
		if (m_markdownPreviewDivider.right > m_markdownPreviewDivider.left
			&& m_markdownPreviewDivider.bottom > m_markdownPreviewDivider.top) {
			const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
				? theme::ThemeMode::Dark : theme::ThemeMode::Light;
			const auto dividerBrush = ::CreateSolidBrush(theme::CThemeService::EffectivePalette(mode).border.ToColorRef());
			::FillRect(dc, &m_markdownPreviewDivider, dividerBrush);
			::DeleteObject(dividerBrush);
		}
		PaintWorkbenchSplitters(dc);
		::EndPaint( hwnd, &ps );
		return 0L;
	}
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	return m_pPrintPreview->OnPaint(hwnd, uMsg, wParam, lParam);
}

/* 印刷プレビュー 垂直スクロールバーメッセージ処理 WM_VSCROLL */
LRESULT CEditWnd::OnVScroll( WPARAM wParam, LPARAM lParam )
{
	/* 印刷プレビューモードか */
	if( !m_pPrintPreview ){
		return 0;
	}
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	return m_pPrintPreview->OnVScroll(wParam, lParam);
}

/* 印刷プレビュー 水平スクロールバーメッセージ処理 */
LRESULT CEditWnd::OnHScroll( WPARAM wParam, LPARAM lParam )
{
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	/* 印刷プレビューモードか */
	if( !m_pPrintPreview ){
		return 0;
	}
	return m_pPrintPreview->OnHScroll( wParam, lParam );
}

LRESULT CEditWnd::OnLButtonDown( [[maybe_unused]] WPARAM wParam, LPARAM lParam )
{
	const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
	if (auto* host = HitTestWorkbenchSplitter(point); host != nullptr) {
		m_resizingWorkbenchPanel = host;
		m_workbenchResizeOrigin = point;
		m_workbenchResizeInitialExtentDip = host->GetExtentDip();
		host->BeginResize();
		::SetCapture(GetHwnd());
		::SetCursor(::LoadCursor(nullptr,
			host->GetEdge() == workbench::WorkbenchEdge::Bottom ? IDC_SIZENS : IDC_SIZEWE));
		return 0;
	}

	if (HitTestMarkdownPreviewDivider(point)) {
		m_markdownPreviewResizeWidthDip = m_markdownPreviewWidthDip;
		m_resizingMarkdownPreview = true;
		::SetCapture(GetHwnd());
		::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
		return 0;
	}

	//by 鬼(2) キャプチャして押されたら非クライアントでもこっちに来る
	if(m_IconClicked != icNone)
		return 0;

	m_ptDragPosOrg.x = LOWORD(lParam);	// horizontal position of cursor
	m_ptDragPosOrg.y = HIWORD(lParam);	// vertical position of cursor
	m_bDragMode      = true;
	SetCapture( GetHwnd() );

	return 0;
}

LRESULT CEditWnd::OnLButtonUp( [[maybe_unused]] WPARAM wParam, [[maybe_unused]] LPARAM lParam )
{
	if (m_resizingWorkbenchPanel != nullptr) {
		auto* host = m_resizingWorkbenchPanel;
		m_resizingWorkbenchPanel = nullptr;
		// OnSize2 already fed the clamped pure-layout extent back into this host.
		// HWND geometry is presentation output and must never become model truth.
		const bool committed = host->CommitResize();
		const bool projectionRequired = committed || m_workbenchLayoutProjectionDeferred;
		m_workbenchLayoutProjectionDeferred = false;
		if (projectionRequired && m_workbenchRuntime != nullptr
			&& !ApplyCurrentWorkbenchLayoutState(false, true)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: committed resize projection failed.\n");
		}
		if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
		// CommitResize may clamp or reject the dragged extent, so this final
		// OnSize2 can move children after the last drag-step repaint. Commit one
		// complete frame unconditionally; SetWindowPos alone leaves the vacated
		// sibling areas valid and stale.
		RedrawWorkbenchFrameForCommittedLayout(true);
		return 0;
	}

	if (m_resizingMarkdownPreview) {
		CommitMarkdownPreviewResize();
		return 0;
	}

	//by 鬼 2002/04/18
	if(m_IconClicked != icNone)
	{
		if(m_IconClicked == icDown)
		{
			m_IconClicked = icClicked;
			//by 鬼(2) タイマー(IDは適当です)
			SetTimer(GetHwnd(), IDT_SYSMENU, GetDoubleClickTime(), nullptr);
		}
		return 0;
	}

	m_bDragMode = false;
//	MYTRACE( L"m_bDragMode = FALSE (OnLButtonUp)\n");
	ReleaseCapture();
	// The drag gesture has ended; queue one no-erase repaint.  Erasing the
	// entire frame here exposes an empty desktop-colored frame before the
	// retained surfaces repaint and is especially visible while the title bar
	// is being dragged.
	::InvalidateRect( GetHwnd(), nullptr, FALSE );
	return 0;
}

/*!	WM_MOUSEMOVE処理
	@date 2008.05.05 novice メモリリーク修正
*/
LRESULT CEditWnd::OnMouseMove( WPARAM wParam, LPARAM lParam )
{
	if (m_resizingWorkbenchPanel != nullptr) {
		const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
		const auto dpi = ::GetDpiForWindow(GetHwnd());
		int extent = m_workbenchResizeInitialExtentDip;
		switch (m_resizingWorkbenchPanel->GetEdge()) {
		case workbench::WorkbenchEdge::Left:
			extent += PixelsToDip(point.x - m_workbenchResizeOrigin.x, dpi);
			break;
		case workbench::WorkbenchEdge::Right:
			extent -= PixelsToDip(point.x - m_workbenchResizeOrigin.x, dpi);
			break;
		case workbench::WorkbenchEdge::Bottom:
			extent -= PixelsToDip(point.y - m_workbenchResizeOrigin.y, dpi);
			break;
		}
		m_resizingWorkbenchPanel->UpdateResize(extent);
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
		// Coalesce pointer samples through the normal paint queue. Child surfaces
		// retain their last complete buffers until this geometry is presented.
		::RedrawWindow(GetHwnd(), nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
		return 0;
	}

	if (m_resizingMarkdownPreview) {
		const POINT point{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
		// The requested width is clamped inside the pure layout calculator, so an
		// over-dragged pointer parks the divider at the limit rather than being
		// ignored, and the stored request stays whatever the pointer asked for.
		m_markdownPreviewResizeWidthDip = markdown::RequestedPreviewWidthDipFromPointer(
			m_markdownPreviewRegion.right, point.x, ::GetDpiForWindow(GetHwnd()));
		RelayoutForMarkdownPreviewDivider();
		return 0;
	}

	//by 鬼
	if(m_IconClicked != icNone)
	{
		//by 鬼(2) 一回押された時だけ
		if(m_IconClicked == icDown)
		{
			POINT pt{};
			::GetCursorPos(&pt); //スクリーン座標

			if (HTSYSMENU == ::SendMessageW(GetHwnd(), WM_NCHITTEST, 0, pt.x | (pt.y << 16))) return 0L;

			::ReleaseCapture();

			m_IconClicked = icNone;

			if (cxx::com_pointer<IDataObject> pDataObject; SUCCEEDED(GetDocument()->GetDataObject(&pDataObject))) {
				CDropSource drop(true);

				//移動禁止なので、戻り値を見ない
				drop.DoDragDrop(pDataObject, DROPEFFECT_COPY | DROPEFFECT_LINK);
			}
		}
		return 0;
	}

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	if (!m_pPrintPreview){
		return 0;
	}
	else {
		return m_pPrintPreview->OnMouseMove( wParam, lParam );
	}
}

LRESULT CEditWnd::OnSetCursor([[maybe_unused]] WPARAM wParam, LPARAM lParam)
{
	if (LOWORD(lParam) != HTCLIENT) return ::DefWindowProc(GetHwnd(), WM_SETCURSOR, wParam, lParam);
	POINT point{};
	if (!::GetCursorPos(&point) || !::ScreenToClient(GetHwnd(), &point)) {
		return ::DefWindowProc(GetHwnd(), WM_SETCURSOR, wParam, lParam);
	}
	if (m_resizingMarkdownPreview || HitTestMarkdownPreviewDivider(point)) {
		::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
		return TRUE;
	}
	auto* host = m_resizingWorkbenchPanel != nullptr
		? m_resizingWorkbenchPanel : HitTestWorkbenchSplitter(point);
	if (host == nullptr) return ::DefWindowProc(GetHwnd(), WM_SETCURSOR, wParam, lParam);
	::SetCursor(::LoadCursor(nullptr,
		host->GetEdge() == workbench::WorkbenchEdge::Bottom ? IDC_SIZENS : IDC_SIZEWE));
	return TRUE;
}

LRESULT CEditWnd::OnCaptureChanged(LPARAM lParam)
{
	if (reinterpret_cast<HWND>(lParam) != GetHwnd()) {
		CancelWorkbenchResize();
		CancelMarkdownPreviewResize();
	}
	return 0;
}

workbench::CWorkbenchPanelHost* CEditWnd::HitTestWorkbenchSplitter(POINT point) const noexcept
{
	const int sashHitSize = std::max(1, ::MulDiv(4, static_cast<int>(::GetDpiForWindow(GetHwnd())), 96));
	const auto containsVerticalSash = [point, sashHitSize](RECT rect) noexcept {
		if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
		const int extra = std::max(0, sashHitSize - static_cast<int>(rect.right - rect.left));
		rect.left -= extra / 2;
		rect.right += extra - extra / 2;
		return ContainsPoint(rect, point);
	};
	const auto containsHorizontalSash = [point, sashHitSize](RECT rect) noexcept {
		if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
		const int extra = std::max(0, sashHitSize - static_cast<int>(rect.bottom - rect.top));
		rect.top -= extra / 2;
		rect.bottom += extra - extra / 2;
		return ContainsPoint(rect, point);
	};
	if (m_leftWorkbenchPanel && containsVerticalSash(m_leftWorkbenchSplitter)) return m_leftWorkbenchPanel.get();
	if (m_rightWorkbenchPanel && containsVerticalSash(m_rightWorkbenchSplitter)) return m_rightWorkbenchPanel.get();
	if (m_bottomWorkbenchPanel && containsHorizontalSash(m_bottomWorkbenchSplitter)) return m_bottomWorkbenchPanel.get();
	return nullptr;
}

void CEditWnd::CancelWorkbenchResize()
{
	if (m_resizingWorkbenchPanel == nullptr) return;
	auto* host = m_resizingWorkbenchPanel;
	m_resizingWorkbenchPanel = nullptr;
	host->CancelResize();
	if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
	const bool projectionRequired = std::exchange(m_workbenchLayoutProjectionDeferred, false);
	if (projectionRequired && m_workbenchRuntime != nullptr
		&& !ApplyCurrentWorkbenchLayoutState(false, true)) {
		::OutputDebugStringW(L"Sakura Editor NEXT: deferred resize projection failed.\n");
	}
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	(void)OnSize2(m_nWinSizeType,
		MAKELONG(client.right - client.left, client.bottom - client.top), false);
}

void CEditWnd::PaintWorkbenchSplitters(HDC dc) const
{
	if (dc == nullptr) return;
	const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark : theme::ThemeMode::Light;
	const auto palette = theme::CThemeService::EffectivePalette(mode);
	const HBRUSH brush = ::CreateSolidBrush(palette.border.ToColorRef());
	if (brush == nullptr) return;
	for (const RECT& rect : { m_leftWorkbenchSplitter, m_rightWorkbenchSplitter, m_bottomWorkbenchSplitter }) {
		if (rect.right > rect.left && rect.bottom > rect.top) ::FillRect(dc, &rect, brush);
	}
	::DeleteObject(brush);
}

HWND CEditWnd::HoveredScrollTarget( LPARAM lParam ) const noexcept
{
	if (m_pPrintPreview) return nullptr;
	const POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	HWND hovered = ::WindowFromPoint(screen);
	// Input-only overlays (the sash targets, the frame resize band) sit above the
	// content they cover, so ask their parent instead of swallowing the wheel.
	while (hovered != nullptr && hovered != GetHwnd()
		&& (::GetWindowLongW(hovered, GWL_STYLE) & WS_CHILD) != 0
		&& (::GetWindowLongW(hovered, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) {
		hovered = ::GetParent(hovered);
	}
	if (hovered == nullptr || hovered == GetHwnd()) return nullptr;
	if (::GetWindowThreadProcessId(hovered, nullptr) != ::GetCurrentThreadId()) return nullptr;
	if (::GetAncestor(hovered, GA_ROOT) != GetHwnd()) return nullptr;
	// The tab strip owns the historical wheel-to-next/previous-window behavior.
	// Keep its root and all descendants on OnMouseWheel -> DoMouseWheel so the
	// m_bChgWndByWheel path is not bypassed by pointer-following forwarding.
	const HWND tabWindow = m_cTabWnd.GetHwnd();
	const HWND tabControl = m_cTabWnd.m_hwndTab;
	for (HWND ancestor = hovered; ancestor != nullptr && ancestor != GetHwnd();
		ancestor = ::GetParent(ancestor)) {
		if (ancestor == tabWindow || ancestor == tabControl) return nullptr;
	}
	// The editor panes keep the historical path, which owns zoom, the caret, and
	// the split-pane dispatch that a bare WM_MOUSEWHEEL forward cannot reproduce.
	for (const auto& view : m_pcEditViewArr) {
		if (view != nullptr && view->GetHwnd() == hovered) return nullptr;
	}
	return hovered;
}

LRESULT CEditWnd::OnMouseWheel( WPARAM wParam, LPARAM lParam )
{
	if( m_pPrintPreview ){
		return m_pPrintPreview->OnMouseWheel( wParam, lParam );
	}
	return Views_DispatchEvent( GetHwnd(), WM_MOUSEWHEEL, wParam, lParam );
}

/** マウスホイール処理

	@date 2007.10.16 ryoji OnMouseWheel()から処理抜き出し
*/
BOOL CEditWnd::DoMouseWheel( WPARAM wParam, LPARAM lParam )
{
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	/* 印刷プレビューモードか */
	if( !m_pPrintPreview ){
		// 2006.03.26 ryoji by assitance with John タブ上ならウィンドウ切り替え
		if( m_pShareData->m_Common.m_sTabBar.m_bChgWndByWheel && nullptr != m_cTabWnd.m_hwndTab )
		{
			POINT pt;
			pt.x = (short)LOWORD( lParam );
			pt.y = (short)HIWORD( lParam );
			int nDelta = (short)HIWORD( wParam );
			HWND hwnd = ::WindowFromPoint( pt );
			if( (hwnd == m_cTabWnd.m_hwndTab || hwnd == m_cTabWnd.GetHwnd()) )
			{
				// 現在開いている編集窓のリストを得る
				EditNode* pEditNodeArr;
				int nRowNum = CAppNodeManager::getInstance()->GetOpenedWindowArr( &pEditNodeArr, TRUE );
				if(  nRowNum > 0 )
				{
					// 自分のウィンドウを調べる
					int i, j;
					int nGroup = 0;
					for( i = 0; i < nRowNum; ++i )
					{
						if( GetHwnd() == pEditNodeArr[i].GetHwnd() )
						{
							nGroup = pEditNodeArr[i].m_nGroup;
							break;
						}
					}
					if( i < nRowNum )
					{
						if( nDelta < 0 )
						{
							// 次のウィンドウ
							for( j = i + 1; j < nRowNum; ++j )
							{
								if( nGroup == pEditNodeArr[j].m_nGroup )
									break;
							}
							if( j >= nRowNum )
							{
								for( j = 0; j < i; ++j )
								{
									if( nGroup == pEditNodeArr[j].m_nGroup )
										break;
								}
							}
						}
						else
						{
							// 前のウィンドウ
							for( j = i - 1; j >= 0; --j )
							{
								if( nGroup == pEditNodeArr[j].m_nGroup )
									break;
							}
							if( j < 0 )
							{
								for( j = nRowNum - 1; j > i; --j )
								{
									if( nGroup == pEditNodeArr[j].m_nGroup )
										break;
								}
							}
						}

						/* 次の（or 前の）ウィンドウをアクティブにする */
						if( i != j )
							ActivateFrameWindow( pEditNodeArr[j].GetHwnd() );
					}

					delete []pEditNodeArr;
				}
				return TRUE;	// 処理した
			}
		}
		return FALSE;	// 処理しなかった
	}
	return FALSE;	// 処理しなかった
}

/* 印刷ページ設定
	印刷プレビュー時にも、そうでないときでも呼ばれる可能性がある。
*/
BOOL CEditWnd::OnPrintPageSetting( void )
{
	/* 印刷設定（CANCEL押したときに破棄するための領域） */
	CDlgPrintSetting	cDlgPrintSetting;
	BOOL				bRes;
	int					nCurrentPrintSetting;
	int					nLineNumberColumns;

	nCurrentPrintSetting = GetDocument()->m_cDocType.GetDocumentAttribute().m_nCurrentPrintSetting;
	if( m_pPrintPreview ){
		nLineNumberColumns = GetActiveView().GetTextArea().DetectWidthOfLineNumberArea_calculate(m_pPrintPreview->m_pLayoutMgr_Print); // 印刷プレビュー時は文書の桁数 2013.5.10 aroka
	}else{
		nLineNumberColumns = 3; // ファイルメニューからの設定時は最小値 2013.5.10 aroka
	}

	bRes = cDlgPrintSetting.DoModal(
		G_AppInstance(),
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		GetHwnd(),
		&nCurrentPrintSetting, /* 現在選択している印刷設定 */
		m_pShareData->m_PrintSettingArr, // 現在の設定はダイアログ側で保持する 2013.5.1 aroka
		nLineNumberColumns // 行番号表示用に桁数を渡す 2013.5.10 aroka
	);

	if( FALSE != bRes ){
		bool bChangePrintSettingNo = false;
		/* 現在選択されているページ設定の番号が変更されたか */
		if( GetDocument()->m_cDocType.GetDocumentAttribute().m_nCurrentPrintSetting != nCurrentPrintSetting )
		{
			/* 変更フラグ(タイプ別設定) */
			STypeConfig* type = new STypeConfig();
			CDocTypeManager().GetTypeConfig( GetDocument()->m_cDocType.GetDocumentType(), *type );
			type->m_nCurrentPrintSetting = nCurrentPrintSetting;
			CDocTypeManager().SetTypeConfig( GetDocument()->m_cDocType.GetDocumentType(), *type );
			delete type;
			GetDocument()->m_cDocType.GetDocumentAttributeWrite().m_nCurrentPrintSetting = nCurrentPrintSetting; // 今の設定にも反映
			CAppNodeGroupHandle(0).SendMessageToAllEditors(
				MYWM_CHANGESETTING,
				(WPARAM)GetDocument()->m_cDocType.GetDocumentType().GetIndex(),
				(LPARAM)PM_CHANGESETTING_TYPE,
				CEditWnd::getInstance()->GetHwnd()
			);
			bChangePrintSettingNo = true;
		}

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		//	印刷プレビュー時のみ。
		if ( m_pPrintPreview ){
			/* 現在の印刷設定 */
			// 2013.08.27 印刷設定番号が変更された時に対応できていなかった
			if( bChangePrintSettingNo ){
				m_pPrintPreview->SetPrintSetting( &m_pShareData->m_PrintSettingArr[GetDocument()->m_cDocType.GetDocumentAttribute().m_nCurrentPrintSetting] );
			}

			/* 印刷プレビュー スクロールバー初期化 */
			//m_pPrintPreview->InitPreviewScrollBar();

			/* 印刷設定の反映 */
			// m_pPrintPreview->OnChangePrintSetting( );

			//::InvalidateRect( GetHwnd(), NULL, TRUE );
		}
		CAppNodeGroupHandle(0).SendMessageToAllEditors(
			MYWM_CHANGESETTING,
			(WPARAM)0,
			(LPARAM)PM_PRINTSETTING,
			CEditWnd::getInstance()->GetHwnd()
		);
	}
	return bRes;
}

///////////////////////////// by 鬼

LRESULT CEditWnd::OnNcLButtonDown(WPARAM wp, LPARAM lp)
{
	LRESULT Result;
	if(wp == HTSYSMENU)
	{
		SetCapture(GetHwnd());
		m_IconClicked = icDown;
		Result = 0;
	}
	else
		Result = DefWindowProc(GetHwnd(), WM_NCLBUTTONDOWN, wp, lp);

	return Result;
}

LRESULT CEditWnd::OnNcLButtonUp(WPARAM wp, LPARAM lp)
{
	LRESULT Result;
	if(m_IconClicked != icNone)
	{
		//念のため
		ReleaseCapture();
		m_IconClicked = icNone;
		Result = 0;
	}
	else if(wp == HTSYSMENU)
		Result = 0;
	else{
		//	2004.05.23 Moca メッセージミス修正
		//	フレームのダブルクリック時後にウィンドウサイズ
		//	変更モードなっていた
		Result = DefWindowProc(GetHwnd(), WM_NCLBUTTONUP, wp, lp);
	}

	return Result;
}

LRESULT CEditWnd::OnLButtonDblClk(WPARAM wp, LPARAM lp) //by 鬼(2)
{
	LRESULT Result;
	if(m_IconClicked != icNone)
	{
		ReleaseCapture();
		m_IconClicked = icDoubleClicked;

		SendMessage(GetHwnd(), WM_SYSCOMMAND, SC_CLOSE, 0);

		Result = 0;
	}
	else {
		//	2004.05.23 Moca メッセージミス修正
		Result = DefWindowProc(GetHwnd(), WM_LBUTTONDBLCLK, wp, lp);
	}

	return Result;
}

/*! ドロップダウンメニュー(開く) */	//@@@ 2002.06.15 MIK
int	CEditWnd::CreateFileDropDownMenu( HWND hwnd )
{
	int			nId;
	HMENU		hMenu;
	HMENU		hMenuPopUp;
	POINT		po;
	RECT		rc;
	int			nIndex;

	// メニュー表示位置を決める	// 2007.03.25 ryoji
	// ※ TBN_DROPDOWN 時の NMTOOLBAR::iItem や NMTOOLBAR::rcButton にはドロップダウンメニュー(開く)ボタンが
	//    複数あるときはどれを押した時も１個目のボタン情報が入るようなのでマウス位置からボタン位置を求める
	::GetCursorPos( &po );
	::ScreenToClient( hwnd, &po );
	nIndex = ApiWrap::Toolbar_Hittest( hwnd, &po );
	if( nIndex < 0 ){
		return 0;
	}
	ApiWrap::Toolbar_GetItemRect( hwnd, nIndex, &rc );
	po.x = rc.left;
	po.y = rc.bottom;
	::ClientToScreen( hwnd, &po );
	GetMonitorWorkRect( po, &rc );
	if( po.x < rc.left )
		po.x = rc.left;
	if( po.y < rc.top )
		po.y = rc.top;

	m_cMenuDrawer.ResetContents();

	/* 空メニューを作る */
	hMenu = ::CreatePopupMenu();

	/* MRUリストのファイルのリストをメニューにする */
	const CMRUFile cMRU;
	hMenu = cMRU.CreateMenu( hMenu, &m_cMenuDrawer );
	if( cMRU.MenuLength() > 0 )
	{
		m_cMenuDrawer.MyAppendMenuSep( hMenu, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr, FALSE );
	}

	/* 最近使ったフォルダーのメニューを作成 */
	const CMRUFolder cMRUFolder;
	hMenuPopUp = cMRUFolder.CreateMenu( &m_cMenuDrawer );
	if ( cMRUFolder.MenuLength() > 0 )
	{
		//	アクティブ
		m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hMenuPopUp, LS(F_FOLDER_USED_RECENTLY), L"" );
	}
	else
	{
		//	非アクティブ
		m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING | MF_POPUP | MF_GRAYED, (UINT_PTR)hMenuPopUp, LS(F_FOLDER_USED_RECENTLY), L"" );
	}

	m_cMenuDrawer.MyAppendMenuSep( hMenu, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr, FALSE );

	/* 履歴の管理のメニューを作成 */
	m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_FAVORITE, L"", L"M", FALSE );
	m_cMenuDrawer.MyAppendMenuSep( hMenu, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr, FALSE );

	m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_FILENEW, L"", L"N", FALSE );
	m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_FILENEW_NEWWINDOW, L"", L"M", FALSE );
	m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, F_FILEOPEN, L"", L"O", FALSE );

	nId = ::TrackPopupMenu(
		hMenu,
		TPM_TOPALIGN
		| TPM_LEFTALIGN
		| TPM_RETURNCMD
		| TPM_LEFTBUTTON
		,
		po.x,
		po.y,
		0,
		GetHwnd(),	// 2009.02.03 ryoji アクセスキー有効化のため hwnd -> GetHwnd() に変更
		nullptr
	);

	::DestroyMenu( hMenu );

	return nId;
}

/*!
	@brief ウィンドウのアイコン設定

	指定されたアイコンをウィンドウに設定する．
	以前のアイコンは破棄する．

	@param hIcon [in] 設定するアイコン
	@param flag [in] アイコン種別．ICON_BIGまたはICON_SMALL.
	@author genta
	@date 2002.09.10
*/
void CEditWnd::SetWindowIcon(HICON hIcon, int flag) const
{
	if (const auto hOld = (HICON)::SendMessageW(GetHwnd(), WM_SETICON, flag, LPARAM(hIcon));
		hOld != nullptr ){
		::DestroyIcon( hOld );
	}
}

/*!
	標準アイコンの取得

	@param hIconBig   [out] 大きいアイコンのハンドル
	@param hIconSmall [out] 小さいアイコンのハンドル

	@author genta
	@date 2002.09.10
	@date 2002.12.02 genta 新設した共通関数を使うように
*/
void CEditWnd::GetDefaultIcon( HICON* hIconBig, HICON* hIconSmall ) const
{
	*hIconBig   = GetAppIcon( G_AppInstance(), ICON_DEFAULT_APP, FN_APP_ICON, false );
	*hIconSmall = GetAppIcon( G_AppInstance(), ICON_DEFAULT_APP, FN_APP_ICON, true );
}

/*!
	アイコンの取得

	指定されたファイル名に対応するアイコン(大・小)を取得して返す．

	@param szFile     [in] ファイル名
	@param hIconBig   [out] 大きいアイコンのハンドル
	@param hIconSmall [out] 小さいアイコンのハンドル

	@retval true 関連づけられたアイコンが見つかった
	@retval false 関連づけられたアイコンが見つからなかった

	@author genta
	@date 2002.09.10
*/
bool CEditWnd::GetRelatedIcon(const WCHAR* szFile, HICON* hIconBig, HICON* hIconSmall) const
{
	if( nullptr != szFile && szFile[0] != L'\0' ){
		WCHAR szExt[_MAX_EXT];
		WCHAR FileType[1024];

		// (.で始まる)拡張子の取得
		_wsplitpath_s( szFile, nullptr, 0, nullptr, 0, nullptr, 0, szExt, std::size(szExt) );

		if( ReadRegistry(HKEY_CLASSES_ROOT, szExt, nullptr, FileType, int(std::size(FileType)) - 13)){
			wcscat( FileType, L"\\DefaultIcon" );
			if( ReadRegistry(HKEY_CLASSES_ROOT, FileType, nullptr, nullptr, 0)){
				// 関連づけられたアイコンを取得する
				SHFILEINFO shfi;
				SHGetFileInfo( szFile, 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_LARGEICON );
				*hIconBig = shfi.hIcon;
				SHGetFileInfo( szFile, 0, &shfi, sizeof(shfi), SHGFI_ICON | SHGFI_SMALLICON );
				*hIconSmall = shfi.hIcon;
				return true;
			}
		}
	}

	//	標準のアイコンを返す
	GetDefaultIcon( hIconBig, hIconSmall );
	return false;
}

/*
	@brief メニューバー表示用フォントの初期化

	メニューバー表示用フォントの初期化を行う．

	@date 2002.12.04 CEditViewのコンストラクタから移動
*/
void CEditWnd::InitMenubarMessageFont(void)
{
	TEXTMETRIC	tm;
	LOGFONT		lf;

	/* LOGFONTの初期化 */
	memset_raw( &lf, 0, sizeof( lf ) );
	lf.lfHeight			= DpiPointsToPixels(-9);	// 2009.10.01 ryoji 高DPI対応（ポイント数から算出）
	lf.lfWidth			= 0;
	lf.lfEscapement		= 0;
	lf.lfOrientation	= 0;
	lf.lfWeight			= 400;
	lf.lfItalic			= 0x0;
	lf.lfUnderline		= 0x0;
	lf.lfStrikeOut		= 0x0;
	lf.lfCharSet		= 0x80;
	lf.lfOutPrecision	= 0x3;
	lf.lfClipPrecision	= 0x2;
	lf.lfQuality		= 0x1;
	lf.lfPitchAndFamily	= 0x31;
	wcscpy( lf.lfFaceName, L"ＭＳ ゴシック" );
	m_hFontCaretPosInfo = ::CreateFontIndirect( &lf );

	MemDcHolder hdc = ::CreateCompatibleDC(nullptr);
	SelectionHolder hFontOld{ hdc };
	hFontOld = ::SelectObject( hdc, m_hFontCaretPosInfo );
	::GetTextMetrics( hdc, &tm );
	m_nCaretPosInfoCharWidth = tm.tmAveCharWidth;
	m_nCaretPosInfoCharHeight = tm.tmHeight;
}

/*
	@brief メニューバーにメッセージを表示する

	事前にメニューバー表示用フォントが初期化されていなくてはならない．
	指定できる文字数は最大30文字．それ以上の場合はうち切って表示する．

	@author genta
	@date 2002.12.04
*/
void CEditWnd::PrintMenubarMessage( const WCHAR* msg )
{
	const auto hWnd = GetHwnd();

	if( nullptr == GetMainMenuHandle() )	// 2007.03.08 ryoji 追加
		return;

	POINT	po,poFrame;
	RECT	rc,rcFrame;
	int		nStrLen;

	// msg == NULL のときは以前の m_pszMenubarMessage で再描画
	if( msg ){
		auto len = int(wcslen(msg));
		wcsncpy( m_pszMenubarMessage, msg, MENUBAR_MESSAGE_MAX_LEN );
		if( len < MENUBAR_MESSAGE_MAX_LEN ){
			wmemset( m_pszMenubarMessage + len, L' ', MENUBAR_MESSAGE_MAX_LEN - len );	//  null終端は不要
		}
	}

	WindowDcHolder hdc{ hWnd };
	hdc = ::GetWindowDC(hWnd);
	SelectionHolder hFontOld{ hdc };
	poFrame.x = 0;
	poFrame.y = 0;
	::ClientToScreen( GetHwnd(), &poFrame );
	::GetWindowRect( GetHwnd(), &rcFrame );
	po.x = rcFrame.right - rcFrame.left;
	po.y = poFrame.y - rcFrame.top;
	hFontOld = ::SelectObject( hdc, m_hFontCaretPosInfo );
	nStrLen = MENUBAR_MESSAGE_MAX_LEN;
	rc.left = po.x - nStrLen * m_nCaretPosInfoCharWidth - ( ::GetSystemMetrics( SM_CXSIZEFRAME ) + 2 );
	rc.right = rc.left + nStrLen * m_nCaretPosInfoCharWidth + 2;
	rc.top = po.y - m_nCaretPosInfoCharHeight - 2;
	rc.bottom = rc.top + m_nCaretPosInfoCharHeight;
	::SetTextColor( hdc, DarkMode::getTextColor() );
	::SetBkColor( hdc, DarkMode::getDlgBackgroundColor());
	{
		const WCHAR* pchText = m_pszMenubarMessage;
		const ULONG cchText = nStrLen;
		const INT nMaxExtent = rc.right - rc.left;
		const DWORD dwFlags = ::GetFontLanguageInfo(hdc);
		INT vDx[MENUBAR_MESSAGE_MAX_LEN] = { 0 };
		WCHAR vGlyphs[(MENUBAR_MESSAGE_MAX_LEN * 3 / 2) + 16]; // エラーグリフの増分を加味した領域を確保

		GCP_RESULTS results = { sizeof(GCP_RESULTS) };
		results.lpDx = vDx;
		results.lpGlyphs = vGlyphs;
		results.nGlyphs = int(std::size(vGlyphs));
		results.nMaxFit = cchText;
		auto placement = ::GetCharacterPlacement(hdc, pchText, cchText, nMaxExtent, &results, dwFlags);

		if (placement != 0) {
			::ExtTextOut(hdc, rc.left, rc.top, ETO_CLIPPED | ETO_OPAQUE, &rc, m_pszMenubarMessage, nStrLen, vDx);
		}
	}
}

/*!
	@brief メッセージの表示

	指定されたメッセージをステータスバーに表示する．
	ステータスバーが非表示の場合はメニューバーの右端に表示する．

	@param msg [in] 表示するメッセージ
	@date 2002.01.26 hor 新規作成
	@date 2002.12.04 genta CEditViewより移動
*/
void CEditWnd::SendStatusMessage( const WCHAR* msg )
{
	if( nullptr == m_cStatusBar.GetStatusHwnd() ){
		// メニューバーへ
		PrintMenubarMessage( msg );
	}
	else{
		// ステータスバーへ
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, msg);
	}
}

/*! ファイル名変更通知

	@author MIK
	@date 2003.05.31 新規作成
	@date 2006.01.28 ryoji ファイル名、Grepモードパラメータを追加
*/
void CEditWnd::ChangeFileNameNotify(
	std::wstring_view tabCaption,
	std::wstring_view tabFilePath,
	bool bIsGrep
) const
{
	const auto pszTabCaption = std::data(tabCaption);
	const auto pszFilePath = std::data(tabFilePath);

	CRecentEditNode	cRecentEditNode;
	int nIndex = cRecentEditNode.FindItemByHwnd( GetHwnd() );
	bool changed = false;
	if( -1 != nIndex )
	{
		EditNode *p = cRecentEditNode.GetItem( nIndex );
		if( p )
		{
			decltype(p->m_szTabCaption) caption;
			wcsncpy_s(caption, std::size(caption), pszTabCaption, _TRUNCATE);
			if (wcscmp(caption, p->m_szTabCaption) != 0) {
				wcscpy_s(p->m_szTabCaption, caption);
				changed = true;
			}

			// 2006.01.28 ryoji ファイル名、Grepモード追加
			decltype(p->m_szFilePath) filePath;
			wcsncpy_s(filePath, std::size(filePath), pszFilePath, _TRUNCATE );
			if (wcscmp(filePath, p->m_szFilePath) != 0) {
				p->m_szFilePath = filePath;
				changed = true;
			}

			p->m_bIsGrep = bIsGrep;
		}
	}
	cRecentEditNode.Terminate();

	if (changed) {
		//ファイル名変更通知をブロードキャストする。
		int nGroup = CAppNodeManager::getInstance()->GetEditNode( GetHwnd() )->GetGroup();
		CAppNodeGroupHandle(nGroup).PostMessageToAllEditors(
			MYWM_TAB_WINDOW_NOTIFY,
			(WPARAM)TWNT_FILE,
			(LPARAM)GetHwnd(),
			GetHwnd()
		);
	}

	return;
}

/*! 常に手前に表示
	@param top  0:トグル動作 1:最前面 2:最前面解除 その他:なにもしない
	@date 2004.09.21 Moca
*/
void CEditWnd::WindowTopMost(int top) const
{
	if( 0 == top ){
		DWORD dwExstyle = (DWORD)::GetWindowLongPtr( GetHwnd(), GWL_EXSTYLE );
		if( dwExstyle & WS_EX_TOPMOST ){
			top = 2; // 最前面である -> 解除
		}else{
			top = 1;
		}
	}

	HWND hwndInsertAfter;
	switch( top ){
	case 1:
		hwndInsertAfter = HWND_TOPMOST;
		break;
	case 2:
		hwndInsertAfter = HWND_NOTOPMOST;
		break;
	default:
		return;
	}

	::SetWindowPos( GetHwnd(), hwndInsertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );

	// タブまとめ時は WS_EX_TOPMOST 状態を全ウィンドウで同期する	// 2007.05.18 ryoji
	if( m_pShareData->m_Common.m_sTabBar.m_bDispTabWnd && !m_pShareData->m_Common.m_sTabBar.m_bDispTabWndMultiWin ){
		HWND hwnd;
		int i;
		for( i = 0, hwndInsertAfter = GetHwnd(); i < m_pShareData->m_sNodes.m_nEditArrNum; i++ ){
			hwnd = m_pShareData->m_sNodes.m_pEditArr[i].GetHwnd();
			if( hwnd != GetHwnd() && IsSakuraMainWindow( hwnd ) ){
				if( !CAppNodeManager::IsSameGroup( GetHwnd(), hwnd ) )
					continue;
				::SetWindowPos( hwnd, hwndInsertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
				hwndInsertAfter = hwnd;
			}
		}
	}
}

// タイマーの更新を開始／停止する。 20060128 aroka
// ツールバー表示はタイマーにより更新しているが、
// アプリのフォーカスが外れたときにウィンドウからON/OFFを
//	呼び出してもらうことにより、余計な負荷を停止したい。
void CEditWnd::Timer_ONOFF(bool bStart) const
{
	if( nullptr != GetHwnd() ){
		if( bStart ){
			/* タイマーを起動 */
			if( 0 == ::SetTimer( GetHwnd(), IDT_TOOLBAR, 300, nullptr ) ){
				WarningMessage( GetHwnd(), LS(STR_ERR_DLGEDITWND03) );
			}
		} else {
			/* タCマーを削除 */
			::KillTimer( GetHwnd(), IDT_TOOLBAR );
		}
	}
	return;
}

/*!	@brief ウィンドウ一覧をポップアップ表示

	@param[in] bMousePos true: マウス位置にポップアップ表示する

	@date 2006.03.23 fon OnListBtnClickをベースに新規作成
	@date 2006.05.10 ryoji ポップアップ位置変更、その他微修正
	@date 2007.02.28 ryoji フルパス指定のパラメータを削除
	@date 2009.06.02 ryoji m_cMenuDrawerの初期化漏れ修正
*/
LRESULT CEditWnd::PopupWinList( bool bMousePos )
{
	POINT pt;

	// ポップアップ位置をアクティブビューの上辺に設定
	RECT rc;

	if( bMousePos ){
		::GetCursorPos( &pt );	// マウスカーソル位置に変更
	}
	else {
		::GetWindowRect( GetActiveView().GetHwnd(), &rc );
		pt.x = rc.right - 150;
		if( pt.x < rc.left )
			pt.x = rc.left;
		pt.y = rc.top;
	}

	// ウィンドウ一覧メニューをポップアップ表示する
	if( nullptr != m_cTabWnd.GetHwnd() ){
		m_cTabWnd.TabListMenu( pt );
	}
	else{
		m_cMenuDrawer.ResetContents();	// 2009.06.02 ryoji 追加
		EditNode*	pEditNodeArr;
		HMENU hMenu = ::CreatePopupMenu();	// 2006.03.23 fon
		int nRowNum = CAppNodeManager::getInstance()->GetOpenedWindowArr( &pEditNodeArr, TRUE );
		WinListMenu( hMenu, pEditNodeArr, nRowNum, TRUE );
		// メニューを表示する
		RECT rcWork;
		GetMonitorWorkRect( pt, &rcWork );	// モニタのワークエリア
		int nId = ::TrackPopupMenu( hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD,
									( pt.x > rcWork.left )? pt.x: rcWork.left,
									( pt.y < rcWork.bottom )? pt.y: rcWork.bottom,
									0, GetHwnd(), nullptr);
		delete [] pEditNodeArr;
		::DestroyMenu( hMenu );
		::SendMessage( GetHwnd(), WM_COMMAND, (WPARAM)nId, (LPARAM)nullptr );
	}

	return 0L;
}

/*! @brief 現在開いている編集窓のリストをメニューにする
	@date  2006.03.23 fon CEditWnd::InitMenuから移動。////が元からあるコメント。//>は追加コメントアウト。
	@date 2009.06.02 ryoji アイテム数が多いときはアクセスキーを 1-9,A-Z の範囲で再使用する（従来は36個未満を仮定）
*/
LRESULT CEditWnd::WinListMenu( HMENU hMenu, EditNode* pEditNodeArr, int nRowNum, [[maybe_unused]] BOOL bFull )
{
	int			i;
	WCHAR		szMenu[_MAX_PATH * 2 + 3];
	const EditInfo*	pfi;

	if( nRowNum > 0 ){
		CFileNameManager::getInstance()->TransformFileName_MakeCache();

		NONCLIENTMETRICS met;
		met.cbSize = CCSIZEOF_STRUCT(NONCLIENTMETRICS, lfMessageFont);
		::SystemParametersInfo(SPI_GETNONCLIENTMETRICS, met.cbSize, &met, 0);
		CDCFont dcFont(met.lfMenuFont, GetHwnd());
		for( i = 0; i < nRowNum; ++i ){
			/* トレイからエディタへの編集ファイル名要求通知 */
			::SendMessage( pEditNodeArr[i].GetHwnd(), MYWM_GETFILEINFO, 0, 0 );
////	From Here Oct. 4, 2000 JEPRO commented out & modified	開いているファイル数がわかるように履歴とは違って1から数える
			pfi = (EditInfo*)&m_pShareData->m_sWorkBuffer.m_EditInfo_MYWM_GETFILEINFO;
			CFileNameManager::getInstance()->GetMenuFullLabel_WinList( szMenu, int(std::size(szMenu)), pfi, pEditNodeArr[i].m_nId, i, dcFont.GetHDC() );
			m_cMenuDrawer.MyAppendMenu( hMenu, MF_BYPOSITION | MF_STRING, IDM_SELWINDOW + pEditNodeArr[i].m_nIndex, szMenu, L"" );
			if( GetHwnd() == pEditNodeArr[i].GetHwnd() ){
				::CheckMenuItem( hMenu, IDM_SELWINDOW + pEditNodeArr[i].m_nIndex, MF_BYCOMMAND | MF_CHECKED );
			}
		}
	}
	return 0L;
}

//2007.09.08 kobake 追加
//!ツールチップのテキストを取得
void CEditWnd::GetTooltipText(WCHAR* pszBuf, size_t nBufCount, UINT_PTR idFrom) const
{
	const auto nID = int(idFrom);

	// 機能文字列の取得 -> pszBuf
	GetDocument()->m_cFuncLookup.Funccode2Name( nID, pszBuf, nBufCount );
	size_t nLen = wcsnlen( pszBuf, nBufCount );

	// 機能に対応するキー名の取得(複数)
	CNativeW**	ppcAssignedKeyList;
	int nAssignedKeyNum = CKeyBind::GetKeyStrList(
		G_AppInstance(),
		m_pShareData->m_Common.m_sKeyBind.m_nKeyNameArrNum,
		m_pShareData->m_Common.m_sKeyBind.m_pKeyNameArr,
		&ppcAssignedKeyList,
		nID
	);

	// pszBufへ結合
	if( 0 < nAssignedKeyNum ){
		for( int j = 0; j < nAssignedKeyNum; ++j ){
			const WCHAR* pszKey = ppcAssignedKeyList[j]->GetStringPtr();
			auto nKeyLen = int(wcslen(pszKey));
			if ( nLen + 9 + nKeyLen < nBufCount ){
				wcscat_s( pszBuf, nBufCount, L"\n        " );
				wcscat_s( pszBuf, nBufCount, pszKey );
				nLen += 9 + nKeyLen;
			}
			delete ppcAssignedKeyList[j];
		}
		delete [] ppcAssignedKeyList;
	}
}

/*! タイマーの処理
	@date 2002.01.03 YAZAKI m_tbMyButtonなどをCShareDataからCMenuDrawerへ移動したことによる修正。
	@date 2003.08.29 wmlhq, ryoji nTimerCountの導入
	@date 2006.01.28 aroka ツールバー更新を OnToolbarTimerに移動した
	@date 2007.04.03 ryoji パラメータ無しにした
*/
void CEditWnd::OnEditTimer( void )
{
	//static	int	nLoopCount = 0; // wmlhq m_nTimerCountに移行
	// タイマーの呼び出し間隔を 500msに変更。300*10→500*6にする。 20060128 aroka
	IncrementTimerCount(6);
	UpdateMarkdownPreviewIfNeeded();
	if (m_workingCopyLifecycleBridge && !m_workingCopyBackendEffectInProgress) {
		(void)m_workingCopyLifecycleBridge->Flush(::GetTickCount64(), false);
	}

	// 2006.01.28 aroka ツールバー更新関連は OnToolbarTimerに移動した。

	//	Aug. 29, 2003 wmlhq, ryoji
	if( m_nTimerCount == 0 && GetCapture() == nullptr ){
		// ファイルのタイムスタンプのチェック処理
		GetDocument()->m_cAutoReloadAgent.CheckFileTimeStamp();

#if 0	// 2011.02.11 ryoji 書込禁止の監視を廃止（復活させるなら「更新の監視」付随ではなく別オプションにしてほしい）
		// ファイル書込可能のチェック処理
		if(GetDocument()->m_cAutoReloadAgent._ToDoChecking()){
			bool bOld = GetDocument()->m_cDocLocker.IsDocWritable();
			GetDocument()->m_cDocLocker.CheckWritable(false);
			if(bOld != GetDocument()->m_cDocLocker.IsDocWritable()){
				this->UpdateCaption();
			}
		}
#endif
	}

	GetDocument()->m_cAutoSaveAgent.CheckAutoSave();
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//                        ビュー管理                           //
// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

/*!
	CEditViewの画面バッファを削除
	@date 2007.09.09 Moca 新規作成
*/
void CEditWnd::Views_DeleteCompatibleBitmap()
{
	// CEditView群へ転送する
	for( int i = 0; i < GetAllViewCount(); i++ ){
		if( GetView(i).GetHwnd() ){
			GetView(i).DeleteCompatibleBitmap();
		}
	}
	m_cMiniMapView.DeleteCompatibleBitmap();
}

LRESULT CEditWnd::Views_DispatchEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch( msg ){
	case WM_ENTERMENULOOP:
	case WM_EXITMENULOOP:
		for( int i = 0; i < GetAllViewCount(); i++){
			GetView(i).DispatchEvent( hwnd, msg, wParam, lParam );
		}
		return 0L;
	default:
		return GetActiveView().DispatchEvent( hwnd, msg, wParam, lParam );
	}
}

/*
	分割指示。2つ目以降のビューを作る
	@param nViewCount  既存のビューも含めたビューの合計要求数
*/
bool CEditWnd::CreateEditViewBySplit(int nViewCount )
{
	if( m_nEditViewMaxCount < nViewCount ){
		return false;
	}
	if( GetAllViewCount() < nViewCount ){
		for( int i = GetAllViewCount(); i < nViewCount; i++ ){
			assert( nullptr == m_pcEditViewArr[i] );
			m_pcEditViewArr[i] = std::make_unique<CEditView>();
			m_pcEditViewArr[i]->Create( m_cSplitterWnd.GetHwnd(), GetDocument(), i, FALSE, false );
			m_pcEditViewArr[i]->SetIndentGuidesEnabled(m_indentGuidesEnabled);
		}
		m_nEditViewCount = nViewCount;
		std::vector<HWND> hWndArr;
		hWndArr.reserve(nViewCount + 1);
		for( int i = 0; i < nViewCount; i++ ){
			hWndArr.push_back( GetView(i).GetHwnd() );
		}
		hWndArr.push_back( nullptr );

		m_cSplitterWnd.SetChildWndArr( &hWndArr[0] );
	}
	return true;
}

/*
	ビューの再初期化
	@date 2010.04.10 CEditDoc::InitAllViewから移動
*/
void CEditWnd::InitAllViews()
{
	/* 先頭へカーソルを移動 */
	for( int i = 0; i < GetAllViewCount(); ++i ){
		//	Apr. 1, 2001 genta
		// 移動履歴の消去
		GetView(i).m_cHistory->Flush();

		/* 現在の選択範囲を非選択状態に戻す */
		GetView(i).GetSelectionInfo().DisableSelectArea( false );

		GetView(i).OnChangeSetting();
		GetView(i).GetCaret().MoveCursor( CLayoutPoint(0, 0), true );
		GetView(i).GetCaret().m_nCaretPosX_Prev = CLayoutInt(0);
	}
	m_cMiniMapView.OnChangeSetting();
}

void CEditWnd::Views_RedrawAll()
{
	//アクティブ以外を再描画してから…
	for( int v = 0; v < GetAllViewCount(); ++v ){
		if( m_nActivePaneIndex != v ){
			GetView(v).RedrawAll();
		}
	}
	m_cMiniMapView.RedrawAll();
	//アクティブを再描画
	GetActiveView().RedrawAll();
}

void CEditWnd::Views_Redraw()
{
	//アクティブ以外を再描画してから…
	for( int v = 0; v < GetAllViewCount(); ++v ){
		if( m_nActivePaneIndex != v )
			GetView(v).Redraw();
	}
	m_cMiniMapView.Redraw();
	//アクティブを再描画
	GetActiveView().Redraw();
}

/* アクティブなペインを設定 */
void  CEditWnd::SetActivePane( int nIndex )
{
	assert_warning( nIndex < GetAllViewCount() );
	DEBUG_TRACE( L"CEditWnd::SetActivePane %d\n", nIndex );

	/* アクティブなビューを切り替える */
	int nOldIndex = m_nActivePaneIndex;
	m_nActivePaneIndex = nIndex;
	m_pcEditView = m_pcEditViewArr[m_nActivePaneIndex].get();

	// フォーカスを移動する	// 2007.10.16 ryoji
	GetView(nOldIndex).GetCaret().m_cUnderLine.CaretUnderLineOFF( true );	//	2002/05/11 YAZAKI
	if( ::GetActiveWindow() == GetHwnd()
		&& ::GetFocus() != GetActiveView().GetHwnd() )
	{
		// ::SetFocus()でフォーカスを切り替える
		::SetFocus( GetActiveView().GetHwnd() );
	}else{
		// 2010.04.08 ryoji
		// 起動と同時にエディットボックスにフォーカスのあるダイアログを表示すると当該エディットボックスに
		// キャレットが表示されない問題(*1)を修正するのため、内部的な切り替えをするのはアクティブペインが
		// 切り替わるときだけにした。← CEditView::OnKillFocus()は自スレッドのキャレットを破棄するので
		// (*1) -GREPDLGオプションによるGREPダイアログ表示や開ファイル後自動実行マクロでのInputBox表示
		if( m_nActivePaneIndex != nOldIndex ){
			// アクティブでないときに::SetFocus()するとアクティブになってしまう
			// （不可視なら可視になる）ので内部的に切り替えるだけにする
			GetView(nOldIndex).OnKillFocus();
			GetActiveView().OnSetFocus();
		}
	}

	GetActiveView().RedrawAll();	/* フォーカス移動時の再描画 */

	m_cSplitterWnd.SetActivePane( nIndex );

	if( nullptr != m_cDlgFind.GetHwnd() ){		/* 「検索」ダイアログ */
		/* モードレス時：検索対象となるビューの変更 */
		m_cDlgFind.ChangeView( (LPARAM)&GetActiveView() );
	}
	if( nullptr != m_cDlgReplace.GetHwnd() ){	/* 「置換」ダイアログ */
		/* モードレス時：検索対象となるビューの変更 */
		m_cDlgReplace.ChangeView( (LPARAM)&GetActiveView() );
	}
	if( nullptr != m_cHokanMgr.GetHwnd() ){	/* 「入力補完」ダイアログ */
		/* モードレス時：検索対象となるビューの変更 */
		m_cHokanMgr.ChangeView( (LPARAM)&GetActiveView() );
	}
	if( nullptr != m_cDlgFuncList.GetHwnd() ){	/* 「アウトライン」ダイアログ */ // 20060201 aroka
		/* モードレス時：現在位置表示の対象となるビューの変更 */
		m_cDlgFuncList.ChangeView( (LPARAM)&GetActiveView() );
	}

	return;
}

/** すべてのペインの描画スイッチを設定する

	@param bDraw [in] 描画スイッチの設定値

	@date 2008.06.08 ryoji 新規作成
*/
bool CEditWnd::SetDrawSwitchOfAllViews( bool bDraw )
{
	bool bDrawSwitchOld = GetActiveView().GetDrawSwitch();

	for (int i = 0; i < GetAllViewCount(); ++i) {
		GetView(i).SetDrawSwitch( bDraw );
	}
	m_cMiniMapView.SetDrawSwitch( bDraw );
	return bDrawSwitchOld;
}

/** すべてのペインをRedrawする

	スクロールバーの状態更新はパラメータでフラグ制御 or 別関数にしたほうがいい？
	@date 2007.07.22 ryoji スクロールバーの状態更新を追加

	@param pcViewExclude [in] Redrawから除外するビュー
	@date 2008.06.08 ryoji pcViewExclude パラメータ追加
*/
void CEditWnd::RedrawAllViews( CEditView* pcViewExclude )
{
	for (int i = 0; i < GetAllViewCount(); ++i) {
		const auto pcView = &GetView(i);
		if( pcView == pcViewExclude )
			continue;
		if( i == m_nActivePaneIndex ){
			pcView->RedrawAll();
		}else{
			pcView->Redraw();
			pcView->AdjustScrollBars();
		}
	}
	m_cMiniMapView.Redraw();
	m_cMiniMapView.AdjustScrollBars();
}

void CEditWnd::Views_DisableSelectArea([[maybe_unused]] bool bRedraw)
{
	for( int i = 0; i < GetAllViewCount(); ++i ){
		if( GetView(i).GetSelectionInfo().IsTextSelected() ){	/* テキストが選択されているか */
			/* 現在の選択範囲を非選択状態に戻す */
			GetView(i).GetSelectionInfo().DisableSelectArea( true );
		}
	}
}

/* すべてのペインで、行番号表示に必要な幅を再設定する（必要なら再描画する） */
BOOL CEditWnd::DetectWidthOfLineNumberAreaAllPane( bool bRedraw )
{
	if( 1 == GetAllViewCount() ){
		return GetActiveView().GetTextArea().DetectWidthOfLineNumberArea( bRedraw );
	}
	// 以下2,4分割限定

	if ( GetActiveView().GetTextArea().DetectWidthOfLineNumberArea( bRedraw ) ){
		/* ActivePaneで計算したら、再設定・再描画が必要と判明した */
		if ( m_cSplitterWnd.GetAllSplitCols() == 2 ){
			GetView(m_nActivePaneIndex^1).GetTextArea().DetectWidthOfLineNumberArea( bRedraw );
		}
		else {
			//	表示されていないので再描画しない
			GetView(m_nActivePaneIndex^1).GetTextArea().DetectWidthOfLineNumberArea( false );
		}
		if ( m_cSplitterWnd.GetAllSplitRows() == 2 ){
			GetView(m_nActivePaneIndex^2).GetTextArea().DetectWidthOfLineNumberArea( bRedraw );
			if ( m_cSplitterWnd.GetAllSplitCols() == 2 ){
				GetView((m_nActivePaneIndex^1)^2).GetTextArea().DetectWidthOfLineNumberArea( bRedraw );
			}
		}
		else {
			GetView(m_nActivePaneIndex^2).GetTextArea().DetectWidthOfLineNumberArea( false );
			GetView((m_nActivePaneIndex^1)^2).GetTextArea().DetectWidthOfLineNumberArea( false );
		}
		return TRUE;
	}
	return FALSE;
}

/** 右端で折り返す
	@param nViewColNum	[in] 右端で折り返すペインの番号
	@retval 折り返しを変更したかどうか
	@date 2008.06.08 ryoji 新規作成
*/
BOOL CEditWnd::WrapWindowWidth( int nPane )
{
	// 右端で折り返す
	CKetaXInt nWidth = GetView(nPane).ViewColNumToWrapColNum( GetView(nPane).GetTextArea().m_nViewColNum );
	if( GetDocument()->m_cLayoutMgr.GetMaxLineKetas() != nWidth ){
		ChangeLayoutParam( false, GetDocument()->m_cLayoutMgr.GetTabSpaceKetas(), GetDocument()->m_cLayoutMgr.m_tsvInfo.m_nTsvMode, nWidth );
		ClearViewCaretPosInfo();
		return TRUE;
	}
	return FALSE;
}

/** 折り返し方法関連の更新
	@retval 画面更新したかどうか
	@date 2008.06.10 ryoji 新規作成
*/
BOOL CEditWnd::UpdateTextWrap( void )
{
	// この関数はコマンド実行ごとに処理の最終段階で利用する
	// （アンドゥ登録＆全ビュー更新のタイミング）
	if( GetDocument()->m_nTextWrapMethodCur == WRAP_WINDOW_WIDTH ){
		BOOL bWrap = WrapWindowWidth( 0 );	// 右端で折り返す
		return bWrap;	// 画面更新＝折り返し変更
	}
	return FALSE;	// 画面更新しなかった
}

/*!	レイアウトパラメータの変更

	具体的にはタブ幅と折り返し位置を変更する．
	現在のドキュメントのレイアウトのみを変更し，共通設定は変更しない．

	@date 2005.08.14 genta 新規作成
	@date 2008.06.18 ryoji レイアウト変更途中はカーソル移動の画面スクロールを見せない（画面のちらつき抑止）
*/
void CEditWnd::ChangeLayoutParam( bool bShowProgress, CKetaXInt nTabSize, int nTsvMode, CKetaXInt nMaxLineKetas )
{
	HWND		hwndProgress = nullptr;
	if( bShowProgress && nullptr != this ){ // TODO: Remove "this" check
		hwndProgress = m_cStatusBar.GetProgressHwnd();
		//	Status Barが表示されていないときはm_hwndProgressBar == NULL
	}

	if( hwndProgress ){
		::ShowWindow( hwndProgress, SW_SHOW );
	}

	//	座標の保存
	CLogicPointEx* posSave = SavePhysPosOfAllView();

	//	レイアウトの更新
	GetDocument()->m_cLayoutMgr.ChangeLayoutParam( nTabSize, nTsvMode, nMaxLineKetas );
	ClearViewCaretPosInfo();

	//	座標の復元
	//	レイアウト変更途中はカーソル移動の画面スクロールを見せない	// 2008.06.18 ryoji
	const bool bDrawSwitchOld = SetDrawSwitchOfAllViews( false );
	RestorePhysPosOfAllView( posSave );
	SetDrawSwitchOfAllViews( bDrawSwitchOld );

	for( int i = 0; i < GetAllViewCount(); i++ ){
		if( GetView(i).GetHwnd() ){
			// Each view paints its complete back buffer.  Queue the update without
			// asking USER32 for an erase pass so a layout change cannot flash a
			// blank frame before the retained content is published.
			InvalidateRect( GetView(i).GetHwnd(), nullptr, FALSE );
			GetView(i).AdjustScrollBars();	// 2008.06.18 ryoji
		}
	}
	if( m_cMiniMapView.GetHwnd() ){
		InvalidateRect( m_cMiniMapView.GetHwnd(), nullptr, FALSE );
		m_cMiniMapView.AdjustScrollBars();
	}
	GetActiveView().GetCaret().ShowCaretPosInfo();	// 2009.07.25 ryoji

	if( hwndProgress ){
		::ShowWindow( hwndProgress, SW_HIDE );
	}
}

/*!
	レイアウトの変更に先立って，全てのViewの座標を物理座標に変換して保存する．

	@return データを保存した配列へのポインタ

	@note 取得した値はレイアウト変更後にCEditWnd::RestorePhysPosOfAllViewへ渡す．
	渡し忘れるとメモリリークとなる．

	@date 2005.08.11 genta  新規作成
	@date 2007.09.06 kobake 戻り値をCLogicPoint*に変更
	@date 2011.12.28 CLogicPointをCLogicPointExに変更。改行より右側でも復帰できるように
*/
CLogicPointEx* CEditWnd::SavePhysPosOfAllView()
{
	const int NUM_OF_VIEW = GetAllViewCount();
	const int NUM_OF_POS = 6;

	CLogicPointEx* pptPosArray = new CLogicPointEx[NUM_OF_VIEW * NUM_OF_POS];

	for( int i = 0; i < NUM_OF_VIEW; ++i ){
		CLayoutPoint tmp = CLayoutPoint(CLayoutInt(0), GetView(i).m_pcTextArea->GetViewTopLine());
		if (const auto layoutLine = GetDocument()->m_cLayoutMgr.SearchLineByLayoutY(tmp.GetY2())) {
			CLogicInt nLineCenter = layoutLine->GetLogicOffset() + layoutLine->GetLengthWithoutEOL() / 2;
			pptPosArray[i * NUM_OF_POS + 0].x = nLineCenter;
			pptPosArray[i * NUM_OF_POS + 0].y = layoutLine->GetLogicLineNo();
		}else{
			pptPosArray[i * NUM_OF_POS + 0].x = CLogicInt(0);
			pptPosArray[i * NUM_OF_POS + 0].y = CLogicInt(0);
		}
		pptPosArray[i * NUM_OF_POS + 0].ext = CLayoutInt(0);
		if( GetView(i).GetSelectionInfo().GetSelectionAnchorRange().GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().GetSelectionAnchorRange().GetFrom(),
				&pptPosArray[i * NUM_OF_POS + 1]
			);
		}
		if( GetView(i).GetSelectionInfo().GetSelectionAnchorRange().GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().GetSelectionAnchorRange().GetTo(),
				&pptPosArray[i * NUM_OF_POS + 2]
			);
		}
		if( GetView(i).GetSelectionInfo().GetSelectionRange().GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().GetSelectionRange().GetFrom(),
				&pptPosArray[i * NUM_OF_POS + 3]
			);
		}
		if( GetView(i).GetSelectionInfo().GetSelectionRange().GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().GetSelectionRange().GetTo(),
				&pptPosArray[i * NUM_OF_POS + 4]
			);
		}
		GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
			GetView(i).GetCaret().GetCaretLayoutPos(),
			&pptPosArray[i * NUM_OF_POS + 5]
		);
	}
	return pptPosArray;
}

/*!	座標の復元

	CEditWnd::SavePhysPosOfAllViewで保存したデータを元に座標値を再計算する．

	@date 2005.08.11 genta  新規作成
	@date 2007.09.06 kobake 引数をCLogicPoint*に変更
	@date 2011.12.28 CLogicPointをCLogicPointExに変更。改行より右側でも復帰できるように
*/
void CEditWnd::RestorePhysPosOfAllView( CLogicPointEx* pptPosArray )
{
	const int NUM_OF_VIEW = GetAllViewCount();
	const int NUM_OF_POS = 6;

	for( int i = 0; i < NUM_OF_VIEW; ++i ){
		CLayoutPoint tmp;
		GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
			pptPosArray[i * NUM_OF_POS + 0],
			&tmp
		);
		GetView(i).m_pcTextArea->SetViewTopLine(tmp.GetY2());

		CLayoutRange selectionAnchor(GetView(i).GetSelectionInfo().GetSelectionAnchorRange());
		if( selectionAnchor.GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 1],
				selectionAnchor.GetFromPointer()
			);
		}
		if( selectionAnchor.GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 2],
				selectionAnchor.GetToPointer()
			);
		}
		GetView(i).GetSelectionInfo().SetSelectionAnchorRange(selectionAnchor);
		CLayoutRange selectionRange(GetView(i).GetSelectionInfo().GetSelectionRange());
		if( selectionRange.GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 3],
				selectionRange.GetFromPointer()
			);
		}
		if( selectionRange.GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 4],
				selectionRange.GetToPointer()
			);
		}
		GetView(i).GetSelectionInfo().ReplaceSelectionRange(selectionRange);
		CLayoutPoint ptPosXY;
		GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
			pptPosArray[i * NUM_OF_POS + 5],
			&ptPosXY
		);
		GetView(i).GetCaret().MoveCursor( ptPosXY, false ); // 2013.06.05 bScrollをtrue=>falase
		GetView(i).GetCaret().m_nCaretPosX_Prev = GetView(i).GetCaret().GetCaretLayoutPos().GetX2();

		CLayoutInt nLeft = CLayoutInt(0);
		if( GetView(i).GetTextArea().m_nViewColNum < GetView(i).GetRightEdgeForScrollBar() ){
			nLeft = GetView(i).GetRightEdgeForScrollBar() - GetView(i).GetTextArea().m_nViewColNum;
		}
		if( nLeft < GetView(i).GetTextArea().GetViewLeftCol() ){
			GetView(i).GetTextArea().SetViewLeftCol( nLeft );
		}

		GetView(i).GetCaret().ShowEditCaret();
	}
	GetActiveView().GetCaret().ShowCaretPosInfo();
	delete[] pptPosArray;
}

/*!
	@brief マウスの状態をクリアする（ホイールスクロール有無状態をクリア）

	@note ホイール操作によるページスクロール・横スクロール対応のために追加。
		  ページスクロール・横スクロールありフラグをOFFする。

	@date 2009.01.17 nasukoji	新規作成
*/
void CEditWnd::ClearMouseState( void )
{
	SetPageScrollByWheel( FALSE );		// ホイール操作によるページスクロール有無
	SetHScrollByWheel( FALSE );			// ホイール操作による横スクロール有無
}

/*! ウィンドウ毎にアクセラレータテーブルを作成する
	@date 2009.08.15 Hidetaka Sakai, nasukoji
	@date 2013.10.19 novice 共有メモリの代わりにWine実行判定処理を呼び出す

	@note Wineでは別プロセスで作成したアクセラレータテーブルを使用することができない。
	      IsWine()によりプロセス毎にアクセラレータテーブルが作成されるようになる
	      ため、ショートカットキーやカーソルキーが正常に処理されるようになる。
*/
void CEditWnd::CreateAccelTbl( void )
{
	m_hAccel = CKeyBind::CreateAccerelator(
		m_pShareData->m_Common.m_sKeyBind.m_nKeyNameArrNum,
		m_pShareData->m_Common.m_sKeyBind.m_pKeyNameArr
	);

	if( nullptr == m_hAccel ){
		ErrorMessage(
			nullptr,
			LS(STR_ERR_DLGEDITWND01)
		);
	}
}

/*! ウィンドウ毎に作成したアクセラレータテーブルを破棄する
	@datet 2009.08.15 Hidetaka Sakai, nasukoji
*/
void CEditWnd::DeleteAccelTbl( void )
{
	if( m_hAccel ){
		m_hAccel = nullptr;
	}
}

//プラグインコマンドをエディタに登録する
void CEditWnd::RegisterPluginCommand( int idCommand )
{
	CPlug* plug = CJackManager::getInstance()->GetCommandById( idCommand );
	RegisterPluginCommand( plug );
}

//プラグインコマンドをエディタに登録する（一括）
void CEditWnd::RegisterPluginCommand()
{
	const CPlug::Array& plugs = CJackManager::getInstance()->GetPlugs( PP_COMMAND );
	for( CPlug::ArrayIter it = plugs.begin(); it != plugs.end(); it++ ) {
		RegisterPluginCommand( *it );
	}
}

//プラグインコマンドをエディタに登録する
void CEditWnd::RegisterPluginCommand( CPlug* plug )
{
	int iBitmap = CMenuDrawer::TOOLBAR_ICON_PLUGCOMMAND_DEFAULT - 1;
	if( !plug->m_sIcon.empty() ){
		iBitmap = m_cMenuDrawer.m_pcIcons->Add( plug->m_cPlugin.GetFilePath( plug->m_sIcon ).c_str() );
	}

	m_cMenuDrawer.AddToolButton( iBitmap, plug->GetFunctionCode() );
}

const LOGFONT& CEditWnd::GetLogfont(bool bTempSetting)
{
	if( bTempSetting && GetDocument()->m_blfCurTemp ){
		return GetDocument()->m_lfCur;
	}
	if (const auto bUseTypeFont = GetDocument()->m_cDocType.GetDocumentAttribute().m_bUseTypeFont) {
		return GetDocument()->m_cDocType.GetDocumentAttribute().m_lf;
	}
	return m_pShareData->m_Common.m_sView.m_lf;
}

int CEditWnd::GetFontPointSize(bool bTempSetting)
{
	if( bTempSetting && GetDocument()->m_blfCurTemp ){
		return GetDocument()->m_nPointSizeCur;
	}
	if (const auto bUseTypeFont = GetDocument()->m_cDocType.GetDocumentAttribute().m_bUseTypeFont) {
		return GetDocument()->m_cDocType.GetDocumentAttribute().m_nPointSize;
	}
	return m_pShareData->m_Common.m_sView.m_nPointSize;
}
ECharWidthCacheMode CEditWnd::GetLogfontCacheMode()
{
	if( GetDocument()->m_blfCurTemp ){
		return CWM_CACHE_LOCAL;
	}
	if (const auto bUseTypeFont = GetDocument()->m_cDocType.GetDocumentAttribute().m_bUseTypeFont) {
		return CWM_CACHE_LOCAL;
	}
	return CWM_CACHE_SHARE;
}

/*!
	@brief 現在のズーム倍率を取得
	@return 1.0を等倍とするズーム倍率
*/
double CEditWnd::GetFontZoom()
{
	if( GetDocument()->m_blfCurTemp ){
		return GetDocument()->m_nCurrentZoom;
	}else{
		return 1.0;
	}
}

void CEditWnd::ClearViewCaretPosInfo()
{
	for( int v = 0; v < GetAllViewCount(); ++v ){
		GetView(v).GetCaret().ClearCaretPosInfoCache();
	}
}
