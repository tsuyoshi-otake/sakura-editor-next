/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "theme/TextMateScopeColorResolver.h"

#include <cwctype>

namespace theme {

namespace {

//! `part` matches `scope` when `scope` equals `part` or begins with
//! `part + L"."` — i.e. dot-segment prefix matching, the same rule
//! `vscode-textmate` uses (`"comment"` matches `"comment.line.double-slash.js"`
//! but not `"commentary"`).
[[nodiscard]] bool ScopeSegmentMatches(std::wstring_view scope, std::wstring_view part) noexcept
{
	if (part.size() > scope.size()) return false;
	if (scope.substr(0, part.size()) != part) return false;
	return scope.size() == part.size() || scope[part.size()] == L'.';
}

[[nodiscard]] int CountDotSegments(std::wstring_view part) noexcept
{
	int count = 1;
	for (const wchar_t c : part) {
		if (c == L'.') ++count;
	}
	return count;
}

[[nodiscard]] std::vector<std::wstring_view> SplitOnWhitespace(std::wstring_view selector)
{
	std::vector<std::wstring_view> parts;
	std::size_t i = 0;
	while (i < selector.size()) {
		while (i < selector.size() && std::iswspace(static_cast<wint_t>(selector[i]))) ++i;
		const std::size_t start = i;
		while (i < selector.size() && !std::iswspace(static_cast<wint_t>(selector[i]))) ++i;
		if (i > start) parts.push_back(selector.substr(start, i - start));
	}
	return parts;
}

} // namespace

std::optional<int> TextMateScopeColorResolver::MatchSelectorForTesting(
	std::wstring_view selector, const std::vector<std::wstring>& scopePath) noexcept
{
	const std::vector<std::wstring_view> parts = SplitOnWhitespace(selector);
	if (parts.empty()) return std::nullopt;

	// Ancestor parts (all but the last) must match scopes strictly shallower
	// than wherever the part after them matched, walking outward from the
	// innermost scope -- a non-contiguous, order-preserving subsequence match,
	// matching `vscode-textmate`'s descendant-combinator semantics for plain
	// space-separated selectors (the only combinator this resolver supports;
	// see the header comment for what is deliberately not implemented).
	std::size_t upperBoundExclusive = scopePath.size();
	int totalScore = 0;
	for (std::size_t k = parts.size(); k-- > 0;) {
		const std::wstring_view part = parts[k];
		bool found = false;
		for (std::size_t idx = upperBoundExclusive; idx-- > 0;) {
			if (ScopeSegmentMatches(scopePath[idx], part)) {
				totalScore += CountDotSegments(part);
				upperBoundExclusive = idx;
				found = true;
				break;
			}
		}
		if (!found) return std::nullopt;
	}
	return totalScore;
}

ScopeColorMatch TextMateScopeColorResolver::Resolve(
	const std::vector<std::wstring>& scopePath, const std::vector<ThemeTokenColorRule>& tokenColors) noexcept
{
	ScopeColorMatch best;
	int bestScore = -1;

	for (const ThemeTokenColorRule& rule : tokenColors) {
		int ruleBestScore = -1;
		if (rule.scopes.empty()) {
			// VS Code treats a `tokenColors` entry with no `scope` at all as a
			// theme-wide default (typically the first entry in a theme file);
			// it matches every scope path at the lowest possible specificity.
			ruleBestScore = 0;
		} else {
			for (const std::wstring& selector : rule.scopes) {
				const std::optional<int> score = MatchSelectorForTesting(selector, scopePath);
				if (score.has_value() && *score > ruleBestScore) ruleBestScore = *score;
			}
		}
		if (ruleBestScore < 0) continue;

		// `>=` rather than `>`: on an exact specificity tie, the later rule in
		// `tokenColors` wins, matching the file-order override VS Code applies
		// when a theme lists two equally-specific rules for the same scope.
		if (ruleBestScore >= bestScore) {
			bestScore = ruleBestScore;
			best.foreground = rule.foreground;
			best.background = rule.background;
			best.fontStyle = rule.fontStyle;
			best.matched = true;
		}
	}

	return best;
}

} // namespace theme
