/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "CType.h"
#include "view/colors/EColorIndexType.h"

/*! Markdown

	This is intentionally a separate type instead of extending the Text type.
	Existing profiles retain their saved Text settings, while a new profile gets
	a Markdown type selected by its file extension.
*/
void CType_Markdown::InitTypeConfigImp(STypeConfig* pType)
{
	wcscpy( pType->m_szTypeName, L"Markdown" );
	wcscpy( pType->m_szTypeExts, L"md,markdown,mdown,mkd" );

	// HTML comments are valid Markdown blocks and the standard comment colorer
	// handles multi-line comments once these delimiters are configured.
	pType->m_cBlockComments[0].SetBlockCommentRule( L"<!--", L"-->" );
	pType->m_eDefaultOutline = OUTLINE_TEXT;

	// Sakura's regular-expression colorer is line-oriented.  These defaults
	// cover the Markdown constructs it can represent without a new parser;
	// fenced code contents continue to use their declared language's plain text.
	int keywordPos = 0;
	wchar_t* pKeyword = pType->m_RegexKeywordList;
	pType->m_bUseRegexKeyword = true;

	auto addRegexKeyword = [&](int index, EColorIndexType colorIndex, const wchar_t* pattern){
		pType->m_RegexKeywordArr[index].m_nColorIndex = colorIndex;
		::wcsncpy_s(
			&pKeyword[keywordPos],
			std::size(pType->m_RegexKeywordList) - keywordPos,
			pattern,
			_TRUNCATE
		);
		keywordPos += int(wcslen(&pKeyword[keywordPos]) + 1);
	};

	addRegexKeyword( 0, COLORIDX_KEYWORD1, L"m/^[ \\t]{0,3}#{1,6}(?=[ \\t]|$).*/k" );
	addRegexKeyword( 1, COLORIDX_WSTRING,  L"m/^[ \\t]*(```|~~~)[^`~\\r\\n]*/k" );
	addRegexKeyword( 2, COLORIDX_WSTRING,  L"/`[^`\\r\\n]+`/k" );
	addRegexKeyword( 3, COLORIDX_URL,      L"/(?<=\\]\\()(?:<)?[^)\\s>]+/k" );
	addRegexKeyword( 4, COLORIDX_KEYWORD2, L"/(\\*\\*|__)(?=\\S)(?:.*?\\S)\\1/k" );
	addRegexKeyword( 5, COLORIDX_KEYWORD2, L"/(\\*|_)(?=\\S)(?:[^*\\r\\n]*?\\S)\\1/k" );
	addRegexKeyword( 6, COLORIDX_KEYWORD2, L"m/^[ \\t]*(?:[-+*]|\\d+[.)])(?=[ \\t])/k" );
	pKeyword[keywordPos] = L'\0';

	// Headings should stand out even in a profile using the default color theme.
	pType->m_ColorInfoArr[COLORIDX_KEYWORD1].m_sFontAttr.m_bBoldFont = true;
}
