/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/TextMateGrammarCompiler.h"

#include <algorithm>
#include <cwctype>

namespace textmate {

namespace {

RuleId AllocateRule(Grammar& grammar, ERuleKind kind)
{
	auto rule = std::make_unique<TextMateRule>();
	const RuleId id = static_cast<RuleId>(grammar.rules.size());
	rule->id = id;
	rule->kind = kind;
	grammar.rules.push_back(std::move(rule));
	return id;
}

//! Parses one `include` string into its structured form. See
//! `EIncludeKind` for what each shape means.
IncludeReference ParseIncludeString(std::wstring_view include)
{
	IncludeReference reference;
	if (include == L"$self") {
		reference.kind = EIncludeKind::Self;
		return reference;
	}
	if (include == L"$base") {
		reference.kind = EIncludeKind::Base;
		return reference;
	}
	if (!include.empty() && include.front() == L'#') {
		reference.kind = EIncludeKind::RepositoryEntry;
		reference.primaryName = std::wstring(include.substr(1));
		return reference;
	}
	reference.kind = EIncludeKind::ExternalGrammar;
	const auto hash = include.find(L'#');
	if (hash == std::wstring_view::npos) {
		reference.primaryName = std::wstring(include);
	} else {
		reference.primaryName = std::wstring(include.substr(0, hash));
		reference.secondaryName = std::wstring(include.substr(hash + 1));
	}
	return reference;
}

class Compiler final {
public:
	explicit Compiler(Grammar& grammar) : m_grammar(grammar) {}

	//! Compiles a `captures` / `beginCaptures` / `endCaptures` /
	//! `whileCaptures` object. Keys are decimal capture-group numbers (`"0"` is
	//! the whole match); non-numeric keys are skipped with a diagnostic rather
	//! than aborting, since a stray key should not lose every other capture.
	std::vector<CaptureRule> CompileCaptures(const TextMateGrammarValue* capturesNode)
	{
		std::vector<CaptureRule> result;
		const auto* object = capturesNode ? capturesNode->AsObject() : nullptr;
		if (!object) return result;

		for (const auto& [key, value] : *object) {
			if (key.empty() || !std::all_of(key.begin(), key.end(), [](wchar_t c) { return std::iswdigit(c) != 0; })) {
				m_diagnostics.push_back({L"textmate: capture key is not a non-negative integer: " + key});
				continue;
			}
			CaptureRule capture;
			capture.groupIndex = std::stoi(key);
			if (const auto* name = value.Find(L"name"); name) {
				capture.name = name->AsStringOr(L"");
			}
			if (const auto* nestedPatterns = value.Find(L"patterns"); nestedPatterns) {
				const RuleId nestedId = AllocateRule(m_grammar, ERuleKind::IncludeOnly);
				m_grammar.rules[static_cast<std::size_t>(nestedId)]->patterns = CompilePatternsArray(nestedPatterns);
				capture.nestedPatternsRuleId = nestedId;
			}
			result.push_back(std::move(capture));
		}
		return result;
	}

	//! Compiles one `patterns` array into a list of already-resolved rules
	//! (`RuleId`) and deferred `include` references (`IncludeReference`).
	//! `disabled: true`/`1` entries are dropped entirely, matching how
	//! vscode-textmate treats VS Code's grammar-debugging affordance.
	std::vector<PatternRef> CompilePatternsArray(const TextMateGrammarValue* patternsNode)
	{
		std::vector<PatternRef> result;
		const auto* array = patternsNode ? patternsNode->AsArray() : nullptr;
		if (!array) return result;

		for (const auto& element : *array) {
			if (!element.IsObject()) {
				m_diagnostics.push_back({L"textmate: pattern array entry is not an object; skipped"});
				continue;
			}
			if (const auto* disabled = element.Find(L"disabled"); disabled && disabled->AsTruthy(false)) {
				continue;
			}
			if (const auto* include = element.Find(L"include"); include && include->IsString()) {
				result.emplace_back(ParseIncludeString(*include->AsString()));
				continue;
			}
			const RuleId ruleId = CompileRule(element);
			if (ruleId != kInvalidRuleId) {
				result.emplace_back(ruleId);
			}
		}
		return result;
	}

	//! Compiles one grammar rule object (a `patterns` element, or a
	//! `repository` member) into the arena. Returns `kInvalidRuleId` only for
	//! a `disabled` node, matching `CompilePatternsArray`'s contract; a
	//! `repository` entry that is `disabled` therefore also never gets a name
	//! in `Grammar::repository`.
	RuleId CompileRule(const TextMateGrammarValue& node)
	{
		if (const auto* disabled = node.Find(L"disabled"); disabled && disabled->AsTruthy(false)) {
			return kInvalidRuleId;
		}

		const auto* matchNode = node.Find(L"match");
		const auto* beginNode = node.Find(L"begin");
		const auto* whileNode = node.Find(L"while");
		const auto* capturesNode = node.Find(L"captures");
		const auto* beginCapturesNode = node.Find(L"beginCaptures");
		const auto* endCapturesNode = node.Find(L"endCaptures");
		const auto* whileCapturesNode = node.Find(L"whileCaptures");

		if (matchNode && matchNode->IsString()) {
			const RuleId id = AllocateRule(m_grammar, ERuleKind::Match);
			auto& rule = *m_grammar.rules[static_cast<std::size_t>(id)];
			rule.name = NameOf(node);
			rule.matchSource = *matchNode->AsString();
			rule.captures = CompileCaptures(capturesNode);
			return id;
		}

		if (beginNode && beginNode->IsString()) {
			const bool isWhile = whileNode && whileNode->IsString();
			const RuleId id = AllocateRule(m_grammar, isWhile ? ERuleKind::BeginWhile : ERuleKind::BeginEnd);
			auto& rule = *m_grammar.rules[static_cast<std::size_t>(id)];
			rule.name = NameOf(node);
			if (const auto* contentName = node.Find(L"contentName"); contentName) {
				rule.contentName = contentName->AsStringOr(L"");
			}
			rule.beginSource = *beginNode->AsString();
			//! `captures` is documented TextMate-grammar shorthand for
			//! supplying the same capture map to both sides of a begin/end (or
			//! begin/while) pair; an explicit `beginCaptures`/`endCaptures`/
			//! `whileCaptures` always takes precedence over it when present.
			rule.beginCaptures = CompileCaptures(beginCapturesNode ? beginCapturesNode : capturesNode);
			if (isWhile) {
				rule.whileSource = *whileNode->AsString();
				rule.whileCaptures = CompileCaptures(whileCapturesNode ? whileCapturesNode : capturesNode);
			} else if (const auto* endNode = node.Find(L"end"); endNode && endNode->IsString()) {
				rule.endSource = *endNode->AsString();
				rule.endCaptures = CompileCaptures(endCapturesNode ? endCapturesNode : capturesNode);
			} else {
				m_diagnostics.push_back({L"textmate: 'begin' present without 'end' or 'while'; treated as an unterminated rule that never matches its close"});
			}
			rule.applyEndPatternLast = [&] {
				const auto* value = node.Find(L"applyEndPatternLast");
				return value ? value->AsTruthy(false) : false;
			}();
			rule.patterns = CompilePatternsArray(node.Find(L"patterns"));
			return id;
		}

		// Neither `match` nor `begin`: an include-only holder (the common case
		// is `{ "patterns": [...] }`, e.g. an unnamed grouping element).
		const RuleId id = AllocateRule(m_grammar, ERuleKind::IncludeOnly);
		auto& rule = *m_grammar.rules[static_cast<std::size_t>(id)];
		rule.name = NameOf(node);
		rule.patterns = CompilePatternsArray(node.Find(L"patterns"));
		return id;
	}

	[[nodiscard]] std::vector<GrammarCompileDiagnostic>& Diagnostics() noexcept { return m_diagnostics; }

private:
	static std::wstring NameOf(const TextMateGrammarValue& node)
	{
		const auto* name = node.Find(L"name");
		return name ? name->AsStringOr(L"") : std::wstring();
	}

	Grammar& m_grammar;
	std::vector<GrammarCompileDiagnostic> m_diagnostics;
};

} // namespace

const TextMateRule* ResolveInclude(const Grammar& grammar, const IncludeReference& reference, IExternalGrammarResolver* externalResolver)
{
	switch (reference.kind) {
	case EIncludeKind::Self:
	case EIncludeKind::Base:
		// `$base` does not yet resolve to an injecting grammar (see
		// `EIncludeKind::Base`'s declaration comment); it falls back to
		// `$self` rather than failing, since that is the correct answer
		// whenever this grammar was not reached through an injection.
		return grammar.Rule(grammar.rootRuleId);

	case EIncludeKind::RepositoryEntry: {
		const auto it = grammar.repository.find(reference.primaryName);
		if (it == grammar.repository.end()) return nullptr;
		return grammar.Rule(it->second);
	}

	case EIncludeKind::ExternalGrammar: {
		if (!externalResolver) return nullptr;
		const Grammar* externalGrammar = externalResolver->ResolveGrammar(reference.primaryName);
		if (!externalGrammar) return nullptr;
		if (reference.secondaryName.empty()) {
			return externalGrammar->Rule(externalGrammar->rootRuleId);
		}
		const auto it = externalGrammar->repository.find(reference.secondaryName);
		if (it == externalGrammar->repository.end()) return nullptr;
		return externalGrammar->Rule(it->second);
	}
	}
	return nullptr;
}

GrammarCompileResult TextMateGrammarCompiler::Compile(const TextMateGrammarValue& root)
{
	GrammarCompileResult result;

	const auto* scopeName = root.Find(L"scopeName");
	if (!scopeName || !scopeName->IsString() || scopeName->AsString()->empty()) {
		result.diagnostics.push_back({L"textmate: grammar document has no non-empty 'scopeName'; compilation aborted"});
		return result;
	}

	auto grammar = std::make_unique<Grammar>();
	grammar->scopeName = *scopeName->AsString();

	if (const auto* fileTypes = root.Find(L"fileTypes"); fileTypes && fileTypes->IsArray()) {
		for (const auto& entry : *fileTypes->AsArray()) {
			if (const auto* text = entry.AsString()) {
				grammar->fileTypes.push_back(*text);
			}
		}
	}

	if (const auto* firstLineMatch = root.Find(L"firstLineMatch"); firstLineMatch && firstLineMatch->IsString()) {
		grammar->firstLineMatchSource = *firstLineMatch->AsString();
	}

	Compiler compiler(*grammar);

	//! Only root-level `repository` members are indexed; a `repository`
	//! nested inside a `begin`/`end`/`patterns` rule is a legal but rare
	//! TextMate shape that this compiler does not merge in. See
	//! `Grammar::repository`'s declaration comment and `textmate/CLAUDE.md`
	//! "Known gaps".
	if (const auto* repositoryNode = root.Find(L"repository"); repositoryNode && repositoryNode->IsObject()) {
		for (const auto& [name, value] : *repositoryNode->AsObject()) {
			const RuleId ruleId = compiler.CompileRule(value);
			if (ruleId != kInvalidRuleId) {
				grammar->repository[name] = ruleId;
			}
		}
	}

	grammar->rootRuleId = AllocateRule(*grammar, ERuleKind::IncludeOnly);
	grammar->rules[static_cast<std::size_t>(grammar->rootRuleId)]->patterns = compiler.CompilePatternsArray(root.Find(L"patterns"));

	result.diagnostics = std::move(compiler.Diagnostics());
	result.grammar = std::move(grammar);
	return result;
}

} // namespace textmate
