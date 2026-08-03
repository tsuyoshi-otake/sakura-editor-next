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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"

static const int MENUBAR_MESSAGE_MAX_LEN = 30;

class CPlug;
class CEditDoc;
class CCustomFrameController;
class CExtensionService;
class IExtensionSecretSessionStorage;
struct SExtensionNativeEditorOptions;
class CExtensionViewRegistry;
class CExtensionDetailSurface;
namespace config {
class ConfigurationSubscription;
}
struct SExtensionDiagnostic;
struct SExtensionDocumentSnapshot;
struct SExtensionDocumentEdit;
struct SExtensionApplyEditResult;
struct DLLSHAREDATA;
namespace terminal {
class CTerminalTool;
}
namespace theme {
class CColorThemeRegistry;
}
namespace markdown {
class CMarkdownPreviewWnd;
}
namespace workbench {
class CActivityBar;
class IWorkbenchRuntime;
class CWorkbenchPanelHost;
class CWorkspaceContext;
enum class WorkbenchEdge : std::uint8_t;
namespace layout {
class IWorkbenchLayoutSubscription;
struct WorkbenchLayoutStateSnapshot;
}
namespace icons {
class CExtensionIconFontRegistry;
class CFileIconThemeRegistry;
}
namespace win32 {
struct BuiltinActiveSurfaceProjection;
enum class BuiltinActiveSurface : std::uint8_t;
}
namespace commands {
class WorkbenchCommandRegistry;
}
namespace explorer {
class CExplorerTool;
}
namespace viewcontainer {
class CViewContainerHost;
class CViewContainerPages;
enum class ViewContainerPage : std::uint8_t;
}
namespace outline {
class COutlineWorkbenchTool;
}
namespace scm {
class CScmWorkbenchTool;
}
namespace extension {
class CExtensionBottomPanelTool;
class CExtensionSidebarTool;
}
namespace notification {
class CNotificationHost;
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
struct SLegacyEditorFunctionCommand final {
	EFunctionCode functionCode = F_0;
	bool redraw = true;
	LPARAM lparam1 = 0;
	LPARAM lparam2 = 0;
	LPARAM lparam3 = 0;
	LPARAM lparam4 = 0;
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
	DirtyPreflightFailed,
	HandoffFailed,
	WorkspaceContextFailed,
	ExplorerProjectionFailed,
};

//! Every native workspace picker/handoff path has one terminal result.  The
//! window owns picker and process composition; the workbench service remains
//! HWND-free.
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
		workbench::IWorkbenchRuntime& workbenchRuntime,
		std::filesystem::path profileDirectory,
		std::unique_ptr<IExtensionSecretSessionStorage> extensionSecretStorage);
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
	[[nodiscard]] bool IsWorkbenchPanelVisible(workbench::WorkbenchEdge edge) const noexcept;
	//! `workbench.action.toggleAuxiliaryBar` (Ctrl+Alt+B). This is the physical Secondary
	//! Side Bar Part, never the Outline View nested in the Primary Side Bar.
	void ToggleSecondarySidebar(bool activate = false);
	[[nodiscard]] bool IsSecondarySidebarVisible() const noexcept;
	void FocusIntegratedTerminal();
	void NewIntegratedTerminal();
	void RedetectPowerShell();
	void ToggleMarkdownPreview();
	//! Show the VS Code-compatible command palette and dispatch the selected extension command.
	[[nodiscard]] bool ShowExtensionCommandPalette();
	[[nodiscard]] bool IsMarkdownPreviewVisible() const noexcept;
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

	//ビュー
	const CEditView&	GetActiveView() const { return *m_pcEditView; }
	CEditView&			GetActiveView()       { return *m_pcEditView; }
	const CEditView&    GetView(int n) const { return *m_pcEditViewArr[n]; }
	CEditView&          GetView(int n)       { return *m_pcEditViewArr[n]; }
	CMiniMapView&       GetMiniMap( void ) { return m_cMiniMapView; }
	//! Diagnostics published for the file currently owned by this editor process.
	[[nodiscard]] std::vector<SExtensionDiagnostic> ExtensionDiagnosticsForCurrentDocument() const;
	//! Called after a native undo unit is finalized; schedules one bounded document snapshot.
	void NotifyExtensionDocumentChanged();

	//! Reveals `workbench.view.extensions`, matching VS Code's reveal-only `workbench.view.*`.
	void ShowExtensionsViewContainer();
	//! True while the Extensions ViewContainer is the active, visible container of its Part.
	[[nodiscard]] bool IsExtensionsViewContainerActive() const;

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
	void EnsureNotificationHost() noexcept;
	void PostDeferredStartupWorkbenchIfReady();
	void CompleteDeferredStartupWorkbench();
	void CloseWorkbench() noexcept;
	void ApplyWorkbenchTheme();
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
	void FinalizeWorkbenchPanelProjection(
		const workbench::win32::BuiltinActiveSurfaceProjection* runtimeProjection = nullptr);
	void ReloadWorkbenchOutlineAndRelayout();
	void BroadcastWorkbenchSettings();
	[[nodiscard]] std::wstring GetSemanticWorkspaceRoot() const;
	void ApplySemanticWorkspaceContext();
	void UpdateWorkspaceFromDocument();
	void RefreshEditorCorePresentation();
	void ApplyEditorCoreSnapshot(const workbench::editor::EditorCoreSnapshot& snapshot, bool restoreFocus = true);
	[[nodiscard]] bool AdoptLoadedLegacyFile();
	[[nodiscard]] bool FinalizeSuccessfulLegacyLoad();
	[[nodiscard]] bool CreateUntitledEditorInput();
	[[nodiscard]] bool CloseActiveEditorInput();
	void ConfigureCustomFrameActions();
	[[nodiscard]] bool ExecuteWorkbenchEditorCommand(std::string_view commandId);
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
	void ClosePublishedExtensionDocument();
	[[nodiscard]] std::string NextEditorOperationId(std::string_view prefix);
	[[nodiscard]] bool ActiveInputMatchesCurrentFile() const;
	[[nodiscard]] SExtensionDocumentSnapshot CaptureExtensionDocumentSnapshot(std::uint64_t version) const;
	void PublishExtensionDocumentOpen(bool forceReopen);
	void PublishExtensionDocumentChange();
	void PublishExtensionDocumentSave();
	void PublishExtensionActiveEditor();
	/*!
		@brief 導入済み拡張の contributes.icons を読み直し、フォントを登録する

		拡張の列挙は CExtensionService::WorkerInitialize() と同じ規約
		（CExtensionManager::EnumInstalled() の各 dir/"extension" にマニフェストが
		あるものだけ、ID は sUniqueId）で行うが、こちらは UI スレッド専用である。
		ワーカー専用状態である m_installedRoots は決して読まない。
	*/
	void RefreshExtensionIconFonts();
	//! Rebuilds the window-local registry from the profile's enabled VSIX roots.
	void RefreshColorThemes();
	//! Rebuilds the file-icon-theme registry from the profile's enabled VSIX roots.
	void RefreshFileIconThemes();
	//! Shows the native equivalent of VS Code's Preferences: Color Theme picker.
	[[nodiscard]] bool ShowColorThemePicker();
	[[nodiscard]] bool PersistColorThemeSelection(std::wstring_view themeId);
	//! Shows the native equivalent of VS Code's Preferences: File Icon Theme picker.
	[[nodiscard]] bool ShowFileIconThemePicker();
	[[nodiscard]] bool PersistFileIconThemeSelection(std::wstring_view themeId);
	//! Applies the selected file icon theme to the native Explorer control.
	void ApplyFileIconTheme();
	SExtensionApplyEditResult ApplyExtensionEdits(
		const std::vector<SExtensionDocumentEdit>& edits,
		std::vector<SExtensionDocumentSnapshot>& snapshots);
	bool ApplyExtensionEditorOptions(const SExtensionNativeEditorOptions& options);
	[[nodiscard]] std::optional<std::string> NextWorkbenchLayoutOperationId(std::string_view action);
	[[nodiscard]] std::optional<std::string> NextOutputPanelOperationId();
	[[nodiscard]] bool SetBuiltinPartVisibility(std::string_view partId, bool visible);
	[[nodiscard]] bool SetBuiltinPartExtent(std::string_view partId, int extentDip);
	[[nodiscard]] bool SetBuiltinViewVisibility(std::string_view viewId, bool visible);
	[[nodiscard]] bool ActivateBuiltinWorkbenchView(std::string_view viewId, bool requestFocus);
	[[nodiscard]] bool IsBuiltinWorkbenchViewActive(std::string_view viewId) const;
	//! Stable VS Code ViewContainer ID rendered by one side-bar page.
	[[nodiscard]] static std::string_view SidebarViewContainerId(
		workbench::viewcontainer::ViewContainerPage page) noexcept;
	//! Inverse of SidebarViewContainerId; std::nullopt for a container this adapter cannot render.
	[[nodiscard]] static std::optional<workbench::viewcontainer::ViewContainerPage>
		ViewContainerPageForId(std::string_view containerId) noexcept;
	//! True when `containerId` is the active ViewContainer of a visible Primary Side Bar.
	[[nodiscard]] bool IsSidebarViewContainerActive(std::string_view containerId) const;
	[[nodiscard]] workbench::commands::WorkbenchEditorCommandContext BuildWorkbenchEditorCommandContext() const;
	[[nodiscard]] bool RefreshWorkbenchCommandContext();
	[[nodiscard]] bool TryExecuteWorkbenchStableCommand(std::string_view commandId, bool& handled);
	[[nodiscard]] bool ArmWorkbenchKeybindingChordTimer() noexcept;
	void ClearWorkbenchKeybindingChord() noexcept;
	void ExpireWorkbenchKeybindingChord() noexcept;
	[[nodiscard]] bool ExecuteToggleSidebarVisibilityCommand();
	[[nodiscard]] bool ExecuteShowExplorerCommand();
	[[nodiscard]] bool ExecuteShowProblemsCommand();
	[[nodiscard]] bool ExecuteShowOutputCommand(bool requestFocus = true);
	[[nodiscard]] bool ExecuteToggleOutputCommand();
	void PersistWorkbenchExtent(workbench::WorkbenchEdge edge, int extentDip);
	void PersistExtensionViewsExtent(int extentDip);
	//! Activates one Primary Side Bar ViewContainer, mirroring a VS Code Activity Bar click.
	void ActivateSidebarPage(workbench::viewcontainer::ViewContainerPage page, bool toggleIfActive);
	//! Applies an already-decided container selection to whichever side bar now owns it.
	void ApplySidebarPage(workbench::viewcontainer::ViewContainerPage page);
	//! Applies an already-decided Secondary Side Bar container selection.
	void ApplyAuxiliaryBarPage(std::optional<workbench::viewcontainer::ViewContainerPage> page);
	//! Refreshes both side-bar titles from the containers they currently render.
	void RefreshSidebarTitles();
	//! The side-bar host that currently renders `page`, or nullptr when neither does.
	[[nodiscard]] workbench::viewcontainer::CViewContainerHost* HostShowingPage(
		workbench::viewcontainer::ViewContainerPage page) const noexcept;
	//! The Part host that physically contains `host`, or nullptr.
	[[nodiscard]] workbench::CWorkbenchPanelHost* PanelHostFor(
		const workbench::viewcontainer::CViewContainerHost* host) const noexcept;
	//! True when the Explorer container is rendered by a visible side bar whose OUTLINE
	//! view is expanded. Outline is a View, so which Part shows it is not fixed.
	[[nodiscard]] bool IsOutlineViewExpanded() const noexcept;
	//! Applies already-committed Outline visibility to whichever host renders Explorer.
	void SetOutlineExpandedInHosts(bool expanded);
	//! The ViewContainer behind one projected side-bar surface, or std::nullopt when the
	//! surface does not belong to a side bar at all.
	[[nodiscard]] static std::optional<workbench::viewcontainer::ViewContainerPage>
		SidebarPageForActiveSurface(workbench::win32::BuiltinActiveSurface surface) noexcept;
	//! Adds or removes Activity Bar entries so only Primary Side Bar containers have one.
	void SyncActivityBarEntries(const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot);
	//! Which physical side bar the pointer is over, if any. VS Code's composite drag and
	//! drop is resolved by the drop target, not by the handle that started the gesture.
	[[nodiscard]] std::optional<workbench::WorkbenchEdge> HitTestSideBarEdge(POINT screenPoint) const;
	//! Runs `workbench.action.moveView` semantics for a container dropped on a side bar.
	void MoveViewContainerToEdge(workbench::viewcontainer::ViewContainerPage page,
		workbench::WorkbenchEdge edge);
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
	void LayoutMarkdownPreview(int left, int top, int right, int bottom, unsigned int dpi);

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
	workbench::output::OutputService* m_outputService = nullptr;
	std::optional<workbench::output::OutputServiceSubscriptionId> m_outputSubscriptionId;

	//自ウィンドウ
	HWND			m_hWnd = nullptr;
	std::unique_ptr<CCustomFrameController> m_customFrame;
	bool			m_dispatchReady = false;
	std::unique_ptr<workbench::CWorkspaceContext> m_workspaceContext;
	std::unique_ptr<workbench::editor::CEmptyEditorSurface> m_emptyEditorSurface;
	std::unique_ptr<CExtensionDetailSurface> m_extensionDetailSurface;
	std::unique_ptr<workbench::editor::IEditorCoreSubscription> m_editorCoreSubscription;
	std::unique_ptr<workbench::layout::IWorkbenchLayoutSubscription> m_layoutStateSubscription;
	//! Window-local command/context boundary; only initialized for runtime-backed workbench windows.
	std::unique_ptr<workbench::commands::WorkbenchContextKeyService> m_workbenchContextKeyService;
	std::unique_ptr<workbench::commands::WorkbenchCommandRegistry> m_workbenchCommandRegistry;
	//! State for the VS Code Windows File-menu keybindings, including Ctrl+K chords.
	workbench::editor::WorkbenchKeybindingState m_workbenchKeybindingState;
	//! The native dynamic-command IDs are resolved against this popup/menu-open
	//! snapshot, never against mutable legacy CMRU state.
	std::vector<workbench::recent::RecentlyOpenedWorkspaceEntry> m_recentlyOpenedWorkspaceMenuSnapshot;
	//! Presentation cache derived only by ApplyEditorCoreSnapshot.
	bool m_hasActiveEditorInput = false;
	bool m_editorCorePresentationInitialized = false;
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
	std::unique_ptr<workbench::CWorkbenchPanelHost> m_leftWorkbenchPanel;
	std::unique_ptr<workbench::CWorkbenchPanelHost> m_rightWorkbenchPanel;
	std::unique_ptr<workbench::CWorkbenchPanelHost> m_bottomWorkbenchPanel;
	std::filesystem::path m_extensionProfileDirectory;
	std::unique_ptr<IExtensionSecretSessionStorage> m_extensionSecretStorage;
	std::unique_ptr<CExtensionService> m_extensionService;
	std::unique_ptr<workbench::notification::CNotificationHost> m_notificationHost;
	std::unique_ptr<workbench::quickinput::CCommandPaletteOverlay> m_commandPaletteOverlay;
	//! 導入済み拡張の contributes.icons。ステータスバーは非所有ポインタで借りるので、
	//! ここが唯一の所有者であり、m_cStatusBar より長く生きなければならない。
	std::unique_ptr<workbench::icons::CExtensionIconFontRegistry> m_extensionIconFonts;
	//! Parsed contributes.themes entries from the same enabled extension set.
	std::unique_ptr<theme::CColorThemeRegistry> m_colorThemeRegistry;
	//! Parsed contributes.iconThemes entries from the same enabled extension set.
	std::unique_ptr<workbench::icons::CFileIconThemeRegistry> m_fileIconThemeRegistry;
	std::wstring m_extensionDocumentUri;
	std::uint64_t m_extensionDocumentVersion = 0;
	bool m_extensionDocumentSyncTimerPending = false;
	bool m_startupOutlineReloadPending = false;
	bool m_startupExtensionDocumentOpenPending = false;
	bool m_startupWorkbenchCompletionPosted = false;
	std::shared_ptr<CExtensionViewRegistry> m_extensionViewRegistry;
	workbench::extension::CExtensionSidebarTool* m_extensionSidebarTool = nullptr;
	workbench::extension::CExtensionBottomPanelTool* m_extensionBottomPanelTool = nullptr;
	//! Both side bars borrow their ViewContainer controls from this shared pool, so a
	//! container survives being moved from one physical Part to the other.
	std::shared_ptr<workbench::viewcontainer::CViewContainerPages> m_viewContainerPages;
	workbench::viewcontainer::CViewContainerHost* m_sidebarHost = nullptr;
	workbench::viewcontainer::CViewContainerHost* m_auxiliaryBarHost = nullptr;
	workbench::explorer::CExplorerTool* m_explorerTool = nullptr;
	workbench::outline::COutlineWorkbenchTool* m_outlineWorkbenchTool = nullptr;
	workbench::scm::CScmWorkbenchTool* m_scmTool = nullptr;
	terminal::CTerminalTool* m_terminalTool = nullptr;
	std::unique_ptr<markdown::CMarkdownPreviewWnd> m_markdownPreview;
	bool m_markdownPreviewVisible = false;
	bool m_markdownPreviewDirty = false;
	int m_markdownPreviewRevision = -1;
	RECT m_markdownPreviewDivider{};
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
	POINT m_workbenchResizeOrigin{};
	int m_workbenchResizeInitialExtentDip = 0;
	RECT m_leftWorkbenchSplitter{};
	RECT m_rightWorkbenchSplitter{};
	RECT m_bottomWorkbenchSplitter{};
	bool m_bottomWorkbenchMaximized = false;
	int m_workbenchZoomPercent = 100;
	int m_workbenchZoomBasePointSize = 0;

public:
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
