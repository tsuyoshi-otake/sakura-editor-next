/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/TextMateTokenizer.h"

#include <algorithm>

#include "textmate/TextMateUtf8.h"

namespace textmate {

namespace {

constexpr int kMaxIncludeExpansionDepth = 64;

//! One match candidate considered at the current position: either a
//! resolved leaf rule (`Match`/`BeginEnd`/`BeginWhile`) reached by expanding
//! `patterns`/`include`, or the current frame's own `end`/`while` pattern.
struct Candidate final {
	const TextMateRule* rule = nullptr; // null when isFrameBoundary
	OnigmoPattern* pattern = nullptr;
	bool isFrameBoundary = false; // true => this is the current frame's end/while
};

//! Expands `holder->patterns` into leaf match candidates, recursively
//! flattening `IncludeOnly` targets (an `include` that resolves to a bare
//! `{ "patterns": [...] }` node, most commonly `$self`/`#name` pointing at
//! another grouping rule). `visitedIncludeOnly` plus `depth` bound the
//! recursion so a self-referential chain of `IncludeOnly` rules (legal, if
//! unusual, TextMate grammar shape) cannot loop forever; hitting the bound
//! silently stops expanding rather than failing the whole tokenize call.
//!
//! `homeGrammar` is the `Grammar` that owns `holder` and therefore owns the
//! plain `RuleId` entries in `holder->patterns`; it changes when recursion
//! crosses an `ExternalGrammar` include (`source.foo`/`source.foo#name`) so
//! that a foreign grammar's own `RuleId`s are never looked up in the wrong
//! arena — a `RuleId` is only a valid index within the `Grammar` that
//! produced it.
void CollectMatchableRules(
	const Grammar& homeGrammar,
	IExternalGrammarResolver* externalResolver,
	const TextMateRule* holder,
	std::vector<const TextMateRule*>& out,
	int depth,
	std::vector<const TextMateRule*>& visitedIncludeOnly)
{
	if (!holder || depth > kMaxIncludeExpansionDepth) return;

	for (const auto& patternRef : holder->patterns) {
		const TextMateRule* resolved = nullptr;
		const Grammar* resolvedGrammar = &homeGrammar;

		if (const auto* ruleId = std::get_if<RuleId>(&patternRef)) {
			resolved = homeGrammar.Rule(*ruleId);
		} else if (const auto* include = std::get_if<IncludeReference>(&patternRef)) {
			if (include->kind == EIncludeKind::ExternalGrammar) {
				if (externalResolver) {
					if (const Grammar* externalGrammar = externalResolver->ResolveGrammar(include->primaryName)) {
						resolvedGrammar = externalGrammar;
						if (include->secondaryName.empty()) {
							resolved = externalGrammar->Rule(externalGrammar->rootRuleId);
						} else if (const auto it = externalGrammar->repository.find(include->secondaryName); it != externalGrammar->repository.end()) {
							resolved = externalGrammar->Rule(it->second);
						}
					}
				}
			} else {
				// Self / Base / RepositoryEntry all resolve within `homeGrammar`.
				resolved = ResolveInclude(homeGrammar, *include, externalResolver);
			}
		}
		if (!resolved) continue;

		if (resolved->kind == ERuleKind::IncludeOnly) {
			if (std::find(visitedIncludeOnly.begin(), visitedIncludeOnly.end(), resolved) != visitedIncludeOnly.end()) {
				continue; // cycle guard
			}
			visitedIncludeOnly.push_back(resolved);
			CollectMatchableRules(*resolvedGrammar, externalResolver, resolved, out, depth + 1, visitedIncludeOnly);
			continue;
		}
		out.push_back(resolved);
	}
}

//! Escapes `text` so it can be spliced into a regex pattern as a literal.
//! Mirrors vscode-textmate's own back-reference substitution, which inserts
//! the begin match's captured text as a literal rather than re-interpreting
//! it as a pattern fragment.
std::wstring EscapeForRegexLiteral(std::wstring_view text)
{
	std::wstring result;
	result.reserve(text.size() + 8);
	for (const wchar_t c : text) {
		switch (c) {
		case L'.': case L'^': case L'$': case L'*': case L'+': case L'?':
		case L'(': case L')': case L'[': case L']': case L'{': case L'}':
		case L'|': case L'\\': case L'/':
			result.push_back(L'\\');
			[[fallthrough]];
		default:
			result.push_back(c);
			break;
		}
	}
	return result;
}

[[nodiscard]] bool HasBackReference(std::wstring_view source) noexcept
{
	for (std::size_t i = 0; i + 1 < source.size(); ++i) {
		if (source[i] == L'\\' && source[i + 1] >= L'1' && source[i + 1] <= L'9') return true;
	}
	return false;
}

//! Replaces every `\1`..`\9` in `source` with the literal (regex-escaped)
//! text of the corresponding capture group from `beginMatch`, sliced out of
//! `beginLine` (the same line text the begin match was found in). A
//! back-reference to a group that did not participate, or that does not
//! exist, is left untouched — vscode-textmate's own behavior is likewise to
//! leave an unresolvable back-reference as literal source text rather than
//! failing the whole pattern.
std::wstring SubstituteBackReferences(std::wstring_view source, std::wstring_view beginLine, const OnigmoMatchResult& beginMatch)
{
	std::wstring result;
	result.reserve(source.size());
	for (std::size_t i = 0; i < source.size(); ++i) {
		if (source[i] == L'\\' && i + 1 < source.size() && source[i + 1] >= L'1' && source[i + 1] <= L'9') {
			const std::size_t groupIndex = static_cast<std::size_t>(source[i + 1] - L'0');
			if (groupIndex < beginMatch.groups.size() && beginMatch.groups[groupIndex].participated) {
				const auto& group = beginMatch.groups[groupIndex];
				result += EscapeForRegexLiteral(beginLine.substr(group.utf16Begin, group.utf16End - group.utf16Begin));
			} else {
				result.push_back(source[i]);
				result.push_back(source[i + 1]);
			}
			++i;
			continue;
		}
		result.push_back(source[i]);
	}
	return result;
}

//! Builds the scope path in effect for the innermost frame of `stack`,
//! outermost to innermost. `includeInnermostContentName` distinguishes
//! "inside the frame's content" (true: content between begin and end, where
//! `contentName` applies) from "at the frame's own begin/end/while boundary
//! text" (false: `name` applies but `contentName` does not yet/no longer).
std::vector<std::wstring> BuildScopePath(const RuleStackHandle& stack, bool includeInnermostContentName)
{
	std::vector<const TextMateRuleStackFrame*> frames;
	for (const TextMateRuleStackFrame* frame = stack.get(); frame; frame = frame->parent.get()) {
		frames.push_back(frame);
	}
	std::vector<std::wstring> path;
	path.reserve(frames.size() * 2);
	for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
		const TextMateRuleStackFrame* frame = *it;
		if (!frame->name.empty()) path.push_back(frame->name);
		const bool isInnermost = (it + 1 == frames.rend());
		if (isInnermost && includeInnermostContentName && !frame->contentName.empty()) {
			path.push_back(frame->contentName);
		}
	}
	return path;
}

//! Splits `[rangeBegin, rangeEnd)` into tokens honoring `captures`'
//! per-group scope names, correctly nesting overlapping capture ranges
//! (e.g. group 1 spanning group 2). Captures whose group did not participate,
//! whose `name` is empty, or that declare a nested `patterns` array
//! (`nestedPatternsRuleId`) contribute no extra scope for that sub-range;
//! nested-pattern re-tokenization inside a capture is not implemented (see
//! `TextMateRuleStackFrame`'s file-level documentation and
//! `textmate/CLAUDE.md` "Known gaps") — the range simply keeps `baseScopePath`.
void EmitCaptureTokens(
	std::size_t rangeBegin,
	std::size_t rangeEnd,
	const std::vector<CaptureRule>& captures,
	const OnigmoMatchResult& match,
	const std::vector<std::wstring>& baseScopePath,
	std::vector<TextMateToken>& outTokens)
{
	if (rangeBegin >= rangeEnd) return;

	struct Span final {
		std::size_t begin;
		std::size_t end;
		const std::wstring* name;
	};
	std::vector<Span> spans;
	for (const auto& capture : captures) {
		if (capture.groupIndex <= 0) continue; // group 0 == whole match == baseScopePath already
		if (capture.name.empty()) continue;
		if (static_cast<std::size_t>(capture.groupIndex) >= match.groups.size()) continue;
		const auto& group = match.groups[static_cast<std::size_t>(capture.groupIndex)];
		if (!group.participated || group.utf16Begin >= group.utf16End) continue;
		spans.push_back({group.utf16Begin, group.utf16End, &capture.name});
	}

	if (spans.empty()) {
		outTokens.push_back(TextMateToken{rangeBegin, rangeEnd, baseScopePath});
		return;
	}

	std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
		if (a.begin != b.begin) return a.begin < b.begin;
		return a.end > b.end; // wider (outer) span first when tied at the same start
	});

	std::vector<std::size_t> boundaries{rangeBegin, rangeEnd};
	for (const auto& span : spans) {
		boundaries.push_back(std::clamp(span.begin, rangeBegin, rangeEnd));
		boundaries.push_back(std::clamp(span.end, rangeBegin, rangeEnd));
	}
	std::sort(boundaries.begin(), boundaries.end());
	boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

	for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
		const std::size_t p = boundaries[i];
		const std::size_t q = boundaries[i + 1];
		if (p >= q) continue;
		std::vector<std::wstring> path = baseScopePath;
		for (const auto& span : spans) {
			if (span.begin <= p && span.end >= q) path.push_back(*span.name);
		}
		outTokens.push_back(TextMateToken{p, q, std::move(path)});
	}
}

} // namespace

RuleStackHandle TextMateTokenizer::InitialState() const
{
	auto root = std::make_shared<TextMateRuleStackFrame>();
	root->rule = RootRule();
	return root;
}

OnigmoPattern* TextMateTokenizer::GetOrCompileStaticPattern(const TextMateRule* rule) const
{
	if (auto it = m_staticPatternCache.find(rule); it != m_staticPatternCache.end()) {
		return it->second.get();
	}

	const std::wstring* source = nullptr;
	if (rule->kind == ERuleKind::Match) {
		source = &rule->matchSource;
	} else if (rule->kind == ERuleKind::BeginEnd || rule->kind == ERuleKind::BeginWhile) {
		source = &rule->beginSource;
	}

	std::unique_ptr<OnigmoPattern> compiled;
	if (source) {
		std::wstring diagnostic;
		compiled = OnigmoPattern::Compile(EncodeUtf8(*source), &diagnostic);
		// A pattern that fails to compile simply never matches (cached as
		// nullptr below); this keeps one malformed rule from aborting
		// tokenization of an otherwise-usable grammar.
	}
	OnigmoPattern* raw = compiled.get();
	m_staticPatternCache.emplace(rule, std::move(compiled));
	return raw;
}

TextMateLineTokenizeResult TextMateTokenizer::TokenizeLine(std::wstring_view line, const RuleStackHandle& previousState, RuleStackHandle& outNextState) const
{
	TextMateLineTokenizeResult result;
	RuleStackHandle stack = previousState ? previousState : InitialState();
	const Utf8LineBuffer encoded = EncodeLineForSearch(line);
	const std::size_t lineLength = line.size();
	std::size_t pos = 0;

	// `while` conditions are evaluated exactly once, at the start of the
	// line, from outermost to innermost frame; the first failure pops that
	// frame and everything nested inside it.
	{
		std::vector<const TextMateRuleStackFrame*> outermostToInnermost;
		for (const TextMateRuleStackFrame* frame = stack.get(); frame; frame = frame->parent.get()) {
			outermostToInnermost.push_back(frame);
		}
		std::reverse(outermostToInnermost.begin(), outermostToInnermost.end());

		for (const TextMateRuleStackFrame* frame : outermostToInnermost) {
			if (!frame->isWhileRule) continue;
			// `stack` may already have been truncated by an earlier failure
			// in this same loop; skip frames no longer present.
			bool stillPresent = false;
			for (const TextMateRuleStackFrame* f = stack.get(); f; f = f->parent.get()) {
				if (f == frame) { stillPresent = true; break; }
			}
			if (!stillPresent) continue;

			if (!frame->endOrWhilePattern) {
				// No usable while pattern (compile failure at push time):
				// treat as an immediate failure, same as a non-match.
				stack = frame->parent;
				continue;
			}
			auto matched = frame->endOrWhilePattern->Search(encoded, pos);
			if (!matched || matched->WholeMatch().utf16Begin != pos) {
				// Not anchored at the current position: treated as failure,
				// matching the common convention that `while` patterns are
				// written to apply right where the previous line left off.
				stack = frame->parent;
				continue;
			}
			const auto& whole = matched->WholeMatch();
			std::vector<std::wstring> boundaryScope = BuildScopePath(stack, false);
			EmitCaptureTokens(whole.utf16Begin, whole.utf16End, frame->rule->whileCaptures, *matched, boundaryScope, result.tokens);
			pos = whole.utf16End;
		}
	}

	std::size_t iterations = 0;
	const std::size_t maxIterations = (std::max<std::size_t>)(static_cast<std::size_t>(2000), lineLength * 20 + 100);

	while (pos <= lineLength) {
		if (++iterations > maxIterations) {
			// Pathological-grammar guard (e.g. a chain of zero-width
			// `IncludeOnly`/begin expansions): stop making progress
			// guarantees and flush the remainder as plain text rather than
			// hang the caller.
			if (pos < lineLength) {
				result.tokens.push_back(TextMateToken{pos, lineLength, BuildScopePath(stack, true)});
			}
			pos = lineLength;
			break;
		}

		const std::size_t positionAtLoopStart = pos;
		const TextMateRuleStackFrame* framePointerAtLoopStart = stack.get();
		const TextMateRule* frameRule = stack->rule;

		std::vector<Candidate> candidates;
		// A `while` frame's boundary pattern is intentionally excluded here:
		// `while` is only ever checked once, at the start of the line (the
		// block above), never as a per-position candidate the way `end` is.
		const bool hasBoundary = stack->endOrWhilePattern != nullptr && !stack->isWhileRule;
		if (hasBoundary && !frameRule->applyEndPatternLast) {
			candidates.push_back({nullptr, stack->endOrWhilePattern.get(), true});
		}
		{
			std::vector<const TextMateRule*> leafRules;
			std::vector<const TextMateRule*> visited;
			CollectMatchableRules(m_grammar, m_externalResolver, frameRule, leafRules, 0, visited);
			for (const TextMateRule* leaf : leafRules) {
				candidates.push_back({leaf, GetOrCompileStaticPattern(leaf), false});
			}
		}
		if (hasBoundary && frameRule->applyEndPatternLast) {
			candidates.push_back({nullptr, stack->endOrWhilePattern.get(), true});
		}

		const Candidate* best = nullptr;
		OnigmoMatchResult bestMatch;
		for (const auto& candidate : candidates) {
			if (!candidate.pattern) continue;
			auto matched = candidate.pattern->Search(encoded, pos);
			if (!matched) continue;
			if (!best || matched->WholeMatch().utf16Begin < bestMatch.WholeMatch().utf16Begin) {
				best = &candidate;
				bestMatch = std::move(*matched);
			}
		}

		if (!best) {
			// Nothing matches anywhere in the rest of the line: the whole
			// remainder is plain text under the current scope.
			if (pos < lineLength) {
				result.tokens.push_back(TextMateToken{pos, lineLength, BuildScopePath(stack, true)});
			}
			pos = lineLength;
			break;
		}

		const auto& whole = bestMatch.WholeMatch();
		if (whole.utf16Begin > pos) {
			result.tokens.push_back(TextMateToken{pos, whole.utf16Begin, BuildScopePath(stack, true)});
		}

		if (best->isFrameBoundary) {
			// Frame's own `end` closes it (a `while` frame's boundary is
			// never a same-line candidate; see the `hasBoundary` comment
			// above, so reaching here always means an `end` pattern).
			std::vector<std::wstring> boundaryScope = BuildScopePath(stack, false);
			EmitCaptureTokens(whole.utf16Begin, whole.utf16End, frameRule->endCaptures, bestMatch, boundaryScope, result.tokens);
			stack = stack->parent;
			pos = whole.utf16End;
		} else if (best->rule->kind == ERuleKind::Match) {
			std::vector<std::wstring> baseScope = BuildScopePath(stack, true);
			if (!best->rule->name.empty()) baseScope.push_back(best->rule->name);
			EmitCaptureTokens(whole.utf16Begin, whole.utf16End, best->rule->captures, bestMatch, baseScope, result.tokens);
			pos = whole.utf16End;
		} else {
			// BeginEnd / BeginWhile: emit the begin-match tokens, then push a
			// new frame whose end/while pattern is compiled fresh for this
			// specific instance (back-references resolved against this
			// match's captures).
			std::vector<std::wstring> baseScope = BuildScopePath(stack, true);
			if (!best->rule->name.empty()) baseScope.push_back(best->rule->name);
			EmitCaptureTokens(whole.utf16Begin, whole.utf16End, best->rule->beginCaptures, bestMatch, baseScope, result.tokens);

			auto frame = std::make_shared<TextMateRuleStackFrame>();
			frame->parent = stack;
			frame->rule = best->rule;
			frame->name = best->rule->name;
			frame->contentName = best->rule->contentName;
			frame->isWhileRule = (best->rule->kind == ERuleKind::BeginWhile);

			const std::wstring& boundarySource = frame->isWhileRule ? best->rule->whileSource : best->rule->endSource;
			if (!boundarySource.empty()) {
				const std::wstring substituted = HasBackReference(boundarySource) ? SubstituteBackReferences(boundarySource, line, bestMatch) : boundarySource;
				std::wstring diagnostic;
				frame->endOrWhilePattern = OnigmoPattern::Compile(EncodeUtf8(substituted), &diagnostic);
				// A compile failure here leaves endOrWhilePattern null; the
				// frame can then only be closed by an enclosing `while`
				// failing or by the document ending. See the field comment
				// on TextMateRuleStackFrame::endOrWhilePattern.
			}

			stack = frame;
			pos = whole.utf16End;
		}

		if (pos == positionAtLoopStart && stack.get() == framePointerAtLoopStart && pos < lineLength) {
			// Neither the position nor the frame changed: a zero-width
			// `Match` (or a zero-width boundary match that somehow left the
			// same frame pointer, which should not normally happen) would
			// otherwise match identically forever. A zero-width `begin`
			// push/`end` pop is deliberately exempt — the stack pointer
			// differs there — so legitimate lookahead-only begin/end rules
			// keep working; `maxIterations` above remains the backstop for
			// a pathological push/pop cycle that never consumes text.
			result.tokens.push_back(TextMateToken{pos, pos + 1, BuildScopePath(stack, true)});
			pos += 1;
		}
	}

	outNextState = stack;
	return result;
}

} // namespace textmate
