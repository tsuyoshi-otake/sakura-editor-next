/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "CSelectLang.h"

#include <Windows.h>

#include <filesystem>

namespace tests1 {

//! Restores both the selected resource module and thread UI language on all
//! GoogleTest exit paths, including ASSERT_*/exception unwinding. When the
//! test ran with no language environment at all, the restore also leaves
//! gm_Langs/gm_Selected empty instead of keeping a half-initialized state;
//! an out-of-range gm_Selected is treated like an absent selection rather
//! than indexing gm_Langs out of bounds.
class ScopedLanguageSelection final {
public:
	ScopedLanguageSelection()
		: m_threadLanguage(::GetThreadUILanguage())
		, m_hadLanguageEnvironment(!CSelectLang::gm_Langs.empty())
	{
		if (m_hadLanguageEnvironment && CSelectLang::gm_Selected < CSelectLang::gm_Langs.size()) {
			m_previousDll = CSelectLang::GetLangInfo(CSelectLang::gm_Selected).GetDllName();
		}
	}

	~ScopedLanguageSelection()
	{
		if (m_hadLanguageEnvironment) {
			CSelectLang::ChangeLang(m_previousDll);
		} else {
			CSelectLang::ChangeLang(L"");
			CSelectLang::gm_Langs.clear();
			CSelectLang::gm_Selected = 0;
		}
		::SetThreadUILanguage(m_threadLanguage);
	}

	ScopedLanguageSelection(const ScopedLanguageSelection&) = delete;
	ScopedLanguageSelection& operator=(const ScopedLanguageSelection&) = delete;

private:
	LANGID m_threadLanguage;
	bool m_hadLanguageEnvironment;
	std::filesystem::path m_previousDll;
};

} // namespace tests1
