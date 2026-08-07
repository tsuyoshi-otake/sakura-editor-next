/*! @file
	@brief `renderLabelWithIcons` の断片列を 1 つの規則で測って描く

	実 VS Code は `StatusBarItem.text` も `Command.title` も banner のメッセージも、
	同じ `renderLabelWithIcons` の出力を同じ規則で描く。ここが 3 つ目の写しになると、
	同じ `$(name)` が場所によって別の絵・別の幅になり得る。`ThemeIconResolver.h` が
	「どう解決するか」を 1 か所に置いているのと同じ理由で、「どう描くか」もここに置く。

	フォント取得だけは呼び出し側で違う。ステータスバーは (書体名, 高さ) ごとに `HFONT`
	をキャッシュして再描画のたびに `CreateFontIndirectW` を呼ばないようにしており、
	その他の呼び出し側は 1 回きりの生成/破棄で足りる。よって取得と解放を差し替え可能な
	1 組として受け取り、既定の実装（都度生成・都度破棄）を用意する。
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/icons/ThemeIconResolver.h"

#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

namespace workbench::icons {

/*!
	@brief グリフ 1 走査ぶんのアイコンフォントをどう用意し、どう手放すか

	`release` は空でもよい。空なら `acquire` が返した `HFONT` は呼び出し側の所有物
	（キャッシュ）であって、描画側は破棄しない。
*/
struct SLabelRunFontProvider {
	std::function<HFONT(std::wstring_view faceName, int height)> acquire;
	std::function<void(HFONT)> release;
};

//! アイコンフォントを 1 つ作る。返した `HFONT` は呼び出し側の所有物。
[[nodiscard]] inline HFONT CreateLabelRunGlyphFont(std::wstring_view faceName, int height)
{
	if (faceName.empty() || faceName.size() >= LF_FACESIZE || height <= 0) return nullptr;
	LOGFONTW logFont{};
	// アイコンフォントは em ボックスいっぱいにグリフを置く前提で作られている。負の
	// lfHeight は文字高（em 高）指定なので、アイコンの正方形と同じ高さを渡す。
	logFont.lfHeight = -height;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), logFont.lfFaceName);
	logFont.lfFaceName[faceName.size()] = L'\0';
	return ::CreateFontIndirectW(&logFont);
}

//! 都度生成・都度破棄。キャッシュを持たない呼び出し側の既定。
[[nodiscard]] inline SLabelRunFontProvider OwnedGlyphFontProvider()
{
	return SLabelRunFontProvider{
		.acquire = [](std::wstring_view faceName, int height) { return CreateLabelRunGlyphFont(faceName, height); },
		.release = [](HFONT font) { if (font != nullptr) ::DeleteObject(font); },
	};
}

//! アイコン断片は 1 辺 `iconSide` の正方形、テキスト断片はその実測幅。
[[nodiscard]] inline int MeasureLabelRuns(HDC dc, const std::vector<SLabelRun>& runs, int iconSide)
{
	int width = 0;
	for (const auto& run : runs) {
		if (run.icon) {
			width += iconSide;
			continue;
		}
		SIZE extent{};
		if (::GetTextExtentPoint32W(dc, run.text.c_str(), static_cast<int>(run.text.size()), &extent)) {
			width += extent.cx;
		}
	}
	return width;
}

/*!
	@brief 断片列を `bounds` の左端から順に描く

	テキストの色は `color` を設定してから描く。取り込み済みベクター codicon も同じ
	`color` で描くので、1 つのラベルの中でアイコンと文字の色がずれることはない。
	`bounds` を横にはみ出す断片は切り詰められ、最後のテキスト断片は
	`DT_END_ELLIPSIS` で省略される。呼び出し側が内側の余白を持つ場合は、その余白を
	差し引いた矩形をここへ渡す。矩形の解釈を 2 通りにしない。
*/
inline void DrawLabelRuns(
	HDC dc,
	const std::vector<SLabelRun>& runs,
	const RECT& bounds,
	int iconSide,
	COLORREF color,
	const SLabelRunFontProvider& fonts)
{
	const LONG right = bounds.right;
	const int height = std::max<int>(0, bounds.bottom - bounds.top);
	LONG cursor = bounds.left;
	::SetTextColor(dc, color);
	for (const auto& run : runs) {
		if (cursor >= right) break;
		if (run.icon) {
			const int side = std::min<int>(iconSide, static_cast<int>(right - cursor));
			if (side <= 0) break;
			const IconRect box{
				static_cast<int>(cursor),
				bounds.top + (height - side) / 2,
				static_cast<int>(cursor) + side,
				bounds.top + (height - side) / 2 + side,
			};
			if (run.resolved.font) {
				const HFONT glyphFont = fonts.acquire
					? fonts.acquire(run.resolved.fontIcon.faceName, std::max(1, box.Height()))
					: nullptr;
				if (glyphFont != nullptr && !run.resolved.fontIcon.glyph.empty()) {
					const HGDIOBJ previous = ::SelectObject(dc, glyphFont);
					RECT glyph{ box.left, box.top, box.right, box.bottom };
					::DrawTextW(dc, run.resolved.fontIcon.glyph.c_str(),
						static_cast<int>(run.resolved.fontIcon.glyph.size()), &glyph,
						DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
					::SelectObject(dc, previous);
				}
				if (glyphFont != nullptr && fonts.release) fonts.release(glyphFont);
			} else {
				codicons::Draw(dc, box, run.resolved.builtin, color);
			}
			cursor += side;
			continue;
		}
		if (run.text.empty()) continue;
		SIZE extent{};
		(void)::GetTextExtentPoint32W(dc, run.text.c_str(), static_cast<int>(run.text.size()), &extent);
		RECT textRect{ cursor, bounds.top, right, bounds.bottom };
		::DrawTextW(dc, run.text.c_str(), static_cast<int>(run.text.size()), &textRect,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		cursor = std::min<LONG>(right, cursor + extent.cx);
	}
}

} // namespace workbench::icons
