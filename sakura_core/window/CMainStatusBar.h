/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CMAINSTATUSBAR_E2FC11D7_4513_4F96_BDCC_E9B278ED0718_H_
#define SAKURA_CMAINSTATUSBAR_E2FC11D7_4513_4F96_BDCC_E9B278ED0718_H_
#pragma once

#include "doc/CDocListener.h"
#include "theme/CThemeService.h"
#include "workbench/scm/SourceControlService.h"
#include "workbench/statusbar/StatusbarViewModel.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class CEditWnd;

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
	//! Stable workbench commands used by built-in status entries and the context menu.
	void SetWorkbenchCommandCallback(std::function<void(std::string_view)> callback);
	void SetStatusbarVisibilityCallback(std::function<void(std::string_view, bool)> callback);
	void SetStatusbarViewSnapshot(workbench::statusbar::StatusbarViewSnapshot snapshot);
	void SetNotificationState(std::size_t pendingCount, std::size_t unreadCount, bool centerVisible);
	[[nodiscard]] bool IsStatusbarEntryVisible(std::string_view id, bool providerVisible = true) const noexcept;
	[[nodiscard]] int ReservedRightWidth() const noexcept;
	[[nodiscard]] static std::string_view LegacyEntryIdForPart(int part) noexcept;
	void InstallPaletteSubclass() noexcept;
	[[nodiscard]] COLORREF GetTextColor() const noexcept { return m_palette.primaryText.ToColorRef(); }
private:
	//! 書体名と字高の組に対して 1 個だけ作るアイコン描画用フォント
	struct IconFont {
		std::wstring faceName;
		int height = 0;
		HFONT font = nullptr;
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
	[[nodiscard]] bool InvokeBuiltinItemAt(POINT point) const;
	void ShowContextMenu(POINT screenPoint);
	[[nodiscard]] std::optional<std::string> EntryIdAt(POINT clientPoint) const;
	//! アイコン用の HFONT を書体名と字高の組で貸し出す。同じ組は 1 個だけ作り、
	//! 再描画のたびに CreateFontIndirectW を呼ばない。失敗したら nullptr を返す。
	[[nodiscard]] HFONT AcquireIconFont(std::wstring_view faceName, int height) const noexcept;
	//! 貸し出し済みの HFONT をすべて破棄する。DC に選択されたままにしてはならない。
	void ReleaseIconFonts() const noexcept;

	CEditWnd*	m_pOwner;
	HWND		m_hwndStatusBar = nullptr;
	HWND		m_hwndProgressBar = nullptr;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	std::vector<workbench::scm::ScmCommand> m_scmCommands;
	mutable std::vector<StatusbarHitTarget> m_statusbarHitTargets;
	workbench::statusbar::StatusbarViewSnapshot m_statusbarViewSnapshot;
	std::size_t m_notificationPendingCount = 0;
	std::size_t m_notificationUnreadCount = 0;
	bool m_notificationCenterVisible = false;
	std::function<void(std::string_view)> m_workbenchCommandCallback;
	std::function<void(std::string_view, bool)> m_statusbarVisibilityCallback;
	mutable std::vector<IconFont> m_iconFontCache;
};
#endif /* SAKURA_CMAINSTATUSBAR_E2FC11D7_4513_4F96_BDCC_E9B278ED0718_H_ */
