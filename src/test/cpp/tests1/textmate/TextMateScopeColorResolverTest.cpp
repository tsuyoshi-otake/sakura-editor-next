/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "theme/CColorThemeRegistry.h"
#include "theme/CThemeService.h"
#include "theme/TextMateScopeColorResolver.h"

namespace {

using theme::ScopeColorMatch;
using theme::TextMateScopeColorResolver;
using theme::ThemeColor;
using theme::ThemeTokenColorRule;

ThemeTokenColorRule Rule(std::vector<std::wstring> scopes, ThemeColor foreground, std::wstring fontStyle = L"")
{
	ThemeTokenColorRule rule;
	rule.scopes = std::move(scopes);
	rule.foreground = foreground;
	rule.fontStyle = std::move(fontStyle);
	return rule;
}

TEST(TextMateScopeColorResolverTest, MatchSelector_ExactSingleSegment_Matches)
{
	const std::vector<std::wstring> scopePath{L"source.js", L"comment.line.double-slash.js"};

	const std::optional<int> score = TextMateScopeColorResolver::MatchSelectorForTesting(L"comment", scopePath);

	ASSERT_TRUE(score.has_value());
	EXPECT_EQ(1, *score);
}

TEST(TextMateScopeColorResolverTest, MatchSelector_DotSegmentPrefix_MatchesDescendantScope)
{
	// "comment" must match "comment.line.double-slash.js" (dot-segment prefix),
	// the same rule vscode-textmate applies to scope selectors.
	const std::vector<std::wstring> scopePath{L"comment.line.double-slash.js"};

	const std::optional<int> score = TextMateScopeColorResolver::MatchSelectorForTesting(L"comment", scopePath);

	ASSERT_TRUE(score.has_value());
	EXPECT_EQ(1, *score);
}

TEST(TextMateScopeColorResolverTest, MatchSelector_PrefixWithoutDotBoundary_DoesNotMatch)
{
	// "comment" must NOT match "commentary.foo" -- the character right after
	// the shared prefix must be a '.' (or nothing), never any other letter.
	const std::vector<std::wstring> scopePath{L"commentary.foo"};

	const std::optional<int> score = TextMateScopeColorResolver::MatchSelectorForTesting(L"comment", scopePath);

	EXPECT_FALSE(score.has_value());
}

TEST(TextMateScopeColorResolverTest, MatchSelector_MultiPartSelector_MatchesAncestorSubsequence)
{
	// "source.js string" is a descendant-combinator selector: "string" must
	// match the innermost scope, and "source.js" must match some strictly
	// shallower ancestor scope -- not necessarily the immediate parent.
	const std::vector<std::wstring> scopePath{L"source.js", L"meta.foo.js", L"string.quoted.double.js"};

	const std::optional<int> score = TextMateScopeColorResolver::MatchSelectorForTesting(L"source.js string", scopePath);

	ASSERT_TRUE(score.has_value());
	// "source.js" contributes 2 dot segments, "string" contributes 1.
	EXPECT_EQ(3, *score);
}

TEST(TextMateScopeColorResolverTest, MatchSelector_AncestorPartMustPrecedeLaterMatch)
{
	// The ancestor part ("source.js") can only match a scope strictly
	// shallower than wherever the later part ("string") matched. Here
	// "source.js" only appears *after* (deeper than) "string.quoted.js" in the
	// scope path, so the selector must fail to match even though both parts
	// individually appear somewhere in the path.
	const std::vector<std::wstring> scopePath{L"string.quoted.js", L"source.js"};

	const std::optional<int> score = TextMateScopeColorResolver::MatchSelectorForTesting(L"source.js string", scopePath);

	EXPECT_FALSE(score.has_value());
}

TEST(TextMateScopeColorResolverTest, MatchSelector_NoMatchingScope_ReturnsNullopt)
{
	const std::vector<std::wstring> scopePath{L"source.js"};

	const std::optional<int> score = TextMateScopeColorResolver::MatchSelectorForTesting(L"comment.line", scopePath);

	EXPECT_FALSE(score.has_value());
}

TEST(TextMateScopeColorResolverTest, MatchSelector_SpecificityIsSumOfDotSegments)
{
	const std::vector<std::wstring> scopePath{L"string.quoted.double.js"};

	const std::optional<int> exact = TextMateScopeColorResolver::MatchSelectorForTesting(L"string.quoted.double", scopePath);
	const std::optional<int> partial = TextMateScopeColorResolver::MatchSelectorForTesting(L"string", scopePath);

	ASSERT_TRUE(exact.has_value());
	ASSERT_TRUE(partial.has_value());
	EXPECT_EQ(3, *exact);
	EXPECT_EQ(1, *partial);
	EXPECT_GT(*exact, *partial);
}

TEST(TextMateScopeColorResolverTest, Resolve_EmptyScopesRule_ActsAsUniversalDefault)
{
	const std::vector<std::wstring> scopePath{L"source.js"};
	const ThemeColor defaultColor{0x10, 0x20, 0x30};
	std::vector<ThemeTokenColorRule> tokenColors{Rule({}, defaultColor)};

	const ScopeColorMatch match = TextMateScopeColorResolver::Resolve(scopePath, tokenColors);

	ASSERT_TRUE(match.matched);
	ASSERT_TRUE(match.foreground.has_value());
	EXPECT_EQ(defaultColor, *match.foreground);
}

TEST(TextMateScopeColorResolverTest, Resolve_MoreSpecificRuleWinsOverLessSpecific)
{
	const std::vector<std::wstring> scopePath{L"string.quoted.double.js"};
	const ThemeColor genericColor{0xAA, 0xAA, 0xAA};
	const ThemeColor specificColor{0xFF, 0x00, 0x00};
	std::vector<ThemeTokenColorRule> tokenColors{
		Rule({L"string"}, genericColor),
		Rule({L"string.quoted.double"}, specificColor),
	};

	const ScopeColorMatch match = TextMateScopeColorResolver::Resolve(scopePath, tokenColors);

	ASSERT_TRUE(match.matched);
	ASSERT_TRUE(match.foreground.has_value());
	EXPECT_EQ(specificColor, *match.foreground);
}

TEST(TextMateScopeColorResolverTest, Resolve_EqualSpecificityTie_LaterRuleWins)
{
	const std::vector<std::wstring> scopePath{L"string.quoted.double.js"};
	const ThemeColor firstColor{0x11, 0x11, 0x11};
	const ThemeColor secondColor{0x22, 0x22, 0x22};
	std::vector<ThemeTokenColorRule> tokenColors{
		Rule({L"string.quoted.double"}, firstColor),
		Rule({L"string.quoted.double"}, secondColor),
	};

	const ScopeColorMatch match = TextMateScopeColorResolver::Resolve(scopePath, tokenColors);

	ASSERT_TRUE(match.matched);
	ASSERT_TRUE(match.foreground.has_value());
	EXPECT_EQ(secondColor, *match.foreground);
}

TEST(TextMateScopeColorResolverTest, Resolve_NoRuleMatches_ReturnsUnmatched)
{
	const std::vector<std::wstring> scopePath{L"source.js"};
	std::vector<ThemeTokenColorRule> tokenColors{Rule({L"comment"}, ThemeColor{0, 0, 0})};

	const ScopeColorMatch match = TextMateScopeColorResolver::Resolve(scopePath, tokenColors);

	EXPECT_FALSE(match.matched);
	EXPECT_FALSE(match.foreground.has_value());
}

TEST(TextMateScopeColorResolverTest, Resolve_FontStylePassedThroughUninterpreted)
{
	const std::vector<std::wstring> scopePath{L"keyword.control.js"};
	std::vector<ThemeTokenColorRule> tokenColors{Rule({L"keyword"}, ThemeColor{0, 0, 0}, L"italic bold")};

	const ScopeColorMatch match = TextMateScopeColorResolver::Resolve(scopePath, tokenColors);

	ASSERT_TRUE(match.matched);
	EXPECT_EQ(L"italic bold", match.fontStyle);
}

} // namespace
