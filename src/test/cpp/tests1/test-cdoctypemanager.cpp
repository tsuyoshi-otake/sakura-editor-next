/*
	Copyright (C) 2021-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"
#include <windows.h>
#include <string>
#include "mem/CNativeW.h"
#include "env/CDocTypeManager.h"
#include "doc/CDocTypeSetting.h"
#include "view/colors/EColorIndexType.h"

TEST(CTypeMarkdown, FreshDefaultsRecognizeMarkdownAndConfigureHighlighting)
{
	STypeConfig type{};
	CType_Markdown().InitTypeConfig(17, type);

	EXPECT_THAT(type.m_szTypeName, StrEq(L"Markdown"));
	EXPECT_THAT(type.m_szTypeExts, StrEq(L"md,markdown,mdown,mkd"));
	EXPECT_TRUE(CDocTypeManager::IsFileNameMatch(type.m_szTypeExts, L"README.md"));
	EXPECT_TRUE(CDocTypeManager::IsFileNameMatch(type.m_szTypeExts, L"README.markdown"));
	EXPECT_FALSE(CDocTypeManager::IsFileNameMatch(type.m_szTypeExts, L"README.txt"));
	EXPECT_EQ(OUTLINE_TEXT, type.m_eDefaultOutline);
	EXPECT_THAT(type.m_cBlockComments[0].getBlockCommentFrom(), StrEq(L"<!--"));
	EXPECT_THAT(type.m_cBlockComments[0].getBlockCommentTo(), StrEq(L"-->"));
	EXPECT_TRUE(type.m_bUseRegexKeyword);
	EXPECT_TRUE(type.m_ColorInfoArr[COLORIDX_KEYWORD1].m_sFontAttr.m_bBoldFont);

	const std::array expected = {
		L"m/^[ \\t]{0,3}#{1,6}(?=[ \\t]|$).*/k",
		L"m/^[ \\t]*(```|~~~)[^`~\\r\\n]*/k",
		L"/`[^`\\r\\n]+`/k",
		L"/(?<=\\]\\()(?:<)?[^)\\s>]+/k",
		L"/(\\*\\*|__)(?=\\S)(?:.*?\\S)\\1/k",
		L"/(\\*|_)(?=\\S)(?:[^*\\r\\n]*?\\S)\\1/k",
		L"m/^[ \\t]*(?:[-+*]|\\d+[.)])(?=[ \\t])/k",
	};
	const wchar_t* regex = type.m_RegexKeywordList;
	for( size_t i = 0; i < std::size(expected); ++i ){
		EXPECT_THAT(regex, StrEq(expected[i]));
		EXPECT_TRUE(CRegexKeyword::RegexKeyCheckSyntax(regex));
		regex += wcslen(regex) + 1;
	}
	EXPECT_EQ(L'\0', regex[0]);

	EXPECT_EQ(COLORIDX_KEYWORD1, type.m_RegexKeywordArr[0].m_nColorIndex);
	EXPECT_EQ(COLORIDX_WSTRING, type.m_RegexKeywordArr[1].m_nColorIndex);
	EXPECT_EQ(COLORIDX_WSTRING, type.m_RegexKeywordArr[2].m_nColorIndex);
	EXPECT_EQ(COLORIDX_URL, type.m_RegexKeywordArr[3].m_nColorIndex);
	EXPECT_EQ(COLORIDX_KEYWORD2, type.m_RegexKeywordArr[4].m_nColorIndex);
	EXPECT_EQ(COLORIDX_KEYWORD2, type.m_RegexKeywordArr[5].m_nColorIndex);
	EXPECT_EQ(COLORIDX_KEYWORD2, type.m_RegexKeywordArr[6].m_nColorIndex);

	CRegexKeyword regexKeyword(L"bregonig.dll");
	ASSERT_TRUE(regexKeyword.RegexKeySetTypes(&type));
	struct RegexCase {
		const wchar_t* line;
		int position;
		EColorIndexType color;
	};
	const std::array regexCases = {
		RegexCase{ L"## Heading", 0, COLORIDX_KEYWORD1 },
		RegexCase{ L"```cpp", 0, COLORIDX_WSTRING },
		RegexCase{ L"Use `code`", 4, COLORIDX_WSTRING },
		RegexCase{ L"[guide](guide.md)", 8, COLORIDX_URL },
		RegexCase{ L"**bold**", 0, COLORIDX_KEYWORD2 },
		RegexCase{ L"*italic*", 0, COLORIDX_KEYWORD2 },
		RegexCase{ L"- item", 0, COLORIDX_KEYWORD2 },
	};
	for( const auto& test : regexCases ){
		ASSERT_TRUE(regexKeyword.RegexKeyLineStart());
		const CStringRef line{ test.line, wcslen(test.line) };
		int matchLength = 0;
		int matchColor = COLORIDX_DEFAULT;
		EXPECT_TRUE(regexKeyword.RegexIsKeyword(line, test.position, &matchLength, &matchColor));
		EXPECT_GT(matchLength, 0);
		EXPECT_EQ(test.color, matchColor);
	}
}

TEST(CDocTypeSetting, NewProfilesUseWorkbenchDarkEditorColors)
{
	ColorInfo text{};
	GetDefaultColorInfo(&text, COLORIDX_TEXT);
	EXPECT_EQ(RGB(212, 212, 212), text.m_sColorAttr.m_cTEXT);
	EXPECT_EQ(RGB(30, 30, 30), text.m_sColorAttr.m_cBACK);

	ColorInfo ruler{};
	GetDefaultColorInfo(&ruler, COLORIDX_RULER);
	EXPECT_EQ(RGB(133, 133, 133), ruler.m_sColorAttr.m_cTEXT);
	EXPECT_EQ(RGB(37, 37, 38), ruler.m_sColorAttr.m_cBACK);

	ColorInfo comment{};
	GetDefaultColorInfo(&comment, COLORIDX_COMMENT);
	EXPECT_EQ(RGB(106, 153, 85), comment.m_sColorAttr.m_cTEXT);
	EXPECT_EQ(RGB(30, 30, 30), comment.m_sColorAttr.m_cBACK);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtNullptr1)
{
	const std::wstring expected = { L"" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(nullptr, nullptr);
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtNullptr2)
{
	const std::wstring expected = { L"" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(nullptr, L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtOnce)
{
	const std::wstring expected = { L"*.txt;*.cpp" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtTwo)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp;h", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtThree)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h;*.hpp" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp;h;hpp", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtAppendPeriod)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L".cpp;.h", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtSeparatorSpace)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp h", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtSeparatorComma)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h" } ;
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp,h", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtTopNullptr)
{
	const std::wstring expected = { L"*.cpp;*.h" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp,h", nullptr);
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtMerge)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"txt,cpp,h", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtMerge2)
{
	const std::wstring expected = { L"*.txt;*.cpp;*.h" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"cpp,h,txt", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtExts64)
{
	const std::wstring expected = { L"*.txt;*.a;*.b;*.c;*.d;*.e;*.f;*.g;*.h;*.i;*.j;*.k;*.l;*.m;*.n;*.o;*.p;*.q;*.r;*.s;*.t;*.u;*.v;*.w;*.x;*.y;*.z;*.1;*.2;*.3;*.4;*.5;*.6" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z,1,2,3,4,5,6", L".txt");
	EXPECT_EQ(expected, actual);
}

TEST(CDocTypeManager, ConvertTypesExtToDlgExtExts64LongFileExt)
{
	const std::wstring expected = { L"*.extension_260_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long;*.a;*.b;*.c;*.d;*.e;*.f;*.g;*.h;*.i;*.j;*.k;*.l;*.m;*.n;*.o;*.p;*.q;*.r;*.s;*.t;*.u;*.v;*.w;*.x;*.y;*.z;*.1;*.2;*.3;*.4;*.5;*.6" };
	std::wstring actual = CDocTypeManager::ConvertTypesExtToDlgExt(L"a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z,1,2,3,4,5,6", L".extension_260_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long_long");
	EXPECT_EQ(expected, actual);
}
