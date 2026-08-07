/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CMAINSTATUSBAR_E2FC11D7_4513_4F96_BDCC_E9B278ED0718_H_
#define SAKURA_CMAINSTATUSBAR_E2FC11D7_4513_4F96_BDCC_E9B278ED0718_H_
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "doc/CDocListener.h"
#include "extension/CExtensionStatusBar.h"
#include "theme/CThemeService.h"
#include "workbench/hover/CHoverWidget.h"
#include "workbench/scm/SourceControlService.h"
#include "workbench/statusbar/StatusbarViewModel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class CEditWnd;

namespace workbench::icons {
class CExtensionIconFontRegistry;
}

class CMainStatusBar : public CDocListenerEx{
public:
	static std::wstring UnicodeToHex(ECodeType eCodeType, std::wstring_view wide, const CommonSetting_Statusbar& sStatusbar);

	//作成・破棄
	CMainStatusBar(CEditWnd* pOwner);
	void CreateStatusBar();		// ステータスバー作成
	void DestroyStatusBar();		/* ステータスバー破棄 */
	void SendStatusMessage2( const WCHAR* msg );	//	Jul. 9, 2005 genta メニューバー右端には出したくない長めのメッセージを出す
	/*!	SendStatusMessage2()が効き目があるかを予めチェック
		@date 2005.07.09 genta
		@note もしSendStatusMessage2()でステータスバー表示以外の処理を追加
		する場合にはここを変更しないと新しい場所への出力が行われない．
		
		@sa SendStatusMessage2
	*/
	bool SendStatusMessage2IsEffective() const
	{
		return nullptr != m_hwndStatusBar;
	}

	//取得
	HWND GetStatusHwnd() const{ return m_hwndStatusBar; }
	HWND GetProgressHwnd() const{ return m_hwndProgressBar; }

	//設定
	bool SetStatusText(int nIndex, int nOption, const WCHAR* pszText, size_t textLen = SIZE_MAX);
	void ShowProgressBar(bool bShow) const;
	void SetPalette(const theme::ThemePalette& palette) noexcept;
	/*!
		@brief SCM プロバイダーが公開した statusBarCommands をそのまま描く

		実 VS Code のステータスバー左端は `SourceControl.statusBarCommands` の
		射影であって、独自に組み立てた 1 本のテキストではない。ラベルは
		`$(git-branch) main` のように `Command.title` そのもので、アイコンも
		クリック先コマンドもそこに含まれる。
	*/
	void SetScmStatusCommands(std::vector<workbench::scm::ScmCommand> commands);
	//! Applies the visible StatusBarItem snapshot on the UI thread.
	void SetExtensionItems(std::vector<SExtensionStatusBarItem> items);
	//! Invoked when a clickable extension item is activated.
	void SetExtensionCommandCallback(std::function<void(std::wstring_view)> callback);
	//! Stable workbench commands used by built-in status entries and the context menu.
	void SetWorkbenchCommandCallback(std::function<void(std::string_view)> callback);
	void SetStatusbarVisibilityCallback(std::function<void(std::string_view, bool)> callback);
	void SetStatusbarViewSnapshot(workbench::statusbar::StatusbarViewSnapshot snapshot);
	void SetNotificationState(std::size_t pendingCount, std::size_t unreadCount, bool centerVisible);
	/*!
		@brief 現在のワークスペース信頼状態を投影する

		VS Code の `status.workspaceTrust`（far-left の `$(shield) Restricted
		Mode`）を塗るための純粋な射影。`Unknown` と `Untrusted` はどちらも
		「制限モード」を意味するので、bool へ早期に潰さず三値のまま保持する。
		`SetScmStatusCommands` / `SetNotificationState` と同じくコールバックを
		一切呼ばない ── コマンド実行や所有者への通知はここではしない。
	*/
	void SetWorkspaceTrustState(config::EWorkspaceTrustState state) noexcept;
	[[nodiscard]] bool IsStatusbarEntryVisible(std::string_view id, bool providerVisible = true) const noexcept;
	[[nodiscard]] int ReservedRightWidth() const noexcept;
	[[nodiscard]] static std::string_view LegacyEntryIdForPart(int part) noexcept;
	/*!
		@brief contributes.icons のレジストリを借りる（所有しない）

		`$(icon-id)` は実 VS Code のグローバルな IconRegistry と同じ解決順で、
		まず拡張が寄与したアイコンを引き、無ければ組み込み codicon へ落とす。
		レジストリの生存はコンポジションルート（CEditWnd）が持つ。nullptr なら
		寄与アイコンは一切解決せず、従来どおり組み込み codicon だけを使う。
	*/
	void SetExtensionIconFonts(const workbench::icons::CExtensionIconFontRegistry* registry) noexcept;
	void InstallPaletteSubclass() noexcept;
	[[nodiscard]] COLORREF GetTextColor() const noexcept { return m_palette.primaryText.ToColorRef(); }
private:
	//! 書体名と字高の組に対して 1 個だけ作る、寄与アイコン描画用フォント
	struct IconFont {
		std::wstring faceName;
		int height = 0;
		HFONT font = nullptr;
	};

	struct ExtensionHitTarget {
		std::wstring handle;
		std::string statusbarId;
		RECT bounds{};
		std::wstring command;
		//! 拡張機能が渡した Markdown 原文。空ならホバーを出さない。解析はホバーを実際に
		//! 出す瞬間まで遅らせる（再描画のたびに解析し直さないため）。
		std::wstring tooltipMarkdown;
		//! vscode.MarkdownString.supportThemeIcons。真のときだけ `$(name)` をアイコンに解釈する。
	bool tooltipSupportsThemeIcons = false;
	bool tooltipIsTrusted = false;
	std::vector<std::wstring> tooltipTrustedCommands;
	};
	struct StatusbarHitTarget {
		std::string id;
		RECT bounds{};
		//! クリック時に実行する安定コマンド ID。空なら押しても何も起きない項目。
		//! 1 つの表示 ID（`status.scm`）に複数の項目がぶら下がるため、実行先は
		//! 表示 ID ではなくヒット領域ごとに持つ。
		std::string command;
	};

	static LRESULT CALLBACK StatusBarSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR subclassId, DWORD_PTR referenceData);
	void PaintStatusBar(HDC dc) const noexcept;
	[[nodiscard]] bool InvokeExtensionItemAt(POINT point) const;
	[[nodiscard]] bool InvokeBuiltinItemAt(POINT point) const;
	void ShowContextMenu(POINT screenPoint);
	[[nodiscard]] std::optional<std::string> EntryIdAt(POINT clientPoint) const;
	//! ツールチップ本文を持つ項目のうち、指定クライアント座標を含む最初のものを返す。
	[[nodiscard]] const ExtensionHitTarget* FindHoverTargetAt(POINT point) const noexcept;
	//! WM_MOUSEMOVE。VS Code の workbench.hover.delay と同じ遅延タイマーを張り直す。
	void OnExtensionHoverMouseMove(POINT point) noexcept;
	//! ホバーウィンドウへのポインター移動を受け、ステータスバーの外でも表示を維持する。
	void OnExtensionHoverPointer(bool inside) noexcept;
	//! ステータスバーとホバーの間を通過するための短い取り下げ猶予を張る。
	void ScheduleExtensionHoverDismiss() noexcept;
	[[nodiscard]] bool IsCursorOnExtensionHoverPath() const noexcept;
	//! 遅延タイマー満了。カーソル直下の項目のツールチップを解析して表示する。
	void ShowExtensionHoverNow();
	//! 表示中/待機中のホバーを取り下げる。タイマーも必ず落とす。
	void HideExtensionHover() noexcept;
	//! 寄与アイコン用の HFONT を書体名と字高の組で貸し出す。同じ組は 1 個だけ作り、
	//! 再描画のたびに CreateFontIndirectW を呼ばない。失敗したら nullptr を返す。
	[[nodiscard]] HFONT AcquireIconFont(std::wstring_view faceName, int height) const noexcept;
	//! 貸し出し済みの HFONT をすべて破棄する。DC に選択されたままにしてはならない。
	void ReleaseIconFonts() const noexcept;

	CEditWnd*	m_pOwner;
	HWND		m_hwndStatusBar = nullptr;
	HWND		m_hwndProgressBar = nullptr;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	std::vector<workbench::scm::ScmCommand> m_scmCommands;
	std::vector<SExtensionStatusBarItem> m_extensionItems;
	mutable std::vector<ExtensionHitTarget> m_extensionHitTargets;
	mutable std::vector<StatusbarHitTarget> m_statusbarHitTargets;
	workbench::statusbar::StatusbarViewSnapshot m_statusbarViewSnapshot;
	std::size_t m_notificationPendingCount = 0;
	std::size_t m_notificationUnreadCount = 0;
	bool m_notificationCenterVisible = false;
	//! Unknown until CEditWnd's first workbench-context refresh pushes the real
	//! value; Unknown paints as restricted, so an un-refreshed window never
	//! shows a false "trusted" state.
	config::EWorkspaceTrustState m_workspaceTrustState = config::EWorkspaceTrustState::Unknown;
	//! VS Code の HoverWidget 相当。TOOLTIPS_CLASSW では描けない書式付き本文を自前で描く。
	workbench::hover::CHoverWidget m_extensionHover;
	//! ホバー待機中/表示中の項目矩形（ステータスバーのクライアント座標）。
	RECT m_hoverAnchor{};
	//! ホバー中の StatusBarItem を更新後も追跡するための安定したハンドル。
	std::wstring m_hoverHandle;
	//! 遅延タイマーが張られている。
	bool m_hoverPending = false;
	//! ステータスバー外へ出た後の取り下げ猶予タイマーが張られている。
	bool m_hoverDismissPending = false;
	//! TrackMouseEvent(TME_LEAVE) 済み。WM_MOUSELEAVE で false に戻る。
	bool m_hoverTracking = false;
	std::function<void(std::wstring_view)> m_extensionCommandCallback;
	std::function<void(std::string_view)> m_workbenchCommandCallback;
	std::function<void(std::string_view, bool)> m_statusbarVisibilityCallback;
	//! 借り物。所有者は CEditWnd。null なら寄与アイコンを解決しない
	const workbench::icons::CExtensionIconFontRegistry* m_extensionIconFonts = nullptr;
	mutable std::vector<IconFont> m_iconFontCache;
};
#endif /* SAKURA_CMAINSTATUSBAR_E2FC11D7_4513_4F96_BDCC_E9B278ED0718_H_ */
