/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "theme/CThemeService.h"
#include "workbench/IWorkbenchTool.h"
#include "workbench/search/SearchModel.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::search {

//! Every localized string the Search view renders.
//!
//! The view owns no resource ids: a pure Win32 control that loaded strings
//! itself would be a second localization path beside the composition root's.
//! Each field keeps VS Code's own English text as its fallback, so an
//! unresolved string still renders what upstream renders.
struct SearchViewTexts final {
	std::wstring searchPlaceholder = L"Search";
	std::wstring replacePlaceholder = L"Replace";
	std::wstring toggleReplace = L"Toggle Replace";
	std::wstring matchCase = L"Match Case";
	std::wstring wholeWord = L"Match Whole Word";
	std::wstring useRegex = L"Use Regular Expression";
	std::wstring preserveCase = L"Preserve Case";
	std::wstring replaceAll = L"Replace All";
	std::wstring replaceOne = L"Replace";
	std::wstring replaceInFile = L"Replace All";
	std::wstring dismiss = L"Dismiss";
	std::wstring noResults = L"No results found.";
	//! `{0}` matches, `{1}` files.
	std::wstring resultSummary = L"{0} results in {1} files";
	std::wstring searching = L"Searching...";
	std::wstring limitHit = L"The result set only contains a subset of all matches. Narrow your search.";
	std::wstring regexUnavailable = L"Regular expression support is unavailable.";
	std::wstring invalidPattern = L"Invalid regular expression.";
	//! Upstream's Search welcome content when no folder is open.
	std::wstring noWorkspace = L"You have not opened a folder in which to search.";
	std::wstring replaceFailed = L"{0} files could not be replaced.";
};

/*!
	@brief The native `workbench.view.search` ViewContainer.

	Models VS Code's Search view: one search widget (query box with Match Case,
	Match Whole Word, and Use Regular Expression toggles, plus a collapsible
	replace box with Preserve Case and Replace All) over a result tree grouped by
	file. Search runs on a worker thread; results reach the window as an
	immutable snapshot, so the rendered rows never observe a partial walk.
*/
class CSearchWorkbenchTool final : public IWorkbenchTool {
public:
	//! Opens one match: full path, 1-based line, 1-based column, match length.
	using MatchActivationCallback =
		std::function<void(std::wstring_view, std::int64_t, int, int)>;
	//! Raised after a replace pass rewrote files, so the host can reload any
	//! editor showing them. VS Code's replace goes through the text model for the
	//! same reason: a stale open document must not overwrite the new contents.
	using FilesChangedCallback = std::function<void(const std::vector<std::wstring>&)>;

	CSearchWorkbenchTool();
	~CSearchWorkbenchTool() override;
	CSearchWorkbenchTool(const CSearchWorkbenchTool&) = delete;
	CSearchWorkbenchTool& operator=(const CSearchWorkbenchTool&) = delete;

	bool Create(HWND parent) override;
	void Layout(const RECT& contentRect, unsigned int dpi) override;
	void Activate() override;
	void Deactivate() override;
	bool PreTranslateMessage(MSG& message) override;
	void Close() override;

	void SetRoot(std::wstring root);
	void SetPalette(const theme::ThemePalette& palette);
	void SetTexts(SearchViewTexts texts);
	void SetMatchActivationCallback(MatchActivationCallback callback);
	void SetFilesChangedCallback(FilesChangedCallback callback);
	void SetVisible(bool visible);
	//! Re-runs the current query, which is what `workbench.action.refreshSearch` does.
	void Refresh();
	//! Focuses the query box and selects its text, as `workbench.view.search` does.
	void FocusQuery();
	//! Reveals the replace box and focuses it, as `workbench.action.replaceInFiles` does.
	void FocusReplace();
	//! Seeds the query box, e.g. from the editor's selection.
	void SetQueryText(std::wstring text);
	[[nodiscard]] HWND GetHwnd() const noexcept;

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	static LRESULT CALLBACK EditSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data);
	static LRESULT CALLBACK ListSubclassProc(HWND window, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR id, DWORD_PTR data);

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::search
