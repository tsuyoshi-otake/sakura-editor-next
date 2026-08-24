/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEDITVIEW_PAINT_0202B897_3D47_48DD_9279_45594D80F726_H_
#define SAKURA_CEDITVIEW_PAINT_0202B897_3D47_48DD_9279_45594D80F726_H_
#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

class CEditView;

namespace view::indent_decoration {

struct LayoutRange final {
	int start = 0;
	int end = 0;
	[[nodiscard]] bool operator==(const LayoutRange&) const = default;
};

//! Projects SENP visual columns into the editor's pixel-based layout units.
[[nodiscard]] constexpr std::optional<LayoutRange> ProjectVisualColumns(
	std::uint32_t visualStart, std::uint32_t visualLength, int layoutUnitsPerColumn) noexcept
{
	if( visualLength == 0 || layoutUnitsPerColumn <= 0 ) return std::nullopt;
	const std::uint64_t visualEnd = static_cast<std::uint64_t>(visualStart)
		+ static_cast<std::uint64_t>(visualLength);
	const auto maximumColumn = static_cast<std::uint64_t>(std::numeric_limits<int>::max())
		/ static_cast<std::uint64_t>(layoutUnitsPerColumn);
	if( visualEnd > maximumColumn ) return std::nullopt;
	return LayoutRange{
		static_cast<int>(visualStart) * layoutUnitsPerColumn,
		static_cast<int>(visualEnd) * layoutUnitsPerColumn,
	};
}

} // namespace view::indent_decoration

namespace view::indent_guide {

//! Measures the leading indentation in visual columns. Tabs advance to the
//! next tab stop, matching the editor layout and VS Code indentation guides.
[[nodiscard]] constexpr std::uint32_t LeadingVisualColumns(
	std::wstring_view line, std::uint32_t tabSize) noexcept
{
	if( tabSize == 0 ) return 0;
	std::uint32_t columns = 0;
	for( const wchar_t ch : line ){
		if( ch == L' ' ){
			if( columns == std::numeric_limits<std::uint32_t>::max() ) return columns;
			++columns;
		}else if( ch == L'\t' ){
			const std::uint32_t advance = tabSize - columns % tabSize;
			if( columns > std::numeric_limits<std::uint32_t>::max() - advance ){
				return std::numeric_limits<std::uint32_t>::max();
			}
			columns += advance;
		}else{
			break;
		}
	}
	return columns;
}

} // namespace view::indent_guide

//! クリッピング領域を計算する際のフラグ
enum EPaintArea{
	PAINT_LINENUMBER = (1<<0), //!< 行番号
	PAINT_RULER      = (1<<1), //!< ルーラー
	PAINT_BODY       = (1<<2), //!< 本文

	//特殊
	PAINT_ALL        = PAINT_LINENUMBER | PAINT_RULER | PAINT_BODY, //!< ぜんぶ
};

class CEditView_Paint{
public:
	virtual CEditView* GetEditView()=0;

public:
	virtual ~CEditView_Paint(){}
	void Call_OnPaint(
		int nPaintFlag,   //!< 描画する領域を選択する
		bool bUseMemoryDC //!< メモリDCを使用する
	);
};
#endif /* SAKURA_CEDITVIEW_PAINT_0202B897_3D47_48DD_9279_45594D80F726_H_ */
