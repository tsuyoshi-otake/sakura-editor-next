/*! @file
 * @brief HWND-free label model for `copyRelativeFilePath`, in VS Code's shape.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cwctype>
#include <string>
#include <string_view>

namespace workbench::explorer {

[[nodiscard]] inline bool IsExplorerPathSeparator(wchar_t character) noexcept
{
	return character == L'\\' || character == L'/';
}

//!
//! @brief Builds the clipboard label `copyRelativeFilePath` produces for one
//! resource against the single workspace root.
//!
//! Upstream resolves the label through `ILabelService.getUriLabel` with
//! `relative: true` (labelService.ts `doGetUriLabel`):
//!
//! - The workspace root itself labels as the empty string - upstream's
//!   `isEqual(folder.uri, resource)` branch - because in a single-root
//!   workspace no root-name prefix is added.
//! - A resource under the root labels as the path remainder with leading
//!   separators trimmed, in the platform's own separators; the Windows tree
//!   already carries backslashes and they are preserved untouched.
//! - A resource outside the root keeps its absolute path: there is no
//!   containing folder, so no relative form exists.
//!
//! Comparison folds ASCII-style case per `towlower` and treats `/` and `\\`
//! as the same separator.  That is a label heuristic, not filesystem-identity
//! resolution; the command copies text, it does not open files.
//!
[[nodiscard]] inline std::wstring BuildExplorerRelativePathLabel(
	std::wstring_view workspaceRootPath, std::wstring_view resourcePath)
{
	while (!workspaceRootPath.empty() && IsExplorerPathSeparator(workspaceRootPath.back())) {
		workspaceRootPath.remove_suffix(1);
	}

	const auto foldsEqual = [](wchar_t left, wchar_t right) noexcept {
		if (IsExplorerPathSeparator(left) && IsExplorerPathSeparator(right)) {
			return true;
		}
		return std::towlower(left) == std::towlower(right);
	};

	if (workspaceRootPath.empty() || workspaceRootPath.size() > resourcePath.size()) {
		return std::wstring(resourcePath);
	}
	for (std::size_t index = 0; index < workspaceRootPath.size(); ++index) {
		if (!foldsEqual(workspaceRootPath[index], resourcePath[index])) {
			return std::wstring(resourcePath);
		}
	}

	std::wstring_view remainder = resourcePath.substr(workspaceRootPath.size());
	while (!remainder.empty() && IsExplorerPathSeparator(remainder.front())) {
		remainder.remove_prefix(1);
	}
	if (remainder.size() == resourcePath.size() - workspaceRootPath.size()) {
		// No separator followed the root prefix: the resource is the root
		// itself (empty remainder) or a sibling whose name merely extends the
		// root's last segment ("C:\src" vs "C:\srcfoo") - the latter is not
		// under the root and keeps its absolute path.
		return remainder.empty() ? std::wstring() : std::wstring(resourcePath);
	}

	return std::wstring(remainder);
}

//!
//! @brief Validates one entered Explorer entry name exactly as upstream VS
//! Code's `isValidBasename` (extpath.ts, Windows flavor) does.
//!
//! Rejected: empty or whitespace-only names, names over 255 characters, the
//! reserved `.` / `..`, any of the Windows-invalid characters
//! `\\ / : * ? " < > |`, names starting or ending with whitespace, names
//! ending with `.`, and the reserved device names (`CON`, `PRN`, `AUX`,
//! `NUL`, `CLOCK$`, `COM0`-`COM9`, `LPT0`-`LPT9`, case-insensitively).  An
//! invalid name never reaches the filesystem boundary; the inline editor
//! treats it as no commit.
//!
[[nodiscard]] inline bool IsValidExplorerEntryName(std::wstring_view name)
{
	if (name.empty() || name.size() > 255) {
		return false;
	}
	bool allWhitespace = true;
	for (const wchar_t character : name) {
		switch (character) {
		case L'\\': case L'/': case L':': case L'*': case L'?':
		case L'"': case L'<': case L'>': case L'|':
			return false;
		default:
			break;
		}
		if (!std::iswspace(character)) {
			allWhitespace = false;
		}
	}
	if (allWhitespace) {
		return false;
	}
	if (std::iswspace(name.front()) || std::iswspace(name.back())) {
		return false;
	}
	if (name == L"." || name == L".." || name.back() == L'.') {
		return false;
	}

	std::wstring folded(name);
	for (auto& character : folded) {
		character = static_cast<wchar_t>(std::towlower(character));
	}
	if (folded == L"con" || folded == L"prn" || folded == L"aux" || folded == L"nul"
		|| folded == L"clock$") {
		return false;
	}
	if (folded.size() == 4 && (folded.compare(0, 3, L"com") == 0 || folded.compare(0, 3, L"lpt") == 0)
		&& folded[3] >= L'0' && folded[3] <= L'9') {
		return false;
	}
	return true;
}

} // namespace workbench::explorer
