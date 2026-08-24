/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "textmate/OnigmoRegexEngine.h"
#include "textmate/TextMateGrammarCompiler.h"
#include "textmate/TextMateGrammarModel.h"

namespace textmate {

//! One rule pushed on the tokenizer's rule stack: a `BeginEnd`/`BeginWhile`
//! instance currently "open" (its `begin`/`while` matched, its `end`/`while`
//! has not yet closed it), or the synthetic root frame. Frames are linked
//! into a persistent (structurally shared) stack via `parent`, exactly like
//! vscode-textmate's own `StackElement`: carrying the innermost
//! `RuleStackHandle` forward to the next line *is* "carrying the end-of-line
//! rule stack forward", the core contract of a line-based TextMate
//! tokenizer.
//!
//! `endOrWhilePattern` is compiled once, at push time, from `rule`'s
//! `endSource`/`whileSource` with `\1`..`\9` back-references substituted
//! using the specific `begin` match's captured text (see
//! `SubstituteBackReferences` in `TextMateTokenizer.cpp`). This is why it
//! lives on the frame instead of being cached per-`TextMateRule` the way
//! `match`/`begin` patterns are: two instances of the same rule pushed with
//! different begin-captures (e.g. two differently-quoted heredocs) compile
//! two different end patterns.
struct TextMateRuleStackFrame final {
	std::shared_ptr<const TextMateRuleStackFrame> parent;
	//! Grammar that owns `rule`. RuleId values in the rule's nested patterns
	//! are only meaningful in this grammar's arena.
	const Grammar* owningGrammar = nullptr;
	//! Never null. The root frame's `rule` is `Grammar::rootRuleId`'s
	//! `IncludeOnly` rule.
	const TextMateRule* rule = nullptr;
	//! Resolved once at push time (`IncludeOnly`/`Match` rules never become a
	//! frame, so only a `BeginEnd`/`BeginWhile` rule's own `name`/
	//! `contentName` reach here; the root frame's are both empty).
	std::wstring name;
	std::wstring contentName;
	//! Null for the root frame, for a plain `Match`-only context, or when
	//! this instance's end/while pattern failed to compile (a documented
	//! degenerate case: the rule can then only be closed by an enclosing
	//! `while` condition failing, or by running out of document).
	std::unique_ptr<OnigmoPattern> endOrWhilePattern;
	bool isWhileRule = false;
};

using RuleStackHandle = std::shared_ptr<const TextMateRuleStackFrame>;

//! One contiguous, non-empty run of a tokenized line sharing one scope path.
//! `scopes` is ordered outermost to innermost, matching
//! `vscode-textmate`'s `IToken.scopes` (e.g. `["source.js", "string.quoted.double.js"]`).
struct TextMateToken final {
	std::size_t utf16Start = 0;
	std::size_t utf16End = 0;
	std::vector<std::wstring> scopes;
};

struct TextMateLineTokenizeResult final {
	std::vector<TextMateToken> tokens;
};

//! Tokenizes one `Grammar`'s worth of document lines, carrying a
//! `RuleStackHandle` across calls the way a real editor buffer would carry
//! it across lines: call `TokenizeLine` once per line, in order, top to
//! bottom, passing the previous call's `outNextState` back in as
//! `previousState` for the next. Re-tokenizing from a different starting
//! line requires either starting over from `InitialState()` or reusing a
//! `RuleStackHandle` captured after some earlier line -- there is no
//! "resynchronize mid-document" support here (VS Code's own incremental
//! re-tokenization on edit is a separate, unimplemented layer; see
//! `textmate/CLAUDE.md`).
//!
//! Not thread-safe: `match`/`begin` pattern compilation is cached lazily
//! per-rule on first use (`m_staticPatternCache`), so concurrent calls into
//! the same `TextMateTokenizer` instance race on that cache. Use one
//! instance per thread, or serialize calls, same as the rest of
//! `sakura_core`'s per-document services.
class TextMateTokenizer final {
public:
	explicit TextMateTokenizer(const Grammar& grammar, IExternalGrammarResolver* externalResolver = nullptr) noexcept
		: m_grammar(grammar), m_externalResolver(externalResolver)
	{
	}

	//! The state to pass as `previousState` for the first line of a document.
	[[nodiscard]] RuleStackHandle InitialState() const;

	//! Tokenizes `line` (a single line's text, with no line-ending
	//! characters -- matching how the rest of `sakura_core` already splits
	//! lines). `previousState` must be either `InitialState()` or a state
	//! this tokenizer previously returned; passing a null handle is
	//! equivalent to `InitialState()`.
	[[nodiscard]] TextMateLineTokenizeResult TokenizeLine(std::wstring_view line, const RuleStackHandle& previousState, RuleStackHandle& outNextState) const;

private:
	const TextMateRule* RootRule() const noexcept { return m_grammar.Rule(m_grammar.rootRuleId); }

	OnigmoPattern* GetOrCompileStaticPattern(const TextMateRule* rule) const;

	const Grammar& m_grammar;
	IExternalGrammarResolver* m_externalResolver = nullptr;
	//! Lazily-compiled `match`/`begin` patterns, keyed by rule identity. These
	//! never contain back-references to another rule's captures, so (unlike
	//! `end`/`while`) one compiled pattern per rule is correct for every use.
	mutable std::unordered_map<const TextMateRule*, std::unique_ptr<OnigmoPattern>> m_staticPatternCache;
};

} // namespace textmate
