/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace textmate {

//! Index into `Grammar::rules`. Rules form a graph (repository entries can
//! reference each other, including cyclically), so they are addressed by a
//! stable small integer rather than owned by value, matching vscode-textmate's
//! own numeric `ruleId` scheme.
using RuleId = std::int32_t;
inline constexpr RuleId kInvalidRuleId = -1;

//! How an unresolved `include` value names its target. Resolution happens at
//! tokenize time (see `TextMateGrammarCompiler.h`), not at compile time, so a
//! forward reference to a repository entry declared later in the same
//! document — or a cyclic pair of repository entries — both work.
enum class EIncludeKind : std::uint8_t {
	//! `#name`: a member of the (root-level) `repository` object.
	RepositoryEntry,
	//! `$self`: the grammar's own top-level `patterns`.
	Self,
	//! `$base`: the top-level patterns of whichever grammar is actually being
	//! tokenized. Sakura NEXT does not yet support one grammar being injected
	//! as a base for another (VS Code's language-injection graph), so `$base`
	//! currently resolves identically to `$self`; this is a documented
	//! divergence, not silent wrong behavior; see `textmate/CLAUDE.md`.
	Base,
	//! `source.foo` or `source.foo#name`: a rule in a *different* grammar,
	//! addressed by scope name. Resolved through the injected
	//! `IExternalGrammarResolver`; see `TextMateGrammarCompiler.h`.
	ExternalGrammar,
};

struct IncludeReference final {
	EIncludeKind kind = EIncludeKind::Self;
	//! For `RepositoryEntry`: the name after `#`.
	//! For `ExternalGrammar`: the scope name before an optional `#`.
	std::wstring primaryName;
	//! For `ExternalGrammar` only: the name after `#`, when present.
	std::wstring secondaryName;
};

//! One element of a `patterns` array (or of `include`, which is a single
//! implicit pattern). Either a rule that has already been compiled in this
//! document (`RuleId`), or a reference that must be resolved later.
using PatternRef = std::variant<RuleId, IncludeReference>;

//! One entry of a `captures` / `beginCaptures` / `endCaptures` /
//! `whileCaptures` map, keyed by capture-group number (0 = whole match).
struct CaptureRule final {
	int groupIndex = 0;
	std::wstring name;
	//! Set when the capture itself has a nested `patterns` array (recursive
	//! tokenization of the captured substring). `kInvalidRuleId` when the
	//! capture is a plain `{ "name": "..." }` entry.
	RuleId nestedPatternsRuleId = kInvalidRuleId;
};

enum class ERuleKind : std::uint8_t {
	//! A leaf `match` rule: matches a single regex, applies `name` to the
	//! whole match, and applies `captures` to the numbered sub-groups.
	Match,
	//! A `begin`/`end` pair. `name` applies to the whole begin..end span;
	//! `contentName` applies to the span strictly between begin and end.
	//! `patterns` tokenizes the content between begin and end.
	BeginEnd,
	//! A `begin`/`while` pair. Like `BeginEnd`, except every content line
	//! must additionally match `while` to remain inside the rule; the first
	//! line that fails to match `while` ends the rule *before* that line is
	//! otherwise tokenized by it.
	BeginWhile,
	//! A rule that exists only to hold a `patterns` array: the synthetic root
	//! rule for a grammar's top-level `patterns`, or any encountered
	//! `{ "patterns": [...] }` object that has neither `match` nor `begin`.
	IncludeOnly,
};

//! One compiled TextMate rule. Uncompiled regex *source* strings are kept
//! here rather than compiled `OnigmoPattern` objects: `end`/`while` sources
//! can contain back-references (`\1`..`\9`) into the `begin` match of the
//! specific instance that pushed them, so the same rule compiles a different
//! `end`/`while` regex on every push when `endHasBackReferences` is true (see
//! `TextMateTokenizer.cpp`). Regex compilation is therefore owned by the
//! tokenizer's per-line matching, not by this static model.
struct TextMateRule final {
	RuleId id = kInvalidRuleId;
	ERuleKind kind = ERuleKind::IncludeOnly;

	//! Scope pushed for the rule's own extent (whole match, or begin..end).
	std::wstring name;
	//! `BeginEnd`/`BeginWhile` only: scope pushed strictly inside the span.
	std::wstring contentName;

	//! `Match` only.
	std::wstring matchSource;
	std::vector<CaptureRule> captures;

	//! `BeginEnd`/`BeginWhile` only.
	std::wstring beginSource;
	std::vector<CaptureRule> beginCaptures;
	//! `BeginEnd` only.
	std::wstring endSource;
	std::vector<CaptureRule> endCaptures;
	//! `BeginWhile` only.
	std::wstring whileSource;
	std::vector<CaptureRule> whileCaptures;
	//! When true (vscode-textmate's default is false), the rule's own `end`
	//! pattern is evaluated *after* nested `patterns` rather than before, so a
	//! pattern that would also match at the end position wins over closing
	//! the rule.
	bool applyEndPatternLast = false;

	//! `IncludeOnly`/`BeginEnd`/`BeginWhile`: nested pattern list. Empty for
	//! `Match` and for a `BeginEnd`/`BeginWhile` rule with no explicit
	//! `patterns` (the span's content then tokenizes as plain text).
	std::vector<PatternRef> patterns;

	//! Skipped `disabled: true`/`disabled: 1` sources compile to no rule at
	//! all rather than to a "disabled" flag, matching how vscode-textmate
	//! treats VS Code's grammar debugging affordance.
};

//! One grammar document, compiled from either JSON or plist source through
//! `TextMateGrammarCompiler`. Immutable once built; tokenization (see
//! `TextMateTokenizer.h`) only reads it.
struct Grammar final {
	std::wstring scopeName;
	std::vector<std::wstring> fileTypes;
	//! Present only when the source declared `firstLineMatch`; VS Code uses
	//! it to guess a grammar for an extensionless file. Sakura NEXT does not
	//! yet call into this (language *selection* is a separate, pre-existing
	//! subsystem — see `sakura_core/types/`); the field is retained so a
	//! future integration does not need to re-parse the source.
	std::optional<std::wstring> firstLineMatchSource;

	//! Rule arena. `RuleId` is an index into this vector; `rules[i]->id == i`
	//! always holds.
	std::vector<std::unique_ptr<TextMateRule>> rules;
	//! Synthetic `IncludeOnly` rule holding the document's top-level
	//! `patterns`; `$self` and `$base` (see `EIncludeKind::Base`) both
	//! resolve here.
	RuleId rootRuleId = kInvalidRuleId;
	//! Root-level `repository` members only. A `repository` object nested
	//! inside a `begin`/`end`/`patterns` rule (legal but rare in real-world
	//! grammars) is intentionally not merged in; see `textmate/CLAUDE.md`
	//! "Known gaps".
	std::unordered_map<std::wstring, RuleId> repository;

	[[nodiscard]] const TextMateRule* Rule(RuleId id) const noexcept
	{
		if (id < 0 || static_cast<std::size_t>(id) >= rules.size()) return nullptr;
		return rules[static_cast<std::size_t>(id)].get();
	}
};

} // namespace textmate
