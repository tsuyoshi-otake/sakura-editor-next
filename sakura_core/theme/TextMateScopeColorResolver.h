/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "theme/CColorThemeRegistry.h"

namespace theme {

//! One token's resolved style, or `matched == false` when no rule in the
//! theme's `tokenColors` applied to the given scope path at all (the caller's
//! existing coarse-category fallback should be used in that case, exactly as
//! `CColorThemeRegistry::ProjectSyntaxPalette` already falls back today).
struct ScopeColorMatch final {
	std::optional<ThemeColor> foreground;
	std::optional<ThemeColor> background;
	std::wstring fontStyle;
	bool matched = false;
};

//! Resolves one TextMate scope path (as produced by a grammar tokenizer, e.g.
//! `textmate::TextMateToken::scopes` — outermost scope first, innermost/most
//! specific scope last) against a theme's `tokenColors` rules, using VS
//! Code's scope-selector specificity ranking: the matching rule whose
//! selector accounts for the most dot-separated scope segments wins, and the
//! later-declared rule wins an exact specificity tie (VS Code applies
//! `tokenColors` in file order, so a later entry is a deliberate override of
//! an earlier, equally-specific one).
//!
//! This resolver has no dependency on `sakura_core/textmate` — it takes a
//! plain `std::vector<std::wstring>` rather than including
//! `TextMateTokenizer.h`, so `theme/` does not gain a compile dependency on
//! `textmate/`. This keeps the dependency direction the caller's choice: an
//! integration layer that already depends on both `textmate::TextMateToken`
//! and `theme::ColorThemeSnapshot` is expected to pass
//! `token.scopes` straight through.
//!
//! Deliberately unimplemented, matching `theme/CLAUDE.md`'s existing
//! "not a full TextMate grammar/scope-selector engine" boundary statement:
//! - The `>` direct-parent-only combinator is not recognized; a selector that
//!   uses it is tokenized as an ordinary (non-matching, since `>` cannot
//!   prefix-match any real scope name) selector segment, so the whole
//!   selector simply never matches — a fail-closed degradation, not a
//!   silent misinterpretation as the (more permissive) descendant
//!   combinator.
//! - The `-` exclusion and `|`/`,`-inside-one-string union/grouping
//!   combinators, and parenthesized groups, are not recognized, for the same
//!   reason and with the same fail-closed degradation.
//! - `fontStyle` is returned as the theme's raw string (e.g. `"italic bold"`)
//!   uninterpreted; turning it into typed flags is the caller's job, exactly
//!   as `theme/CLAUDE.md` already calls out italics as unsupported today.
//!
//! No production caller wires this resolver's output into rendering yet —
//! see `textmate/CLAUDE.md`; this type exists to define the boundary stage 4
//! of the TextMate grammar engine work requires, not to complete the wiring.
class TextMateScopeColorResolver final {
public:
	TextMateScopeColorResolver() = delete;

	[[nodiscard]] static ScopeColorMatch Resolve(
		const std::vector<std::wstring>& scopePath, const std::vector<ThemeTokenColorRule>& tokenColors) noexcept;

	//! Exposed for direct unit testing of the specificity ranking in
	//! isolation from a full `ThemeTokenColorRule` list. Returns the selector's
	//! match score (the total number of dot-separated segments across every
	//! selector part that matched) or `std::nullopt` when `selector` does not
	//! match `scopePath` at all.
	[[nodiscard]] static std::optional<int> MatchSelectorForTesting(
		std::wstring_view selector, const std::vector<std::wstring>& scopePath) noexcept;
};

} // namespace theme
