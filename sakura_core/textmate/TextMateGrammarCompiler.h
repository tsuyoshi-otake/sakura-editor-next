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
#include <vector>

#include "textmate/TextMateGrammarModel.h"
#include "textmate/TextMateGrammarValue.h"

namespace textmate {

//! Looks up a second grammar by TextMate scope name (`source.python`,
//! `text.html.basic`, ...), so `include: "source.foo"` /
//! `include: "source.foo#bar"` can cross into it. Sakura NEXT's extension
//! host owns the set of installed grammars; this interface lets
//! `sakura_core/textmate` stay ignorant of how grammars are discovered or
//! cached. `senp::ISenpLanguageService` supplies the enabled installed grammar
//! set in production. Tests and standalone callers may pass `nullptr`, which
//! makes every external include resolve to no patterns (fail closed).
class IExternalGrammarResolver {
public:
	virtual ~IExternalGrammarResolver() = default;

	//! Returns the compiled grammar registered for `scopeName`, or `nullptr`
	//! when no such grammar is known. The returned pointer must stay valid for
	//! at least the lifetime of the tokenization call that requested it.
	virtual const Grammar* ResolveGrammar(std::wstring_view scopeName) = 0;
};

//! Resolves one `IncludeReference` (already parsed out of an `include:`
//! string by the compiler) against `grammar`, following into
//! `externalResolver` for `source.foo`/`source.foo#name` references.
//!
//! This is a plain function rather than a `Grammar` member because
//! resolution is intentionally *not* cached at compile time: `repository`
//! entries can reference each other cyclically (`#a` includes `#b` includes
//! `#a`), and VS Code resolves every `include` fresh at the point it is
//! expanded during tokenization. Returns `nullptr` when the reference cannot
//! be resolved (unknown repository name, unknown external scope, or unknown
//! member of an external grammar's repository) -- an unresolved include
//! contributes zero patterns rather than aborting tokenization.
const TextMateRule* ResolveInclude(
	const Grammar& grammar,
	const IncludeReference& reference,
	IExternalGrammarResolver* externalResolver);

//! One non-fatal problem found while compiling a grammar document, e.g. an
//! `include` string that could not be parsed, or a `repository` entry with an
//! unrecognized shape. Compilation continues past these; only a handful of
//! structural problems (see `GrammarCompileResult::Succeeded`) are fatal.
struct GrammarCompileDiagnostic final {
	std::wstring message;
};

struct GrammarCompileResult final {
	std::unique_ptr<Grammar> grammar;
	std::vector<GrammarCompileDiagnostic> diagnostics;

	[[nodiscard]] bool Succeeded() const noexcept { return grammar != nullptr; }
};

//! Compiles a format-neutral `TextMateGrammarValue` parse tree (produced by
//! either `TextMateJsonGrammarLoader` or `TextMatePlistGrammarLoader`) into a
//! `Grammar`. This is the single place that understands the TextMate grammar
//! *shape* (`patterns`/`repository`/`include`/`begin`-`end`/`captures`/...);
//! neither loader duplicates any of this logic.
class TextMateGrammarCompiler final {
public:
	TextMateGrammarCompiler() = delete;

	//! `root` must be the document's top-level object (what a `.tmLanguage.json`
	//! file or a plist's top-level `<dict>` decodes to). Fails (returns a result
	//! with `grammar == nullptr`) only when the document has no usable
	//! `scopeName`; every other malformed/missing field degrades to an empty or
	//! best-effort value plus a diagnostic, matching vscode-textmate's tolerance
	//! for slightly-malformed real-world grammars.
	[[nodiscard]] static GrammarCompileResult Compile(const TextMateGrammarValue& root);
};

} // namespace textmate
