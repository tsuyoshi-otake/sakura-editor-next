/*!	@file
	@brief アウトライン解析ダイアログボックス

	@author Norio Nakatani
	@date 1998/06/23 新規作成
	@date 1998/12/04 再作成
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2001, genta, hor
	Copyright (C) 2002, aroka, hor, YAZAKI, frozen
	Copyright (C) 2003, little YOSHI
	Copyright (C) 2005, genta
	Copyright (C) 2006, aroka
	Copyright (C) 2007, ryoji
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#ifndef SAKURA_CDLGFUNCLIST_B22A3877_572A_49B7_B683_50ECA451A6F8_H_
#define SAKURA_CDLGFUNCLIST_B22A3877_572A_49B7_B683_50ECA451A6F8_H_
#pragma once

#include <Windows.h>
#include <CommCtrl.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include "dlg/CDialog.h"
#include "doc/CEditDoc.h"
#include "outline/CFuncInfoArr.h"
#include "workbench/outline/OutlineParserWorker.h"
#include "workbench/outline/OutlineRefreshScheduler.h"
#include "workbench/outline/OutlineViewLifecycle.h"

class CFuncInfo;
class CFuncInfoArr; // 2002/2/10 aroka
class CDataProfile;
class CEditView;
class CEditWnd;
class CLoadAgent;

namespace workbench::outline {

[[nodiscard]] DWORD NormalizeWorkbenchOutlineTreeStyle( DWORD style ) noexcept;
[[nodiscard]] DWORD NormalizeWorkbenchOutlineListStyle( DWORD style ) noexcept;
[[nodiscard]] DWORD NormalizeWorkbenchOutlineControlExStyle( DWORD exStyle ) noexcept;
[[nodiscard]] bool NormalizeWorkbenchOutlineDialogTemplate( void* data, size_t size ) noexcept;

} // namespace workbench::outline

//! アウトライン動作指定
#define OUTLINE_LAYOUT_FOREGROUND (0)   //!< 前面用の動作
#define OUTLINE_LAYOUT_BACKGROUND (1)   //!< 背後用の動作
#define OUTLINE_LAYOUT_FILECHANGED (2)  //!< ファイル切替用の動作（前面だが特殊）

//! ツリービューをソートする基準
#define SORTTYPE_DEFAULT       0 //!< デフォルト(ノードに関連づけれられた値順,昇順)
#define SORTTYPE_DEFAULT_DESC  1 //!< デフォルト(ノードに関連づけれられた値順,降順)
#define SORTTYPE_ATOZ          2 //!< アルファベット順(昇順)
#define SORTTYPE_ZTOA          3 //!< アルファベット順(降順)

// ファイルツリー関連クラス
enum EFileTreeSettingFrom{
	EFileTreeSettingFrom_Common,
	EFileTreeSettingFrom_Type,
	EFileTreeSettingFrom_File
};

class CFileTreeSetting{
public:
	std::vector<SFileTreeItem>	m_aItems;		//!< ツリーアイテム
	bool		m_bProject;				//!< プロジェクトファイルモード
	SFilePath	m_szDefaultProjectIni;	//!< デフォルトiniファイル名
	SFilePath	m_szLoadProjectIni;		//!< 現在読み込んでいるiniファイル名
	EFileTreeSettingFrom	m_eFileTreeSettingOrgType;
	EFileTreeSettingFrom	m_eFileTreeSettingLoadType;
};

//!	アウトライン解析ダイアログボックス
class CDlgFuncList final : public CDialog
{
public:
	using WorkbenchNativeSurfaceSubmitter = std::function<void(HDC, const RECT&)>;
	/*
	||  Constructors
	*/
	CDlgFuncList();
	/*
	||  Attributes & Operations
	*/
	HWND DoModeless( HINSTANCE, HWND, LPARAM, CFuncInfoArr*, CLayoutInt, CLayoutInt, int, int, bool );/* モードレスダイアログの表示 */
	void ChangeView(LPARAM pcEditView);	/* モードレス時：検索対象となるビューの変更 */
	void SetWorkbenchParent( HWND parent ) noexcept;
	void SetWorkbenchMode( bool enabled ) noexcept;
	void SetWorkbenchNativeSurfaceSubmitter( WorkbenchNativeSurfaceSubmitter submitter ) noexcept;
	void SetWorkbenchAppearance(
		COLORREF text,
		COLORREF background,
		COLORREF hover,
		COLORREF selection,
		COLORREF selectionText,
		HFONT font,
		int itemHeight,
		HIMAGELIST symbolImages ) noexcept;
	static constexpr int WorkbenchSymbolImageCount() noexcept { return 10; }
	[[nodiscard]] static int WorkbenchSymbolImageIndex( int info ) noexcept;
	[[nodiscard]] static std::wstring_view WorkbenchSymbolCodiconName( int imageIndex ) noexcept;
	[[nodiscard]] HWND GetWorkbenchParent() const noexcept { return m_hwndWorkbenchParent; }
	[[nodiscard]] bool IsWorkbenchMode() const noexcept { return m_bWorkbenchMode; }
	[[nodiscard]] static bool IsAsyncWorkbenchOutlineType( int outlineType ) noexcept;
	[[nodiscard]] workbench::outline::OutlineDocumentVersion GetWorkbenchDocumentVersion() noexcept;
	[[nodiscard]] bool HasCurrentWorkbenchModel() noexcept;
	[[nodiscard]] std::uint64_t GetWorkbenchModelGeneration() const noexcept { return m_workbenchModelGeneration; }
	[[nodiscard]] std::uint64_t GetWorkbenchReparseCount() const noexcept { return m_workbenchReparseCount; }
	[[nodiscard]] const workbench::outline::OutlinePhaseTimings& GetLastWorkbenchTimings() const noexcept { return m_workbenchLastTimings; }
	[[nodiscard]] workbench::outline::OutlineWorkerTerminal GetLastWorkbenchTerminal() const noexcept { return m_workbenchLastTerminal; }
	[[nodiscard]] bool IsDocking() const noexcept { return !m_bWorkbenchMode && m_eDockSide > DOCKSIDE_FLOAT; }
	[[nodiscard]] EDockSide GetDockSide() const noexcept { return m_eDockSide; }

protected:
	INT_PTR DispatchEvent( HWND hWnd, UINT wMsg, WPARAM wParam, LPARAM lParam ) override;	// 2007.11.07 ryoji 標準以外のメッセージを捕捉する

	CommonSetting_OutLine& CommonSet(void){ return m_pShareData->m_Common.m_sOutline; }
	STypeConfig& TypeSet(void){ return m_type; }
	int& ProfDockSet() { return CommonSet().m_nOutlineDockSet; }
	BOOL& ProfDockSync() { return CommonSet().m_bOutlineDockSync; }
	BOOL& ProfDockDisp() { return (ProfDockSet() == 0)? CommonSet().m_bOutlineDockDisp: TypeSet().m_bOutlineDockDisp; }
	EDockSide& ProfDockSide() { return (ProfDockSet() == 0)? CommonSet().m_eOutlineDockSide: TypeSet().m_eOutlineDockSide; }
	int& ProfDockLeft() { return (ProfDockSet() == 0)? CommonSet().m_cxOutlineDockLeft: TypeSet().m_cxOutlineDockLeft; }
	int& ProfDockTop() { return (ProfDockSet() == 0)? CommonSet().m_cyOutlineDockTop: TypeSet().m_cyOutlineDockTop; }
	int& ProfDockRight() { return (ProfDockSet() == 0)? CommonSet().m_cxOutlineDockRight: TypeSet().m_cxOutlineDockRight; }
	int& ProfDockBottom() { return (ProfDockSet() == 0)? CommonSet().m_cyOutlineDockBottom: TypeSet().m_cyOutlineDockBottom; }
	void SetTypeConfig(CTypeConfig docType, const STypeConfig& type);

public:
	/*! 現在の種別と同じなら
	*/
	bool CheckListType( int nOutLineType ) const { return nOutLineType == m_nOutlineType; }
	void Redraw( int nOutLineType, int nListType, CFuncInfoArr*, CLayoutInt nCurLine, CLayoutInt nCurCol );
	void Refresh( void );
	bool ChangeLayout( int nId );
	void OnOutlineNotify( WPARAM wParam, LPARAM lParam );
	void SyncColor( void );
	void SetWindowText( const WCHAR* szTitle );		//ダイアログタイトルの設定
	EFunctionCode GetFuncCodeRedraw(int outlineType);
	void LoadFileTreeSetting( CFileTreeSetting& data, SFilePath& IniDirPath );
	void NotifyCaretMovement( CLayoutInt nCurLine, CLayoutInt nCurCol );
	void NotifyDocModification();
	[[nodiscard]] bool RequestWorkbenchOutline( int outlineType, bool forceRefresh = false );
	void StopWorkbenchOutlineWorker() noexcept;
	void HandleWorkbenchWorkerResult( LPARAM lParam ) noexcept;
	[[nodiscard]] workbench::outline::OutlineWorkerStateSnapshot GetWorkbenchWorkerState() const noexcept;

protected:
	bool m_bInChangeLayout;

	CFuncInfoArr*	m_pcFuncInfoArr;	/* 関数情報配列 */
	CLayoutInt		m_nCurLine;			/* 現在行 */
	CLayoutInt		m_nCurCol;			/* 現在桁 */
	int				m_nSortCol;			/* ソートする列番号 */
	int				m_nSortColOld;		//!< ソートする列番号(OLD)
	bool			m_bSortDesc;		//!< 降順
	CNativeW		m_cmemClipText;		/* クリップボードコピー用テキスト */
	bool			m_bLineNumIsCRLF;	/* 行番号の表示 false=折り返し単位／true=改行単位 */
	int				m_nListType;		/* 一覧の種類 */
public:
	int				m_nDocType;			//! ドキュメントの種類 */
	int				m_nOutlineType;		/* アウトライン解析の種別 */
protected:
	friend class CEditWnd;
	friend class CLoadAgent;
	bool			m_bEditWndReady;	/* エディタ画面の準備完了 */
	BOOL OnInitDialog(HWND hwndDlg, WPARAM wParam, LPARAM lParam) override;
	BOOL OnBnClicked(int wID) override;
	BOOL OnNotify(NMHDR* pNMHDR) override;
	BOOL OnSize( WPARAM wParam, LPARAM lParam ) override;
	BOOL OnMinMaxInfo( LPARAM lParam );
	BOOL OnDestroy(void) override; // 20060201 aroka
	BOOL OnCbnSelEndOk( HWND hwndCtl, int wID ) override;
	BOOL OnContextMenu(WPARAM wParam, LPARAM lParam) override;
	void SetData() override;	/* ダイアログデータの設定 */
	int GetData( void ) override;	/* ダイアログデータの取得 */

	/*
	||  実装ヘルパ関数
	*/
	BOOL OnJump( bool bCheckAutoClose = true, bool bFileJump = true );	//	bCheckAutoClose：「このダイアログを自動的に閉じる」をチェックするかどうか
	void SetTreeJava(HWND hwndDlg, HTREEITEM hInsertAfter, BOOL bAddClass);	/* ツリーコントロールの初期化：Javaメソッドツリー */
	void SetTree(HTREEITEM hInsertAfter, bool tagjump = false, bool nolabel = false);		/* ツリーコントロールの初期化：汎用品 */
	void SetTreeFile();				// ツリーコントロールの初期化：ファイルツリー
	void SetListVB( void );			/* リストビューコントロールの初期化：VisualBasic */		// Jul 10, 2003  little YOSHI
	void SetDocLineFuncList();
	void SetItemSelection( int nSelectItemIndex, bool bAllowExpand );
	void SetItemSelectionForTreeView( HWND hwndTree, int nSelectItemIndex, bool bAllowExpand );
	void SetItemSelectionForListView( HWND hwndList, int nSelectItemIndex );
	bool GetFuncInfoIndex( CLayoutInt nCurLine, CLayoutInt nCurCol, int* pnIndexOut );

	void SetTreeFileSub(HTREEITEM hParent, const WCHAR* pszFile);
	// 2002/11/1 frozen
	void SortTree(HWND hWndTree,HTREEITEM htiParent);//!< ツリービューの項目をソートする（ソート基準はm_nSortTypeを使用）
#if 0
// 2002.04.01 YAZAKI SetTreeTxt()、SetTreeTxtNest()は廃止。GetTreeTextNextはもともと使用されていなかった。
	void SetTreeTxt( HWND );	/* ツリーコントロールの初期化：テキストトピックツリー */
	int SetTreeTxtNest( HWND, HTREEITEM, int, int, HTREEITEM*, int );
	void GetTreeTextNext( HWND, HTREEITEM, int );
#endif

	//	Apr. 23, 2005 genta リストビューのソートを関数として独立させた
	void SortListView(HWND hwndList, int sortcol);
	static int CALLBACK CompareFunc_Asc( LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort );
	static int CALLBACK CompareFunc_Desc( LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort );

	// 2001.12.03 hor
//	void SetTreeBookMark( HWND );		/* ツリーコントロールの初期化：ブックマーク */
	LPVOID GetHelpIdTable(void) override;	//@@@ 2002.01.18 add
	void Key2Command(WORD KeyCode);		//	キー操作→コマンド変換
	bool HitTestSplitter( int xPos, int yPos );
	int HitTestCaptionButton( int xPos, int yPos );
	INT_PTR OnNcCalcSize( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	INT_PTR OnNcHitTest( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	INT_PTR OnNcMouseMove( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	INT_PTR OnMouseMove( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	INT_PTR OnNcLButtonDown( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	INT_PTR OnLButtonUp( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	INT_PTR OnNcPaint( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam );
	BOOL OnTimer( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam ) override;
	void GetDockSpaceRect( LPRECT pRect );
	void GetCaptionRect( LPRECT pRect );
	bool GetCaptionButtonRect( int nButton, LPRECT pRect );
	void DoMenu( POINT pt, HWND hwndFrom );
	BOOL PostOutlineNotifyToAllEditors( WPARAM wParam, LPARAM lParam );
	EDockSide GetDropRect( POINT ptDrag, POINT ptDrop, LPRECT pRect, bool bForceFloat );
	BOOL Track( POINT ptDrag );
	bool GetTreeFileFullName(HWND, HTREEITEM, std::wstring*, int*);
	bool TagJumpTimer(const WCHAR*, CMyPoint, bool);

private:
	enum class WorkbenchApplyStage : std::uint8_t {
		MaterializeModel,
		PrepareControls,
		CollectTree,
		ClearList,
		ClearTree,
		InsertTree,
		ExpandTree,
		ApplyLineMarks,
		Finalize,
	};

	class WorkbenchApplyState final {
		friend class CDlgFuncList;
		std::uint64_t generation = 0;
		workbench::outline::OutlineDocumentVersion documentVersion{};
		workbench::outline::OutlineParseResult parse;
		std::unique_ptr<CFuncInfoArr> model;
		std::vector<HTREEITEM> nodeItems;
		std::vector<HTREEITEM> clearTreeItems;
		std::vector<HTREEITEM> treeScanPending;
		std::size_t symbolIndex = 0;
		std::size_t clearListRemaining = 0;
		std::size_t clearTreeIndex = 0;
		std::size_t treeIndex = 0;
		std::size_t expandIndex = 0;
		std::size_t lineIndex = 0;
		bool lineMarksReset = false;
		CDocLine* lineMarkResetCursor = nullptr;
		WorkbenchApplyStage stage = WorkbenchApplyStage::MaterializeModel;
		std::uint64_t startUs = 0;
		std::uint64_t layoutBeginUs = 0;
		std::uint64_t treeBeginUs = 0;
		std::uint64_t expansionBeginUs = 0;
		std::uint64_t lineMarksBeginUs = 0;
		HTREEITEM insertAfter = TVI_LAST;
		int previousOutlineType = OUTLINE_DEFAULT;
		int previousListType = OUTLINE_DEFAULT;
	};

	static constexpr UINT kWorkbenchApplyMessage = WM_APP + 0x573;
	[[nodiscard]] bool UsesCompactPanelLayout() const noexcept { return m_bWorkbenchMode || IsDocking(); }
	void ApplyWorkbenchAppearance() noexcept;
	void DisarmWorkbenchRefreshTimer() noexcept;
	void BeginWorkbenchApply( std::unique_ptr<workbench::outline::OutlineWorkerResult> result ) noexcept;
	void HandleWorkbenchApplyStep() noexcept;
	void CancelWorkbenchApply() noexcept;
	[[nodiscard]] bool IsWorkbenchApplyCurrent() const noexcept;
	[[nodiscard]] bool PostWorkbenchApplyStep() noexcept;
	void ObserveWorkbenchDocument( CEditView* view ) noexcept;
	void CommitWorkbenchModel() noexcept;
	[[nodiscard]] std::shared_ptr<const workbench::outline::OutlineDocumentSnapshot>
		CaptureWorkbenchSnapshot( std::uint64_t* captureUs ) const;
	void CommitWorkbenchParseResult(
		std::unique_ptr<workbench::outline::OutlineWorkerResult> result ) noexcept;
	void BuildWorkbenchClipboardText();

	//	May 18, 2001 genta
	/*!
		@brief アウトライン解析種別

		0: List, 1: Tree
	*/
	int	m_nViewType;

	// 2002.02.16 hor Treeのダブルクリックでフォーカス移動できるように 1/4
	// (無理矢理なのでどなたか修正お願いします)
	bool m_bWaitTreeProcess;

	int m_nSortType;						//!< ツリービューをソートする基準
	int m_nTreeItemCount;
	bool m_bDummyLParamMode;				//!< m_vecDummylParams有効/無効
	std::vector<int> m_vecDummylParams;		//!< ダミー要素の識別値

	// 選択中の関数情報
	CFuncInfo* m_cFuncInfo;
	std::wstring m_sJumpFile;

	const WCHAR* m_pszTimerJumpFile;
	CMyPoint	m_pointTimerJump;
	bool		m_bTimerJumpAutoClose;

	EDockSide	m_eDockSide;	// 現在の画面の表示位置
	HWND		m_hwndWorkbenchParent = nullptr;
	bool		m_bWorkbenchMode = false;
	COLORREF	m_workbenchText = RGB(0xCC, 0xCC, 0xCC);
	COLORREF	m_workbenchBackground = RGB(0x25, 0x25, 0x26);
	COLORREF	m_workbenchHover = RGB(0x2A, 0x2D, 0x2E);
	COLORREF	m_workbenchSelection = RGB(0x37, 0x37, 0x3D);
	COLORREF	m_workbenchSelectionText = RGB(0xFF, 0xFF, 0xFF);
	HFONT		m_workbenchFont = nullptr;
	int			m_workbenchItemHeight = 22;
	HIMAGELIST	m_workbenchSymbolImages = nullptr;
	CEditDoc*	m_workbenchDocument = nullptr;
	workbench::outline::OutlineDocumentVersion m_workbenchDocumentVersion{};
	workbench::outline::OutlineDocumentVersion m_workbenchModelVersion{};
	std::uint64_t m_workbenchNextDocumentIdentity = 0;
	std::uint64_t m_workbenchModelGeneration = 0;
	std::uint64_t m_workbenchReparseCount = 0;
	workbench::outline::OutlineDocumentVersion m_workbenchRequestedVersion{};
	std::uint64_t m_workbenchRequestedGeneration = 0;
	std::uint64_t m_workbenchRequestStartUs = 0;
	workbench::outline::OutlineRefreshScheduler m_workbenchRefreshScheduler;
	workbench::outline::OutlineWorkerTerminal m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Closed;
	workbench::outline::OutlinePhaseTimings m_workbenchLastTimings{};
	std::unique_ptr<CFuncInfoArr> m_workbenchCommittedModel;
	std::unique_ptr<workbench::outline::OutlineParserWorker> m_workbenchParser;
	std::unique_ptr<WorkbenchApplyState> m_workbenchApply;
	std::vector<HTREEITEM> m_workbenchTreeItems;
	class WorkbenchTreeLabel final {
	public:
		WorkbenchTreeLabel() = default;
		WorkbenchTreeLabel( std::wstring text, int depth )
			: m_text(std::move(text))
			, m_depth(depth)
		{
		}

		[[nodiscard]] const std::wstring& Text() const noexcept { return m_text; }
		[[nodiscard]] int Depth() const noexcept { return m_depth; }

	private:
		std::wstring m_text;
		int m_depth = 0;
	};
	std::map<HTREEITEM, WorkbenchTreeLabel> m_workbenchTreeLabels;
	bool m_workbenchClipboardUsesGenericTree = false;
	bool m_workbenchClipboardTagJump = false;
	bool m_workbenchClipboardNoLabel = false;
	WorkbenchNativeSurfaceSubmitter m_workbenchNativeSurfaceSubmitter;
	int			m_workbenchAppearanceWidth = -1;
	bool		m_workbenchAppearanceDirty = true;
	bool		m_workbenchTreeContentDirty = true;
	HWND		m_hwndToolTip;	/*!< ツールチップ（ボタン用） */
	bool		m_bStretching;
	bool		m_bHovering;
	int			m_nHilightedBtn;
	int			m_nCapturingBtn;

	STypeConfig m_type;
	CFileTreeSetting	m_fileTreeSetting;

	static LPDLGTEMPLATE m_pDlgTemplate;
	static DWORD m_dwDlgTmpSize;
	static HINSTANCE m_lastRcInstance;		// リソース生存チェック用

	POINT				m_ptDefaultSize;
	POINT				m_ptDefaultSizeClient;
	RECT				m_rcItems[12];

	bool		m_bFuncInfoArrIsUpToDate = false;
};
#endif /* SAKURA_CDLGFUNCLIST_B22A3877_572A_49B7_B683_50ECA451A6F8_H_ */
