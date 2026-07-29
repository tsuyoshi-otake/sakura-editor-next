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
