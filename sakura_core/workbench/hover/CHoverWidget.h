/*! @file
	@brief 書式付きホバー（VS Code の HoverWidget 相当）

	VS Code のホバーは `vs/base/browser/ui/hover/hoverWidget.ts` の `HoverWidget` が
	持つ独立したフローティング要素で、中身は `renderMarkdown()` が組み立てた本物の
	書式付き内容である。Win32 の `TOOLTIPS_CLASSW` は平文 1 本しか描けないため、
	見出し・強調・インラインコード・テーマアイコン・表を保った表示は原理的に作れない。
	そこで `WS_POPUP` を自前で描き、HoverMarkdown が返すブロックモデルを GDI で
	レイアウトする。

	色は theme::ThemePalette から採り、VS Code の `editorHoverWidget.*` に対応させる。
	表は罫線を引かない（VS Code の Markdown ホバーも引かない）。拡張が列として渡した
	区切り文字がそのまま区切りに見えるのは、上流と同じ結果である。
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include "theme/CThemeService.h"
#include "workbench/hover/HoverMarkdown.h"

#include <string>
#include <vector>

namespace workbench::icons {
class CExtensionIconFontRegistry;
}

namespace workbench::hover {

//! VS Code の `workbench.hover.delay` 既定値。
inline constexpr UINT kHoverDelayMilliseconds = 500;

/*!
	@brief 1 個の書式付きホバーウィンドウ

	所有者ウィンドウ 1 つにつき 1 個持ち、表示のたびに内容を差し替える。マウス入力を
	受け取らない（`WS_EX_TRANSPARENT`）ので、VS Code のようにホバー内をポイントし続けて
	リンクを押すことはできない。この差異は sakura_core/workbench/hover/CLAUDE.md に記録。
*/
class CHoverWidget final {
public:
	CHoverWidget() noexcept = default;
	~CHoverWidget();
	CHoverWidget(const CHoverWidget&) = delete;
	CHoverWidget& operator=(const CHoverWidget&) = delete;
	CHoverWidget(CHoverWidget&&) = delete;
	CHoverWidget& operator=(CHoverWidget&&) = delete;

	//! 所有者ウィンドウに紐づくポップアップを作る。二重呼び出しは何もしない。
	bool Create(HWND owner) noexcept;
	void Destroy() noexcept;

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_hwnd; }
	[[nodiscard]] bool IsVisible() const noexcept;

	void SetPalette(const theme::ThemePalette& palette) noexcept;
	//! 寄与アイコンの解決に使う。所有はしない（レジストリはウィンドウより長生き）。
	void SetIconRegistry(const workbench::icons::CExtensionIconFontRegistry* registry) noexcept;

	/*!
		@brief 内容を差し替えて表示する

		@param [in] document 表示するブロックモデル。空なら Hide() と同じ。
		@param [in] anchorScreen アンカー矩形（スクリーン座標）。原則その上辺に接して出る。
	*/
	void Show(const SDocument& document, const RECT& anchorScreen);
	void Hide() noexcept;

private:
	//! 1 つの描画実行単位。レイアウト時に位置とフォントが確定する。
	struct SPositionedRun {
		RECT bounds{};
		std::wstring text;
		std::wstring iconId;
		int fontIndex = 0;
		bool link = false;
	};
	//! 背景・罫線などの塗り。描画順は runs より先。
	struct SFilledRect {
		RECT bounds{};
		COLORREF color = 0;
	};
	struct SLayout {
		std::vector<SFilledRect> fills;
		std::vector<SPositionedRun> runs;
		int width = 0;
		int height = 0;
	};
	//! 寄与アイコンフォント (faceName, height) のキャッシュ。
	struct SIconFont {
		std::wstring faceName;
		int height = 0;
		HFONT font = nullptr;
	};

	static LRESULT CALLBACK HoverWndProc(HWND, UINT, WPARAM, LPARAM) noexcept;
	static bool EnsureWindowClass() noexcept;

	[[nodiscard]] SLayout BuildLayout(HDC dc, const SDocument& document, UINT dpi) const;
	void OnPaint(HDC dc) const noexcept;
	void ReleaseFonts() noexcept;
	bool EnsureFonts(UINT dpi) noexcept;
	[[nodiscard]] HFONT AcquireIconFont(const std::wstring& faceName, int height) const noexcept;
	void PositionWindow(const RECT& anchorScreen) noexcept;

	HWND m_hwnd = nullptr;
	HWND m_owner = nullptr;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	const workbench::icons::CExtensionIconFontRegistry* m_iconRegistry = nullptr;
	SLayout m_layout;
	UINT m_fontDpi = 0;
	std::vector<HFONT> m_fonts;
	//! 描画中（const な OnPaint）に必要になった寄与アイコンフォントを取り込むためのキャッシュ。
	mutable std::vector<SIconFont> m_iconFonts;
};

} // namespace workbench::hover
