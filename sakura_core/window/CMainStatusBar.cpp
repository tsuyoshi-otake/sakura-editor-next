/*! @file */
/*
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "CMainStatusBar.h"
#include "window/CEditWnd.h"
#include "CEditApp.h"
#include "apiwrap/CommonControl.h"
#include "apiwrap/DarkMode.h"
#include "workbench/IconMetrics.h"
#include "workbench/hover/HoverMarkdown.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CExtensionIconFont.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/ThemeIconResolver.h"

#include "charset/CCodeFactory.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <new>
#include <set>
#include <string_view>
#include <vector>

namespace {

constexpr UINT_PTR kSakuraStatusBarSubclassId = 1;

//! ホバー遅延タイマーの ID。comctl32 がステータスバー自身に張るタイマーと衝突しない値。
constexpr UINT_PTR kExtensionHoverTimerId = 0xACE1;
//! ステータスバーとポップアップの間を横切るためのポインター監視タイマー。
constexpr UINT_PTR kExtensionHoverDismissTimerId = 0xACE2;
constexpr UINT kExtensionHoverDismissPollMilliseconds = 50;
constexpr std::string_view kNotificationStatusId = "status.notifications";
constexpr std::string_view kShowNotificationsCommand = "notifications.showList";
constexpr std::string_view kHideNotificationsCommand = "notifications.hideList";
constexpr std::string_view kToggleStatusbarCommand = "workbench.action.toggleStatusbarVisibility";

constexpr std::array<std::string_view, 8> kLegacyStatusbarIds{
	"", "status.editor.selection", "status.editor.eol", "sakura.status.editor.characterCode",
	"status.editor.encoding", "sakura.status.macroRecording", "status.editor.inputMode",
	"status.editor.zoom",
};

std::string ExtensionStatusbarId(const SExtensionStatusBarItem& item)
{
	const std::string extensionId = wcstou8s(item.extensionId);
	const std::string itemId = wcstou8s(item.itemId);
	if (extensionId.empty()) return itemId;
	if (itemId.empty() || itemId == extensionId) return extensionId;
	return extensionId + "." + itemId;
}

void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) noexcept
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		::FillRect(dc, &rect, brush);
		::DeleteObject(brush);
	}
}

[[nodiscard]] bool HasCaseInsensitivePrefix(std::wstring_view value, std::wstring_view prefix) noexcept
{
	if (value.size() < prefix.size()) return false;
	for (std::size_t index = 0; index < prefix.size(); ++index) {
		if (std::towlower(static_cast<std::wint_t>(value[index])) !=
			std::towlower(static_cast<std::wint_t>(prefix[index]))) return false;
	}
	return true;
}

void DrawBranchIcon(HDC dc, const RECT& part, COLORREF color, UINT dpi) noexcept
{
	if (dc == nullptr) return;
	const auto box = workbench::icons::LeadingStatusIconBounds(
		{ part.left, part.top, part.right, part.bottom }, dpi);
	if (box.Width() <= 0 || box.Height() <= 0) return;
	workbench::icons::codicons::Draw(dc, box,
		workbench::icons::codicons::Icon::GitBranch, color);
}

//! インラインアイコン 1 個が占める正方形の一辺。矩形の高さで頭打ちにする。
[[nodiscard]] int InlineStatusIconSide(int availableHeight, UINT dpi) noexcept
{
	return std::max(0, std::min(workbench::icons::ScaleDip(workbench::icons::kStatusIconDip, dpi),
		std::max(0, availableHeight)));
}

} // namespace

/*!
 * 文字コードの16進表示
 *
 * ステータスバー表示用に文字を16進表記に変換する
 *
 * @param [in] eCodeType 文字コードセット種別
 * @param [in] wide 表示する文字
 * @param [in] sStatusBar 共通設定 ステータスバー
 */
/* static */ std::wstring CMainStatusBar::UnicodeToHex(ECodeType eCodeType, std::wstring_view wide, const CommonSetting_Statusbar& sStatusBar)
{
	// 出力先バッファを確保する
	std::wstring buffer(32, L'\0');

	// Hex変換
	if (CCodeFactory::CreateCodeBase(eCodeType)->UnicodeToHex(std::data(wide), int(std::size(wide)), std::data(buffer), &sStatusBar) != RESULT_COMPLETE) {
		// 変換に失敗したらUNICODEで変換する
		CCodeFactory::CreateCodeBase(CODE_UTF16BE)->UnicodeToHex(std::data(wide), int(std::size(wide)), std::data(buffer), &sStatusBar);
	}

	// 出力先バッファのサイズを調整する
	buffer.resize(::wcsnlen(buffer.c_str(), std::size(buffer)));

	return buffer;
}

CMainStatusBar::CMainStatusBar(CEditWnd* pOwner)
: m_pOwner(pOwner)
{
}

//	キーワード：ステータスバー順序
/* ステータスバー作成 */
void CMainStatusBar::CreateStatusBar()
{
	if( m_hwndStatusBar )return;

	/* ステータスバー */
	m_hwndStatusBar = ::CreateWindowEx(
		WS_EX_RIGHT | WS_EX_COMPOSITED,
		STATUSCLASSNAME,
		nullptr,
		// VS Code's status bar has no legacy resize grip.  Keeping SBARS_SIZEGRIP
		// reserves an extra system-scrollbar width after the rightmost item and
		// leaves the notifications bell visibly detached from the window edge.
		WS_CHILD/* | WS_VISIBLE*/,	// 2007.03.08 ryoji WS_VISIBLE 除去
		0, 0, 0, 0, // X, Y, nWidth, nHeight
		m_pOwner->GetHwnd(),
		(HMENU)IDW_STATUSBAR,
		CEditApp::getInstance()->GetAppInstance(),
		nullptr
	);

	InstallPaletteSubclass();

	// 拡張機能ステータスバー項目のホバー。VS Code の HoverWidget と同じく、
	// StatusBarItem.tooltip の MarkdownString を書式付きのまま描く独立したポップアップ。
	// TOOLTIPS_CLASSW は平文 1 本しか描けないため使わない。
	if (m_extensionHover.Create(m_hwndStatusBar)) {
		m_extensionHover.SetPalette(m_palette);
		m_extensionHover.SetIconRegistry(m_extensionIconFonts);
		m_extensionHover.SetPointerCallback([this](bool inside) {
			OnExtensionHoverPointer(inside);
		});
		m_extensionHover.SetLinkCallback([this](std::wstring_view target) {
			constexpr wchar_t commandScheme[] = {
				L'c', L'o', L'm', L'm', L'a', L'n', L'd', L':', L'\0'
			};
			if (!HasCaseInsensitivePrefix(target, commandScheme)) return false;
			const std::wstring_view command = target.substr(std::size(commandScheme) - 1);
			if (command.empty() || !m_extensionCommandCallback) return false;
			m_extensionCommandCallback(command);
			return true;
		});
	}

	/* プログレスバー */
	m_hwndProgressBar = ::CreateWindowEx(
		WS_EX_TOOLWINDOW,
		PROGRESS_CLASS,
		nullptr,
		WS_CHILD /*|  WS_VISIBLE*/,
		3,
		5,
		150,
		13,
		m_hwndStatusBar,
		nullptr,
		CEditApp::getInstance()->GetAppInstance(),
		nullptr
	);

	DarkMode::setProgressBarCtrlSubclass(m_hwndProgressBar);

	if( nullptr != m_pOwner->m_cFuncKeyWnd.GetHwnd() ){
		m_pOwner->m_cFuncKeyWnd.SizeBox_ONOFF( FALSE );
	}

	//スプリッターの、サイズボックスの位置を変更
	m_pOwner->m_cSplitterWnd.DoSplit( -1, -1);
}

/* ステータスバー破棄 */
void CMainStatusBar::DestroyStatusBar()
{
	if( nullptr != m_hwndProgressBar ){
		::DestroyWindow( m_hwndProgressBar );
		m_hwndProgressBar = nullptr;
	}
	HideExtensionHover();
	m_extensionHover.SetPointerCallback({});
	m_extensionHover.Destroy();
	// フォントは DC へ選択したままにしていない（PaintStatusBar が毎回元へ戻す）ので、
	// ここで破棄してよい。
	ReleaseIconFonts();
	if (m_hwndStatusBar != nullptr) {
		::RemoveWindowSubclass(m_hwndStatusBar, StatusBarSubclassProc, kSakuraStatusBarSubclassId);
		::DestroyWindow(m_hwndStatusBar);
	}
	m_hwndStatusBar = nullptr;

	if( nullptr != m_pOwner->m_cFuncKeyWnd.GetHwnd() ){
		bool bSizeBox;
		if( GetDllShareData().m_Common.m_sWindow.m_nFUNCKEYWND_Place == 0 ){	/* ファンクションキー表示位置／0:上 1:下 */
			/* サイズボックスの表示／非表示切り替え */
			bSizeBox = false;
		}
		else{
			bSizeBox = true;
			/* ステータスパーを表示している場合はサイズボックスを表示しない */
			if( nullptr != m_hwndStatusBar ){
				bSizeBox = false;
			}
		}
		m_pOwner->m_cFuncKeyWnd.SizeBox_ONOFF( bSizeBox );
	}
	//スプリッターの、サイズボックスの位置を変更
	m_pOwner->m_cSplitterWnd.DoSplit( -1, -1 );
}

void CMainStatusBar::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	m_extensionHover.SetPalette(palette);
	if (m_hwndStatusBar != nullptr) {
		::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
	}
}

void CMainStatusBar::SetScmText(std::wstring text)
{
	if (m_scmText == text) return;
	m_scmText = std::move(text);
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

void CMainStatusBar::SetExtensionItems(std::vector<SExtensionStatusBarItem> items)
{
	std::erase_if(items, [](const auto& item) { return !item.visible || item.text.empty(); });
	const bool hoverActive = m_extensionHover.IsVisible() || m_hoverPending;
	const std::wstring hoverHandle = m_hoverHandle;
	m_extensionItems = std::move(items);
	// 拡張機能の progress/status 更新では全スナップショットが再送される。表示中の
	// 項目が同じ handle なら、VS Code と同じくそのホバーを更新のたびに閉じない。
	// 本当に項目が消えたときだけ古い内容を取り下げる。
	if (hoverActive) {
		const auto stillExists = std::find_if(m_extensionItems.begin(), m_extensionItems.end(),
			[&hoverHandle](const auto& item) { return !hoverHandle.empty() && item.handle == hoverHandle; });
		if (stillExists == m_extensionItems.end()) HideExtensionHover();
	}
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

void CMainStatusBar::SetExtensionCommandCallback(std::function<void(std::wstring_view)> callback)
{
	m_extensionCommandCallback = std::move(callback);
}

void CMainStatusBar::SetWorkbenchCommandCallback(std::function<void(std::string_view)> callback)
{
	m_workbenchCommandCallback = std::move(callback);
}

void CMainStatusBar::SetStatusbarVisibilityCallback(
	std::function<void(std::string_view, bool)> callback)
{
	m_statusbarVisibilityCallback = std::move(callback);
}

void CMainStatusBar::SetStatusbarViewSnapshot(workbench::statusbar::StatusbarViewSnapshot snapshot)
{
	m_statusbarViewSnapshot = std::move(snapshot);
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

void CMainStatusBar::SetNotificationState(
	std::size_t pendingCount, std::size_t unreadCount, bool centerVisible)
{
	if (m_notificationPendingCount == pendingCount && m_notificationUnreadCount == unreadCount
		&& m_notificationCenterVisible == centerVisible) return;
	m_notificationPendingCount = pendingCount;
	m_notificationUnreadCount = unreadCount;
	m_notificationCenterVisible = centerVisible;
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

bool CMainStatusBar::IsStatusbarEntryVisible(std::string_view id, bool providerVisible) const noexcept
{
	if (!providerVisible || !workbench::statusbar::StatusbarViewModel::IsValidId(id)) return false;
	return std::find(m_statusbarViewSnapshot.hiddenIds.begin(), m_statusbarViewSnapshot.hiddenIds.end(), id)
		== m_statusbarViewSnapshot.hiddenIds.end();
}

int CMainStatusBar::ReservedRightWidth() const noexcept
{
	if (!IsStatusbarEntryVisible(kNotificationStatusId)) return 0;
	const UINT dpi = m_hwndStatusBar != nullptr ? (std::max)(96u, ::GetDpiForWindow(m_hwndStatusBar)) : 96u;
	return workbench::icons::ScaleDip(28, dpi);
}

std::string_view CMainStatusBar::LegacyEntryIdForPart(int part) noexcept
{
	if (part < 0 || static_cast<std::size_t>(part) >= kLegacyStatusbarIds.size()) return {};
	return kLegacyStatusbarIds[static_cast<std::size_t>(part)];
}

void CMainStatusBar::SetExtensionIconFonts(
	const workbench::icons::CExtensionIconFontRegistry* registry) noexcept
{
	if (m_extensionIconFonts == registry) return;
	m_extensionIconFonts = registry;
	// ホバーも `$(name)` を同じレジストリで解決する。ステータスバー本文とホバー本文で
	// 解決結果が食い違ってはならない。
	m_extensionHover.SetIconRegistry(registry);
	// 借りていた書体は今のレジストリのものなので、差し替え時は必ず捨てる。
	// 登録解除済みの書体名で CreateFontIndirectW を続けると、代替書体で描いた
	// 無関係なグリフが出てしまう。
	ReleaseIconFonts();
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

HFONT CMainStatusBar::AcquireIconFont(const std::wstring& faceName, int height) const noexcept
{
	if (faceName.empty() || height <= 0) return nullptr;
	// LOGFONTW::lfFaceName は LF_FACESIZE 文字（終端含む）まで。溢れる書体名は
	// 黙って切り詰めず、描かないほうを選ぶ（別書体で代替されるより誤解が少ない）。
	if (faceName.size() >= LF_FACESIZE) return nullptr;

	for (const auto& cached : m_iconFontCache) {
		if (cached.height == height && cached.faceName == faceName) return cached.font;
	}

	LOGFONTW logFont{};
	logFont.lfHeight = -height;	// 負値は em 高（文字高）指定
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	::wcsncpy_s(logFont.lfFaceName, faceName.c_str(), _TRUNCATE);

	const HFONT font = ::CreateFontIndirectW(&logFont);
	if (font == nullptr) return nullptr;
	try {
		m_iconFontCache.push_back({ faceName, height, font });
	}
	catch (const std::bad_alloc&) {
		::DeleteObject(font);
		return nullptr;
	}
	return font;
}

void CMainStatusBar::ReleaseIconFonts() const noexcept
{
	for (const auto& cached : m_iconFontCache) {
		if (cached.font != nullptr) ::DeleteObject(cached.font);
	}
	m_iconFontCache.clear();
}

void CMainStatusBar::InstallPaletteSubclass() noexcept
{
	if (m_hwndStatusBar == nullptr) return;
	// The recursive darkmodelib pass runs after child creation. Remove only its
	// status-bar painter, then put Sakura's palette-aware painter last in the chain.
	DarkMode::removeStatusBarCtrlSubclass(m_hwndStatusBar);
	::RemoveWindowSubclass(m_hwndStatusBar, StatusBarSubclassProc, kSakuraStatusBarSubclassId);
	::SetWindowSubclass(m_hwndStatusBar, StatusBarSubclassProc, kSakuraStatusBarSubclassId,
		reinterpret_cast<DWORD_PTR>(this));
	::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
}

LRESULT CALLBACK CMainStatusBar::StatusBarSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR referenceData)
{
	auto* self = reinterpret_cast<CMainStatusBar*>(referenceData);
	switch (message) {
	case WM_ERASEBKGND:
		return TRUE;
	case WM_PRINTCLIENT:
		if (self != nullptr && reinterpret_cast<HDC>(wParam) != nullptr) {
			self->PaintStatusBar(reinterpret_cast<HDC>(wParam));
			return 0;
		}
		break;
	case WM_PAINT:
		if (self != nullptr) {
			PAINTSTRUCT paint{};
			const HDC dc = ::BeginPaint(window, &paint);
			self->PaintStatusBar(dc);
			::EndPaint(window, &paint);
			return 0;
		}
		break;
	case WM_THEMECHANGED:
		::InvalidateRect(window, nullptr, FALSE);
		break;
	case WM_LBUTTONUP:
		// VS Code もホバー中にクリックするとホバーは閉じる。コマンドが走って項目の
		// 中身が入れ替わったあとも古い内容が残らないよう、実行の前に取り下げる。
		if (self != nullptr) self->HideExtensionHover();
		if (self != nullptr && (self->InvokeBuiltinItemAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })
			|| self->InvokeExtensionItemAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }))) {
			return 0;
		}
		break;
	case WM_CONTEXTMENU:
		if (self != nullptr) {
			self->HideExtensionHover();
			self->ShowContextMenu({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
			return 0;
		}
		break;
	case WM_MOUSEMOVE:
		if (self != nullptr) {
			self->OnExtensionHoverMouseMove({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		}
		break;
	case WM_MOUSELEAVE:
		if (self != nullptr) {
			self->m_hoverTracking = false;
			if (self->m_extensionHover.IsVisible()) {
				self->ScheduleExtensionHoverDismiss();
			} else {
				self->HideExtensionHover();
			}
		}
		break;
	case WM_TIMER:
		if (self != nullptr && wParam == kExtensionHoverTimerId) {
			self->ShowExtensionHoverNow();
			return 0;
		}
		if (self != nullptr && wParam == kExtensionHoverDismissTimerId) {
			// Keep polling while the cursor is crossing the anchor-to-hover path. This
			// avoids relying on the order of WM_MOUSELEAVE/WM_MOUSEMOVE between two
			// separate top-level windows.
			if (self->IsCursorOnExtensionHoverPath()) return 0;
			::KillTimer(window, kExtensionHoverDismissTimerId);
			self->m_hoverDismissPending = false;
			self->HideExtensionHover();
			return 0;
		}
		break;
	case WM_SETCURSOR:
		if (self != nullptr) {
			POINT point{};
			if (::GetCursorPos(&point) && ::ScreenToClient(window, &point)) {
				for (const auto& target : self->m_statusbarHitTargets) {
					if (target.id == kNotificationStatusId && ::PtInRect(&target.bounds, point)) {
						::SetCursor(::LoadCursor(nullptr, IDC_HAND));
						return TRUE;
					}
				}
				for (const auto& target : self->m_extensionHitTargets) {
					// tooltip-only 項目（command が空）は WM_LBUTTONUP でも無視される
					// ので、ここでも手のひらカーソルを出さない。押しても何も起きない
					// 項目にクリック可能な見た目を与えるのは既存の挙動からの逸脱になる。
					if (!target.command.empty() && ::PtInRect(&target.bounds, point)) {
						::SetCursor(::LoadCursor(nullptr, IDC_HAND));
						return TRUE;
					}
				}
			}
		}
		break;
	case WM_NCDESTROY:
		::RemoveWindowSubclass(window, StatusBarSubclassProc, subclassId);
		if (self != nullptr && self->m_hwndStatusBar == window) {
			// ホバーはこのウィンドウの所有ポップアップなので、親が消える前に必ず
			// 落とす。タイマーも m_hwndStatusBar に張っているため、null 化より先に
			// 取り下げないと KillTimer の宛先が無くなる。
			self->HideExtensionHover();
			self->m_extensionHover.SetPointerCallback({});
			self->m_extensionHover.Destroy();
			self->m_hwndStatusBar = nullptr;
		}
		break;
	default:
		break;
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

void CMainStatusBar::PaintStatusBar(HDC dc) const noexcept
{
	if (dc == nullptr || m_hwndStatusBar == nullptr) return;

	RECT client{};
	::GetClientRect(m_hwndStatusBar, &client);
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;
	if (width <= 0 || height <= 0) return;

	const HDC buffer = ::CreateCompatibleDC(dc);
	const HBITMAP bitmap = buffer == nullptr ? nullptr : ::CreateCompatibleBitmap(dc, width, height);
	const HGDIOBJ oldBitmap = bitmap == nullptr ? nullptr : ::SelectObject(buffer, bitmap);
	HDC target = oldBitmap == nullptr ? dc : buffer;

	// Match the VS Code workbench status treatment: the complete bar carries
	// the active accent, while every label and line icon uses its paired
	// high-contrast foreground.
	FillSolidRect(target, client, m_palette.accent.ToColorRef());
	::SetBkMode(target, TRANSPARENT);
	::SetTextColor(target, m_palette.highlightText.ToColorRef());
	const HFONT font = reinterpret_cast<HFONT>(::SendMessageW(m_hwndStatusBar, WM_GETFONT, 0, 0));
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(target, font);
	m_extensionHitTargets.clear();
	m_statusbarHitTargets.clear();
	int scmWidth = 0;
	if (!m_scmText.empty() && IsStatusbarEntryVisible("status.scm")) {
		SIZE extent{};
		if (::GetTextExtentPoint32W(target, m_scmText.c_str(), static_cast<int>(m_scmText.size()), &extent)) {
			const UINT scmDpi = static_cast<UINT>(::GetDpiForWindow(m_hwndStatusBar));
			const int textInset = workbench::icons::StatusTextInsetPixels(scmDpi);
			scmWidth = std::min(width, static_cast<int>(extent.cx) + textInset + 8);
			const RECT scmIconRect{ 0, 0, scmWidth, height };
			DrawBranchIcon(target, scmIconRect, m_palette.highlightText.ToColorRef(), scmDpi);
			RECT scmRect{ textInset, 0, scmWidth - 4, height };
			::DrawTextW(target, m_scmText.c_str(), static_cast<int>(m_scmText.size()), &scmRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			m_statusbarHitTargets.push_back({ "status.scm", scmIconRect });
		}
	}

	const int partCount = static_cast<int>(::SendMessageW(m_hwndStatusBar, SB_GETPARTS, 0, 0));
	const UINT dpi = static_cast<UINT>(::GetDpiForWindow(m_hwndStatusBar));
	const int itemInset = workbench::icons::ScaleDip(workbench::icons::kStatusItemInsetDip, dpi);
	// パート 0 はレガシーのドキュメント状態（カーソル位置・文字コード等）が占める帯で、
	// ドキュメントが 1 つも開いていなければパートは 1 つも無い。しかし拡張のステータス
	// 項目はそれとは別のレイヤーで、実 VS Code はエディターが開いていなくても
	// ステータスバーと拡張項目を常に描く。「パートが無い」を「拡張項目も描かない」と
	// 読み替えると、空のウィンドウで拡張項目が丸ごと消える（実機で確認した欠陥）。
	// パートが無いときはステータスバー自身のクライアント矩形を帯として使う。
	RECT messageRect = client;
	if (partCount > 0) {
		(void)::SendMessageW(m_hwndStatusBar, SB_GETRECT, 0, reinterpret_cast<LPARAM>(&messageRect));
	}
	messageRect.left = std::max<LONG>(messageRect.left, scmWidth);
	// The final right-aligned entry owns the full client edge.  The notification
	// item's own 28-DIP hit target supplies its padding, matching VS Code's
	// rightmost statusbar item instead of preserving a removed size-grip gap.
	LONG statusContentRight = client.right;
	statusContentRight = (std::max<LONG>)(messageRect.left, statusContentRight);
	RECT notificationRect{};
	if (IsStatusbarEntryVisible(kNotificationStatusId)) {
		const int notificationWidth = ReservedRightWidth();
		notificationRect = { (std::max<LONG>)(messageRect.left, statusContentRight - notificationWidth),
			messageRect.top, statusContentRight, messageRect.bottom };
		messageRect.right = (std::min)(messageRect.right, notificationRect.left);
	}

	const int statusHeight = std::max<int>(0, messageRect.bottom - messageRect.top);
	const int inlineIconSide = InlineStatusIconSide(statusHeight, dpi);

	struct ExtensionLayoutItem {
		const SExtensionStatusBarItem* item = nullptr;
		std::vector<workbench::icons::SLabelRun> runs;
		int width = 0;
	};
	std::vector<ExtensionLayoutItem> leftItems;
	std::vector<ExtensionLayoutItem> rightItems;
	for (const auto& item : m_extensionItems) {
		if (!IsStatusbarEntryVisible(ExtensionStatusbarId(item), item.visible)) continue;
		// 実 VS Code の renderLabelWithIcons と同じく、ラベル中のアイコンは位置そのままで
		// 何個でもインラインに並ぶ。先頭 1 個だけを特別扱いしてはならない。
		// 組み込み `$(name)` は同梱 codicon.ttf の 1 グリフとして描く。書体名が空なら
		// 登録に失敗しているので、取り込み済みベクターへ縮退する。
		auto runs = workbench::icons::ParseLabelWithIcons(
			item.text, m_extensionIconFonts, workbench::icons::CCodiconFont::Instance().FaceName());
		int contentWidth = 0;
		for (const auto& run : runs) {
			if (run.icon) {
				contentWidth += inlineIconSide;
				continue;
			}
			SIZE extent{};
			if (::GetTextExtentPoint32W(target, run.text.c_str(), static_cast<int>(run.text.size()), &extent)) {
				contentWidth += extent.cx;
			}
		}
		ExtensionLayoutItem layout{
			.item = &item,
			.runs = std::move(runs),
			.width = workbench::icons::StatusItemPartWidthPixels(contentWidth, dpi),
		};
		if (item.alignment == EExtensionStatusBarAlignment::Right) rightItems.push_back(std::move(layout));
		else leftItems.push_back(std::move(layout));
	}
	const auto drawExtensionItem = [&](const ExtensionLayoutItem& layout, int left, int right) {
		if (layout.item == nullptr || right <= left) return;
		RECT itemRect{ left, messageRect.top, right, messageRect.bottom };
		const COLORREF iconColor = m_palette.highlightText.ToColorRef();
		const LONG contentRight = std::max<LONG>(itemRect.left, itemRect.right - itemInset);
		LONG cursorX = std::min<LONG>(contentRight, itemRect.left + itemInset);
		for (const auto& run : layout.runs) {
			if (cursorX >= contentRight) break;
			if (run.icon) {
				const int side = std::min<int>(inlineIconSide, static_cast<int>(contentRight - cursorX));
				if (side <= 0) break;
				const workbench::icons::IconRect box{
					static_cast<int>(cursorX),
					itemRect.top + (statusHeight - side) / 2,
					static_cast<int>(cursorX) + side,
					itemRect.top + (statusHeight - side) / 2 + side,
				};
				if (run.resolved.font) {
					// アイコンフォント（寄与アイコンも同梱 codicon.ttf も）は em ボックス
					// いっぱいにグリフを置く前提で作られている。負の lfHeight は文字高
					// （em 高）指定なので、アイコンの正方形と同じ高さを渡してから矩形の
					// 中央へ寄せる。
					const HFONT glyphFont = AcquireIconFont(
						run.resolved.fontIcon.faceName, std::max(1, box.Height()));
					if (glyphFont != nullptr && !run.resolved.fontIcon.glyph.empty()) {
						const HFONT previousFont = static_cast<HFONT>(::SelectObject(target, glyphFont));
						RECT glyphRect{ box.left, box.top, box.right, box.bottom };
						::DrawTextW(target, run.resolved.fontIcon.glyph.c_str(),
							static_cast<int>(run.resolved.fontIcon.glyph.size()), &glyphRect,
							DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
						::SelectObject(target, previousFont);
					}
				} else {
					workbench::icons::codicons::Draw(target, box, run.resolved.builtin, iconColor);
				}
				cursorX += side;
				continue;
			}
			if (run.text.empty()) continue;
			SIZE extent{};
			(void)::GetTextExtentPoint32W(target, run.text.c_str(), static_cast<int>(run.text.size()), &extent);
			RECT textRect{ cursorX, itemRect.top, contentRight, itemRect.bottom };
			::DrawTextW(target, run.text.c_str(), static_cast<int>(run.text.size()), &textRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			cursorX = std::min<LONG>(contentRight, cursorX + extent.cx);
		}
		// ツールチップは Markdown 原文のまま持ち、実際にホバーを出す瞬間に解析する。
		// 再描画のたびに解析すると、書式付きホバーでは平文射影より明確に高くつく。
		if (!layout.item->command.empty() || !layout.item->tooltip.empty()) {
			m_extensionHitTargets.push_back({ layout.item->handle, ExtensionStatusbarId(*layout.item), itemRect,
				layout.item->command, layout.item->tooltip,
				layout.item->tooltipSupportsThemeIcons, layout.item->tooltipIsTrusted,
				layout.item->tooltipTrustedCommands });
		}
	};

	int rightGroupWidth = 0;
	for (const auto& layout : rightItems) {
		rightGroupWidth = std::min<int>(messageRect.right - messageRect.left,
			rightGroupWidth + std::max(0, layout.width));
	}
	int rightCursor = std::max<int>(messageRect.left, messageRect.right - rightGroupWidth);
	const int rightGroupLeft = rightCursor;
	for (const auto& layout : rightItems) {
		const int next = std::min<int>(messageRect.right, rightCursor + layout.width);
		drawExtensionItem(layout, rightCursor, next);
		rightCursor = next;
	}
	int leftCursor = messageRect.left;
	for (const auto& layout : leftItems) {
		if (leftCursor >= rightGroupLeft) break;
		const int next = std::min<int>(rightGroupLeft, leftCursor + layout.width);
		drawExtensionItem(layout, leftCursor, next);
		leftCursor = next;
	}
	messageRect.left = leftCursor;
	messageRect.right = rightGroupLeft;
	for (int part = 0; part < partCount; ++part) {
		RECT partRect{};
		if (::SendMessageW(m_hwndStatusBar, SB_GETRECT, part, reinterpret_cast<LPARAM>(&partRect)) == FALSE) continue;
		if (part == 0) partRect = messageRect;
		const auto entryId = LegacyEntryIdForPart(part);
		if (!entryId.empty()) {
			if (!IsStatusbarEntryVisible(entryId)) continue;
			m_statusbarHitTargets.push_back({ std::string(entryId), partRect });
		}
		const LRESULT textInfo = ::SendMessageW(m_hwndStatusBar, SB_GETTEXTLENGTHW, part, 0);
		const UINT textLength = LOWORD(textInfo);
		const UINT style = HIWORD(textInfo);
		std::vector<wchar_t> text(static_cast<size_t>(textLength) + 1, L'\0');
		const LRESULT itemData = ::SendMessageW(m_hwndStatusBar, SB_GETTEXTW, part,
			reinterpret_cast<LPARAM>(text.data()));
		if ((style & SBT_OWNERDRAW) != 0) {
			DRAWITEMSTRUCT drawItem{ ODT_STATIC, 0, static_cast<UINT>(part), ODA_DRAWENTIRE, 0,
				m_hwndStatusBar, target, partRect, static_cast<ULONG_PTR>(itemData) };
			::SendMessageW(::GetParent(m_hwndStatusBar), WM_DRAWITEM,
				static_cast<WPARAM>(::GetDlgCtrlID(m_hwndStatusBar)), reinterpret_cast<LPARAM>(&drawItem));
		} else if (textLength != 0) {
			// Each part already reserves the full adjacent gap. Keep half before the
			// label and leave the other half after it so exact GDI extents do not
			// ellipsize and neighbouring labels still remain eight DIP apart.
			partRect.left = std::min<LONG>(partRect.right, partRect.left + itemInset);
			::DrawTextW(target, text.data(), static_cast<int>(textLength), &partRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
	}

	if (notificationRect.right > notificationRect.left) {
		const int side = InlineStatusIconSide(notificationRect.bottom - notificationRect.top, dpi);
		const auto glyph = workbench::icons::FindCodiconGlyph(
			m_notificationUnreadCount == 0 ? L"bell" : L"bell-dot");
		const auto& codiconFont = workbench::icons::CCodiconFont::Instance();
		const HFONT glyphFont = codiconFont.IsAvailable()
			? AcquireIconFont(std::wstring(codiconFont.FaceName()), std::max(1, side)) : nullptr;
		if (glyph && glyphFont != nullptr) {
			const HFONT previousFont = static_cast<HFONT>(::SelectObject(target, glyphFont));
			wchar_t text[2]{ *glyph, L'\0' };
			RECT glyphRect = notificationRect;
			::DrawTextW(target, text, 1, &glyphRect,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
			::SelectObject(target, previousFont);
		}
		m_statusbarHitTargets.push_back({ std::string(kNotificationStatusId), notificationRect });
		std::wstring tooltip = m_notificationPendingCount == 0
			? L"通知はありません" : L"通知 (" + std::to_wstring(m_notificationPendingCount) + L")";
		m_extensionHitTargets.push_back({ L"status.notifications", std::string(kNotificationStatusId),
			notificationRect, L"", std::move(tooltip), false, false, {} });
	}

	if (oldFont != nullptr) ::SelectObject(target, oldFont);
	if (oldBitmap != nullptr) {
		::BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
		::SelectObject(buffer, oldBitmap);
	}
	if (bitmap != nullptr) ::DeleteObject(bitmap);
	if (buffer != nullptr) ::DeleteDC(buffer);
}

bool CMainStatusBar::InvokeExtensionItemAt(POINT point) const
{
	if (!m_extensionCommandCallback) return false;
	for (const auto& target : m_extensionHitTargets) {
		// tooltip-only 項目（command が空）はクリックしても何も起きない。既存の挙動を
		// 変えないよう、command を持つ項目だけを対象にする。
		if (!target.command.empty() && ::PtInRect(&target.bounds, point)) {
			m_extensionCommandCallback(target.command);
			return true;
		}
	}
	return false;
}

bool CMainStatusBar::InvokeBuiltinItemAt(POINT point) const
{
	if (!m_workbenchCommandCallback) return false;
	for (const auto& target : m_statusbarHitTargets) {
		if (target.id == kNotificationStatusId && ::PtInRect(&target.bounds, point)) {
			m_workbenchCommandCallback(
				m_notificationCenterVisible ? kHideNotificationsCommand : kShowNotificationsCommand);
			return true;
		}
	}
	return false;
}

std::optional<std::string> CMainStatusBar::EntryIdAt(POINT clientPoint) const
{
	for (const auto& target : m_statusbarHitTargets) {
		if (::PtInRect(&target.bounds, clientPoint)) return target.id;
	}
	for (const auto& target : m_extensionHitTargets) {
		if (!target.statusbarId.empty() && ::PtInRect(&target.bounds, clientPoint)) return target.statusbarId;
	}
	return std::nullopt;
}

void CMainStatusBar::ShowContextMenu(POINT screenPoint)
{
	if (m_hwndStatusBar == nullptr) return;
	if (screenPoint.x == -1 && screenPoint.y == -1) {
		RECT rect{};
		::GetWindowRect(m_hwndStatusBar, &rect);
		screenPoint = { rect.left + (rect.right - rect.left) / 2, rect.top + (rect.bottom - rect.top) / 2 };
	}
	POINT clientPoint = screenPoint;
	(void)::ScreenToClient(m_hwndStatusBar, &clientPoint);
	const auto clickedId = EntryIdAt(clientPoint);

	const HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr) return;
	constexpr UINT kHideStatusbarMenu = 1;
	constexpr UINT kHideClickedMenu = 2;
	constexpr UINT kFirstEntryMenu = 100;
	::AppendMenuW(menu, MF_STRING, kHideStatusbarMenu, L"ステータス バーを非表示");
	::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

	std::vector<const workbench::statusbar::StatusbarEntry*> menuEntries;
	std::set<std::string, std::less<>> seen;
	for (const auto& entry : m_statusbarViewSnapshot.entries) {
		if (!entry.providerVisible || entry.name.empty() || !seen.insert(entry.id).second) continue;
		const UINT command = kFirstEntryMenu + static_cast<UINT>(menuEntries.size());
		const UINT flags = MF_STRING | (IsStatusbarEntryVisible(entry.id) ? MF_CHECKED : MF_UNCHECKED);
		::AppendMenuW(menu, flags, command, entry.name.c_str());
		menuEntries.push_back(&entry);
	}

	const workbench::statusbar::StatusbarEntry* clickedEntry = nullptr;
	if (clickedId) {
		const auto found = std::find_if(m_statusbarViewSnapshot.entries.begin(), m_statusbarViewSnapshot.entries.end(),
			[&clickedId](const auto& entry) { return entry.id == *clickedId; });
		if (found != m_statusbarViewSnapshot.entries.end() && IsStatusbarEntryVisible(found->id)) clickedEntry = &*found;
	}
	if (clickedEntry != nullptr) {
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		const std::wstring label = L"'" + clickedEntry->name + L"' を非表示";
		::AppendMenuW(menu, MF_STRING, kHideClickedMenu, label.c_str());
	}

	const UINT selected = ::TrackPopupMenu(menu,
		TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenPoint.x, screenPoint.y, 0,
		m_pOwner->GetHwnd(), nullptr);
	::DestroyMenu(menu);
	if (selected == kHideStatusbarMenu) {
		if (m_workbenchCommandCallback) m_workbenchCommandCallback(kToggleStatusbarCommand);
	} else if (selected == kHideClickedMenu && clickedEntry != nullptr) {
		if (m_statusbarVisibilityCallback) m_statusbarVisibilityCallback(clickedEntry->id, true);
	} else if (selected >= kFirstEntryMenu && selected - kFirstEntryMenu < menuEntries.size()) {
		const auto& entry = *menuEntries[selected - kFirstEntryMenu];
		if (m_statusbarVisibilityCallback) {
			m_statusbarVisibilityCallback(entry.id, IsStatusbarEntryVisible(entry.id));
		}
	}
}

const CMainStatusBar::ExtensionHitTarget* CMainStatusBar::FindHoverTargetAt(POINT point) const noexcept
{
	for (const auto& target : m_extensionHitTargets) {
		// コマンドだけ持ちツールチップの無い項目には、出すものが無い。
		if (target.tooltipMarkdown.empty()) continue;
		if (::PtInRect(&target.bounds, point)) return &target;
	}
	return nullptr;
}

void CMainStatusBar::OnExtensionHoverMouseMove(POINT point) noexcept
{
	if (m_hwndStatusBar == nullptr) return;
	if (m_hoverDismissPending) {
		::KillTimer(m_hwndStatusBar, kExtensionHoverDismissTimerId);
		m_hoverDismissPending = false;
	}
	// ステータスバーの外へ出たことは WM_MOUSEMOVE では分からないので、TME_LEAVE を
	// 張っておく。ステータスバーからホバーへ移動すると WM_MOUSELEAVE が先に届くため、
	// ホスト側でホバーウィンドウへの移動猶予を管理する。
	if (!m_hoverTracking) {
		TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, m_hwndStatusBar, 0 };
		m_hoverTracking = ::TrackMouseEvent(&track) != FALSE;
	}

	const auto* target = FindHoverTargetAt(point);
	if (target == nullptr) {
		if (m_extensionHover.IsVisible() && IsCursorOnExtensionHoverPath()) {
			ScheduleExtensionHoverDismiss();
		} else {
			HideExtensionHover();
		}
		return;
	}
	// 同じ項目の中で動いただけなら、待機も表示もやり直さない。矩形で同一性を見るのは、
	// m_extensionHitTargets が描画のたびに作り直されて添字が当てにならないため。
	if ((m_hoverPending || m_extensionHover.IsVisible()) && ::EqualRect(&m_hoverAnchor, &target->bounds)) return;

	HideExtensionHover();
	m_hoverAnchor = target->bounds;
	m_hoverHandle = target->handle;
	m_hoverPending = ::SetTimer(m_hwndStatusBar, kExtensionHoverTimerId,
		workbench::hover::kHoverDelayMilliseconds, nullptr) != 0;
}

void CMainStatusBar::ShowExtensionHoverNow()
{
	if (m_hwndStatusBar == nullptr) return;
	::KillTimer(m_hwndStatusBar, kExtensionHoverTimerId);
	m_hoverPending = false;

	// タイマー満了時点のカーソル位置を改めて読む。待機中に項目が入れ替わっていれば、
	// 張った時とは別の（あるいは何も無い）項目の上にいる。
	POINT cursor{};
	if (::GetCursorPos(&cursor) == FALSE || ::ScreenToClient(m_hwndStatusBar, &cursor) == FALSE) {
		HideExtensionHover();
		return;
	}
	const auto* target = FindHoverTargetAt(cursor);
	if (target == nullptr) {
		HideExtensionHover();
		return;
	}

	const auto document = workbench::hover::Parse(target->tooltipMarkdown,
		{ .supportThemeIcons = target->tooltipSupportsThemeIcons });
	if (document.empty()) {
		HideExtensionHover();
		return;
	}

	RECT anchor = target->bounds;
	m_hoverAnchor = anchor;
	m_hoverHandle = target->handle;
	::MapWindowPoints(m_hwndStatusBar, HWND_DESKTOP, reinterpret_cast<POINT*>(&anchor), 2);
	m_extensionHover.Show(document, anchor, target->tooltipIsTrusted, target->tooltipTrustedCommands);
}

void CMainStatusBar::OnExtensionHoverPointer(bool inside) noexcept
{
	if (inside) {
		// The dismiss timer is intentionally a short polling timer. Keeping it
		// alive while inside the popup also covers platforms where the popup does
		// not receive a matching WM_MOUSELEAVE after a cross-window transition.
		return;
	}
	if (m_extensionHover.IsVisible()) ScheduleExtensionHoverDismiss();
}

void CMainStatusBar::ScheduleExtensionHoverDismiss() noexcept
{
	if (m_hwndStatusBar == nullptr || !m_extensionHover.IsVisible() || m_hoverDismissPending) return;
	if (::SetTimer(
			m_hwndStatusBar,
			kExtensionHoverDismissTimerId,
			kExtensionHoverDismissPollMilliseconds,
			nullptr) != 0) {
		m_hoverDismissPending = true;
	} else {
		HideExtensionHover();
	}
}

bool CMainStatusBar::IsCursorOnExtensionHoverPath() const noexcept
{
	if (!m_extensionHover.IsVisible() || m_hwndStatusBar == nullptr) return false;
	if (m_extensionHover.IsPointerInside()) return true;

	POINT cursor{};
	if (::GetCursorPos(&cursor) == FALSE) return false;
	RECT anchor = m_hoverAnchor;
	RECT popup{};
	::MapWindowPoints(m_hwndStatusBar, HWND_DESKTOP, reinterpret_cast<POINT*>(&anchor), 2);
	if (::GetWindowRect(m_extensionHover.GetHwnd(), &popup) == FALSE) return false;

	if (::PtInRect(&anchor, cursor) != FALSE) return true;
	const RECT bridge{
		(std::min)(anchor.left, popup.left),
		(std::min)(anchor.bottom, popup.bottom),
		(std::max)(anchor.right, popup.right),
		(std::max)(anchor.top, popup.top),
	};
	return ::PtInRect(&bridge, cursor) != FALSE;
}

void CMainStatusBar::HideExtensionHover() noexcept
{
	if (m_hoverPending && m_hwndStatusBar != nullptr) {
		::KillTimer(m_hwndStatusBar, kExtensionHoverTimerId);
	}
	if (m_hoverDismissPending && m_hwndStatusBar != nullptr) {
		::KillTimer(m_hwndStatusBar, kExtensionHoverDismissTimerId);
	}
	m_hoverPending = false;
	m_hoverDismissPending = false;
	m_hoverAnchor = RECT{};
	m_hoverHandle.clear();
	m_extensionHover.Hide();
}

/*!
	@brief メッセージの表示
	
	指定されたメッセージをステータスバーに表示する．
	メニューバー右端に入らないものや，桁位置表示を隠したくないものに使う
	
	呼び出し前にSendStatusMessage2IsEffective()で処理の有無を
	確認することで無駄な処理を省くことが出来る．

	@param msg [in] 表示するメッセージ
	@date 2005.07.09 genta 新規作成
	
	@sa SendStatusMessage2IsEffective
*/
void CMainStatusBar::SendStatusMessage2( const WCHAR* msg )
{
	if( nullptr != m_hwndStatusBar ){
		SetStatusText(0, SBT_NOBORDERS, msg);
	}
}

/*!
	@brief 文字列をステータスバーの指定されたパートに表示する
	
	@param nIndex [in] パートのインデクス
	@param nOption [in] 描画オペレーションタイプ
	@param pszText [in] 表示テキスト
	@param textLen [in] 表示テキストの文字数
*/
bool CMainStatusBar::SetStatusText(int nIndex, int nOption, const WCHAR* pszText, size_t textLen /* = SIZE_MAX */)
{
	if( !m_hwndStatusBar ){
		assert(m_hwndStatusBar != nullptr);
		return false;
	}
	// StatusBar_SetText 関数を呼びだすかどうかを判定する
	// （StatusBar_SetText は SB_SETTEXT メッセージを SendMessage で送信する）
	bool bDraw = true;
	do {
		// オーナードローの場合は SB_SETTEXT メッセージを無条件に発行するように判定
		// 本来表示に変化が無い場合には呼び出さない方が表示のちらつきが減るので好ましいが
		// 判定が難しいので諦める
		if( nOption == SBT_OWNERDRAW ){
			break;
		}
		// オーナードローではない場合で NULLの場合は空文字に置き換える
		// NULL を渡しても問題が無いのかどうか公式ドキュメントに記載されていない
		// NULL のままでも問題は発生しないようだが念の為に対策を追加
		if( pszText == nullptr ){
			static const wchar_t emptyStr[] = L"";
			pszText = emptyStr;
			textLen = 0;
		}
		LRESULT res = ::ApiWrap::StatusBar_GetTextLength( m_hwndStatusBar, nIndex );
		// 表示オペレーション値が変化する場合は SB_SETTEXT メッセージを発行
		if( HIWORD(res) != nOption ){
			break;
		}
		size_t prevTextLen = LOWORD(res);
		WCHAR prev[1024];
		// 設定済みの文字列長が長過ぎて取得できない場合は、SB_SETTEXT メッセージを発行
		if( prevTextLen >= int(std::size(prev)) ){
			break;
		}
		// 設定する文字列長パラメータが SIZE_MAX（引数のデフォルト値）な場合は文字列長を取得
		if( textLen == SIZE_MAX ){
			textLen = wcslen(pszText);
		}
		// 設定済みの文字列長と設定する文字列長が異なる場合は、SB_SETTEXT メッセージを発行
		if( prevTextLen != textLen ){
			break;
		}
		if( prevTextLen > 0 ){
			ApiWrap::StatusBar_GetText( m_hwndStatusBar, nIndex, prev );
			// 設定済みの文字列と設定する文字列を比較して異なる場合は、SB_SETTEXT メッセージを発行
			bDraw = wcscmp(prev, pszText) != 0;
		}
		else {
			// 設定する文字列長が0の場合は設定する文字列長が0より大きい場合のみ設定する（既に空文字なら空文字を設定する必要は無い為）
			bDraw = textLen > 0;
		}
	}while (false);
	if (bDraw) {
		ApiWrap::StatusBar_SetText(m_hwndStatusBar, nIndex | nOption, pszText);
	}
	return bDraw;
}

//! プログレスバーの表示/非表示を切り替える
void CMainStatusBar::ShowProgressBar(bool bShow) const {
	if (m_hwndStatusBar && m_hwndProgressBar) {
		// プログレスバーを表示するステータスバー上の領域を取得
		RECT rcProgressArea = {};
		ApiWrap::StatusBar_GetRect(m_hwndStatusBar, 0, &rcProgressArea);
		if (bShow) {
			::ShowWindow(m_hwndProgressBar, SW_SHOW);
		} else {
			::ShowWindow(m_hwndProgressBar, SW_HIDE);
		}
		// プログレスバー表示領域を再描画
		::InvalidateRect(m_hwndStatusBar, &rcProgressArea, TRUE);
		::UpdateWindow(m_hwndStatusBar);
	}
}
