/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/window/TerminalClipboardPaste.h"

#include <algorithm>

using terminal::FormatTerminalPastePath;
using terminal::SelectTerminalPasteImagesToRemove;
using terminal::TerminalPasteImageEntry;
using terminal::kMaxRetainedTerminalPasteImages;

TEST(TerminalClipboardPaste, LeavesSimplePathsUnquoted)
{
	EXPECT_EQ(L"C:\\Temp\\paste.png", FormatTerminalPastePath(L"C:\\Temp\\paste.png"));
	EXPECT_EQ(L"", FormatTerminalPastePath(L""));
}

TEST(TerminalClipboardPaste, QuotesPathsWithSpacesAndEscapesEmbeddedQuotes)
{
	EXPECT_EQ(L"\"C:\\Users\\Demo User\\paste.png\"",
		FormatTerminalPastePath(L"C:\\Users\\Demo User\\paste.png"));
	EXPECT_EQ(L"\"C:\\odd\"\"name.png\"", FormatTerminalPastePath(L"C:\\odd\"name.png"));
}

TEST(TerminalClipboardPaste, KeepsOnlyTheNewestPasteImages)
{
	std::vector<TerminalPasteImageEntry> entries{
		{ L"a.png", 10 },
		{ L"b.png", 30 },
		{ L"c.png", 20 },
		{ L"d.png", 40 },
	};
	const auto doomed = SelectTerminalPasteImagesToRemove(std::move(entries), 2);
	ASSERT_EQ(2u, doomed.size());
	EXPECT_NE(doomed.end(), std::find(doomed.begin(), doomed.end(), L"a.png"));
	EXPECT_NE(doomed.end(), std::find(doomed.begin(), doomed.end(), L"c.png"));
}

TEST(TerminalClipboardPaste, NeverRemovesTheJustSavedPasteImage)
{
	// Newest-first retention would drop `keep.png`, but the just-saved path is
	// protected so a clock quirk cannot delete the file whose path was pasted.
	std::vector<TerminalPasteImageEntry> entries{
		{ L"keep.png", 1 },
		{ L"old1.png", 100 },
		{ L"old2.png", 90 },
	};
	const auto doomed = SelectTerminalPasteImagesToRemove(std::move(entries), 1, L"keep.png");
	ASSERT_EQ(1u, doomed.size());
	EXPECT_EQ(L"old2.png", doomed.front());
	EXPECT_EQ(32u, kMaxRetainedTerminalPasteImages);
}
