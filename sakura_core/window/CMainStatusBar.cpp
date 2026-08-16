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
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/LabelRunPainter.h"
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

constexpr std::string_view kNotificationStatusId = "status.notifications";
constexpr std::string_view kRemoteHostStatusId = "status.host";
constexpr std::string_view kRemoteShowMenuCommand = "workbench.action.remote.showMenu";
constexpr std::string_view kShowNotificationsCommand = "notifications.showList";
constexpr std::string_view kHideNotificationsCommand = "notifications.hideList";
constexpr std::string_view kToggleStatusbarCommand = "workbench.action.toggleStatusbarVisibility";
constexpr wchar_t kRemoteHostLabel[] = L"$(remote)";

constexpr std::array<std::string_view, 8> kLegacyStatusbarIds{
	"", "status.editor.selection", "status.editor.eol", "sakura.status.editor.characterCode",
	"status.editor.encoding", "sakura.status.macroRecording", "status.editor.inputMode",
	"status.editor.zoom",
};

void FillSolidRect(HDC dc, const RECT& rect, COLORREF color) noexcept
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		::FillRect(dc, &rect, brush);
		::DeleteObject(brush);
	}
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
	if (m_hwndStatusBar != nullptr) {
		::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
	}
}

void CMainStatusBar::SetScmStatusCommands(std::vector<workbench::scm::ScmCommand> commands)
{
	if (m_scmCommands == commands) return;
	m_scmCommands = std::move(commands);
	if (m_hwndStatusBar != nullptr) ::InvalidateRect(m_hwndStatusBar, nullptr, FALSE);
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

HFONT CMainStatusBar::AcquireIconFont(std::wstring_view faceName, int height) const noexcept
{
	for (const auto& cached : m_iconFontCache) {
		if (cached.height == height && cached.faceName == faceName) return cached.font;
	}

	// `workbench/icons/LabelRunPainter.h` が唯一の LOGFONTW 組み立て規則。ここで
	// 別の複製を持つと、ホバーと違う書体でグリフが描かれかねない。空の書体名・
	// 高さ 0 以下・LF_FACESIZE を超える書体名はそちら側で弾かれる（fail-closed）。
	const HFONT font = workbench::icons::CreateLabelRunGlyphFont(faceName, height);
	if (font == nullptr) return nullptr;
	try {
		m_iconFontCache.push_back({ std::wstring(faceName), height, font });
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
		if (self != nullptr && self->InvokeBuiltinItemAt(
			{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })) {
			return 0;
		}
		break;
	case WM_CONTEXTMENU:
		if (self != nullptr) {
			self->ShowContextMenu({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
			return 0;
		}
		break;
	case WM_SETCURSOR:
		if (self != nullptr) {
			POINT point{};
			if (::GetCursorPos(&point) && ::ScreenToClient(window, &point)) {
				for (const auto& target : self->m_statusbarHitTargets) {
					// 通知は状態で show/hide が入れ替わるため command を持たないが、
					// 押せば必ず何かが起きる。それ以外は command を持つ項目だけを
					// クリック可能に見せる。
					const bool clickable = target.id == kNotificationStatusId || !target.command.empty();
					if (clickable && ::PtInRect(&target.bounds, point)) {
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
	m_statusbarHitTargets.clear();

	const UINT dpi = static_cast<UINT>(::GetDpiForWindow(m_hwndStatusBar));
	const int itemInset = workbench::icons::ScaleDip(workbench::icons::kStatusItemInsetDip, dpi);
	const COLORREF iconColor = m_palette.highlightText.ToColorRef();

	// 実 VS Code は StatusBarItem.text も SourceControl.statusBarCommands の
	// Command.title も同じ renderLabelWithIcons で描く。
	// 測る側も描く側も `workbench/icons/LabelRunPainter.h` が唯一の規則。ここに 3 つ目
	// の写しを置くと、同じ `$(name)` がステータスバーと SCM ビューと他のワークベンチ面で別の絵・
	// 別の幅になり得る。`itemInset` の扱いだけはこの呼び出し側の都合なので、内側余白を
	// 差し引いた矩形を渡す（矩形の解釈を 2 通りにしない）。
	const auto measureLabelRuns = [&](const std::vector<workbench::icons::SLabelRun>& runs, int iconSide) {
		return workbench::icons::MeasureLabelRuns(target, runs, iconSide);
	};
	// `release` は空。ここで返る `HFONT` は `m_iconFontCache` の所有物で、書体レジストリ
	// 差し替え時にキャッシュ側がまとめて解放する（window/CLAUDE.md の解放契約）。
	// 描画のたびに破棄すると、次の再描画がぶら下がったハンドルを選択することになる。
	const workbench::icons::SLabelRunFontProvider glyphFonts{
		.acquire = [this](std::wstring_view faceName, int height) { return AcquireIconFont(faceName, height); },
		.release = {},
	};
	const auto drawLabelRuns = [&](const std::vector<workbench::icons::SLabelRun>& runs,
		const RECT& itemRect, int iconSide) {
		const RECT content{
			std::min<LONG>(std::max<LONG>(itemRect.left, itemRect.right - itemInset), itemRect.left + itemInset),
			itemRect.top,
			std::max<LONG>(itemRect.left, itemRect.right - itemInset),
			itemRect.bottom,
		};
		workbench::icons::DrawLabelRuns(target, runs, content, iconSide, iconColor, glyphFonts);
	};

	// VS Code's leftmost status entry is the remote host indicator (`status.host`),
	// then the SCM provider's published `statusBarCommands`. Do not fold the remote
	// glyph into the branch item: they are separate hit targets and commands.
	int leftCursor = 0;
	if (IsStatusbarEntryVisible(kRemoteHostStatusId)) {
		const int remoteIconSide = InlineStatusIconSide(height, dpi);
		const auto runs = workbench::icons::ParseLabelWithIcons(
			kRemoteHostLabel, workbench::icons::CCodiconFont::Instance().FaceName());
		const int itemWidth = workbench::icons::StatusItemPartWidthPixels(
			measureLabelRuns(runs, remoteIconSide), dpi);
		const int right = std::min(width, leftCursor + itemWidth);
		if (right > leftCursor) {
			const RECT itemRect{ leftCursor, 0, right, height };
			drawLabelRuns(runs, itemRect, remoteIconSide);
			m_statusbarHitTargets.push_back({
				std::string(kRemoteHostStatusId), itemRect, std::string(kRemoteShowMenuCommand) });
			leftCursor = right;
		}
	}

	int scmWidth = leftCursor;
	if (IsStatusbarEntryVisible("status.scm")) {
		const int scmIconSide = InlineStatusIconSide(height, dpi);
		for (const auto& command : m_scmCommands) {
			if (command.title.empty()) break;
			const std::wstring title = u8stowcs(command.title);
			const auto runs = workbench::icons::ParseLabelWithIcons(
				title, workbench::icons::CCodiconFont::Instance().FaceName());
			const int itemWidth = workbench::icons::StatusItemPartWidthPixels(
				measureLabelRuns(runs, scmIconSide), dpi);
			const int right = std::min(width, scmWidth + itemWidth);
			if (right <= scmWidth) break;
			const RECT itemRect{ scmWidth, 0, right, height };
			drawLabelRuns(runs, itemRect, scmIconSide);
			m_statusbarHitTargets.push_back({ "status.scm", itemRect, command.command });
			scmWidth = right;
		}
	}

	const int partCount = static_cast<int>(::SendMessageW(m_hwndStatusBar, SB_GETPARTS, 0, 0));
	// パート 0 はレガシーのドキュメント状態が占める帯。ドキュメントが無い場合は
	// ステータスバー自身のクライアント矩形を使う。
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
			? AcquireIconFont(codiconFont.FaceName(), std::max(1, side)) : nullptr;
		if (glyph && glyphFont != nullptr) {
			const HFONT previousFont = static_cast<HFONT>(::SelectObject(target, glyphFont));
			wchar_t text[2]{ *glyph, L'\0' };
			RECT glyphRect = notificationRect;
			::DrawTextW(target, text, 1, &glyphRect,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
			::SelectObject(target, previousFont);
		}
		m_statusbarHitTargets.push_back({ std::string(kNotificationStatusId), notificationRect });
	}

	if (oldFont != nullptr) ::SelectObject(target, oldFont);
	if (oldBitmap != nullptr) {
		::BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
		::SelectObject(buffer, oldBitmap);
	}
	if (bitmap != nullptr) ::DeleteObject(bitmap);
	if (buffer != nullptr) ::DeleteDC(buffer);
}

bool CMainStatusBar::InvokeBuiltinItemAt(POINT point) const
{
	if (!m_workbenchCommandCallback) return false;
	for (const auto& target : m_statusbarHitTargets) {
		if (!::PtInRect(&target.bounds, point)) continue;
		if (target.id == kNotificationStatusId) {
			// 通知だけは表示状態で show/hide が入れ替わるので、公開された 1 本の
			// コマンドではなく現在の状態から決める。
			m_workbenchCommandCallback(
				m_notificationCenterVisible ? kHideNotificationsCommand : kShowNotificationsCommand);
			return true;
		}
		if (!target.command.empty()) {
			m_workbenchCommandCallback(target.command);
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
