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
#include "_main/CCommandLine.h"	/// 2003/1/26 aroka
#include "_main/CAppMode.h"
#include "basis/CErrorInfo.h"
#include "dlg/CDlgAbout.h"
#include "dlg/CDlgPrintSetting.h"
#include "env/CShareData.h"
#include "env/CSakuraEnvironment.h"
#include "charset/CCodeFactory.h"
#include "charset/CCodeBase.h"
#include "charset/charset.h"
#include "CEditApp.h"
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
#include "markdown/CMarkdownPreviewWnd.h"
#include "markdown/MarkdownPreviewLayout.h"
#include "terminal/window/CTerminalTool.h"
#include "theme/CThemeService.h"
#include "workbench/CWorkbenchPanelHost.h"
#include "workbench/CWorkspaceContext.h"
#include "workbench/IWorkbenchRuntime.h"
#include "workbench/IconMetrics.h"
#include "workbench/WorkbenchLayout.h"
#include "workbench/activity/CActivityBar.h"
#include "workbench/extension/CExtensionBottomPanelTool.h"
#include "workbench/extension/CExtensionSidebarTool.h"
#include "workbench/explorer/CExplorerTool.h"
#include "workbench/viewcontainer/CViewContainerHost.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/WorkbenchZoom.h"
#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/commands/WorkbenchContextKeyService.h"
#include "workbench/outline/COutlineWorkbenchTool.h"
#include "workbench/editor/CEditDocLegacyEditorBackend.h"
#include "workbench/editor/CEditorServiceLegacyAdapter.h"
#include "workbench/editor/CEmptyEditorSurface.h"
#include "workbench/editor/CExtensionDetailSurface.h"
#include "workbench/editor/EditorCommandIds.h"
#include "workbench/editor/EditorWorkingCopyCoordinator.h"
#include "workbench/editor/persistence/EditorWorkingCopyLifecycleBridge.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/layout/WorkbenchLayoutStateService.h"
#include "workbench/win32/BuiltinPartProjection.h"
#include "workbench/win32/ProblemsOutputPanelProjection.h"
#include "platform/uri/UriIdentity.h"

#include "macro/CMacroFactory.h"
#include "view/colors/CColorStrategy.h"
#include "view/figures/CFigureManager.h"
#include "extension/CExtensionPane.h"
#include "extension/CExtensionQuickInputDialog.h"
#include "extension/CExtensionManager.h"
#include "extension/CExtensionService.h"
#include "workbench/icons/CExtensionIconFont.h"
#include "extension/CExtensionViewRegistry.h"
#include "extension/IExtensionSecretStorage.h"
#include "cmd/COpeBlk.h"
#include "cmd/CViewCommander_inline.h"

#include <cwctype>
#include <limits>
#include <mutex>
#include <utility>

namespace
{
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

namespace {

constexpr std::string_view kLegacyEditorInputId = "legacy.editor.1";

//! Width of the frame-edge drop zone that stands in for a hidden side bar during a
//! composite drag. VS Code accepts an edge drop to reveal the Secondary Side Bar; the
//! exact strip width is a native detail with no upstream counterpart.
constexpr int kSideBarDropEdgeDip = 48;

//! Maps the legacy shared-memory active tool onto a Primary Side Bar ViewContainer.
//! Outline and Terminal are not Primary Side Bar containers, so both fall back to
//! Explorer: Outline is a View nested in Explorer, and Terminal lives in the Panel.
[[nodiscard]] workbench::viewcontainer::ViewContainerPage SidebarPageForLegacyTool(
	EWorkbenchActiveTool tool) noexcept
{
	using workbench::viewcontainer::ViewContainerPage;
	switch (tool) {
	case WORKBENCH_TOOL_SCM: return ViewContainerPage::SourceControl;
	case WORKBENCH_TOOL_EXTENSIONS: return ViewContainerPage::Extensions;
	case WORKBENCH_TOOL_EXPLORER:
	case WORKBENCH_TOOL_OUTLINE:
	case WORKBENCH_TOOL_TERMINAL:
		break;
	}
	return ViewContainerPage::Explorer;
}

//! The Activity Bar entry for one ViewContainer. Every built-in page has exactly one.
[[nodiscard]] workbench::ActivityBarItem ActivityBarItemForPage(
	workbench::viewcontainer::ViewContainerPage page) noexcept
{
	using workbench::viewcontainer::ViewContainerPage;
	switch (page) {
	case ViewContainerPage::SourceControl: return workbench::ActivityBarItem::SourceControl;
	case ViewContainerPage::Extensions: return workbench::ActivityBarItem::Extensions;
	case ViewContainerPage::Explorer:
	case ViewContainerPage::Count:
		break;
	}
	return workbench::ActivityBarItem::Explorer;
}

[[nodiscard]] std::optional<workbench::viewcontainer::ViewContainerPage> PageForActivityBarItem(
	workbench::ActivityBarItem item) noexcept
{
	using workbench::viewcontainer::ViewContainerPage;
	switch (item) {
	case workbench::ActivityBarItem::Explorer: return ViewContainerPage::Explorer;
	case workbench::ActivityBarItem::SourceControl: return ViewContainerPage::SourceControl;
	case workbench::ActivityBarItem::Extensions: return ViewContainerPage::Extensions;
	case workbench::ActivityBarItem::Count: break;
	}
	return std::nullopt;
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
	catch (...) {
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

[[nodiscard]] std::wstring ExtensionLanguageIdForPath(const std::filesystem::path& path)
{
	auto extension = path.extension().wstring();
	std::ranges::transform(extension, extension.begin(), [](wchar_t value) {
		return static_cast<wchar_t>(std::towlower(value));
	});
	static constexpr std::pair<std::wstring_view, std::wstring_view> mappings[] = {
		{ L".md", L"markdown" }, { L".markdown", L"markdown" },
		{ L".json", L"json" }, { L".jsonc", L"jsonc" },
		{ L".js", L"javascript" }, { L".jsx", L"javascriptreact" },
		{ L".mjs", L"javascript" }, { L".cjs", L"javascript" },
		{ L".ts", L"typescript" }, { L".tsx", L"typescriptreact" },
		{ L".css", L"css" }, { L".scss", L"scss" }, { L".less", L"less" },
		{ L".html", L"html" }, { L".htm", L"html" }, { L".vue", L"vue" },
		{ L".xml", L"xml" }, { L".svg", L"xml" },
		{ L".yaml", L"yaml" }, { L".yml", L"yaml" },
		{ L".py", L"python" }, { L".rb", L"ruby" }, { L".php", L"php" },
		{ L".c", L"c" }, { L".h", L"c" }, { L".cc", L"cpp" },
		{ L".cpp", L"cpp" }, { L".cxx", L"cpp" }, { L".hpp", L"cpp" },
		{ L".java", L"java" }, { L".cs", L"csharp" }, { L".go", L"go" }, { L".rs", L"rust" },
		{ L".ini", L"ini" }, { L".toml", L"toml" },
		{ L".sh", L"shellscript" }, { L".ps1", L"powershell" }, { L".bat", L"bat" },
	};
	for (const auto& [suffix, language] : mappings) {
		if (extension == suffix) return std::wstring(language);
	}
	return L"plaintext";
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

static void ShowCodeBox( HWND hWnd, CEditDoc* pcEditDoc )
{
	// カーソル位置の文字列を取得
	const CLayout*	pcLayout;
	CLogicInt		nLineLen;
	const CEditView* pcView = &GetEditWnd().GetActiveView();
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
				::MessageBox( hWnd, szMsg, GSTR_APPNAME, MB_OK );
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
	workbench::IWorkbenchRuntime& workbenchRuntime,
	std::filesystem::path profileDirectory,
	std::unique_ptr<IExtensionSecretSessionStorage> extensionSecretStorage)
	: CEditWnd()
{
	m_editorServiceAdapter = &editorServiceAdapter;
	m_legacyEditorBackend = &legacyEditorBackend;
	m_workingCopyCoordinator = &workingCopyCoordinator;
	m_workingCopyLifecycleBridge = &workingCopyLifecycleBridge;
	m_workbenchRuntime = &workbenchRuntime;
	m_extensionProfileDirectory = std::move(profileDirectory);
	m_extensionSecretStorage = std::move(extensionSecretStorage);
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
	if (commandId == command_ids::NewUntitledFile) return CreateUntitledEditorInput();
	if (commandId == command_ids::OpenFile) {
		GetDocument()->HandleCommand(F_FILEOPEN);
		return true;
	}
	if (commandId == command_ids::OpenFolder) {
		OpenWorkspaceFolder();
		return true;
	}
	if (commandId == command_ids::Save || commandId == command_ids::SaveAs
		|| commandId == command_ids::Revert || commandId == command_ids::CloseActiveEditor) {
		return ExecuteActiveWorkingCopyCommand(commandId);
	}
	if (commandId == command_ids::ShowCommands) {
		ShowExtensionCommandPalette();
		return true;
	}
	if (commandId == command_ids::OpenSettings) {
		return CEditApp::getInstance()->OpenPropertySheet(-1);
	}
	return false;
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

	const auto baseCode = static_cast<EFunctionCode>(static_cast<int>(request.functionCode) & 0xffff);
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
		if (!WideToUtf8Bounded(reinterpret_cast<const wchar_t*>(request.lparam1), 4096, options.suggestedTarget)
			|| !TryCanonicalEncodingId(static_cast<ECodeType>(request.lparam2), options.encodingId)
			|| !TryWorkingCopyLineEnding(static_cast<EEolType>(request.lparam3), options.lineEnding)) {
			dispatch.operation = invalidInput();
			dispatch.legacyResult = FALSE;
			break;
		}
		dispatch.operation = ExecuteActiveWorkingCopyOperation(command_ids::SaveAs, options);
		dispatch.legacyResult = dispatch.operation->status == EEditorWorkingCopyOperationStatus::Succeeded
			? TRUE : FALSE;
		break;

	case F_FILESAVEAS:
		if (!TryWorkingCopyLineEnding(static_cast<EEolType>(request.lparam3), options.lineEnding)) {
			dispatch.operation = invalidInput();
			dispatch.legacyResult = FALSE;
			break;
		}
		if (auto target = FileIdentityFromLegacyPath(reinterpret_cast<const wchar_t*>(request.lparam1))) {
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
	if (baseCode == F_TOGGLE_LEFT_EXPLORER && m_workbenchRuntime != nullptr) {
		bool handled = false;
		(void)TryExecuteWorkbenchStableCommand("workbench.view.explorer", handled);
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
	const bool presentationChanged = !m_editorCorePresentationInitialized
		|| m_hasActiveEditorInput != hasActiveInput;
	m_hasActiveEditorInput = hasActiveInput;
	m_editorCorePresentationInitialized = true;
	if (!presentationChanged) return;

	const HWND splitter = m_cSplitterWnd.GetHwnd();
	const HWND emptySurface = m_emptyEditorSurface ? m_emptyEditorSurface->GetHwnd() : nullptr;
	const HWND extensionDetailSurface = m_extensionDetailSurface ? m_extensionDetailSurface->GetHwnd() : nullptr;
	const HWND focused = ::GetFocus();
	const bool editorOwnedFocus = focused != nullptr
		&& ((splitter != nullptr && (focused == splitter || ::IsChild(splitter, focused)))
			|| (emptySurface != nullptr && (focused == emptySurface || ::IsChild(emptySurface, focused)))
			|| (extensionDetailSurface != nullptr && (focused == extensionDetailSurface || ::IsChild(extensionDetailSurface, focused))));

	if (hasActiveInput) {
		if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
		if (m_extensionDetailSurface) m_extensionDetailSurface->Hide();
		if (splitter != nullptr && !m_pPrintPreview) ::ShowWindow(splitter, SW_SHOWNA);
		if (const HWND minimap = m_cMiniMapView.GetHwnd(); minimap != nullptr && !m_pPrintPreview) {
			::ShowWindow(minimap, SW_SHOWNA);
		}
		PublishExtensionDocumentOpen(false);
	} else {
		m_markdownPreviewVisible = false;
		m_markdownPreviewDirty = false;
		m_markdownPreviewRevision = -1;
		m_markdownPreviewDivider = {};
		if (m_markdownPreview) m_markdownPreview->Show(false);
		if (splitter != nullptr) ::ShowWindow(splitter, SW_HIDE);
		if (const HWND minimap = m_cMiniMapView.GetHwnd(); minimap != nullptr) {
			::ShowWindow(minimap, SW_HIDE);
		}
		if (m_extensionDetailSurface && m_extensionDetailSurface->HasExtension() && !m_pPrintPreview) {
			m_extensionDetailSurface->Show();
		} else if (m_emptyEditorSurface && !m_pPrintPreview) {
			m_emptyEditorSurface->Show();
		}
		ClearDocumentStatus();
		ClosePublishedExtensionDocument();
	}

	UpdateCaption();
	m_cTabWnd.RefreshDocumentActionState();
	if (const HWND window = GetHwnd(); ::IsWindow(window)) {
		RECT client{};
		::GetClientRect(window, &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}

	if (!restoreFocus || !editorOwnedFocus || m_pPrintPreview) return;
	if (hasActiveInput) {
		if (const HWND view = GetActiveView().GetHwnd(); ::IsWindowVisible(view)) ::SetFocus(view);
	} else if (m_extensionDetailSurface && m_extensionDetailSurface->HasExtension()) {
		m_extensionDetailSurface->Focus();
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

void CEditWnd::ClosePublishedExtensionDocument()
{
	if (m_extensionDocumentSyncTimerPending) {
		if (const HWND window = GetHwnd(); ::IsWindow(window)) {
			::KillTimer(window, IDT_EXTENSION_DOCUMENT_SYNC);
		}
		m_extensionDocumentSyncTimerPending = false;
	}
	if (m_extensionService) {
		if (m_extensionDocumentVersion != 0) {
			m_extensionService->CloseDocument({ ::GetCurrentProcessId(), 1 });
		}
		m_extensionService->SetActiveEditor({}, {});
	}
	m_extensionDocumentUri.clear();
	m_extensionDocumentVersion = 0;
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

bool CEditWnd::InitializeWorkbench()
{
	if (m_workspaceContext != nullptr) return true;

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
			return false;
		}
		return false;
	};
	const auto commitExtensionViewsExtent = [this](workbench::WorkbenchEdge, int extentDip) {
		if (m_workbenchRuntime == nullptr) {
			PersistExtensionViewsExtent(extentDip);
			return true;
		}
		return SetBuiltinPartExtent(workbench::layout::ids::part::Auxiliarybar, extentDip);
	};
	auto& settings = m_pShareData->m_Common.m_sWorkbench;

	// The Extensions ViewContainer is hosted in the Primary Side Bar exactly like VS
	// Code's `workbench.view.extensions`, so its registry must exist before that host.
	m_extensionViewRegistry = std::make_shared<CExtensionViewRegistry>();

	// VS Code's `workbench.view.extensions` *is* the OpenVSX Marketplace, so the
	// container's own content is built here rather than as a separate floating pane.
	// OpenVSX is a profile-scoped workbench service: fail closed (no factory at all)
	// when the immutable bootstrap/configuration authority is unavailable instead of
	// guessing a profile.
	workbench::viewcontainer::CViewContainerPages::MarketplaceFactory marketplaceFactory;
	if (m_workbenchRuntime != nullptr) {
		marketplaceFactory = [this](HWND owner) -> std::unique_ptr<CExtensionPane> {
			auto pane = std::make_unique<CExtensionPane>(
				m_workbenchRuntime->Configuration(),
				m_workbenchRuntime->Bootstrap().UserDataProfile().SelectedProfileId(),
				m_pShareData->m_sHandles.m_hwndTray);
			// m_extensionService is constructed after marketplaceFactory (this lambda) is
			// built, so the callback must read it through `this` at call time -- never
			// capture the pointer itself. By the time an install can complete, the
			// service is long since constructed. A null service (not yet constructed, or
			// already torn down) makes this a no-op rather than a dangling call.
			pane->SetOnExtensionInstalled([this]() {
				if (m_extensionService) m_extensionService->RequestInstalledExtensionRescan();
				// 新しく入った拡張の contributes.icons も同じ契機で取り込む。
				// 再起動を待たずにステータスバーが本物のグリフを描けるようにする。
				RefreshExtensionIconFonts();
			});
			pane->SetOnExtensionSelected([this](const SOpenVsxExtension& extension) {
				if (!m_extensionDetailSurface) return;
				if (extension.sNamespace.empty() && extension.sName.empty()) {
					m_extensionDetailSurface->ClearExtension();
					m_extensionDetailSurface->Hide();
					if (!HasActiveEditorInput() && m_emptyEditorSurface) m_emptyEditorSurface->Show();
					RECT client{};
					if (GetHwnd() != nullptr && ::GetClientRect(GetHwnd(), &client)) {
						(void)OnSize2(m_nWinSizeType,
							MAKELONG(client.right - client.left, client.bottom - client.top), false);
					}
					return;
				}
				m_extensionDetailSurface->ShowExtension(extension);
				if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
				RECT client{};
				if (GetHwnd() != nullptr && ::GetClientRect(GetHwnd(), &client)) {
					(void)OnSize2(m_nWinSizeType,
						MAKELONG(client.right - client.left, client.bottom - client.top), false);
				}
			});
			pane->SetOnExtensionReadme([this](const SOpenVsxExtension&, CExtensionPane::EExtensionReadmeState state, const std::wstring& markdown, const std::wstring& error) {
				if (!m_extensionDetailSurface || !m_extensionDetailSurface->HasExtension()) return;
				switch (state) {
				case CExtensionPane::EExtensionReadmeState::Loading:
					m_extensionDetailSurface->SetReadmeLoading();
					break;
				case CExtensionPane::EExtensionReadmeState::Ready:
					m_extensionDetailSurface->SetReadmeMarkdown(markdown);
					break;
				case CExtensionPane::EExtensionReadmeState::Error:
					m_extensionDetailSurface->SetReadmeError(error.empty() ? L"README could not be loaded." : error);
					break;
				case CExtensionPane::EExtensionReadmeState::Unsupported:
					m_extensionDetailSurface->SetReadmeUnsupported();
					break;
				}
			});
			if (pane->Open(G_AppInstance(), owner) == nullptr) {
				return nullptr;
			}
			return pane;
		};
	}

	// VS Code renders the same composite bar in the Primary and the Secondary Side Bar, so
	// a ViewContainer must be able to move between them without being recreated. The page
	// controls therefore live in a shared pool that outlives either host.
	m_viewContainerPages = std::make_shared<workbench::viewcontainer::CViewContainerPages>(
		m_cDlgFuncList, m_extensionViewRegistry, std::move(marketplaceFactory));
	if (!m_viewContainerPages->Create(GetHwnd())) {
		m_viewContainerPages.reset();
		CloseWorkbench();
		return false;
	}
	m_explorerTool = m_viewContainerPages->Explorer();
	m_outlineWorkbenchTool = m_viewContainerPages->Outline();
	m_scmTool = m_viewContainerPages->SourceControl();
	m_extensionSidebarTool = m_viewContainerPages->Extensions();
	const auto workspaceRoot = GetSemanticWorkspaceRoot();
	m_explorerTool->SetRoot(workspaceRoot);
	m_scmTool->SetRoot(workspaceRoot);
	m_explorerTool->SetFileActivationCallback([this](std::wstring_view path) {
		const std::wstring ownedPath(path);
		GetActiveView().GetCommander().Command_FILEOPEN(ownedPath.c_str());
	});
	m_scmTool->SetFileActivationCallback([this](std::wstring_view path) {
		const std::wstring ownedPath(path);
		GetActiveView().GetCommander().Command_FILEOPEN(ownedPath.c_str());
	});
	m_scmTool->SetStateChangedCallback([this](const workbench::scm::GitScmState& state) {
		m_cStatusBar.SetScmText(workbench::scm::FormatStatusLine(state));
	});

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
		if (expanded && m_dispatchReady) ReloadWorkbenchOutlineAndRelayout();
		return true;
	};
	const auto onHeaderDrag = [this](workbench::WorkbenchEdge edge, POINT screenPoint) {
		const auto* source = edge == workbench::WorkbenchEdge::Right ? m_auxiliaryBarHost : m_sidebarHost;
		if (source == nullptr) return;
		const auto page = source->ActivePage();
		if (!page) return;
		if (const auto target = HitTestSideBarEdge(screenPoint); target && *target != edge) {
			MoveViewContainerToEdge(*page, *target);
		}
	};

	m_leftWorkbenchPanel = std::make_unique<workbench::CWorkbenchPanelHost>(
		workbench::WorkbenchEdge::Left, settings.m_nLeftPanelExtent96, commitExtent);
	auto sidebarHost = std::make_unique<workbench::viewcontainer::CViewContainerHost>(
		m_viewContainerPages, requestOutlineExpanded);
	m_sidebarHost = sidebarHost.get();
	m_leftWorkbenchPanel->SetHeaderDragCallback(onHeaderDrag);
	if (!m_leftWorkbenchPanel->Create(GetHwnd(), G_AppInstance(), std::move(sidebarHost))) {
		m_sidebarHost = nullptr;
		m_leftWorkbenchPanel.reset();
	}

	// VS Code's Secondary Side Bar is empty until a ViewContainer is moved into it, but it
	// is the very same composite host, so it renders a borrowed page once one arrives.
	m_rightWorkbenchPanel = std::make_unique<workbench::CWorkbenchPanelHost>(
		workbench::WorkbenchEdge::Right, settings.m_nExtensionViewsExtent96, commitExtensionViewsExtent);
	m_rightWorkbenchPanel->SetTitle(L"SECONDARY SIDE BAR");
	auto auxiliaryHost = std::make_unique<workbench::viewcontainer::CViewContainerHost>(
		m_viewContainerPages, requestOutlineExpanded);
	m_auxiliaryBarHost = auxiliaryHost.get();
	m_rightWorkbenchPanel->SetHeaderDragCallback(onHeaderDrag);
	if (!m_rightWorkbenchPanel->Create(GetHwnd(), G_AppInstance(), std::move(auxiliaryHost))) {
		m_auxiliaryBarHost = nullptr;
		m_rightWorkbenchPanel.reset();
	}
	if (m_sidebarHost != nullptr) {
		// Explorer is VS Code's default Primary Side Bar container.
		m_sidebarHost->ShowPage(workbench::viewcontainer::ViewContainerPage::Explorer);
	}
	RefreshSidebarTitles();

	m_bottomWorkbenchPanel = std::make_unique<workbench::CWorkbenchPanelHost>(
		workbench::WorkbenchEdge::Bottom, settings.m_nBottomPanelExtent96, commitExtent);
	auto bottomPanelTool = std::make_unique<workbench::extension::CExtensionBottomPanelTool>();
	m_extensionBottomPanelTool = bottomPanelTool.get();
	m_terminalTool = bottomPanelTool->Terminal();
	bottomPanelTool->SetTabSelectionCallback(
		[this](workbench::extension::ExtensionBottomPanelTab tab) {
			if (m_workbenchRuntime == nullptr) return true;
			switch (tab) {
			case workbench::extension::ExtensionBottomPanelTab::Terminal:
				return ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Terminal, false);
			case workbench::extension::ExtensionBottomPanelTab::Problems:
				break;
			case workbench::extension::ExtensionBottomPanelTab::Output:
				break;
			default:
				return false;
			}
			const std::string_view commandId = tab == workbench::extension::ExtensionBottomPanelTab::Problems
				? "workbench.actions.view.problems"
				: "workbench.action.output.toggleOutput";
			bool handled = false;
			const bool succeeded = TryExecuteWorkbenchStableCommand(commandId, handled);
			if (handled) return succeeded;
			return tab == workbench::extension::ExtensionBottomPanelTab::Problems
				? ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Problems, false)
				: ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Output, false);
		});
	m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
	m_terminalTool->SetPanelActions({
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
	if (!m_bottomWorkbenchPanel->Create(GetHwnd(), G_AppInstance(), std::move(bottomPanelTool))) {
		m_extensionBottomPanelTool = nullptr;
		m_terminalTool = nullptr;
		m_bottomWorkbenchPanel.reset();
	}

	m_activityBar = std::make_unique<workbench::CActivityBar>([this](workbench::ActivityBarItem item) {
		const auto resolved = PageForActivityBarItem(item);
		if (!resolved) return;
		const auto page = *resolved;
		// VS Code's `ViewContainerActivityAction` hides the Primary Side Bar when the clicked
		// ViewContainer already is the visible active one, and opens it otherwise; the default
		// `workbench.activityBar.iconClickBehavior` is "toggle". Hiding therefore belongs to the
		// click gesture, never to `workbench.view.*`, which only ever reveals a container.
		if (m_workbenchRuntime != nullptr) {
			const std::string_view commandId = IsSidebarViewContainerActive(SidebarViewContainerId(page))
				? std::string_view("workbench.action.toggleSidebarVisibility")
				: page == workbench::viewcontainer::ViewContainerPage::Explorer
					? std::string_view("workbench.view.explorer")
					: std::string_view();
			if (!commandId.empty()) {
				bool handled = false;
				(void)TryExecuteWorkbenchStableCommand(commandId, handled);
				if (handled) return;
			}
		}
		ActivateSidebarPage(page, true);
	});
	// VS Code's Activity Bar icon is a composite drag handle: dropping it on another side
	// bar runs the same `moveViewContainerToLocation` the Command Palette move uses.
	m_activityBar->SetContainerDragCallback([this](workbench::ActivityBarItem item, POINT screenPoint) {
		const auto resolved = PageForActivityBarItem(item);
		if (!resolved) return;
		if (const auto target = HitTestSideBarEdge(screenPoint);
			target && *target != workbench::WorkbenchEdge::Left) {
			MoveViewContainerToEdge(*resolved, *target);
		}
	});
	if (!m_activityBar->Create(GetHwnd(), G_AppInstance())) m_activityBar.reset();

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
				(void)ExecuteWorkbenchEditorCommand(commandId);
			});
		if (!m_emptyEditorSurface->Create(GetHwnd(), G_AppInstance())) {
			m_emptyEditorSurface.reset();
		}
		m_extensionDetailSurface = std::make_unique<CExtensionDetailSurface>();
		if (m_extensionDetailSurface->Open(G_AppInstance(), GetHwnd()) == nullptr) {
			m_extensionDetailSurface.reset();
		} else {
			m_extensionDetailSurface->Hide();
			m_extensionDetailSurface->SetOnInstallRequested([this]() {
				if (m_viewContainerPages && m_viewContainerPages->Marketplace()) {
					m_viewContainerPages->Marketplace()->InstallSelectedExtension();
				}
			});
			m_extensionDetailSurface->SetOnCloseRequested([this]() {
				if (m_viewContainerPages && m_viewContainerPages->Marketplace()) {
					m_viewContainerPages->Marketplace()->ClearExtensionSelection();
				}
				if (m_extensionDetailSurface) m_extensionDetailSurface->ClearExtension();
				if (m_emptyEditorSurface && !HasActiveEditorInput()) m_emptyEditorSurface->Show();
				RECT client{};
				if (GetHwnd() != nullptr && ::GetClientRect(GetHwnd(), &client)) {
					(void)OnSize2(m_nWinSizeType,
						MAKELONG(client.right - client.left, client.bottom - client.top), false);
				}
			});
		}
	}

	const bool initialized = m_leftWorkbenchPanel != nullptr
		&& m_rightWorkbenchPanel != nullptr
		&& m_bottomWorkbenchPanel != nullptr
		&& m_activityBar != nullptr
		&& (!editorBridgeEnabled || (m_emptyEditorSurface != nullptr && m_extensionDetailSurface != nullptr));
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
			.toggleSidebarVisibility = [this]() {
				return ExecuteToggleSidebarVisibilityCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "sidebar layout command failed" };
			},
			.showExplorer = [this]() {
				return ExecuteShowExplorerCommand()
					? workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} }
					: workbench::commands::WorkbenchCommandExecutionResult{
						workbench::commands::EWorkbenchCommandExecutionStatus::Failed, "explorer layout command failed" };
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
		});
		if (!registration.Succeeded()) {
			CloseWorkbench();
			return false;
		}
	}
	if (m_workbenchRuntime != nullptr && !InitializeWorkbenchServiceProjection()) {
		CloseWorkbench();
		return false;
	}
	const auto extensionProfileDirectory = m_extensionProfileDirectory.empty()
		? GetIniFileName().parent_path() : m_extensionProfileDirectory;
	// contributes.icons は拡張ホストの接続とは独立に読める（マニフェストとフォント
	// ファイルだけで完結する）。ホストが繋がらなくてもアイコンは正しく描けるよう、
	// サービス構築より先に用意し、ステータスバーへ非所有で貸す。
	m_extensionIconFonts = std::make_unique<workbench::icons::CExtensionIconFontRegistry>();
	m_cStatusBar.SetExtensionIconFonts(m_extensionIconFonts.get());
	RefreshExtensionIconFonts();

	// workspace/configuration/update の書き込み先は、この window が借りている
	// workbench runtime そのもの（Settings writeback の唯一の所有者）。ここで渡さないと
	// bridge は runtime 無しのまま構築され、拡張からの update() は常に -32001 で閉じる。
	// runtime を持たない単体テスト経路では nullptr のままで、その場合も fail-closed になる。
	m_extensionService = std::make_unique<CExtensionService>(
		GetHwnd(), m_pShareData->m_sHandles.m_hwndTray, extensionProfileDirectory,
		m_extensionViewRegistry, std::move(m_extensionSecretStorage), m_markerService, m_outputService,
		m_workbenchRuntime);
	m_extensionService->SetApplyEditHandler([this](const std::vector<SExtensionDocumentEdit>& edits) {
		CExtensionService::NativeApplyEditResult completion;
		completion.result = ApplyExtensionEdits(edits, completion.snapshots);
		return completion;
	});
	m_extensionService->SetEditorOptionsHandler([this](const SExtensionNativeEditorOptions& options) {
		return ApplyExtensionEditorOptions(options);
	});
	m_extensionBottomPanelTool->SetProblemActivationCallback(
		[this](const workbench::win32::ProblemsPanelEntry& problem) {
			const auto path = ExtensionFilePathFromUri(problem.resourceUri);
			if (!path) return;
			GetActiveView().GetCommander().Command_FILEOPEN(path->c_str());
			// TODO: add an explicit UTF-16-to-Sakura position adapter before applying
			// problem.range. The service range is deliberately not a legacy column.
		});
	m_extensionBottomPanelTool->SetOutputChannelSelectionCallback([this](const std::string& channelId) {
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
	m_cStatusBar.SetExtensionCommandCallback([this](std::wstring_view command) {
		if (m_extensionService) m_extensionService->ExecuteCommand(command);
	});
	m_extensionSidebarTool->SetRequestChildrenCallback([this](std::wstring_view view, std::wstring_view parent) {
		if (m_extensionService) m_extensionService->RequestTreeChildren(view, parent);
	});
	m_extensionSidebarTool->SetSelectionChangedCallback(
		[this](std::wstring_view view, const std::vector<std::wstring>& items) {
			if (m_extensionService) m_extensionService->NotifyTreeSelection(view, items);
		});
	m_extensionSidebarTool->SetCheckboxChangedCallback(
		[this](std::wstring_view view, std::wstring_view item, bool checked) {
			if (m_extensionService) m_extensionService->NotifyTreeCheckbox(view, item, checked);
		});
	m_extensionSidebarTool->SetCommandCallback(
		[this](std::wstring_view command, std::string_view argumentsJson,
			std::wstring_view, std::wstring_view) {
			if (m_extensionService) m_extensionService->ExecuteCommand(command, argumentsJson);
		});
	m_extensionSidebarTool->SetVisibilityChangedCallback([this](bool visible) {
		if (m_extensionService) m_extensionService->NotifyViewVisibility(visible);
	});

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

	ApplyWorkbenchTheme();
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
	// CEditWnd::Create runs before the startup file is loaded.  Parsing the
	// bootstrap document here only produces a result that the load immediately
	// invalidates.  Coalesce both document-dependent jobs and complete them after
	// the real document has reached its first painted frame.  The outline reload
	// stays a runtime-less legacy path; a runtime-backed window projects its
	// Explorer/Outline state from the committed layout snapshot instead.
	m_startupOutlineReloadPending = m_workbenchRuntime == nullptr && settings.m_bRightPanelVisible != FALSE;
	m_startupExtensionDocumentOpenPending = true;
	return true;
}

void CEditWnd::PostDeferredStartupWorkbenchIfReady()
{
	if (m_startupWorkbenchCompletionPosted
		|| m_startupDrawState != StartupDrawState::Committed
		|| !m_startupFirstContentPainted
		|| !m_cDlgFuncList.m_bEditWndReady
		|| (!m_startupOutlineReloadPending && !m_startupExtensionDocumentOpenPending)) {
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
	const bool publishDocument = std::exchange(m_startupExtensionDocumentOpenPending, false);
	const bool reloadOutline = std::exchange(m_startupOutlineReloadPending, false);
	if (!publishDocument && !reloadOutline) return;

	if (publishDocument && m_extensionService) {
		PublishExtensionDocumentOpen(true);
	}
	const auto& settings = m_pShareData->m_Common.m_sWorkbench;
	if (reloadOutline
		&& settings.m_bRightPanelVisible != FALSE
		&& m_outlineWorkbenchTool != nullptr
		&& IsOutlineViewExpanded()) {
		ReloadWorkbenchOutlineAndRelayout();
	}
}

std::vector<SExtensionDiagnostic> CEditWnd::ExtensionDiagnosticsForCurrentDocument() const
{
	if (!HasActiveEditorInput() || !m_extensionService
		|| !GetDocument()->m_cDocFile.GetFilePathClass().IsValidPath()) return {};
	const auto uri = ExtensionFileUriFromPath(GetDocument()->m_cDocFile.GetFilePath());
	return uri.empty() ? std::vector<SExtensionDiagnostic>{} : m_extensionService->DiagnosticsForUri(uri);
}

SExtensionDocumentSnapshot CEditWnd::CaptureExtensionDocumentSnapshot(std::uint64_t version) const
{
	SExtensionDocumentSnapshot snapshot;
	snapshot.id = { ::GetCurrentProcessId(), 1 };
	const auto& file = GetDocument()->m_cDocFile;
	if (file.GetFilePathClass().IsValidPath()) {
		const std::filesystem::path path(file.GetFilePath());
		snapshot.uri = ExtensionFileUriFromPath(path);
		snapshot.languageId = ExtensionLanguageIdForPath(path);
	} else {
		snapshot.uri = L"untitled:sakura-editor-next/" + std::to_wstring(::GetCurrentProcessId()) + L"/1";
		snapshot.languageId = L"plaintext";
	}
	for (const CDocLine* line = GetDocument()->m_cDocLineMgr.GetDocLineTop(); line; line = line->GetNextLine()) {
		const auto length = static_cast<std::size_t>(line->GetLengthWithEOL());
		if (length != 0) snapshot.text.append(line->GetPtr(), length);
	}
	snapshot.version = version;
	snapshot.dirty = GetDocument()->m_cDocEditor.IsModified();
	return snapshot;
}

void CEditWnd::PublishExtensionDocumentOpen(bool forceReopen)
{
	if (!m_extensionService) return;
	if (!HasActiveEditorInput()) {
		ClosePublishedExtensionDocument();
		return;
	}
	auto snapshot = CaptureExtensionDocumentSnapshot(1);
	if (snapshot.uri.empty()) return;
	if (!m_extensionDocumentUri.empty() && (forceReopen || snapshot.uri != m_extensionDocumentUri)) {
		if (m_extensionDocumentSyncTimerPending) {
			::KillTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC);
			m_extensionDocumentSyncTimerPending = false;
		}
		m_extensionService->CloseDocument({ ::GetCurrentProcessId(), 1 });
		m_extensionDocumentUri.clear();
		m_extensionDocumentVersion = 0;
	}
	if (m_extensionDocumentVersion == 0) {
		m_extensionDocumentUri = snapshot.uri;
		m_extensionDocumentVersion = snapshot.version;
		m_extensionService->OpenDocument(std::move(snapshot));
	}
	PublishExtensionActiveEditor();
}

void CEditWnd::NotifyExtensionDocumentChanged()
{
	if (!HasActiveEditorInput()) return;
	const bool synchronized = SynchronizeLegacyDocumentState(
		GetDocument()->m_cDocEditor.IsModified(), true);
	if (synchronized && !m_workingCopyBackendEffectInProgress && m_workingCopyLifecycleBridge) {
		(void)m_workingCopyLifecycleBridge->NotifyCurrentChanged(::GetTickCount64());
	}
	if (!m_extensionService || m_extensionDocumentVersion == 0 || m_extensionDocumentSyncTimerPending) return;
	if (::SetTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC, 8, nullptr) != 0) {
		m_extensionDocumentSyncTimerPending = true;
	} else {
		PublishExtensionDocumentChange();
	}
}

void CEditWnd::PublishExtensionDocumentChange()
{
	if (!HasActiveEditorInput()) {
		ClosePublishedExtensionDocument();
		return;
	}
	if (!m_extensionService || m_extensionDocumentVersion == 0) return;
	auto snapshot = CaptureExtensionDocumentSnapshot(++m_extensionDocumentVersion);
	if (snapshot.uri != m_extensionDocumentUri) {
		PublishExtensionDocumentOpen(true);
		return;
	}
	m_extensionService->ChangeDocument(std::move(snapshot));
	PublishExtensionActiveEditor();
}

void CEditWnd::PublishExtensionDocumentSave()
{
	if (!HasActiveEditorInput()) {
		ClosePublishedExtensionDocument();
		return;
	}
	if (!m_extensionService) return;
	if (m_extensionDocumentSyncTimerPending) {
		::KillTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC);
		m_extensionDocumentSyncTimerPending = false;
		PublishExtensionDocumentChange();
	}
	if (m_extensionDocumentVersion == 0) {
		PublishExtensionDocumentOpen(false);
		return;
	}
	auto snapshot = CaptureExtensionDocumentSnapshot(m_extensionDocumentVersion);
	if (snapshot.uri != m_extensionDocumentUri) {
		PublishExtensionDocumentOpen(true);
		return;
	}
	snapshot.dirty = false;
	m_extensionService->SaveDocument(std::move(snapshot));
}

void CEditWnd::PublishExtensionActiveEditor()
{
	if (!HasActiveEditorInput() || !m_extensionService || m_extensionDocumentVersion == 0) return;
	const auto caret = GetActiveView().GetCaret().GetCaretLogicPos();
	const int line = Int(caret.y);
	const int character = Int(caret.x);
	const SExtensionTextPosition position{
		static_cast<std::uint32_t>((std::max)(0, line)),
		static_cast<std::uint32_t>((std::max)(0, character)),
	};
	m_extensionService->SetActiveEditor({ ::GetCurrentProcessId(), 1 }, position);
}

void CEditWnd::RefreshExtensionIconFonts()
{
	if (!m_extensionIconFonts) return;
	CExtensionManager manager;
	for (const auto& installed : manager.EnumInstalled()) {
		const auto root = installed.dir / CExtensionManager::kVsixContentDir;
		std::error_code error;
		if (!std::filesystem::is_regular_file(root / CExtensionManager::kManifestFileName, error) || error) {
			continue;
		}
		// 読めない・contributes.icons を持たない拡張は登録 0 件で正常終了する。
		// マニフェストが壊れている場合だけ false になるが、その拡張を飛ばすだけで
		// 他の拡張の登録は続ける（fail closed であって fail stop ではない）。
		(void)m_extensionIconFonts->RegisterExtension(installed.sUniqueId, root);
	}
	if (m_cStatusBar.GetStatusHwnd() != nullptr) {
		::InvalidateRect(m_cStatusBar.GetStatusHwnd(), nullptr, FALSE);
	}
}

SExtensionApplyEditResult CEditWnd::ApplyExtensionEdits(
	const std::vector<SExtensionDocumentEdit>& edits,
	std::vector<SExtensionDocumentSnapshot>& snapshots)
{
	if (!HasActiveEditorInput() || m_extensionDocumentVersion == 0) {
		return { EExtensionApplyEditStatus::UnknownDocument };
	}
	if (edits.size() != 1 || edits.front().documentId != SExtensionDocumentId{ ::GetCurrentProcessId(), 1 }) {
		return { EExtensionApplyEditStatus::UnknownDocument };
	}
	if (m_extensionDocumentSyncTimerPending) {
		::KillTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC);
		m_extensionDocumentSyncTimerPending = false;
		PublishExtensionDocumentChange();
		return { EExtensionApplyEditStatus::VersionMismatch };
	}
	auto& view = GetActiveView();
	if (view.GetCommander().GetOpeBlk() != nullptr || view.m_bDoing_UndoRedo) {
		return { EExtensionApplyEditStatus::CommandReentry };
	}
	CExtensionDocumentSync validationDocuments;
	if (validationDocuments.Open(CaptureExtensionDocumentSnapshot(m_extensionDocumentVersion)) !=
		EExtensionDocumentUpdateResult::Applied) {
		return { EExtensionApplyEditStatus::UnknownDocument };
	}
	CExtensionApplyEdit validation(validationDocuments);
	const auto validated = validation.Apply(edits);
	if (!validated.Applied()) return validated;
	const auto updated = validationDocuments.Snapshot(edits.front().documentId);
	if (!updated) return { EExtensionApplyEditStatus::UnknownDocument };

	auto& lineManager = GetDocument()->m_cDocLineMgr;
	const int lineCount = Int(lineManager.GetLineCount());
	const int lastLineIndex = (std::max)(0, lineCount - 1);
	const CDocLine* lastLine = lineManager.GetLine(CLogicInt(lastLineIndex));
	CLogicRange wholeDocument;
	wholeDocument.SetFrom(CLogicPoint(0, 0));
	wholeDocument.SetTo(CLogicPoint(lastLine ? lastLine->GetLengthWithEOL() : CLogicInt(0), lastLineIndex));
	auto* undo = new COpeBlk();
	view.GetCommander().SetOpeBlk(undo);
	undo->AddRef();
	view.ReplaceData_CEditView2(wholeDocument, updated->text.data(),
		CLogicInt(static_cast<int>(updated->text.size())), true, undo);
	view.SetUndoBuffer();
	if (m_extensionDocumentSyncTimerPending) {
		::KillTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC);
		m_extensionDocumentSyncTimerPending = false;
	}
	m_extensionDocumentVersion = updated->version;
	auto appliedSnapshot = CaptureExtensionDocumentSnapshot(m_extensionDocumentVersion);
	appliedSnapshot.dirty = true;
	snapshots.push_back(std::move(appliedSnapshot));
	return validated;
}

bool CEditWnd::ApplyExtensionEditorOptions(const SExtensionNativeEditorOptions& options)
{
	if (!HasActiveEditorInput() || m_extensionDocumentVersion == 0) return false;
	if (options.documentId != SExtensionDocumentId{ ::GetCurrentProcessId(), 1 }) return false;
	auto* document = GetDocument();
	if (!document) return false;
	if (options.insertSpaces) {
		document->m_cDocType.GetDocumentAttributeWrite().m_bInsSpace = *options.insertSpaces;
	}
	if (options.tabSize) {
		document->m_bTabSpaceCurTemp = true;
		auto& layout = document->m_cLayoutMgr;
		const CKetaXInt tabSize(static_cast<int>(*options.tabSize));
		if (layout.GetTabSpaceKetas() != tabSize) {
			ChangeLayoutParam(false, tabSize, layout.m_tsvInfo.m_nTsvMode, layout.GetMaxLineKetas());
		}
	}
	return true;
}

void CEditWnd::CloseWorkbench() noexcept
{
	// Model callbacks own only the shared gate. Disconnect it before removing
	// subscriptions so an in-flight callback cannot post into torn-down panels.
	CloseWorkbenchServiceProjection();
	if (m_layoutStateSubscription) m_layoutStateSubscription->Unsubscribe();
	m_layoutStateSubscription.reset();
	// Registry executors capture this window and must be gone before any host they
	// can project is closed. Context state has no external owner and is window-local.
	m_workbenchCommandRegistry.reset();
	m_workbenchContextKeyService.reset();
	if (m_editorCoreSubscription) m_editorCoreSubscription->Unsubscribe();
	m_editorCoreSubscription.reset();
	if (m_viewContainerPages && m_viewContainerPages->Marketplace()) {
		// Detach the composition-root callback before destroying the shared
		// Marketplace page; no selection event may reach a torn-down editor surface.
		m_viewContainerPages->Marketplace()->SetOnExtensionSelected({});
		m_viewContainerPages->Marketplace()->SetOnExtensionReadme({});
	}
	if (m_extensionDetailSurface) {
		m_extensionDetailSurface->SetOnInstallRequested({});
		m_extensionDetailSurface->SetOnCloseRequested({});
		m_extensionDetailSurface->ClearExtension();
		m_extensionDetailSurface->Destroy();
	}
	m_extensionDetailSurface.reset();
	if (m_emptyEditorSurface) m_emptyEditorSurface->Destroy();
	m_emptyEditorSurface.reset();
	ClosePublishedExtensionDocument();
	m_startupOutlineReloadPending = false;
	m_startupExtensionDocumentOpenPending = false;
	m_startupWorkbenchCompletionPosted = false;
	if (m_extensionDocumentSyncTimerPending) {
		::KillTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC);
		m_extensionDocumentSyncTimerPending = false;
	}
	if (m_extensionSidebarTool) {
		m_extensionSidebarTool->SetRequestChildrenCallback({});
		m_extensionSidebarTool->SetSelectionChangedCallback({});
		m_extensionSidebarTool->SetCheckboxChangedCallback({});
		m_extensionSidebarTool->SetCommandCallback({});
		m_extensionSidebarTool->SetVisibilityChangedCallback({});
	}
	if (m_extensionBottomPanelTool) {
		m_extensionBottomPanelTool->SetProblemActivationCallback({});
		m_extensionBottomPanelTool->SetOutputChannelSelectionCallback({});
		m_extensionBottomPanelTool->SetTabSelectionCallback({});
	}
	m_cStatusBar.SetExtensionCommandCallback({});
	// ステータスバーはレジストリを非所有で借りているだけなので、破棄より先に
	// 借用を明示的に返させる。宣言順に依存した暗黙のメンバー破棄順に頼らない。
	m_cStatusBar.SetExtensionIconFonts(nullptr);
	if (m_extensionService) m_extensionService->Shutdown();
	m_extensionService.reset();
	m_extensionIconFonts.reset();
	m_cStatusBar.SetExtensionItems({});
	if (m_activityBar) m_activityBar->Close();
	// The shared pages are borrowed by both side-bar hosts, so they must be destroyed
	// before either host window that may still be their parent.
	if (m_viewContainerPages) m_viewContainerPages->Close();
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->Close();
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->Close();
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->Close();
	m_activityBar.reset();
	m_leftWorkbenchPanel.reset();
	m_rightWorkbenchPanel.reset();
	m_bottomWorkbenchPanel.reset();
	m_sidebarHost = nullptr;
	m_auxiliaryBarHost = nullptr;
	m_viewContainerPages.reset();
	m_extensionSidebarTool = nullptr;
	m_extensionBottomPanelTool = nullptr;
	m_extensionViewRegistry.reset();
	m_explorerTool = nullptr;
	m_outlineWorkbenchTool = nullptr;
	m_scmTool = nullptr;
	m_terminalTool = nullptr;
	m_bottomWorkbenchMaximized = false;
	m_workspaceContext.reset();
}

void CEditWnd::ApplyWorkbenchTheme()
{
	const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark
		: theme::ThemeMode::Light;
	const auto palette = theme::CThemeService::EffectivePalette(mode);
	m_cStatusBar.SetPalette(palette);
	if (m_cTabWnd.GetHwnd()) m_cTabWnd.UpdateTheme();
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->SetPalette(palette);
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->SetPalette(palette);
	if (m_viewContainerPages) m_viewContainerPages->SetPalette(palette);
	if (m_sidebarHost) m_sidebarHost->SetPalette(palette);
	if (m_auxiliaryBarHost) m_auxiliaryBarHost->SetPalette(palette);
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->SetPalette(palette);
	if (m_extensionBottomPanelTool) m_extensionBottomPanelTool->SetPalette(palette);
	if (m_terminalTool) m_terminalTool->SetPalette(palette);
	if (m_markdownPreview) m_markdownPreview->SetPalette(palette);
	if (m_emptyEditorSurface) m_emptyEditorSurface->SetPalette(palette);
	if (m_extensionDetailSurface) m_extensionDetailSurface->SetPalette(palette);
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
		activityPalette.highContrast = theme::CThemeService::IsHighContrastActive();
		m_activityBar->SetPalette(activityPalette);
	}
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
		applyPanel(m_rightWorkbenchPanel.get(), settings.m_bExtensionViewsVisible,
			settings.m_nExtensionViewsExtent96);
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

bool CEditWnd::ApplyCurrentWorkbenchLayoutState(bool finalizeProjection,
	bool broadcastMirrorChanges, bool* mirrorChanged)
{
	if (mirrorChanged != nullptr) *mirrorChanged = false;
	if (m_workbenchRuntime == nullptr || m_leftWorkbenchPanel == nullptr
		|| m_bottomWorkbenchPanel == nullptr || m_rightWorkbenchPanel == nullptr) {
		return false;
	}

	workbench::layout::WorkbenchLayoutStateSnapshot snapshot;
	workbench::win32::BuiltinPartProjectionResult result;
	workbench::win32::BuiltinActiveSurfaceProjectionResult surfaceResult;
	try {
		snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		result = workbench::win32::ProjectBuiltinParts(snapshot);
		surfaceResult = workbench::win32::ProjectBuiltinActiveSurfaces(snapshot);
	}
	catch (...) {
		return false;
	}
	if (!result.Succeeded() || !surfaceResult.Succeeded()) return false;
	if (m_workbenchContextKeyService != nullptr) {
		const auto contextResult = m_workbenchContextKeyService->SetCoreProjection(snapshot);
		if (!contextResult.Succeeded()
			&& contextResult.status != workbench::commands::EWorkbenchContextMutationStatus::NotApplicable) {
			return false;
		}
	}

	const auto applyPart = [](workbench::CWorkbenchPanelHost& host,
		const workbench::win32::BuiltinPartProjectionState& part) {
		if (part.committedExtentDip) {
			host.ApplyExtentDip(static_cast<int>(*part.committedExtentDip));
		}
		if (part.visible) host.Show(); else host.Hide();
	};
	applyPart(*m_leftWorkbenchPanel, result.projection->left);
	applyPart(*m_bottomWorkbenchPanel, result.projection->bottom);
	applyPart(*m_rightWorkbenchPanel, result.projection->right);
	if (!result.projection->bottom.visible) m_bottomWorkbenchMaximized = false;

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
	updateVisible(settings.m_bLeftPanelVisible, result.projection->left.visible);
	updateExtent(settings.m_nLeftPanelExtent96, m_leftWorkbenchPanel->GetExtentDip());
	updateVisible(settings.m_bBottomPanelVisible, result.projection->bottom.visible);
	updateExtent(settings.m_nBottomPanelExtent96, m_bottomWorkbenchPanel->GetExtentDip());
	updateVisible(settings.m_bExtensionViewsVisible, result.projection->right.visible);
	updateExtent(settings.m_nExtensionViewsExtent96, m_rightWorkbenchPanel->GetExtentDip());
	if (mirrorChanged != nullptr) *mirrorChanged = changed;

	const bool surfacesApplied = ApplyBuiltinWorkbenchSurfaces(
		snapshot, *surfaceResult.projection);
	if (finalizeProjection) FinalizeWorkbenchPanelProjection(&*surfaceResult.projection);
	if (surfacesApplied && finalizeProjection) {
		ApplyBuiltinWorkbenchFocus(*surfaceResult.projection);
	}
	if (changed && broadcastMirrorChanges) BroadcastWorkbenchSettings();
	return surfacesApplied;
}

bool CEditWnd::ApplyBuiltinWorkbenchSurfaces(
	const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot,
	const workbench::win32::BuiltinActiveSurfaceProjection& projection)
{
	if (m_workbenchRuntime == nullptr || m_sidebarHost == nullptr || m_auxiliaryBarHost == nullptr
		|| m_extensionBottomPanelTool == nullptr || m_extensionSidebarTool == nullptr) {
		return false;
	}

	const auto outline = std::ranges::find(snapshot.views,
		workbench::layout::ids::view::Outline, &workbench::layout::WorkbenchViewState::viewId);
	if (outline != snapshot.views.end()) {
		SetOutlineExpandedInHosts(outline->visible);
	}

	// Both side bars are the same composite concept in VS Code, so each resolves its own
	// active container independently and a container may legitimately live in either one.
	std::optional<workbench::viewcontainer::ViewContainerPage> sidebarPage;
	std::optional<workbench::viewcontainer::ViewContainerPage> auxiliaryPage;
	if (projection.sidebar) {
		sidebarPage = SidebarPageForActiveSurface(*projection.sidebar);
		if (!sidebarPage) return false;
	}
	if (projection.auxiliaryBar) {
		auxiliaryPage = SidebarPageForActiveSurface(*projection.auxiliaryBar);
		if (!auxiliaryPage) return false;
	}
	// One ViewContainer has exactly one location; two hosts claiming it is incoherent.
	if (sidebarPage && auxiliaryPage && *sidebarPage == *auxiliaryPage) return false;
	ApplyAuxiliaryBarPage(auxiliaryPage);
	if (sidebarPage) {
		ApplySidebarPage(*sidebarPage);
	} else if (auxiliaryPage && m_sidebarHost->ActivePage() == auxiliaryPage) {
		// The container this side bar used to render has moved out, and no replacement is
		// active, so the Primary Side Bar is genuinely empty.
		m_sidebarHost->ShowPage(std::nullopt);
		RefreshSidebarTitles();
	}
	SyncActivityBarEntries(snapshot);

	if (projection.panel) {
		switch (*projection.panel) {
		case workbench::win32::BuiltinActiveSurface::Terminal:
			m_extensionBottomPanelTool->SetActiveTab(
				workbench::extension::ExtensionBottomPanelTab::Terminal);
			break;
		case workbench::win32::BuiltinActiveSurface::Problems:
			m_extensionBottomPanelTool->SetActiveTab(
				workbench::extension::ExtensionBottomPanelTab::Problems);
			break;
		case workbench::win32::BuiltinActiveSurface::Output:
			m_extensionBottomPanelTool->SetActiveTab(
				workbench::extension::ExtensionBottomPanelTab::Output);
			break;
		default:
			return false;
		}
	}

	const auto partVisible = [&snapshot](std::string_view partId) {
		const auto part = std::ranges::find(snapshot.parts, partId,
			&workbench::layout::WorkbenchPartState::partId);
		return part != snapshot.parts.end() && part->visible;
	};
	// The Activity Bar belongs to the Primary Side Bar. A container that moved to the
	// Secondary Side Bar has no Activity Bar entry at all, so it can never be selected here.
	std::optional<workbench::ActivityBarItem> activeItem;
	if (partVisible(workbench::layout::ids::part::Sidebar) && sidebarPage) {
		activeItem = ActivityBarItemForPage(*sidebarPage);
	}
	if (m_activityBar) m_activityBar->SetSelectedItem(activeItem);
	return true;
}

std::optional<workbench::viewcontainer::ViewContainerPage> CEditWnd::SidebarPageForActiveSurface(
	workbench::win32::BuiltinActiveSurface surface) noexcept
{
	using workbench::viewcontainer::ViewContainerPage;
	switch (surface) {
	case workbench::win32::BuiltinActiveSurface::Explorer:
	case workbench::win32::BuiltinActiveSurface::Outline:
		return ViewContainerPage::Explorer;
	case workbench::win32::BuiltinActiveSurface::SourceControl:
		return ViewContainerPage::SourceControl;
	case workbench::win32::BuiltinActiveSurface::Extensions:
		return ViewContainerPage::Extensions;
	default:
		break;
	}
	return std::nullopt;
}

void CEditWnd::ApplyBuiltinWorkbenchFocus(
	const workbench::win32::BuiltinActiveSurfaceProjection& projection)
{
	if (projection.focus) {
		switch (*projection.focus) {
		case workbench::win32::BuiltinActiveSurface::Explorer:
		case workbench::win32::BuiltinActiveSurface::SourceControl:
		case workbench::win32::BuiltinActiveSurface::Extensions: {
			// Focus follows the container, and the container decides which Part hosts it.
			const auto page = SidebarPageForActiveSurface(*projection.focus);
			auto* host = page ? PanelHostFor(HostShowingPage(*page)) : nullptr;
			if (host != nullptr) host->ActivateTool();
			break;
		}
		case workbench::win32::BuiltinActiveSurface::Outline:
			if (auto* explorerHost = HostShowingPage(
				workbench::viewcontainer::ViewContainerPage::Explorer)) {
				explorerHost->FocusOutline();
			}
			break;
		case workbench::win32::BuiltinActiveSurface::Terminal:
		case workbench::win32::BuiltinActiveSurface::Problems:
		case workbench::win32::BuiltinActiveSurface::Output:
			if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->ActivateTool();
			break;
		case workbench::win32::BuiltinActiveSurface::Editor:
			if (m_emptyEditorSurface != nullptr && m_emptyEditorSurface->IsVisible()) {
				m_emptyEditorSurface->Focus();
			} else if (HasActiveEditorInput() && GetActiveView().GetHwnd() != nullptr
				&& ::IsWindowVisible(GetActiveView().GetHwnd())) {
				::SetFocus(GetActiveView().GetHwnd());
			}
			break;
		}
	}
}

void CEditWnd::OnWorkbenchLayoutStateChanged()
{
	if (!m_layoutStateSubscription) return;
	if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
	if (!ApplyCurrentWorkbenchLayoutState(true, true)) {
		::OutputDebugStringW(L"Sakura Editor NEXT: committed workbench layout projection failed.\n");
	}
}

bool CEditWnd::InitializeWorkbenchServiceProjection()
{
	if (m_workbenchRuntime == nullptr || m_extensionBottomPanelTool == nullptr
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

void CEditWnd::OnWorkbenchServiceProjectionChanged()
{
	if (m_extensionBottomPanelTool == nullptr || m_markerService == nullptr || m_outputService == nullptr
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
		m_extensionBottomPanelTool->SetProblemsSnapshot(problems);
		m_extensionBottomPanelTool->SetOutputSnapshot(output);

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

void CEditWnd::FinalizeWorkbenchPanelProjection(
	const workbench::win32::BuiltinActiveSurfaceProjection* runtimeProjection)
{
	const auto& settings = m_pShareData->m_Common.m_sWorkbench;
	const bool leftVisible = m_leftWorkbenchPanel != nullptr
		&& m_leftWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;
	const bool rightVisible = m_rightWorkbenchPanel != nullptr
		&& m_rightWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;
	const bool bottomVisible = m_bottomWorkbenchPanel != nullptr
		&& m_bottomWorkbenchPanel->GetState() != workbench::WorkbenchPanelState::Hidden;

	if (m_workbenchRuntime == nullptr) {
		std::optional<workbench::ActivityBarItem> activeItem;
		switch (settings.m_eActiveTool) {
		case WORKBENCH_TOOL_EXPLORER:
			if (leftVisible) activeItem = workbench::ActivityBarItem::Explorer;
			break;
		case WORKBENCH_TOOL_OUTLINE:
			// Outline now lives inside Explorer.
			if (leftVisible) activeItem = workbench::ActivityBarItem::Explorer;
			break;
		case WORKBENCH_TOOL_TERMINAL:
			// Terminal is reached from the bottom panel/title-bar controls rather
			// than occupying a dedicated Activity Bar button.
			break;
		case WORKBENCH_TOOL_SCM:
			if (leftVisible) activeItem = workbench::ActivityBarItem::SourceControl;
			break;
		case WORKBENCH_TOOL_EXTENSIONS:
			if (leftVisible) activeItem = workbench::ActivityBarItem::Extensions;
			break;
		}
		// Secondary Side Bar visibility owns no Activity Bar item; every Activity Bar
		// ViewContainer lives in the Primary Side Bar, exactly like VS Code.
		(void)rightVisible;
		if (m_activityBar) m_activityBar->SetSelectedItem(activeItem);
	}
	ApplyWorkbenchTheme();
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}
	const bool terminalSelected = m_workbenchRuntime == nullptr
		? settings.m_eActiveTool == WORKBENCH_TOOL_TERMINAL
		: runtimeProjection != nullptr
			&& runtimeProjection->panel == workbench::win32::BuiltinActiveSurface::Terminal;
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

void CEditWnd::ReloadWorkbenchOutlineAndRelayout()
{
	const bool commandSucceeded = GetActiveView().GetCommander().Command_FUNCLIST(
		SHOW_RELOAD, OUTLINE_DEFAULT ) != FALSE;
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

std::wstring CEditWnd::GetSemanticWorkspaceRoot() const
{
	if (m_workbenchRuntime == nullptr) {
		return m_workspaceContext == nullptr ? std::wstring{} : m_workspaceContext->GetRoot();
	}

	const auto snapshot = m_workbenchRuntime->WorkspaceContext().Snapshot();
	// The native Explorer currently has one root. Do not silently collapse a
	// multi-root workspace to its first folder; that receives a real tree model
	// in the later view-container slice.
	if (snapshot.kind != config::EWorkspaceKind::Folder || snapshot.folders.size() != 1) return {};
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
	if (m_scmTool) m_scmTool->SetRoot(root);
	if (m_terminalTool) m_terminalTool->SetWorkingDirectory(m_workspaceContext->GetNewTerminalWorkingDirectory());
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
	if (!workbench::output::OutputService::IsValidOperationId(operationId)) return std::nullopt;
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
		|| viewId == workbench::layout::ids::view::SourceControl
		|| viewId == workbench::layout::ids::view::Terminal
		|| viewId == workbench::layout::ids::view::Problems
		|| viewId == workbench::layout::ids::view::Output
		|| viewId == workbench::layout::ids::view::Extensions;
	if (!supported) return false;
	try {
		auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		auto operationId = NextWorkbenchLayoutOperationId("activate-view");
		if (!operationId) return false;
		auto result = m_workbenchRuntime->LayoutState().ActivateView({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.viewId = std::string(viewId),
		});
		if (result.status != workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			&& result.status != workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
			return false;
		}
		snapshot = std::move(result.snapshot);

		const auto view = std::ranges::find(snapshot.views, viewId,
			&workbench::layout::WorkbenchViewState::viewId);
		if (view == snapshot.views.end()) return false;
		const auto container = std::ranges::find(snapshot.containers, view->containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end()) return false;
		const std::string resolvedContainerId = container->containerId;
		const std::string resolvedViewId = view->viewId;

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
		if (!part->visible) {
			operationId = NextWorkbenchLayoutOperationId("reveal-active-view-part");
			if (!operationId) return false;
			result = m_workbenchRuntime->LayoutState().SetPartVisibility({
				.operation = {
					.operationId = std::move(*operationId),
					.expectedRevision = snapshot.revision,
				},
				.partId = std::string(partId),
				.visible = true,
			});
			if (result.status != workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
				&& result.status != workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
				return false;
			}
			snapshot = std::move(result.snapshot);
		}

		if (!requestFocus) return true;
		operationId = NextWorkbenchLayoutOperationId("focus-active-view");
		if (!operationId) return false;
		result = m_workbenchRuntime->LayoutState().SetFocus({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.focus = {
				.partId = std::string(partId),
				.containerId = resolvedContainerId,
				.viewId = resolvedViewId,
			},
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

std::string_view CEditWnd::SidebarViewContainerId(
	workbench::viewcontainer::ViewContainerPage page) noexcept
{
	using workbench::viewcontainer::ViewContainerPage;
	switch (page) {
	case ViewContainerPage::SourceControl:
		return workbench::layout::ids::viewContainer::SourceControl;
	case ViewContainerPage::Extensions:
		return workbench::layout::ids::viewContainer::Extensions;
	case ViewContainerPage::Explorer:
	case ViewContainerPage::Count:
		break;
	}
	return workbench::layout::ids::viewContainer::Explorer;
}

std::optional<workbench::viewcontainer::ViewContainerPage> CEditWnd::ViewContainerPageForId(
	std::string_view containerId) noexcept
{
	using workbench::viewcontainer::ViewContainerPage;
	if (containerId == workbench::layout::ids::viewContainer::Explorer) return ViewContainerPage::Explorer;
	if (containerId == workbench::layout::ids::viewContainer::SourceControl) {
		return ViewContainerPage::SourceControl;
	}
	if (containerId == workbench::layout::ids::viewContainer::Extensions) {
		return ViewContainerPage::Extensions;
	}
	return std::nullopt;
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
		const auto result = m_workbenchContextKeyService->SetCoreProjection(
			m_workbenchRuntime->LayoutState().Snapshot());
		return result.Succeeded()
			|| result.status == workbench::commands::EWorkbenchContextMutationStatus::NotApplicable;
	}
	catch (...) {
		return false;
	}
}

bool CEditWnd::TryExecuteWorkbenchStableCommand(std::string_view commandId, bool& handled)
{
	handled = false;
	if (m_workbenchCommandRegistry == nullptr || m_workbenchContextKeyService == nullptr) return false;
	if (!m_workbenchCommandRegistry->Find(commandId)) return false;
	handled = true;
	if (!RefreshWorkbenchCommandContext()) {
		::OutputDebugStringW(L"Sakura Editor NEXT: workbench command context refresh failed.\n");
		return false;
	}

	const auto result = m_workbenchCommandRegistry->Execute(commandId, m_workbenchContextKeyService->Snapshot());
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

void CEditWnd::SetWorkbenchPanelVisible(workbench::WorkbenchEdge edge, bool visible, bool activate)
{
	if (m_workbenchRuntime != nullptr) {
		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		bool mirrorChanged = false;

		if (edge == workbench::WorkbenchEdge::Right) {
			// This is the legacy Outline nested inside the left Sidebar. It is not
			// the physical VS Code Auxiliary Bar hosted on the right.
			if (m_viewContainerPages == nullptr || m_leftWorkbenchPanel == nullptr) return;
			const bool committed = visible && activate
				? ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Outline, true)
				: (!visible || SetBuiltinPartVisibility(workbench::layout::ids::part::Sidebar, true))
					&& SetBuiltinViewVisibility(workbench::layout::ids::view::Outline, visible);
			if (!committed) return;
			if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
				::OutputDebugStringW(L"Sakura Editor NEXT: Outline reveal projection failed.\n");
				return;
			}
			if (mirrorChanged) BroadcastWorkbenchSettings();
			return;
		}

		const std::string_view partId = edge == workbench::WorkbenchEdge::Left
			? workbench::layout::ids::part::Sidebar : workbench::layout::ids::part::Panel;
		const std::string_view viewId = edge == workbench::WorkbenchEdge::Left
			? workbench::layout::ids::view::Explorer : workbench::layout::ids::view::Terminal;
		const bool committed = visible && activate
			? ActivateBuiltinWorkbenchView(viewId, true)
			: SetBuiltinPartVisibility(partId, visible);
		if (!committed) return;
		if (!ApplyCurrentWorkbenchLayoutState(true, false, &mirrorChanged)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: workbench visibility projection failed.\n");
			return;
		}
		if (mirrorChanged) BroadcastWorkbenchSettings();
		return;
	}

	workbench::CWorkbenchPanelHost* host = nullptr;
	BOOL* savedVisible = nullptr;
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	std::optional<workbench::ActivityBarItem> item;
	switch (edge) {
	case workbench::WorkbenchEdge::Left:
		host = m_leftWorkbenchPanel.get();
		savedVisible = &settings.m_bLeftPanelVisible;
		item = workbench::ActivityBarItem::Explorer;
		break;
	case workbench::WorkbenchEdge::Right:
		host = m_leftWorkbenchPanel.get();
		savedVisible = &settings.m_bRightPanelVisible;
		item = workbench::ActivityBarItem::Explorer;
		break;
	case workbench::WorkbenchEdge::Bottom:
		host = m_bottomWorkbenchPanel.get();
		savedVisible = &settings.m_bBottomPanelVisible;
		break;
	}
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
			ApplySidebarPage(workbench::viewcontainer::ViewContainerPage::Explorer);
		}
		if (visible) host->Show(); else host->Hide();
	}
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}
	// Materialize a newly shown tool only after the host has received its real
	// bounds.  In particular, ConPTY must not be started from the host's initial
	// empty rectangle, which would create a 1x1 pseudo console and lose the
	// shell's startup output before the first resize.
	if (host != nullptr && visible && activate) {
		if (edge == workbench::WorkbenchEdge::Right && m_viewContainerPages) {
			if (m_dispatchReady) ReloadWorkbenchOutlineAndRelayout();
			if (auto* explorerHost = HostShowingPage(
				workbench::viewcontainer::ViewContainerPage::Explorer)) {
				explorerHost->FocusOutline();
			}
		} else {
			host->ActivateTool();
		}
	}
	if (m_activityBar && visible && activate) m_activityBar->SetSelectedItem(item);
	if (visibilityChanged || leftVisibilityChanged || oldActiveTool != settings.m_eActiveTool) {
		BroadcastWorkbenchSettings();
	}
}

void CEditWnd::ActivateSidebarPage(workbench::viewcontainer::ViewContainerPage page, bool toggleIfActive)
{
	if (!m_leftWorkbenchPanel || !m_sidebarHost) return;
	using workbench::viewcontainer::ViewContainerPage;
	std::string_view requestedView = workbench::layout::ids::view::Explorer;
	auto legacyTool = WORKBENCH_TOOL_EXPLORER;
	const auto activityItem = ActivityBarItemForPage(page);
	switch (page) {
	case ViewContainerPage::SourceControl:
		requestedView = workbench::layout::ids::view::SourceControl;
		legacyTool = WORKBENCH_TOOL_SCM;
		break;
	case ViewContainerPage::Extensions:
		requestedView = workbench::layout::ids::view::Extensions;
		legacyTool = WORKBENCH_TOOL_EXTENSIONS;
		break;
	case ViewContainerPage::Explorer:
	case ViewContainerPage::Count:
		break;
	}
	// The toggle compares ViewContainers exactly as VS Code does, so an Outline selection inside
	// the Explorer container still counts as "Explorer is already active".
	const bool alreadyActive = m_workbenchRuntime != nullptr
		? IsSidebarViewContainerActive(SidebarViewContainerId(page))
		: IsWorkbenchPanelVisible(workbench::WorkbenchEdge::Left)
			&& m_pShareData->m_Common.m_sWorkbench.m_eActiveTool == legacyTool;
	if (toggleIfActive && alreadyActive) {
		SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Left, false, false);
		if (m_workbenchRuntime == nullptr && m_activityBar) {
			m_activityBar->SetSelectedItem(std::nullopt);
		}
		return;
	}
	if (m_workbenchRuntime != nullptr) {
		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		if (!ActivateBuiltinWorkbenchView(requestedView, true)) return;
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
	ApplySidebarPage(page);
	m_leftWorkbenchPanel->Show();
	if (GetHwnd()) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}
	m_leftWorkbenchPanel->ActivateTool();
	if (m_activityBar) m_activityBar->SetSelectedItem(activityItem);
	BroadcastWorkbenchSettings();
}

void CEditWnd::ApplySidebarPage(workbench::viewcontainer::ViewContainerPage page)
{
	if (m_sidebarHost == nullptr) return;
	// A ViewContainer has exactly one location, so the side bar that just gained it takes
	// the page window away from the other one instead of both claiming to render it.
	if (m_auxiliaryBarHost != nullptr && m_auxiliaryBarHost->ActivePage() == page) {
		m_auxiliaryBarHost->ShowPage(std::nullopt);
	}
	m_sidebarHost->ShowPage(page);
	RefreshSidebarTitles();
}

void CEditWnd::ApplyAuxiliaryBarPage(std::optional<workbench::viewcontainer::ViewContainerPage> page)
{
	if (m_auxiliaryBarHost == nullptr) return;
	if (page && m_sidebarHost != nullptr && m_sidebarHost->ActivePage() == page) {
		m_sidebarHost->ShowPage(std::nullopt);
	}
	m_auxiliaryBarHost->ShowPage(page);
	RefreshSidebarTitles();
}

void CEditWnd::RefreshSidebarTitles()
{
	// The host header is the VS Code ViewContainer title, so it follows whichever container
	// that side bar currently renders.
	if (m_leftWorkbenchPanel && m_sidebarHost != nullptr) {
		const auto page = m_sidebarHost->ActivePage();
		m_leftWorkbenchPanel->SetTitle(page
			? workbench::viewcontainer::CViewContainerPages::PageTitle(*page)
			: L"");
	}
	if (m_rightWorkbenchPanel && m_auxiliaryBarHost != nullptr) {
		const auto page = m_auxiliaryBarHost->ActivePage();
		m_rightWorkbenchPanel->SetTitle(page
			? workbench::viewcontainer::CViewContainerPages::PageTitle(*page)
			: L"SECONDARY SIDE BAR");
	}
}

workbench::viewcontainer::CViewContainerHost* CEditWnd::HostShowingPage(
	workbench::viewcontainer::ViewContainerPage page) const noexcept
{
	if (m_sidebarHost != nullptr && m_sidebarHost->ActivePage() == page) return m_sidebarHost;
	if (m_auxiliaryBarHost != nullptr && m_auxiliaryBarHost->ActivePage() == page) {
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
	const auto* host = HostShowingPage(workbench::viewcontainer::ViewContainerPage::Explorer);
	if (host == nullptr) return false;
	const auto* panel = PanelHostFor(host);
	return panel != nullptr
		&& panel->GetState() != workbench::WorkbenchPanelState::Hidden
		&& host->IsOutlineExpanded();
}

void CEditWnd::SetOutlineExpandedInHosts(bool expanded)
{
	if (auto* host = HostShowingPage(workbench::viewcontainer::ViewContainerPage::Explorer)) {
		host->SetOutlineExpanded(expanded);
		return;
	}
	// No side bar renders the Explorer container right now, so only the shared model fact
	// changes; the next host to receive that container lays out from it.
	if (m_viewContainerPages) m_viewContainerPages->SetOutlineExpanded(expanded);
}

void CEditWnd::SyncActivityBarEntries(const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot)
{
	if (!m_activityBar) return;
	// VS Code moves the whole composite entry together with its ViewContainer: a container
	// that now lives in the Secondary Side Bar has no Activity Bar icon at all. A greyed-out
	// placeholder would be a fake capability, so the entry is removed instead.
	for (std::size_t index = 0; index < workbench::viewcontainer::kViewContainerPageCount; ++index) {
		const auto page = static_cast<workbench::viewcontainer::ViewContainerPage>(index);
		const auto container = std::ranges::find(snapshot.containers, SidebarViewContainerId(page),
			&workbench::layout::WorkbenchViewContainerState::containerId);
		const bool inSideBar = container != snapshot.containers.end()
			&& container->location == workbench::layout::EWorkbenchViewContainerLocation::SideBar;
		m_activityBar->SetItemVisible(ActivityBarItemForPage(page), inSideBar);
	}
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

void CEditWnd::MoveViewContainerToEdge(workbench::viewcontainer::ViewContainerPage page,
	workbench::WorkbenchEdge edge)
{
	// Only the two side bars are composite drop targets here. Moving a ViewContainer into
	// the Panel is VS Code's separate `workbench.action.movePanelTo*` family and is not
	// approximated by this gesture.
	if (edge != workbench::WorkbenchEdge::Left && edge != workbench::WorkbenchEdge::Right) return;
	if (m_workbenchRuntime == nullptr) return;
	const auto location = edge == workbench::WorkbenchEdge::Right
		? workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar
		: workbench::layout::EWorkbenchViewContainerLocation::SideBar;
	const auto containerId = SidebarViewContainerId(page);
	try {
		auto snapshot = m_workbenchRuntime->LayoutState().Snapshot();
		const auto container = std::ranges::find(snapshot.containers, containerId,
			&workbench::layout::WorkbenchViewContainerState::containerId);
		if (container == snapshot.containers.end() || container->location == location) return;

		if (m_resizingWorkbenchPanel != nullptr) CancelWorkbenchResize();
		auto operationId = NextWorkbenchLayoutOperationId("move-view-container");
		if (!operationId) return;
		auto result = m_workbenchRuntime->LayoutState().MoveContainer({
			.operation = {
				.operationId = std::move(*operationId),
				.expectedRevision = snapshot.revision,
			},
			.containerId = std::string(containerId),
			.location = location,
			.order = container->order,
		});
		if (result.status != workbench::layout::EWorkbenchLayoutOperationStatus::Succeeded
			&& result.status != workbench::layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
			return;
		}
		snapshot = std::move(result.snapshot);

		// VS Code's `CompositeDragAndDrop.drop` opens the dropped composite in its new home,
		// which also reveals that Part when it was hidden.
		const std::string_view viewId = page == workbench::viewcontainer::ViewContainerPage::SourceControl
			? workbench::layout::ids::view::SourceControl
			: page == workbench::viewcontainer::ViewContainerPage::Extensions
				? workbench::layout::ids::view::Extensions
				: workbench::layout::ids::view::Explorer;
		if (!ActivateBuiltinWorkbenchView(viewId, true)) return;

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

void CEditWnd::PersistExtensionViewsExtent(int extentDip)
{
	auto& settings = m_pShareData->m_Common.m_sWorkbench;
	if (settings.m_nExtensionViewsExtent96 == extentDip) return;
	settings.m_nExtensionViewsExtent96 = extentDip;
	BroadcastWorkbenchSettings();
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
	settings.m_bExtensionViewsVisible = visible ? FALSE : TRUE;
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
	if (m_workbenchRuntime == nullptr && edge == workbench::WorkbenchEdge::Right
		&& show && m_dispatchReady) {
		(void)GetActiveView().GetCommander().Command_FUNCLIST(SHOW_RELOAD, OUTLINE_DEFAULT);
	}
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

void CEditWnd::OpenWorkspaceFolder()
{
	if (!m_workspaceContext) return;

	// SelectDir uses the native IFileDialog with FOS_PICKFOLDERS and
	// FOS_FORCEFILESYSTEM. Keep all state intact when the user cancels or the
	// dialog cannot return a filesystem path.
	std::array<WCHAR, 32768> selectedDirectory{};
	auto initialDirectory = GetSemanticWorkspaceRoot();
	if (initialDirectory.empty()) initialDirectory = m_workspaceContext->GetNewTerminalWorkingDirectory();
	if (!SelectDir(GetHwnd(), L"作業フォルダーを開く", initialDirectory, selectedDirectory)) return;

	const auto absoluteRoot = MakeAbsolutePath(selectedDirectory.data());
	const DWORD attributes = ::GetFileAttributesW(absoluteRoot.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return;

	if (m_workbenchRuntime != nullptr) {
		auto uri = platform::uri::Uri::FromWindowsPath(absoluteRoot);
		if (!uri) return;
		const std::filesystem::path selectedPath(absoluteRoot);
		auto displayName = selectedPath.filename().native();
		if (displayName.empty()) displayName = selectedPath.root_name().native();
		if (displayName.empty()) return;
		const auto before = m_workbenchRuntime->WorkspaceContext().Snapshot();
		config::SetFolderRequest request {
			.operation = {
				.operationId = NextEditorOperationId("workspace.openFolder"),
				.expectedRevision = before.revision,
			},
			.folderUri = std::move(*uri.value),
			.displayName = std::move(displayName),
		};
		const auto result = m_workbenchRuntime->WorkspaceContext().SetFolder(request);
		if (result.outcome != config::EWorkspaceContextOutcome::Succeeded
			&& result.outcome != config::EWorkspaceContextOutcome::NotApplicable) return;
		ApplySemanticWorkspaceContext();
	} else {
		// Unit-only/legacy construction keeps the pre-runtime fallback.
		m_workspaceContext->SetExplicitRoot(absoluteRoot);
		ApplySemanticWorkspaceContext();
	}
	SetWorkbenchPanelVisible(workbench::WorkbenchEdge::Left, true, true);
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

bool CEditWnd::EnsureMarkdownPreview()
{
	if (m_markdownPreview && m_markdownPreview->IsCreated()) {
		return true;
	}
	m_markdownPreview = std::make_unique<markdown::CMarkdownPreviewWnd>();
	if (!m_markdownPreview->Create(GetHwnd())) {
		m_markdownPreview.reset();
		return false;
	}
	const auto mode = m_pShareData->m_Common.m_sWindow.m_bDarkMode
		? theme::ThemeMode::Dark : theme::ThemeMode::Light;
	m_markdownPreview->SetPalette(theme::CThemeService::EffectivePalette(mode));
	const auto dpi = GetHwnd() == nullptr ? 96U : ::GetDpiForWindow(GetHwnd());
	m_markdownPreview->SetEditorFont(GetLogfont(), dpi);
	return true;
}

void CEditWnd::CloseMarkdownPreview() noexcept
{
	m_markdownPreviewVisible = false;
	m_markdownPreviewDirty = false;
	m_markdownPreviewRevision = -1;
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
	bool truncated = false;
	m_markdownPreview->SetDocument(markdown::ParseMarkdown(GetMarkdownPreviewSource(&truncated)));
	m_markdownPreview->SetSourceTruncated(truncated);
	m_markdownPreviewRevision = GetDocument()->m_cDocEditor.m_cOpeBuf.GetCurrentPointer();
	m_markdownPreviewDirty = false;
}

void CEditWnd::UpdateMarkdownPreviewIfNeeded()
{
	if (!m_markdownPreviewVisible || !m_markdownPreview) {
		return;
	}
	if (!IsMarkdownPreviewAvailable()) {
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

void CEditWnd::LayoutMarkdownPreview(int left, int top, int right, int bottom, unsigned int dpi)
{
	const RECT previousDivider = m_markdownPreviewDivider;
	if (!HasActiveEditorInput()) {
		m_markdownPreviewDivider = {};
		if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), &previousDivider, FALSE);
		if (const HWND splitter = m_cSplitterWnd.GetHwnd(); splitter != nullptr) {
			::ShowWindow(splitter, SW_HIDE);
		}
		if (m_markdownPreview) m_markdownPreview->Show(false);
		if (m_extensionDetailSurface && m_extensionDetailSurface->HasExtension()) {
			m_extensionDetailSurface->Layout({ left, top, right, bottom }, dpi);
			if (!m_pPrintPreview) m_extensionDetailSurface->Show();
			if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
		} else if (m_emptyEditorSurface) {
			m_emptyEditorSurface->Layout({ left, top, right, bottom }, dpi);
			if (!m_pPrintPreview) m_emptyEditorSurface->Show();
		}
		return;
	}
	if (m_extensionDetailSurface) m_extensionDetailSurface->Hide();
	if (m_emptyEditorSurface) m_emptyEditorSurface->Hide();
	if (const HWND splitter = m_cSplitterWnd.GetHwnd(); splitter != nullptr && !m_pPrintPreview) {
		::ShowWindow(splitter, SW_SHOWNA);
	}
	const bool showPreview = m_markdownPreviewVisible && m_markdownPreview != nullptr && !m_pPrintPreview;
	const auto layout = markdown::CalculateMarkdownPreviewLayout(left, right, dpi, showPreview);
	m_markdownPreviewDivider = { layout.dividerLeft, top, layout.dividerRight, bottom };
	if (!showPreview || layout.PreviewWidth() == 0) {
		m_markdownPreviewDivider = {};
	}
	if (GetHwnd() != nullptr) {
		::InvalidateRect(GetHwnd(), &previousDivider, FALSE);
		::InvalidateRect(GetHwnd(), &m_markdownPreviewDivider, FALSE);
	}
	::MoveWindow(m_cSplitterWnd.GetHwnd(), layout.editorLeft, top,
		layout.EditorWidth(), std::max(0, bottom - top), TRUE);
	if (!m_markdownPreview) {
		return;
	}
	const RECT previewBounds{ layout.previewLeft, top, layout.previewRight, bottom };
	m_markdownPreview->SetEditorFont(GetLogfont(), dpi);
	m_markdownPreview->Layout(previewBounds, dpi);
	m_markdownPreview->Show(showPreview && layout.PreviewWidth() > 0);
}

void CEditWnd::ToggleMarkdownPreview()
{
	if (!IsMarkdownPreviewAvailable()) {
		return;
	}
	m_markdownPreviewVisible = !m_markdownPreviewVisible;
	if (m_markdownPreviewVisible) {
		if (!EnsureMarkdownPreview()) {
			m_markdownPreviewVisible = false;
			return;
		}
		m_markdownPreviewDirty = true;
		m_markdownPreviewRevision = -1;
		RefreshMarkdownPreview();
	} else if (m_markdownPreview) {
		m_markdownPreview->Show(false);
	}
	m_cTabWnd.RefreshDocumentActionState();
	if (GetHwnd() != nullptr) {
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType, MAKELONG(client.right - client.left, client.bottom - client.top), false);
	}
}

bool CEditWnd::PreTranslateWorkbenchMessage(MSG& message)
{
	if (m_emptyEditorSurface && m_emptyEditorSurface->PreTranslateMessage(message)) return true;
	if (message.message == WM_KEYDOWN && (::GetKeyState(VK_CONTROL) & 0x8000) != 0
		&& (::GetKeyState(VK_MENU) & 0x8000) == 0) {
		if (message.wParam == L'P' && (::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
			ShowExtensionCommandPalette();
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
	if (m_resizingWorkbenchPanel != nullptr && message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
		CancelWorkbenchResize();
		return true;
	}
	if (m_activityBar && m_activityBar->PreTranslateMessage(message)) return true;
	if (m_bottomWorkbenchPanel && m_bottomWorkbenchPanel->PreTranslateMessage(message)) return true;
	if (m_leftWorkbenchPanel && m_leftWorkbenchPanel->PreTranslateMessage(message)) return true;
	return m_rightWorkbenchPanel && m_rightWorkbenchPanel->PreTranslateMessage(message);
}

void CEditWnd::ShowExtensionCommandPalette()
{
	if (!m_extensionService || !GetHwnd()) return;
	m_extensionService->Start();
	const auto commands = m_extensionService->SearchCommands(L"");
	SExtensionQuickInputRequest request;
	request.kind = EExtensionQuickInputKind::QuickPick;
	request.title = L"Command Palette";
	request.placeholder = L"実行するコマンドを選択してください (Ctrl+Shift+P)";
	request.items.reserve(commands.size());
	for (std::size_t index = 0; index < commands.size(); ++index) {
		const auto& command = commands[index];
		request.items.push_back({
			.sourceIndex = index,
			.label = command.enabled ? command.label : L"(無効) " + command.label,
			.description = command.detail,
			.detail = command.id,
		});
	}
	if (request.items.empty()) {
		m_cStatusBar.SetStatusText(0, SBT_NOBORDERS, L"利用できる拡張コマンドはありません");
		return;
	}
	CExtensionQuickInputDialog dialog(request);
	const auto completion = dialog.DoModal(GetHwnd());
	if (completion.state != EExtensionQuickInputState::Accepted || completion.selectedIndices.size() != 1) return;
	const auto selected = completion.selectedIndices.front();
	if (selected >= commands.size() || !commands[selected].enabled) return;
	m_extensionService->ExecuteCommand(commands[selected].id);
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
		m_startupExtensionDocumentOpenPending = true;
		m_startupOutlineReloadPending =
			m_pShareData->m_Common.m_sWorkbench.m_bRightPanelVisible != FALSE;
	} else {
		// The startup gate is fully satisfied; later loads publish synchronously.
		m_startupExtensionDocumentOpenPending = false;
		PublishExtensionDocumentOpen(true);
	}
	if (m_markdownPreviewVisible) {
		if (IsMarkdownPreviewAvailable()) {
			m_markdownPreviewDirty = true;
			m_markdownPreviewRevision = -1;
			RefreshMarkdownPreview();
		} else if (m_markdownPreview) {
			m_markdownPreviewVisible = false;
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
	PublishExtensionDocumentSave();
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
		CEditApp::getInstance()->m_pcGrepAgent->m_bGrepMode ); // 2006.01.28 ryoji ファイル名、Grepモードパラメータを追加
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
		TopErrorMessage(GetHwnd(), L"ワークベンチの初期化に失敗しました。\nFailed to initialize the workbench.");
		AbortStartupDrawTransaction();
		::DestroyWindow(GetHwnd());
		m_hWnd = hWnd = nullptr;
		return hWnd;
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
			case F_FILE_USED_RECENTLY:		// 最近使ったファイル
				{
					CRecentFile	cRecentFile;
					nCount = cRecentFile.GetViewCount();
				}
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
	if( m_pShareData->m_Common.m_sWindow.m_bDispMiniMap ){	/* タブバーを表示する */
		if( !m_cMiniMapView.GetHwnd() ){
			m_cMiniMapView.Create( GetHwnd() );
		}
	}else{
		if( m_cMiniMapView.GetHwnd() ){
			m_cMiniMapView.Close();
		}
	}
}

//! Reveals `workbench.view.extensions`, matching VS Code's reveal-only `workbench.view.*`.
void CEditWnd::ShowExtensionsViewContainer()
{
	// VS Code's `workbench.view.*` commands only ever reveal a container; hiding belongs
	// to the Activity Bar click gesture (`workbench.action.toggleSidebarVisibility`).
	ActivateSidebarPage( workbench::viewcontainer::ViewContainerPage::Extensions, false );
}

//! True while the Extensions ViewContainer is the active, visible container of its Part.
bool CEditWnd::IsExtensionsViewContainerActive() const
{
	// IsBuiltinWorkbenchViewActive resolves the container's real location (Primary Side
	// Bar, Panel, or Secondary Side Bar), so a container the user moved to the Secondary
	// Side Bar still reports correctly here.
	return IsBuiltinWorkbenchViewActive( workbench::layout::ids::view::Extensions );
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
		::RedrawWindow( GetHwnd(), nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW );	// ステータスバーに必要？

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
	if (uMsg == WM_WINDOWPOSCHANGING && IsStartupDrawSuppressed()) {
		if (auto* position = reinterpret_cast<WINDOWPOS*>(lParam)) {
			position->flags &= ~SWP_SHOWWINDOW;
		}
	}
	LRESULT customFrameResult = 0;
	if (m_customFrame && m_customFrame->HandleWindowMessage(uMsg, wParam, lParam, customFrameResult)) {
		return customFrameResult;
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
	case MYWM_EXTENSION_WORKBENCH_CHANGED:
		if (m_extensionService) {
			const auto changes = static_cast<EExtensionWorkbenchChange>(wParam);
			const auto bits = static_cast<std::uint32_t>(changes);
			if ((bits & static_cast<std::uint32_t>(EExtensionWorkbenchChange::StatusBar)) != 0) {
				m_cStatusBar.SetExtensionItems(m_extensionService->StatusBarItems());
			}
			if ((bits & static_cast<std::uint32_t>(EExtensionWorkbenchChange::Views)) != 0 && m_extensionSidebarTool) {
				m_extensionSidebarTool->Refresh();
			}
			if ((bits & static_cast<std::uint32_t>(EExtensionWorkbenchChange::Diagnostics)) != 0) {
				Views_DeleteCompatibleBitmap();
				RedrawAllViews(nullptr);
			}
			if ((bits & static_cast<std::uint32_t>(EExtensionWorkbenchChange::Progress)) != 0) {
				m_cStatusBar.SetExtensionItems(m_extensionService->StatusBarItems());
			}
		}
		return 0;
	case MYWM_EXTENSION_NOTIFICATION_PROMPT:
		return m_extensionService ? m_extensionService->HandleNotificationPrompt(lParam) : 0;
	case MYWM_EXTENSION_QUICK_INPUT_PROMPT:
		return m_extensionService ? m_extensionService->HandleQuickInputPrompt(lParam) : 0;
	case MYWM_EXTENSION_APPLY_EDIT_PROMPT:
		return m_extensionService ? m_extensionService->HandleApplyEditPrompt(lParam) : 0;
	case MYWM_EXTENSION_EDITOR_OPTIONS_PROMPT:
		return m_extensionService ? m_extensionService->HandleEditorOptionsPrompt(lParam) : 0;
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
		return 0;
	case WM_MOUSEWHEEL:
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
	case WM_SHOWWINDOW:
		if( !wParam ){
			Views_DeleteCompatibleBitmap();
		}
		return ::DefWindowProc( hwnd, uMsg, wParam, lParam );

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
	case WM_PAINT:
		return OnPaint( hwnd, uMsg, wParam, lParam );

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

	case WM_ACTIVATEAPP:
		m_bIsActiveApp = (wParam != 0);	// 自アプリがアクティブかどうか
		if (m_extensionService) {
			m_extensionService->SetWindowState(m_bIsActiveApp);
			if (m_bIsActiveApp) PublishExtensionActiveEditor();
		}

		// アクティブ化なら編集ウィンドウリストの先頭に移動する		// 2007.04.08 ryoji WM_SETFOCUS から移動
		if( m_bIsActiveApp ){
			CAppNodeGroupHandle(0).AddEditWndList( GetHwnd() );	// リスト移動処理

			// 2009.01.17 nasukoji	ホイールスクロール有無状態をクリア
			ClearMouseState();
		}

		// キャプション設定、タイマーON/OFF		// 2007.03.08 ryoji WM_ACTIVATEから移動
		UpdateCaption();
		m_cFuncKeyWnd.Timer_ONOFF( m_bIsActiveApp ); // 20060126 aroka
		this->Timer_ONOFF( m_bIsActiveApp ); // 20060128 aroka

		return 0L;

	case WM_ENABLE:
		// 右ドロップファイルの受け入れ設定／解除	// 2009.01.09 ryoji
		// Note: DragAcceptFilesを適用した左ドロップについては Enable/Disable で自動的に受け入れ設定／解除が切り替わる
		if( (BOOL)wParam ){
			m_pcDropTarget->Register_DropTarget( m_hWnd );
		}else{
			m_pcDropTarget->Revoke_DropTarget();
		}
		return 0L;

	case WM_WINDOWPOSCHANGED:
		// ポップアップウィンドウの表示切替指示をポストする	// 2007.10.22 ryoji
		// ・WM_SHOWWINDOWはすべての表示切替で呼ばれるわけではないのでWM_WINDOWPOSCHANGEDで処理
		//   （タブグループ解除などの設定変更時はWM_SHOWWINDOWは呼ばれない）
		// ・即時切替だとタブ切替に干渉して元のタブに戻ってしまうことがあるので後で切り替える
		if (const auto pwp = (WINDOWPOS*)lParam;
			pwp->flags & SWP_SHOWWINDOW)
			::PostMessage( hwnd, MYWM_SHOWOWNEDPOPUPS, TRUE, 0 );
		else if( pwp->flags & SWP_HIDEWINDOW )
			::PostMessage( hwnd, MYWM_SHOWOWNEDPOPUPS, FALSE, 0 );

		return ::DefWindowProc( hwnd, uMsg, wParam, lParam );

	case MYWM_SHOWOWNEDPOPUPS:
		::ShowOwnedPopups( m_hWnd, (BOOL)wParam );	// 2007.10.22 ryoji
		return 0L;

	case WM_SIZE:
//		MYTRACE( L"WM_SIZE\n" );
		/* WM_SIZE 処理 */
		if( SIZE_MINIMIZED == wParam ){
			this->UpdateCaption();
		}
		return OnSize( wParam, lParam );

	//From here 2003.05.31 MIK
	case WM_MOVE:
		// From Here 2004.05.13 Moca ウィンドウ位置継承
		//	最後の位置を復元するため，移動されるたびに共有メモリに位置を保存する．
		if (WINSIZEMODE_SAVE == m_pShareData->m_Common.m_sWindow.m_eSaveWindowPos &&
			!::IsZoomed(hWnd) &&
			!::IsIconic(hWnd)) {
				// 2005.11.23 Moca ワークエリア座標だとずれるのでスクリーン座標に変更
				// Aero Snapで縦方向最大化で終了して次回起動するときは元のサイズにする必要があるので、
				// GetWindowRect()ではなくGetWindowPlacement()で得たワークエリア座標をスクリーン座標に変換して記憶する	// 2009.09.02 ryoji
				RECT rcWin;
				WINDOWPLACEMENT wp;
				wp.length = sizeof(wp);
				::GetWindowPlacement( GetHwnd(), &wp );	// ワークエリア座標
				rcWin = wp.rcNormalPosition;
				RECT rcWork, rcMon;
				GetMonitorWorkRect( GetHwnd(), &rcWork, &rcMon );
				::OffsetRect(&rcWin, rcWork.left - rcMon.left, rcWork.top - rcMon.top);	// スクリーン座標に変換
				m_pShareData->m_Common.m_sWindow.m_nWinPosX = rcWin.left;
				m_pShareData->m_Common.m_sWindow.m_nWinPosY = rcWin.top;
		}
		// To Here 2004.05.13 Moca ウィンドウ位置継承
		return DefWindowProc( hwnd, uMsg, wParam, lParam );
	//To here 2003.05.31 MIK
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

	case WM_SETFOCUS:
//		MYTRACE( L"WM_SETFOCUS\n" );

		// Aug. 29, 2003 wmlhq & ryojiファイルのタイムスタンプのチェック処理 OnTimer に移行
		m_nTimerCount = 9;

		// ビューにフォーカスを移動する	// 2007.10.16 ryoji
		if( !m_pPrintPreview ){
			if (HasActiveEditorInput()) {
				::SetFocus(GetActiveView().GetHwnd());
			} else if (m_emptyEditorSurface) {
				m_emptyEditorSurface->Focus();
			}
		}
		lRes = 0;

//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
		/* 印刷プレビューモードのときは、キー操作は全部PrintPreviewBarへ転送 */
		if( m_pPrintPreview ){
			m_pPrintPreview->SetFocusToPrintPreviewBar();
		}

		return lRes;

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
					ShowCodeBox( GetHwnd(), GetDocument() );
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
	case WM_CLOSE:
		if( OnClose( nullptr, false ) ){
			::DestroyWindow( hwnd );
		}
		return 0L;
	case WM_DESTROY:
		AbortStartupDrawTransaction();
		m_dispatchReady = false;
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
		ApplyWorkbenchTheme();
		return 0L;

	case WM_SETTINGCHANGE:
		ApplyWorkbenchTheme();
		return ::DefWindowProc(hwnd, uMsg, wParam, lParam);

	case MYWM_UIPI_CHECK:
		/* エディタ－トレイ間でのUI特権分離の確認メッセージ */	// 2007.06.07 ryoji
		return LRESULT(lParam);

	case MYWM_CLOSE:
		/* エディタへの終了要求 */
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
			if (m_customFrame) {
				m_customFrame->SetThemeMode(
					m_pShareData->m_Common.m_sWindow.m_bDarkMode
						? theme::ThemeMode::Dark
						: theme::ThemeMode::Light);
			}
			/* ダークモード設定を反映する */
			{
				if( (m_pShareData->m_Common.m_sWindow.m_bDarkMode != FALSE) != IsDarkModeActive() ){
					ApplyDarkModeSetting(m_pShareData->m_Common.m_sWindow.m_bDarkMode);
					// タイトルバーとメニューバーを更新する
					DarkMode::setDarkTitleBarEx(GetHwnd(), true);
					DarkMode::setWindowMenuBarSubclass(GetHwnd());

					// 子コントロールのテーマを更新する
					DarkMode::setChildCtrlsTheme(GetHwnd());

					// タブバーのテーマを更新する（ツールチップ等）
					if( m_cTabWnd.GetHwnd() ){
						m_cTabWnd.UpdateTheme();
					}

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
				bSelect = GetActiveView().GetSelectionInfo().m_bSelectingLock;
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
		m_cTabWnd.TabWindowNotify( wParam, lParam );
		{
			RECT		rc;
			::GetClientRect( GetHwnd(), &rc );
			OnSize2( m_nWinSizeType, MAKELONG( rc.right - rc.left, rc.bottom - rc.top ), false );
			GetActiveView().SetIMECompFormPos();
		}
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

/*! 終了時の処理

	@param hWndFrom [in] 終了要求の Wimdow Handle	//2013/4/9 Uchi

	@retval TRUE: 終了して良い / FALSE: 終了しない
*/
int	CEditWnd::OnClose(HWND hWndActive, bool bGrepNoConfirm )
{
	/* ファイルを閉じるときのMRU登録 & 保存確認 & 保存実行 */
	int nRet = TRUE;
	if (HasActiveEditorInput()) {
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
	case F_FILE_USED_RECENTLY:		// 最近使ったファイル
		/* MRUリストのファイルのリストをメニューにする */
		{
			//@@@ 2001.12.26 YAZAKI MRUリストは、CMRUに依頼する
			const CMRUFile cMRU;
			cMRU.CreateMenu( hMenu, &m_cMenuDrawer );	//	ファイルメニュー
			bInList = (cMRU.MenuLength() > 0);
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
		if (m_extensionService) m_extensionService->Start();
		CAppNodeGroupHandle(0).PostMessageToAllEditors( MYWM_FIRST_IDLE, ::GetCurrentProcessId(), 0, nullptr );	// プロセスの初回アイドリング通知	// 2008.04.19 ryoji
		::PostMessage( m_pShareData->m_sHandles.m_hwndTray, MYWM_FIRST_IDLE, (WPARAM)::GetCurrentProcessId(), (LPARAM)0 );
		::KillTimer( m_hWnd, wParam );
		PostDeferredStartupWorkbenchIfReady();
		break;
	case IDT_EXTENSION_DOCUMENT_SYNC:
		::KillTimer(GetHwnd(), IDT_EXTENSION_DOCUMENT_SYNC);
		m_extensionDocumentSyncTimerPending = false;
		PublishExtensionDocumentChange();
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
		::InvalidateRect( GetHwnd(), nullptr, TRUE );
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
		::InvalidateRect( GetHwnd(), nullptr, TRUE );
		::UpdateWindow( GetHwnd() /* m_pPrintPreview->GetPrintPreviewBarHANDLE() */);
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
	partEdges[partCount - 1] = client.right - client.left;
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
		const int width = workbench::icons::StatusItemPartWidthPixels(extent.cx, dpi);
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
		::MoveWindow(hwndToolBar, 0, nCustomTitleHeight, cx, nToolBarHeight, TRUE);
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

		if( m_startupDrawState != StartupDrawState::Committing ){
			::UpdateWindow( m_cStatusBar.GetStatusHwnd() );	// 2006.06.17 ryoji 即時描画でちらつきを減らす
		}
		::GetWindowRect( m_cStatusBar.GetStatusHwnd(), &rc );
		nStatusBarHeight = rc.bottom - rc.top;
		bMiniMapSizeBox = false;
	}
	::GetClientRect( GetHwnd(), &rcClient );

	//@@@ From 2003.05.31 MIK
	//タブウインドウ追加に伴い，ファンクションキー表示位置も調整

	//タブウインドウ
	int nTabHeightBottom = 0;
	nTabWndHeight = 0;
	const bool showDocumentTabs = HasActiveEditorInput()
		|| m_pShareData->m_sNodes.m_nEditArrNum > 1;
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
			if( m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ){
				::MoveWindow( m_cTabWnd.GetHwnd(), 0, nCustomTitleHeight + nToolBarHeight + nFuncKeyWndHeight, cx, nTabWndHeight, TRUE );
			}else{
				::MoveWindow( m_cTabWnd.GetHwnd(), 0, nCustomTitleHeight + nToolBarHeight, cx, nTabWndHeight, TRUE );
			}
			m_cTabWnd.OnSize();
			::GetWindowRect( m_cTabWnd.GetHwnd(), &rc );
			if( nTabWndHeight != rc.bottom - rc.top ){
				nTabWndHeight = rc.bottom - rc.top;
				if( m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ){
					::MoveWindow( m_cTabWnd.GetHwnd(), 0, nCustomTitleHeight + nToolBarHeight + nFuncKeyWndHeight, cx, nTabWndHeight, TRUE );
				}else{
					::MoveWindow( m_cTabWnd.GetHwnd(), 0, nCustomTitleHeight + nToolBarHeight, cx, nTabWndHeight, TRUE );
				}
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
			::MoveWindow( m_cTabWnd.GetHwnd(), 0,
				cy - nFuncKeyWndHeight - nStatusBarHeight - nTabWndHeight, cx, nTabWndHeight, TRUE );
			m_cTabWnd.OnSize();
			::GetWindowRect( m_cTabWnd.GetHwnd(), &rc );
			if( nTabWndHeight != rc.bottom - rc.top ){
				nTabWndHeight = rc.bottom - rc.top;
				::MoveWindow( m_cTabWnd.GetHwnd(), 0,
					cy - nFuncKeyWndHeight - nStatusBarHeight - nTabWndHeight, cx, nTabWndHeight, TRUE );
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
			::MoveWindow(
				m_cFuncKeyWnd.GetHwnd(),
				0,
				nCustomTitleHeight + nToolBarHeight,
				cx,
				nFuncKeyWndHeight, TRUE );
		}
		else if( m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 1 )
		{	/* ファンクションキー表示位置／0:上 1:下 */
			::MoveWindow(
				m_cFuncKeyWnd.GetHwnd(),
				0,
				cy - nFuncKeyWndHeight - nStatusBarHeight,
				cx,
				nFuncKeyWndHeight, TRUE
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
		if( m_startupDrawState != StartupDrawState::Committing ){
			::UpdateWindow( m_cFuncKeyWnd.GetHwnd() );	// 2006.06.17 ryoji 即時描画でちらつきを減らす
		}
	}

	workbench::WorkbenchLayoutRequest layoutRequest;
	layoutRequest.clientWidth = cx;
	layoutRequest.clientHeight = cy;
	const auto physicalDpi = GetHwnd() == nullptr ? 96 : ::GetDpiForWindow(GetHwnd());
	layoutRequest.dpi = workbench::ScaleDpi(physicalDpi, m_workbenchZoomPercent);
	layoutRequest.titleBarHeightPixels = nCustomTitleHeight;
	layoutRequest.topAccessoryHeightPixels = nToolBarHeight
		+ (m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ? nFuncKeyWndHeight : 0);
	layoutRequest.documentTabsHeightPixels = nTabWndHeight;
	layoutRequest.bottomAccessoryHeightPixels = nTabHeightBottom
		+ (m_pShareData->m_Common.m_sWindow.m_nFUNCKEYWND_Place == 1 ? nFuncKeyWndHeight : 0);
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
	layoutRequest.leftPaneWidthDip = m_leftWorkbenchPanel
		? m_leftWorkbenchPanel->GetPendingExtentDip() : m_pShareData->m_Common.m_sWorkbench.m_nLeftPanelExtent96;
	layoutRequest.rightPaneWidthDip = m_rightWorkbenchPanel
		? m_rightWorkbenchPanel->GetPendingExtentDip() : m_pShareData->m_Common.m_sWorkbench.m_nExtensionViewsExtent96;
	layoutRequest.bottomPaneHeightDip = m_bottomWorkbenchPanel
		? m_bottomWorkbenchPanel->GetPendingExtentDip() : m_pShareData->m_Common.m_sWorkbench.m_nBottomPanelExtent96;
	layoutRequest.minimapWidthDip = GetDllShareData().m_Common.m_sWindow.m_nMiniMapWidth;
	const auto layout = workbench::CalculateWorkbenchLayout(layoutRequest);
	m_leftWorkbenchSplitter = ToWinRect(layout.leftSplitter);
	m_rightWorkbenchSplitter = ToWinRect(layout.rightSplitter);
	m_bottomWorkbenchSplitter = ToWinRect(layout.bottomSplitter);

	if (m_activityBar) m_activityBar->Layout(ToWinRect(layout.activityBar), layoutRequest.dpi);
	if (m_leftWorkbenchPanel) m_leftWorkbenchPanel->Layout(ToWinRect(layout.leftPane), layoutRequest.dpi);
	if (m_rightWorkbenchPanel) m_rightWorkbenchPanel->Layout(ToWinRect(layout.rightPane), layoutRequest.dpi);
	if (m_bottomWorkbenchPanel) m_bottomWorkbenchPanel->Layout(ToWinRect(layout.bottomPane), layoutRequest.dpi);
	if (m_cTabWnd.GetHwnd() && m_cTabWnd.m_eTabPosition == TabPosition_Top) {
		::MoveWindow(m_cTabWnd.GetHwnd(), layout.documentTabs.left, layout.documentTabs.top,
			layout.documentTabs.Width(), layout.documentTabs.Height(), TRUE);
		m_cTabWnd.OnSize();
	}

	if( m_cMiniMapView.GetHwnd() ){
		::ShowWindow(m_cMiniMapView.GetHwnd(),
			layoutRequest.showMinimap && !m_pPrintPreview ? SW_SHOWNA : SW_HIDE);
		::MoveWindow(m_cMiniMapView.GetHwnd(), layout.minimap.left, layout.minimap.top,
			layout.minimap.Width(), layout.minimap.Height(), TRUE);
		if (layoutRequest.rightPane != workbench::WorkbenchPanelState::Hidden
			|| layoutRequest.bottomPane != workbench::WorkbenchPanelState::Hidden) {
			bMiniMapSizeBox = false;
		}
		m_cMiniMapView.SplitBoxOnOff(FALSE, FALSE, bMiniMapSizeBox);
	}

	auto editorBounds = layout.editor;

	LayoutMarkdownPreview(editorBounds.left, editorBounds.top, editorBounds.right, editorBounds.bottom,
		physicalDpi);
	//@@@ To 2003.05.31 MIK

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
		// The pure layout calculator may shrink an over-large requested extent to
		// preserve the editor minimum. Persist what was actually displayed, not
		// the unconstrained mouse-derived request.
		RECT actualBounds{};
		if (host->GetHwnd() != nullptr && ::GetClientRect(host->GetHwnd(), &actualBounds)) {
			const int actualPixels = host->GetEdge() == workbench::WorkbenchEdge::Bottom
				? actualBounds.bottom - actualBounds.top
				: actualBounds.right - actualBounds.left;
			host->UpdateResize(PixelsToDip(actualPixels, ::GetDpiForWindow(GetHwnd())));
		}
		const bool committed = host->CommitResize();
		if (committed && m_workbenchRuntime != nullptr
			&& !ApplyCurrentWorkbenchLayoutState(false, true)) {
			::OutputDebugStringW(L"Sakura Editor NEXT: committed resize projection failed.\n");
		}
		if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
		RECT client{};
		::GetClientRect(GetHwnd(), &client);
		(void)OnSize2(m_nWinSizeType,
			MAKELONG(client.right - client.left, client.bottom - client.top), false);
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
	::InvalidateRect( GetHwnd(), nullptr, TRUE );
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
	auto* host = m_resizingWorkbenchPanel != nullptr
		? m_resizingWorkbenchPanel : HitTestWorkbenchSplitter(point);
	if (host == nullptr) return ::DefWindowProc(GetHwnd(), WM_SETCURSOR, wParam, lParam);
	::SetCursor(::LoadCursor(nullptr,
		host->GetEdge() == workbench::WorkbenchEdge::Bottom ? IDC_SIZENS : IDC_SIZEWE));
	return TRUE;
}

LRESULT CEditWnd::OnCaptureChanged(LPARAM lParam)
{
	if (reinterpret_cast<HWND>(lParam) != GetHwnd()) CancelWorkbenchResize();
	return 0;
}

workbench::CWorkbenchPanelHost* CEditWnd::HitTestWorkbenchSplitter(POINT point) const noexcept
{
	if (m_leftWorkbenchPanel && ContainsPoint(m_leftWorkbenchSplitter, point)) return m_leftWorkbenchPanel.get();
	if (m_rightWorkbenchPanel && ContainsPoint(m_rightWorkbenchSplitter, point)) return m_rightWorkbenchPanel.get();
	if (m_bottomWorkbenchPanel && ContainsPoint(m_bottomWorkbenchSplitter, point)) return m_bottomWorkbenchPanel.get();
	return nullptr;
}

void CEditWnd::CancelWorkbenchResize()
{
	if (m_resizingWorkbenchPanel == nullptr) return;
	auto* host = m_resizingWorkbenchPanel;
	m_resizingWorkbenchPanel = nullptr;
	host->CancelResize();
	if (::GetCapture() == GetHwnd()) ::ReleaseCapture();
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
//@@@ 2002.01.14 YAZAKI 印刷プレビューをCPrintPreviewに独立させたことによる変更
	::UpdateWindow( GetHwnd() /* m_pPrintPreview->GetPrintPreviewBarHANDLE() */);
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
		if( bWrap ){
			// WrapWindowWidth() で追加した更新リージョンで画面更新する
			for( int i = 0; i < GetAllViewCount(); i++ ){
				::UpdateWindow( GetView(i).GetHwnd() );
			}
			if( m_cMiniMapView.GetHwnd() ){
				::UpdateWindow( m_cMiniMapView.GetHwnd() );
			}
		}
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
			InvalidateRect( GetView(i).GetHwnd(), nullptr, TRUE );
			GetView(i).AdjustScrollBars();	// 2008.06.18 ryoji
		}
	}
	if( m_cMiniMapView.GetHwnd() ){
		InvalidateRect( m_cMiniMapView.GetHwnd(), nullptr, TRUE );
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
		if( GetView(i).GetSelectionInfo().m_sSelectBgn.GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().m_sSelectBgn.GetFrom(),
				&pptPosArray[i * NUM_OF_POS + 1]
			);
		}
		if( GetView(i).GetSelectionInfo().m_sSelectBgn.GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().m_sSelectBgn.GetTo(),
				&pptPosArray[i * NUM_OF_POS + 2]
			);
		}
		if( GetView(i).GetSelectionInfo().m_sSelect.GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().m_sSelect.GetFrom(),
				&pptPosArray[i * NUM_OF_POS + 3]
			);
		}
		if( GetView(i).GetSelectionInfo().m_sSelect.GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LayoutToLogicEx(
				GetView(i).GetSelectionInfo().m_sSelect.GetTo(),
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

		if( GetView(i).GetSelectionInfo().m_sSelectBgn.GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 1],
				GetView(i).GetSelectionInfo().m_sSelectBgn.GetFromPointer()
			);
		}
		if( GetView(i).GetSelectionInfo().m_sSelectBgn.GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 2],
				GetView(i).GetSelectionInfo().m_sSelectBgn.GetToPointer()
			);
		}
		if( GetView(i).GetSelectionInfo().m_sSelect.GetFrom().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 3],
				GetView(i).GetSelectionInfo().m_sSelect.GetFromPointer()
			);
		}
		if( GetView(i).GetSelectionInfo().m_sSelect.GetTo().y >= 0 ){
			GetDocument()->m_cLayoutMgr.LogicToLayoutEx(
				pptPosArray[i * NUM_OF_POS + 4],
				GetView(i).GetSelectionInfo().m_sSelect.GetToPointer()
			);
		}
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
