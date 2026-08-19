/*! @file
	@brief Register the bundled seti.ttf as a process-private font
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/icons/CSetiFont.h"

#include "workbench/icons/EmbeddedFontResource.h"
#include "workbench/icons/SetiFileIcon.h"

namespace workbench::icons {

namespace {

//! Declared in sakura_rc.rc2 as `SETIFONT RCDATA "workbench/icons/seti.ttf"`.
constexpr const wchar_t* kSetiResourceName = L"SETIFONT";

} // namespace

CSetiFont::CSetiFont() noexcept
{
	m_fontResourceHandle = RegisterEmbeddedFontResource(kSetiResourceName, m_fontBytes);
	if (m_fontResourceHandle == nullptr) return;
	// The name the theme table is written against. Keeping the two in one place means
	// a resolved glyph and the face it is drawn in can never disagree.
	m_faceName = seti::kFontFamily;
}

CSetiFont::~CSetiFont()
{
	UnregisterEmbeddedFontResource(m_fontResourceHandle);
	m_fontResourceHandle = nullptr;
}

const CSetiFont& CSetiFont::Instance() noexcept
{
	static const CSetiFont instance;
	return instance;
}

} // namespace workbench::icons
