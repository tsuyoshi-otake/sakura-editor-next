/*! @file @brief Bounded native syntax highlighting for Markdown fenced code. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace markdown {

//! Semantic token classes consumed by the native Markdown preview renderer.
enum class CodeTokenKind : std::uint8_t {
	Keyword,
	Type,
	Literal,
	Number,
	String,
	Comment,
	Operator,
	Punctuation,
	Preprocessor,
	Tag,
	Attribute,
	Variable,
	Heading,
	Emphasis,
	Code,
	Link,
};

//! Every scan finishes in one explicit terminal state, including malformed input.
enum class CodeHighlightTerminalState : std::uint8_t {
	Completed,
	UnterminatedString,
	UnterminatedComment,
	UnterminatedConstruct,
	TokenLimitReached,
};

struct CodeHighlightToken {
	CodeTokenKind kind = CodeTokenKind::Keyword;
	//! Zero-based UTF-16 code-unit offset into the unchanged input.
	std::size_t start = 0;
	//! UTF-16 code-unit length. Tokens are non-empty, ordered, and non-overlapping.
	std::size_t length = 0;

	bool operator==(const CodeHighlightToken&) const = default;
};

struct CodeHighlightResult {
	//! Canonical language name. Unsupported inputs normalize to "plain".
	std::wstring language;
	std::vector<CodeHighlightToken> tokens;
	CodeHighlightTerminalState terminalState = CodeHighlightTerminalState::Completed;
	//! Number of UTF-16 code units consumed by the state machine.
	std::size_t scannedLength = 0;
	//! Deterministic character probes plus SIMD-skipped code units, for linearity tests.
	std::size_t workUnits = 0;
};

inline constexpr std::size_t kDefaultMaximumCodeHighlightTokens = 16 * 1024;

//! Trims and ASCII-folds a fence language, then maps aliases to a supported name.
[[nodiscard]] std::wstring NormalizeMarkdownCodeLanguage(std::wstring_view language);

//! Highlights without modifying source. Token output is bounded while scanning still
//! reaches the end, so callers can distinguish truncation from malformed input.
[[nodiscard]] CodeHighlightResult HighlightMarkdownCode(
	std::wstring_view language,
	std::wstring_view source,
	std::size_t maximumTokens = kDefaultMaximumCodeHighlightTokens);

} // namespace markdown
