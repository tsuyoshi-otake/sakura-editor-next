/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

// 2000.10.08 JEPRO  背景色を真っ白RGB(255,255,255)→(255,251,240)に変更(眩しさを押さえた)
// 2000.12.09 Jepro  note: color setting (詳細は CshareData.h を参照のこと)
// 2000.09.04 JEPRO  シングルクォーテーション文字列に色を割り当てるが色分け表示はしない
// 2000.10.17 JEPRO  色分け表示するように変更(最初のFALSE→TRUE)
// 2008.03.27 kobake 大整理

#include "StdAfx.h"
#include "doc/CDocTypeSetting.h"

#include "view/colors/EColorIndexType.h"

#include "CSelectLang.h"

//! 色設定(保存用)
struct ColorInfoIni {
	int				m_nNameId;			//!< 項目名
	ColorInfoBase	m_sColorInfo;		//!< 色設定
};

/*!
 * 色設定のデフォルト値
 */
const std::array ColorInfo_DEFAULT = {
//				項目名,									表示,		太字,		下線,		文字色,					背景色,
	ColorInfoIni{ STR_COLOR_TEXT,						{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_RULER,						{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 239, 239, 239 ) } } },
	ColorInfoIni{ STR_COLOR_CURSOR,						{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_CURSOR_IMEON,				{ true,		{ false,	false },	{ RGB( 255,   0,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_CURSOR_LINE_BG,				{ false,	{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 255, 255, 128 ) } } },
	ColorInfoIni{ STR_COLOR_CURSOR_LINE,				{ true,		{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_CURSOR_COLUMN,				{ false,	{ false,	false },	{ RGB( 128, 128, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_NOTE_LINE,					{ false,	{ false,	false },	{ RGB( 192, 192, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_LINE_NO,					{ true,		{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 239, 239, 239 ) } } },
	ColorInfoIni{ STR_COLOR_LINE_NO_CHANGE,				{ true,		{ true,		false },	{ RGB(   0,   0, 255 ),	RGB( 239, 239, 239 ) } } },
	ColorInfoIni{ STR_COLOR_EVEN_LINE_BG,				{ false,	{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 243, 243, 243 ) } } },
	ColorInfoIni{ STR_COLOR_TAB,						{ true,		{ false,	false },	{ RGB( 128, 128, 128 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_HALF_SPACE,					{ false,	{ false,	false },	{ RGB( 192, 192, 192 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_FULL_SPACE,					{ true,		{ false,	false },	{ RGB( 192, 192, 192 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_CTRL_CODE,					{ true,		{ false,	false },	{ RGB( 255, 255,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_CR,							{ true,		{ false,	false },	{ RGB(   0, 128, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_WRAP_MARK,					{ true,		{ false,	false },	{ RGB( 255,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_VERT_LINE,					{ false,	{ false,	false },	{ RGB( 192, 192, 192 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_EOF,						{ true,		{ false,	false },	{ RGB(   0, 255, 255 ),	RGB(   0,   0,   0 ) } } },
	ColorInfoIni{ STR_COLOR_NUMBER,						{ false,	{ false,	false },	{ RGB( 235,   0,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_BRACKET,					{ false,	{ true,		false },	{ RGB( 128,   0,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_SELECTED_AREA,				{ true,		{ false,	false },	{ RGB(  49, 106, 197 ),	RGB(  49, 106, 197 ) } } },
	ColorInfoIni{ STR_COLOR_SEARCH_WORD1,				{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 255, 255,   0 ) } } },
	ColorInfoIni{ STR_COLOR_SEARCH_WORD2,				{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 160, 255, 255 ) } } },
	ColorInfoIni{ STR_COLOR_SEARCH_WORD3,				{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 153, 255, 153 ) } } },
	ColorInfoIni{ STR_COLOR_SEARCH_WORD4,				{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 255, 153, 153 ) } } },
	ColorInfoIni{ STR_COLOR_SEARCH_WORD5,				{ true,		{ false,	false },	{ RGB(   0,   0,   0 ),	RGB( 255, 102, 255 ) } } },
	ColorInfoIni{ STR_COLOR_COMMENT,					{ true,		{ false,	false },	{ RGB(   0, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_SINGLE_QUOTE,				{ true,		{ false,	false },	{ RGB(  64, 128, 128 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_DOUBLE_QUOTE,				{ true,		{ false,	false },	{ RGB( 128,   0,  64 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_HERE_DOCUMENT,				{ false,	{ false,	false },	{ RGB( 128,   0,  64 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_URL,						{ true,		{ false,	true  },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD1,					{ true,		{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD2,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD3,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD4,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD5,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD6,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD7,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD8,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD9,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_KEYWORD10,					{ true,		{ false,	false },	{ RGB( 255, 128,   0 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD1,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD2,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD3,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD4,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD5,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD6,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD7,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD8,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD9,				{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_REGEX_KEYWORD10,			{ false,	{ false,	false },	{ RGB(   0,   0, 255 ),	RGB( 255, 251, 240 ) } } },
	ColorInfoIni{ STR_COLOR_DIFF_ADD,					{ true,		{ false,	false },	{ RGB(   0,   0, 210 ),	RGB( 162, 208, 255 ) } } },
	ColorInfoIni{ STR_COLOR_DIFF_CNG,					{ true,		{ false,	false },	{ RGB(   0, 111,   0 ),	RGB( 189, 253, 192 ) } } },
	ColorInfoIni{ STR_COLOR_DIFF_DEL,					{ true,		{ false,	false },	{ RGB( 213, 106,   0 ),	RGB( 255, 233, 172 ) } } },
	ColorInfoIni{ STR_COLOR_BOOKMARK,					{ true ,	{ false,	false },	{ RGB( 255, 251, 240 ),	RGB(   0, 128, 192 ) } } },
	ColorInfoIni{ STR_COLOR_PAGEVIEW,					{ true ,	{ false,	false },	{ RGB( 255, 251, 240 ),	RGB( 190, 230, 255 ) } } },
};

void ApplyDarkWorkbenchDefaults(ColorInfoBase& info, size_t index) noexcept
{
	auto& colors = info.m_sColorAttr;
	if (colors.m_cBACK == RGB(255, 251, 240) || colors.m_cBACK == RGB(0, 0, 0)) {
		colors.m_cBACK = RGB(30, 30, 30);
	} else if (colors.m_cBACK == RGB(239, 239, 239)) {
		colors.m_cBACK = RGB(37, 37, 38);
	} else if (colors.m_cBACK == RGB(243, 243, 243)) {
		colors.m_cBACK = RGB(42, 45, 46);
	}

	switch (index) {
	case COLORIDX_TEXT:
	case COLORIDX_CARET:
	case COLORIDX_BRACKET_PAIR:
		colors.m_cTEXT = RGB(212, 212, 212);
		break;
	case COLORIDX_RULER:
	case COLORIDX_GYOU:
	case COLORIDX_GYOU_MOD:
		colors.m_cTEXT = RGB(133, 133, 133);
		break;
	case COLORIDX_CARET_IME:
	case COLORIDX_CURSORVLINE:
	case COLORIDX_EOL:
	case COLORIDX_EOF:
	case COLORIDX_URL:
	case COLORIDX_KEYWORD1:
	case COLORIDX_REGEX1:
	case COLORIDX_REGEX2:
	case COLORIDX_REGEX3:
	case COLORIDX_REGEX4:
	case COLORIDX_REGEX5:
	case COLORIDX_REGEX6:
	case COLORIDX_REGEX7:
	case COLORIDX_REGEX8:
	case COLORIDX_REGEX9:
	case COLORIDX_REGEX10:
		colors.m_cTEXT = RGB(86, 156, 214);
		break;
	case COLORIDX_TAB:
	case COLORIDX_SPACE:
	case COLORIDX_ZENSPACE:
	case COLORIDX_VERTLINE:
	case COLORIDX_NOTELINE:
		colors.m_cTEXT = RGB(96, 96, 96);
		break;
	case COLORIDX_CTRLCODE:
		colors.m_cTEXT = RGB(220, 220, 170);
		break;
	case COLORIDX_WRAP:
	case COLORIDX_KEYWORD2:
	case COLORIDX_KEYWORD3:
	case COLORIDX_KEYWORD4:
	case COLORIDX_KEYWORD5:
	case COLORIDX_KEYWORD6:
	case COLORIDX_KEYWORD7:
	case COLORIDX_KEYWORD8:
	case COLORIDX_KEYWORD9:
	case COLORIDX_KEYWORD10:
		colors.m_cTEXT = RGB(197, 134, 192);
		break;
	case COLORIDX_DIGIT:
		colors.m_cTEXT = RGB(181, 206, 168);
		break;
	case COLORIDX_COMMENT:
		colors.m_cTEXT = RGB(106, 153, 85);
		break;
	case COLORIDX_SSTRING:
	case COLORIDX_WSTRING:
	case COLORIDX_HEREDOC:
		colors.m_cTEXT = RGB(206, 145, 120);
		break;
	case COLORIDX_DIFF_APPEND:
		colors = { RGB(156, 220, 254), RGB(38, 79, 120) };
		break;
	case COLORIDX_DIFF_CHANGE:
		colors = { RGB(181, 206, 168), RGB(20, 67, 43) };
		break;
	case COLORIDX_DIFF_DELETE:
		colors = { RGB(244, 135, 113), RGB(90, 29, 29) };
		break;
	case COLORIDX_MARK:
		colors = { RGB(255, 255, 255), RGB(0, 122, 204) };
		break;
	case COLORIDX_PAGEVIEW:
		colors = { RGB(212, 212, 212), RGB(37, 37, 38) };
		break;
	default:
		break;
	}
}

void GetDefaultColorInfo(ColorInfo* pColorInfo, size_t nIndex) noexcept
{
	static_assert(std::size(ColorInfo_DEFAULT) == size_t(COLORIDX_LAST));

	assert( nIndex < int(std::size(ColorInfo_DEFAULT)) );

	ColorInfoBase* p = pColorInfo;
	*p = ColorInfo_DEFAULT[nIndex].m_sColorInfo; //ColorInfoBase
	ApplyDarkWorkbenchDefaults(*p, nIndex);
	GetDefaultColorInfoName(pColorInfo, nIndex);
	pColorInfo->m_nColorIdx = int(nIndex);
}

void GetDefaultColorInfoName(ColorInfo* pColorInfo, size_t nIndex) noexcept
{
	static_assert(std::size(ColorInfo_DEFAULT) == size_t(COLORIDX_LAST));

	assert( nIndex < int(std::size(ColorInfo_DEFAULT)) );

	::wcscpy_s(pColorInfo->m_szName, LS(ColorInfo_DEFAULT[nIndex].m_nNameId));
}

int GetDefaultColorInfoCount() noexcept
{
	return int(std::size(ColorInfo_DEFAULT));
}
