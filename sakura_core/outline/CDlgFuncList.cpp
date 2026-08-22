/*!	@file
	@brief アウトライン解析ダイアログボックス

	@author Norio Nakatani

	@date 2001/06/23 N.Nakatani Visual Basicのアウトライン解析
*/
/*
	Copyright (C) 1998-2001, Norio Nakatani
	Copyright (C) 2000, genta
	Copyright (C) 2001, Stonee, JEPRO, genta, hor
	Copyright (C) 2002, MIK, aroka, hor, genta, YAZAKI, Moca, frozen
	Copyright (C) 2003, zenryaku, Moca, naoh, little YOSHI, genta,
	Copyright (C) 2004, zenryaku, Moca, novice
	Copyright (C) 2005, genta, zenryaku, ぜっと, D.S.Koba
	Copyright (C) 2006, genta, aroka, ryoji, Moca
	Copyright (C) 2006, genta, ryoji
	Copyright (C) 2007, ryoji
	Copyright (C) 2010, ryoji
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#include "StdAfx.h"
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <limits>
#include <algorithm>
#include <limits.h>
#include "outline/CDlgFuncList.h"
#include "outline/CFuncInfo.h"
#include "outline/CFuncInfoArr.h"// 2002/2/3 aroka
#include "outline/CDlgFileTree.h"
#include "window/CEditWnd.h"	//	2006/2/11 aroka 追加
#include "doc/CEditDoc.h"
#include "uiparts/CGraphics.h"
#include "util/shell.h"
#include "util/os.h"
#include "util/input.h"
#include "util/window.h"
#include "env/CAppNodeManager.h"
#include "env/CDocTypeManager.h"
#include "env/CFileNameManager.h"
#include "env/CShareData.h"
#include "env/CShareData_IO.h"
#include "grep/CGrepEnumKeys.h"
#include "grep/CGrepEnumFilterFiles.h"
#include "grep/CGrepEnumFilterFolders.h"
#include "env/CDataProfile.h"
#include "dlg/CDlgTagJumpList.h"
#include "typeprop/CImpExpManager.h"
#include "apiwrap/StdApi.h"
#include "apiwrap/CommonControl.h"
#include "apiwrap/StdControl.h"
#include "sakura_rc.h"
#include "sakura.hh"
#include "config/system_constants.h"
#include "config/app_constants.h"
#include "apiwrap/DarkMode.h"

// 画面ドッキング用の定義	// 2010.06.05 ryoji
#define DEFINE_SYNCCOLOR
#define DOCK_SPLITTER_WIDTH		DpiScaleX(5)
#define DOCK_MIN_SIZE			DpiScaleX(60)
#define DOCK_BUTTON_NUM			(3)

// ビューの種別
#define VIEWTYPE_LIST	0
#define VIEWTYPE_TREE	1

namespace {

constexpr UINT_PTR kWorkbenchRefreshTimerIdBase = 0x10000;
constexpr UINT kWorkbenchRefreshDebounceMs = 75;
constexpr std::size_t kWorkbenchApplyChunkItems = 96;
constexpr std::uint64_t kWorkbenchApplyBudgetUs = 4000;

[[nodiscard]] constexpr UINT_PTR WorkbenchRefreshTimerId( std::uint64_t token ) noexcept
{
	if( token == 0
		|| token > static_cast<std::uint64_t>((std::numeric_limits<UINT_PTR>::max)())
			- kWorkbenchRefreshTimerIdBase ) return 0;
	return kWorkbenchRefreshTimerIdBase + static_cast<UINT_PTR>(token);
}

[[nodiscard]] constexpr std::uint64_t WorkbenchRefreshTimerToken( UINT_PTR timerId ) noexcept
{
	return timerId > kWorkbenchRefreshTimerIdBase
		? static_cast<std::uint64_t>(timerId - kWorkbenchRefreshTimerIdBase) : 0;
}

[[nodiscard]] std::uint64_t WorkbenchNowUs() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

//! Seed an iterative native-tree walk.  The walk itself is budgeted by the
//! apply state machine so discovering a large retired hierarchy cannot block
//! the editor thread before incremental deletion starts.
void SeedWorkbenchTreeScan(
	decltype(::GetActiveWindow()) hwndTree, std::vector<HTREEITEM>& pending )
{
	if( hwndTree == nullptr ) return;
	if( const HTREEITEM root = TreeView_GetRoot(hwndTree); root != nullptr ) {
		pending.push_back(root);
	}
}

class DialogTemplateCursor final {
public:
	DialogTemplateCursor( void* data, size_t size ) noexcept
		: m_data(static_cast<std::byte*>(data)), m_size(size) {}

	[[nodiscard]] bool CanRead( size_t offset, size_t size ) const noexcept
	{
		return offset <= m_size && size <= m_size - offset;
	}

	template <class T>
	[[nodiscard]] bool Read( size_t offset, T& value ) const noexcept
	{
		if( !CanRead(offset, sizeof(T)) ) return false;
		std::memcpy( &value, m_data + offset, sizeof(T) );
		return true;
	}

	template <class T>
	[[nodiscard]] bool Write( size_t offset, T value ) noexcept
	{
		if( !CanRead(offset, sizeof(T)) ) return false;
		std::memcpy( m_data + offset, &value, sizeof(T) );
		return true;
	}

	[[nodiscard]] bool Advance( size_t& offset, size_t amount ) const noexcept
	{
		if( !CanRead(offset, amount) ) return false;
		offset += amount;
		return true;
	}

	[[nodiscard]] bool AlignDword( size_t& offset ) const noexcept
	{
		const auto address = reinterpret_cast<std::uintptr_t>(m_data) + offset;
		const size_t padding = static_cast<size_t>((4u - (address & 3u)) & 3u);
		return Advance( offset, padding );
	}

	[[nodiscard]] bool SkipStringOrOrdinal( size_t& offset ) const noexcept
	{
		WORD value = 0;
		if( !Read(offset, value) || !Advance(offset, sizeof(value)) ) return false;
		if( value == 0 ) return true;
		if( value == 0xffff ) return Advance(offset, sizeof(WORD));
		while( value != 0 ){
			if( !Read(offset, value) || !Advance(offset, sizeof(value)) ) return false;
		}
		return true;
	}

	[[nodiscard]] bool SkipFont( size_t& offset, bool extended ) const noexcept
	{
		const size_t fixedSize = extended ? 6u : sizeof(WORD);
		return Advance(offset, fixedSize) && SkipStringOrOrdinal(offset);
	}

private:
	std::byte* m_data;
	size_t m_size;
};

} // namespace

DWORD workbench::outline::NormalizeWorkbenchOutlineTreeStyle( DWORD style ) noexcept
{
	return (style & ~(WS_BORDER | TVS_HASLINES | TVS_SHOWSELALWAYS))
		| TVS_HASBUTTONS | TVS_LINESATROOT | TVS_FULLROWSELECT;
}

DWORD workbench::outline::NormalizeWorkbenchOutlineListStyle( DWORD style ) noexcept
{
	return style & ~WS_BORDER;
}

DWORD workbench::outline::NormalizeWorkbenchOutlineControlExStyle( DWORD exStyle ) noexcept
{
	return exStyle & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
}

bool workbench::outline::NormalizeWorkbenchOutlineDialogTemplate( void* data, size_t size ) noexcept
{
	if( data == nullptr ) return false;
	DialogTemplateCursor cursor(data, size);
	WORD first = 0;
	WORD second = 0;
	if( !cursor.Read(0, first) || !cursor.Read(sizeof(WORD), second) ) return false;
	const bool extended = first == 1 && second == 0xffff;

	DWORD dialogStyle = 0;
	WORD itemCount = 0;
	size_t offset = extended ? 26u : 18u;
	if( !cursor.Read(extended ? 12u : 0u, dialogStyle)
		|| !cursor.Read(extended ? 16u : 8u, itemCount)
		|| !cursor.CanRead(0, offset) ){
		return false;
	}
	if( !cursor.SkipStringOrOrdinal(offset)
		|| !cursor.SkipStringOrOrdinal(offset)
		|| !cursor.SkipStringOrOrdinal(offset) ){
		return false;
	}
	if( (dialogStyle & DS_SETFONT) != 0 && !cursor.SkipFont(offset, extended) ) return false;

	bool foundTree = false;
	bool foundList = false;
	for( WORD item = 0; item < itemCount; ++item ){
		if( !cursor.AlignDword(offset) ) return false;
		const size_t itemHeader = offset;
		const size_t itemHeaderSize = extended ? 24u : 18u;
		const size_t exStyleOffset = itemHeader + (extended ? 4u : 4u);
		const size_t styleOffset = itemHeader + (extended ? 8u : 0u);
		DWORD itemId = 0;
		if( extended ){
			if( !cursor.Read(itemHeader + 20u, itemId) ) return false;
		}else{
			WORD shortId = 0;
			if( !cursor.Read(itemHeader + 16u, shortId) ) return false;
			itemId = shortId;
		}
		DWORD style = 0;
		DWORD exStyle = 0;
		if( !cursor.Read(styleOffset, style) || !cursor.Read(exStyleOffset, exStyle)
			|| !cursor.Advance(offset, itemHeaderSize) ){
			return false;
		}

		if( itemId == IDC_TREE_FL ){
			foundTree = cursor.Write(styleOffset, NormalizeWorkbenchOutlineTreeStyle(style))
				&& cursor.Write(exStyleOffset, NormalizeWorkbenchOutlineControlExStyle(exStyle));
			if( !foundTree ) return false;
		}else if( itemId == IDC_LIST_FL ){
			foundList = cursor.Write(styleOffset, NormalizeWorkbenchOutlineListStyle(style))
				&& cursor.Write(exStyleOffset, NormalizeWorkbenchOutlineControlExStyle(exStyle));
			if( !foundList ) return false;
		}

		if( !cursor.SkipStringOrOrdinal(offset) || !cursor.SkipStringOrOrdinal(offset) ) return false;
		WORD extraBytes = 0;
		if( !cursor.Read(offset, extraBytes) || !cursor.Advance(offset, sizeof(extraBytes))
			|| !cursor.Advance(offset, extraBytes) ){
			return false;
		}
	}
	return foundTree && foundList;
}

//アウトライン解析 CDlgFuncList.cpp	//@@@ 2002.01.07 add start MIK
const DWORD p_helpids[] = {	//12200
	IDC_BUTTON_COPY,					HIDC_FL_BUTTON_COPY,	//コピー
	IDOK,								HIDOK_FL,				//ジャンプ
	IDCANCEL,							HIDCANCEL_FL,			//キャンセル
	IDC_BUTTON_HELP,					HIDC_FL_BUTTON_HELP,	//ヘルプ
	IDC_CHECK_bAutoCloseDlgFuncList,	HIDC_FL_CHECK_bAutoCloseDlgFuncList,	//自動的に閉じる
	IDC_LIST_FL,						HIDC_FL_LIST1,			//トピックリスト	IDC_LIST1->IDC_LIST_FL	2008/7/3 Uchi
	IDC_TREE_FL,						HIDC_FL_TREE1,			//トピックツリー	IDC_TREE1->IDC_TREE_FL	2008/7/3 Uchi
	IDC_CHECK_bFunclistSetFocusOnJump,	HIDC_FL_CHECK_bFunclistSetFocusOnJump,	//ジャンプでフォーカス移動する
	IDC_CHECK_bMarkUpBlankLineEnable,	HIDC_FL_CHECK_bMarkUpBlankLineEnable,	//空行を無視する
	IDC_COMBO_nSortType,				HIDC_COMBO_nSortType,	//順序
	IDC_BUTTON_WINSIZE,					HIDC_FL_BUTTON_WINSIZE,	//ウィンドウ位置保存	// 2006.08.06 ryoji
	IDC_BUTTON_MENU,					HIDC_FL_BUTTON_MENU,	//ウィンドウの位置メニュー
	IDC_BUTTON_SETTING,					HIDC_FL_BUTTON_SETTING,	//設定
//	IDC_STATIC,							-1,
	0, 0
};	//@@@ 2002.01.07 add end MIK

static const SAnchorList anchorList[] = {
	{IDC_BUTTON_COPY, ANCHOR_BOTTOM},
	{IDOK, ANCHOR_BOTTOM},
	{IDCANCEL, ANCHOR_BOTTOM},
	{IDC_BUTTON_HELP, ANCHOR_BOTTOM},
	{IDC_CHECK_bAutoCloseDlgFuncList, ANCHOR_BOTTOM},
	{IDC_LIST_FL, ANCHOR_ALL},
	{IDC_TREE_FL, ANCHOR_ALL},
	{IDC_CHECK_bFunclistSetFocusOnJump, ANCHOR_BOTTOM},
	{IDC_CHECK_bMarkUpBlankLineEnable , ANCHOR_BOTTOM},
	{IDC_COMBO_nSortType, ANCHOR_TOP},
	{IDC_BUTTON_WINSIZE, ANCHOR_BOTTOM}, // 20060201 aroka
	{IDC_BUTTON_MENU, ANCHOR_BOTTOM},
};

//関数リストの列
enum EFuncListCol {
	FL_COL_ROW		= 0,	//行
	FL_COL_COL		= 1,	//桁
	FL_COL_NAME		= 2,	//関数名
	FL_COL_REMARK	= 3		//備考
};

/*! ソート比較用プロシージャ */
int CALLBACK CDlgFuncList::CompareFunc_Asc( LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort )
{
	CFuncInfo*		pcFuncInfo1;
	CFuncInfo*		pcFuncInfo2;
	CDlgFuncList*	pcDlgFuncList;
	pcDlgFuncList = (CDlgFuncList*)lParamSort;

	pcFuncInfo1 = pcDlgFuncList->m_pcFuncInfoArr->GetAt( lParam1 );
	if( nullptr == pcFuncInfo1 ){
		return -1;
	}
	pcFuncInfo2 = pcDlgFuncList->m_pcFuncInfoArr->GetAt( lParam2 );
	if( nullptr == pcFuncInfo2 ){
		return -1;
	}
	//	Apr. 23, 2005 genta 行番号を左端へ
	if( FL_COL_NAME == pcDlgFuncList->m_nSortCol){	/* 名前でソート */
		return wmemicmp( pcFuncInfo1->m_cmemFuncName.GetStringPtr(), pcFuncInfo2->m_cmemFuncName.GetStringPtr() );
	}
	//	Apr. 23, 2005 genta 行番号を左端へ
	if( FL_COL_ROW == pcDlgFuncList->m_nSortCol){	/* 行（＋桁）でソート */
		if( pcFuncInfo1->m_nFuncLineCRLF < pcFuncInfo2->m_nFuncLineCRLF ){
			return -1;
		}else
		if( pcFuncInfo1->m_nFuncLineCRLF == pcFuncInfo2->m_nFuncLineCRLF ){
			if( pcFuncInfo1->m_nFuncColCRLF < pcFuncInfo2->m_nFuncColCRLF ){
				return -1;
			}else
			if( pcFuncInfo1->m_nFuncColCRLF == pcFuncInfo2->m_nFuncColCRLF ){
				return 0;
			}else{
				return 1;
			}
		}else{
			return 1;
		}
	}
	if( FL_COL_COL == pcDlgFuncList->m_nSortCol){	/* 桁でソート */
		if( pcFuncInfo1->m_nFuncColCRLF < pcFuncInfo2->m_nFuncColCRLF ){
			return -1;
		}else
		if( pcFuncInfo1->m_nFuncColCRLF == pcFuncInfo2->m_nFuncColCRLF ){
			return 0;
		}else{
			return 1;
		}
	}
	// From Here 2001.12.07 hor
	if( FL_COL_REMARK == pcDlgFuncList->m_nSortCol){	/* 備考でソート */
		if( pcFuncInfo1->m_nInfo < pcFuncInfo2->m_nInfo ){
			return -1;
		}else
		if( pcFuncInfo1->m_nInfo == pcFuncInfo2->m_nInfo ){
			return 0;
		}else{
			return 1;
		}
	}
	// To Here 2001.12.07 hor
	return -1;
}

int CALLBACK CDlgFuncList::CompareFunc_Desc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
	return -1 * CompareFunc_Asc(lParam1, lParam2, lParamSort);
}

EFunctionCode CDlgFuncList::GetFuncCodeRedraw(int outlineType)
{
	if( outlineType == OUTLINE_BOOKMARK ){
		return F_BOOKMARK_VIEW;
	}else if( outlineType == OUTLINE_FILETREE ){
		return F_FILETREE;
	}
	return F_OUTLINE;
}

static EOutlineType GetOutlineTypeRedraw(int outlineType)
{
	if( outlineType == OUTLINE_BOOKMARK ){
		return OUTLINE_BOOKMARK;
	}else if( outlineType == OUTLINE_FILETREE ){
		return OUTLINE_FILETREE;
	}
	return OUTLINE_DEFAULT;
}

LPDLGTEMPLATE CDlgFuncList::m_pDlgTemplate = nullptr;
DWORD CDlgFuncList::m_dwDlgTmpSize = 0;
HINSTANCE CDlgFuncList::m_lastRcInstance = nullptr;

CDlgFuncList::CDlgFuncList() : CDialog(true)
{
	/* サイズ変更時に位置を制御するコントロール数 */
	static_assert( int(std::size(anchorList)) == int(std::size(m_rcItems)) );

	m_pcFuncInfoArr = nullptr;		/* 関数情報配列 */
	m_nCurLine = CLayoutInt(0);				/* 現在行 */
	m_nOutlineType = OUTLINE_DEFAULT;
	m_nListType = OUTLINE_DEFAULT;
	//	Apr. 23, 2005 genta 行番号を左端へ
	m_nSortCol = 0;				/* ソートする列番号 2004.04.06 zenryaku 標準は行番号(1列目) */
	m_nSortColOld = -1;
	m_bLineNumIsCRLF = false;	/* 行番号の表示 false=折り返し単位／true=改行単位 */
	m_bWaitTreeProcess = false;	// 2002.02.16 hor Treeのダブルクリックでフォーカス移動できるように 2/4
	m_nSortType = SORTTYPE_DEFAULT;
	m_cFuncInfo = nullptr;			/* 現在の関数情報 */
	m_bEditWndReady = false;	/* エディタ画面の準備完了 */
	m_bInChangeLayout = false;
	m_pszTimerJumpFile = nullptr;
	m_ptDefaultSize.x = -1;
	m_ptDefaultSize.y = -1;
	m_bDummyLParamMode = false;
	m_workbenchCommittedModel = std::make_unique<CFuncInfoArr>();
}

void CDlgFuncList::SetWorkbenchParent( HWND parent ) noexcept
{
	m_hwndWorkbenchParent = parent;
}

void CDlgFuncList::SetWorkbenchMode( bool enabled ) noexcept
{
	// The dialog template style and parent are fixed at creation time.  Callers close the
	// current dialog before switching modes; repeated assignments remain harmless.
	if( GetHwnd() == nullptr || m_bWorkbenchMode == enabled ) {
		m_bWorkbenchMode = enabled;
	}
}

void CDlgFuncList::DisarmWorkbenchRefreshTimer() noexcept
{
	m_workbenchRefreshScheduler.Disarm([this](std::uint64_t token) {
		const UINT_PTR timerId = WorkbenchRefreshTimerId(token);
		if( GetHwnd() != nullptr && timerId != 0 ) ::KillTimer(GetHwnd(), timerId);
	});
}

void CDlgFuncList::ObserveWorkbenchDocument( CEditView* view ) noexcept
{
	CEditDoc* const document = view != nullptr ? view->GetDocument() : nullptr;
	if( document == m_workbenchDocument ) return;

	// A document switch invalidates both queued UI debounce callbacks and the
	// worker's result fence before the new identity is published.
	StopWorkbenchOutlineWorker();
	if( IsWorkbenchMode() && GetHwnd() != nullptr ) {
		// Do not leave the previous document's handles selectable while the new
		// document is being parsed.  The model owner is retained for rollback,
		// but all native actions are disabled until the new version commits.
		TreeView_DeleteAllItems(GetItemHwnd(IDC_TREE_FL));
		ListView_DeleteAllItems(GetItemHwnd(IDC_LIST_FL));
		::EnableWindow(GetItemHwnd(IDC_BUTTON_COPY), FALSE);
		m_workbenchTreeItems.clear();
		m_workbenchTreeLabels.clear();
		m_cmemClipText.SetString(L"");
	}
	m_workbenchDocument = document;
	m_workbenchDocumentVersion = {};
	m_workbenchModelVersion = {};
	m_workbenchRequestedVersion = {};
	m_workbenchRequestedGeneration = 0;
	m_bFuncInfoArrIsUpToDate = false;
	m_workbenchTreeContentDirty = true;
	if( document == nullptr ) return;
	if( m_workbenchNextDocumentIdentity == std::numeric_limits<std::uint64_t>::max() ) return;

	m_workbenchDocumentVersion.identity = ++m_workbenchNextDocumentIdentity;
	m_workbenchDocumentVersion.version = 0;
}

workbench::outline::OutlineDocumentVersion CDlgFuncList::GetWorkbenchDocumentVersion() noexcept
{
	ObserveWorkbenchDocument( reinterpret_cast<CEditView*>(m_lParam) );
	return m_workbenchDocumentVersion;
}

bool CDlgFuncList::HasCurrentWorkbenchModel() noexcept
{
	const auto documentVersion = GetWorkbenchDocumentVersion();
	return documentVersion.IsValid() && m_bFuncInfoArrIsUpToDate
		&& m_workbenchModelVersion == documentVersion;
}

void CDlgFuncList::CommitWorkbenchModel() noexcept
{
	if( !IsWorkbenchMode() || !m_workbenchDocumentVersion.IsValid() ) return;
	m_workbenchModelVersion = m_workbenchDocumentVersion;
	if( m_workbenchModelGeneration != std::numeric_limits<std::uint64_t>::max() ) {
		++m_workbenchModelGeneration;
	}
	if( m_workbenchReparseCount != std::numeric_limits<std::uint64_t>::max() ) {
		++m_workbenchReparseCount;
	}
}

bool CDlgFuncList::IsAsyncWorkbenchOutlineType( int outlineType ) noexcept
{
	return outlineType == OUTLINE_C || outlineType == OUTLINE_C_CPP
		|| outlineType == OUTLINE_CPP || outlineType == OUTLINE_JAVA;
}

std::shared_ptr<const workbench::outline::OutlineDocumentSnapshot>
CDlgFuncList::CaptureWorkbenchSnapshot( std::uint64_t* captureUs ) const
{
	const auto begin = std::chrono::steady_clock::now();
	const CEditDoc* const document = m_workbenchDocument;
	if( document == nullptr || !m_workbenchDocumentVersion.IsValid() ) return nullptr;

	auto snapshot = std::make_shared<workbench::outline::OutlineDocumentSnapshot>();
	snapshot->documentVersion = m_workbenchDocumentVersion;
	const wchar_t* const filePath = document->m_cDocFile.GetFilePath();
	if( filePath != nullptr ) snapshot->filePath = filePath;
	snapshot->extendedLineDelimiters = m_pShareData->m_Common.m_sEdit.m_bEnableExtEol != FALSE;
	// Copy the small parser presentation values once.  The worker must not call
	// LS() or read shared settings after this boundary has been published.
	snapshot->cppAnonymousName = LS(STR_OUTLINE_CPP_NONAME);
	snapshot->cppDefinitionPosition = LS(STR_OUTLINE_CPP_DEFPOS);
	snapshot->javaDefinitionPosition = LS(STR_OUTLINE_JAVA_DEFPOS);
	for( const auto [info, resource] : {
		std::pair{ FL_OBJ_DECLARE, STR_DLGFNCLST_APND_DECLARE },
		std::pair{ FL_OBJ_CLASS, STR_DLGFNCLST_APND_CLASS },
		std::pair{ FL_OBJ_STRUCT, STR_DLGFNCLST_APND_STRUCT },
		std::pair{ FL_OBJ_ENUM, STR_DLGFNCLST_APND_ENUM },
		std::pair{ FL_OBJ_UNION, STR_DLGFNCLST_APND_UNION },
		std::pair{ FL_OBJ_NAMESPACE, STR_DLGFNCLST_APND_NAMESPACE },
		std::pair{ FL_OBJ_INTERFACE, STR_DLGFNCLST_APND_INTERFACE },
		std::pair{ FL_OBJ_GLOBAL, STR_DLGFNCLST_APND_GLOBAL }
	} ) {
		snapshot->appendText.emplace(info, LS(resource));
	}

	const CLogicInt lineCount = document->m_cDocLineMgr.GetLineCount();
	if( lineCount < CLogicInt(0)
		|| lineCount > CLogicInt((std::numeric_limits<int>::max)())
		|| static_cast<std::size_t>(lineCount) > snapshot->lineSpans.max_size() ) return nullptr;
	snapshot->lineSpans.reserve(static_cast<std::size_t>(lineCount));
	// Walk the intrusive line chain once.  GetLine(line) is optimized for nearby
	// accesses, but still contains direction/cache selection on every call; the
	// chain makes the capture cost unambiguously O(lines + text) with one branch
	// per line and does not perturb CDocLineMgr's mutable lookup cursor.
	const CDocLine* docLine = document->m_cDocLineMgr.GetDocLineTop();
	for( CLogicInt line = CLogicInt(0); line < lineCount; ++line ) {
		if( docLine == nullptr ) return nullptr;
		CLogicInt length = CLogicInt(0);
		const auto* const text = docLine->GetDocLineStrWithEOL(&length);
		if( length < CLogicInt(0) ) return nullptr;
		const auto lineLength = text != nullptr && length > CLogicInt(0)
			? static_cast<std::size_t>(length) : std::size_t(0);
		if( text == nullptr && length > CLogicInt(0) ) return nullptr;
		if( !snapshot->AppendLine(text != nullptr
			? std::wstring_view(text, lineLength) : std::wstring_view()) ) return nullptr;
		docLine = docLine->GetNextLine();
	}
	if( captureUs != nullptr ) {
		*captureUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - begin).count());
	}
	return snapshot;
}

bool CDlgFuncList::RequestWorkbenchOutline( int outlineType, bool forceRefresh )
{
	if( !IsWorkbenchMode() || !IsAsyncWorkbenchOutlineType(outlineType) || GetHwnd() == nullptr ) return false;
	ObserveWorkbenchDocument( reinterpret_cast<CEditView*>(m_lParam) );
	if( !m_workbenchDocumentVersion.IsValid() ) {
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
		m_workbenchLastTimings = {};
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
		return false;
	}
	// Ordinary activation is allowed to reuse either the already-submitted
	// generation or the one quiet-period callback.  This check intentionally
	// precedes snapshot capture: the snapshot is O(total document text).
	if( m_workbenchRefreshScheduler.CanReuse(
		forceRefresh,
		m_nOutlineType,
		outlineType,
		m_workbenchRequestedVersion,
		m_workbenchRequestedGeneration,
		m_workbenchDocumentVersion ) ) return true;
	DisarmWorkbenchRefreshTimer();
	if( !forceRefresh && m_nOutlineType == outlineType && HasCurrentWorkbenchModel() ) return true;

	const std::uint64_t captureStartUs = WorkbenchNowUs();
	std::uint64_t captureUs = 0;
	const auto snapshot = CaptureWorkbenchSnapshot(&captureUs);
	if( snapshot == nullptr ) {
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
		m_workbenchLastTimings = {};
		m_workbenchLastTimings.snapshotCaptureUs = captureUs;
		m_workbenchLastTimings.totalUs = captureUs;
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
		return false;
	}
	if( m_workbenchParser == nullptr ) m_workbenchParser = std::make_unique<workbench::outline::OutlineParserWorker>();
	m_workbenchParser->SetNotificationWindow(GetHwnd(), true);
	const std::uint64_t previousGeneration = m_workbenchRequestedGeneration;
	const std::uint64_t previousRequestStartUs = m_workbenchRequestStartUs;
	const auto submission = m_workbenchParser->Submit(snapshot, outlineType, outlineType, captureUs);
	if( (submission.status == workbench::outline::OutlineWorkerRequestStatus::ActiveDeduplicated
		|| submission.status == workbench::outline::OutlineWorkerRequestStatus::PendingDeduplicated)
		&& previousGeneration != 0 && submission.generation == previousGeneration ) {
		// Submit confirms the same generation under its mutex.  Keep the original
		// capture-start timestamp and request version for end-to-end timing.
		m_workbenchRequestStartUs = previousRequestStartUs;
		return true;
	}
	if( submission.status == workbench::outline::OutlineWorkerRequestStatus::GenerationExhausted
		|| submission.status == workbench::outline::OutlineWorkerRequestStatus::Closed
		|| submission.status == workbench::outline::OutlineWorkerRequestStatus::InvalidSnapshot
		|| submission.generation == 0 ) {
		// Generation exhaustion and all other schedule failures are explicit UI
		// terminals.  They never leave a request marker that could cause a retry
		// storm; a later explicit reload may start a new lifecycle if possible.
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
		m_workbenchLastTimings = {};
		m_workbenchLastTimings.snapshotCaptureUs = captureUs;
		m_workbenchLastTimings.totalUs = WorkbenchNowUs() - captureStartUs;
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
		return false;
	}

	m_nOutlineType = outlineType;
	m_nListType = outlineType;
	m_workbenchRequestedVersion = snapshot->documentVersion;
	m_workbenchRequestedGeneration = submission.generation;
	m_workbenchRequestStartUs = captureStartUs;
	m_bFuncInfoArrIsUpToDate = false;
	return true;
}

void CDlgFuncList::StopWorkbenchOutlineWorker() noexcept
{
	CancelWorkbenchApply();
	m_workbenchRefreshScheduler.Stop([this](std::uint64_t token) {
		const UINT_PTR timerId = WorkbenchRefreshTimerId(token);
		if( GetHwnd() != nullptr && timerId != 0 ) ::KillTimer(GetHwnd(), timerId);
	});
	if( m_workbenchParser == nullptr ) {
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Closed;
		m_workbenchLastTimings = {};
		m_bFuncInfoArrIsUpToDate = false;
		return;
	}
	m_workbenchParser->SetNotificationWindow(nullptr, false);
	m_workbenchParser->Close();
	m_workbenchParser.reset();
	m_workbenchRequestedVersion = {};
	m_workbenchRequestedGeneration = 0;
	m_workbenchRequestStartUs = 0;
	m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Closed;
	m_workbenchLastTimings = {};
	m_bFuncInfoArrIsUpToDate = false;
}

bool CDlgFuncList::IsWorkbenchApplyCurrent() const noexcept
{
	return m_workbenchApply != nullptr
		&& m_bWorkbenchMode
		&& GetHwnd() != nullptr
		&& m_workbenchApply->generation != 0
		&& m_workbenchApply->generation == m_workbenchRequestedGeneration
		&& m_workbenchApply->documentVersion == m_workbenchDocumentVersion;
}

bool CDlgFuncList::PostWorkbenchApplyStep() noexcept
{
	return IsWorkbenchApplyCurrent()
		&& ::PostMessageW(GetHwnd(), kWorkbenchApplyMessage, 0, 0) != FALSE;
}

void CDlgFuncList::CancelWorkbenchApply() noexcept
{
	if( m_workbenchApply == nullptr ) return;
	const bool controlsTouched = m_workbenchApply->stage != WorkbenchApplyStage::MaterializeModel;
	if( controlsTouched && GetHwnd() != nullptr ) {
		const auto hwndTree = GetItemHwnd(IDC_TREE_FL);
		const auto hwndList = GetItemHwnd(IDC_LIST_FL);
		if( hwndTree != nullptr ) {
			// Do not synchronously tear down a partial hierarchy here.  This
			// method runs on the edit/close notification path, where a full
			// TreeView_DeleteAllItems would reintroduce the exact UI stall the
			// staged apply is meant to remove.  The next apply owns incremental
			// deletion; closing the dialog destroys the native control anyway.
			::ShowWindow(hwndTree, SW_HIDE);
			::SendMessageW(hwndTree, WM_SETREDRAW, TRUE, 0);
			::EnableWindow(hwndTree, TRUE);
		}
		if( hwndList != nullptr ) ::SendMessageW(hwndList, WM_SETREDRAW, TRUE, 0);
	}
	m_pcFuncInfoArr = m_workbenchCommittedModel.get();
	m_nOutlineType = m_workbenchApply->previousOutlineType;
	m_nListType = m_workbenchApply->previousListType;
	m_bDummyLParamMode = false;
	m_vecDummylParams.clear();
	m_workbenchTreeItems.clear();
	m_workbenchTreeLabels.clear();
	m_workbenchTreeContentDirty = true;
	m_workbenchAppearanceDirty = true;
	m_workbenchApply.reset();
	m_bFuncInfoArrIsUpToDate = false;
}

void CDlgFuncList::BeginWorkbenchApply(
	std::unique_ptr<workbench::outline::OutlineWorkerResult> result ) noexcept
{
	if( result == nullptr || !IsWorkbenchMode() || GetHwnd() == nullptr ) return;
	try {
		// A new current result owns the projection slot.  Normally document
		// invalidation cancels the old slot before this point, but keeping the
		// hand-off defensive prevents a late replacement from leaving partially
		// materialized native controls behind.
		if( m_workbenchApply != nullptr ) CancelWorkbenchApply();
		auto state = std::make_unique<WorkbenchApplyState>();
		state->generation = result->generation;
		state->documentVersion = result->parse.documentVersion;
		state->parse = std::move(result->parse);
		state->previousOutlineType = m_nOutlineType;
		state->previousListType = m_nListType;
		state->startUs = WorkbenchNowUs();
		state->layoutBeginUs = state->startUs;
		state->model = std::make_unique<CFuncInfoArr>();
		state->model->m_szFilePath = state->parse.filePath.c_str();
		m_workbenchApply = std::move(state);
		m_bFuncInfoArrIsUpToDate = false;
		if( const auto hwndTree = GetItemHwnd(IDC_TREE_FL); hwndTree != nullptr ) {
			// Keep the committed tree visible while its replacement is built.  Redraw
			// is disabled and the control is disabled below, so the user continues to
			// see the old model until the final swap without paying a hide/show layout
			// pass for a large common-control tree.
			::SendMessageW(hwndTree, WM_SETREDRAW, FALSE, 0);
			::EnableWindow(hwndTree, FALSE);
		}
		m_workbenchLastTimings = m_workbenchApply->parse.timings;
		if( !PostWorkbenchApplyStep() ) {
			CancelWorkbenchApply();
			m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
			m_workbenchRequestedVersion = {};
			m_workbenchRequestedGeneration = 0;
			m_workbenchRequestStartUs = 0;
		}
	}catch( const std::exception& ) {
		CancelWorkbenchApply();
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
		m_workbenchLastTimings = {};
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
	}
}

void CDlgFuncList::HandleWorkbenchApplyStep() noexcept
{
	if( !IsWorkbenchApplyCurrent() ) {
		CancelWorkbenchApply();
		return;
	}

	auto fail = [this]() noexcept {
		CancelWorkbenchApply();
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
		m_workbenchLastTimings = {};
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
	};
	auto yield = [this, &fail]() noexcept {
		if( !PostWorkbenchApplyStep() ) fail();
	};

	try {
		auto& state = *m_workbenchApply;
		const std::uint64_t sliceBeginUs = WorkbenchNowUs();
		std::size_t processed = 0;
		const auto shouldYield = [&]() noexcept {
			return processed >= kWorkbenchApplyChunkItems
				|| WorkbenchNowUs() - sliceBeginUs >= kWorkbenchApplyBudgetUs;
		};

		switch( state.stage ) {
		case WorkbenchApplyStage::MaterializeModel: {
			CEditView* const view = reinterpret_cast<CEditView*>(m_lParam);
			CEditDoc* const document = view != nullptr ? view->GetDocument() : nullptr;
			if( document == nullptr ) { fail(); return; }
			while( state.symbolIndex < state.parse.symbols.size() && !shouldYield() ) {
				const auto& symbol = state.parse.symbols[state.symbolIndex++];
				const CLogicInt logicalLine = CLogicInt(std::max(0, symbol.logicalLine));
				const CLogicInt logicalColumn = CLogicInt(std::max(0, symbol.logicalColumn));
				CLayoutPoint layout(0, 0);
				document->m_cLayoutMgr.LogicToLayout(
					CLogicPoint(logicalColumn > 0 ? logicalColumn - CLogicInt(1) : CLogicInt(0),
						logicalLine > 0 ? logicalLine - CLogicInt(1) : CLogicInt(0)),
					&layout);
				const wchar_t* const fileName = symbol.fileName.empty() ? nullptr : symbol.fileName.c_str();
				state.model->AppendData(
					logicalLine,
					logicalColumn,
					layout.GetY2() + CLayoutInt(1),
					layout.GetX2() + CLayoutInt(1),
					symbol.name.c_str(),
					fileName,
					symbol.info,
					symbol.depth);
				++processed;
			}
			if( state.symbolIndex != state.parse.symbols.size() ) { yield(); return; }
			for( const auto& [info, text] : state.parse.appendText ) state.model->SetAppendText(info, text, true);
			state.parse.timings.layoutProjectionUs = WorkbenchNowUs() - state.layoutBeginUs;
			state.stage = WorkbenchApplyStage::PrepareControls;
			break;
		}

		case WorkbenchApplyStage::PrepareControls: {
			const auto hwndTree = GetItemHwnd(IDC_TREE_FL);
			const auto hwndList = GetItemHwnd(IDC_LIST_FL);
			if( hwndTree == nullptr || hwndList == nullptr ) { fail(); return; }
			::SendMessageW(hwndList, WM_SETREDRAW, FALSE, 0);
			::SendMessageW(hwndTree, WM_SETREDRAW, FALSE, 0);
			::ShowWindow(GetItemHwnd(IDC_BUTTON_SETTING), SW_HIDE);
			::ShowWindow(hwndList, SW_HIDE);
			::EnableWindow(hwndTree, FALSE);
			state.clearListRemaining = static_cast<std::size_t>(
				(std::max)(0, ListView_GetItemCount(hwndList)));
			state.clearTreeItems.clear();
			if( const int itemCount = TreeView_GetCount(hwndTree); itemCount > 0 ) {
				state.clearTreeItems.reserve(static_cast<std::size_t>(itemCount));
			}
			state.clearTreeIndex = 0;
			state.treeScanPending.clear();
			SeedWorkbenchTreeScan(hwndTree, state.treeScanPending);
			m_bDummyLParamMode = false;
			m_vecDummylParams.clear();
			m_workbenchTreeItems.assign(state.parse.symbols.size(), nullptr);
			m_workbenchTreeLabels.clear();
			m_workbenchClipboardUsesGenericTree = false;
			m_workbenchClipboardTagJump = false;
			m_workbenchClipboardNoLabel = false;
			m_cmemClipText.SetString(L"");
			m_pcFuncInfoArr = state.model.get();
			m_nOutlineType = state.parse.outlineType;
			m_nListType = state.parse.listType;
			m_nViewType = VIEWTYPE_TREE;
			m_bFuncInfoArrIsUpToDate = false;
			state.nodeItems.clear();
			state.nodeItems.reserve(state.parse.TreeNodes().size());
			state.insertAfter = m_nSortType == SORTTYPE_DEFAULT_DESC ? TVI_FIRST : TVI_LAST;
			if( m_nListType == OUTLINE_JAVA ) {
				::SetWindowText(GetHwnd(), LS(STR_DLGFNCLST_TITLE_JAVA));
			}else{
				::SetWindowText(GetHwnd(), LS(STR_DLGFNCLST_TITLE_CPP));
			}
			state.stage = WorkbenchApplyStage::CollectTree;
			break;
		}

		case WorkbenchApplyStage::CollectTree: {
			const auto hwndTree = GetItemHwnd(IDC_TREE_FL);
			if( hwndTree == nullptr ) { fail(); return; }
			while( !state.treeScanPending.empty() && !shouldYield() ) {
				const HTREEITEM item = state.treeScanPending.back();
				state.treeScanPending.pop_back();
				state.clearTreeItems.push_back(item);
				// Push the sibling first and the child second so the stack walks
				// depth-first while retaining parent-before-child discovery order.
				if( const HTREEITEM sibling = TreeView_GetNextSibling(hwndTree, item);
					sibling != nullptr ) state.treeScanPending.push_back(sibling);
				if( const HTREEITEM child = TreeView_GetChild(hwndTree, item);
					child != nullptr ) state.treeScanPending.push_back(child);
				++processed;
			}
			if( !state.treeScanPending.empty() ) { yield(); return; }
			std::reverse(state.clearTreeItems.begin(), state.clearTreeItems.end());
			state.stage = WorkbenchApplyStage::ClearList;
			break;
		}

		case WorkbenchApplyStage::ClearList: {
			const auto hwndList = GetItemHwnd(IDC_LIST_FL);
			if( hwndList == nullptr ) { fail(); return; }
			while( state.clearListRemaining != 0 && !shouldYield() ) {
				if( !ListView_DeleteItem(hwndList, 0) ) { fail(); return; }
				--state.clearListRemaining;
				++processed;
			}
			if( state.clearListRemaining != 0 ) { yield(); return; }
			state.stage = WorkbenchApplyStage::ClearTree;
			break;
		}

		case WorkbenchApplyStage::ClearTree: {
			const auto hwndTree = GetItemHwnd(IDC_TREE_FL);
			if( hwndTree == nullptr ) { fail(); return; }
			while( state.clearTreeIndex < state.clearTreeItems.size() && !shouldYield() ) {
				if( !TreeView_DeleteItem(hwndTree, state.clearTreeItems[state.clearTreeIndex]) ) {
					fail();
					return;
				}
				++state.clearTreeIndex;
				++processed;
			}
			if( state.clearTreeIndex != state.clearTreeItems.size() ) { yield(); return; }
			state.clearTreeItems.clear();
			state.treeBeginUs = WorkbenchNowUs();
			state.stage = WorkbenchApplyStage::InsertTree;
			break;
		}

		case WorkbenchApplyStage::InsertTree: {
			const auto hwndTree = GetItemHwnd(IDC_TREE_FL);
			if( hwndTree == nullptr ) { fail(); return; }
			while( state.treeIndex < state.parse.TreeNodes().size() && !shouldYield() ) {
				const auto& node = state.parse.TreeNodes()[state.treeIndex];
				if( node.ParentNodeIndex() >= static_cast<int>(state.nodeItems.size()) ) { fail(); return; }
				TV_INSERTSTRUCTW insert{};
				insert.hParent = node.ParentNodeIndex() >= 0
					? state.nodeItems[static_cast<std::size_t>(node.ParentNodeIndex())] : TVI_ROOT;
				insert.hInsertAfter = state.insertAfter;
				insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
				insert.item.pszText = const_cast<wchar_t*>(node.Label().c_str());
				insert.item.lParam = node.SymbolIndex() >= 0 ? node.SymbolIndex() : -1;
				insert.item.iImage = node.SymbolIndex() >= 0 ? WorkbenchSymbolImageIndex(node.Info()) : 0;
				insert.item.iSelectedImage = insert.item.iImage;
				const HTREEITEM item = TreeView_InsertItem(hwndTree, &insert);
				if( item == nullptr ) { fail(); return; }
				state.nodeItems.push_back(item);
				if( node.SymbolIndex() >= 0
					&& static_cast<std::size_t>(node.SymbolIndex()) < m_workbenchTreeItems.size() ) {
					m_workbenchTreeItems[static_cast<std::size_t>(node.SymbolIndex())] = item;
				}
				m_workbenchTreeLabels.emplace(item, WorkbenchTreeLabel{node.Label(), node.Depth()});
				++state.treeIndex;
				++processed;
			}
			if( state.treeIndex != state.parse.TreeNodes().size() ) { yield(); return; }
			state.parse.timings.nativeTreeBuildUs = WorkbenchNowUs() - state.treeBeginUs;
			m_workbenchLastTimings.nativeTreeBuildUs = state.parse.timings.nativeTreeBuildUs;
			state.expansionBeginUs = WorkbenchNowUs();
			state.stage = WorkbenchApplyStage::ExpandTree;
			break;
		}

		case WorkbenchApplyStage::ExpandTree: {
			// Do not expand every node during a result commit.  Common-controls
			// perform layout and notification work for each expansion, which turns
			// an otherwise bounded apply into another O(tree-size) UI stall.  Roots
			// remain visible, while users can expand children on demand; selection
			// continues to choose the nearest already-visible ancestor.
			state.parse.timings.expansionUs = WorkbenchNowUs() - state.expansionBeginUs;
			state.lineMarksBeginUs = WorkbenchNowUs();
			state.stage = WorkbenchApplyStage::ApplyLineMarks;
			break;
		}

		case WorkbenchApplyStage::ApplyLineMarks: {
			CEditView* const view = reinterpret_cast<CEditView*>(m_lParam);
			CEditDoc* const document = view != nullptr ? view->GetDocument() : nullptr;
			if( document == nullptr ) { fail(); return; }
			if( !state.lineMarksReset ) {
				if( state.lineMarkResetCursor == nullptr ) {
					state.lineMarkResetCursor = document->m_cDocLineMgr.GetDocLineTop();
				}
				while( state.lineMarkResetCursor != nullptr && !shouldYield() ) {
					CDocLine* const line = state.lineMarkResetCursor;
					state.lineMarkResetCursor = line->GetNextLine();
					CFuncListManager().SetLineFuncList(line, false);
					++processed;
				}
				if( state.lineMarkResetCursor != nullptr ) { yield(); return; }
				state.lineMarksReset = true;
			}
			while( state.lineIndex < state.model->GetNum() && !shouldYield() ) {
				const CFuncInfo* const info = state.model->GetAt(state.lineIndex++);
				if( info != nullptr && info->m_nFuncLineCRLF > 0 ) {
					if( CDocLine* const line = document->m_cDocLineMgr.GetLine(info->m_nFuncLineCRLF - 1) ) {
						CFuncListManager().SetLineFuncList(line, true);
					}
				}
				++processed;
			}
			if( state.lineIndex != static_cast<std::size_t>(state.model->GetNum()) ) { yield(); return; }
			state.parse.timings.lineMarkProjectionUs = WorkbenchNowUs() - state.lineMarksBeginUs;
			state.stage = WorkbenchApplyStage::Finalize;
			break;
		}

		case WorkbenchApplyStage::Finalize: {
			const auto selectionBegin = WorkbenchNowUs();
			m_workbenchCommittedModel = std::move(state.model);
			m_pcFuncInfoArr = m_workbenchCommittedModel.get();
			m_bFuncInfoArrIsUpToDate = true;
			CommitWorkbenchModel();
			CEditView* const view = reinterpret_cast<CEditView*>(m_lParam);
			if( view != nullptr ) {
				m_nCurLine = view->GetCaret().GetCaretLayoutPos().GetY2() + CLayoutInt(1);
				m_nCurCol = view->GetCaret().GetCaretLayoutPos().GetX2() + CLayoutInt(1);
				int selection = -1;
				if( GetFuncInfoIndex(m_nCurLine, m_nCurCol, &selection) ) SetItemSelection(selection, false);
			}
			state.parse.timings.selectionUs = WorkbenchNowUs() - selectionBegin;
			::ShowWindow(GetItemHwnd(IDC_BUTTON_SETTING), SW_HIDE);
			::ShowWindow(GetItemHwnd(IDC_LIST_FL), SW_HIDE);
			::SendMessageW(GetItemHwnd(IDC_TREE_FL), WM_SETREDRAW, TRUE, 0);
			::SendMessageW(GetItemHwnd(IDC_LIST_FL), WM_SETREDRAW, TRUE, 0);
			::EnableWindow(GetItemHwnd(IDC_TREE_FL), TRUE);
			// InsertTree already supplied the complete value-owned labels and icon
			// indices.  Do not synchronously walk every visible root here to measure
			// and shorten text: a document with thousands of top-level classes makes
			// that otherwise "visible-only" pass a single long UI stall.  The native
			// TreeView clips the complete labels, while later explicit appearance or
			// resize requests can still perform the optional compacting pass.
			m_workbenchTreeContentDirty = false;
			m_workbenchAppearanceDirty = false;
			::RedrawWindow(GetHwnd(), nullptr, nullptr,
				RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE | RDW_NOINTERNALPAINT);
			state.parse.timings.appearanceUs = 0;
			state.parse.timings.nativeCommitUs = WorkbenchNowUs() - state.startUs;
			state.parse.timings.totalUs = m_workbenchRequestStartUs != 0
				? WorkbenchNowUs() - m_workbenchRequestStartUs : WorkbenchNowUs() - state.startUs;
			m_workbenchLastTimings = state.parse.timings;
			m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Parsed;
			m_workbenchRequestedVersion = {};
			m_workbenchRequestedGeneration = 0;
			m_workbenchRequestStartUs = 0;
			m_workbenchApply.reset();
			return;
		}
		}

		if( m_workbenchApply != nullptr && IsWorkbenchApplyCurrent() ) yield();
	}catch( const std::exception& ) {
		fail();
	}
}

workbench::outline::OutlineWorkerStateSnapshot CDlgFuncList::GetWorkbenchWorkerState() const noexcept
{
	return m_workbenchParser != nullptr
		? m_workbenchParser->GetStateSnapshot()
		: workbench::outline::OutlineWorkerStateSnapshot{};
}

void CDlgFuncList::HandleWorkbenchWorkerResult( LPARAM lParam ) noexcept
{
	if( m_workbenchParser == nullptr ) return;
	auto result = m_workbenchParser->TakePendingResult(
		reinterpret_cast<workbench::outline::OutlineWorkerResult*>(lParam));
	if( result == nullptr ) return;
	CommitWorkbenchParseResult(std::move(result));
}

void CDlgFuncList::CommitWorkbenchParseResult(
	std::unique_ptr<workbench::outline::OutlineWorkerResult> result ) noexcept
{
	if( result == nullptr || !IsWorkbenchMode() ) return;
	if( result->generation == 0 ) {
		// Generation zero is an invalid sentinel.  Do not let a malformed or
		// late notification consume a newer request marker.
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Superseded;
		m_workbenchLastTimings = result->parse.timings;
		return;
	}
	if( result->generation != m_workbenchRequestedGeneration ) {
		// Out-of-order completion is a terminal stale result for this delivery,
		// but the newer request remains the owner of the in-flight markers.
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Superseded;
		m_workbenchLastTimings = result->parse.timings;
		return;
	}

	auto finishSuperseded = [&]() noexcept {
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Superseded;
		m_workbenchLastTimings = result->parse.timings;
		m_workbenchLastTimings.totalUs = m_workbenchRequestStartUs != 0
			? WorkbenchNowUs() - m_workbenchRequestStartUs : result->parse.timings.totalUs;
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
	};

	CEditView* const view = reinterpret_cast<CEditView*>(m_lParam);
	CEditDoc* const document = view != nullptr ? view->GetDocument() : nullptr;
	if( document == nullptr ) {
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Closed;
		m_workbenchLastTimings = result->parse.timings;
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
		return;
	}
	if( GetWorkbenchDocumentVersion() != result->parse.documentVersion ) {
		finishSuperseded();
		return;
	}

	auto finishFailure = [&]() noexcept {
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Failed;
		m_workbenchLastTimings = result->parse.timings;
		m_workbenchLastTimings.totalUs = m_workbenchRequestStartUs != 0
			? WorkbenchNowUs() - m_workbenchRequestStartUs : result->parse.timings.totalUs;
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
	};

	if( result->terminal == workbench::outline::OutlineWorkerTerminal::Failed ) {
		// A current failure is observable and terminal, while the previously
		// committed model remains intact.  Older failures are rejected above.
		finishFailure();
		return;
	}
	if( result->terminal == workbench::outline::OutlineWorkerTerminal::Closed ) {
		m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Closed;
		m_workbenchLastTimings = result->parse.timings;
		m_workbenchRequestedVersion = {};
		m_workbenchRequestedGeneration = 0;
		m_workbenchRequestStartUs = 0;
		m_bFuncInfoArrIsUpToDate = false;
		return;
	}
	if( result->terminal == workbench::outline::OutlineWorkerTerminal::Cancelled
		|| result->terminal == workbench::outline::OutlineWorkerTerminal::Superseded ) {
		finishSuperseded();
		return;
	}
	if( result->terminal != workbench::outline::OutlineWorkerTerminal::Parsed ) {
		finishFailure();
		return;
	}

	BeginWorkbenchApply(std::move(result));
}

int CDlgFuncList::WorkbenchSymbolImageIndex( int info ) noexcept
{
	switch( info & FUNCINFO_INFOMASK ){
	case FL_OBJ_DECLARE:
		return 1;
	case FL_OBJ_FUNCTION:
		return 2;
	case FL_OBJ_CLASS:
		return 3;
	case FL_OBJ_STRUCT:
	case FL_OBJ_UNION:
		return 4;
	case FL_OBJ_ENUM:
		return 5;
	case FL_OBJ_NAMESPACE:
		return 6;
	case FL_OBJ_INTERFACE:
		return 7;
	case FL_OBJ_GLOBAL:
		return 8;
	case FL_OBJ_DEFINITION:
		return 0;
	default:
		return 9;
	}
}

std::wstring_view CDlgFuncList::WorkbenchSymbolCodiconName( int imageIndex ) noexcept
{
	switch( imageIndex ){
	case 1:
		return L"symbol-method";
	case 2:
		return L"symbol-function";
	case 3:
		return L"symbol-class";
	case 4:
		return L"symbol-structure";
	case 5:
		return L"symbol-enum";
	case 6:
		return L"symbol-namespace";
	case 7:
		return L"symbol-interface";
	case 8:
		return L"symbol-variable";
	case 9:
		return L"symbol-property";
	case 0:
	default:
		return L"symbol-misc";
	}
}

void CDlgFuncList::SetWorkbenchAppearance(
	COLORREF text,
	COLORREF background,
	COLORREF hover,
	COLORREF selection,
	COLORREF selectionText,
	HFONT font,
	int itemHeight,
	HIMAGELIST symbolImages ) noexcept
{
	const int normalizedItemHeight = (std::max)(1, itemHeight);
	const bool treeContentChanged = m_workbenchFont != font
		|| m_workbenchItemHeight != normalizedItemHeight
		|| m_workbenchSymbolImages != symbolImages;
	const bool appearanceChanged = treeContentChanged
		|| m_workbenchText != text
		|| m_workbenchBackground != background
		|| m_workbenchHover != hover
		|| m_workbenchSelection != selection
		|| m_workbenchSelectionText != selectionText;
	m_workbenchText = text;
	m_workbenchBackground = background;
	m_workbenchHover = hover;
	m_workbenchSelection = selection;
	m_workbenchSelectionText = selectionText;
	m_workbenchFont = font;
	m_workbenchItemHeight = normalizedItemHeight;
	m_workbenchSymbolImages = symbolImages;
	m_workbenchAppearanceDirty = m_workbenchAppearanceDirty || appearanceChanged;
	m_workbenchTreeContentDirty = m_workbenchTreeContentDirty || treeContentChanged;
	if( m_workbenchAppearanceDirty || m_workbenchTreeContentDirty ) ApplyWorkbenchAppearance();
}

struct WorkbenchTreeTextMetrics final {
	HWND window = nullptr;
	HDC dc = nullptr;
	HGDIOBJ oldFont = nullptr;
	int clientWidth = 0;
	int indent = 0;
	int imageWidth = 0;
	int scrollWidth = 0;
	int ellipsisWidth = 0;
	~WorkbenchTreeTextMetrics() noexcept
	{
		if( dc != nullptr ) {
			if( oldFont != nullptr ) (void)::SelectObject(dc, oldFont);
			if( window != nullptr ) ::ReleaseDC(window, dc);
		}
	}
};

static int TreeDummylParamToFuncInfoIndex(const std::vector<int>& vec, LPARAM lParam);

static std::wstring CompactWorkbenchTreeText(
	const WorkbenchTreeTextMetrics& metrics, const wchar_t* text, int depth );

void CDlgFuncList::ApplyWorkbenchAppearance() noexcept
{
	const auto appearanceBegin = std::chrono::steady_clock::now();
	if( !IsWorkbenchMode() || GetHwnd() == nullptr ) return;
	if( !m_workbenchAppearanceDirty && !m_workbenchTreeContentDirty ) return;
	const bool updateTreeContent = m_workbenchTreeContentDirty;

	const HWND hwndTree = GetItemHwnd( IDC_TREE_FL );
	if( hwndTree != nullptr ){
		const LONG_PTR currentStyle = ::GetWindowLongPtrW( hwndTree, GWL_STYLE );
		const LONG_PTR style = workbench::outline::NormalizeWorkbenchOutlineTreeStyle(
			static_cast<DWORD>(currentStyle) );
		const LONG_PTR currentExStyle = ::GetWindowLongPtrW( hwndTree, GWL_EXSTYLE );
		const LONG_PTR exStyle = workbench::outline::NormalizeWorkbenchOutlineControlExStyle(
			static_cast<DWORD>(currentExStyle) );
		// Redraw() reaches here for every document update. Recalculate the native frame only
		// when the edge styles actually change; otherwise the common control can flash its
		// non-client border over the workbench surface.
		const bool frameChanged = currentStyle != style || currentExStyle != exStyle;
		if( currentStyle != style ) ::SetWindowLongPtrW( hwndTree, GWL_STYLE, style );
		if( currentExStyle != exStyle ) ::SetWindowLongPtrW( hwndTree, GWL_EXSTYLE, exStyle );
		::SendMessageW( hwndTree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER );
		::SendMessageW( hwndTree, TVM_SETITEMHEIGHT, static_cast<WPARAM>(m_workbenchItemHeight), 0 );
		if( m_workbenchFont != nullptr ) ::SendMessageW( hwndTree, WM_SETFONT, reinterpret_cast<WPARAM>(m_workbenchFont), FALSE );
		TreeView_SetBkColor( hwndTree, m_workbenchBackground );
		TreeView_SetTextColor( hwndTree, m_workbenchText );
		TreeView_SetImageList( hwndTree, m_workbenchSymbolImages, TVSIL_NORMAL );

		if( updateTreeContent ){
			WorkbenchTreeTextMetrics metrics;
			metrics.window = hwndTree;
			RECT client{};
			if( ::GetClientRect(hwndTree, &client) ) {
				metrics.clientWidth = client.right - client.left;
				metrics.indent = static_cast<int>(TreeView_GetIndent(hwndTree));
				if( const HIMAGELIST images = TreeView_GetImageList(hwndTree, TVSIL_NORMAL); images != nullptr ) {
					int imageHeight = 0;
					(void)::ImageList_GetIconSize(images, &metrics.imageWidth, &imageHeight);
				}
				metrics.scrollWidth = ::GetSystemMetrics(SM_CXVSCROLL) + 6;
				metrics.dc = ::GetDC(hwndTree);
				if( metrics.dc != nullptr ) {
					const HFONT font = m_workbenchFont != nullptr
						? m_workbenchFont
						: reinterpret_cast<HFONT>(::SendMessageW(hwndTree, WM_GETFONT, 0, 0));
					if( font != nullptr ) metrics.oldFont = ::SelectObject(metrics.dc, font);
					SIZE ellipsis{};
					if( ::GetTextExtentPoint32W(metrics.dc, L"...", 3, &ellipsis) ) {
						metrics.ellipsisWidth = ellipsis.cx;
					}
				}
			}
			std::vector<HTREEITEM> pending;
			// Large workbench trees are populated incrementally.  Updating every
			// off-screen label here would recreate the same UI-thread stall that the
			// staged insertion avoids, so defer those rows until they become visible.
			for( HTREEITEM visible = TreeView_GetFirstVisible(hwndTree);
				visible != nullptr;
				visible = TreeView_GetNextVisible(hwndTree, visible) ) {
				pending.push_back(visible);
			}
			while( !pending.empty() ){
				const HTREEITEM itemHandle = pending.back();
				pending.pop_back();
				TVITEMW source{};
				source.mask = TVIF_PARAM;
				source.hItem = itemHandle;
				(void)TreeView_GetItem( hwndTree, &source );
				std::wstring compactText;
				const CFuncInfo* info = nullptr;
				int modelIndex = -1;
				if( source.lParam >= 0 ) {
					modelIndex = m_bDummyLParamMode
						? TreeDummylParamToFuncInfoIndex(m_vecDummylParams, source.lParam)
						: static_cast<int>(source.lParam);
				}
				if( m_pcFuncInfoArr != nullptr
					&& modelIndex >= 0 && modelIndex < m_pcFuncInfoArr->GetNum() ){
					info = m_pcFuncInfoArr->GetAt(static_cast<size_t>(modelIndex));
				}
				const auto label = m_workbenchTreeLabels.find(itemHandle);
				if( label != m_workbenchTreeLabels.end() ) {
					compactText = CompactWorkbenchTreeText(
						metrics, label->second.Text().c_str(), label->second.Depth() );
				}
				TVITEMW item{};
				item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
				item.hItem = itemHandle;
				const int imageIndex = info != nullptr ? WorkbenchSymbolImageIndex(info->m_nInfo) : 0;
				item.iImage = imageIndex;
				item.iSelectedImage = imageIndex;
				if( !compactText.empty() ){
					item.mask |= TVIF_TEXT;
					item.pszText = compactText.data();
				}
				(void)TreeView_SetItem( hwndTree, &item );
			}
		}
		if( frameChanged ){
			::SetWindowPos( hwndTree, nullptr, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED );
		}
	}

	const HWND hwndList = GetItemHwnd( IDC_LIST_FL );
	if( hwndList != nullptr ){
		const LONG_PTR currentStyle = ::GetWindowLongPtrW( hwndList, GWL_STYLE );
		const LONG_PTR style = workbench::outline::NormalizeWorkbenchOutlineListStyle(
			static_cast<DWORD>(currentStyle) );
		const LONG_PTR currentExStyle = ::GetWindowLongPtrW( hwndList, GWL_EXSTYLE );
		const LONG_PTR exStyle = workbench::outline::NormalizeWorkbenchOutlineControlExStyle(
			static_cast<DWORD>(currentExStyle) );
		const bool frameChanged = currentStyle != style || currentExStyle != exStyle;
		if( currentStyle != style ) ::SetWindowLongPtrW( hwndList, GWL_STYLE, style );
		if( currentExStyle != exStyle ) ::SetWindowLongPtrW( hwndList, GWL_EXSTYLE, exStyle );
		if( m_workbenchFont != nullptr ) ::SendMessageW( hwndList, WM_SETFONT, reinterpret_cast<WPARAM>(m_workbenchFont), FALSE );
		ListView_SetTextColor( hwndList, m_workbenchText );
		ListView_SetTextBkColor( hwndList, m_workbenchBackground );
		ListView_SetBkColor( hwndList, m_workbenchBackground );
		if( frameChanged ){
			::SetWindowPos( hwndList, nullptr, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED );
		}
	}
	m_workbenchAppearanceDirty = false;
	m_workbenchTreeContentDirty = false;
	::RedrawWindow( GetHwnd(), nullptr, nullptr,
		RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE | RDW_NOINTERNALPAINT );
	m_workbenchLastTimings.appearanceUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - appearanceBegin).count());
}

/*!
	標準以外のメッセージを捕捉する

	@date 2007.11.07 ryoji 新規
*/
INT_PTR CDlgFuncList::DispatchEvent( HWND hWnd, UINT wMsg, WPARAM wParam, LPARAM lParam )
{
	if( wMsg == workbench::outline::OutlineParserWorker::kWorkerResultMessage ) {
		HandleWorkbenchWorkerResult(lParam);
		return 0;
	}
	if( wMsg == kWorkbenchApplyMessage ) {
		HandleWorkbenchApplyStep();
		return 0;
	}
	INT_PTR result;
	result = CDialog::DispatchEvent( hWnd, wMsg, wParam, lParam );
	if( wMsg == WM_PAINT && IsWorkbenchMode() && m_workbenchNativeSurfaceSubmitter ) {
		RECT client{};
		if( ::GetClientRect( hWnd, &client ) != FALSE && client.right > client.left && client.bottom > client.top ) {
			if( HDC dc = ::GetDC( hWnd ); dc != nullptr ) {
				m_workbenchNativeSurfaceSubmitter( dc, client );
				::ReleaseDC( hWnd, dc );
			}
		}
	}

	switch( wMsg ){
	case WM_ACTIVATEAPP:
		if( IsDocking() || IsWorkbenchMode() )
			break;

		// 自分が最初にアクティブ化された場合は一旦編集ウィンドウをアクティブ化して戻す
		//
		// Note. このダイアログは他とは異なるウィンドウスタイルのため閉じたときの挙動が異なる．
		// 他はスレッド内最近アクティブなウィンドウがアクティブになるが，このダイアログでは
		// セッション内全体での最近アクティブウィンドウがアクティブになってしまう．
		// それでは都合が悪いので，特別に以下の処理を行って他と同様な挙動が得られるようにする．
		if( (BOOL)wParam ){
			if( ::GetActiveWindow() == GetHwnd() ){
				::SetActiveWindow( GetEditWnd().GetHwnd() );
				BlockingHook( nullptr );	// キュー内に溜まっているメッセージを処理
				::SetActiveWindow( GetHwnd() );
				return 0L;
			}
		}
		break;

	case WM_NCPAINT:
		if( IsWorkbenchMode() ) return result;
		return OnNcPaint( hWnd, wMsg, wParam, lParam );
	case WM_NCCALCSIZE:
		if( IsWorkbenchMode() ) return result;
		return OnNcCalcSize( hWnd, wMsg, wParam, lParam );
	case WM_NCHITTEST:
		if( IsWorkbenchMode() ) return result;
		return OnNcHitTest( hWnd, wMsg, wParam, lParam );
	case WM_NCMOUSEMOVE:
		if( IsWorkbenchMode() ) return result;
		return OnNcMouseMove( hWnd, wMsg, wParam, lParam );
	case WM_MOUSEMOVE:
		if( IsWorkbenchMode() ) return result;
		return OnMouseMove( hWnd, wMsg, wParam, lParam );
	case WM_NCLBUTTONDOWN:
		if( IsWorkbenchMode() ) return result;
		return OnNcLButtonDown( hWnd, wMsg, wParam, lParam );
	case WM_LBUTTONUP:
		if( IsWorkbenchMode() ) return result;
		return OnLButtonUp( hWnd, wMsg, wParam, lParam );
	case WM_NCRBUTTONUP:
		if( IsDocking() && wParam == HTCAPTION ){
			// ドッキングのときはコンテキストメニューを明示的に呼び出す必要があるらしい
			::SendMessage( GetHwnd(), WM_CONTEXTMENU, (WPARAM)GetHwnd(), lParam );
			return 1L;
		}
		break;
	case WM_GETMINMAXINFO:
		return OnMinMaxInfo( lParam );
	case WM_SETTEXT:
		if( IsDocking() ){
			// キャプションを再描画する
			// ※ この時点ではまだテキスト設定されていないので RDW_UPDATENOW では NG
			::RedrawWindow( hWnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_NOINTERNALPAINT );
		}
		break;
	case WM_MOUSEACTIVATE:
		if( IsDocking() ){
			// 分割バー以外の場所ならフォーカス移動
			if( !(HTLEFT <= LOWORD(lParam) && LOWORD(lParam) <= HTBOTTOMRIGHT) ){
				::SetFocus( GetHwnd() );
			}
		}
		break;
	case WM_COMMAND:
		if( IsDocking() ){
			// コンボボックスのフォーカスが変化したらキャプションを再描画する（アクティブ／非アクティブ切替）
			if( LOWORD(wParam) == IDC_COMBO_nSortType ){
				if( HIWORD(wParam) == CBN_SETFOCUS || HIWORD(wParam) == CBN_KILLFOCUS ){
					::RedrawWindow( hWnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOINTERNALPAINT );
				}
			}
		}
		break;
	case WM_NOTIFY:
		if( IsDocking() ){
			// ツリーやリストのフォーカスが変化したらキャプションを再描画する（アクティブ／非アクティブ切替）
			NMHDR* pNMHDR = (NMHDR*)lParam;
			if( pNMHDR->code == NM_SETFOCUS || pNMHDR->code == NM_KILLFOCUS ){
				::RedrawWindow( hWnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOINTERNALPAINT );
			}
		}
		{
			NMHDR* pNMHDR = (NMHDR*)lParam;
			if( pNMHDR->code == TVN_ITEMEXPANDING ){
				NMTREEVIEW* pNMTREEVIEW = (NMTREEVIEW*)lParam;
				TVITEM* pItem = &(pNMTREEVIEW->itemNew);
				if( m_nListType == OUTLINE_FILETREE ){
					SetTreeFileSub( pItem->hItem, nullptr );
				}
			}
		}
		break;
	default:
		break;
	}

	return result;
}

void CDlgFuncList::SetWorkbenchNativeSurfaceSubmitter(
	WorkbenchNativeSurfaceSubmitter submitter ) noexcept
{
	m_workbenchNativeSurfaceSubmitter = std::move( submitter );
}

void CDlgFuncList::BuildWorkbenchClipboardText()
{
	if( !IsWorkbenchMode() || m_pcFuncInfoArr == nullptr ) return;
	m_cmemClipText.SetString(L"");
	if( !HasCurrentWorkbenchModel() ) return;
	if( m_workbenchClipboardUsesGenericTree ) {
		// SetTree's legacy format is intentionally retained for synchronous
		// workbench/plugin analyzers.  The async C++/Java path below uses the
		// historical SetTreeJava format instead.
		for( int index = 0; index < m_pcFuncInfoArr->GetNum(); ++index ) {
			const CFuncInfo* const info = m_pcFuncInfoArr->GetAt(index);
			if( info == nullptr || !info->IsAddClipText() ) continue;
			CNativeW text;
			if( m_workbenchClipboardTagJump ) {
				const WCHAR* fileName = info->m_cmemFileName.GetStringPtr();
				if( fileName == nullptr ) fileName = m_pcFuncInfoArr->m_szFilePath;
				text.AppendString(fileName);
				if( info->m_nFuncLineCRLF > 0 ) {
					WCHAR line[32];
					auto_sprintf(line, L"(%d,%d): ", info->m_nFuncLineCRLF, info->m_nFuncColCRLF);
					text.AppendString(line);
				}
			}
			if( !m_workbenchClipboardNoLabel ) {
				for( int depth = 0; depth < info->m_nDepth; ++depth ) text.AppendString(L"  ");
				text.AppendString(L" ");
				text.AppendNativeData(info->m_cmemFuncName);
			}
			text.AppendString(L"\r\n");
			m_cmemClipText.AppendNativeData(text);
		}
		return;
	}
	for( int index = 0; index < m_pcFuncInfoArr->GetNum(); ++index ) {
		const CFuncInfo* const info = m_pcFuncInfoArr->GetAt(index);
		if( info == nullptr ) continue;
		WCHAR prefix[2048];
		auto_sprintf(
			prefix,
			L"%s(%d,%d): ",
			m_pcFuncInfoArr->m_szFilePath.c_str(),
			info->m_nFuncLineCRLF,
			info->m_nFuncColCRLF );
		m_cmemClipText.AppendString(prefix);
		m_cmemClipText.AppendNativeData(info->m_cmemFuncName);
		if( info->m_nInfo == FL_OBJ_DECLARE ) {
			m_cmemClipText.AppendString(m_pcFuncInfoArr->GetAppendText(FL_OBJ_DECLARE).c_str());
		}
		m_cmemClipText.AppendString(L"\r\n");
	}
}

/* モードレスダイアログの表示 */
/*
 * @note 2011.06.25 syat nOutlineTypeを追加
 *   nOutlineTypeとnListTypeはほとんどの場合同じ値だが、プラグインの場合は例外で、
 *   nOutlineTypeはアウトライン解析のID、nListTypeはプラグイン内で指定するリスト形式となる。
 */
HWND CDlgFuncList::DoModeless(
	HINSTANCE		hInstance,
	HWND			hwndParent,
	LPARAM			lParam,
	CFuncInfoArr*	pcFuncInfoArr,
	CLayoutInt		nCurLine,
	CLayoutInt		nCurCol,
	int				nOutlineType,		
	int				nListType,
	bool			bLineNumIsCRLF		/* 行番号の表示 false=折り返し単位／true=改行単位 */
)
{
	CEditView* pcEditView=(CEditView*)lParam;
	if( !pcEditView ) return nullptr;
	ObserveWorkbenchDocument( pcEditView );
	if( IsWorkbenchMode() && pcFuncInfoArr == nullptr ) {
		if( m_workbenchCommittedModel == nullptr ) m_workbenchCommittedModel = std::make_unique<CFuncInfoArr>();
		m_pcFuncInfoArr = m_workbenchCommittedModel.get();
		m_bFuncInfoArrIsUpToDate = false;
	}else{
		m_pcFuncInfoArr = pcFuncInfoArr;	/* 関数情報配列 */
		m_bFuncInfoArrIsUpToDate = true;
	}
	m_nCurLine = nCurLine;				/* 現在行 */
	m_nCurCol = nCurCol;				/* 現在桁 */
	m_nOutlineType = nOutlineType;		/* アウトライン解析の種別 */
	m_nListType = nListType;			/* 一覧の種類 */
	m_bLineNumIsCRLF = bLineNumIsCRLF;	/* 行番号の表示 false=折り返し単位／true=改行単位 */
	m_nDocType = pcEditView->GetDocument()->m_cDocType.GetDocumentType().GetIndex();
	CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
	m_nSortCol = m_type.m_nOutlineSortCol;
	m_nSortColOld = m_nSortCol;
	m_bSortDesc = m_type.m_bOutlineSortDesc;
	m_nSortType = m_type.m_nOutlineSortType;

	bool bType = (ProfDockSet() != 0);
	if( bType ){
		m_type.m_nDockOutline = m_nOutlineType;
		SetTypeConfig( CTypeConfig(m_nDocType), m_type );
	}else{
		CommonSet().m_nDockOutline = m_nOutlineType;
	}

	// 2007.04.18 genta : 「フォーカスを移す」と「自動的に閉じる」がチェックされている場合に
	// ダブルクリックを行うと，trueのまま残ってしまうので，ウィンドウを開いたときにリセットする．
	m_bWaitTreeProcess = false;

	m_eDockSide = ProfDockSide();
	if( IsWorkbenchMode() && m_hwndWorkbenchParent == nullptr ) return nullptr;
	HWND hwndRet;
	if( IsDocking() || IsWorkbenchMode() ){
		// ドッキング用にダイアログテンプレートに手を加えてから表示する（WS_CHILD化）
		HINSTANCE hInstance2 = CSelectLang::getLangRsrcInstance();
		if( !m_pDlgTemplate || m_lastRcInstance != hInstance2 ){
			HRSRC hResInfo = ::FindResource( hInstance2, MAKEINTRESOURCE(IDD_FUNCLIST), RT_DIALOG );
			if( !hResInfo ) return nullptr;
			HGLOBAL hResData = ::LoadResource( hInstance2, hResInfo );
			if( !hResData ) return nullptr;
			m_pDlgTemplate = (LPDLGTEMPLATE)::LockResource( hResData );
			if( !m_pDlgTemplate ) return nullptr;
			m_dwDlgTmpSize = ::SizeofResource( hInstance2, hResInfo );
			// 言語切り替えでリソースがアンロードされていないか確認するためインスタンスを記憶する
			m_lastRcInstance = hInstance2;
		}
		LPDLGTEMPLATE pDlgTemplate = (LPDLGTEMPLATE)::GlobalAlloc( GMEM_FIXED, m_dwDlgTmpSize );
		if( !pDlgTemplate ) return nullptr;
		::CopyMemory( pDlgTemplate, m_pDlgTemplate, m_dwDlgTmpSize );
		pDlgTemplate->style = (WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | DS_SETFONT)
			| (IsWorkbenchMode()? DS_CONTROL: 0);
		if( IsWorkbenchMode()
			&& !workbench::outline::NormalizeWorkbenchOutlineDialogTemplate(pDlgTemplate, m_dwDlgTmpSize) ){
			::GlobalFree( pDlgTemplate );
			return nullptr;
		}
		HWND dialogParent = IsWorkbenchMode()? m_hwndWorkbenchParent: MyGetAncestor(hwndParent, GA_ROOT);
		hwndRet = CDialog::DoModeless( hInstance, dialogParent, pDlgTemplate, lParam, SW_HIDE );
		::GlobalFree( pDlgTemplate );
		if( !IsWorkbenchMode() ) GetEditWnd().EndLayoutBars( m_bEditWndReady );	// 画面の再レイアウト
	}else{
		hwndRet = CDialog::DoModeless( hInstance, MyGetAncestor(hwndParent, GA_ROOT), IDD_FUNCLIST, lParam, SW_SHOW );
	}
	if( hwndRet != nullptr && (!IsWorkbenchMode() || pcFuncInfoArr != nullptr) ) CommitWorkbenchModel();
	return hwndRet;
}

/* モードレス時：検索対象となるビューの変更 */
void CDlgFuncList::ChangeView( LPARAM pcEditView )
{
	m_lParam = pcEditView;
	ObserveWorkbenchDocument( reinterpret_cast<CEditView*>(pcEditView) );
	return;
}

/*! ダイアログデータの設定 */
void CDlgFuncList::SetData()
{
	if( IsWorkbenchMode() && m_workbenchApply != nullptr ) return;
	if( IsWorkbenchMode() ) {
		m_workbenchTreeContentDirty = true;
		// These are commit-scoped scratch measurements.  Retained timings are
		// published only after the complete result projection succeeds.
		m_workbenchLastTimings.lineMarkProjectionUs = 0;
		m_workbenchLastTimings.nativeTreeBuildUs = 0;
		m_workbenchLastTimings.appearanceUs = 0;
		m_workbenchLastTimings.expansionUs = 0;
	}
	HWND			hwndList;
	HWND			hwndTree;
	hwndList = GetItemHwnd( IDC_LIST_FL );
	hwndTree = GetItemHwnd( IDC_TREE_FL );

	m_bDummyLParamMode = false;
	m_vecDummylParams.clear();
	m_workbenchTreeItems.clear();
	m_workbenchTreeLabels.clear();
	m_workbenchClipboardUsesGenericTree = false;
	m_workbenchClipboardTagJump = false;
	m_workbenchClipboardNoLabel = false;

	::SendMessage(hwndList, WM_SETREDRAW, (WPARAM)FALSE, 0);
	::SendMessage(hwndTree, WM_SETREDRAW, (WPARAM)FALSE, 0);
	ListView_DeleteAllItems( hwndList );
	TreeView_DeleteAllItems( hwndTree );
	::ShowWindow( GetItemHwnd(IDC_BUTTON_SETTING), SW_HIDE );
	const HTREEITEM hInsertAfter = (m_nSortType == SORTTYPE_DEFAULT_DESC) ? TVI_FIRST : TVI_LAST;

	const auto lineMarkBegin = std::chrono::steady_clock::now();
	SetDocLineFuncList();
	if( IsWorkbenchMode() ) {
		m_workbenchLastTimings.lineMarkProjectionUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - lineMarkBegin).count());
	}
	const auto nativeTreeBegin = std::chrono::steady_clock::now();
	if( OUTLINE_C_CPP == m_nListType || OUTLINE_CPP == m_nListType ){	/* C++メソッドリスト */
		m_nViewType = VIEWTYPE_TREE;
		SetTreeJava( GetHwnd(), hInsertAfter, TRUE );	// Jan. 04, 2002 genta Java Method Treeに統合
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_CPP) );
	}
	else if( OUTLINE_FILE == m_nListType ){	//@@@ 2002.04.01 YAZAKI アウトライン解析にルールファイル導入
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_RULE) );
	}
	else if( OUTLINE_WZTXT == m_nListType ){ //@@@ 2003.05.20 zenryaku 階層付テキストアウトライン解析
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_WZ) ); //	2003.06.22 Moca 名前変更
	}
	else if( OUTLINE_HTML == m_nListType ){ //@@@ 2003.05.20 zenryaku HTMLアウトライン解析
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);
		::SetWindowText( GetHwnd(), L"HTML" );
	}
	else if( OUTLINE_TEX == m_nListType ){ //@@@ 2003.07.20 naoh TeXアウトライン解析
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);
		::SetWindowText( GetHwnd(), L"TeX" );
	}
	else if( OUTLINE_TEXT == m_nListType ){ /* テキスト・トピックリスト */
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);	//@@@ 2002.04.01 YAZAKI テキストトピックツリーも、汎用SetTreeを呼ぶように変更。
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_TEXT) );
	}
	else if( OUTLINE_JAVA == m_nListType ){ /* Javaメソッドツリー */
		m_nViewType = VIEWTYPE_TREE;
		SetTreeJava( GetHwnd(), hInsertAfter, TRUE );
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_JAVA) );
	}
	//	2007.02.08 genta Python追加
	else if( OUTLINE_PYTHON == m_nListType ){ /* Python メソッドツリー */
		m_nViewType = VIEWTYPE_TREE;
		SetTree( hInsertAfter, true );
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_PYTHON) );
	}
	else if( OUTLINE_COBOL == m_nListType ){ /* COBOL アウトライン */
		m_nViewType = VIEWTYPE_TREE;
		SetTreeJava( GetHwnd(), hInsertAfter, FALSE );
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_COBOL) );
	}
	else if( OUTLINE_VB == m_nListType ){	/* VisualBasic アウトライン */
		m_nViewType = IsWorkbenchMode() ? VIEWTYPE_TREE : VIEWTYPE_LIST;
		if( IsWorkbenchMode() ) SetTree(hInsertAfter);
		else SetListVB();
		::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_VB) );
	}
	else if( OUTLINE_XML == m_nListType ){ // XMLツリー
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);
		::SetWindowText( GetHwnd(), L"XML" );
	}
	else if ( OUTLINE_FILETREE == m_nListType ){
		m_nViewType = VIEWTYPE_TREE;
		SetTreeFile();
		::SetWindowText( GetHwnd(), LS(F_FILETREE) );	// ファイルツリー
	}
	else if( OUTLINE_TREE == m_nListType ){ /* 汎用ツリー */
		m_nViewType = VIEWTYPE_TREE;
		SetTree(hInsertAfter);
		::SetWindowText( GetHwnd(), L"" );
	}
	else if( OUTLINE_TREE_TAGJUMP == m_nListType ){ /* 汎用ツリー(タグジャンプ付き) */
		m_nViewType = VIEWTYPE_TREE;
		SetTree( hInsertAfter, true );
		::SetWindowText( GetHwnd(), L"" );
	}
	else if( OUTLINE_CLSTREE == m_nListType ){ /* 汎用クラスツリー */
		m_nViewType = VIEWTYPE_TREE;
		SetTreeJava( GetHwnd(), hInsertAfter, TRUE );
		::SetWindowText( GetHwnd(), L"" );
	}
	else{
		m_nViewType = IsWorkbenchMode() ? VIEWTYPE_TREE : VIEWTYPE_LIST;
		switch( m_nListType ){
		case OUTLINE_C:
			::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_C) );
			break;
		case OUTLINE_PLSQL:
			::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_PLSQL) );
			break;
		case OUTLINE_ASM:
			::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_ASM) );
			break;
		case OUTLINE_PERL:	//	Sep. 8, 2000 genta
			::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_PERL) );
			break;
// Jul 10, 2003  little YOSHI  上に移動しました--->>
//		case OUTLINE_VB:	// 2001/06/23 N.Nakatani for Visual Basic
//			::SetWindowText( GetHwnd(), "Visual Basic アウトライン" );
//			break;
// <<---ここまで
		case OUTLINE_ERLANG:	//	2009.08.10 genta
			::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_ERLANG) );
			break;
		case OUTLINE_BOOKMARK:
			LV_COLUMN col;
			col.mask = LVCF_TEXT;
			col.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_LIST_TEXT));
			col.iSubItem = 0;
			//	Apr. 23, 2005 genta 行番号を左端へ
			ListView_SetColumn( hwndList, FL_COL_NAME, &col );
			::SetWindowText( GetHwnd(), LS(STR_DLGFNCLST_TITLE_BOOK) );
			break;
		case OUTLINE_LIST:	// 汎用リスト 2010.03.28 syat
			::SetWindowText( GetHwnd(), L"" );
			break;
		default:
			break;
		}
		if( IsWorkbenchMode() ){
			// The workbench Outline is a symbol hierarchy.  Legacy list-only
			// analyzers become a flat tree so they share the same compact row,
			// icon, keyboard and selection model as hierarchical analyzers.
			SetTree(hInsertAfter);
		}else{
		//	May 18, 2001 genta
		//	Windowがいなくなると後で都合が悪いので、表示しないだけにしておく
		//::DestroyWindow( hwndTree );
//		::ShowWindow( hwndTree, SW_HIDE );
		int				i;
		WCHAR			szText[2048];
		LV_ITEM			item;

		m_cmemClipText.SetString(L"");	/* クリップボードコピー用テキスト */
		{
			const int nBuffLenTag = int(13 + wcslen(m_pcFuncInfoArr->m_szFilePath));
			const int nNum = m_pcFuncInfoArr->GetNum();
			int nBuffLen = 0;
			for(int i2 = 0; i2 < nNum; ++i2 ){
				const auto pcFuncInfo = m_pcFuncInfoArr->GetAt(i2);
				nBuffLen += pcFuncInfo->m_cmemFuncName.GetStringLength();
			}
			m_cmemClipText.AllocStringBuffer( nBuffLen + nBuffLenTag * nNum );
		}

		::EnableWindow( GetItemHwnd( IDC_BUTTON_COPY ), TRUE );

		for( i = 0; i < m_pcFuncInfoArr->GetNum(); ++i ){
			/* 現在の解析結果要素 */
			const auto pcFuncInfo = m_pcFuncInfoArr->GetAt( i );

			//	From Here Apr. 23, 2005 genta 行番号を左端へ
			/* 行番号の表示 false=折り返し単位／true=改行単位 */
			if(m_bLineNumIsCRLF ){
				auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncLineCRLF );
			}else{
				auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncLineLAYOUT );
			}
			item.mask = LVIF_TEXT | LVIF_PARAM;
			item.pszText = szText;
			item.iItem = i;
			item.lParam	= i;
			item.iSubItem = FL_COL_ROW;
			ListView_InsertItem( hwndList, &item);

			// 2010.03.17 syat 桁追加
			/* 行番号の表示 false=折り返し単位／true=改行単位 */
			if(m_bLineNumIsCRLF ){
				auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncColCRLF );
			}else{
				auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncColLAYOUT );
			}
			item.mask = LVIF_TEXT;
			item.pszText = szText;
			item.iItem = i;
			item.iSubItem = FL_COL_COL;
			ListView_SetItem( hwndList, &item);

			item.mask = LVIF_TEXT;
			item.pszText = const_cast<WCHAR*>(pcFuncInfo->m_cmemFuncName.GetStringPtr());
			item.iItem = i;
			item.iSubItem = FL_COL_NAME;
			ListView_SetItem( hwndList, &item);
			//	To Here Apr. 23, 2005 genta 行番号を左端へ

			item.mask = LVIF_TEXT;
			if(  1 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK01));}else
			if( 10 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK02));}else
			if( 20 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK03));}else
			if( 11 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK04));}else
			if( 21 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK05));}else
			if( 31 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK06));}else
			if( 41 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK07));}else
			if( 50 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK08));}else
			if( 51 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK09));}else
			if( 52 == pcFuncInfo->m_nInfo ){item.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_REMARK10));}else{
				// Jul 10, 2003  little YOSHI
				// ここにあったVB関係の処理はSetListVB()メソッドに移動しました。

				item.pszText = const_cast<WCHAR*>(L"");
			}
			item.iItem = i;
			item.iSubItem = FL_COL_REMARK;
			ListView_SetItem( hwndList, &item);

			/* クリップボードにコピーするテキストを編集 */
			if(item.pszText[0] != L'\0'){
				// 検出結果の種類(関数,,,)があるとき
				auto_sprintf(
					szText,
					L"%s(%d,%d): ",
					m_pcFuncInfoArr->m_szFilePath.c_str(),		/* 解析対象ファイル名 */
					pcFuncInfo->m_nFuncLineCRLF,		/* 検出行番号 */
					pcFuncInfo->m_nFuncColCRLF		/* 検出桁番号 */
				);
				m_cmemClipText.AppendString(szText);
				// "%s(%s)\r\n"
				m_cmemClipText.AppendNativeData(pcFuncInfo->m_cmemFuncName);
				m_cmemClipText.AppendString(L"(");
				m_cmemClipText.AppendString(item.pszText);
				m_cmemClipText.AppendString(L")\r\n");
			}else{
				// 検出結果の種類(関数,,,)がないとき
				auto_sprintf(
					szText,
					L"%s(%d,%d): ",
					m_pcFuncInfoArr->m_szFilePath.c_str(),		/* 解析対象ファイル名 */
					pcFuncInfo->m_nFuncLineCRLF,		/* 検出行番号 */
					pcFuncInfo->m_nFuncColCRLF		/* 検出桁番号 */
				);
				m_cmemClipText.AppendString(szText);
				m_cmemClipText.AppendNativeData(pcFuncInfo->m_cmemFuncName);
				m_cmemClipText.AppendString(L"\r\n");
			}
		}
		//2002.02.08 hor Listは列幅調整とかを実行する前に表示しとかないと変になる
		::ShowWindow( hwndList, SW_SHOW );
		/* 列の幅をデータに合わせて調整 */
		ListView_SetColumnWidth( hwndList, FL_COL_ROW, LVSCW_AUTOSIZE );
		ListView_SetColumnWidth( hwndList, FL_COL_COL, LVSCW_AUTOSIZE );
		ListView_SetColumnWidth( hwndList, FL_COL_NAME, LVSCW_AUTOSIZE );
		ListView_SetColumnWidth( hwndList, FL_COL_REMARK, LVSCW_AUTOSIZE );
		ListView_SetColumnWidth( hwndList, FL_COL_ROW, ListView_GetColumnWidth( hwndList, FL_COL_ROW ) + 16 );
		ListView_SetColumnWidth( hwndList, FL_COL_COL, ListView_GetColumnWidth( hwndList, FL_COL_COL ) + 16 );
		ListView_SetColumnWidth( hwndList, FL_COL_NAME, ListView_GetColumnWidth( hwndList, FL_COL_NAME ) + 16 );
		ListView_SetColumnWidth( hwndList, FL_COL_REMARK, ListView_GetColumnWidth( hwndList, FL_COL_REMARK ) + 16 );

		// 2005.07.05 ぜっと
		DWORD dwExStyle  = ListView_GetExtendedListViewStyle( hwndList );
		dwExStyle |= LVS_EX_FULLROWSELECT;
		ListView_SetExtendedListViewStyle( hwndList, dwExStyle );
		}
	}
	if( IsWorkbenchMode() ) {
		m_workbenchLastTimings.nativeTreeBuildUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - nativeTreeBegin).count());
	}

	/* アウトライン ダイアログを自動的に閉じる */
	::CheckDlgButton( GetHwnd(), IDC_CHECK_bAutoCloseDlgFuncList, m_pShareData->m_Common.m_sOutline.m_bAutoCloseDlgFuncList );
	/* アウトライン ブックマーク一覧で空行を無視する */
	::CheckDlgButton( GetHwnd(), IDC_CHECK_bMarkUpBlankLineEnable, m_pShareData->m_Common.m_sOutline.m_bMarkUpBlankLineEnable );
	/* アウトライン ジャンプしたらフォーカスを移す */
	::CheckDlgButton( GetHwnd(), IDC_CHECK_bFunclistSetFocusOnJump, m_pShareData->m_Common.m_sOutline.m_bFunclistSetFocusOnJump );

	/* アウトライン ■位置とサイズを記憶する */ // 20060201 aroka
	::CheckDlgButton( GetHwnd(), IDC_BUTTON_WINSIZE, m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos );
	// ボタンが押されているかはっきりさせる 2008/6/5 Uchi
	ApiWrap::DlgItem_SetText( GetHwnd(), IDC_BUTTON_WINSIZE,
		m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos ? L"■" : L"□" );

	/* ダイアログを自動的に閉じるならフォーカス移動オプションは関係ない */
	if(m_pShareData->m_Common.m_sOutline.m_bAutoCloseDlgFuncList){
		::EnableWindow( GetItemHwnd( IDC_CHECK_bFunclistSetFocusOnJump ), FALSE );
	}else{
		::EnableWindow( GetItemHwnd( IDC_CHECK_bFunclistSetFocusOnJump ), TRUE );
	}

	//2002.02.08 hor
	//空行をどう扱うかのチェックボックスはブックマーク一覧のときだけ表示する
	if(OUTLINE_BOOKMARK == m_nListType){
		::EnableWindow( GetItemHwnd( IDC_CHECK_bMarkUpBlankLineEnable ), TRUE );
		if( !UsesCompactPanelLayout() ) ::ShowWindow( GetItemHwnd( IDC_CHECK_bMarkUpBlankLineEnable ), SW_SHOW );
	}else{
		::ShowWindow( GetItemHwnd( IDC_CHECK_bMarkUpBlankLineEnable ), SW_HIDE );
		::EnableWindow( GetItemHwnd( IDC_CHECK_bMarkUpBlankLineEnable ), FALSE );
	}
	// 2002/11/1 frozen 項目のソート基準を設定するコンボボックスはブックマーク一覧の以外の時に表示する
	// Nov. 5, 2002 genta ツリー表示の時だけソート基準コンボボックスを表示
	CEditView* pcEditView = (CEditView*)m_lParam;
	int nDocType = pcEditView->GetDocument()->m_cDocType.GetDocumentType().GetIndex();
	if( nDocType != m_nDocType ){
		// 以前とはドキュメントタイプが変わったので初期化する
		m_nDocType = nDocType;
		m_nSortCol = m_type.m_nOutlineSortCol;
		m_nSortColOld = m_nSortCol;
		m_bSortDesc = m_type.m_bOutlineSortDesc;
		m_nSortType = m_type.m_nOutlineSortType;
	}
	if( IsWorkbenchMode() ){
		::EnableWindow( GetItemHwnd(IDC_COMBO_nSortType), FALSE );
		::ShowWindow( GetItemHwnd(IDC_COMBO_nSortType), SW_HIDE );
		::ShowWindow( GetItemHwnd(IDC_STATIC_nSortType), SW_HIDE );
		if( m_nViewType == VIEWTYPE_TREE
			&& m_nSortType != SORTTYPE_DEFAULT
			&& m_nSortType != SORTTYPE_DEFAULT_DESC ){
			SortTree(hwndTree, TVI_ROOT);
		}
	}else if( m_nViewType == VIEWTYPE_TREE && m_nListType != OUTLINE_FILETREE ){
		HWND hWnd_Combo_Sort = GetItemHwnd( IDC_COMBO_nSortType );
		if( m_nListType == OUTLINE_FILETREE ){
			::EnableWindow( hWnd_Combo_Sort , FALSE );
		}else{
			::EnableWindow( hWnd_Combo_Sort , TRUE );
		}
		::ShowWindow( hWnd_Combo_Sort , SW_SHOW );
		ApiWrap::Combo_ResetContent( hWnd_Combo_Sort ); // 2002.11.10 Moca 追加
		ApiWrap::Combo_AddString( hWnd_Combo_Sort , LS(STR_DLGFNCLST_SORTTYPE1));	// SORTTYPE_DEFAULT
		ApiWrap::Combo_AddString( hWnd_Combo_Sort , LS(STR_DLGFNCLST_SORTTYPE1_2));	// SORTTYPE_DEFAULT_DESC
		ApiWrap::Combo_AddString( hWnd_Combo_Sort , LS(STR_DLGFNCLST_SORTTYPE2));    // SORTTYPE_ATOZ
		ApiWrap::Combo_AddString( hWnd_Combo_Sort , LS(STR_DLGFNCLST_SORTTYPE2_2));  // SORTTYPE_ZTOA
		ApiWrap::Combo_SetCurSel( hWnd_Combo_Sort , m_nSortType );
		::ShowWindow( GetItemHwnd( IDC_STATIC_nSortType ), SW_SHOW );
		if (m_nSortType != SORTTYPE_DEFAULT && m_nSortType != SORTTYPE_DEFAULT_DESC)
			SortTree(hwndTree, TVI_ROOT);
	}else if( m_nListType == OUTLINE_FILETREE ){
		::ShowWindow( GetItemHwnd(IDC_COMBO_nSortType), SW_HIDE );
		::ShowWindow( GetItemHwnd(IDC_STATIC_nSortType), SW_HIDE );
		::ShowWindow( GetItemHwnd(IDC_BUTTON_SETTING), SW_SHOW );
	}else {
		::EnableWindow( GetItemHwnd( IDC_COMBO_nSortType ), FALSE );
		::ShowWindow( GetItemHwnd( IDC_COMBO_nSortType ), SW_HIDE );
		::ShowWindow( GetItemHwnd( IDC_STATIC_nSortType ), SW_HIDE );
		//ListView_SortItems( hwndList, CompareFunc_Asc, (LPARAM)this );  // 2005.04.05 zenryaku ソート状態を保持
		SortListView( hwndList, m_nSortCol );	// 2005.04.23 genta 関数化(ヘッダー書き換えのため)
	}

	//2002.02.08 hor
	//（IDC_LIST_FLもIDC_TREE_FLも常に存在していて、m_nViewTypeによって、どちらを表示するかを選んでいる）
	HWND hwndShow = (VIEWTYPE_LIST == m_nViewType) ? hwndList : hwndTree;
	::ShowWindow(hwndTree, SW_HIDE);
	::ShowWindow(hwndList, SW_HIDE);
	::ShowWindow(hwndShow, SW_SHOW);
	::SendMessage(hwndList, WM_SETREDRAW, (WPARAM)TRUE, 0);
	::SendMessage(hwndTree, WM_SETREDRAW, (WPARAM)TRUE, 0);
	// Workbench keeps the established default-expanded Outline behavior.  This is
	// the one expansion pass for a commit; SetTreeJava does not also expand root
	// siblings in workbench mode.
	if( IsWorkbenchMode() ){
		const auto expansionBegin = std::chrono::steady_clock::now();
		std::vector<HTREEITEM> pending;
		if( const HTREEITEM root = TreeView_GetRoot(hwndTree); root != nullptr ) pending.push_back(root);
		while( !pending.empty() ){
			const HTREEITEM item = pending.back();
			pending.pop_back();
			(void)TreeView_Expand( hwndTree, item, TVE_EXPAND );
			if( const HTREEITEM sibling = TreeView_GetNextSibling(hwndTree, item); sibling != nullptr ) pending.push_back(sibling);
			if( const HTREEITEM child = TreeView_GetChild(hwndTree, item); child != nullptr ) pending.push_back(child);
		}
		m_workbenchLastTimings.expansionUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - expansionBegin).count());
		if( m_workbenchAppearanceDirty || m_workbenchTreeContentDirty ) ApplyWorkbenchAppearance();
	}
	// 選択状態更新
	int nFuncInfoIndex = -1;
	if ( !IsWorkbenchMode() && GetFuncInfoIndex(m_nCurLine, m_nCurCol, &nFuncInfoIndex) ) {
		SetItemSelection(nFuncInfoIndex, true);
	}
	if (::GetForegroundWindow() == MyGetAncestor(GetHwnd(), GA_ROOT) && IsChild(GetHwnd(), GetFocus()))
		::SetFocus(hwndShow);
}

bool CDlgFuncList::GetTreeFileFullName(HWND hwndTree, HTREEITEM target, std::wstring* pPath, int* pnItem)
{
	(*pPath).clear();
	*pnItem = -1;
	do{
		TVITEM tvItem;
		WCHAR szFileName[_MAX_PATH];
		tvItem.mask = TVIF_HANDLE | TVIF_TEXT;
		tvItem.pszText = szFileName;
		tvItem.cchTextMax = int(std::size(szFileName));
		tvItem.hItem = target;
		TreeView_GetItem( hwndTree, &tvItem );
		if( ((-tvItem.lParam) % 10) == 3 ){
			*pnItem = int((-tvItem.lParam) / 10);
			std::wstring path = m_pcFuncInfoArr->GetAt(*pnItem)->m_cmemFileName.GetStringPtr();
			path += L"\\";
			path += *pPath;
			*pPath = std::move(path);
			return true;
		}
		if( tvItem.lParam != -1 && tvItem.lParam != -2 ){
			return false;
		}
		if( *pPath != L"" ){
			std::wstring path = szFileName;
			path += L"\\";
			path += *pPath;
			*pPath = std::move(path);
		}else{
			*pPath = szFileName;
		}
		target = TreeView_GetParent( hwndTree, target );
	}while( target != nullptr );
	return false;
}

/*! lParamからFuncInfoの番号を算出
	vecにはダミーのlParam番号が入っているのでずれている数を数える
*/
static int TreeDummylParamToFuncInfoIndex(const std::vector<int>& vec, LPARAM lParam)
{
	// vec = { 3,6,7 }; count dummies below lParam with one lower_bound.
	// lParam 0,1,2,3,4,5,6,7,8 -> return 0,1,2,-1,3,4,-1,-1,5.
	if( lParam < 0 ) return -1;
	const int item = static_cast<int>(lParam);
	const auto it = std::lower_bound(vec.cbegin(), vec.cend(), item);
	if( it != vec.cend() && *it == item ) return -1;
	return item - static_cast<int>(std::distance(vec.cbegin(), it));
}

/* ダイアログデータの取得 */
/* 0==条件未入力   0より大きい==正常   0より小さい==入力エラー */
int CDlgFuncList::GetData( void )
{
	if( IsWorkbenchMode() && !HasCurrentWorkbenchModel() ) {
		m_cFuncInfo = nullptr;
		m_sJumpFile.clear();
		return -1;
	}
	HWND			hwndList;
	HWND			hwndTree;
	int				nItem;
	LV_ITEM			item;
	HTREEITEM		htiItem;
	TV_ITEM			tvi;

	m_cFuncInfo = nullptr;
	m_sJumpFile.clear();
	hwndList = GetItemHwnd( IDC_LIST_FL );
	if( m_nViewType == VIEWTYPE_LIST ){
		//	List
		nItem = ListView_GetNextItem( hwndList, -1, LVNI_ALL | LVNI_SELECTED );
		if( -1 == nItem ){
			return -1;
		}
		item.mask = LVIF_PARAM;
		item.iItem = nItem;
		item.iSubItem = 0;
		ListView_GetItem( hwndList, &item );
		m_cFuncInfo = m_pcFuncInfoArr->GetAt( item.lParam );
	}else{
		hwndTree = GetItemHwnd( IDC_TREE_FL );
		if( nullptr != hwndTree ){
			htiItem = TreeView_GetSelection( hwndTree );

			tvi.mask = TVIF_HANDLE | TVIF_PARAM;
			tvi.hItem = htiItem;
			tvi.pszText = nullptr;
			tvi.cchTextMax = 0;
			if( TreeView_GetItem( hwndTree, &tvi ) ){
				// lParamが-1以下は pcFuncInfoArrには含まれない項目
				if( 0 <= tvi.lParam ){
					int nIndex;
					if( m_bDummyLParamMode ){
						// ダミー要素を排除:SetTreeJava
						nIndex = TreeDummylParamToFuncInfoIndex(m_vecDummylParams, tvi.lParam);
					}else{
						nIndex = (int)tvi.lParam;
					}
					if( 0 <= nIndex ){
						m_cFuncInfo = m_pcFuncInfoArr->GetAt(nIndex);
					}
				}else{
					if( m_nListType == OUTLINE_FILETREE ){
						if( tvi.lParam == -1 ){
							int nItem2;
							if( !GetTreeFileFullName( hwndTree, htiItem, &m_sJumpFile, &nItem2 ) ){
								m_sJumpFile.clear(); // error
							}
						}
					}
				}
			}
		}
	}
	return 1;
}

/* Java/C++メソッドツリーの最大ネスト深さ */
// 2016.03.06 vector化で16 -> 32 まで増やしておく
#define MAX_JAVA_TREE_NEST 32

/*! ツリーコントロールの初期化：Javaメソッドツリー

	Java Method Treeの構築: 関数リストを元にTreeControlを初期化する。

	@date 2002.01.04 genta C++ツリーを統合
	@date 2020.09.12 選択処理をGetFuncInfoIndex,SetItemSelectionへ移動
*/
void CDlgFuncList::SetTreeJava( [[maybe_unused]] HWND hwndDlg, HTREEITEM hInsertAfter, BOOL bAddClass )
{
	int				i;
	HWND			hwndTree;
	int				bSelected;
	CLayoutInt		nFuncLineOld;
	CLayoutInt		nFuncColOld;
	CLayoutInt		nFuncLineTop(INT_MAX);
	CLayoutInt		nFuncColTop(INT_MAX);
	TV_INSERTSTRUCT	tvis;
	const WCHAR*	pPos;
	HTREEITEM		htiGlobal = nullptr;	// Jan. 04, 2001 genta C++と統合
	HTREEITEM		htiClass = nullptr;
	HTREEITEM		htiItem;
	TV_ITEM			tvi;
	int				nClassNest;
	std::vector<std::wstring> vStrClasses;
	std::map<std::wstring, HTREEITEM> workbenchClassItems;
	const bool workbenchMode = IsWorkbenchMode();
	if( workbenchMode ) {
		m_workbenchClipboardUsesGenericTree = false;
		m_workbenchClipboardTagJump = false;
		m_workbenchClipboardNoLabel = false;
	}
	if( workbenchMode ) m_workbenchTreeItems.assign(
		static_cast<std::size_t>((std::max)(0, m_pcFuncInfoArr->GetNum())), nullptr);
	if( workbenchMode ) m_workbenchTreeLabels.clear();

	::EnableWindow( GetItemHwnd( IDC_BUTTON_COPY ), TRUE );
	m_bDummyLParamMode = true;
	m_vecDummylParams.clear();
	int nlParamCount = 0;

	hwndTree = GetItemHwnd( IDC_TREE_FL );

	if( !workbenchMode ) {
		m_cmemClipText.SetString( L"" );
		const int nBuffLenTag = int(13 + wcslen(m_pcFuncInfoArr->m_szFilePath));
		const int nNum = m_pcFuncInfoArr->GetNum();
		int nBuffLen = 0;
		for( int i2 = 0; i2 < nNum; i2++ ){
			const auto pcFuncInfo = m_pcFuncInfoArr->GetAt(i2);
			nBuffLen += pcFuncInfo->m_cmemFuncName.GetStringLength();
		}
		m_cmemClipText.AllocStringBuffer( nBuffLen + nBuffLenTag * nNum );
	}
	// 追加文字列の初期化（プラグインで指定済みの場合は上書きしない）
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_DECLARE,		LS(STR_DLGFNCLST_APND_DECLARE),	false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_CLASS,		LS(STR_DLGFNCLST_APND_CLASS),		false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_STRUCT,		LS(STR_DLGFNCLST_APND_STRUCT),		false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_ENUM,		LS(STR_DLGFNCLST_APND_ENUM),		false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_UNION,		LS(STR_DLGFNCLST_APND_UNION),		false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_NAMESPACE,	LS(STR_DLGFNCLST_APND_NAMESPACE),	false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_INTERFACE,	LS(STR_DLGFNCLST_APND_INTERFACE),	false );
	m_pcFuncInfoArr->SetAppendText( FL_OBJ_GLOBAL,		LS(STR_DLGFNCLST_APND_GLOBAL),		false );

	nFuncLineOld = CLayoutInt(-1);
	nFuncColOld = CLayoutInt(-1);
	bSelected = FALSE;
	for( i = 0; i < m_pcFuncInfoArr->GetNum(); ++i ){
		const auto pcFuncInfo = m_pcFuncInfoArr->GetAt( i );
		const WCHAR* pWork = pcFuncInfo->m_cmemFuncName.GetStringPtr();
		int m = 0;
		vStrClasses.clear();
		nClassNest = 0;
		/* クラス名::メソッドの場合 */
		if( nullptr != ( pPos = wcsstr( pWork, L"::" ) )
			&& wcsncmp_literal(pWork, L"operator ") != 0 ){
			/* インナークラスのネストレベルを調べる */
			int	k;
			int	nWorkLen;
			int	nCharChars;
			int	nNestTemplate = 0;
			nWorkLen = (int)wcslen( pWork );
			for( k = 0; k < nWorkLen; ++k ){
				//2009.9.21 syat ネストが深すぎる際のBOF対策
				if( nClassNest == MAX_JAVA_TREE_NEST ){
					k = nWorkLen;
					break;
				}
				nCharChars = CNativeW::GetSizeOfChar( pWork, nWorkLen, k );
				if( 1 == nCharChars && 0 == nNestTemplate && L':' == pWork[k] ){
					//	Jan. 04, 2001 genta
					//	C++の統合のため、\に加えて::をクラス区切りとみなすように
					if( k < nWorkLen - 1 && L':' == pWork[k+1] ){
						std::wstring strClass(&pWork[m], k - m);
						vStrClasses.push_back(strClass);
						++nClassNest;
						m = k + 2;
						++k;
						// Klass::operator std::string
						if( wcsncmp_literal(pWork + m, L"operator ") == 0 ){
							break;
						}
					}
					else 
						break;
				}
				else if( 1 == nCharChars && L'\\' == pWork[k] ){
					std::wstring strClass(&pWork[m], k - m);
					vStrClasses.push_back(strClass);
					++nClassNest;
					m = k + 1;
				}
				else if( 1 == nCharChars && L'<' == pWork[k] ){
					// namesp::function<std::string> のようなものを処理する
					nNestTemplate++;
				}
				else if( 1 == nCharChars && L'>' == pWork[k] ){
					if( 0 < nNestTemplate ){
						nNestTemplate--;
					}
				}
				if( 2 == nCharChars ){
					++k;
				}
			}
		}
		if( 0 < nClassNest ){
			int	k;
			//	Jan. 04, 2001 genta
			//	関数先頭のセット(ツリー構築で使う)
			pWork = pWork + m; // 2 == lstrlen( "::" );

			/* クラス名のアイテムが登録されているか */
			HTREEITEM htiParent = TVI_ROOT;
			std::wstring classPathKey;
			if( !workbenchMode ) htiClass = TreeView_GetFirstVisible( hwndTree );
			for( k = 0; k < nClassNest; ++k ){
				//	Apr. 1, 2001 genta
				//	追加文字列を全角にしたのでメモリもそれだけ必要
				//	6 == strlen( "クラス" ), 1 == strlen( L'\0' )

				// 2002/10/30 frozen
				// bAddClass == true の場合の仕様変更
				// 既存の項目は　「(クラス名)(半角スペース一個)(追加文字列)」
				// となっているとみなし、szClassArr[k] が 「クラス名」と一致すれば、それを親ノードに設定。
				// ただし、一致する項目が複数ある場合は最初の項目を親ノードにする。
				// 一致しない場合は「(クラス名)(半角スペース一個)クラス」のノードを作成する。
				classPathKey.append(vStrClasses[k]);
				classPathKey.push_back(L'\0');
				if( workbenchMode ) {
					const auto found = workbenchClassItems.find(classPathKey);
					htiClass = found != workbenchClassItems.end() ? found->second : nullptr;
				}else{
					size_t nClassNameLen = vStrClasses[k].size();
					for( ; nullptr != htiClass ; htiClass = TreeView_GetNextSibling( hwndTree, htiClass ))
					{
						tvi.mask = TVIF_HANDLE | TVIF_TEXT;
						tvi.hItem = htiClass;

						std::vector<WCHAR> vecStr;
						if( ApiWrap::TreeView_GetItemTextVector(hwndTree, tvi, vecStr) ){
							const WCHAR* pszLabel = &vecStr[0];
							if( 0 == wcsncmp(vStrClasses[k].c_str(), pszLabel, nClassNameLen) ){
								if( bAddClass ){
									if( pszLabel[nClassNameLen]==L' ' ){
										break;
									}
								}else{
									if( pszLabel[nClassNameLen]==L'\0' ){
										break;
									}
								}
							}
						}
					}
				}

				/* クラス名のアイテムが登録されていないので登録 */
				if( nullptr == htiClass ){
					// 2002/10/28 frozen 上からここへ移動
					std::wstring strClassName = vStrClasses[k];
					
					if( bAddClass )
					{
						if( pcFuncInfo->m_nInfo == FL_OBJ_NAMESPACE )
						{
							//wcscat( pClassName, L" 名前空間" );
							strClassName += m_pcFuncInfoArr->GetAppendText(FL_OBJ_NAMESPACE);
						}
						else
							//wcscat( pClassName, L" クラス" );
							strClassName += m_pcFuncInfoArr->GetAppendText(FL_OBJ_CLASS);
					}
					tvis.hParent = htiParent;
					tvis.hInsertAfter = hInsertAfter;
					tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
					tvis.item.pszText = const_cast<WCHAR*>(strClassName.c_str());
					// 2016.03.06 item.lParamは登録順の連番に変更
					tvis.item.lParam = nlParamCount;
					m_vecDummylParams.push_back(nlParamCount);
					nlParamCount++;

				htiClass = TreeView_InsertItem( hwndTree, &tvis );
				if( workbenchMode ) {
					workbenchClassItems.emplace(classPathKey, htiClass);
					m_workbenchTreeLabels[htiClass] = { strClassName, k };
				}
				}else{
					//none
				}
				htiParent = htiClass;
				//if( k + 1 >= nClassNest ){
				//	break;
				//}
				if( !workbenchMode ) htiClass = TreeView_GetChild( hwndTree, htiClass );
			}
			htiClass = htiParent;
		}else{
			//	Jan. 04, 2001 genta
			//	Global空間の場合 (C++のみ)

			// 2002/10/27 frozen ここから
			// 2007.05.26 genta "__interface" をクラスに類する扱いにする
			// 2011.09.25 syat プラグインで追加された要素をクラスに類する扱いにする
			if( FL_OBJ_CLASS <= pcFuncInfo->m_nInfo  && pcFuncInfo->m_nInfo <= FL_OBJ_ELEMENT_MAX )
				htiClass = TVI_ROOT;
			else
			{
			// 2002/10/27 frozen ここまで
				if( htiGlobal == nullptr ){
					TV_INSERTSTRUCT	tvg;
					const auto& sGlobal = m_pcFuncInfoArr->GetAppendText( FL_OBJ_GLOBAL );

					::ZeroMemory( &tvg, sizeof(tvg));
					tvg.hParent = TVI_ROOT;
					tvg.hInsertAfter = hInsertAfter;
					tvg.item.mask = TVIF_TEXT | TVIF_PARAM;
					//tvg.item.pszText = const_cast<WCHAR*>(L"グローバル");
					tvg.item.pszText = const_cast<WCHAR*>(sGlobal.c_str());
					tvg.item.lParam = nlParamCount;
					m_vecDummylParams.push_back(nlParamCount);
					nlParamCount++;
					htiGlobal = TreeView_InsertItem( hwndTree, &tvg );
					if( workbenchMode ) m_workbenchTreeLabels[htiGlobal] = { sGlobal, 0 };
				}
				htiClass = htiGlobal;
			}
		}
		std::wstring strFuncName = pWork;

		// 2002/10/27 frozen 追加文字列の種類を増やした
		switch(pcFuncInfo->m_nInfo)
		{
		case FL_OBJ_DEFINITION:		//「定義位置」に追加文字列は不要なため除外
		case FL_OBJ_NAMESPACE:		//「名前空間」は別の場所で処理してるので除外
		case FL_OBJ_GLOBAL:			//「グローバル」は別の場所で処理してるので除外
			break;
		default:
			strFuncName += m_pcFuncInfoArr->GetAppendText(pcFuncInfo->m_nInfo);
		}

/* 該当クラス名のアイテムの子として、メソッドのアイテムを登録 */
		tvis.hParent = htiClass;
		tvis.hInsertAfter = hInsertAfter;
		tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText = const_cast<WCHAR*>(strFuncName.c_str());
		tvis.item.lParam = nlParamCount;
		nlParamCount++;
		htiItem = TreeView_InsertItem( hwndTree, &tvis );
		if( workbenchMode && static_cast<std::size_t>(i) < m_workbenchTreeItems.size() ) {
			m_workbenchTreeItems[static_cast<std::size_t>(i)] = htiItem;
		}
		if( workbenchMode ) m_workbenchTreeLabels[htiItem] = { strFuncName, nClassNest };

		/* クリップボードにコピーするテキストを編集 */
		if( !workbenchMode ) {
			WCHAR szText[2048];
			auto_sprintf(
				szText,
				L"%s(%d,%d): ",
				m_pcFuncInfoArr->m_szFilePath.c_str(),		/* 解析対象ファイル名 */
				pcFuncInfo->m_nFuncLineCRLF,		/* 検出行番号 */
				pcFuncInfo->m_nFuncColCRLF		/* 検出桁番号 */
			);
			m_cmemClipText.AppendString( szText ); /* クリップボードコピー用テキスト */
			// "%s%ls\r\n"
			m_cmemClipText.AppendNativeData(pcFuncInfo->m_cmemFuncName);
			m_cmemClipText.AppendString(FL_OBJ_DECLARE == pcFuncInfo->m_nInfo ? m_pcFuncInfoArr->GetAppendText( FL_OBJ_DECLARE ).c_str() : L"" ); 	//	Jan. 04, 2001 genta C++で使用
			m_cmemClipText.AppendString(L"\r\n");
		}

		//	Jan. 04, 2001 genta
		//	deleteはその都度行うのでここでは不要
	}
	/* ソート、ノードの展開をする */
//	TreeView_SortChildren( hwndTree, TVI_ROOT, 0 );
	if( !workbenchMode ) {
		const auto expansionBegin = std::chrono::steady_clock::now();
		htiClass = TreeView_GetFirstVisible( hwndTree );
		while( nullptr != htiClass ){
		//		TreeView_SortChildren( hwndTree, htiClass, 0 );
			TreeView_Expand( hwndTree, htiClass, TVE_EXPAND );
			htiClass = TreeView_GetNextSibling( hwndTree, htiClass );
		}
	}

//	GetTreeTextNext( hwndTree, NULL, 0 );
	m_nTreeItemCount = nlParamCount;
	return;
}

/*! リストビューコントロールの初期化：VisualBasic

  長くなったので独立させました。

  @date Jul 10, 2003  little YOSHI
  @date 2020.09.12 選択処理をGetFuncInfoIndex,SetItemSelectionへ移動
*/
void CDlgFuncList::SetListVB (void)
{
	int				i;
	WCHAR			szType[64];
	WCHAR			szOption[64];
	LV_ITEM			item;
	HWND			hwndList;

	::EnableWindow( GetItemHwnd( IDC_BUTTON_COPY ), TRUE );

	hwndList = GetItemHwnd( IDC_LIST_FL );

	m_cmemClipText.SetString( L"" );
	{
		const int nBuffLenTag = int(17 + wcslen(m_pcFuncInfoArr->m_szFilePath));
		const int nNum = m_pcFuncInfoArr->GetNum();
		int nBuffLen = 0;
		for( int i2 = 0; i2 < nNum; i2++ ){
			const auto pcFuncInfo = m_pcFuncInfoArr->GetAt(i2);
			nBuffLen += pcFuncInfo->m_cmemFuncName.GetStringLength();
		}
		m_cmemClipText.AllocStringBuffer( nBuffLen + nBuffLenTag * nNum );
	}

	WCHAR			szText[2048];
	for( i = 0; i < m_pcFuncInfoArr->GetNum(); ++i ){
		/* 現在の解析結果要素 */
		const auto pcFuncInfo = m_pcFuncInfoArr->GetAt( i );

		//	From Here Apr. 23, 2005 genta 行番号を左端へ
		/* 行番号の表示 false=折り返し単位／true=改行単位 */
		if(m_bLineNumIsCRLF ){
			auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncLineCRLF );
		}else{
			auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncLineLAYOUT );
		}
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.pszText = szText;
		item.iItem = i;
		item.iSubItem = FL_COL_ROW;
		item.lParam	= i;
		ListView_InsertItem( hwndList, &item);

		// 2010.03.17 syat 桁追加
		/* 行番号の表示 false=折り返し単位／true=改行単位 */
		if(m_bLineNumIsCRLF ){
			auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncColCRLF );
		}else{
			auto_sprintf( szText, L"%d", pcFuncInfo->m_nFuncColLAYOUT );
		}
		item.mask = LVIF_TEXT;
		item.pszText = szText;
		item.iItem = i;
		item.iSubItem = FL_COL_COL;
		ListView_SetItem( hwndList, &item);

		item.mask = LVIF_TEXT;
		item.pszText = const_cast<WCHAR*>(pcFuncInfo->m_cmemFuncName.GetStringPtr());
		item.iItem = i;
		item.iSubItem = FL_COL_NAME;
		ListView_SetItem( hwndList, &item);
		//	To Here Apr. 23, 2005 genta 行番号を左端へ

		item.mask = LVIF_TEXT;

		// 2001/06/23 N.Nakatani for Visual Basic
		//	Jun. 26, 2001 genta 半角かな→全角に
		wmemset(szText, L'\0', int(std::size(szText)));
		wmemset(szType, L'\0', int(std::size(szType)));
		wmemset(szOption, L'\0', int(std::size(szOption)));
		if( 1 == ((pcFuncInfo->m_nInfo >> 8) & 0x01) ){
			// スタティック宣言(Static)
			// 2006.12.12 Moca 末尾にスペース追加
			wcscpy(szOption, LS(STR_DLGFNCLST_VB_STATIC));
		}
		switch ((pcFuncInfo->m_nInfo >> 4) & 0x0f) {
			case 2  :	// プライベート(Private)
				wcsncat(szOption, LS(STR_DLGFNCLST_VB_PRIVATE), int(std::size(szOption)) - wcslen(szOption)); //	2006.12.17 genta サイズ誤り修正
				break;

			case 3  :	// フレンド(Friend)
				wcsncat(szOption, LS(STR_DLGFNCLST_VB_FRIEND), int(std::size(szOption)) - wcslen(szOption)); //	2006.12.17 genta サイズ誤り修正
				break;

			default :	// パブリック(Public)
				wcsncat(szOption, LS(STR_DLGFNCLST_VB_PUBLIC), int(std::size(szOption)) - wcslen(szOption)); //	2006.12.17 genta サイズ誤り修正
		}
		int nInfo = pcFuncInfo->m_nInfo;
		switch (nInfo & 0x0f) {
			case 1:		// 関数(Function)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_FUNCTION));
				break;

			// 2006.12.12 Moca ステータス→プロシージャに変更
			case 2:		// プロシージャ(Sub)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_PROC));
				break;

			case 3:		// プロパティ 取得(Property Get)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_PROPGET));
				break;

			case 4:		// プロパティ 設定(Property Let)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_PROPLET));
				break;

			case 5:		// プロパティ 参照(Property Set)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_PROPSET));
				break;

			case 6:		// 定数(Const)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_CONST));
				break;

			case 7:		// 列挙型(Enum)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_ENUM));
				break;

			case 8:		// ユーザ定義型(Type)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_TYPE));
				break;

			case 9:		// イベント(Event)
				wcscpy(szType, LS(STR_DLGFNCLST_VB_EVENT));
				break;

			default:	// 未定義なのでクリア
				nInfo	= 0;
		}
		if ( 2 == ((nInfo >> 8) & 0x02) ) {
			// 宣言(Declareなど)
			wcsncat(szType, LS(STR_DLGFNCLST_VB_DECL), int(std::size(szType)) - wcslen(szType));
		}

		WCHAR szTypeOption[256]; // 2006.12.12 Moca auto_sprintfの入出力で同一変数を使わないための作業領域追加
		if ( 0 == nInfo ) {
			szTypeOption[0] = L'\0';	//	2006.12.17 genta 全体を0で埋める必要はない
		} else
		if ( szOption[0] == L'\0' ) {
			auto_sprintf(szTypeOption, L"%s", szType);
		} else {
			auto_sprintf(szTypeOption, L"%s（%s）", szType, szOption);
		}
		item.pszText = szTypeOption;
		item.iItem = i;
		item.iSubItem = FL_COL_REMARK;
		ListView_SetItem( hwndList, &item);

		/* クリップボードにコピーするテキストを編集 */
		if(item.pszText[0] != L'\0'){
			// 検出結果の種類(関数,,,)があるとき
			// 2006.12.12 Moca szText を自分自身にコピーしていたバグを修正
			auto_sprintf(
				szText,
				L"%s(%d,%d): ",
				m_pcFuncInfoArr->m_szFilePath.c_str(),		/* 解析対象ファイル名 */
				pcFuncInfo->m_nFuncLineCRLF,		/* 検出行番号 */
				pcFuncInfo->m_nFuncColCRLF		/* 検出桁番号 */
			);
			m_cmemClipText.AppendString(szText);
			// "%s(%s)\r\n"
			m_cmemClipText.AppendNativeData(pcFuncInfo->m_cmemFuncName);
			m_cmemClipText.AppendString(L"(");
			m_cmemClipText.AppendString(item.pszText);
			m_cmemClipText.AppendString(L")\r\n");
		}else{
			// 検出結果の種類(関数,,,)がないとき
			auto_sprintf(
				szText,
				L"%s(%d,%d): ",
				m_pcFuncInfoArr->m_szFilePath.c_str(),		/* 解析対象ファイル名 */
				pcFuncInfo->m_nFuncLineCRLF,		/* 検出行番号 */
				pcFuncInfo->m_nFuncColCRLF		/* 検出桁番号 */
			);
			m_cmemClipText.AppendString(szText);
			// "%s\r\n"
			m_cmemClipText.AppendNativeData(pcFuncInfo->m_cmemFuncName);
			m_cmemClipText.AppendString(L"\r\n");
		}
	}

	//2002.02.08 hor Listは列幅調整とかを実行する前に表示しとかないと変になる
	::ShowWindow( hwndList, SW_SHOW );
	/* 列の幅をデータに合わせて調整 */
	ListView_SetColumnWidth( hwndList, FL_COL_ROW, LVSCW_AUTOSIZE );
	ListView_SetColumnWidth( hwndList, FL_COL_COL, LVSCW_AUTOSIZE );
	ListView_SetColumnWidth( hwndList, FL_COL_NAME, LVSCW_AUTOSIZE );
	ListView_SetColumnWidth( hwndList, FL_COL_REMARK, LVSCW_AUTOSIZE );
	ListView_SetColumnWidth( hwndList, FL_COL_ROW, ListView_GetColumnWidth( hwndList, FL_COL_ROW ) + 16 );
	ListView_SetColumnWidth( hwndList, FL_COL_COL, ListView_GetColumnWidth( hwndList, FL_COL_COL ) + 16 );
	ListView_SetColumnWidth( hwndList, FL_COL_NAME, ListView_GetColumnWidth( hwndList, FL_COL_NAME ) + 16 );
	ListView_SetColumnWidth( hwndList, FL_COL_REMARK, ListView_GetColumnWidth( hwndList, FL_COL_REMARK ) + 16 );

	return;
}

/*! 汎用ツリーコントロールの初期化：CFuncInfo::m_nDepthを利用して親子を設定

	@param[in] tagjump タグジャンプ形式で出力する

	@date 2002.04.01 YAZAKI
	@date 2002.11.10 Moca 階層の制限をなくした
	@date 2007.02.25 genta クリップボード出力をタブジャンプ可能な書式に変更
	@date 2007.03.04 genta タブジャンプ可能な書式に変更するかどうかのフラグを追加
	@date 2014.06.06 Moca 他ファイルへのタグジャンプ機能を追加
	@date 2020.09.12 選択処理をGetFuncInfoIndex,SetItemSelectionへ移動
*/
static std::wstring CompactWorkbenchTreeText(
	const WorkbenchTreeTextMetrics& metrics, const wchar_t* text, int depth )
{
	std::wstring result = text != nullptr ? text : L"";
	if( metrics.dc == nullptr || result.empty() ) return result;
	const int textLeft = (std::max)(0, depth) * metrics.indent + metrics.imageWidth + 11;
	const int available = (std::max)(0, metrics.clientWidth - textLeft - metrics.scrollWidth);
	if( available <= 0 ) return L"...";
	const int length = static_cast<int>((std::min)(result.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
	SIZE size{};
	if( ::GetTextExtentPoint32W(metrics.dc, result.c_str(), length, &size)
		&& size.cx > available ) {
		int fit = 0;
		const int prefixWidth = (std::max)(0, available - metrics.ellipsisWidth);
		if( !::GetTextExtentExPointW(
			metrics.dc, result.c_str(), length, prefixWidth, &fit, nullptr, &size) ) {
			// The fallback is logarithmic in label length and still reuses the
			// commit-scoped DC/font/metrics snapshot.
			int low = 0;
			int high = length;
			while( low < high ) {
				const int mid = low + (high - low + 1) / 2;
				SIZE prefix{};
				if( ::GetTextExtentPoint32W(metrics.dc, result.c_str(), mid, &prefix)
					&& prefix.cx <= prefixWidth ) low = mid;
				else high = mid - 1;
			}
			fit = low;
		}
		result.resize(static_cast<std::size_t>((std::max)(0, fit)));
		result += L"...";
	}
	return result;
}

void CDlgFuncList::SetTree(HTREEITEM hInsertAfter, bool tagjump, bool nolabel)
{
	HWND hwndTree = GetItemHwnd( IDC_TREE_FL );
	const bool workbenchMode = IsWorkbenchMode();
	if( workbenchMode ) {
		m_workbenchClipboardUsesGenericTree = true;
		m_workbenchClipboardTagJump = tagjump;
		m_workbenchClipboardNoLabel = nolabel;
		m_workbenchTreeLabels.clear();
	}

	int i;
	int nFuncInfoArrNum = m_pcFuncInfoArr->GetNum();
	if( workbenchMode ) m_workbenchTreeItems.assign(
		static_cast<std::size_t>((std::max)(0, nFuncInfoArrNum)), nullptr);
	int nStackPointer = 0;
	int nStackDepth = 32; // phParentStack の確保している数
	HTREEITEM* phParentStack;
	phParentStack = (HTREEITEM*)malloc( nStackDepth * sizeof( HTREEITEM ) );
	phParentStack[ nStackPointer ] = TVI_ROOT;

	if( !workbenchMode ) {
		m_cmemClipText.SetString(L"");
		int nCount = 0;
		int nBuffLen = 0;
		int nBuffLenTag = 3; // " \r\n"
		if( tagjump ){
			nBuffLenTag = int(10 + wcslen(m_pcFuncInfoArr->m_szFilePath));
		}
		for( int i2 = 0; i2 < nFuncInfoArrNum; i2++ ){
			const CFuncInfo* pcFuncInfo = m_pcFuncInfoArr->GetAt(i2);
			if( pcFuncInfo->IsAddClipText() ){
				nBuffLen += pcFuncInfo->m_cmemFuncName.GetStringLength() + pcFuncInfo->m_nDepth * 2;
				nCount++;
			}
		}
		m_cmemClipText.AllocStringBuffer( nBuffLen + nBuffLenTag * nCount );
	}

	for (i = 0; i < nFuncInfoArrNum; i++){
		CFuncInfo* pcFuncInfo = m_pcFuncInfoArr->GetAt(i);

		/*	新しいアイテムを作成
			現在の親の下にぶら下げる形で、最後に追加する。
		*/
		HTREEITEM hItem;
		TV_INSERTSTRUCT cTVInsertStruct;
		cTVInsertStruct.hParent = phParentStack[ nStackPointer ];
		cTVInsertStruct.hInsertAfter = hInsertAfter;
		cTVInsertStruct.item.mask = TVIF_TEXT | TVIF_PARAM;
		// ApplyWorkbenchAppearance owns the single workbench text pass.  The
		// insertion path keeps the full label so a later resize/font change can
		// compact from the value-owned model instead of a previously truncated label.
		cTVInsertStruct.item.pszText = pcFuncInfo->m_cmemFuncName.GetStringPtr();
		cTVInsertStruct.item.lParam = i;	//	あとでこの数値（＝m_pcFuncInfoArrの何番目のアイテムか）を見て、目的地にジャンプするぜ!!。
		if( workbenchMode ){
			cTVInsertStruct.item.mask |= TVIF_IMAGE | TVIF_SELECTEDIMAGE;
			const int imageIndex = WorkbenchSymbolImageIndex(pcFuncInfo->m_nInfo);
			cTVInsertStruct.item.iImage = imageIndex;
			cTVInsertStruct.item.iSelectedImage = imageIndex;
		}

		/*	親子関係をチェック
		*/
		if (nStackPointer != pcFuncInfo->m_nDepth){
			//	レベルが変わりました!!
			//	※が、2段階深くなることは考慮していないので注意。
			//	　もちろん、2段階以上浅くなることは考慮済み。

			// 2002.11.10 Moca 追加 確保したサイズでは足りなくなった。再確保
			if( nStackDepth <= pcFuncInfo->m_nDepth + 1 ){
				nStackDepth = pcFuncInfo->m_nDepth + 4; // 多めに確保しておく
				HTREEITEM* phTi;
				phTi = (HTREEITEM*)realloc( phParentStack, nStackDepth * sizeof( HTREEITEM ) );
				if( nullptr != phTi ){
					phParentStack = phTi;
				}else{
					goto end_of_func;
				}
			}
			nStackPointer = pcFuncInfo->m_nDepth;
			cTVInsertStruct.hParent = phParentStack[ nStackPointer ];
		}
		hItem = TreeView_InsertItem( hwndTree, &cTVInsertStruct );
		if( workbenchMode && static_cast<std::size_t>(i) < m_workbenchTreeItems.size() ) {
			m_workbenchTreeItems[static_cast<std::size_t>(i)] = hItem;
		}
		if( workbenchMode ) m_workbenchTreeLabels[hItem] = {
			pcFuncInfo->m_cmemFuncName.GetStringPtr() != nullptr
				? pcFuncInfo->m_cmemFuncName.GetStringPtr() : L"",
			pcFuncInfo->m_nDepth };
		phParentStack[ nStackPointer+1 ] = hItem;

		/* クリップボードコピー用テキストを作成する */
		//	2003.06.22 Moca dummy要素はツリーに入れるがTAGJUMPには加えない
		if( !workbenchMode && pcFuncInfo->IsAddClipText() ){
			CNativeW text;
			if( tagjump ){
				const WCHAR* pszFileName = pcFuncInfo->m_cmemFileName.GetStringPtr();
				if( pszFileName == nullptr ){
					pszFileName = m_pcFuncInfoArr->m_szFilePath;
				}
				text.AllocStringBuffer(
					  pcFuncInfo->m_cmemFuncName.GetStringLength()
					+ nStackPointer * 2 + 1
					+ wcslen( pszFileName )
					+ 20
				);
				//	2007.03.04 genta タグジャンプできる形式で書き込む
				text.AppendString( pszFileName );
				
				if( 0 < pcFuncInfo->m_nFuncLineCRLF ){
					WCHAR linenum[32];
					auto_sprintf( linenum, L"(%d,%d): ",
						pcFuncInfo->m_nFuncLineCRLF,				/* 検出行番号 */
						pcFuncInfo->m_nFuncColCRLF					/* 検出桁番号 */
					);
					text.AppendString( linenum );
				}
			}

			if( !nolabel ){
				for( int cnt = 0; cnt < nStackPointer; cnt++ ){
					text.AppendString(L"  ");
				}
				text.AppendString(L" ");
				
				text.AppendNativeData( pcFuncInfo->m_cmemFuncName );
			}
			text.AppendString( L"\r\n" );
			m_cmemClipText.AppendNativeData( text );	/* クリップボードコピー用テキスト */
		}
	}

end_of_func:;

	::EnableWindow( GetItemHwnd( IDC_BUTTON_COPY ), TRUE );

	free( phParentStack );
}

void CDlgFuncList::SetDocLineFuncList()
{
	if( m_nOutlineType == OUTLINE_BOOKMARK ){
		return;
	}
	if( m_nOutlineType == OUTLINE_FILETREE ){
		return;
	}
	CEditView* pcEditView=(CEditView*)m_lParam;
	CDocLineMgr* pcDocLineMgr = &pcEditView->GetDocument()->m_cDocLineMgr;
	
	CFuncListManager().ResetAllFucListMark(pcDocLineMgr, false);
	int i;
	int num = m_pcFuncInfoArr->GetNum();
	for( i = 0; i < num; ++i ){
		const CFuncInfo* pcFuncInfo = m_pcFuncInfoArr->GetAt(i);
		if( 0 < pcFuncInfo->m_nFuncLineCRLF ){
			CDocLine* pcDocLine = pcDocLineMgr->GetLine( pcFuncInfo->m_nFuncLineCRLF - 1 );
			if( pcDocLine ){
				CFuncListManager().SetLineFuncList( pcDocLine, true );
			}
		}
	}
}

/*! ファイルツリー作成
	@note m_pcFuncInfoArrにフルパス情報を書き込みつつツリーを作成
*/
void CDlgFuncList::SetTreeFile()
{
	HWND hwndTree = GetItemHwnd( IDC_TREE_FL );

	m_cmemClipText.SetString(L"");
	SFilePath IniDirPath;
	LoadFileTreeSetting( m_fileTreeSetting, IniDirPath );
	m_pcFuncInfoArr->Empty();
	int nFuncInfo = 0;
	std::vector<HTREEITEM> hParentTree;
	hParentTree.push_back(TVI_ROOT);
	for( int i = 0; i < (int)m_fileTreeSetting.m_aItems.size(); i++ ){
		WCHAR szPath[_MAX_PATH];
		WCHAR szPath2[_MAX_PATH];
		const SFileTreeItem& item = m_fileTreeSetting.m_aItems[i];
		// item.m_szTargetPath => szPath メタ文字の展開
		if( !CFileNameManager::ExpandMetaToFolder(item.m_szTargetPath, szPath, int(std::size(szPath))) ){
			wcscpy_s(szPath, std::size(szPath), L"<Error:Long Path>");
		}
		// szPath => szPath2 <iniroot>展開
		const WCHAR* pszFrom = szPath;
		if( m_fileTreeSetting.m_szLoadProjectIni[0] != L'\0'){
			CNativeW strTemp(pszFrom);
			strTemp.Replace(L"<iniroot>", IniDirPath.c_str());
			if( int(std::size(szPath2)) <= strTemp.GetStringLength() ){
				wcscpy_s(szPath2, std::size(szPath), L"<Error:Long Path>");
			}else{
				wcscpy_s(szPath2, std::size(szPath), strTemp.GetStringPtr());
			}
		}else{
			wcscpy(szPath2, pszFrom);
		}
		// szPath2 => szPath 「.」やショートパス等の展開
		pszFrom = szPath2;
		if( ::GetLongFileName(pszFrom, szPath) ){
		}else{
			wcscpy(szPath, pszFrom);
		}
		while( item.m_nDepth < (int)hParentTree.size() - 1 ){
			hParentTree.resize(hParentTree.size() - 1);
		}
		const WCHAR* pszLabel = szPath;
		if( item.m_szLabelName[0] != L'\0' ){
			pszLabel = item.m_szLabelName;
		}
		// lvis.item.lParam
		// 0 以下(nFuncInfo): m_pcFuncInfoArr->At(nFuncInfo)にファイル名
		// -1: Grepのファイル名要素
		// -2: Grepのサブフォルダー要素
		// -(nFuncInfo * 10 + 3): Grepルートフォルダー要素
		// -4: データ・追加操作なし
		TVINSERTSTRUCT tvis;
		tvis.hParent      = hParentTree.back();
		tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
		if( item.m_eFileTreeItemType == EFileTreeItemType_Grep ){
			m_pcFuncInfoArr->AppendData( CLogicInt(-1), CLogicInt(-1), CLayoutInt(-1), CLayoutInt(-1), L"", szPath, 0, 0 );
			tvis.item.pszText = const_cast<WCHAR*>(pszLabel);
			tvis.item.lParam  = -(nFuncInfo * 10 + 3);
			HTREEITEM hParent = TreeView_InsertItem(hwndTree, &tvis);
			nFuncInfo++;
			SetTreeFileSub( hParent, nullptr );
		}else if( item.m_eFileTreeItemType == EFileTreeItemType_File ){
			m_pcFuncInfoArr->AppendData( CLogicInt(-1), CLogicInt(-1), CLayoutInt(-1), CLayoutInt(-1), L"", szPath, 0, 0 );
			tvis.item.pszText = const_cast<WCHAR*>(pszLabel);
			tvis.item.lParam  = nFuncInfo;
			TreeView_InsertItem(hwndTree, &tvis);
			nFuncInfo++;
		}else if( item.m_eFileTreeItemType == EFileTreeItemType_Folder ){
			pszLabel = item.m_szLabelName;
			if( pszLabel[0] == L'\0' ){
				pszLabel = L"Folder";
			}
			tvis.item.pszText = const_cast<WCHAR*>(pszLabel);
			tvis.item.lParam  = -4;
			HTREEITEM hParent = TreeView_InsertItem(hwndTree, &tvis);
			hParentTree.push_back(hParent);
		}
	}
}

void CDlgFuncList::SetTreeFileSub( HTREEITEM hParent, const WCHAR* pszFile )
{
	HWND hwndTree = GetItemHwnd( IDC_TREE_FL );

	if( nullptr != TreeView_GetChild( hwndTree, hParent ) ){
		return;
	}

	HTREEITEM hItemSelected = nullptr;

	std::wstring basePath;
	int nItem = 0; // 設定Item番号
	if( !GetTreeFileFullName( hwndTree, hParent, &basePath, &nItem ) ){
		return; // error
	}

	int count = 0;
	CGrepEnumKeys cGrepEnumKeys;
	int errNo = cGrepEnumKeys.SetFileKeys( m_fileTreeSetting.m_aItems[nItem].m_szTargetFile );
	if( errNo != 0 ){
		TVINSERTSTRUCT tvis;
		tvis.hParent      = hParent;
		tvis.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
		tvis.item.pszText = const_cast<WCHAR*>(L"<Wild Card Error>");
		tvis.item.lParam = -4;
		TreeView_InsertItem(hwndTree, &tvis);
		return;
	}
	CGrepEnumOptions cGrepEnumOptions;
	cGrepEnumOptions.m_bIgnoreHidden   = m_fileTreeSetting.m_aItems[nItem].m_bIgnoreHidden;
	cGrepEnumOptions.m_bIgnoreReadOnly = m_fileTreeSetting.m_aItems[nItem].m_bIgnoreReadOnly;
	cGrepEnumOptions.m_bIgnoreSystem   = m_fileTreeSetting.m_aItems[nItem].m_bIgnoreSystem;
	CGrepEnumFiles cGrepExceptAbsFiles;
	cGrepExceptAbsFiles.Enumerates(L"", cGrepEnumKeys.m_vecExceptAbsFileKeys, cGrepEnumOptions);
	CGrepEnumFolders cGrepExceptAbsFolders;
	cGrepExceptAbsFolders.Enumerates(L"", cGrepEnumKeys.m_vecExceptAbsFolderKeys, cGrepEnumOptions);

	//フォルダー一覧作成
	CGrepEnumFilterFolders cGrepEnumFilterFolders;
	cGrepEnumFilterFolders.Enumerates( basePath.c_str(), cGrepEnumKeys, cGrepEnumOptions, cGrepExceptAbsFolders );
	int nItemCount = cGrepEnumFilterFolders.GetCount();
	count = nItemCount;
	for( int i = 0; i < nItemCount; i++ ){
		TVINSERTSTRUCT tvis;
		tvis.hParent      = hParent;
		tvis.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
		tvis.item.pszText = const_cast<WCHAR*>(cGrepEnumFilterFolders.GetFileName(i));
		tvis.item.lParam  = -2;
		tvis.item.cChildren = 1; // ダミーの子要素を持たせて[+]を表示
		TreeView_InsertItem(hwndTree, &tvis);
	}

	//ファイル一覧作成
	CGrepEnumFilterFiles cGrepEnumFilterFiles;
	cGrepEnumFilterFiles.Enumerates( basePath.c_str(), cGrepEnumKeys, cGrepEnumOptions, cGrepExceptAbsFiles );
	nItemCount = cGrepEnumFilterFiles.GetCount();
	count += nItemCount;
	for( int i = 0; i < nItemCount; i ++ ){
		const WCHAR* pFile = cGrepEnumFilterFiles.GetFileName(i);
		TVINSERTSTRUCT tvis;
		tvis.hParent      = hParent;
		tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
		tvis.item.pszText = const_cast<WCHAR*>(pFile);
		tvis.item.lParam  = -1;
		HTREEITEM hItem = TreeView_InsertItem(hwndTree, &tvis);
		if( pszFile && wmemicmp(pszFile, pFile) == 0 ){
			hItemSelected = hItem;
		}
	}
	if( hItemSelected ){
		TreeView_SelectItem( hwndTree, hItemSelected );
	}
	if( count == 0 ){
		// [+]記号削除
		TVITEM item;
		item.mask  = TVIF_HANDLE | TVIF_CHILDREN;
		item.cChildren = 0;
		item.hItem = hParent;
		TreeView_SetItem(hwndTree, &item);
	}
}

BOOL CDlgFuncList::OnInitDialog( HWND hwndDlg, WPARAM wParam, LPARAM lParam )
{
	m_bStretching = false;
	m_bHovering = false;
	m_nHilightedBtn = -1;
	m_nCapturingBtn = -1;

	_SetHwnd( hwndDlg );

	HWND		hwndList;
	int			nCxVScroll;
	int			nColWidthArr[] = { 0, 10, 46, 80 };
	RECT		rc;
	LV_COLUMN	col;
	hwndList = ::GetDlgItem( hwndDlg, IDC_LIST_FL );
	::SetWindowLongPtr(hwndList, GWL_STYLE, ::GetWindowLongPtr(hwndList, GWL_STYLE) | LVS_SHOWSELALWAYS );
	// 2005.10.21 zenryaku 1行選択
	ListView_SetExtendedListViewStyle(hwndList,
		ListView_GetExtendedListViewStyle(hwndList) | LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP);

	::GetWindowRect( hwndList, &rc );
	nCxVScroll = ::GetSystemMetrics( SM_CXVSCROLL );

	col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.fmt = LVCFMT_LEFT;
	col.cx = rc.right - rc.left - ( nColWidthArr[1] + nColWidthArr[2] + nColWidthArr[3] ) - nCxVScroll - 8;
	//	Apr. 23, 2005 genta 行番号を左端へ
	col.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_LIST_LINE_M));
	col.iSubItem = FL_COL_ROW;
	ListView_InsertColumn( hwndList, FL_COL_ROW, &col);

	// 2010.03.17 syat 桁追加
	col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.fmt = LVCFMT_LEFT;
	col.cx = nColWidthArr[FL_COL_COL];
	col.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_LIST_COL));
	col.iSubItem = FL_COL_COL;
	ListView_InsertColumn( hwndList, FL_COL_COL, &col);

	col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.fmt = LVCFMT_LEFT;
	col.cx = nColWidthArr[FL_COL_NAME];
	//	Apr. 23, 2005 genta 行番号を左端へ
	col.pszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_LIST_FUNC));
	col.iSubItem = FL_COL_NAME;
	ListView_InsertColumn( hwndList, FL_COL_NAME, &col);

	col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.fmt = LVCFMT_LEFT;
	col.cx = nColWidthArr[FL_COL_REMARK];
	col.pszText = const_cast<WCHAR*>(L" ");
	col.iSubItem = FL_COL_REMARK;
	ListView_InsertColumn( hwndList, FL_COL_REMARK, &col);

	/* アウトライン位置とサイズを初期化する */ // 20060201 aroka
	CEditView* pcEditView=(CEditView*)m_lParam;
	if( pcEditView != nullptr ){
		if( IsWorkbenchMode() ){
			m_xPos = 0;
			m_yPos = 0;
			m_nShowCmd = SW_HIDE;
			::GetClientRect( m_hwndWorkbenchParent, &rc );
			m_nWidth = std::max( 0L, rc.right - rc.left );
			m_nHeight = std::max( 0L, rc.bottom - rc.top );
		}else if( !IsDocking() && m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos ){
			WINDOWPLACEMENT cWindowPlacement;
			cWindowPlacement.length = sizeof( cWindowPlacement );
			if (::GetWindowPlacement( GetEditWnd().GetHwnd(), &cWindowPlacement )){
				/* ウィンドウ位置・サイズを-1以外の値にしておくと、CDialogで使用される． */
				m_xPos = m_pShareData->m_Common.m_sOutline.m_xOutlineWindowPos + cWindowPlacement.rcNormalPosition.left;
				m_yPos = m_pShareData->m_Common.m_sOutline.m_yOutlineWindowPos + cWindowPlacement.rcNormalPosition.top;
				m_nWidth =  m_pShareData->m_Common.m_sOutline.m_widthOutlineWindow;
				m_nHeight = m_pShareData->m_Common.m_sOutline.m_heightOutlineWindow;
			}
		}else if( IsDocking() ){
			m_xPos = 0;
			m_yPos = 0;
			m_nShowCmd = SW_HIDE;
			::GetWindowRect( ::GetParent(pcEditView->GetHwnd()), &rc );	// ここではまだ GetDockSpaceRect() は使えない
			EDockSide eDockSide = GetDockSide();
			switch( eDockSide ){
			case DOCKSIDE_LEFT:		m_nWidth = ProfDockLeft();		break;
			case DOCKSIDE_TOP:		m_nHeight = ProfDockTop();		break;
			case DOCKSIDE_RIGHT:	m_nWidth = ProfDockRight();		break;
			case DOCKSIDE_BOTTOM:	m_nHeight = ProfDockBottom();	break;
			default:
				break;
			}
			if( eDockSide == DOCKSIDE_LEFT || eDockSide == DOCKSIDE_RIGHT ){
				if( m_nWidth == 0 )	// 初回
					m_nWidth = (rc.right - rc.left) / 3;
				if( m_nWidth > rc.right - rc.left - DOCK_MIN_SIZE ) m_nWidth = rc.right - rc.left - DOCK_MIN_SIZE;
				if( m_nWidth < DOCK_MIN_SIZE ) m_nWidth = DOCK_MIN_SIZE;
			}else{
				if( m_nHeight == 0 )	// 初回
					m_nHeight = (rc.bottom - rc.top) / 3;
				if( m_nHeight > rc.bottom - rc.top - DOCK_MIN_SIZE ) m_nHeight = rc.bottom - rc.top - DOCK_MIN_SIZE;
				if( m_nHeight < DOCK_MIN_SIZE ) m_nHeight = DOCK_MIN_SIZE;
			}
		}
	}

	if( !IsWorkbenchMode() && !m_bInChangeLayout ){	// ChangeLayout() 処理中は設定変更しない
		bool bType = (ProfDockSet() != 0);
		if( bType ){
			CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
		}
		ProfDockDisp() = TRUE;
		if( bType ){
			SetTypeConfig( CTypeConfig(m_nDocType), m_type );
		}
		// 他ウィンドウに変更を通知する
		if( ProfDockSync() ){
			HWND hwndEdit = GetEditWnd().GetHwnd();
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );
		}
	}

	if( !UsesCompactPanelLayout() ){
		/* 基底クラスメンバ */
		CreateSizeBox();

		LONG_PTR lStyle = ::GetWindowLongPtr( GetHwnd(), GWL_STYLE );
		::SetWindowLongPtr( GetHwnd(), GWL_STYLE, lStyle | WS_THICKFRAME );
		::SetWindowPos( GetHwnd(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED );
	}

	m_hwndToolTip = nullptr;
	if( IsDocking() ){
		//ツールチップを作成する。（「閉じる」などのボタン用）
		m_hwndToolTip = ::CreateWindowEx(
			0,
			TOOLTIPS_CLASS,
			nullptr,
			WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			GetHwnd(),
			nullptr,
			m_hInstance,
			nullptr
			);

		// ツールチップをマルチライン可能にする（SHRT_MAX: Win95でINT_MAXだと表示されない）
		ApiWrap::Tooltip_SetMaxTipWidth( m_hwndToolTip, SHRT_MAX );

		// アウトラインにツールチップを追加する
		TOOLINFO	ti;
		ti.cbSize      = CCSIZEOF_STRUCT(TOOLINFO, lpszText);
		ti.uFlags      = TTF_SUBCLASS | TTF_IDISHWND;	// TTF_IDISHWND: uId は HWND で rect は無視（HWND 全体）
		ti.hwnd        = GetHwnd();
		ti.hinst       = m_hInstance;
		ti.uId         = (UINT_PTR)GetHwnd();
		ti.lpszText    = nullptr;
		ti.rect.left   = 0;
		ti.rect.top    = 0;
		ti.rect.right  = 0;
		ti.rect.bottom = 0;
		ApiWrap::Tooltip_AddTool( m_hwndToolTip, &ti );

	}
	if( UsesCompactPanelLayout() ){
		// 埋め込み時は内容だけを残す。タイトルとリサイズUIはpanel hostが所有する。
		HWND hwndPrev;
		HWND hwnd = ::GetWindow( GetHwnd(), GW_CHILD );
		while( hwnd ){
			int nId = ::GetDlgCtrlID( hwnd );
			hwndPrev = hwnd;
			hwnd = ::GetWindow( hwnd, GW_HWNDNEXT );
			switch( nId ){
			case IDC_LIST_FL:
			case IDC_TREE_FL:
				continue;
			default:
				break;
			}
			ShowWindow( hwndPrev, SW_HIDE );
		}
	}

	ApplyWorkbenchAppearance();
	SyncColor();

	::GetWindowRect( hwndDlg, &rc );
	m_ptDefaultSize.x = rc.right - rc.left;
	m_ptDefaultSize.y = rc.bottom - rc.top;
	
	::GetClientRect( hwndDlg, &rc );
	m_ptDefaultSizeClient.x = rc.right;
	m_ptDefaultSizeClient.y = rc.bottom;

	for( int i = 0; i < int(std::size(anchorList)); i++ ){
		GetItemClientRect( anchorList[i].id, m_rcItems[i] );
		// ドッキング中はウィンドウ幅いっぱいまで伸ばす
		if( UsesCompactPanelLayout() ){
			if( anchorList[i].anchor == ANCHOR_ALL ){
				::GetClientRect( hwndDlg, &rc );
				m_rcItems[i].right = rc.right;
				m_rcItems[i].bottom = rc.bottom;
			}
		}
	}

	return CDialog::OnInitDialog( hwndDlg, wParam, lParam );
}

BOOL CDlgFuncList::OnBnClicked( int wID )
{
	switch( wID ){
	case IDC_BUTTON_MENU:
		RECT rcMenu;
		GetWindowRect( GetItemHwnd( IDC_BUTTON_MENU ), &rcMenu );
		POINT ptMenu;
		ptMenu.x = rcMenu.left;
		ptMenu.y = rcMenu.bottom;
		DoMenu( ptMenu, GetHwnd() );
		return TRUE;
	case IDC_BUTTON_HELP:
		/* 「アウトライン解析」のヘルプ */
		//Apr. 5, 2001 JEPRO 修正漏れを追加 (Stonee, 2001/03/12 第四引数を、機能番号からヘルプトピック番号を調べるようにした)
		MyWinHelp( GetHwnd(), HELP_CONTEXT, ::FuncID_To_HelpContextID(F_OUTLINE) );	// 2006.10.10 ryoji MyWinHelpに変更に変更
		return TRUE;
	case IDOK:
		return OnJump();
	case IDCANCEL:
		if( m_bModal ){		/* モーダル ダイアログか */
			::EndDialog( GetHwnd(), 0 );
		}else{
			if( IsWorkbenchMode() ){
				// The host owns panel visibility. Escape must not destroy the child dialog.
				return TRUE;
			}else if( IsDocking() ){
				::SetFocus( ((CEditView*)m_lParam)->GetHwnd() );
			}else{
				::DestroyWindow( GetHwnd() );
			}
		}
		return TRUE;
	case IDC_BUTTON_COPY:
		// Windowsクリップボードにコピー 
		// 2004.02.17 Moca 関数化
		if( IsWorkbenchMode() && (IsAsyncWorkbenchOutlineType(m_nListType) || m_workbenchClipboardUsesGenericTree) ) BuildWorkbenchClipboardText();
		SetClipboardText( GetHwnd(), m_cmemClipText.GetStringPtr(), m_cmemClipText.GetStringLength() );
		return TRUE;
	case IDC_BUTTON_WINSIZE:
		{// ウィンドウの位置とサイズを記憶 // 20060201 aroka
			m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos = ::IsDlgButtonChecked( GetHwnd(), IDC_BUTTON_WINSIZE );
		}
		// ボタンが押されているかはっきりさせる 2008/6/5 Uchi
		ApiWrap::DlgItem_SetText( GetHwnd(), IDC_BUTTON_WINSIZE,
			m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos ? L"■" : L"□" );
		return TRUE;
	//2002.02.08 オプション切替後List/Treeにフォーカス移動
	case IDC_CHECK_bAutoCloseDlgFuncList:
	case IDC_CHECK_bMarkUpBlankLineEnable:
	case IDC_CHECK_bFunclistSetFocusOnJump:
		m_pShareData->m_Common.m_sOutline.m_bAutoCloseDlgFuncList = ::IsDlgButtonChecked( GetHwnd(), IDC_CHECK_bAutoCloseDlgFuncList );
		m_pShareData->m_Common.m_sOutline.m_bMarkUpBlankLineEnable = ::IsDlgButtonChecked( GetHwnd(), IDC_CHECK_bMarkUpBlankLineEnable );
		m_pShareData->m_Common.m_sOutline.m_bFunclistSetFocusOnJump = ::IsDlgButtonChecked( GetHwnd(), IDC_CHECK_bFunclistSetFocusOnJump );
		if(m_pShareData->m_Common.m_sOutline.m_bAutoCloseDlgFuncList){
			::EnableWindow( GetItemHwnd( IDC_CHECK_bFunclistSetFocusOnJump ), FALSE );
		}else{
			::EnableWindow( GetItemHwnd( IDC_CHECK_bFunclistSetFocusOnJump ), TRUE );
		}
		if(wID==IDC_CHECK_bMarkUpBlankLineEnable&&m_nListType==OUTLINE_BOOKMARK){
			CEditView* pcEditView=(CEditView*)m_lParam;
			pcEditView->GetCommander().HandleCommand( F_BOOKMARK_VIEW, true, TRUE, 0, 0, 0 );
			m_nCurLine=pcEditView->GetCaret().GetCaretLayoutPos().GetY2() + CLayoutInt(1);
			CDocTypeManager().GetTypeConfig(pcEditView->GetDocument()->m_cDocType.GetDocumentType(), m_type);
			SetData();
		}else
		if(m_nViewType == VIEWTYPE_TREE){
			::SetFocus( GetItemHwnd( IDC_TREE_FL ) );
		}else{
			::SetFocus( GetItemHwnd( IDC_LIST_FL ) );
		}
		return TRUE;
	case IDC_BUTTON_SETTING:
		{
			CDlgFileTree cDlgFileTree;
			int nRet = cDlgFileTree.DoModal( G_AppInstance(), GetHwnd(), (LPARAM)this );
			if( nRet == TRUE ){
				EFunctionCode nFuncCode = GetFuncCodeRedraw(m_nOutlineType);
				CEditView* pcEditView = (CEditView*)m_lParam;
				pcEditView->GetCommander().HandleCommand( nFuncCode, true, SHOW_RELOAD, 0, 0, 0 );
			}
		}
	default:
		break;
	}
	/* 基底クラスメンバ */
	return CDialog::OnBnClicked( wID );
}

BOOL CDlgFuncList::OnNotify(NMHDR* pNMHDR)
{
	NM_LISTVIEW*	pnlv;
	HWND			hwndList;
	HWND			hwndTree;
	NM_TREEVIEW*	pnmtv;
//	int				nLineTo;

	pnlv = (NM_LISTVIEW*)pNMHDR;

	CEditView* pcEditView=(CEditView*)m_lParam;
	hwndList = GetItemHwnd( IDC_LIST_FL );
	hwndTree = GetItemHwnd( IDC_TREE_FL );

	if( hwndTree == pNMHDR->hwndFrom ){
		pnmtv = (NM_TREEVIEW *)pNMHDR;
		switch( pnmtv->hdr.code ){
		case NM_CLICK:
			if( UsesCompactPanelLayout() ){
				// この時点ではまだ選択変更されていないが OnJump() の予備動作として先に選択変更しておく
				TVHITTESTINFO tvht = {};
				::GetCursorPos( &tvht.pt );
				::ScreenToClient( hwndTree, &tvht.pt );
				TreeView_HitTest( hwndTree, &tvht );
				if( (tvht.flags & TVHT_ONITEM) && tvht.hItem ){
					TreeView_SelectItem( hwndTree, tvht.hItem );
					OnJump( false, false );
					return TRUE;
				}
			}
			break;
		case NM_DBLCLK:
			// 2002.02.16 hor Treeのダブルクリックでフォーカス移動できるように 3/4
			OnJump();
			m_bWaitTreeProcess=true;
			::SetWindowLongPtr( GetHwnd(), DWLP_MSGRESULT, TRUE );	// ツリーの展開／縮小をしない
			return TRUE;
			//return OnJump();
		case TVN_KEYDOWN:
			if( ((TV_KEYDOWN *)pNMHDR)->wVKey == VK_SPACE ){
				OnJump( false );
				return TRUE;
			}
			Key2Command( ((TV_KEYDOWN *)pNMHDR)->wVKey );
			return TRUE;
		case NM_KILLFOCUS:
			// 2002.02.16 hor Treeのダブルクリックでフォーカス移動できるように 4/4
			if(m_bWaitTreeProcess){
				if(m_pShareData->m_Common.m_sOutline.m_bFunclistSetFocusOnJump){
					::SetFocus( pcEditView->GetHwnd() );
				}
				m_bWaitTreeProcess=false;
			}
			return TRUE;
		default:
			break;
		}
	}else
	if( hwndList == pNMHDR->hwndFrom ){
		switch(pNMHDR->code ){
		case LVN_COLUMNCLICK:
//			MYTRACE( L"LVN_COLUMNCLICK\n" );
			m_nSortCol =  pnlv->iSubItem;
			if( m_nSortCol == m_nSortColOld ){
				m_bSortDesc = !m_bSortDesc;
			}
			m_nSortColOld = m_nSortCol;
			{
				STypeConfig* type = new STypeConfig();
				CDocTypeManager().GetTypeConfig( CTypeConfig(m_nDocType), *type );
				type->m_nOutlineSortCol = m_nSortCol;
				type->m_bOutlineSortDesc = m_bSortDesc;
				SetTypeConfig( CTypeConfig(m_nDocType), *type );
				delete type;
			}
			//	Apr. 23, 2005 genta 関数として独立させた
			SortListView( hwndList, m_nSortCol );
			return TRUE;
		case NM_CLICK:
			if( UsesCompactPanelLayout() ){
				OnJump( false, false );
				return TRUE;
			}
			break;
		case NM_DBLCLK:
				OnJump();
			return TRUE;
		case LVN_KEYDOWN:
			if( ((LV_KEYDOWN *)pNMHDR)->wVKey == VK_SPACE ){
				OnJump( false );
				return TRUE;
			}
			Key2Command( ((LV_KEYDOWN *)pNMHDR)->wVKey );
			return TRUE;
		default:
			break;
		}
	}

#ifdef DEFINE_SYNCCOLOR
	if( UsesCompactPanelLayout() ){
		if( hwndList == pNMHDR->hwndFrom || hwndTree == pNMHDR->hwndFrom ){
			if(pNMHDR->code == NM_CUSTOMDRAW ){
				LPNMCUSTOMDRAW lpnmcd = (LPNMCUSTOMDRAW)pNMHDR;
				switch( lpnmcd->dwDrawStage ){
				case CDDS_PREPAINT:
					::SetWindowLongPtr( GetHwnd(), DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW );
					break;
				case CDDS_ITEMPREPAINT:
					{	// 選択アイテムを反転表示にする
						if( IsWorkbenchMode() ){
							const bool selected = hwndList == pNMHDR->hwndFrom
								? (ListView_GetItemState(hwndList, lpnmcd->dwItemSpec, LVIS_SELECTED) != 0)
								: ((lpnmcd->uItemState & CDIS_SELECTED) != 0);
							const bool hot = (lpnmcd->uItemState & CDIS_HOT) != 0;
							const COLORREF text = selected ? m_workbenchSelectionText : m_workbenchText;
							const COLORREF background = selected ? m_workbenchSelection
								: (hot ? m_workbenchHover : m_workbenchBackground);
							if( hwndList == pNMHDR->hwndFrom ){
								((LPNMLVCUSTOMDRAW)lpnmcd)->clrText = text;
								((LPNMLVCUSTOMDRAW)lpnmcd)->clrTextBk = background;
								if( selected ) lpnmcd->uItemState &= ~CDIS_SELECTED;
							}else{
								((LPNMTVCUSTOMDRAW)lpnmcd)->clrText = text;
								((LPNMTVCUSTOMDRAW)lpnmcd)->clrTextBk = background;
							}
							break;
						}
						const STypeConfig	*TypeDataPtr = &(pcEditView->m_pcEditDoc->m_cDocType.GetDocumentAttribute());
						COLORREF clrText = TypeDataPtr->m_ColorInfoArr[COLORIDX_TEXT].m_sColorAttr.m_cTEXT;
						COLORREF clrTextBk = TypeDataPtr->m_ColorInfoArr[COLORIDX_TEXT].m_sColorAttr.m_cBACK;
						if( hwndList == pNMHDR->hwndFrom ){
							//if( lpnmcd->uItemState & CDIS_SELECTED ){	// 非選択のアイテムもすべて CDIS_SELECTED で来る？
							if( ListView_GetItemState( hwndList, lpnmcd->dwItemSpec, LVIS_SELECTED ) ){
								((LPNMLVCUSTOMDRAW)lpnmcd)->clrText = clrText ^ RGB(255, 255, 255);
								((LPNMLVCUSTOMDRAW)lpnmcd)->clrTextBk = clrTextBk ^ RGB(255, 255, 255);
								lpnmcd->uItemState = 0;	// リストビューには選択としての描画をさせないようにする？
							}
						}else{
							if( lpnmcd->uItemState & CDIS_SELECTED ){
								((LPNMTVCUSTOMDRAW)lpnmcd)->clrText = clrText ^ RGB(255, 255, 255);
								((LPNMTVCUSTOMDRAW)lpnmcd)->clrTextBk = clrTextBk ^ RGB(255, 255, 255);
							}
						}
					}
					::SetWindowLongPtr( GetHwnd(), DWLP_MSGRESULT, CDRF_DODEFAULT );
					break;
				default:
					break;
				}

				return TRUE;
			}
		}
	}
#endif

	return FALSE;
}
/*!
	指定されたカラムでリストビューをソートする．
	同時にヘッダーも書き換える．

	ソート後はフォーカスが画面内に現れるように表示位置を調整する．

	@par 表示位置調整の小技
	EnsureVisibleの結果は，上スクロールの場合は上端に，下スクロールの場合は
	下端に目的の項目が現れる．端から少し離したい場合はオフセットを与える必要が
	あるが，スクロール方向がわからないと±がわからない
	そのため最初に一番下に一回スクロールさせることでEnsureVisibleでは
	かならず上スクロールになるようにすることで，ソート後の表示位置を
	固定する

	@param[in] hwndList	リストビューのウィンドウハンドル
	@param[in] sortcol	ソートするカラム番号(0-2)

	@date 2005.04.23 genta 関数として独立させた
	@date 2005.04.29 genta ソート後の表示位置調整
	@date 2010.03.17 syat 桁追加
*/
void CDlgFuncList::SortListView(HWND hwndList, int sortcol)
{
	LV_COLUMN		col;
	int col_no;

	//	Apr. 23, 2005 genta 行番号を左端へ

//	if( sortcol == 1 ){
	{
		col_no = FL_COL_NAME;
		col.mask = LVCF_TEXT;
	// From Here 2001.12.03 hor
	//	col.pszText = L"関数名 *";
		if(OUTLINE_BOOKMARK == m_nListType){
			col.pszText = const_cast<WCHAR*>( sortcol == col_no ? LS(STR_DLGFNCLST_LIST_TEXT_M) : LS(STR_DLGFNCLST_LIST_TEXT) );
		}else{
			col.pszText = const_cast<WCHAR*>( sortcol == col_no ? LS(STR_DLGFNCLST_LIST_FUNC_M) : LS(STR_DLGFNCLST_LIST_FUNC) );
		}
	// To Here 2001.12.03 hor
		col.iSubItem = 0;
		ListView_SetColumn( hwndList, col_no, &col );

		col_no = FL_COL_ROW;
		col.mask = LVCF_TEXT;
		col.pszText = const_cast<WCHAR*>( sortcol == col_no ? LS(STR_DLGFNCLST_LIST_LINE_M) : LS(STR_DLGFNCLST_LIST_LINE) );
		col.iSubItem = 0;
		ListView_SetColumn( hwndList, col_no, &col );

		// 2010.03.17 syat 桁追加
		col_no = FL_COL_COL;
		col.mask = LVCF_TEXT;
		col.pszText = const_cast<WCHAR*>( sortcol == col_no ? LS(STR_DLGFNCLST_LIST_COL_M) : LS(STR_DLGFNCLST_LIST_COL) );
		col.iSubItem = 0;
		ListView_SetColumn( hwndList, col_no, &col );

		col_no = FL_COL_REMARK;
	// From Here 2001.12.07 hor
		col.mask = LVCF_TEXT;
		col.pszText = const_cast<WCHAR*>( sortcol == col_no ? LS(STR_DLGFNCLST_LIST_M) : L"" );
		col.iSubItem = 0;
		ListView_SetColumn( hwndList, col_no, &col );
	// To Here 2001.12.07 hor

		ListView_SortItems( hwndList, (m_bSortDesc ? CompareFunc_Desc : CompareFunc_Asc), (LPARAM)this );
	}
	//	2005.04.23 zenryaku 選択された項目が見えるようにする

	//	Apr. 29, 2005 genta 一旦一番下にスクロールさせる
	ListView_EnsureVisible( hwndList,
		ListView_GetItemCount(hwndList) - 1,
		FALSE );
	
	//	Jan.  9, 2006 genta 先頭から1つ目と2つ目の関数が
	//	選択された場合にスクロールされなかった
	int keypos = ListView_GetNextItem(hwndList, -1, LVNI_FOCUSED) - 2;
	ListView_EnsureVisible( hwndList,
		keypos >= 0 ? keypos : 0,
		FALSE );
}

/*!	ウィンドウサイズが変更された

	@date 2003.06.22 Moca コードの整理(コントロールの処理方法をテーブルに持たせる)
	@date 2003.08.16 genta 配列はstaticに(無駄な初期化を行わないため)
*/
BOOL CDlgFuncList::OnSize( WPARAM wParam, LPARAM lParam )
{
	// 今のところ CEditWnd::OnSize() からの呼び出しでは lParam は CEditWnd 側 の lParam のまま渡される	// 2010.06.05 ryoji
	RECT rcDlg;
	::GetClientRect( GetHwnd(), &rcDlg );
	lParam = MAKELONG(rcDlg.right - rcDlg.left, rcDlg.bottom -  rcDlg.top);	// 自前で補正

	/* 基底クラスメンバ */
	CDialog::OnSize( wParam, lParam );
	if( IsWorkbenchMode() ){
		const auto childLayout = workbench::outline::MakeOutlineChildLayout(
			rcDlg.right - rcDlg.left, rcDlg.bottom - rcDlg.top );
		const int width = childLayout.bounds.right - childLayout.bounds.left;
		const int height = childLayout.bounds.bottom - childLayout.bounds.top;
		const HWND controls[] = { GetItemHwnd(IDC_TREE_FL), GetItemHwnd(IDC_LIST_FL) };
		HDWP deferred = ::BeginDeferWindowPos(static_cast<int>(std::size(controls)));
		for( const HWND control : controls ){
			if( deferred == nullptr || control == nullptr ) continue;
			deferred = ::DeferWindowPos( deferred, control, nullptr,
				childLayout.bounds.left, childLayout.bounds.top, width, height,
				SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW );
		}
		const bool positioned = deferred != nullptr && ::EndDeferWindowPos(deferred) != FALSE;
		if( !positioned ){
			for( const HWND control : controls ){
				if( control != nullptr ) ::SetWindowPos( control, nullptr,
					childLayout.bounds.left, childLayout.bounds.top, width, height,
					SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW );
			}
		}
		if( m_workbenchAppearanceWidth != width ){
			m_workbenchAppearanceWidth = width;
			m_workbenchTreeContentDirty = true;
		}
		if( m_workbenchAppearanceDirty || m_workbenchTreeContentDirty ) ApplyWorkbenchAppearance();
		else ::RedrawWindow( GetHwnd(), nullptr, nullptr,
			RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE | RDW_NOINTERNALPAINT );
		return TRUE;
	}

	RECT  rc;
	POINT ptNew;
	ptNew.x = rcDlg.right - rcDlg.left;
	ptNew.y = rcDlg.bottom - rcDlg.top;

	for( int i = 0 ; i < int(std::size(anchorList)); i++ ){
		HWND hwndCtrl = GetItemHwnd(anchorList[i].id);
		ResizeItem( hwndCtrl, m_ptDefaultSizeClient, ptNew, m_rcItems[i], anchorList[i].anchor, (anchorList[i].anchor != ANCHOR_ALL));
//	2013.2.6 aroka ちらつき防止用の試行錯誤
		if(anchorList[i].anchor == ANCHOR_ALL){
			::UpdateWindow( hwndCtrl );
		}
	}

//	if( IsDocking() )
	{
		// ダイアログ部分を再描画（ツリー／リストの範囲はちらつかないように除外）
		::InvalidateRect( GetHwnd(), nullptr, FALSE );
		POINT pt;
		::GetWindowRect( GetItemHwnd( IDC_TREE_FL ), &rc );
		pt.x = rc.left;
		pt.y = rc.top;
		::ScreenToClient( GetHwnd(), &pt );
		::OffsetRect( &rc, pt.x - rc.left, pt.y - rc.top );
		::ValidateRect( GetHwnd(), &rc );
	}
	return TRUE;
}

BOOL CDlgFuncList::OnMinMaxInfo( LPARAM lParam )
{
	LPMINMAXINFO lpmmi = (LPMINMAXINFO) lParam;
	if( m_ptDefaultSize.x < 0 ){
		return 0;
	}
	lpmmi->ptMinTrackSize.x = m_ptDefaultSize.x/2;
	lpmmi->ptMinTrackSize.y = m_ptDefaultSize.y/3;
	lpmmi->ptMaxTrackSize.x = m_ptDefaultSize.x*2;
	lpmmi->ptMaxTrackSize.y = m_ptDefaultSize.y*2;
	return 0;
}
static inline int CALLBACK Compare_by_ItemData(LPARAM lParam1, LPARAM lParam2, [[maybe_unused]] LPARAM lParamSort)
{
	if( lParam1< lParam2 )
		return -1;
	if( lParam1 > lParam2 )
		return 1;
	else
		return 0;
}

static int CALLBACK Compare_by_ItemDataDesc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
	return Compare_by_ItemData(lParam2, lParam1, lParamSort);
}

struct STreeViewSortData{
	std::vector<std::wstring> m_vecText;
};

static int CALLBACK Compare_by_ItemText(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
	STreeViewSortData* pData = (STreeViewSortData*)lParamSort;
	std::wstring* pText1 = &pData->m_vecText[lParam1];
	std::wstring* pText2 = &pData->m_vecText[lParam2];
	int result = ::lstrcmpi(pText1->c_str(), pText2->c_str());
	if( result == 0 ){
		// 同じ名前は登録順
		return Compare_by_ItemData(lParam1, lParam2, lParamSort);
	}
	return result;
}

static int CALLBACK Compare_by_ItemTextDesc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
	return Compare_by_ItemText(lParam2, lParam1, lParamSort);
}

BOOL CDlgFuncList::OnDestroy( void )
{
	StopWorkbenchOutlineWorker();
	CDialog::OnDestroy();

	/* アウトライン ■位置とサイズを記憶する */ // 20060201 aroka
	// 前提条件：m_lParam が CDialog::OnDestroy でクリアされないこと
	HWND hwndEdit = IsWorkbenchMode()? nullptr: GetEditWnd().GetHwnd();
	if( !IsWorkbenchMode() && !IsDocking() && m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos ){
		/* 親のウィンドウ位置・サイズを記憶 */
		WINDOWPLACEMENT cWindowPlacement;
		cWindowPlacement.length = sizeof( cWindowPlacement );
		if (::GetWindowPlacement( hwndEdit, &cWindowPlacement )){
			/* ウィンドウ位置・サイズを記憶 */
			m_pShareData->m_Common.m_sOutline.m_xOutlineWindowPos = m_xPos - cWindowPlacement.rcNormalPosition.left;
			m_pShareData->m_Common.m_sOutline.m_yOutlineWindowPos = m_yPos - cWindowPlacement.rcNormalPosition.top;
			m_pShareData->m_Common.m_sOutline.m_widthOutlineWindow = m_nWidth;
			m_pShareData->m_Common.m_sOutline.m_heightOutlineWindow = m_nHeight;
		}
	}

	// ドッキング画面を閉じるときは画面を再レイアウトする
	// ドッキングでアプリ終了時には hwndEdit は NULL になっている（親に先に WM_DESTROY が送られるため）
	if( !IsWorkbenchMode() && IsDocking() && hwndEdit )
		GetEditWnd().EndLayoutBars();

	// 明示的にアウトライン画面を閉じたときだけアウトライン表示フラグを OFF にする
	// フローティングでアプリ終了時やタブモードで裏にいる場合は ::IsWindowVisible( hwndEdit ) が FALSE を返す
	if( !IsWorkbenchMode() && hwndEdit && ::IsWindowVisible( hwndEdit ) && !m_bInChangeLayout ){	// ChangeLayout() 処理中は設定変更しない
		bool bType = (ProfDockSet() != 0);
		if( bType ){
			CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
		}
		ProfDockDisp() = FALSE;
		if( bType ){
			SetTypeConfig( CTypeConfig(m_nDocType), m_type );
		}
		// 他ウィンドウに変更を通知する
		if( ProfDockSync() ){
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );
		}
	}

	if( m_hwndToolTip ){
		::DestroyWindow( m_hwndToolTip );
		m_hwndToolTip = nullptr;
	}
	::KillTimer( GetHwnd(), 1 );

	return TRUE;
}

/*!
	@date 2016.03.04 Moca OnCbnSelChange -> OnCbnSelEndOk マウスで一覧から選択中にソートされないように変更
*/
BOOL CDlgFuncList::OnCbnSelEndOk( HWND hwndCtl, int wID )
{
	int nSelect = ApiWrap::Combo_GetCurSel( hwndCtl );
	switch(wID)
	{
	case IDC_COMBO_nSortType:
		if( m_nSortType != nSelect )
		{
			m_nSortType = nSelect;
			STypeConfig* type = new STypeConfig();
			CDocTypeManager().GetTypeConfig( CTypeConfig(m_nDocType), *type );
			type->m_nOutlineSortType = m_nSortType;
			SetTypeConfig( CTypeConfig(m_nDocType), *type );
			delete type;
			HWND hWndTree = GetItemHwnd(IDC_TREE_FL);
			::SendMessageAny(hWndTree, WM_SETREDRAW, (WPARAM)FALSE, 0);
			SortTree(hWndTree,TVI_ROOT);
			::SendMessageAny(hWndTree, WM_SETREDRAW, (WPARAM)TRUE, 0);
		}
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

static void SortTree_Sub(HWND hWndTree,HTREEITEM htiParent, STreeViewSortData& data, int nSortType)
{
	if( SORTTYPE_ATOZ == nSortType || SORTTYPE_ZTOA == nSortType ){
		for(HTREEITEM htiItem = TreeView_GetChild( hWndTree, htiParent ); nullptr != htiItem ; htiItem = TreeView_GetNextSibling( hWndTree, htiItem )){
			TVITEM item;
			item.mask = TVIF_HANDLE | TVIF_TEXT | TVIF_PARAM;
			item.hItem = htiItem;
			std::vector<WCHAR> vecStr;
			if( ApiWrap::TreeView_GetItemTextVector(hWndTree, item, vecStr) ){
				data.m_vecText[item.lParam].assign(&vecStr[0]);
			}
		}
	}
	TVSORTCB sort;
	sort.hParent = htiParent;
	switch( nSortType ){
	case SORTTYPE_DEFAULT:
		sort.lpfnCompare = Compare_by_ItemData;
		sort.lParam = 0;
		TreeView_SortChildrenCB(hWndTree , &sort , FALSE);
		// TreeView_SortChildren(hWndTree,htiParent,FALSE);
		break;
	case SORTTYPE_DEFAULT_DESC:
		sort.lpfnCompare = Compare_by_ItemDataDesc;
		sort.lParam = 0;
		TreeView_SortChildrenCB(hWndTree , &sort , FALSE);
		break;
	case SORTTYPE_ATOZ:
		sort.lpfnCompare = Compare_by_ItemText;
		sort.lParam = (LPARAM)&data;
		TreeView_SortChildrenCB(hWndTree , &sort , FALSE);
		break;
	case SORTTYPE_ZTOA:
		sort.lpfnCompare = Compare_by_ItemTextDesc;
		sort.lParam = (LPARAM)&data;
		TreeView_SortChildrenCB(hWndTree , &sort , FALSE);
		break;
	default:
		assert(0);
		break;
	}

	for(HTREEITEM htiItem = TreeView_GetChild( hWndTree, htiParent ); nullptr != htiItem ; htiItem = TreeView_GetNextSibling( hWndTree, htiItem )){
		SortTree_Sub(hWndTree, htiItem, data, nSortType);
	}
}

void CDlgFuncList::SortTree(HWND hWndTree,HTREEITEM htiParent)
{
	STreeViewSortData data;
	int size = m_pcFuncInfoArr->GetNum();
	if( m_bDummyLParamMode ){
		size = m_nTreeItemCount;
	}
	data.m_vecText.resize(size);
	SortTree_Sub(hWndTree, htiParent, data, m_nSortType);
}

bool CDlgFuncList::TagJumpTimer( const WCHAR* pFile, CMyPoint point, bool bCheckAutoClose )
{
	CEditView* pcView = reinterpret_cast<CEditView*>(m_lParam);

	// ファイルを開いていない場合は自分で開く
	if( pcView->GetDocument()->IsAcceptLoad() ){
		std::wstring strFile = pFile;
		pcView->GetCommander().Command_FILEOPEN( strFile.c_str(), CODE_AUTODETECT, CAppMode::getInstance()->IsViewMode(), nullptr );
		if( point.y != -1 ){
			if( pcView->GetDocument()->m_cDocFile.GetFilePathClass().IsValidPath() ){
				CLogicPoint pt;
				pt.x = CLogicInt(point.GetX() - 1);
				pt.y = CLogicInt(point.GetY() - 1);
				if( pt.x < 0 ){
					pt.x = 0;
				}
				pcView->GetCommander().Command_MOVECURSOR( pt, 0 );
			}
		}
		return true;
	}
	m_pszTimerJumpFile = pFile;
	m_pointTimerJump = point;
	m_bTimerJumpAutoClose = bCheckAutoClose;
	::SetTimer( GetHwnd(), 2, 200, nullptr ); // id == 2
	return false;
}

BOOL CDlgFuncList::OnJump( bool bCheckAutoClose, bool bFileJump )	//2002.02.08 hor 引数追加
{
	if( IsWorkbenchMode() && !HasCurrentWorkbenchModel() ) return FALSE;
	int				nLineTo;
	int				nColTo;
	/* ダイアログデータの取得 */
	if( 0 < GetData() && (m_cFuncInfo != nullptr || 0 < m_sJumpFile.size() ) ){
		if( m_bModal ){		/* モーダル ダイアログか */
			//モーダル表示する場合は、m_cFuncInfoを取得するアクセサを実装して結果取得すること。
			::EndDialog( GetHwnd(), 1 );
		}else{
			bool bFileJumpSelf = true;
			if( 0 < m_sJumpFile.size() ){
				if( bFileJump ){
					// ファイルツリーの場合
					if( m_bModal ){		/* モーダル ダイアログか */
						//モーダル表示する場合は、m_cFuncInfoを取得するアクセサを実装して結果取得すること。
						::EndDialog( GetHwnd(), 1 );
					}
					CMyPoint poCaret;
					poCaret.x = -1;
					poCaret.y = -1;
					bFileJumpSelf = TagJumpTimer(m_sJumpFile.c_str(), poCaret, bCheckAutoClose);
				}
			}else
			if( m_cFuncInfo != nullptr && 0 < m_cFuncInfo->m_cmemFileName.GetStringLength() ){
				if( bFileJump ){
					nLineTo = m_cFuncInfo->m_nFuncLineCRLF;
					nColTo = m_cFuncInfo->m_nFuncColCRLF;
					// 別のファイルへジャンプ
					CMyPoint poCaret; // TagJumpSubも1開始
					poCaret.x = nColTo;
					poCaret.y = nLineTo;
					bFileJumpSelf = TagJumpTimer(m_cFuncInfo->m_cmemFileName.GetStringPtr(), poCaret, bCheckAutoClose);
				}
			}else{
				nLineTo = m_cFuncInfo->m_nFuncLineCRLF;
				nColTo = m_cFuncInfo->m_nFuncColCRLF;
				/* カーソルを移動させる */
				CLogicPoint	poCaret;
				poCaret.x = nColTo - 1;
				poCaret.y = nLineTo - 1;

				m_pShareData->m_sWorkBuffer.m_LogicPoint = poCaret;

				//	2006.07.09 genta 移動時に選択状態を保持するように
				::SendMessageAny( GetEditWnd().GetHwnd(),
					MYWM_SETCARETPOS, 0, PM_SETCARETPOS_KEEPSELECT );
			}
			if( bCheckAutoClose && bFileJumpSelf ){
				/* アウトライン ダイアログを自動的に閉じる */
				if( UsesCompactPanelLayout() ){
					::PostMessageAny( ((CEditView*)m_lParam)->GetHwnd(), MYWM_SETACTIVEPANE, 0, 0 );
				}
				else if( m_pShareData->m_Common.m_sOutline.m_bAutoCloseDlgFuncList ){
					::DestroyWindow( GetHwnd() );
				}
				else if( m_pShareData->m_Common.m_sOutline.m_bFunclistSetFocusOnJump ){
					::SetFocus( ((CEditView*)m_lParam)->GetHwnd() );
				}
			}
		}
	}
	return TRUE;
}

//@@@ 2002.01.18 add start
LPVOID CDlgFuncList::GetHelpIdTable(void)
{
	return (LPVOID)p_helpids;
}
//@@@ 2002.01.18 add end

/*!	キー操作をコマンドに変換するヘルパー関数
	
*/
void CDlgFuncList::Key2Command(WORD KeyCode)
{
	CEditView*	pcEditView;
// novice 2004/10/10
	/* Shift,Ctrl,Altキーが押されていたか */
	int nIdx = getCtrlKeyState();
	EFunctionCode nFuncCode=CKeyBind::GetFuncCode(
			((WORD)(((BYTE)(KeyCode)) | ((WORD)((BYTE)(nIdx))) << 8)),
			m_pShareData->m_Common.m_sKeyBind.m_nKeyNameArrNum,
			m_pShareData->m_Common.m_sKeyBind.m_pKeyNameArr
	);
	switch( nFuncCode ){
	case F_REDRAW:
		nFuncCode=GetFuncCodeRedraw(m_nOutlineType);
		/*FALLTHROUGH*/
	case F_OUTLINE:
	case F_OUTLINE_TOGGLE: // 20060201 aroka フォーカスがあるときはリロード
	case F_BOOKMARK_VIEW:
	case F_FILETREE:
		pcEditView=(CEditView*)m_lParam;
		pcEditView->GetCommander().HandleCommand( nFuncCode, true, SHOW_RELOAD, 0, 0, 0 ); // 引数の変更 20060201 aroka

		break;
	case F_BOOKMARK_SET:
		OnJump( false );
		pcEditView=(CEditView*)m_lParam;
		pcEditView->GetCommander().HandleCommand( nFuncCode, true, 0, 0, 0, 0 );

		break;
	case F_COPY:
	case F_CUT:
		OnBnClicked( IDC_BUTTON_COPY );
		break;
	default:
		break;
	}
}

/*!
	@date 2002.10.05 genta
*/
void CDlgFuncList::Redraw( int nOutLineType, int nListType, CFuncInfoArr* pcFuncInfoArr, CLayoutInt nCurLine, CLayoutInt nCurCol )
{
	CEditView* pcEditView = (CEditView*)m_lParam;
	ObserveWorkbenchDocument( pcEditView );
	m_nDocType = pcEditView->GetDocument()->m_cDocType.GetDocumentType().GetIndex();
	CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
	SyncColor();

	m_nOutlineType = nOutLineType;
	m_nListType = nListType;
	m_pcFuncInfoArr = pcFuncInfoArr;	/* 関数情報配列 */
	m_bFuncInfoArrIsUpToDate = true;
	m_nCurLine = nCurLine;				/* 現在行 */
	m_nCurCol = nCurCol;				/* 現在桁 */

	bool bType = (ProfDockSet() != 0);
	if( bType ){
		m_type.m_nDockOutline = m_nOutlineType;
		SetTypeConfig( CTypeConfig(m_nDocType), m_type );
	}else{
		CommonSet().m_nDockOutline = m_nOutlineType;
	}

	SetData();
	CommitWorkbenchModel();
}

//ダイアログタイトルの設定
void CDlgFuncList::SetWindowText( const WCHAR* szTitle )
{
	::SetWindowText( GetHwnd(), szTitle );
}

/** 配色適用処理
	@date 2010.06.05 ryoji 新規作成
*/
void CDlgFuncList::SyncColor( void )
{
	if( IsWorkbenchMode() ){
		ApplyWorkbenchAppearance();
		return;
	}
	if( !UsesCompactPanelLayout() )
		return;
#ifdef DEFINE_SYNCCOLOR
	// テキスト色・背景色をビューと同色にする
	CEditView* pcEditView = (CEditView*)m_lParam;
	const STypeConfig	*TypeDataPtr = &(pcEditView->m_pcEditDoc->m_cDocType.GetDocumentAttribute());
	COLORREF clrText = TypeDataPtr->m_ColorInfoArr[COLORIDX_TEXT].m_sColorAttr.m_cTEXT;
	COLORREF clrBack = TypeDataPtr->m_ColorInfoArr[COLORIDX_TEXT].m_sColorAttr.m_cBACK;

	HWND hwndTree = GetItemHwnd( IDC_TREE_FL );
	TreeView_SetTextColor( hwndTree, clrText );
	TreeView_SetBkColor( hwndTree, clrBack );
	{
		// WinNT4.0 あたりではウィンドウスタイルを強制的に再設定しないと
		// ツリーアイテムの左側が真っ黒になる
		LONG lStyle = (LONG)GetWindowLongPtr(hwndTree, GWL_STYLE);
		SetWindowLongPtr( hwndTree, GWL_STYLE, lStyle & ~(TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT) );
		SetWindowLongPtr( hwndTree, GWL_STYLE, lStyle );
	}
	::SetWindowPos( hwndTree, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED );	// なぜかこうしないと四辺１ドット幅分だけ色変更が即時適用されない（←スタイル再設定とは無関係）
	::InvalidateRect( hwndTree, nullptr, TRUE );

	HWND hwndList = GetItemHwnd( IDC_LIST_FL );
	ListView_SetTextColor( hwndList, clrText );
	ListView_SetTextBkColor( hwndList, clrBack );
	ListView_SetBkColor( hwndList, clrBack );
	::InvalidateRect( hwndList, nullptr, TRUE );
#endif
}

/** ドッキング対象矩形の取得（スクリーン座標）
	@date 2010.06.05 ryoji 新規作成
*/
void CDlgFuncList::GetDockSpaceRect( LPRECT pRect )
{
	CEditView* pcEditView = (CEditView*)m_lParam;
	// CDlgFuncList と CSplitterWnd の外接矩形
	// 2014.12.02 ミニマップ対応
	HWND hwnd[3];
	RECT rc[3];
	hwnd[0] = ::GetParent( pcEditView->GetHwnd() );	// CSplitterWnd
	int nCount = 1;
	if( IsDocking() ){
		hwnd[nCount] = GetHwnd();
		nCount++;
	}
	hwnd[nCount] = GetEditWnd().GetMiniMap().GetHwnd();
	if( hwnd[nCount] != nullptr ){
		nCount++;
	}
	for( int i = 0; i < nCount; i++ ){
		::GetWindowRect(hwnd[i], &rc[i]);
	}
	if( 1 == nCount ){
		*pRect = rc[0];
	}else if( 2 == nCount ){
		::UnionRect(pRect, &rc[0], &rc[1]);
	}else{
		RECT rcTemp;
		::UnionRect(&rcTemp, &rc[0], &rc[1]);
		::UnionRect(pRect, &rcTemp, &rc[2]);
	}
}

/**キャプション矩形取得（スクリーン座標）
	@date 2010.06.05 ryoji 新規作成
*/
void CDlgFuncList::GetCaptionRect( LPRECT pRect )
{
	RECT rc;
	::GetWindowRect( GetHwnd(), &rc );
	EDockSide eDockSide = GetDockSide();
	pRect->left = rc.left + ((eDockSide == DOCKSIDE_RIGHT)? DOCK_SPLITTER_WIDTH: 0);
	pRect->top = rc.top + ((eDockSide == DOCKSIDE_BOTTOM)? DOCK_SPLITTER_WIDTH: 0);
	pRect->right = rc.right - ((eDockSide == DOCKSIDE_LEFT)? DOCK_SPLITTER_WIDTH: 0);
	pRect->bottom = pRect->top + (::GetSystemMetrics( SM_CYSMCAPTION ) + 1);
}

/** キャプション上のボタン矩形取得（スクリーン座標）
	@date 2010.06.05 ryoji 新規作成
*/
bool CDlgFuncList::GetCaptionButtonRect( int nButton, LPRECT pRect )
{
	if( !IsDocking() )
		return false;
	if( nButton >= DOCK_BUTTON_NUM )
		return false;
	GetCaptionRect( pRect );
	::OffsetRect( pRect, 0, 1 );
	int cx = ::GetSystemMetrics( SM_CXSMSIZE );
	pRect->left = pRect->right - cx * (nButton + 1);
	pRect->right = pRect->left + cx;
	pRect->bottom = pRect->top + ::GetSystemMetrics( SM_CYSMSIZE );
	return true;
}

/** 分割バーへのヒットテスト（スクリーン座標）
	@date 2010.06.05 ryoji 新規作成
*/
bool CDlgFuncList::HitTestSplitter( int xPos, int yPos )
{
	if( !IsDocking() )
		return false;

	bool bRet = false;
	RECT rc;
	::GetWindowRect(GetHwnd(), &rc);

	EDockSide eDockSide = GetDockSide();
	switch( eDockSide ){
	case DOCKSIDE_LEFT:		bRet = (rc.right - xPos < DOCK_SPLITTER_WIDTH);		break;
	case DOCKSIDE_TOP:		bRet = (rc.bottom - yPos < DOCK_SPLITTER_WIDTH);	break;
	case DOCKSIDE_RIGHT:	bRet = (xPos - rc.left< DOCK_SPLITTER_WIDTH);		break;
	case DOCKSIDE_BOTTOM:	bRet = (yPos - rc.top < DOCK_SPLITTER_WIDTH);		break;
	default:
		break;
	}

	return bRet;
}

/** キャプション上のボタンへのヒットテスト（スクリーン座標）
	@date 2010.06.05 ryoji 新規作成
*/
int CDlgFuncList::HitTestCaptionButton( int xPos, int yPos )
{
	if( !IsDocking() )
		return -1;

	POINT pt;
	pt.x = xPos;
	pt.y = yPos;

	RECT rcBtn;
	GetCaptionRect( &rcBtn );
	::OffsetRect( &rcBtn, 0, 1 );
	rcBtn.left = rcBtn.right - ::GetSystemMetrics( SM_CXSMSIZE );
	rcBtn.bottom = rcBtn.top + ::GetSystemMetrics( SM_CYSMSIZE );
	int nBtn = -1;
	for( int i = 0; i < DOCK_BUTTON_NUM; i++ ){
		if( ::PtInRect( &rcBtn, pt ) ){
			nBtn = i;	// 右端から i 番目のボタン上
			break;
		}
		::OffsetRect( &rcBtn, -(rcBtn.right - rcBtn.left), 0 );
	}

	return nBtn;
}

/** WM_NCCALCSIZE 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnNcCalcSize( [[maybe_unused]] HWND hwnd, [[maybe_unused]] UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam )
{
	if( !IsDocking() )
		return 0L;

	// 自ウィンドウのクライアント領域を定義する
	// これでキャプションや分割バーを非クライアント領域にすることができる
	NCCALCSIZE_PARAMS* pNCS = (NCCALCSIZE_PARAMS*)lParam;
	pNCS->rgrc[0].top += (::GetSystemMetrics( SM_CYSMCAPTION ) + 1);
	switch( GetDockSide() ){
	case DOCKSIDE_LEFT:		pNCS->rgrc[0].right -= DOCK_SPLITTER_WIDTH;		break;
	case DOCKSIDE_TOP:		pNCS->rgrc[0].bottom -= DOCK_SPLITTER_WIDTH;	break;
	case DOCKSIDE_RIGHT:	pNCS->rgrc[0].left += DOCK_SPLITTER_WIDTH;		break;
	case DOCKSIDE_BOTTOM:	pNCS->rgrc[0].top += DOCK_SPLITTER_WIDTH;		break;
	default:
		break;
	}
	return 1L;
}

/** WM_NCHITTEST 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnNcHitTest( [[maybe_unused]] HWND hwnd, [[maybe_unused]] UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam )
{
	if( !IsDocking() )
		return 0L;

	INT_PTR nRet = HTERROR;
	POINT pt;
	pt.x = MAKEPOINTS(lParam).x;
	pt.y = MAKEPOINTS(lParam).y;
	if( HitTestSplitter(pt.x, pt.y) ){
		switch( GetDockSide() ){
		case DOCKSIDE_LEFT:		nRet = HTRIGHT;		break;
		case DOCKSIDE_TOP:		nRet = HTBOTTOM;	break;
		case DOCKSIDE_RIGHT:	nRet = HTLEFT;		break;
		case DOCKSIDE_BOTTOM:	nRet = HTTOP;		break;
		default:
			break;
		}
	}else {
		RECT rc;
		GetCaptionRect( &rc );
		nRet = ::PtInRect( &rc, pt )? HTCAPTION: HTCLIENT;
	}
	::SetWindowLongPtr( GetHwnd(), DWLP_MSGRESULT, nRet );

	return nRet;
}

/** WM_TIMER 処理
	@date 2010.06.05 ryoji 新規作成
*/
BOOL CDlgFuncList::OnTimer( HWND hwnd, [[maybe_unused]] UINT uMsg, WPARAM wParam, [[maybe_unused]] LPARAM lParam )
{
	const auto workbenchTimerToken = WorkbenchRefreshTimerToken(static_cast<UINT_PTR>(wParam));
	if( workbenchTimerToken != 0 ) {
		const auto currentVersion = GetWorkbenchDocumentVersion();
		(void)m_workbenchRefreshScheduler.ConsumeTimer(
			workbenchTimerToken,
			IsWorkbenchMode() && IsAsyncWorkbenchOutlineType(m_nOutlineType),
			GetHwnd() != nullptr,
			currentVersion,
			[hwnd](std::uint64_t token) {
				const UINT_PTR timerId = WorkbenchRefreshTimerId(token);
				if( timerId != 0 ) ::KillTimer(hwnd, timerId);
			},
			[this] {
				// The timer is the only path that captures a burst of edit notifications.
				// RequestWorkbenchOutline revalidates the current document and arms the
				// notification gate before submitting the immutable snapshot.
				(void)RequestWorkbenchOutline(m_nOutlineType, true);
			});
		return FALSE;
	}else if( wParam == 2 ){
		CEditView* pcView = reinterpret_cast<CEditView*>(m_lParam);
		if( m_pszTimerJumpFile ){
			const WCHAR* pszFile = m_pszTimerJumpFile;
			m_pszTimerJumpFile = nullptr;
			bool bSelf = false;
			pcView->TagJumpSub( pszFile, m_pointTimerJump, false, false, &bSelf );
			if( m_bTimerJumpAutoClose ){
				if( UsesCompactPanelLayout() ){
					if( bSelf ){
						::PostMessageAny( pcView->GetHwnd(), MYWM_SETACTIVEPANE, 0, 0 );
					}
				}
				else if( m_pShareData->m_Common.m_sOutline.m_bAutoCloseDlgFuncList ){
					::DestroyWindow( GetHwnd() );
				}
				else if( m_pShareData->m_Common.m_sOutline.m_bFunclistSetFocusOnJump ){
					if( bSelf ){
						::SetFocus( pcView->GetHwnd() );
					}
				}
			}
		}
		::KillTimer(hwnd, 2);
		return FALSE;
	}else if( wParam == 3 ){
		::KillTimer(hwnd, 3);
		HWND hwndTree = ::GetDlgItem(hwnd, IDC_TREE_FL);
		ApiWrap::TreeView_ExpandAll(hwndTree, true, 64);
	}else  if( wParam == 4 ){
		::KillTimer(hwnd, 4);
		HWND hwndTree = ::GetDlgItem(hwnd, IDC_TREE_FL);
		ApiWrap::TreeView_ExpandAll(hwndTree, false, 64);
	}

	if( !IsDocking() )
		return FALSE;

	if( wParam == 1 ){
		// カーソルがウィンドウ外にある場合にも WM_NCMOUSEMOVE を送る
		POINT pt;
		RECT rc;
		::GetCursorPos( &pt );
		::GetWindowRect( hwnd, &rc );
		if( !::PtInRect( &rc, pt ) ){
			::SendMessageAny( hwnd, WM_NCMOUSEMOVE, 0, MAKELONG( pt.x, pt.y ) );
		}
	}

	return FALSE;
}

/** WM_NCMOUSEMOVE 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnNcMouseMove( HWND hwnd, [[maybe_unused]] UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam )
{
	if( !IsDocking() )
		return 0L;

	POINT pt;
	pt.x = MAKEPOINTS(lParam).x;
	pt.y = MAKEPOINTS(lParam).y;

	// カーソルがウィンドウ内に入ったらタイマー起動
	// ウィンドウ外に出たらタイマー削除
	RECT rc;
	::GetWindowRect( GetHwnd(), &rc );
	bool bHovering = ::PtInRect( &rc, pt )? true: false;
	if( bHovering != m_bHovering )
	{
		m_bHovering = bHovering;
		if( m_bHovering )
			::SetTimer( hwnd, 1, 200, nullptr );
		else
			::KillTimer( hwnd, 1 );
	}

	// マウスカーソルがボタン上にあればハイライト
	int nHilightedBtn = HitTestCaptionButton(pt.x, pt.y);
	if( nHilightedBtn != m_nHilightedBtn ){
		// ハイライト状態の変更を反映するために再描画する
		m_nHilightedBtn = nHilightedBtn;
		::RedrawWindow( GetHwnd(), nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOINTERNALPAINT );

		// ツールチップ更新
		TOOLINFO ti;
		::ZeroMemory( &ti, sizeof(ti) );
		ti.cbSize       = CCSIZEOF_STRUCT(TOOLINFO, lpszText);
		ti.hwnd         = GetHwnd();
		ti.hinst        = m_hInstance;
		ti.uId          = (UINT_PTR)GetHwnd();
		switch( m_nHilightedBtn ){
		case 0: ti.lpszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_TIP_CLOSE)); break;
		case 1: ti.lpszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_TIP_WIN)); break;
		case 2: ti.lpszText = const_cast<WCHAR*>(LS(STR_DLGFNCLST_TIP_UPDATE)); break;
		default: ti.lpszText = nullptr;	// 消す
		}
		ApiWrap::Tooltip_UpdateTipText( m_hwndToolTip, &ti );
	}

	return 0L;
}

/** WM_MOUSEMOVE 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnMouseMove( [[maybe_unused]] HWND hwnd, [[maybe_unused]] UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam )
{
	if( !IsDocking() )
		return 0L;

	if( m_bStretching ){	// マウスのドラッグ位置にあわせてサイズを変更する
		POINT pt;
		pt.x = MAKEPOINTS(lParam).x;
		pt.y = MAKEPOINTS(lParam).y;
		::ClientToScreen( GetHwnd(), &pt );

		RECT rc;
		GetDockSpaceRect(&rc);

		// 画面サイズが小さすぎるときは何もしない
		EDockSide eDockSide = GetDockSide();
		if( eDockSide == DOCKSIDE_LEFT || eDockSide == DOCKSIDE_RIGHT ){
			if( rc.right - rc.left < DOCK_MIN_SIZE )
				return 0L;
		}else{
			if( rc.bottom - rc.top < DOCK_MIN_SIZE )
				return 0L;
		}

		// マウスが上下左右に行き過ぎなら範囲内に調整する
		if( pt.x > rc.right - DOCK_MIN_SIZE ) pt.x = rc.right - DOCK_MIN_SIZE;
		if( pt.x < rc.left + DOCK_MIN_SIZE ) pt.x = rc.left + DOCK_MIN_SIZE;
		if( pt.y > rc.bottom - DOCK_MIN_SIZE ) pt.y = rc.bottom - DOCK_MIN_SIZE;
		if( pt.y < rc.top + DOCK_MIN_SIZE ) pt.y = rc.top + DOCK_MIN_SIZE;

		// クライアント座標系に変換して新しい位置とサイズを計算する
		POINT ptLT;
		ptLT.x = rc.left;
		ptLT.y = rc.top;
		::ScreenToClient( m_hwndParent, &ptLT );
		::OffsetRect( &rc, ptLT.x - rc.left, ptLT.y - rc.top );
		::ScreenToClient( m_hwndParent, &pt );
		switch( eDockSide ){
		case DOCKSIDE_LEFT:		rc.right = pt.x - DOCK_SPLITTER_WIDTH / 2 + DOCK_SPLITTER_WIDTH;	break;
		case DOCKSIDE_TOP:		rc.bottom = pt.y - DOCK_SPLITTER_WIDTH / 2 + DOCK_SPLITTER_WIDTH;	break;
		case DOCKSIDE_RIGHT:	rc.left = pt.x - DOCK_SPLITTER_WIDTH / 2;	break;
		case DOCKSIDE_BOTTOM:	rc.top = pt.y - DOCK_SPLITTER_WIDTH / 2;	break;
		default:
			break;
		}

		// 以前と同じ配置なら無駄に移動しない
		RECT rcOld;
		::GetWindowRect( GetHwnd(), &rcOld );
		ptLT.x = rcOld.left;
		ptLT.y = rcOld.top;
		::ScreenToClient( m_hwndParent, &ptLT );
		::OffsetRect( &rcOld, ptLT.x - rcOld.left, ptLT.y - rcOld.top );
		if( ::EqualRect( &rcOld, &rc ) )
			return 0L;

		// 移動する
		::SetWindowPos( GetHwnd(), nullptr,
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
			SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOACTIVATE );
		GetEditWnd().EndLayoutBars( m_bEditWndReady );

		// 移動後の配置情報を記憶する
		GetWindowRect( GetHwnd(), &rc );
		bool bType = (ProfDockSet() != 0);
		if( bType ){
			CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
		}
		switch( GetDockSide() ){
		case DOCKSIDE_LEFT:		ProfDockLeft() = rc.right - rc.left;	break;
		case DOCKSIDE_TOP:		ProfDockTop() = rc.bottom - rc.top;		break;
		case DOCKSIDE_RIGHT:	ProfDockRight() = rc.right - rc.left;	break;
		case DOCKSIDE_BOTTOM:	ProfDockBottom() = rc.bottom - rc.top;	break;
		default:
			break;
		}
		if( bType ){
			SetTypeConfig(CTypeConfig(m_nDocType), m_type);
		}
		return 1L;
	}

	return 0L;
}

/** WM_NCLBUTTONDOWN 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnNcLButtonDown( [[maybe_unused]] HWND hwnd, [[maybe_unused]] UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	POINT pt;
	pt.x = MAKEPOINTS(lParam).x;
	pt.y = MAKEPOINTS(lParam).y;

	if( !IsDocking() ){
		if( GetDockSide() == DOCKSIDE_FLOAT ){
			if( wParam == HTCAPTION  && !::IsZoomed(GetHwnd()) && !::IsIconic(GetHwnd()) ){
				::SetActiveWindow( GetHwnd() );
				// 上の SetActiveWindow() で WM_ACTIVATEAPP へ行くケースでは、WM_ACTIVATEAPP に入れた特殊処理（エディタ本体を一時的にアクティブ化して戻す）
				// に余計に時間がかかるため、上の SetActiveWindow() 後にはボタンが離されていることがある。その場合は Track() を開始せずに抜ける。
				if( (::GetAsyncKeyState( ::GetSystemMetrics(SM_SWAPBUTTON)? VK_RBUTTON: VK_LBUTTON ) & 0x8000) == 0 )
					return 1L;	// ボタンは既に離されている
				Track( pt );	// タイトルバーのドラッグ＆ドロップによるドッキング配置変更
				return 1L;
			}
		}
		return 0L;
	}

	int nBtn;
	if( HitTestSplitter(pt.x, pt.y) ){	// 分割バー
		m_bStretching = true;
		::SetCapture( GetHwnd() );	// OnMouseMoveでのサイズ制限のために自前のキャプチャが必要
	}else{
		if( (nBtn = HitTestCaptionButton(pt.x, pt.y)) >= 0 ){	// キャプション上のボタン
			if( nBtn == 1 ){	// メニュー
				RECT rcBtn;
				GetCaptionButtonRect( nBtn, &rcBtn );
				pt.x = rcBtn.left;
				pt.y = rcBtn.bottom;
				DoMenu( pt, GetHwnd() );
				// メニュー選択せずにリストやツリーをクリックしたらボタンがハイライトのままになるので更新
				::RedrawWindow( GetHwnd(), nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOINTERNALPAINT );
			}else{
				m_nCapturingBtn = nBtn;
				::SetCapture( GetHwnd() );
			}
		}else{	// 残りはタイトルバーのみ
			Track( pt );	// タイトルバーのドラッグ＆ドロップによるドッキング配置変更
		}
	}

	return 1L;
}

/** WM_LBUTTONUP 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnLButtonUp( [[maybe_unused]] HWND hwnd, [[maybe_unused]] UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam )
{
	if( !IsDocking() )
		return 0L;

	if( m_bStretching ){
		::ReleaseCapture();
		m_bStretching = false;

		if( ProfDockSync() ){
			// 他ウィンドウに変更を通知する
			HWND hwndEdit = GetEditWnd().GetHwnd();
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );
		}
		return 1L;
	}

	if( m_nCapturingBtn >= 0 ){
		::ReleaseCapture();
		POINT pt;
		pt.x = MAKEPOINTS(lParam).x;
		pt.y = MAKEPOINTS(lParam).y;
		::ClientToScreen( GetHwnd(), &pt );
		int nBtn = HitTestCaptionButton( pt.x, pt.y);
		if( nBtn == m_nCapturingBtn ){
			if( nBtn == 0 ){	// 閉じる
				::DestroyWindow( GetHwnd() );
			}else if( m_nCapturingBtn == 2 ){	// 更新
				EFunctionCode nFuncCode = GetFuncCodeRedraw(m_nOutlineType);
				CEditView* pcEditView = (CEditView*)m_lParam;
				pcEditView->GetCommander().HandleCommand( nFuncCode, true, SHOW_RELOAD, 0, 0, 0 );
			}
		}
		m_nCapturingBtn = -1;
		return 1L;
	}

	return 0L;
}

/** WM_NCPAINT 処理
	@date 2010.06.05 ryoji 新規作成
*/
INT_PTR CDlgFuncList::OnNcPaint( HWND hwnd, [[maybe_unused]] UINT uMsg, [[maybe_unused]] WPARAM wParam, [[maybe_unused]] LPARAM lParam )
{
	if( !IsDocking() )
		return 0L;

	EDockSide eDockSide = GetDockSide();

	HDC hdc;
	RECT rc, rcScr, rcWk;

	//描画対象
	hdc = ::GetWindowDC( hwnd );
	CGraphics gr(hdc);
	::GetWindowRect( hwnd, &rcScr );
	rc = rcScr;
	::OffsetRect( &rc, -rcScr.left, -rcScr.top );

	// 分割線を描画する
	rcWk = rc;
	switch( eDockSide ){
	case DOCKSIDE_LEFT:		rcWk.left = rcWk.right - DOCK_SPLITTER_WIDTH; break;
	case DOCKSIDE_TOP:		rcWk.top = rcWk.bottom - DOCK_SPLITTER_WIDTH; break;
	case DOCKSIDE_RIGHT:	rcWk.right = rcWk.left + DOCK_SPLITTER_WIDTH; break;
	case DOCKSIDE_BOTTOM:	rcWk.bottom = rcWk.top + DOCK_SPLITTER_WIDTH; break;
	default:
		break;
	}
	//::MyFillRect( gr, rcWk, COLOR_3DFACE );
	::MyFillRect(gr, rcWk, DarkMode::getBackgroundColor());
	::DrawEdge( gr, &rcWk, EDGE_ETCHED, BF_TOPLEFT );

	// タイトルを描画する
	BOOL bThemeActive = ::IsThemeActive();
	HWND hwndFocus = ::GetFocus();
	BOOL bActive = (GetHwnd() == hwndFocus || ::IsChild(GetHwnd(), hwndFocus));
	RECT rcCaption;
	GetCaptionRect( &rcCaption );
	::OffsetRect( &rcCaption, -rcScr.left, -rcScr.top );
	rcWk = rcCaption;
	// ↓DrawCaption() に DC_SMALLCAP を指定してはいけないっぽい
	// ↓DC_SMALLCAP 指定のものを Win7(64bit版) で動かしてみたら描画位置が下にずれて上半分しか見えなかった（x86ビルド/x64ビルドのどちらも NG）
	gr.SetTextForeColor(DarkMode::getTextColor());
	gr.SetTextBackColor(DarkMode::getBackgroundColor());
	wchar_t buff[256];
	::GetWindowText(GetHwnd(), buff, 256);
	COLORREF clrCaption = bActive ? DarkMode::getHotBackgroundColor() : DarkMode::getBackgroundColor();
	::MyFillRect( gr, rcWk, clrCaption );
	::DrawEdge( gr, &rcCaption, BDR_SUNKENOUTER, BF_TOP );
	::DrawText(gr, buff, -1, &rcCaption, DT_TOP|DT_LEFT);

	// タイトル上のボタンを描画する
	NONCLIENTMETRICS ncm;
	ncm.cbSize = CCSIZEOF_STRUCT( NONCLIENTMETRICS, lfMessageFont );	// 以前のプラットフォームに WINVER >= 0x0600 で定義される構造体のフルサイズを渡すと失敗する
	::SystemParametersInfo( SPI_GETNONCLIENTMETRICS, ncm.cbSize, (PVOID)&ncm, 0 );
	LOGFONT lf;
	memset( &lf, 0, sizeof(LOGFONT) );
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfHeight = ncm.lfCaptionFont.lfHeight;
	::lstrcpy( lf.lfFaceName, L"Marlett" );
	HFONT hFont = ::CreateFontIndirect( &lf );
	::lstrcpy( lf.lfFaceName, L"Webdings" );
	HFONT hFont2 = ::CreateFontIndirect( &lf );
	gr.SetTextBackTransparent( true );

	static const WCHAR szBtn[DOCK_BUTTON_NUM] = { (WCHAR)0x72/* 閉じる */, (WCHAR)0x36/* メニュー */, (WCHAR)0x71/* 更新 */ };
	HFONT hFontBtn[DOCK_BUTTON_NUM] = { hFont/* 閉じる */, hFont/* メニュー */, hFont2/* 更新 */ };
	POINT pt;
	::GetCursorPos( &pt );
	pt.x -= rcScr.left;
	pt.y -= rcScr.top;
	RECT rcBtn = rcCaption;
	::OffsetRect( &rcBtn, 0, 1 );
	rcBtn.left = rcBtn.right - ::GetSystemMetrics( SM_CXSMSIZE );
	rcBtn.bottom = rcBtn.top + ::GetSystemMetrics( SM_CYSMSIZE );
	for( int i = 0; i < DOCK_BUTTON_NUM; i++ ){
		int nClrCaptionText;
		// マウスカーソルがボタン上にあればハイライト
		if( ::PtInRect( &rcBtn, pt ) ){
			::MyFillRect( gr, rcBtn, COLOR_ACTIVECAPTION );
			nClrCaptionText = COLOR_CAPTIONTEXT;
		}else{
			nClrCaptionText = ( bActive? COLOR_CAPTIONTEXT: COLOR_INACTIVECAPTIONTEXT );
		}
		gr.PushMyFont( hFontBtn[i] );
		::SetTextColor( gr, ::GetSysColor( nClrCaptionText ) );
		::DrawText( gr, &szBtn[i], 1, &rcBtn, DT_SINGLELINE | DT_CENTER | DT_VCENTER );
		::OffsetRect( &rcBtn, -(rcBtn.right - rcBtn.left), 0 );
		gr.PopMyFont();
	}

	::DeleteObject( hFont );
	::DeleteObject( hFont2 );

	::ReleaseDC( hwnd, hdc );
	return 1L;
}

/** メニュー処理
	@date 2010.06.05 ryoji 新規作成
*/
void CDlgFuncList::DoMenu( POINT pt, HWND hwndFrom )
{
	// メニューを作成する
	CEditView* pcEditView = &GetEditWnd().GetActiveView();
	CDocTypeManager().GetTypeConfig( CTypeConfig(m_nDocType), m_type );
	EDockSide eDockSide = ProfDockSide();	// 設定上の配置
	UINT uFlags = MF_BYPOSITION | MF_STRING;
	const bool bDropDown = (hwndFrom == GetHwnd()); // true=ドロップダウン, false=右クリック
	const bool bWorkbench = IsWorkbenchMode();
	HMENU hMenu = ::CreatePopupMenu();
	HMENU hMenuSub = (bDropDown || bWorkbench) ? nullptr : ::CreatePopupMenu();
	int iPos = 0;

	if( bDropDown == false ){
		// 将来、ここに hwndFrom に応じた状況依存メニューを追加するといいかも
		// （ツリーなら「すべて展開」／「すべて縮小」とか、そういうの）
		::InsertMenu( hMenu, iPos++, MF_BYPOSITION | MF_STRING, 450, LS(STR_DLGFNCLST_MENU_UPDATE) );
		int flag = 0;
		if( FALSE == ::IsWindowEnabled( GetItemHwnd(IDC_BUTTON_COPY) ) ){
			flag |= MF_GRAYED;
		}
		::InsertMenu( hMenu, iPos++, MF_BYPOSITION | MF_STRING | flag, 451, LS(STR_DLGFNCLST_MENU_COPY) );
		if( m_nViewType == VIEWTYPE_TREE ){
			::InsertMenu(hMenu, iPos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
			::InsertMenu(hMenu, iPos++, MF_BYPOSITION | MF_STRING, 500, LS(STR_DLGFNCLST_MENU_EXPAND));
			::InsertMenu(hMenu, iPos++, MF_BYPOSITION | MF_STRING, 501, LS(STR_DLGFNCLST_MENU_COLLAPSE));
		}else if( m_nListType == OUTLINE_BOOKMARK ){
			::InsertMenu(hMenu, iPos++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
			flag = 0;
			HWND hwndList = GetItemHwnd(IDC_LIST_FL);
			if( ListView_GetSelectedCount(hwndList) == 0 ){
				flag |= MF_GRAYED;
			}
			::InsertMenu(hMenu, iPos++, MF_BYPOSITION | MF_STRING | flag, 510, LS(STR_DLGFNCLST_MENU_BOOK_DEL));
			::InsertMenu(hMenu, iPos++, MF_BYPOSITION | MF_STRING, 511, LS(STR_DLGFNCLST_MENU_BOOK_ALL_DEL));
		}
		if( !bWorkbench ){
			::InsertMenu( hMenu, iPos++, MF_BYPOSITION | MF_SEPARATOR, 0,	nullptr );
			::InsertMenu( hMenu, iPos++, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)hMenuSub,	LS(STR_DLGFNCLST_MENU_WINPOS) );
		}
	}

	if( !bWorkbench ){
		int iPosSub = 0;
		HMENU& hMenuRef = bDropDown ? hMenu : hMenuSub;
		int& iPosRef = bDropDown ? iPos : iPosSub;
		int iFrom = iPosRef;
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 100 + DOCKSIDE_LEFT,       LS(STR_DLGFNCLST_MENU_LEFTDOC) );
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 100 + DOCKSIDE_RIGHT,      LS(STR_DLGFNCLST_MENU_RIGHTDOC) );
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 100 + DOCKSIDE_TOP,        LS(STR_DLGFNCLST_MENU_TOPDOC) );
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 100 + DOCKSIDE_BOTTOM,     LS(STR_DLGFNCLST_MENU_BOTDOC) );
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 100 + DOCKSIDE_FLOAT,      LS(STR_DLGFNCLST_MENU_FLOATING) );
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 100 + DOCKSIDE_UNDOCKABLE, LS(STR_DLGFNCLST_MENU_NODOCK) );
		int iTo = iPosRef - 1;
		for( int i = iFrom; i <= iTo; i++ ){
			if( static_cast<EDockSide>(::GetMenuItemID(hMenuRef, i)) == (100 + eDockSide) ){
				::CheckMenuRadioItem( hMenuRef, iFrom, iTo, i, MF_BYPOSITION );
				break;
			}
		}
		::InsertMenu( hMenuRef, iPosRef++, MF_BYPOSITION | MF_SEPARATOR, 0,	nullptr );
		::InsertMenu( hMenuRef, iPosRef++, uFlags, 200, LS(STR_DLGFNCLST_MENU_SYNC) );
		::CheckMenuItem( hMenuRef, 200, MF_BYCOMMAND | ProfDockSync()? MF_CHECKED: MF_UNCHECKED );
		::InsertMenu( hMenuRef, iPosRef++, MF_BYPOSITION | MF_SEPARATOR, 0,	nullptr );
		::InsertMenu( hMenuRef, iPosRef++, MF_BYPOSITION | MF_STRING, 300, LS(STR_DLGFNCLST_MENU_INHERIT) );
		::InsertMenu( hMenuRef, iPosRef++, MF_BYPOSITION | MF_STRING, 301, LS(STR_DLGFNCLST_MENU_TYPE) );
		::CheckMenuRadioItem( hMenuRef, 300, 301, (ProfDockSet() == 0)? 300: 301, MF_BYCOMMAND );
		::InsertMenu( hMenuRef, iPosRef++, MF_BYPOSITION | MF_SEPARATOR, 0,	nullptr );
		::InsertMenu( hMenuRef, iPosRef++, MF_BYPOSITION | MF_STRING, 305, LS(STR_DLGFNCLST_MENU_UNIFY) );
	}

	if( bDropDown == false && !bWorkbench ){
		::InsertMenu( hMenu, iPos++, MF_BYPOSITION | MF_SEPARATOR, 0,	nullptr );
		::InsertMenu( hMenu, iPos++, MF_BYPOSITION | MF_STRING, 452, LS(STR_DLGFNCLST_MENU_CLOSE) );
	}

	// メニューを表示する
	RECT rcWork;
	GetMonitorWorkRect( pt, &rcWork );	// モニタのワークエリア
	int nId = ::TrackPopupMenu( hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD,
								( pt.x > rcWork.left )? pt.x: rcWork.left,
								( pt.y < rcWork.bottom )? pt.y: rcWork.bottom,
								0, GetHwnd(), nullptr);
	::DestroyMenu( hMenu );	// サブメニューは再帰的に破棄される

	// メニュー選択された状態に切り替える
	EFunctionCode nFuncCode = GetFuncCodeRedraw(m_nOutlineType);
	HWND hwndEdit = GetEditWnd().GetHwnd();
	if( nId == 450 ){	// 更新
		pcEditView->GetCommander().HandleCommand( nFuncCode, true, SHOW_RELOAD, 0, 0, 0 );
	}
	else if( nId == 451 ){	// コピー
		// Windowsクリップボードにコピー 
		if( IsWorkbenchMode() && (IsAsyncWorkbenchOutlineType(m_nListType) || m_workbenchClipboardUsesGenericTree) ) BuildWorkbenchClipboardText();
		SetClipboardText( GetHwnd(), m_cmemClipText.GetStringPtr(), m_cmemClipText.GetStringLength() );
	}
	else if( nId == 452 ){	// 閉じる
		::DestroyWindow( GetHwnd() );
	}else if( nId == 500 ){	// すべて展開
		::SetTimer(GetHwnd(), 3, 100, nullptr);
	}else if( nId == 501 ){	// すべて縮小
		::SetTimer(GetHwnd(), 4, 100, nullptr);
	}else if( nId == 510 ){	// ブックマーク削除
		HWND hwndList = GetItemHwnd(IDC_LIST_FL);
		int nItem = ListView_GetNextItem(hwndList, -1, LVNI_ALL | LVNI_SELECTED);
		if( nItem != -1 ){
			LVITEM item;
			item.mask = LVIF_PARAM;
			item.iItem = nItem;
			item.iSubItem = 0;
			ListView_GetItem(hwndList, &item);
			const CFuncInfo* pFuncInfo = m_pcFuncInfoArr->GetAt(item.lParam);
			// FIXME: 行番号があってるとは限らない
			CDocLine* pCDocLine = pcEditView->GetDocument()->m_cDocLineMgr.GetLine(pFuncInfo->m_nFuncLineCRLF - 1);
			if( pCDocLine ){
				CBookmarkSetter cBookmark(pCDocLine);
				cBookmark.SetBookmark(false);
				GetEditWnd().Views_Redraw();
			}
		}
		pcEditView->GetCommander().HandleCommand(nFuncCode, true, SHOW_RELOAD, 0, 0, 0);
	}else if( nId == 511 ){	// ブックマークすべて削除
		pcEditView->GetCommander().HandleCommand(F_BOOKMARK_RESET, TRUE, 0, 0, 0, 0);
		pcEditView->GetCommander().HandleCommand(nFuncCode, true, SHOW_RELOAD, 0, 0, 0);
	}
	else if( nId == 300 || nId == 301 ){	// ドッキング配置の継承方法
		ProfDockSet() = nId - 300;
		ChangeLayout( OUTLINE_LAYOUT_FOREGROUND );	// 自分自身への強制変更
		if( ProfDockSync() ){
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );	// 他ウィンドウにドッキング配置変更を通知する
		}
	}
	else if( nId == 305 ){	// 設定コピー
		if( IDOK == ::MYMESSAGEBOX( hwndEdit,
						MB_OKCANCEL | MB_ICONINFORMATION, GSTR_APPNAME,
						LS(STR_DLGFNCLST_UNIFY) ) ){
			CommonSet().m_bOutlineDockDisp = GetHwnd()? TRUE: FALSE;
			CommonSet().m_eOutlineDockSide = GetDockSide();
			if( GetHwnd() ){
				RECT rc;
				GetWindowRect( GetHwnd(), &rc );
				switch( GetDockSide() ){	// 現在のドッキングモード
					case DOCKSIDE_LEFT:		CommonSet().m_cxOutlineDockLeft = rc.right - rc.left;	break;
					case DOCKSIDE_TOP:		CommonSet().m_cyOutlineDockTop = rc.bottom - rc.top;	break;
					case DOCKSIDE_RIGHT:	CommonSet().m_cxOutlineDockRight = rc.right - rc.left;	break;
					case DOCKSIDE_BOTTOM:	CommonSet().m_cyOutlineDockBottom = rc.bottom - rc.top;	break;
					default:
						break;
				}
			}
			STypeConfig* type = new STypeConfig();
			for( int i = 0; i < GetDllShareData().m_nTypesCount; i++ ){
				CDocTypeManager().GetTypeConfig( CTypeConfig(i), *type );
				type->m_bOutlineDockDisp = CommonSet().m_bOutlineDockDisp;
				type->m_eOutlineDockSide = CommonSet().m_eOutlineDockSide;
				type->m_cxOutlineDockLeft = CommonSet().m_cxOutlineDockLeft;
				type->m_cyOutlineDockTop = CommonSet().m_cyOutlineDockTop;
				type->m_cxOutlineDockRight = CommonSet().m_cxOutlineDockRight;
				type->m_cyOutlineDockBottom = CommonSet().m_cyOutlineDockBottom;
				CDocTypeManager().SetTypeConfig( CTypeConfig(i), *type );
			}
			delete type;
			ChangeLayout( OUTLINE_LAYOUT_FOREGROUND );	// 自分自身への強制変更
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );	// 他ウィンドウにドッキング配置変更を通知する
		}
	}
	else if( nId == 200 ){	// ドッキング配置の同期をとる
		ProfDockSync() = !ProfDockSync();
		ChangeLayout( OUTLINE_LAYOUT_FOREGROUND );	// 自分自身への強制変更
		if( ProfDockSync() ){
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );	// 他ウィンドウにドッキング配置変更を通知する
		}
	}
	else if( nId >= 100 - 1 ){	// ドッキングモード （※ DOCKSIDE_UNDOCKABLE は -1 です） */
		int* pnWidth = nullptr;
		int* pnHeight = nullptr;
		RECT rc;
		GetDockSpaceRect( &rc );
		eDockSide = EDockSide(nId - 100);	// 新しいドッキングモード
		bool bType = (ProfDockSet() != 0);
		if( bType ){
			CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
		}
		if( eDockSide > DOCKSIDE_FLOAT ){
			switch( eDockSide ){
			case DOCKSIDE_LEFT:		pnWidth = &ProfDockLeft();		break;
			case DOCKSIDE_TOP:		pnHeight = &ProfDockTop();		break;
			case DOCKSIDE_RIGHT:	pnWidth = &ProfDockRight();		break;
			case DOCKSIDE_BOTTOM:	pnHeight = &ProfDockBottom();	break;
			default:
				break;
			}
			if( eDockSide == DOCKSIDE_LEFT || eDockSide == DOCKSIDE_RIGHT ){
				if( *pnWidth == 0 )	// 初回
					*pnWidth = (rc.right - rc.left) / 3;
				if( *pnWidth > rc.right - rc.left - DOCK_MIN_SIZE ) *pnWidth = rc.right - rc.left - DOCK_MIN_SIZE;
				if( *pnWidth < DOCK_MIN_SIZE ) *pnWidth = DOCK_MIN_SIZE;
			}else{
				if( *pnHeight == 0 )	// 初回
					*pnHeight = (rc.bottom - rc.top) / 3;
				if( *pnHeight > rc.bottom - rc.top - DOCK_MIN_SIZE ) *pnHeight = rc.bottom - rc.top - DOCK_MIN_SIZE;
				if( *pnHeight < DOCK_MIN_SIZE ) *pnHeight = DOCK_MIN_SIZE;
			}
		}

		// ドッキング配置変更
		ProfDockDisp() = GetHwnd()? TRUE: FALSE;
		ProfDockSide() = eDockSide;	// 新しいドッキングモードを適用
		if( bType ){
			SetTypeConfig(CTypeConfig(m_nDocType), m_type);
		}
		ChangeLayout( OUTLINE_LAYOUT_FOREGROUND );	// 自分自身への強制変更
		if( ProfDockSync() ){
			PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)hwndEdit );	// 他ウィンドウにドッキング配置変更を通知する
		}
	}
}

/** 現在の設定に応じて表示を刷新する
	@date 2010.06.05 ryoji 新規作成
*/
void CDlgFuncList::Refresh( void )
{
	CEditWnd* pcEditWnd = &GetEditWnd();
	BOOL bReloaded = ChangeLayout( OUTLINE_LAYOUT_FILECHANGED );	// 現在設定に従ってアウトライン画面を再配置する
	if( !bReloaded && pcEditWnd->m_cDlgFuncList.GetHwnd() ){
		EOutlineType nOutlineType = GetOutlineTypeRedraw(m_nOutlineType);
		pcEditWnd->GetActiveView().GetCommander().Command_FUNCLIST( SHOW_RELOAD, nOutlineType );	// 開く	※ HandleCommand(F_OUTLINE,...) だと印刷プレビュー状態で実行されないので Command_FUNCLIST()
	}
	if( MyGetAncestor( ::GetForegroundWindow(), GA_ROOTOWNER2 ) == pcEditWnd->GetHwnd() )
		::SetFocus( pcEditWnd->GetActiveView().GetHwnd() );	// フォーカスを戻す
}

/** 現在の設定に応じて配置を変更する（できる限り再解析しない）

	@param nId [in] 動作指定．OUTLINE_LAYOUT_FOREGROUND: 前面用の動作 / OUTLINE_LAYOUT_BACKGROUND: 背後用の動作 / OUTLINE_LAYOUT_FILECHANGED: ファイル切替用の動作（前面だが特殊）
	@retval 解析を実行したかどうか．true: 実行した / false: 実行しなかった

	@date 2010.06.10 ryoji 新規作成
*/
bool CDlgFuncList::ChangeLayout( int nId )
{
	// Workbench geometry and visibility are owned by CWorkbenchPanelHost.  Legacy
	// outline notifications must never reserve editor space or move this child.
	if( IsWorkbenchMode() ) return false;

	struct SAutoSwitch
	{
		SAutoSwitch( bool* pbSwitch ): m_pbSwitch( pbSwitch ) { *m_pbSwitch = true; }
		SAutoSwitch(const SAutoSwitch&) = delete;
		SAutoSwitch& operator = (const SAutoSwitch&) = delete;
		SAutoSwitch(SAutoSwitch&&) noexcept = delete;
		SAutoSwitch& operator = (SAutoSwitch&&) noexcept = delete;
		~SAutoSwitch() { *m_pbSwitch = false; }
		bool* m_pbSwitch;
	} SAutoSwitch( &m_bInChangeLayout );	// 処理中は m_bInChangeLayout フラグを ON にしておく

	CEditDoc* pDoc = CEditDoc::GetInstance(0);	// 今は非表示かもしれないので (CEditView*)m_lParam は使えない
	m_nDocType = pDoc->m_cDocType.GetDocumentType().GetIndex();
	CDocTypeManager().GetTypeConfig( CTypeConfig(m_nDocType), m_type );

	BOOL bDockDisp = ProfDockDisp();
	EDockSide eDockSideNew = ProfDockSide();

	if( !GetHwnd() ){	// 現在は非表示
		if( bDockDisp ){	// 新設定は表示
			if( eDockSideNew <= DOCKSIDE_FLOAT ){
				if( nId == OUTLINE_LAYOUT_BACKGROUND ) return false;	// 裏ではフローティングは開かない（従来互換）※無理に開くとタブモード時は画面が切り替わってしまう
				if( nId == OUTLINE_LAYOUT_FILECHANGED ) return false;	// ファイル切替ではフローティングは開かない（従来互換）
			}
			// ※ 裏では一時的に Disable 化しておいて開く（タブモードでの不正な画面切り替え抑止）
			CEditView* pcEditView = &GetEditWnd().GetActiveView();
			if( nId == OUTLINE_LAYOUT_BACKGROUND ) ::EnableWindow( GetEditWnd().GetHwnd(), FALSE );
			if( m_nOutlineType == OUTLINE_DEFAULT ){
				bool bType = (ProfDockSet() != 0);
				if( bType ){
					m_nOutlineType = m_type.m_nDockOutline;
					SetTypeConfig( CTypeConfig(m_nDocType), m_type );
				}else{
					m_nOutlineType = CommonSet().m_nDockOutline;
				}
			}
			EOutlineType nOutlineType = GetOutlineTypeRedraw(m_nOutlineType);	// ブックマークかアウトライン解析かは最後に開いていた時の状態を引き継ぐ（初期状態はアウトライン解析）
			pcEditView->GetCommander().Command_FUNCLIST( SHOW_NORMAL, nOutlineType );	// 開く	※ HandleCommand(F_OUTLINE,...) だと印刷プレビュー状態で実行されないので Command_FUNCLIST()
			if( nId == OUTLINE_LAYOUT_BACKGROUND ) ::EnableWindow( GetEditWnd().GetHwnd(), TRUE );
			return true;	// 解析した
		}
	}else{	// 現在は表示
		EDockSide eDockSideOld = GetDockSide();

		CEditView* pcEditView = (CEditView*)m_lParam;
		if( !bDockDisp ){	// 新設定は非表示
			if( eDockSideOld <= DOCKSIDE_FLOAT ){	// 現在はフローティング
				if( nId == OUTLINE_LAYOUT_BACKGROUND ) return false;	// 裏ではフローティングは閉じない（従来互換）
				if( nId == OUTLINE_LAYOUT_FILECHANGED && eDockSideNew <= DOCKSIDE_FLOAT ) return false;	// ファイル切替では新設定もフローティングなら再利用（従来互換）
			}
			::DestroyWindow( GetHwnd() );	// 閉じる
			return false;
		}

		// ドッキング⇔フローティング切替では閉じて開く
		if( (eDockSideOld <= DOCKSIDE_FLOAT) != (eDockSideNew <= DOCKSIDE_FLOAT) ){
			::DestroyWindow( GetHwnd() );	// 閉じる
			if( eDockSideNew <= DOCKSIDE_FLOAT ){	// 新設定はフローティング
				m_xPos = m_yPos = -1;	// 画面位置を初期化する
				if( nId == OUTLINE_LAYOUT_BACKGROUND ) return false;	// 裏ではフローティングは開かない（従来互換）※無理に開くとタブモード時は画面が切り替わってしまう
				if( nId == OUTLINE_LAYOUT_FILECHANGED ) return false;	// ファイル切替ではフローティングは開かない（従来互換）
			}
			// ※ 裏では一時的に Disable 化しておいて開く（タブモードでの不正な画面切り替え抑止）
			if( nId == OUTLINE_LAYOUT_BACKGROUND ) ::EnableWindow( GetEditWnd().GetHwnd(), FALSE );
			if( m_nOutlineType == OUTLINE_DEFAULT ){
				bool bType = (ProfDockSet() != 0);
				if( bType ){
					m_nOutlineType = m_type.m_nDockOutline;
					SetTypeConfig( CTypeConfig(m_nDocType), m_type );
				}else{
					m_nOutlineType = CommonSet().m_nDockOutline;
				}
			}
			EOutlineType nOutlineType = GetOutlineTypeRedraw(m_nOutlineType);
			pcEditView->GetCommander().Command_FUNCLIST( SHOW_NORMAL, nOutlineType );	// 開く	※ HandleCommand(F_OUTLINE,...) だと印刷プレビュー状態で実行されないので Command_FUNCLIST()
			if( nId == OUTLINE_LAYOUT_BACKGROUND ) ::EnableWindow( GetEditWnd().GetHwnd(), TRUE );
			return true;	// 解析した
		}

		// フローティング→フローティングでは配置同期せずに現状維持
		if( eDockSideOld <= DOCKSIDE_FLOAT ){
			m_eDockSide = eDockSideNew;
			return false;
		}

		// ドッキング→ドッキングでは配置同期
		RECT rc;
		POINT ptLT;
		GetDockSpaceRect( &rc );
		ptLT.x = rc.left;
		ptLT.y = rc.top;
		::ScreenToClient( m_hwndParent, &ptLT );
		::OffsetRect( &rc, ptLT.x - rc.left, ptLT.y - rc.top );

		switch( eDockSideNew ){
		case DOCKSIDE_LEFT:		rc.right = rc.left + ProfDockLeft();	break;
		case DOCKSIDE_TOP:		rc.bottom = rc.top + ProfDockTop();		break;
		case DOCKSIDE_RIGHT:	rc.left = rc.right - ProfDockRight();	break;
		case DOCKSIDE_BOTTOM:	rc.top = rc.bottom - ProfDockBottom();	break;
		default:
			break;
		}

		// 以前と同じ配置なら無駄に移動しない
		RECT rcOld;
		::GetWindowRect( GetHwnd(), &rcOld );
		ptLT.x = rcOld.left;
		ptLT.y = rcOld.top;
		::ScreenToClient( m_hwndParent, &ptLT );
		::OffsetRect( &rcOld, ptLT.x - rcOld.left, ptLT.y - rcOld.top );
		if( eDockSideOld == eDockSideNew && ::EqualRect( &rcOld, &rc ) ){
			::InvalidateRect( GetHwnd(), nullptr, TRUE );	// いちおう再描画だけ
			return false;	// 配置変更不要（例：別のファイルタイプからの通知）
		}

		// 移動する
		m_eDockSide = eDockSideNew;	// 自身のドッキング配置の記憶を更新
		::SetWindowPos( GetHwnd(), nullptr,
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
			SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOACTIVATE | ((eDockSideOld == eDockSideNew)? 0: SWP_FRAMECHANGED) );	// SWP_FRAMECHANGED 指定で WM_NCCALCSIZE（非クライアント領域の再計算）に誘導する
		GetEditWnd().EndLayoutBars( m_bEditWndReady );
	}
	return false;
}

/** アウトライン通知(MYWM_OUTLINE_NOTIFY)処理

	wParam: 通知種別
	lParam: 種別毎のパラメータ

	@date 2010.06.07 ryoji 新規作成
*/
void CDlgFuncList::OnOutlineNotify( WPARAM wParam, LPARAM lParam )
{
	switch( wParam ){
	case 0:	// 設定変更通知（ドッキングモード or サイズ）, lParam: 通知元の HWND
		if( (HWND)lParam == GetEditWnd().GetHwnd() )
			return;	// 自分からの通知は無視
		ChangeLayout( OUTLINE_LAYOUT_BACKGROUND );	// アウトライン画面を再配置
		break;
	default:
		break;
	}
	return;
}

/** 他ウィンドウにアウトライン通知をポストする
	@date 2010.06.10 ryoji 新規作成
*/
BOOL CDlgFuncList::PostOutlineNotifyToAllEditors( WPARAM wParam, LPARAM lParam )
{
	return CAppNodeGroupHandle(0).PostMessageToAllEditors( MYWM_OUTLINE_NOTIFY, (WPARAM)wParam, (LPARAM)lParam, GetHwnd() );
}

void CDlgFuncList::SetTypeConfig( CTypeConfig docType, const STypeConfig& type )
{
	CDocTypeManager().SetTypeConfig(docType, type);
}

/** コンテキストメニュー処理
	@date 2010.06.07 ryoji 新規作成
*/
BOOL CDlgFuncList::OnContextMenu( WPARAM wParam, LPARAM lParam )
{
	// キャプションかリスト／ツリー上ならメニューを表示する
	HWND hwndFrom = (HWND)wParam;
	if( ::SendMessage( GetHwnd(), WM_NCHITTEST, 0, lParam ) == HTCAPTION
			|| hwndFrom == GetItemHwnd( IDC_LIST_FL )
			|| hwndFrom == GetItemHwnd( IDC_TREE_FL )
	){
		POINT pt;
		pt.x = MAKEPOINTS(lParam).x;
		pt.y = MAKEPOINTS(lParam).y;
		if( pt.x == -1 && pt.y == -1 ){	// キーボード（メニューキー や Shift F10）からの呼び出し
			RECT rc;
			::GetWindowRect( hwndFrom, &rc );
			pt.x = rc.left;
			pt.y = rc.top;
		}
		DoMenu( pt, hwndFrom );
		return TRUE;
	}

	return CDialog::OnContextMenu( wParam, lParam );	// その他のコントロール上ではポップアップヘルプを表示する
}

/** タイトルバーのドラッグ＆ドロップでドッキング配置する際の移動先矩形を求める
	@date 2010.06.17 ryoji 新規作成
*/
EDockSide CDlgFuncList::GetDropRect( POINT ptDrag, POINT ptDrop, LPRECT pRect, bool bForceFloat )
{
	struct CDockStretch{
		static int GetIdealStretch( int nStretch, int nMaxStretch )
		{
			if( nStretch == 0 )
				nStretch = nMaxStretch / 3;
			if( nStretch > nMaxStretch - DOCK_MIN_SIZE ) nStretch = nMaxStretch - DOCK_MIN_SIZE;
			if( nStretch < DOCK_MIN_SIZE ) nStretch = DOCK_MIN_SIZE;
			return nStretch;
		}
	};

	// 移動しない矩形を取得する
	RECT rcWnd;
	::GetWindowRect( GetHwnd(), &rcWnd );
	if( IsDocking() && !bForceFloat ){
		if( ::PtInRect( &rcWnd, ptDrop ) ){
			*pRect = rcWnd;
			return GetDockSide();	// 移動しない位置だった
		}
	}

	// ドッキング用の矩形を取得する
	EDockSide eDockSide = DOCKSIDE_FLOAT;	// フローティングに仮決め
	RECT rcDock;
	GetDockSpaceRect( &rcDock );
	if( !bForceFloat && ::PtInRect( &rcDock, ptDrop ) ){
		int cxLeft		= CDockStretch::GetIdealStretch( ProfDockLeft(), rcDock.right - rcDock.left );
		int cyTop		= CDockStretch::GetIdealStretch( ProfDockTop(), rcDock.bottom - rcDock.top );
		int cxRight		= CDockStretch::GetIdealStretch( ProfDockRight(), rcDock.right - rcDock.left );
		int cyBottom	= CDockStretch::GetIdealStretch( ProfDockBottom(), rcDock.bottom - rcDock.top );

		int nDock = ::GetSystemMetrics( SM_CXCURSOR );
		if( ptDrop.x - rcDock.left < nDock ){
			eDockSide = DOCKSIDE_LEFT;
			rcDock.right = rcDock.left + cxLeft;
		}
		else if( rcDock.right - ptDrop.x < nDock ){
			eDockSide = DOCKSIDE_RIGHT;
			rcDock.left = rcDock.right - cxRight;
		}
		else if( ptDrop.y - rcDock.top < nDock ){
			eDockSide = DOCKSIDE_TOP;
			rcDock.bottom = rcDock.top + cyTop;
		}
		else if( rcDock.bottom - ptDrop.y < nDock ){
			eDockSide = DOCKSIDE_BOTTOM;
			rcDock.top = rcDock.bottom - cyBottom;
		}
		if( eDockSide != DOCKSIDE_FLOAT ){
			*pRect = rcDock;
			return eDockSide;	// ドッキング位置だった
		}
	}

	// フローティング用の矩形を取得する
	if( !IsDocking() ){	// フローティング → フローティング
		::OffsetRect( &rcWnd, ptDrop.x - ptDrag.x, ptDrop.y - ptDrag.y );
		*pRect = rcWnd;
	}else{	// ドッキング → フローティング
		int cx, cy;
		RECT rcFloat;
		rcFloat.left = 0;
		rcFloat.top = 0;
		if( m_pShareData->m_Common.m_sOutline.m_bRememberOutlineWindowPos
				&& m_pShareData->m_Common.m_sOutline.m_widthOutlineWindow	// 初期値だと 0 になっている
				&& m_pShareData->m_Common.m_sOutline.m_heightOutlineWindow	// 初期値だと 0 になっている
		){
			// 記憶しているサイズ
			rcFloat.right = m_pShareData->m_Common.m_sOutline.m_widthOutlineWindow;
			rcFloat.bottom = m_pShareData->m_Common.m_sOutline.m_heightOutlineWindow;
			cx = ::GetSystemMetrics( SM_CXMIN );
			cy = ::GetSystemMetrics( SM_CYMIN );
			if( rcFloat.right < cx ) rcFloat.right = cx;
			if( rcFloat.bottom < cy ) rcFloat.bottom = cy;
		}
		else{
			HINSTANCE hInstance2 = CSelectLang::getLangRsrcInstance();
			if ( m_lastRcInstance != hInstance2 ) {
				HRSRC hResInfo = ::FindResource( hInstance2, MAKEINTRESOURCE(IDD_FUNCLIST), RT_DIALOG );
				if( !hResInfo ) return eDockSide;
				HGLOBAL hResData = ::LoadResource( hInstance2, hResInfo );
				if( !hResData ) return eDockSide;
				m_pDlgTemplate = (LPDLGTEMPLATE)::LockResource( hResData );
				if( !m_pDlgTemplate ) return eDockSide;
				m_dwDlgTmpSize = ::SizeofResource( hInstance2, hResInfo );
				// 言語切り替えでリソースがアンロードされていないか確認するためインスタンスを記憶する
				m_lastRcInstance = hInstance2;
			}
			// デフォルトのサイズ（ダイアログテンプレートで決まるサイズ）
			rcFloat.right = m_pDlgTemplate->cx;
			rcFloat.bottom = m_pDlgTemplate->cy;
			::MapDialogRect( GetHwnd(), &rcFloat );
			rcFloat.right += ::GetSystemMetrics( SM_CXDLGFRAME ) * 2;	// ※ Create 時のスタイル変更でサイズ変更不可からサイズ変更可能にしている
			rcFloat.bottom += ::GetSystemMetrics( SM_CYCAPTION ) + ::GetSystemMetrics( SM_CYDLGFRAME ) * 2;
		}
		cy = ::GetSystemMetrics( SM_CYCAPTION );
		::OffsetRect( &rcFloat, ptDrop.x - cy * 2, ptDrop.y - cy / 2 );
		*pRect = rcFloat;
	}

	return DOCKSIDE_FLOAT;	// フローティング位置だった
}

/** タイトルバーのドラッグ＆ドロップでドッキング配置を変更する
	@date 2010.06.17 ryoji 新規作成
*/
BOOL CDlgFuncList::Track( POINT ptDrag )
{
	if( ::GetCapture() )
		return FALSE;

	struct SLockWindowUpdate
	{	// 画面にゴミが残らないように
		SLockWindowUpdate(){ ::LockWindowUpdate( ::GetDesktopWindow() ); }
		SLockWindowUpdate(const SLockWindowUpdate&) = delete;
		SLockWindowUpdate& operator = (const SLockWindowUpdate&) = delete;
		SLockWindowUpdate(SLockWindowUpdate&&) noexcept = delete;
		SLockWindowUpdate& operator = (SLockWindowUpdate&&) noexcept = delete;
		~SLockWindowUpdate(){ ::LockWindowUpdate( nullptr ); }
	} sLockWindowUpdate;

	const SIZE sizeFull = {8, 8};	// フローティング配置用の枠線の太さ
	const SIZE sizeHalf = {4, 4};	// ドッキング配置用の枠線の太さ
	const SIZE sizeClear = {0, 0};	// 枠線描画しない

	POINT pt;
	RECT rc;
	RECT rcDragLast;
	SIZE sizeLast = sizeClear;
	BOOL bDragging = false;	// まだ本格開始しない
	int cxDragSm = ::GetSystemMetrics( SM_CXDRAG );
	int cyDragSm = ::GetSystemMetrics( SM_CYDRAG );

	::SetCapture( GetHwnd() );	// キャプチャ開始

	while( ::GetCapture() == GetHwnd() )
	{
		MSG msg;
		if (!::GetMessage(&msg, nullptr, 0, 0)){
			::PostQuitMessage( (int)msg.wParam );
			break;
		}

		switch (msg.message){
		case WM_MOUSEMOVE:
			::GetCursorPos( &pt );

			bool bStart;
			bStart = false;
			if( !bDragging ){
				// 押した位置からいくらか動いてからドラッグ開始にする
				if( abs(pt.x - ptDrag.x) >= cxDragSm || abs(pt.y - ptDrag.y) >= cyDragSm ){
					bDragging = bStart = true;	// ここから開始
				}
			}
			if( bDragging ){	// ドラッグ中
				// ドロップ先矩形を描画する
				EDockSide eDockSide = GetDropRect( ptDrag, pt, &rc, ApiWrap::GetKeyState_Control() );
				SIZE sizeNew = (eDockSide <= DOCKSIDE_FLOAT)? sizeFull: sizeHalf;
				CGraphics::DrawDropRect( &rc, sizeNew, bStart? nullptr: &rcDragLast, sizeLast );
				rcDragLast = rc;
				sizeLast = sizeNew;
			}
			break;

		case WM_LBUTTONUP:
			::GetCursorPos( &pt );

			::ReleaseCapture();
			if( bDragging ){
				// ドッキング配置を変更する
				EDockSide eDockSide = GetDropRect( ptDrag, pt, &rc, ApiWrap::GetKeyState_Control() );
				CGraphics::DrawDropRect( nullptr, sizeClear, &rcDragLast, sizeLast );

				bool bType = (ProfDockSet() != 0);
				if( bType ){
					CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
				}
				ProfDockDisp() = GetHwnd()? TRUE: FALSE;
				ProfDockSide() = eDockSide;	// 新しいドッキングモードを適用
				switch( eDockSide ){
				case DOCKSIDE_LEFT:		ProfDockLeft() = rc.right - rc.left;	break;
				case DOCKSIDE_TOP:		ProfDockTop() = rc.bottom - rc.top;		break;
				case DOCKSIDE_RIGHT:	ProfDockRight() = rc.right - rc.left;	break;
				case DOCKSIDE_BOTTOM:	ProfDockBottom() = rc.bottom - rc.top;	break;
				default:
					break;
				}
				if( bType ){
					SetTypeConfig(CTypeConfig(m_nDocType), m_type);
				}
				ChangeLayout( OUTLINE_LAYOUT_FOREGROUND );	// 自分自身への強制変更
				if( !IsDocking() ){
					::MoveWindow( GetHwnd(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE );
				}
				if( ProfDockSync() ){
					PostOutlineNotifyToAllEditors( (WPARAM)0, (LPARAM)GetEditWnd().GetHwnd() );	// 他ウィンドウにドッキング配置変更を通知する
				}
				return TRUE;
			}
			return FALSE;

		case WM_KEYUP:
			if( bDragging ){
				if( msg.wParam == VK_CONTROL ){
					// フローティングを強制するモードを抜ける
					::GetCursorPos( &pt );
					EDockSide eDockSide = GetDropRect( ptDrag, pt, &rc, false );
					SIZE sizeNew = (eDockSide <= DOCKSIDE_FLOAT)? sizeFull: sizeHalf;
					CGraphics::DrawDropRect( &rc, sizeNew, &rcDragLast, sizeLast );
					rcDragLast = rc;
					sizeLast = sizeNew;
				}
			}
			break;

		case WM_KEYDOWN:
			if( bDragging ){
				if( msg.wParam == VK_CONTROL ){
					// フローティングを強制するモードに入る
					::GetCursorPos( &pt );
					GetDropRect( ptDrag, pt, &rc, true );
					CGraphics::DrawDropRect( &rc, sizeFull, &rcDragLast, sizeLast );
					sizeLast = sizeFull;
					rcDragLast = rc;
				}
			}
			if( msg.wParam == VK_ESCAPE ){
				// キャンセル
				::ReleaseCapture();
				if( bDragging )
					CGraphics::DrawDropRect( nullptr, sizeClear, &rcDragLast, sizeLast );
				return FALSE;
			}
			break;

		case WM_RBUTTONDOWN:
			// キャンセル
			::ReleaseCapture();
			if( bDragging )
				CGraphics::DrawDropRect( nullptr, sizeClear, &rcDragLast, sizeLast );
			return FALSE;

		default:
			::DispatchMessage( &msg );
			break;
		}
	}

	::ReleaseCapture();
	return FALSE;
}
void CDlgFuncList::LoadFileTreeSetting( CFileTreeSetting& data, SFilePath& IniDirPath )
{
	const SFileTree* pFileTree;
	if( ProfDockSet() == 0 ){
		pFileTree = &(CommonSet().m_sFileTree);
		data.m_eFileTreeSettingOrgType = EFileTreeSettingFrom_Common;
	}else{
		CDocTypeManager().GetTypeConfig(CTypeConfig(m_nDocType), m_type);
		pFileTree = &(TypeSet().m_sFileTree);
		data.m_eFileTreeSettingOrgType = EFileTreeSettingFrom_Type;
	}
	data.m_eFileTreeSettingLoadType = data.m_eFileTreeSettingOrgType;
	data.m_bProject = pFileTree->m_bProject;
	data.m_szDefaultProjectIni = pFileTree->m_szProjectIni;
	data.m_szLoadProjectIni = L"";
	if( data.m_bProject ){
		// 各フォルダーのプロジェクトファイル読み込み
		WCHAR szPath[_MAX_PATH];
		::GetLongFileName( L".", szPath );
		wcscat( szPath, L"\\" );
		int maxDir = CDlgTagJumpList::CalcMaxUpDirectory( szPath );
		for( int i = 0; i <= maxDir; i++ ){
			CDataProfile cProfile;
			cProfile.SetReadingMode();
			std::wstring strIniFileName;
			strIniFileName += szPath;
			strIniFileName += CommonSet().m_sFileTreeDefIniName;
			if( cProfile.ReadProfile(strIniFileName.c_str()) ){
				CImpExpFileTree::IO_FileTreeIni(cProfile, data.m_aItems);
				data.m_eFileTreeSettingLoadType = EFileTreeSettingFrom_File;
				IniDirPath = szPath;
				CutLastYenFromDirectoryPath( IniDirPath );
				data.m_szLoadProjectIni = strIniFileName.c_str();
				break;
			}
			CDlgTagJumpList::DirUp( szPath );
		}
	}
	if( data.m_szLoadProjectIni[0] == L'\0' ){
		// デフォルトプロジェクトファイル読み込み
		bool bReadIni = false;
		if( pFileTree->m_szProjectIni[0] != L'\0' ){
			CDataProfile cProfile;
			cProfile.SetReadingMode();
			const WCHAR* pszIniFileName;
			WCHAR szDir[_MAX_PATH * 2];
			if( _IS_REL_PATH( pFileTree->m_szProjectIni ) ){
				// sakura.iniからの相対パス
				GetInidirOrExedir( szDir, pFileTree->m_szProjectIni );
				pszIniFileName = szDir;
			}else{
				pszIniFileName = pFileTree->m_szProjectIni;
			}
			if( cProfile.ReadProfile(pszIniFileName) ){
				CImpExpFileTree::IO_FileTreeIni(cProfile, data.m_aItems);
				data.m_szLoadProjectIni = pszIniFileName;
				bReadIni = true;
			}
		}
		if( !bReadIni ){
			// 共通設定orタイプ別設定から読み込み
			//m_fileTreeSetting = *pFileTree;
			data.m_aItems.resize( pFileTree->m_nItemCount );
			for( int i = 0; i < pFileTree->m_nItemCount; i++ ){
				data.m_aItems[i] = pFileTree->m_aItems[i];
			}
		}
	}
}

/*!
	キャレットの移動を通知
	@param[in]	nCurLine	移動後の行
	@param[in]	nCurCol		移動後の桁
*/
void CDlgFuncList::NotifyCaretMovement( CLayoutInt nCurLine, CLayoutInt nCurCol )
{
	if( !::IsWindowVisible( this->GetHwnd() ) ){
		return;
	}

	if( !m_bFuncInfoArrIsUpToDate ){
		return;
	}

	if( m_nCurLine == nCurLine && m_nCurCol == nCurCol ){
		return;
	}

	m_nCurLine = nCurLine;
	m_nCurCol = nCurCol;

	int nFuncInfoIndex = -1;
	if( GetFuncInfoIndex( nCurLine, nCurCol, &nFuncInfoIndex ) ){
		SetItemSelection( nFuncInfoIndex, false );
	}

	return;
}

/*!
	ドキュメントの変更を通知
*/
void CDlgFuncList::NotifyDocModification()
{
	// もう最新ではなくなりました
	m_bFuncInfoArrIsUpToDate = false;
	ObserveWorkbenchDocument( reinterpret_cast<CEditView*>(m_lParam) );
	CancelWorkbenchApply();
	if( m_workbenchDocumentVersion.IsValid() ){
		if( m_workbenchDocumentVersion.version != std::numeric_limits<std::uint64_t>::max() ){
			++m_workbenchDocumentVersion.version;
		}else if( m_workbenchNextDocumentIdentity != std::numeric_limits<std::uint64_t>::max() ){
			m_workbenchDocumentVersion.identity = ++m_workbenchNextDocumentIdentity;
			m_workbenchDocumentVersion.version = 0;
		}else{
			m_workbenchDocumentVersion = {};
		}
	}
	if( IsWorkbenchMode() && IsAsyncWorkbenchOutlineType(m_nOutlineType) ) {
		if( m_workbenchParser != nullptr ) {
			(void)m_workbenchParser->CancelObsolete(m_workbenchDocumentVersion);
		}
		// Edits only advance the revision and arm one quiet-period callback.
		// Capturing the full document is deliberately deferred so a typing burst
		// pays one O(total text) snapshot copy instead of one per keystroke.
		(void)m_workbenchRefreshScheduler.NotifyChange(
			true,
			GetHwnd() != nullptr,
			m_workbenchDocumentVersion,
			[this](std::uint64_t timerToken) {
				const UINT_PTR timerId = WorkbenchRefreshTimerId(timerToken);
				return timerId != 0 && ::SetTimer(GetHwnd(), timerId,
					kWorkbenchRefreshDebounceMs, nullptr) != 0;
			},
			[this](std::uint64_t timerToken) {
				const UINT_PTR timerId = WorkbenchRefreshTimerId(timerToken);
				if( GetHwnd() != nullptr && timerId != 0 ) ::KillTimer(GetHwnd(), timerId);
			},
			[this] {
				// Timer allocation failure falls back to one immediate latest-version
				// request; an edit notification is never silently dropped.
				(void)RequestWorkbenchOutline(m_nOutlineType, true);
			},
			[this] {
				m_workbenchLastTerminal = workbench::outline::OutlineWorkerTerminal::Closed;
				m_workbenchLastTimings = {};
				m_workbenchRequestedVersion = {};
				m_workbenchRequestedGeneration = 0;
				m_workbenchRequestStartUs = 0;
				m_bFuncInfoArrIsUpToDate = false;
			});
	}

	return;
}

/*!
	リスト/ツリービュー上のアイテムを選択または選択解除
	@param[in]	nFuncInfoIndex	選択対象とする関数情報配列のインデックス(-1の場合は選択解除)
	@param[in]	bAllowExpand	選択対象を含むノードを展開するかどうか(ツリービュー以外では無視)
*/
void CDlgFuncList::SetItemSelection( int nFuncInfoIndex, bool bAllowExpand )
{
	if( IsWorkbenchMode() && !HasCurrentWorkbenchModel() ) return;
	if( m_nViewType == VIEWTYPE_TREE ){
		HWND hwndTree = GetItemHwnd( IDC_TREE_FL );
		SetItemSelectionForTreeView( hwndTree, nFuncInfoIndex, bAllowExpand );
	}else if( m_nViewType == VIEWTYPE_LIST ){
		HWND hwndList = GetItemHwnd( IDC_LIST_FL );
		SetItemSelectionForListView( hwndList, nFuncInfoIndex );
	}else{
		;
	}

	return;
}

/*!
	ツリービュー上のアイテムを選択または選択解除
	@param[in]	hwndTree		対象とするツリービューのハンドル
	@param[in]	nFuncInfoIndex	選択対象とする関数情報配列のインデックス(-1の場合は選択解除)
	@param[in]	bAllowExpand	選択対象を含むノードを展開するかどうか
*/
void CDlgFuncList::SetItemSelectionForTreeView( HWND hwndTree, int nFuncInfoIndex, bool bAllowExpand )
{
	if( nFuncInfoIndex == -1 ){
		TreeView_SelectItem( hwndTree, nullptr );
		return;
	}

	std::vector<HTREEITEM> htiStack;
	HTREEITEM htiFound = nullptr;
	if( IsWorkbenchMode()
		&& nFuncInfoIndex >= 0
		&& static_cast<std::size_t>(nFuncInfoIndex) < m_workbenchTreeItems.size() ) {
		htiFound = m_workbenchTreeItems[static_cast<std::size_t>(nFuncInfoIndex)];
	}
	if( htiFound == nullptr ) {
		htiStack.reserve( TreeView_GetCount( hwndTree ) );
		htiStack.push_back( TreeView_GetRoot( hwndTree ) );
		size_t nStackIndex = 0;
		while( htiFound == nullptr && nStackIndex < htiStack.size() ){
			HTREEITEM htiCurrent = htiStack[nStackIndex];
			for( ; nullptr != htiCurrent ; htiCurrent = TreeView_GetNextSibling( hwndTree, htiCurrent ) ){
				TVITEM tvItem = {};
				tvItem.mask = TVIF_HANDLE | TVIF_PARAM;
				tvItem.hItem = htiCurrent;
				TreeView_GetItem( hwndTree, &tvItem );
				if( nFuncInfoIndex == TreeDummylParamToFuncInfoIndex( m_vecDummylParams, tvItem.lParam ) ){
					// 発見
					htiFound = htiCurrent;
					break;
				}

				HTREEITEM htiChild = TreeView_GetChild( hwndTree, htiCurrent );
				if( htiChild != nullptr ){
					htiStack.push_back( htiChild );
				}
			}
			++nStackIndex;
		}
	}

	if( htiFound != nullptr ){
		HTREEITEM htiSelect = htiFound;
		if( !bAllowExpand ){
			// 未展開のアイテムは勝手に開けないよう気を付けて選択
			HTREEITEM htiParent = TreeView_GetParent( hwndTree, htiFound );
			while( htiParent != nullptr && (TreeView_GetItemState( hwndTree, htiParent, TVIS_EXPANDED ) & TVIS_EXPANDED) == 0 ){
				// [A] 展開
				//  +-[B] 未展開 <<< ここを探します
				//     +-[C] 未展開
				//        +-[htiFound]
				htiSelect = htiParent;
				htiParent = TreeView_GetParent( hwndTree, htiParent );
			}
		}
		TreeView_SelectItem( hwndTree, htiSelect );
		TreeView_EnsureVisible( hwndTree, htiSelect );
	}

	return;
}

/*!
	リストビュー上のアイテムを選択または選択解除
	@param[in]	hwndList		対象とするリストビューのハンドル
	@param[in]	nFuncInfoIndex	選択対象とする関数情報配列のインデックス(-1の場合は選択解除)
*/
void CDlgFuncList::SetItemSelectionForListView( HWND hwndList, int nFuncInfoIndex )
{
	if( nFuncInfoIndex == -1 ){
		ListView_SetItemState( hwndList, nFuncInfoIndex, 0, LVIS_SELECTED | LVIS_FOCUSED );
		return;
	}

	int nCount = ListView_GetItemCount( hwndList );
	for( int i = 0; i < nCount; ++i ){
		LVITEM lvItem = {};
		lvItem.mask = LVIF_PARAM;
		lvItem.iItem = i;
		lvItem.iSubItem = 0;
		ListView_GetItem( hwndList, &lvItem );
		if( lvItem.lParam == nFuncInfoIndex ){
			ListView_SetItemState( hwndList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );
			ListView_EnsureVisible( hwndList, i, FALSE );
			break;
		}
	}

	return;
}

/*!
	指定した位置に該当する関数情報のインデックスを取得
	@param[in]	nCurLine	行
	@param[in]	nCurCol		桁
	@param[out]	pnIndexOut	該当する関数情報のインデックスを格納
	@retval		true		該当あり
	@retval		false		該当なし(出力引数には何も設定せず)
*/
bool CDlgFuncList::GetFuncInfoIndex( [[maybe_unused]] CLayoutInt nCurLine, [[maybe_unused]] CLayoutInt nCurCol, int* pnIndexOut )
{
	const CFuncInfo* pcFuncInfo = nullptr;
	CLayoutInt nFuncLineOld(-1);
	CLayoutInt nFuncColOld(-1);
	CLayoutInt nFuncLineTop(INT_MAX);
	CLayoutInt nFuncColTop(INT_MAX);
	int nFoundIndex = -1;
	int i;

	if( m_pcFuncInfoArr == nullptr || pnIndexOut == nullptr ){
		return false;
	}
	if( IsWorkbenchMode()
		&& IsAsyncWorkbenchOutlineType(m_nListType)
		&& m_workbenchCommittedModel.get() == m_pcFuncInfoArr ) {
		const int count = m_pcFuncInfoArr->GetNum();
		if( count == 0 ) return false;
		int low = 0;
		int high = count;
		bool canUseBinarySearch = true;
		while( low < high ) {
			const int middle = low + (high - low) / 2;
			const CFuncInfo* const info = m_pcFuncInfoArr->GetAt(middle);
			if( info == nullptr ) {
				canUseBinarySearch = false;
				break;
			}
			const bool notAfterCaret = info->m_nFuncLineLAYOUT < nCurLine
				|| (info->m_nFuncLineLAYOUT == nCurLine && info->m_nFuncColLAYOUT <= nCurCol);
			if( notAfterCaret ) low = middle + 1;
			else high = middle;
		}
		if( canUseBinarySearch ) {
			*pnIndexOut = low > 0 ? low - 1 : 0;
			return true;
		}
	}

	// SetTree,SetTreeJava,SetListVB,SetDataにあった処理を持ってきました

	for( i = 0; i < m_pcFuncInfoArr->GetNum(); ++i ){
		pcFuncInfo = m_pcFuncInfoArr->GetAt( i );

		if( (pcFuncInfo->m_cmemFileName.GetStringPtr() && m_pcFuncInfoArr->m_szFilePath[0]) ){
			if( 0 != wmemicmp( pcFuncInfo->m_cmemFileName.GetStringPtr(), m_pcFuncInfoArr->m_szFilePath.c_str() ) ){
				continue;
			}
		}

		if( nFoundIndex == -1 ){
			if( pcFuncInfo->m_nFuncLineLAYOUT < nFuncLineTop
				|| (pcFuncInfo->m_nFuncLineLAYOUT == nFuncLineTop && pcFuncInfo->m_nFuncColLAYOUT <= nFuncColTop) ){
				nFuncLineTop = pcFuncInfo->m_nFuncLineLAYOUT;
				nFuncColTop = pcFuncInfo->m_nFuncColLAYOUT;
				nFoundIndex = i;
			}
		}else{
			if( (nFuncLineOld < pcFuncInfo->m_nFuncLineLAYOUT
				|| (nFuncLineOld == pcFuncInfo->m_nFuncLineLAYOUT && nFuncColOld <= pcFuncInfo->m_nFuncColLAYOUT))
			  && (pcFuncInfo->m_nFuncLineLAYOUT < m_nCurLine
				|| (pcFuncInfo->m_nFuncLineLAYOUT == m_nCurLine && pcFuncInfo->m_nFuncColLAYOUT <= m_nCurCol)) ){
				nFuncLineOld = pcFuncInfo->m_nFuncLineLAYOUT;
				nFuncColOld = pcFuncInfo->m_nFuncColLAYOUT;
				nFoundIndex = i;
			}
		}
	}

	if( nFoundIndex == -1 ){
		return false;
	}

	*pnIndexOut = nFoundIndex;

	return true;
}
