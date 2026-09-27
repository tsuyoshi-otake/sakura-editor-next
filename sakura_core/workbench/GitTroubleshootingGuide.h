/*! @file
 * @brief Opens the locale-matched Git and GitHub troubleshooting guide.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include "CSelectLang.h"
#include "sakura_rc.h"

#include <Windows.h>
#include <ShellAPI.h>
#include <string>

namespace workbench {

[[nodiscard]] inline const wchar_t* GitTroubleshootingGuideUrl() noexcept
{
	switch (PRIMARYLANGID(CSelectLang::getDefaultLangId())) {
	case LANG_JAPANESE:
		return L"https://github.com/tsuyoshi-otake/sakura-editor-next/blob/main/docs/user/git-github-cli-troubleshooting.md";
	case LANG_CHINESE:
		return L"https://github.com/tsuyoshi-otake/sakura-editor-next/blob/main/docs/user/git-github-cli-troubleshooting.zh-CN.md";
	default:
		return L"https://github.com/tsuyoshi-otake/sakura-editor-next/blob/main/docs/user/git-github-cli-troubleshooting.en.md";
	}
}

inline void OpenGitTroubleshootingGuide(HWND parent, const std::wstring& title)
{
	const auto* url = GitTroubleshootingGuideUrl();
	SHELLEXECUTEINFOW info{};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	info.lpVerb = L"open";
	info.lpFile = url;
	info.nShow = SW_SHOWNORMAL;
	if (::ShellExecuteExW(&info)) return;

	const auto localized = CSelectLang::LoadStringW(STR_WORKBENCH_VIEW_HELP_OPEN_FAILED);
	std::wstring error = localized.empty()
		? L"Could not open the guide. Open this URL in a browser:"
		: std::wstring(localized);
	error.append(L"\n").append(url);
	::MessageBoxW(parent, error.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
}

} // namespace workbench
