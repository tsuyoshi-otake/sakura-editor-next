/*!	@file
	@brief 編集ウィンドウ（外枠）管理クラス

	@author Norio Nakatani
	@date 1998/05/13 新規作成
	@date 2002/01/14 YAZAKI PrintPreviewの分離
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000, genta
	Copyright (C) 2001-2002, YAZAKI
	Copyright (C) 2002, aroka, genta, MIK
	Copyright (C) 2003, MIK, genta, wmlhq
	Copyright (C) 2004, Moca
	Copyright (C) 2005, genta, Moca
	Copyright (C) 2006, ryoji, aroka, fon, yukihane
	Copyright (C) 2007, ryoji
	Copyright (C) 2008, ryoji
	Copyright (C) 2009, nasukoji
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#ifndef SAKURA_CEDITWND_6C771A35_3CC8_4932_BF15_823C40487A9F_H_
#define SAKURA_CEDITWND_6C771A35_3CC8_4932_BF15_823C40487A9F_H_
#pragma once

#include <shellapi.h>// HDROP
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sakura/editor/EditorFrameEvents.h>
#include "_main/global.h"
#include "_os/CDropTarget.h"
#include "CMainToolBar.h"
#include "CTabWnd.h"	//@@@ 2003.05.31 MIK
#include "func/CFuncKeyWnd.h"
#include "CMainStatusBar.h"
#include "view/CEditView.h"
#include "window/CSplitterWnd.h"
#include "dlg/CDlgFind.h"
#include "dlg/CDlgReplace.h"
#include "dlg/CDlgJump.h"
#include "dlg/CDlgGrep.h"
#include "dlg/CDlgGrepReplace.h"
#include "dlg/CDlgSetCharSet.h"
#include "outline/CDlgFuncList.h"
#include "env/CHokanMgr.h"
#include "util/design_template.h"
#include "doc/CDocListener.h"
#include "uiparts/CMenuDrawer.h"
#include "view/CViewFont.h"
#include "view/CMiniMapView.h"

#include "cxx/ResourceHolder.hpp"

#include "print/CPrintPreview.h"
#include "workbench/editor/EditorWorkingCopyTypes.h"
#include "workbench/editor/WorkbenchKeybindingState.h"
#include "workbench/commands/WorkbenchContextKeyService.h"
#include "config/WorkspaceContextTypes.h"
#include "workbench/recent/RecentlyOpenedWorkspaceMenuProjection.h"
#include "workbench/output/IOutputService.h"
#include "workbench/problems/MarkerService.h"
#include "markdown/MarkdownPreviewCommandState.h"
#include "markdown/MarkdownPreviewLayout.h"

static const int MENUBAR_MESSAGE_MAX_LEN = 30;

class CPlug;
class CEditDoc;
class CCustomFrameController;
class CDiffSurface;
struct CEditWndFrameRuntimeState;
struct SDiffSurfaceContent;
namespace config {
class ConfigurationSubscription;
struct ConfigurationTarget;
}
struct DLLSHAREDATA;
namespace platform::filesystem {
class IFileService;
}
namespace terminal {
class CTerminalTool;
struct TerminalTabPresentationSettings;
enum class TerminalShortcutPreset : std::uint8_t;
}
namespace theme {
class CColorThemeRegistry;
}
namespace markdown {
class CMarkdownPreviewWnd;
}
namespace update {
class UpdateComposition;
//! Repeating the alias keeps `update/IUpdateService.h` out of this header; an
//! identical alias redeclaration is the same declaration, so the two cannot
//! drift into different types without a compile error here.
using UpdateServiceSubscriptionId = std::uint64_t;
}
namespace senp {
class ISenpLanguageService;
class ISenpRuntimeService;
}
namespace workbench {
class CActivityBar;
class IWorkbenchRuntime;
class CWorkbenchPanelHost;
class CWorkspaceContext;
enum class WorkbenchEdge : std::uint8_t;
enum class ActivityBarLocation : std::uint8_t;
namespace account {
class AccountDiscoveryService;
}
namespace layout {
class IWorkbenchLayoutSubscription;
struct WorkbenchLayoutStateSnapshot;
}
namespace icons {
}
namespace win32 {
struct BuiltinActiveSurfaceProjection;
enum class BuiltinActiveSurface : std::uint8_t;
}
namespace commands {
class WorkbenchCommandRegistry;
struct WorkbenchCommandExecutionResult;
}
namespace explorer {
class CExplorerTool;
enum class ExplorerFileActivationKind : unsigned char;
}
namespace viewcontainer {
class CViewContainerHost;
class CViewContainerPages;
}
namespace outline {
class COutlineWorkbenchTool;
}
namespace scm {
class CScmWorkbenchTool;
}
namespace search {
class CSearchWorkbenchTool;
}
namespace extensions {
class CExtensionsWorkbenchTool;
}
namespace panel {
class CBottomPanelTool;
}
namespace quickinput {
class CCommandPaletteOverlay;
}
namespace editor {
class CEditDocLegacyEditorBackend;
class CEditorServiceLegacyAdapter;
class EditorWorkingCopyCoordinator;
class CEmptyEditorSurface;
class IEditorCoreSubscription;
struct EditorCoreSnapshot;
}
namespace editor::persistence {
class EditorWorkingCopyLifecycleBridge;
struct EditorWorkingCopyCompletionToken;
}
}

//メインウィンドウ内コントロールID
#define IDT_EDIT		455  // 20060128 aroka
#define IDT_TOOLBAR		456
#define IDT_CAPTION		457
#define IDT_FIRST_IDLE	458
#define IDT_EXTENSION_DOCUMENT_SYNC 459
#define IDT_WORKBENCH_KEYBINDING_CHORD 460
#define IDT_SYSMENU		1357
#define ID_TOOLBAR		100

struct STabGroupInfo {
	HWND			hwndTop = nullptr;
	WINDOWPLACEMENT	wpTop = {};

	STabGroupInfo() = default;

	bool IsValid() const noexcept { return hwndTop != nullptr; }
};

//! Parameter-preserving request at the legacy command/workbench migration seam.
//! functionCode retains its FA_* source flags; the adapter decodes only its low word.
class SLegacyEditorFunctionCommand final {
public:
	SLegacyEditorFunctionCommand(EFunctionCode functionCode, bool redraw,
		std::intptr_t lparam1, std::intptr_t lparam2,
		std::intptr_t lparam3, std::intptr_t lparam4) noexcept
		: m_functionCode(functionCode)
		, m_redraw(redraw)
		, m_lparam1(lparam1)
		, m_lparam2(lparam2)
		, m_lparam3(lparam3)
		, m_lparam4(lparam4)
	{
	}

	[[nodiscard]] EFunctionCode FunctionCode() const noexcept { return m_functionCode; }
	[[nodiscard]] bool Redraw() const noexcept { return m_redraw; }
	[[nodiscard]] std::intptr_t Parameter1() const noexcept { return m_lparam1; }
	[[nodiscard]] std::intptr_t Parameter2() const noexcept { return m_lparam2; }
	[[nodiscard]] std::intptr_t Parameter3() const noexcept { return m_lparam3; }
	[[nodiscard]] std::intptr_t Parameter4() const noexcept { return m_lparam4; }

private:
	const EFunctionCode m_functionCode;
	const bool m_redraw;
	const std::intptr_t m_lparam1;
	const std::intptr_t m_lparam2;
	const std::intptr_t m_lparam3;
	const std::intptr_t m_lparam4;
};

//! A handled operation never falls back to the legacy implementation, including
//! cancellation, conflict, unsupported, and failure terminals.
struct SWorkingCopyFunctionDispatchResult final {
	bool handled = false;
	BOOL legacyResult = TRUE;
	std::optional<workbench::editor::EditorWorkingCopyOperationResult> operation;
};

//! Terminal outcome for the one-input Hot Exit recovery projection.  Recovery
//! has already committed the backing CEditDoc before this boundary runs, so a
//! failure is deliberately observable to startup rather than being hidden by a
//! best-effort native refresh.
enum class ERecoveredEditorProjectionResult : std::uint8_t {
	Succeeded,
	InvalidRecovery,
	CoreActivationFailed,
	NativeProjectionFailed,
};

//! Terminal outcome for the native Open Folder adapter.  Keep picker, workspace
//! context, and Explorer projection failures distinct so the stable command
//! boundary never reports one terminal as another.
enum class EOpenWorkspaceFolderResult : std::uint8_t {
	Succeeded,
	Cancelled,
	InvalidSelection,
	PickerFailed,
	WorkspaceContextFailed,
	ExplorerProjectionFailed,
};

//! The four branch commands the built-in Git provider contributes, named after
//! upstream's own command IDs (`git.checkout`, `git.checkoutDetached`,
//! `git.branch`, `git.branchFrom`) so the native adapter cannot drift from the
//! identity it is registered under.
enum class EGitBranchCommand : std::uint8_t {
	Checkout,
	CheckoutDetached,
	Branch,
	BranchFrom,
};

//! The six working-tree commands the built-in Git provider contributes, named
//! after upstream's own command IDs. The `*All` members carry no operand
//! because upstream's handlers for them take only the repository; the other
//! three act on the rows the invocation named.
enum class EGitStageCommand : std::uint8_t {
	Stage,
	StageAll,
	Unstage,
	UnstageAll,
	Clean,
	CleanAll,
};

//! The three commit commands the built-in Git provider contributes, named after
//! upstream's own command IDs. All three are repository-scoped: upstream's
//! `commitWithAnyInput` takes only the repository and reads its message off that
//! repository's own SCM input box, so none of them carries an operand payload.
enum class EGitCommitCommand : std::uint8_t {
	Commit,
	CommitAmend,
	UndoCommit,
};

//! The nine remote commands the built-in Git provider contributes, named after
//! upstream's own command IDs. Upstream ships the prune, all-remotes, and
//! rebase variants as separate commands rather than as flags on one, so they
//! are separate members here: a caller-supplied flag would be a payload shape
//! upstream never publishes. All nine are repository-scoped — which remote and
//! which branch come from HEAD's own upstream or from a Quick Pick.
enum class EGitSyncCommand : std::uint8_t {
	Fetch,
	FetchPrune,
	FetchAll,
	Pull,
	PullRebase,
	Push,
	Sync,
	SyncRebase,
	Publish,
};

//! The five update operations the workbench can ask for, named after the
//! `IUpdateService` members they call rather than after any one of the two
//! command families that reach them: upstream binds both `update.checkForUpdate`
//! and `update.check` to `checkForUpdates`, so naming these after a command ID
//! would make one of each pair look like the odd one out.
enum class EUpdateCommand : std::uint8_t {
	CheckForUpdates,
	DownloadUpdate,
	ApplyUpdate,
	QuitAndInstall,
	ShowUpdateInfo,
};

//! Every native workspace picker/transition path has one terminal result.  The
//! window owns picker and process composition; the workbench service remains
//! HWND-free. Folder transitions stay in-process; only explicit window
//! composition paths launch a successor.
enum class EWorkspaceWindowTransitionResult : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
};

//! 編集ウィンドウ（外枠）管理クラス
// 2002.02.17 YAZAKI CShareDataのインスタンスは、CProcessにひとつあるのみ。
// 2007.10.30 kobake IsFuncEnable,IsFuncCheckedをFunccode.hに移動
// 2007.10.30 kobake OnHelp_MenuItemをCEditAppに移動
class CEditWnd
	: public TSingleInstance<CEditWnd>
, public CDocListenerEx
, public sakura::editor::IEditorFrameEventSink
{
private:
	using AccelHolder = cxx::ResourceHolder<&::DestroyAcceleratorTable>;
	using CDropTargetHolder = std::unique_ptr<CDropTarget>;
	using CEditViewHolder = std::unique_ptr<CEditView>;
	using CEditViewsArray = std::array<CEditViewHolder, 4>;
	using CPrintPreviewHolder = std::unique_ptr<CPrintPreview>;
	using CViewFontHolder = std::unique_ptr<CViewFont>;
	using FontHolder = cxx::ResourceHolder<&::DeleteObject, HFONT>;
	using MemDcHolder = cxx::ResourceHolder<&::DeleteDC>;
	using SelectionHolder = cxx::ResourceHolder<&::SelectObject>;
	using SMenubarMessage = StaticString<MENUBAR_MESSAGE_MAX_LEN>;
	using WindowDcHolder = cxx::ResourceHolder<&::ReleaseDC>;

public:
	//! Resource label for the state-dependent Close Folder / Close Workspace
	//! File-menu slot. Zero means the slot is intentionally omitted in Empty.
	[[nodiscard]] static UINT CloseWorkspaceMenuLabelResource(config::EWorkspaceKind kind) noexcept;

	CEditWnd();
	CEditWnd(
		workbench::editor::CEditorServiceLegacyAdapter& editorServiceAdapter,
		workbench::editor::CEditDocLegacyEditorBackend& legacyEditorBackend,
		workbench::editor::EditorWorkingCopyCoordinator& workingCopyCoordinator,
		workbench::editor::persistence::EditorWorkingCopyLifecycleBridge& workingCopyLifecycleBridge,
		workbench::IWorkbenchRuntime& workbenchRuntime);
	~CEditWnd() override;

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                           作成                              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//	Mar. 7, 2002 genta 文書タイプ用引数追加
	// 2007.06.26 ryoji グループ指定引数追加
	//! 作成
	HWND Create(
		const CEditDoc*	pcEditDoc,
		CImageListMgr*	pcIcons,
		int				nGroup
	);
	void	_GetTabGroupInfo(STabGroupInfo* pTabGroupInfo, int& nGroup) const;
	void	_GetWindowRectForInit(CMyRect* rcResult, int nGroup, const STabGroupInfo& sTabGroupInfo) const;	//!< ウィンドウ生成用の矩形を取得
	HWND _CreateMainWindow(int nGroup, const STabGroupInfo& sTabGroupInfo);
	void _AdjustInMonitor(const STabGroupInfo& sTabGroupInfo);

	void OpenDocumentWhenStart(
		const SLoadInfo& sLoadInfo		//!< [in]
	);

	//! Initial-window presentation is committed once after document/layout setup.
	void CommitStartupDrawTransaction();
	[[nodiscard]] bool IsStartupDrawSuppressed() const noexcept;
	[[nodiscard]] bool IsStartupDrawCommitting() const noexcept;
	//! Records completion of the first full paint of the primary editor view.
	void RecordFirstStartupContentPaint() noexcept;
	//! Aggregates minimap work in memory; the transaction emits one summary.
	void RecordStartupMiniMapImmediateUpdate() noexcept;
	void RecordStartupMiniMapPaint(std::int64_t qpcTicks) noexcept;

	void SetDocumentTypeWhenCreate(
		ECodeType		nCharCode,							//!< [in] 漢字コード
		bool			bViewMode,							//!< [in] ビューモードで開くかどうか
		CTypeConfig	nDocumentType = CTypeConfig(-1)	//!< [in] 文書タイプ．-1のとき強制指定無し．
	);
	void UpdateCaption();
	//! True only when the last applied authoritative Editor Core snapshot has an active input.
	//! Unit-only windows without the migration seam retain the historical always-backed editor behavior.
	[[nodiscard]] bool HasActiveEditorInput() const noexcept
	{
		return m_editorServiceAdapter == nullptr || m_hasActiveEditorInput;
	}
	//! Explicitly adopts a legitimate pathless legacy tool/untitled input after its legacy setup completed.
	[[nodiscard]] bool AdoptLegacyUntitledInput(std::string_view kind);
	//! Selects the one recovered core input and immediately applies the resulting
	//! authoritative snapshot to this native window.  The legacy document is only
	//! a prepared backing object; it is never consulted to infer the active input.
	[[nodiscard]] ERecoveredEditorProjectionResult ReconcileRecoveredEditorInput(
		std::string_view recoveredInputId, std::string_view effectiveActiveInputId);
	//! Central parameter-preserving migration seam used by CViewCommander.
	[[nodiscard]] SWorkingCopyFunctionDispatchResult TryExecuteWorkingCopyFileCommand(
		const SLegacyEditorFunctionCommand& request);
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                         イベント                            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//ドキュメントイベント
	void OnBeforeLoad(SLoadInfo* sLoadInfo) override;
	void OnAfterLoad(const SLoadInfo& sLoadInfo) override;
	ELoadFinalizationStatus OnFinalLoad(ELoadResult eLoadResult) override;
	void OnAfterSave(const SSaveInfo& sSaveInfo) override;
	//! Arms the common load listener while the old native content is still intact.
	//! FileCloseOpen uses this after target/check/close-veto validation and before its close event.
	void PrepareLegacyLoadReplacement();

	//管理
	void MessageLoop( void );								/* メッセージループ */
	LRESULT DispatchEvent(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);	/* メッセージ処理 */
	[[nodiscard]] sakura::editor::EditorFrameEffect HandleEditorFrameEvent(
		const sakura::editor::EditorFrameEvent& event) override;
	void AttachMainWindowEarly(HWND hWnd);
	LRESULT DispatchBootstrapEvent(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
	[[nodiscard]] bool IsDispatchReady() const noexcept { return m_dispatchReady; }

	//各種イベント
	LRESULT OnPaint(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);	/* 描画処理 */
	LRESULT OnSize(WPARAM wParam, LPARAM lParam);	/* WM_SIZE 処理 */
	LRESULT OnSize2(WPARAM wParam, LPARAM lParam, bool bUpdateStatus);
	LRESULT OnLButtonUp(WPARAM wParam, LPARAM lParam);
	LRESULT OnLButtonDown(WPARAM wParam, LPARAM lParam);
	LRESULT OnMouseMove(WPARAM wParam, LPARAM lParam);
	LRESULT OnSetCursor(WPARAM wParam, LPARAM lParam);
	LRESULT OnCaptureChanged(LPARAM lParam);
	HWND HoveredScrollTarget(LPARAM lParam) const noexcept;
	LRESULT OnMouseWheel(WPARAM wParam, LPARAM lParam);
	BOOL DoMouseWheel( WPARAM wParam, LPARAM lParam );	// マウスホイール処理	// 2007.10.16 ryoji
	LRESULT OnHScroll(WPARAM wParam, LPARAM lParam);
	LRESULT OnVScroll(WPARAM wParam, LPARAM lParam);
	int	OnClose(HWND hWndActive, bool bGrepNoConfirm);	/* 終了時の処理 */
	void OnDropFiles(HDROP hDrop);	/* ファイルがドロップされた */
	BOOL OnPrintPageSetting( void );/* 印刷ページ設定 */
	LRESULT OnTimer(WPARAM wParam, LPARAM lParam);	// WM_TIMER 処理	// 2007.04.03 ryoji
	void OnEditTimer( void );	/* タイマーの処理 */
	void	OnCaptionTimer() const;
	void OnSysMenuTimer( void );
	void OnCommand(WORD wNotifyCode, WORD wID, HWND hwndCtl);
	LRESULT OnNcLButtonDown(WPARAM wp, LPARAM lp);
	LRESULT OnNcLButtonUp(WPARAM wp, LPARAM lp);
	LRESULT OnLButtonDblClk(WPARAM wp, LPARAM lp);

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                           通知                              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

	//ファイル名変更通知
	void	ChangeFileNameNotify(std::wstring_view tabCaption, std::wstring_view tabFilePath, bool bIsGrep) const;

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                         メニュー                            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	void InitMenu(HMENU hMenu, UINT uPos, BOOL fSystemMenu);
	void InitMenu_Function(HMENU hMenu, EFunctionCode eFunc, const wchar_t* pszName, const wchar_t* pszKey);
	bool InitMenu_Special(HMENU hMenu, EFunctionCode eFunc);
	void InitMenubarMessageFont(void);	//	メニューバーへのメッセージ表示機能をCEditWndより移管	//	Dec. 4, 2002 genta
	LRESULT WinListMenu(HMENU hMenu, EditNode* pEditNodeArr, int nRowNum, BOOL bFull);	/*!< ウィンドウ一覧メニュー作成処理 */	// 2006.03.23 fon
	LRESULT PopupWinList( bool bMousePos );	/*!< ウィンドウ一覧ポップアップ表示処理 */	// 2006.03.23 fon	// 2007.02.28 ryoji フルパス指定のパラメータを削除
	void RegisterPluginCommand();			//プラグインコマンドをエディタに登録する
	void RegisterPluginCommand( int id );	//プラグインコマンドをエディタに登録する
	void RegisterPluginCommand( CPlug* id );	//プラグインコマンドをエディタに登録する

	void SetMenuFuncSel( HMENU hMenu, EFunctionCode nFunc, const WCHAR* sKey, bool flag );				// 表示の動的選択	2010/5/19 Uchi

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                           整形                              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	void LayoutMainMenu( void );		// メインメニュー					// 2010/5/16 Uchi
	void LayoutToolBar( void );			/* ツールバーの配置処理 */			// 2006.12.19 ryoji
	void LayoutFuncKey( void );			/* ファンクションキーの配置処理 */	// 2006.12.19 ryoji
	void LayoutTabBar( void );			/* タブバーの配置処理 */			// 2006.12.19 ryoji
	void LayoutStatusBar( void );		/* ステータスバーの配置処理 */		// 2006.12.19 ryoji
	void LayoutStatusBarParts();		//!< 現在の表示内容に合わせて右側項目を詰めて配置する
	void LayoutMiniMap();				// ミニマップの配置処理
	[[nodiscard]] bool SetMiniMapEnabled(bool enabled, bool persist = true);
	[[nodiscard]] bool ApplyMiniMapContextCommand(
		minimap::ContextCommand command, bool persist = true);
	void EndLayoutBars( BOOL bAdjust = TRUE );	/* バーの配置終了処理 */	// 2006.12.19 ryoji
	bool SetWorkbenchPanelVisible(
		workbench::WorkbenchEdge edge, bool visible, bool activate = false);
	void ToggleWorkbenchPanel(workbench::WorkbenchEdge edge, bool activate = false);
	//! Select a window-local workbench root. Picker cancellation/invalid input is
	//! non-applicable; a native Explorer projection failure is explicitly failed.
	[[nodiscard]] EOpenWorkspaceFolderResult OpenWorkspaceFolder();
	//! Native-command bridge for the workspace File-menu function codes.  It
	//! keeps CViewCommander on the same stable-command path as menu dispatch.
	void ExecuteWorkbenchFileFunction(EFunctionCode functionCode);
	//! True only for the real runtime-backed Workbench composition, never the classic test/legacy path.
	[[nodiscard]] bool IsWorkbenchRuntimeBacked() const noexcept { return m_workbenchRuntime != nullptr; }
	[[nodiscard]] senp::ISenpRuntimeService* GetSenpRuntime() const noexcept;
	[[nodiscard]] senp::ISenpLanguageService* GetSenpLanguageService() const noexcept;
	[[nodiscard]] bool IsWorkbenchPanelVisible(workbench::WorkbenchEdge edge) const noexcept;
	//! `workbench.action.toggleAuxiliaryBar` (Ctrl+Alt+B). This is the physical Secondary
	//! Side Bar Part, never the Outline View nested in the Primary Side Bar.
	void ToggleSecondarySidebar(bool activate = false);
	[[nodiscard]] bool IsSecondarySidebarVisible() const noexcept;
	void FocusIntegratedTerminal();
	void NewIntegratedTerminal();
	void RedetectPowerShell();
	void ToggleMarkdownPreview();
	[[nodiscard]] bool IsMarkdownPreviewVisible() const noexcept;
	//! VS Code の Markdown scroll sync: エディタ側の表示先頭行にプレビューを追従させる。
	//! ミニマップ操作も実ビューの ScrollAtV を経由するのでここに集約している。
	void SyncMarkdownPreviewToEditorScroll(const CEditView& view);
	[[nodiscard]] bool IsMarkdownPreviewAvailable() const;

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                           設定                              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	void PrintPreviewModeONOFF( void );	/* 印刷プレビューモードのオン/オフ */
	
	//アイコン
	void	SetWindowIcon(HICON hIcon, int flag) const;
	void GetDefaultIcon( HICON* hIconBig, HICON* hIconSmall ) const;	//	Sep. 10, 2002 genta
	bool GetRelatedIcon(const WCHAR* szFile, HICON* hIconBig, HICON* hIconSmall) const;	//	Sep. 10, 2002 genta
	void SetPageScrollByWheel( BOOL bState ) { m_bPageScrollByWheel = bState; }		// ホイール操作によるページスクロール有無を設定する（TRUE=あり, FALSE=なし）	// 2009.01.17 nasukoji
	void SetHScrollByWheel( BOOL bState ) { m_bHorizontalScrollByWheel = bState; }	// ホイール操作による横スクロール有無を設定する（TRUE=あり, FALSE=なし）	// 2009.01.17 nasukoji
	void ClearMouseState( void );		// 2009.01.17 nasukoji	マウスの状態をクリアする（ホイールスクロール有無状態をクリア）

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                           情報                              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //

	//! 自アプリがアクティブかどうか	// 2007.03.08 ryoji
	bool IsActiveApp() const { return m_bIsActiveApp; }

	//!ツールチップのテキストを取得。2007.09.08 kobake 追加
	void GetTooltipText(WCHAR* pszBuf, size_t nBufCount, UINT_PTR idFrom) const;

	//!印刷プレビュー中かどうか
	bool IsInPreviewMode()
	{
		return m_pPrintPreview!=nullptr;
	}

	BOOL IsPageScrollByWheel() const { return m_bPageScrollByWheel; }		// ホイール操作によるページスクロール有無	// 2009.01.17 nasukoji
	BOOL IsHScrollByWheel() const { return m_bHorizontalScrollByWheel; }	// ホイール操作による横スクロール有無		// 2009.01.17 nasukoji

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                           表示                              //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	void PrintMenubarMessage( const WCHAR* msg );
	void SendStatusMessage( const WCHAR* msg );		//	Dec. 4, 2002 genta 実体をCEditViewから移動

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                      ウィンドウ操作                         //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	void	WindowTopMost(int top) const;

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                        ビュー管理                           //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	LRESULT Views_DispatchEvent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	bool CreateEditViewBySplit(int nViewCount);
	void InitAllViews();
	void Views_RedrawAll();
	void Views_Redraw();
	void SetActivePane(int nIndex);	/* アクティブなペインを設定 */
	int GetActivePane( void ) const { return m_nActivePaneIndex; }	/* アクティブなペインを取得 */ //2007.08.26 kobake const追加
	bool SetDrawSwitchOfAllViews( bool bDraw );					/* すべてのペインの描画スイッチを設定する */	// 2008.06.08 ryoji
	void RedrawAllViews( CEditView* pcViewExclude );				/* すべてのペインをRedrawする */
	void Views_DisableSelectArea(bool bRedraw);
	BOOL DetectWidthOfLineNumberAreaAllPane( bool bRedraw );	/* すべてのペインで、行番号表示に必要な幅を再設定する（必要なら再描画する） */
	BOOL WrapWindowWidth( int nPane );	/* 右端で折り返す */	// 2008.06.08 ryoji
	BOOL UpdateTextWrap( void );		/* 折り返し方法関連の更新 */	// 2008.06.10 ryoji
	//	Aug. 14, 2005 genta TAB幅と折り返し位置の更新
	void ChangeLayoutParam( bool bShowProgress, CKetaXInt nTabSize, int nTsvMode, CKetaXInt nMaxLineKetas );
	//	Aug. 14, 2005 genta
	CLogicPointEx* SavePhysPosOfAllView();
	void RestorePhysPosOfAllView( CLogicPointEx* pptPosArray );
	// 互換BMPによる画面バッファ 2007.09.09 Moca
	void Views_DeleteCompatibleBitmap(); //!< CEditViewの画面バッファを削除

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                       各種アクセサ                          //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	HWND			GetHwnd()		const	{ return m_hWnd; }
	CMenuDrawer&	GetMenuDrawer()			{ return m_cMenuDrawer; }
	CEditDoc*		GetDocument()           { return m_pcEditDoc; }
	const CEditDoc*	GetDocument() const     { return m_pcEditDoc; }
	//! Filesystem breadcrumbs for the active input, relative to its longest matching workspace root.
	[[nodiscard]] std::vector<std::wstring> BuildActiveDocumentBreadcrumbSegments() const;

	//ビュー
	const CEditView&	GetActiveView() const { return *m_pcEditView; }
	CEditView&			GetActiveView()       { return *m_pcEditView; }
	const CEditView&    GetView(int n) const { return *m_pcEditViewArr[n]; }
	CEditView&          GetView(int n)       { return *m_pcEditViewArr[n]; }
	CMiniMapView&       GetMiniMap( void ) { return m_cMiniMapView; }
	bool                IsEnablePane(int n) const { return 0 <= n && n < m_nEditViewCount; }
	int                 GetAllViewCount() const { return m_nEditViewCount; }

	CEditView*			GetDragSourceView() const					{ return m_pcDragSourceView; }
	void				SetDragSourceView( CEditView* pcDragSourceView )	{ m_pcDragSourceView = pcDragSourceView; }

	CViewFont* GetViewFont(bool isMiniMap) const noexcept {
		return isMiniMap
			? m_pcViewFontMiniMap.get()
			: m_pcViewFont.get();
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                         実装補助                            //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
//by 鬼
protected:
	enum EIconClickStatus{
		icNone,
		icDown,
		icClicked,
		icDoubleClicked
	};

protected:
	//ドロップダウンメニュー
	int	CreateFileDropDownMenu(HWND hwnd);	//開く(ドロップダウン)	//@@@ 2002.06.15 MIK

	//タイマー
	void	Timer_ONOFF(bool bStart) const; /* 更新の開始／停止 20060128 aroka */

	// メニュー
	//! 先頭・末尾・連続した区切り線を取り除く。空グループを含むメニューでのみ効果がある。
	static void RemoveRedundantMenuSeparators(HMENU hMenu);
	void CheckFreeSubMenu(HWND hWnd, HMENU hMenu, UINT uPos);		// メニューバーの無効化を検査	2010/6/18 Uchi
	void CheckFreeSubMenuSub(HMENU hMenu, int nLv);			// メニューバーの無効化を検査	2010/6/18 Uchi
	[[nodiscard]] HMENU GetMainMenuHandle() const noexcept;

//public:
	//! 周期内でm_nTimerCountをインクリメント
	void IncrementTimerCount(int nInterval)
	{
		m_nTimerCount++;
		if( nInterval <= m_nTimerCount ){ // 2012.11.29 aroka 呼び出し間隔のバグ修正
			m_nTimerCount = 0;
		}
	}

	void CreateAccelTbl( void ); // ウィンドウ毎のアクセラレータテーブル作成(Wine用)
	void DeleteAccelTbl( void ); // ウィンドウ毎のアクセラレータテーブル破棄(Wine用)

public:
	//D&Dフラグ管理
	void SetDragPosOrg(CMyPoint ptDragPosOrg){ m_ptDragPosOrg=ptDragPosOrg; }
	void SetDragMode(bool bDragMode){ m_bDragMode = bDragMode; }
	bool GetDragMode() const{ return m_bDragMode; }
	const CMyPoint& GetDragPosOrg() const{ return m_ptDragPosOrg; }

	/* IDropTarget実装 */	// 2008.06.20 ryoji
	STDMETHODIMP DragEnter(LPDATAOBJECT pDataObject, DWORD dwKeyState, POINTL pt [[maybe_unused]], LPDWORD pdwEffect) const;
	STDMETHODIMP DragOver(DWORD dwKeyState [[maybe_unused]], POINTL pt [[maybe_unused]], LPDWORD pdwEffect) const;
	STDMETHODIMP DragLeave() const;
	STDMETHODIMP Drop(LPDATAOBJECT pDataObject, DWORD dwKeyState, POINTL pt, LPDWORD pdwEffect);

	//フォーカス管理
	int GetCurrentFocus() const{ return m_nCurrentFocus; }
	void SetCurrentFocus(int n){ m_nCurrentFocus = n; }

	const LOGFONT&	GetLogfont(bool bTempSetting = true);
	int			GetFontPointSize(bool bTempSetting = true);
	ECharWidthCacheMode GetLogfontCacheMode();
	double GetFontZoom();
	//! Called by the successor process only after it has crossed the ready IPC
	//! boundary.  The predecessor never mutates typed recent history.
	void RecordCurrentWorkspaceAfterReady();

	void ClearViewCaretPosInfo();

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
	//                        メンバ変数                           //
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- //
private:
	enum class StartupDrawState {
		Inactive,
		Suppressing,
		Committing,
		Committed,
		Aborted,
	};

	void BeginStartupDrawTransaction() noexcept;
	void AbortStartupDrawTransaction() noexcept;
	void FinishStartupTabSwap() noexcept;
	void EmitStartupMiniMapSummary() noexcept;
	[[nodiscard]] bool ShouldDeferStartupLayout() const noexcept;

	bool InitializeWorkbench();
	void RefreshStatusbarPresentation();
	void SetStatusbarEntryHidden(std::string_view id, bool hidden);
	void PostDeferredStartupWorkbenchIfReady();
	void CompleteDeferredStartupWorkbench();
	void CloseWorkbench() noexcept;
	//! Applies the committed theme, or one non-persistent Quick Pick preview.
	//! An invalid explicit preview leaves the currently painted theme untouched.
	[[nodiscard]] bool ApplyWorkbenchTheme(std::wstring_view previewTheme = {});
	void ApplyWorkbenchSettingsFromSharedData(bool finalizeProjection = true);
	[[nodiscard]] bool ApplyInitialWorkbenchLayoutState();
	[[nodiscard]] bool ApplyCurrentWorkbenchLayoutState(bool finalizeProjection,
		bool broadcastMirrorChanges, bool* mirrorChanged = nullptr);
	[[nodiscard]] bool ApplyBuiltinWorkbenchSurfaces(
		const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot,
		const workbench::win32::BuiltinActiveSurfaceProjection& projection);
	void ApplyBuiltinWorkbenchFocus(
		const workbench::win32::BuiltinActiveSurfaceProjection& projection);
	void OnWorkbenchLayoutStateChanged();
	void OnWorkbenchServiceProjectionChanged();
	[[nodiscard]] bool InitializeWorkbenchServiceProjection();
	void CloseWorkbenchServiceProjection() noexcept;
	//! Composes the per-window update stack and subscribes to it. Returns false
	//! only for a programming error; a machine that cannot update at all (no
	//! staging root, unreadable network policy) is a normal absence and leaves
	//! the window running with no update surfaces at all.
	void InitializeUpdateProjection() noexcept;
	void CloseUpdateProjection() noexcept;
	//! UI-thread terminal for `MYWM_WORKBENCH_UPDATE_STATE_CHANGED`: re-reads the
	//! committed state, refreshes the context projection, and repaints the title.
	void OnWorkbenchUpdateStateChanged();
	void FinalizeWorkbenchPanelProjection(
		const workbench::win32::BuiltinActiveSurfaceProjection* runtimeProjection = nullptr);
	void RedrawWorkbenchFrameForCommittedLayout(bool immediate);
	[[nodiscard]] bool InitializeFrameRuntime() noexcept;
	void UpdateFrameRuntimeCadence(bool invalidateSource = false) noexcept;
	//! Rebinds the SCM retained-paint target after a committed layout/visibility
	//! change. The source remains the SCM-owned Changes child HWND; this only
	//! updates the borrowed target metadata and runtime epochs.
	void UpdateFrameRuntimeNativeSurfaces() noexcept;
	void PublishCommittedGdiFrame() noexcept;
	void CloseFrameRuntime() noexcept;
	void ReloadWorkbenchOutlineAndRelayout();
	void BroadcastWorkbenchSettings();
	[[nodiscard]] std::wstring GetSemanticWorkspaceRoot() const;
	void ApplySemanticWorkspaceContext();
	void UpdateWorkspaceFromDocument();
	void OpenExplorerFile(std::wstring_view path,
		workbench::explorer::ExplorerFileActivationKind kind);
	//! Opens one Search result. `line` and `column` are 1-based UTF-16 positions.
	void OpenSearchMatch(std::wstring_view path, std::int64_t line, int column);
	//! Opens `path` and places the caret at one zero-based UTF-16 position.
	//! Shared by the Problems panel and the Search view.
	void OpenDocumentAtMarkerPosition(std::wstring_view path,
		std::uint32_t zeroBasedLine, std::uint32_t zeroBasedColumn);
	//! Reloads this frame's document when a replace pass rewrote it underneath.
	void ReloadReplacedFiles(const std::vector<std::wstring>& paths);
	[[nodiscard]] std::wstring BuildExplorerLaunchOptions(bool preview) const;
	void RefreshEditorCorePresentation();
	void ApplyEditorCoreSnapshot(const workbench::editor::EditorCoreSnapshot& snapshot, bool restoreFocus = true);
	//! Projects authoritative workspace/editor state into Explorer and SCM welcome variants.
	void UpdateWorkbenchWelcomeState();
	[[nodiscard]] bool AdoptLoadedLegacyFile();
	[[nodiscard]] bool FinalizeSuccessfulLegacyLoad();
	[[nodiscard]] bool CreateUntitledEditorInput();
	[[nodiscard]] bool CloseActiveEditorInput();
	void ConfigureCustomFrameActions();
	[[nodiscard]] bool ExecuteWorkbenchEditorCommand(std::string_view commandId);
	[[nodiscard]] EOpenWorkspaceFolderResult ApplyFolderWorkspace(
		const std::wstring& absoluteRoot, bool revealExplorer);
	[[nodiscard]] EWorkspaceWindowTransitionResult OpenWorkspaceConfiguration();
	[[nodiscard]] EWorkspaceWindowTransitionResult AddFolderToWorkspace();
	[[nodiscard]] EWorkspaceWindowTransitionResult SaveWorkspaceAs();
	[[nodiscard]] EWorkspaceWindowTransitionResult DuplicateWorkspaceInNewWindow();
	[[nodiscard]] EWorkspaceWindowTransitionResult CloseWorkspaceWindow();
	[[nodiscard]] EWorkspaceWindowTransitionResult ShowRecentlyOpenedWorkspaceMenu();
	[[nodiscard]] EWorkspaceWindowTransitionResult OpenRecentlyOpenedWorkspace(
		const workbench::recent::RecentlyOpenedWorkspaceEntry& entry);
	void RecordRecentlyOpenedWorkspaceAfterReady(
		workbench::recent::ERecentlyOpenedWorkspaceKind kind, const platform::uri::Uri& uri);
	[[nodiscard]] bool AppendRecentlyOpenedWorkspaceMenu(HMENU hMenu, bool hasRecentFiles);
	void AppendRecentlyOpenedWorkspaceMenuRows(HMENU hMenu,
		const std::vector<workbench::recent::RecentlyOpenedWorkspaceMenuRow>& rows);
	//! Upstream's static Open Recent tail. It belongs to the menu-bar submenu
	//! only; the Ctrl+R history list carries no clear action.
	void AppendClearRecentlyOpenedMenuItem(HMENU hMenu, bool hasPrecedingRows);
	[[nodiscard]] EWorkspaceWindowTransitionResult ClearRecentlyOpenedHistory();
	[[nodiscard]] bool TryExecuteRecentlyOpenedWorkspaceMenuCommand(std::int32_t commandId);
	[[nodiscard]] bool HasRecentlyOpenedItems() const;
	[[nodiscard]] EWorkspaceWindowTransitionResult LaunchWorkspaceTarget(
		const std::wstring& commandLineOption, bool closeCurrentWindow,
		const std::wstring& stagedTargetToDeleteOnFailure = {});
	[[nodiscard]] EWorkspaceWindowTransitionResult PrepareWorkspaceReplacement();
	[[nodiscard]] EWorkspaceWindowTransitionResult CloseWorkspaceWindowOnce();
	[[nodiscard]] bool ExecuteActiveWorkingCopyCommand(
		std::string_view commandId, bool suppressCloseConfirmation = false,
		bool disposeWindow = false);
	[[nodiscard]] workbench::editor::EditorWorkingCopyOperationResult ExecuteActiveWorkingCopyOperation(
		std::string_view commandId,
		const workbench::editor::EditorWorkingCopySaveOptions& saveOptions = {},
		std::optional<workbench::editor::EditorDocumentIdentity> targetIdentity = std::nullopt,
		bool suppressCloseConfirmation = false,
		bool disposeWindow = false);
	void DispatchEditorFunction(EFunctionCode functionCode);
	[[nodiscard]] bool SynchronizeLegacyDocumentState(bool dirty, bool contentChanged);
	void ClearDocumentStatus();
	[[nodiscard]] std::string NextEditorOperationId(std::string_view prefix);
	[[nodiscard]] bool ActiveInputMatchesCurrentFile() const;
	//! Rebuilds the window-local registry from Sakura's bundled themes.
	void RefreshColorThemes();
	//! Shows Sakura's color themes through the shared VS Code-compatible Quick Pick.
	[[nodiscard]] bool ShowColorThemePicker();
	[[nodiscard]] std::wstring ConfiguredColorThemeLabel() const;
	[[nodiscard]] bool PersistColorThemeSelection(std::wstring_view themeId);
	//! Runs one of the built-in Git provider's branch commands, presenting its
	//! Quick Pick and input box through the same native surfaces every other
	//! workbench picker uses.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitBranchCommand(
		EGitBranchCommand command);
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteOpenWorkspaceFolderCommand();
	//! Runs one update operation. Returns `Unsupported` when this window has no
	//! update stack at all, which is the honest answer for an installation that
	//! cannot update itself rather than a silently successful no-op.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteUpdateCommand(
		EUpdateCommand command);
	//! `update.restart`'s two halves: arm the staged installer, then run the
	//! ordinary quit. Separate because the quit can destroy this window before it
	//! returns, and that constraint is worth stating in one place.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteUpdateQuitAndInstall();
	//! Runs one of the built-in Git provider's working-tree commands.
	//! `argumentsJson` is the payload `BuildGitStageArguments` produces; it is
	//! empty for the `*All` members and for a Command Palette invocation.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitStageCommand(
		EGitStageCommand command, std::string_view argumentsJson);
	//! Runs one of the built-in Git provider's commit commands. The message comes
	//! from the Source Control view's own input box, exactly as upstream reads
	//! `repository.inputBox.value`, so a window without that view has no message
	//! to commit rather than an empty one. `argumentsJson` is the optional
	//! post-commit payload published by the action button.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitCommitCommand(
		EGitCommitCommand command, std::string_view argumentsJson = {});
	//! Runs VS Code's `vscode.diff` API command: reads both named sides, diffs
	//! them, and projects the result onto the native diff surface. Like upstream
	//! it is a general command over two URIs, not a Git-specific entry point;
	//! `git.openChange` is one of its callers rather than a parallel pipeline.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteVsCodeDiffCommand(
		std::string_view argumentsJson);
	//! Runs VS Code's `vscode.open` API command. A working-tree URI opens the real
	//! document; a `git:` URI names committed or staged content, which needs a
	//! read-only editor this product does not have, so it fails closed.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteVsCodeOpenCommand(
		std::string_view argumentsJson);
	//! Runs the built-in Git provider's `git.openChange`. Upstream resolves each
	//! named row's own change command and executes it, so this resolves the row
	//! against the **live** Source Control state and then delegates to the two API
	//! commands above rather than repeating their work.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitOpenChangeCommand(
		std::string_view argumentsJson);
	//! Runs `git.stageSelectedRanges` (`stage == true`) or
	//! `git.unstageSelectedRanges`. Upstream's handlers read the active diff
	//! editor's selections; this reads the diff surface's selected rows, which is
	//! the same operand in the shape this surface can express.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitSelectedRangesCommand(bool stage);
	//! Runs one of the built-in Git provider's remote commands. The repository's
	//! HEAD, upstream, and ahead/behind counts come from the published SCM state,
	//! and its remotes from `git remote --verbose`, exactly as upstream's
	//! `getRemotes` reads them; neither is inferred from the other. The optional
	//! flag accounts for a commit that just succeeded before the asynchronous refresh.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitSyncCommand(
		EGitSyncCommand command, bool commitJustSucceeded = false);
	//! Runs `git.init`. `argumentsJson` is the payload a Source Control welcome-
	//! content link or a Command Palette invocation carries; it decodes to the
	//! `skipFolderPrompt` bool `RunGitInit` takes through
	//! `ParseGitInitSkipFolderPromptArgument`. A successful initialize followed
	//! by an accepted "open the repository?" prompt calls `ApplyFolderWorkspace`
	//! on this window, resolving `EGitInitPostAction::OfferToOpen`'s decision
	//! into the real capability - see `workbench/scm/CLAUDE.md`.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitInitCommand(
		std::string_view argumentsJson);
	//! Runs `git.clone`. Takes no argument, matching upstream's own
	//! zero-parameter handler. Unlike `git.init`, the pure `GitCloneCommandResult`
	//! carries no post-clone "open it?" decision, so this window does not offer
	//! one either - a recorded divergence in `workbench/scm/CLAUDE.md`.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitCloneCommand(bool recurseSubmodules);
	//! Runs `explorer.newFile`/`explorer.newFolder`: starts the Explorer's inline
	//! create row under the resource the payload names. The filesystem write
	//! happens later, in `CommitExplorerCreate`, when the user commits the name.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteExplorerNewEntry(
		std::string_view argumentsJson, bool directory);
	//! Runs `renameFile`: starts the Explorer's inline label edit on the resource
	//! the payload names. The rename itself happens in `CommitExplorerRename`.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteExplorerRenameFile(
		std::string_view argumentsJson);
	//! Runs `moveFileToTrash` and `deleteFile`. They are two registered commands,
	//! never one reading a flag, but their confirmation/delete/error flow is one
	//! shape, so both executors bind here with the trash decision already made.
	//! A declined confirmation is `Succeeded`, as upstream's cancelled dialog
	//! resolves the command without error; a trash failure offers upstream's
	//! permanent-delete fallback prompt before reporting failure.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteExplorerDelete(
		std::string_view argumentsJson, bool useTrash);
	//! Runs `copyFilePath` / `copyRelativeFilePath`: puts the resource's absolute
	//! path, or its label relative to the workspace root, on the clipboard.
	//! `git.copyCommitId` / `git.copyCommitMessage`. `message` selects which of
	//! the commit's two texts is copied; both name the commit by the id in the
	//! payload and read the text from the Graph's own history.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteGitCopyCommitCommand(
		std::string_view argumentsJson, bool message);
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteExplorerCopyPath(
		std::string_view argumentsJson, bool relative);
	//! Runs `revealFileInOS` ("Reveal in File Explorer" on Windows): opens a File
	//! Explorer window with the resource selected.
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteExplorerRevealInOS(
		std::string_view argumentsJson);
	//! Lazily creates the window-owned Win32 file service the Explorer executors
	//! write through. Returns null when creation fails; every caller fails closed.
	[[nodiscard]] platform::filesystem::IFileService* EnsureExplorerFileService();
	//! Terminal for the Explorer's inline rename: validates nothing further (the
	//! tree already applied `IsValidExplorerEntryName`) and renames through the
	//! filesystem boundary without overwrite. Failure surfaces a modal error.
	void CommitExplorerRename(std::wstring_view path, std::wstring_view newName);
	//! Terminal for the Explorer's inline create: makes the directory, or creates
	//! the file atomically-if-missing and opens it pinned, matching upstream's
	//! post-create open. Failure surfaces a modal error.
	void CommitExplorerCreate(std::wstring_view parentDirectory, std::wstring_view name, bool directory);
	//! Projects one resolved comparison onto the native diff surface.
	//!
	//! Like the extension detail surface this is a composition-layer projection
	//! rather than an `EditorInput`, so it is refused while a document input is
	//! active instead of displacing the open document. Showing a diff also
	//! retracts the extension detail surface, mirroring VS Code, where opening an
	//! editor replaces whatever the group was showing.
	[[nodiscard]] bool ShowDiffSurface(SDiffSurfaceContent content);
	//! Retracts the diff surface and restores whichever projection ranks next.
	void ClearDiffSurface();
	//! Re-runs one full client-area layout pass after a projection changed.
	void RelayoutEditorProjections();
	[[nodiscard]] std::optional<std::string> NextWorkbenchLayoutOperationId(std::string_view action);
	[[nodiscard]] std::optional<std::string> NextOutputPanelOperationId();
	[[nodiscard]] bool SetBuiltinPartVisibility(std::string_view partId, bool visible);
	[[nodiscard]] bool SetBuiltinPartExtent(std::string_view partId, int extentDip);
	[[nodiscard]] bool SetBuiltinViewVisibility(std::string_view viewId, bool visible);
	[[nodiscard]] bool ActivateBuiltinWorkbenchView(std::string_view viewId, bool requestFocus);
	[[nodiscard]] bool IsBuiltinWorkbenchViewActive(std::string_view viewId) const;
	//! True when `containerId` is the active ViewContainer of a visible Primary Side Bar.
	[[nodiscard]] bool IsSidebarViewContainerActive(std::string_view containerId) const;
	[[nodiscard]] bool IsAuxiliaryViewContainerActive(std::string_view containerId) const;
	[[nodiscard]] workbench::commands::WorkbenchEditorCommandContext BuildWorkbenchEditorCommandContext() const;
	[[nodiscard]] workbench::commands::WorkbenchScmCommandContext BuildWorkbenchScmCommandContext() const;
	[[nodiscard]] workbench::commands::WorkbenchUpdateCommandContext BuildWorkbenchUpdateCommandContext() const;
	[[nodiscard]] bool RefreshWorkbenchCommandContext();
	//! `argumentsJson` is the invocation's `Command.arguments` payload. Empty is
	//! the argument-less invocation every surface but the SCM view produces, and
	//! a command bound to an argument-less executor ignores it either way.
	[[nodiscard]] bool TryExecuteWorkbenchStableCommand(
		std::string_view commandId, bool& handled, std::string_view argumentsJson = {});
	[[nodiscard]] bool ArmWorkbenchKeybindingChordTimer() noexcept;
	void ClearWorkbenchKeybindingChord() noexcept;
	void ExpireWorkbenchKeybindingChord() noexcept;
	/*!
		@brief 拡張の `contributes.keybindings` を打鍵表へ載せ直す

		キー式の解釈は登録時に一度だけ行い、打鍵ごとには行わない。式が読めない項目は
		ここで落とす。落とした事実は診断出力に残す（黙って効かないのが一番たちが悪い）。
	*/
	[[nodiscard]] bool EnsureQuickInputOverlay();
	[[nodiscard]] bool ShowCommandPalette();
	[[nodiscard]] bool ExecuteToggleSidebarVisibilityCommand();
	[[nodiscard]] bool ExecuteShowExplorerCommand();
	[[nodiscard]] bool ExecuteShowExtensionsCommand();
	[[nodiscard]] bool ExecuteShowProblemsCommand();
	[[nodiscard]] bool ExecuteShowOutputCommand(bool requestFocus = true);
	[[nodiscard]] bool ExecuteToggleOutputCommand();
	//! Applies one supported value of VS Code's workbench.activityBar.location.
	//! Hidden is deliberately absent from the type and from every native surface.
	[[nodiscard]] bool SetActivityBarLocation(workbench::ActivityBarLocation location,
		bool persist);
	[[nodiscard]] workbench::ActivityBarLocation ReadActivityBarLocationSetting() const;
	[[nodiscard]] bool PersistActivityBarLocationSelection(
		workbench::ActivityBarLocation location);
	//! Reprojects both side-bar composite bars and the title-bar GlobalCompositeBar.
	void ApplyActivityBarLocationSetting();
	void PersistWorkbenchExtent(workbench::WorkbenchEdge edge, int extentDip);
	//! Activates one Primary Side Bar ViewContainer, mirroring a VS Code Activity Bar click.
	void ActivateSidebarPage(std::string_view containerId, bool toggleIfActive);
	//! Applies an already-decided container selection to whichever side bar now owns it.
	//! An empty ID renders the empty state.
	void ApplySidebarPage(std::string_view containerId);
	//! Applies an already-decided Secondary Side Bar container selection. Empty for none.
	void ApplyAuxiliaryBarPage(std::string_view containerId);
	//! Refreshes both side-bar titles from the containers they currently render.
	void RefreshSidebarTitles();
	//! Re-resolves Workbench text after the process-wide language resource changes.
	void RefreshLocalizedWorkbenchText();
	//! The side-bar host that currently renders `containerId`, or nullptr when neither does.
	[[nodiscard]] workbench::viewcontainer::CViewContainerHost* HostShowingPage(
		std::string_view containerId) const noexcept;
	//! The Part host that physically contains `host`, or nullptr.
	[[nodiscard]] workbench::CWorkbenchPanelHost* PanelHostFor(
		const workbench::viewcontainer::CViewContainerHost* host) const noexcept;
	//! True when the Explorer container is rendered by a visible side bar whose OUTLINE
	//! view is expanded. Outline is a View, so which Part shows it is not fixed.
	[[nodiscard]] bool IsOutlineViewExpanded() const noexcept;
	//! Applies already-committed Outline visibility to whichever host renders Explorer.
	void SetOutlineExpandedInHosts(bool expanded);
	//! The ViewContainer behind one projected side-bar surface, empty when the surface does
	//! not belong to a side bar at all.
	[[nodiscard]] static std::string_view SidebarPageForActiveSurface(
		workbench::win32::BuiltinActiveSurface surface) noexcept;
	/*!
		@brief Rebuilds the Activity Bar strip and the side-bar page pool from the registry.

		Both follow the registry, so a container an extension contributed gets an icon *and* a
		renderable page without this window knowing that extension exists. Driving them from one
		snapshot keeps the strip and the pool from disagreeing about which containers exist.
		`layoutState` is nullptr before the runtime has committed any layout, in which case every
		projected container is treated as living where its declaration put it: the Primary Side Bar.
	*/
	void SyncViewContainers(const workbench::layout::WorkbenchLayoutStateSnapshot* layoutState);
	/*!
		@brief Publishes the Source Control ViewContainer's number badge.

		Upstream's `scm.contribution.ts` sums `provider.count ?? <resources in the
		provider's groups>` over every published repository and shows that through
		`IActivityService.showViewContainerActivity`. `scm.countBadge` gates it:
		`off` publishes nothing at all, and `focused` is answered as `all` here
		because this fork publishes a single repository, where the two agree by
		construction. The source is the published provider snapshot, never the
		view's own parse, so the badge cannot describe a publication the Source
		Control view has not rendered.
	*/
	void SyncScmActivityBadge();
	//! `scm.countBadge` resolved through the same profile/workspace/folder target
	//! the other settings reads use. Returns the registered default when unset.
	//! The profile/workspace/folder target every workbench settings read uses.
	//! A Folder workspace names its own folder as the workspace identity; the
	//! configuration service rejects a folder target that has no workspace.
	[[nodiscard]] config::ConfigurationTarget BuildWorkbenchConfigurationTarget() const;
	[[nodiscard]] std::wstring ReadScmCountBadgeSetting() const;
	//! Resolves `scm.inputMinLineCount` / `scm.inputMaxLineCount` through the same
	//! profile/workspace/folder target and hands them to the Source Control view.
	void ApplyScmInputLineCountSetting();
	//! Resolves `sakura.terminal.shortcutPreset` and hands it to the terminal panel.
	//! A fork extension, not an upstream key; see terminal/CLAUDE.md.
	void ApplyTerminalShortcutPresetSetting();
	//! Applies terminal.integrated.scrollback to existing and future terminal
	//! models without restarting their PTYs.
	void ApplyTerminalScrollbackSetting();
	//! Reads the terminal.integrated.tabs.* presentation policy once from one
	//! coherent configuration snapshot and pushes plain data to CTerminalTool.
	//! Configuration never reaches TerminalTabManager or the paint path.
	void ApplyTerminalTabPresentationSettings();
	//! Reads the supported editor.minimap.* cohort from one configuration revision.
	void ApplyMiniMapSettings();
	[[nodiscard]] minimap::Options ReadMiniMapSettings() const;
	//! Resolves VS Code's editor-owned `editor.guides.indentation` setting and
	//! applies it to every editor pane; SENP decoration state is not consulted.
	void ApplyIndentGuideSettings();
	bool PersistMiniMapContextSelection(
		minimap::ContextCommand command, const minimap::Options& options);
	void CommitMiniMapOptions(const minimap::Options& options);
	//! Writes the terminal keybinding preset the user picked from the terminal menu
	//! into the profile settings document.
	bool PersistTerminalShortcutPresetSelection(terminal::TerminalShortcutPreset preset);
	//! Resolves `git.decorations.enabled` and the two `explorer.decorations.*` keys
	//! and applies them to the File Explorer's decoration rendering.
	void ApplyExplorerDecorationSettings();
	//! `git.decorations.enabled`. False means the provider publishes nothing at all,
	//! which is upstream's own distinction from rendering a decoration without color.
	bool m_gitDecorationsEnabled = true;
	//! Which physical side bar the pointer is over, if any. VS Code's composite drag and
	//! drop is resolved by the drop target, not by the handle that started the gesture.
	[[nodiscard]] std::optional<workbench::WorkbenchEdge> HitTestSideBarEdge(POINT screenPoint) const;
	//! Runs `workbench.action.moveView` semantics for a container dropped on a side bar.
	void MoveViewContainerToEdge(std::string_view containerId, workbench::WorkbenchEdge edge);
	void ToggleBottomWorkbenchMaximized();
	void SetWorkbenchZoomPercent(int percent);
	[[nodiscard]] bool PreTranslateWorkbenchMessage(MSG& message);
	[[nodiscard]] workbench::CWorkbenchPanelHost* HitTestWorkbenchSplitter(POINT point) const noexcept;
	void CancelWorkbenchResize();
	void PaintWorkbenchSplitters(HDC dc) const;
	[[nodiscard]] bool EnsureMarkdownPreview();
	void CloseMarkdownPreview() noexcept;
	void RefreshMarkdownPreview();
	void UpdateMarkdownPreviewIfNeeded();
	[[nodiscard]] std::wstring GetMarkdownPreviewSource(bool* truncated = nullptr);
	/*!
		@brief Splits the central region between the editor view and the preview

		`minimapWidth` is the minimap's column, which is part of the editor, not a
		frame-level band: VS Code draws the minimap inside the editor group, so a
		side-by-side preview must push it left with the editor rather than leave it
		stranded against the frame. The region passed in therefore includes that
		column, and the placed minimap rectangle is returned for the caller to
		apply.
	*/
	[[nodiscard]] RECT LayoutMarkdownPreview(int left, int top, int right, int bottom,
		unsigned int dpi, int minimapWidth, bool minimapOnLeft);
	//! True when the point is on the Markdown preview divider, with VS Code's sash hit slop.
	[[nodiscard]] bool HitTestMarkdownPreviewDivider(POINT point) const noexcept;
	void CommitMarkdownPreviewResize();
	void CancelMarkdownPreviewResize();
	void AbortMarkdownPreviewResize() noexcept;
	//! Re-runs the frame layout while the divider is being dragged.
	void RelayoutForMarkdownPreviewDivider();
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ExecuteMarkdownPreviewCommand(
		markdown::MarkdownPreviewCommand command);
	[[nodiscard]] workbench::commands::WorkbenchCommandExecutionResult ApplyMarkdownPreviewCommandResult(
		const markdown::MarkdownPreviewCommandResult& result);

	//共有データ
	DLLSHAREDATA*	m_pShareData = &GetDllShareData();

	//ドキュメント
	CEditDoc* 		m_pcEditDoc = &GetEditDoc();
	// Non-owning migration seams composed by CEditApp. Unit-only CEditWnd instances
	// may leave them null and retain the legacy-only behavior.
	workbench::editor::CEditorServiceLegacyAdapter* m_editorServiceAdapter = nullptr;
	workbench::editor::CEditDocLegacyEditorBackend* m_legacyEditorBackend = nullptr;
	workbench::editor::EditorWorkingCopyCoordinator* m_workingCopyCoordinator = nullptr;
	workbench::editor::persistence::EditorWorkingCopyLifecycleBridge* m_workingCopyLifecycleBridge = nullptr;
	workbench::IWorkbenchRuntime* m_workbenchRuntime = nullptr;
	//! A shared callback-only gate lets service notifications outlive this window
	//! without retaining or dereferencing CEditWnd from a model callback thread.
	struct WorkbenchServiceProjectionGate;
	std::shared_ptr<WorkbenchServiceProjectionGate> m_workbenchServiceProjectionGate;
	//! Configuration callbacks never touch CEditWnd directly; they coalesce a
	//! message through this gate and are disconnected before workbench teardown.
	struct ThemeConfigurationGate;
	std::shared_ptr<ThemeConfigurationGate> m_themeConfigurationGate;
	std::unique_ptr<config::ConfigurationSubscription> m_themeConfigurationSubscription;
	//! Running-only borrows from m_workbenchRuntime. They are released after the
	//! gate is disconnected and their exact subscriptions are removed.
	workbench::problems::MarkerService* m_markerService = nullptr;
	std::optional<workbench::problems::MarkerSubscriptionId> m_markerSubscriptionId;
	workbench::output::IOutputService* m_outputService = nullptr;
	std::optional<workbench::output::OutputServiceSubscriptionId> m_outputSubscriptionId;
	//! The window's own update stack. Owned rather than borrowed because the
	//! update service has no runtime owner: it is composed from configuration and
	//! the build's own identity, both of which are available per window.
	std::unique_ptr<update::UpdateComposition> m_updateComposition;
	//! Update notifications arrive on the service's worker thread; like the
	//! service-projection gate this one only ever posts to an HWND.
	struct UpdateStateGate;
	std::shared_ptr<UpdateStateGate> m_updateStateGate;
	std::optional<update::UpdateServiceSubscriptionId> m_updateSubscriptionId;
	//! The last committed `updateState` id, read and written on the UI thread
	//! only. It is the single source the context projection, the title-bar
	//! indicator, and the gear entry all derive from, so they cannot disagree.
	std::string m_updateStateId{ "uninitialized" };

	//自ウィンドウ
	HWND			m_hWnd = nullptr;
	std::unique_ptr<CCustomFrameController> m_customFrame;
	bool			m_dispatchReady = false;
	//! True while a wheel message is synchronously forwarded to a hovered child.
	//! A child that declines the message may let DefWindowProc propagate it back
	//! to this frame; that propagated message must terminate instead of being
	//! forwarded to the same child again.
	bool			m_mouseWheelForwarding = false;
	std::unique_ptr<workbench::CWorkspaceContext> m_workspaceContext;
	//! Window-local, bounded discovery for the read-only Account popup. The
	//! service owns its worker and is stopped before the frame callback and
	//! workspace context are torn down.
	std::unique_ptr<workbench::account::AccountDiscoveryService> m_accountDiscoveryService;
	std::unique_ptr<workbench::editor::CEmptyEditorSurface> m_emptyEditorSurface;
	//! Native side-by-side comparison surface. This is a
	//! composition-layer projection rather than an `EditorInput`, so it may be visible only
	//! while the native editor has no active document.
	std::unique_ptr<CDiffSurface> m_diffSurface;
	//! Where the comparison on the diff surface came from, retained so a selection
	//! can be staged. Only the three strings are kept: the text itself lives in the
	//! surface, and a second copy could describe a comparison the screen replaced.
	//! All three are empty exactly when no comparison is shown.
	std::wstring m_diffRepositoryRoot;
	std::wstring m_diffOriginalUri;
	std::wstring m_diffModifiedUri;
	std::unique_ptr<workbench::editor::IEditorCoreSubscription> m_editorCoreSubscription;
	std::unique_ptr<workbench::layout::IWorkbenchLayoutSubscription> m_layoutStateSubscription;
	//! Window-local command/context boundary; only initialized for runtime-backed workbench windows.
	std::unique_ptr<workbench::commands::WorkbenchContextKeyService> m_workbenchContextKeyService;
	std::unique_ptr<workbench::commands::WorkbenchCommandRegistry> m_workbenchCommandRegistry;
	//! Window-owned Win32 file service for the Explorer file-operation commands.
	//! Created lazily on first use because `IWorkbenchRuntime` exposes no file
	//! service and a window that never runs an Explorer command needs none.
	std::unique_ptr<platform::filesystem::IFileService> m_explorerFileService;
	//! State for the VS Code Windows File-menu keybindings, including Ctrl+K chords.
	workbench::editor::WorkbenchKeybindingState m_workbenchKeybindingState;
	//! The native dynamic-command IDs are resolved against this popup/menu-open
	//! snapshot, never against mutable legacy CMRU state.
	std::vector<workbench::recent::RecentlyOpenedWorkspaceEntry> m_recentlyOpenedWorkspaceMenuSnapshot;
	//! Presentation cache derived only by ApplyEditorCoreSnapshot.
	bool m_hasActiveEditorInput = false;
	bool m_editorCorePresentationInitialized = false;
	//! One unpinned Explorer preview is replaceable by the next single-click.
	//! Double-clicking or editing clears this state, matching VS Code editor groups.
	bool m_explorerPreviewEditor = false;
	//! Set only while a coordinator backend call may synchronously raise OnAfterSave.
	//! The RAII scope restores it on every completion path, including a legacy exception.
	bool m_workingCopyBackendEffectInProgress = false;
	//! Set only after the real legacy save/discard/cancel preflight accepted a
	//! replacement.  The subsequent close consumes it exactly once so the user is
	//! never prompted twice after the successor has acknowledged ready.
	bool m_workspaceReplacementClosePreflightAccepted = false;
	//! Load listeners form one synchronous native replacement transaction. The old
	//! persistence token is captured before CLoadAgent mutates CEditDoc and completed
	//! only after the new native document is atomically reflected in Editor Core.
	std::unique_ptr<workbench::editor::persistence::EditorWorkingCopyCompletionToken>
		m_pendingLoadCompletionToken;
	bool m_pendingLoadHadActiveInput = false;
	bool m_pendingLoadReachedAfter = false;
	bool m_pendingLoadPrearmed = false;
	std::uint64_t m_editorOperationSequence = 0;
	std::uint64_t m_workbenchLayoutOperationSequence = 0;
	std::uint64_t m_outputPanelOperationSequence = 0;
	std::unique_ptr<workbench::CActivityBar> m_activityBar;
	//! Horizontal top/bottom placement renders an independent composite bar for
	//! the Secondary Side Bar; default placement leaves this HWND empty/hidden.
	std::unique_ptr<workbench::CActivityBar> m_auxiliaryActivityBar;
	workbench::ActivityBarLocation m_activityBarLocation{};
	std::unique_ptr<workbench::CWorkbenchPanelHost> m_leftWorkbenchPanel;
	std::unique_ptr<workbench::CWorkbenchPanelHost> m_rightWorkbenchPanel;
	std::unique_ptr<workbench::CWorkbenchPanelHost> m_bottomWorkbenchPanel;
	std::unique_ptr<workbench::quickinput::CCommandPaletteOverlay> m_commandPaletteOverlay;
	//! Invalidated before workbench teardown so deferred Git Quick Input
	//! continuations cannot call back into a destroyed CEditWnd.
	std::shared_ptr<std::atomic_bool> m_gitBranchCommandSession;
	//! Built-in color themes available to this window.
	std::unique_ptr<theme::CColorThemeRegistry> m_colorThemeRegistry;
	bool m_startupOutlineReloadPending = false;
	bool m_startupWorkbenchCompletionPosted = false;
	//! Both side bars borrow their ViewContainer controls from this shared pool, so a
	//! container survives being moved from one physical Part to the other.
	std::shared_ptr<workbench::viewcontainer::CViewContainerPages> m_viewContainerPages;
	//! Opaque window-owned frame bridge. The concrete state includes the bounded
	//! finalizer reservation and stays out of this composition header.
	std::unique_ptr<CEditWndFrameRuntimeState> m_frameRuntimeState;
	workbench::viewcontainer::CViewContainerHost* m_sidebarHost = nullptr;
	workbench::viewcontainer::CViewContainerHost* m_auxiliaryBarHost = nullptr;
	workbench::explorer::CExplorerTool* m_explorerTool = nullptr;
	workbench::outline::COutlineWorkbenchTool* m_outlineWorkbenchTool = nullptr;
	workbench::scm::CScmWorkbenchTool* m_scmTool = nullptr;
	workbench::search::CSearchWorkbenchTool* m_searchTool = nullptr;
	workbench::extensions::CExtensionsWorkbenchTool* m_extensionsTool = nullptr;
	terminal::CTerminalTool* m_terminalTool = nullptr;
	workbench::panel::CBottomPanelTool* m_bottomPanelTool = nullptr;
	std::unique_ptr<markdown::CMarkdownPreviewWnd> m_markdownPreview;
	markdown::MarkdownPreviewCommandState m_markdownPreviewCommandState;
	bool m_markdownPreviewVisible = false;
	//! Reentrancy guard shared by both directions of the Markdown scroll sync.
	bool m_markdownPreviewScrollSyncing = false;
	//! Last source line pushed to the preview, so an unchanged scroll costs nothing.
	int m_markdownPreviewSyncedSourceLine = -1;
	bool m_markdownPreviewDirty = false;
	int m_markdownPreviewRevision = -1;
	std::uint64_t m_markdownPreviewGeneration = 0;
	RECT m_markdownPreviewDivider{};
	//! The user's dragged preview width; kPreviewDefaultWidthRequestDip until dragged.
	int m_markdownPreviewWidthDip = markdown::kPreviewDefaultWidthRequestDip;
	//! Width used only by the active drag; committed on mouse-up and discarded on cancel.
	int m_markdownPreviewResizeWidthDip = markdown::kPreviewDefaultWidthRequestDip;
	//! The region the divider splits, kept for the drag arithmetic and its repaint.
	RECT m_markdownPreviewRegion{};
	//! The minimap column cached by the committed frame layout for preview-only drag samples.
	int m_markdownPreviewMinimapWidth = 0;
	bool m_markdownPreviewMinimapOnLeft = false;
	bool m_resizingMarkdownPreview = false;
	bool m_layoutInProgress = false;
	bool m_layoutPending = false;
	WPARAM m_pendingLayoutWParam = SIZE_RESTORED;
	LPARAM m_pendingLayoutLParam = 0;
	bool m_pendingLayoutUpdateStatus = false;
	StartupDrawState m_startupDrawState = StartupDrawState::Inactive;
	bool m_startupSavedDrawSwitch = true;
	bool m_startupCommitLayoutAllowed = false;
	bool m_startupFirstContentPainted = false;
	bool m_startupMiniMapSummaryEmitted = false;
	std::int64_t m_startupMiniMapPaintQpcTicks = 0;
	std::int64_t m_startupMiniMapPaintCount = 0;
	std::int64_t m_startupMiniMapImmediateUpdateCount = 0;
	int m_startupShowCommand = SW_SHOW;
	HWND m_startupPreviousTabWindow = nullptr;
	workbench::CWorkbenchPanelHost* m_resizingWorkbenchPanel = nullptr;
	//! A layout notification observed inside a resize paint is applied at the
	//! gesture's explicit commit/cancel terminal state, never by cancelling it.
	bool m_workbenchLayoutProjectionDeferred = false;
	POINT m_workbenchResizeOrigin{};
	int m_workbenchResizeInitialExtentDip = 0;
	RECT m_leftWorkbenchSplitter{};
	RECT m_rightWorkbenchSplitter{};
	RECT m_bottomWorkbenchSplitter{};
	std::optional<std::array<RECT, 4>> m_appliedWorkbenchHostGeometry;
	bool m_bottomWorkbenchMaximized = false;
	int m_workbenchZoomPercent = 100;
	int m_workbenchZoomBasePointSize = 0;

public:
	/*!
		@brief The opened root folder's display name, empty when no folder is open

		This is VS Code's `${rootName}` caption variable: the folder's own name,
		never its full path.
	*/
	[[nodiscard]] std::wstring GetWorkspaceRootName() const;
	//子ウィンドウ
	CMainToolBar	m_cToolbar{ this };			//!< ツールバー
	CTabWnd			m_cTabWnd;			//!< タブウインドウ	//@@@ 2003.05.31 MIK
	CFuncKeyWnd		m_cFuncKeyWnd;		//!< ファンクションバー
	CMainStatusBar	m_cStatusBar{ this };		//!< ステータスバー
	CPrintPreviewHolder	m_pPrintPreview = nullptr;	//!< 印刷プレビュー表示情報。必要になったときのみインスタンスを生成する。

	CSplitterWnd	m_cSplitterWnd;		//!< 分割フレーム
	CEditView*		m_pcDragSourceView = nullptr;	//!< ドラッグ元のビュー
	CViewFontHolder		m_pcViewFont = std::make_unique<CViewFont>(&GetLogfont());		//!< フォント
	CViewFontHolder		m_pcViewFontMiniMap = std::make_unique<CViewFont>(&GetLogfont(), true);		//!< フォント

	//ダイアログ達
	CDlgFind		m_cDlgFind;			// 「検索」ダイアログ
	CDlgReplace		m_cDlgReplace;		// 「置換」ダイアログ
	CDlgJump		m_cDlgJump;			// 「指定行へジャンプ」ダイアログ
	CDlgGrep		m_cDlgGrep;			// Grepダイアログ
	CDlgGrepReplace	m_cDlgGrepReplace;	// Grep置換ダイアログ
	CDlgFuncList	m_cDlgFuncList;		// アウトライン解析結果ダイアログ
	CHokanMgr		m_cHokanMgr;		// 入力補完
	CDlgSetCharSet	m_cDlgSetCharSet;	// 「文字コードセット設定」ダイアログ

private:
	// 2010.04.10 Moca  public -> private. 起動直後は[0]のみ有効 4つとは限らないので注意
	CEditViewsArray	m_pcEditViewArr{};	//!< ビュー
	CEditView*		m_pcEditView;		//!< 有効なビュー
	CMiniMapView	m_cMiniMapView;		//!< ミニマップ
	minimap::Options m_miniMapOptions{};
	bool m_indentGuidesEnabled = true;
	int				m_nActivePaneIndex = 0;	//!< 有効なビューのindex
	int				m_nEditViewCount = 1;	//!< 有効なビューの数
	const int		m_nEditViewMaxCount = int(std::size(m_pcEditViewArr));//!< ビューの最大数=4

	//ヘルパ
	CMenuDrawer		m_cMenuDrawer;

	//状態
	bool			m_bIsActiveApp = false;		//!< 自アプリがアクティブかどうか	// 2007.03.08 ryoji
	LPWSTR			m_pszLastCaption = nullptr;
	SMenubarMessage m_pszMenubarMessage;		//!< メニューバー右端に表示するメッセージ
public:
	int				m_nTimerCount;		//!< OnTimer用 2003.08.29 wmlhq
	CLogicPointEx*	m_posSaveAry = nullptr;		//!< フォント変更前の座標
private:
	int				m_nCurrentFocus = 0;	//!< 現在のフォーカス情報
	int				m_nWinSizeType = SIZE_RESTORED;	//!< サイズ変更のタイプ。SIZE_MAXIMIZED, SIZE_MINIMIZED 等。
	BOOL			m_bPageScrollByWheel;		//!< ホイール操作によるページスクロールあり	// 2009.01.17 nasukoji
	BOOL			m_bHorizontalScrollByWheel;	//!< ホイール操作による横スクロールあり		// 2009.01.17 nasukoji
	AccelHolder		m_hAccel = nullptr;			//!< ウィンドウ毎のアクセラレータテーブルのハンドル

	//フォント・イメージ
	FontHolder		m_hFontCaretPosInfo = nullptr;	//!< キャレットの行桁位置表示用フォント
	int				m_nCaretPosInfoCharWidth;	//!< キャレットの行桁位置表示用フォントの幅
	int				m_nCaretPosInfoCharHeight;	//!< キャレットの行桁位置表示用フォントの高さ

	//D&Dフラグ
	bool			m_bDragMode = false;
	CMyPoint		m_ptDragPosOrg;
	CDropTargetHolder	m_pcDropTarget = std::make_unique<CDropTarget>(this);	//!< 右ボタンドロップ用

	//その他フラグ
	EIconClickStatus	m_IconClicked = icNone;

public:
	ESelectCountMode	m_nSelectCountMode = SELECT_COUNT_TOGGLE; // 選択文字カウント方法
};

CEditWnd* GetEditWndPtr() noexcept;
CEditWnd& GetEditWnd();

#endif /* SAKURA_CEDITWND_6C771A35_3CC8_4932_BF15_823C40487A9F_H_ */
