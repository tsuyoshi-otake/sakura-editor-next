/*! @file
	@brief 同梱した codicon.ttf をプロセス private フォントとして登録する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/icons/CCodiconFont.h"

#include "workbench/icons/EmbeddedFontResource.h"

namespace workbench::icons {

namespace {

//! sakura_rc.rc2 が `CODICONFONT RCDATA "workbench/icons/codicon.ttf"` で埋め込む名前。
//! 名前付きリソースにしてあるので、既存の数値 ID 空間とは衝突し得ない。
constexpr const wchar_t* kCodiconResourceName = L"CODICONFONT";

} // namespace

CCodiconFont::CCodiconFont() noexcept
{
	m_fontResourceHandle = RegisterEmbeddedFontResource(kCodiconResourceName, m_fontBytes);
	if (m_fontResourceHandle == nullptr) return;
	m_faceName = L"codicon";
}

CCodiconFont::~CCodiconFont()
{
	UnregisterEmbeddedFontResource(m_fontResourceHandle);
	m_fontResourceHandle = nullptr;
}

const CCodiconFont& CCodiconFont::Instance() noexcept
{
	static const CCodiconFont instance;
	return instance;
}

} // namespace workbench::icons
