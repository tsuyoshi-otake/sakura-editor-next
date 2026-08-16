/*! @file
	@brief Terminal clipboard paste helpers for text, dropped files, and images

	Claude Code, Codex, and Cursor CLI accept image *paths*, not raw clipboard
	bitmaps. When paste finds an image and no useful Unicode text, Sakura writes a
	PNG under the process temp directory and pastes that absolute path through the
	same bracketed-paste encoder used for text. Successful saves keep only the
	newest `kMaxRetainedTerminalPasteImages` PNGs in that folder.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace terminal {

//! Cap on retained paste PNGs under `%TEMP%\\sakura-editor\\terminal-paste\\`.
inline constexpr std::size_t kMaxRetainedTerminalPasteImages = 32;

//! Quotes a Windows path for paste when it contains spaces or special shell characters.
[[nodiscard]] std::wstring FormatTerminalPastePath(std::wstring_view path);

struct TerminalPasteImageEntry {
	std::wstring path;
	//! Arbitrary monotonic clock for tests; production uses last-write time.
	std::int64_t lastWriteTimeMs = 0;
};

/*!
	@brief Chooses which paste images to delete after a successful save.

	Entries are sorted newest-first; everything after `keepNewest` is removed.
	`keepPath` is never selected even when the list is larger than the cap.
*/
[[nodiscard]] std::vector<std::wstring> SelectTerminalPasteImagesToRemove(
	std::vector<TerminalPasteImageEntry> entries,
	std::size_t keepNewest,
	std::wstring_view keepPath = {});

/*!
	@brief Saves a clipboard bitmap as a PNG under `%TEMP%\\sakura-editor\\terminal-paste\\`.

	Prefers the registered `PNG` clipboard format, then `CF_DIB` / `CF_DIBV5`.
	Returns the absolute path on success. The clipboard must not already be open
	by the caller. After a successful write, older PNGs beyond
	`kMaxRetainedTerminalPasteImages` are deleted.
*/
[[nodiscard]] std::optional<std::wstring> SaveClipboardImageAsPng(HWND owner);

//! Returns absolute paths from `CF_HDROP`, empty when the clipboard has no file drop.
[[nodiscard]] std::vector<std::wstring> ReadClipboardFileDropPaths(HWND owner);

} // namespace terminal
