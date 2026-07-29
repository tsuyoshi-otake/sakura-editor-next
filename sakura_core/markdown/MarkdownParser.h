/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace markdown {

//! The block types rendered by the lightweight native Markdown preview.
enum class BlockKind {
	Heading,
	Paragraph,
	BulletListItem,
	OrderedListItem,
	BlockQuote,
	CodeBlock,
	HorizontalRule,
};

//! Inline decoration kinds.  The preview deliberately keeps this surface small.
enum class InlineKind {
	Link,
};

struct InlineSpan {
	InlineKind kind = InlineKind::Link;
	std::size_t start = 0;
	std::size_t length = 0;
};

struct Block {
	BlockKind kind = BlockKind::Paragraph;
	//! Heading depth for headings, indentation depth for list items.
	int level = 0;
	//! A rendered list marker, empty for non-list blocks.
	std::wstring marker;
	//! Plain text after inline Markdown has been normalized for display.
	std::wstring text;
	//! Ranges in text that receive an inline visual treatment.
	std::vector<InlineSpan> inlineSpans;
};

struct Document {
	std::vector<Block> blocks;
};

//! Parses the preview's intentionally bounded Markdown subset without I/O or window state.
[[nodiscard]] Document ParseMarkdown(std::wstring_view source);

} // namespace markdown
