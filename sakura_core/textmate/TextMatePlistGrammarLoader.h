/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "textmate/TextMateGrammarCompiler.h"

namespace textmate {

//! Loads a classic `.tmLanguage` grammar: an Apple property list serialized as
//! XML (`<plist><dict>...</dict></plist>`).
//!
//! There is no existing bounded XML/plist boundary in `sakura_core` to route
//! through (unlike JSON, which has `platform::serialization::JsoncDocument`),
//! so this loader contains its own minimal, purpose-built recursive-descent
//! XML reader. It is deliberately narrow: it understands only the handful of
//! plist element types real `.tmLanguage` grammars use
//! (`dict`/`array`/`key`/`string`/`integer`/`real`/`true`/`false`, plus
//! `data`/`date` as a best-effort string fallback — see the .cpp) and is not
//! a general-purpose XML parser.
//!
//! XXE safety: `<!DOCTYPE ...>` is recognized and skipped as opaque text
//! (including its bracketed internal subset), but its contents are never
//! interpreted — no `<!ENTITY>` declaration, internal or external, is ever
//! registered or expanded, so a crafted `<!ENTITY xxe SYSTEM "file:///...">`
//! cannot exfiltrate anything: `&xxe;` in element content decodes as an
//! *unknown entity reference* and fails the parse (see `EPlistDiagnosticCode`
//! below), the same as any other unrecognized `&name;`. Only the five
//! predefined XML entities (`&amp; &lt; &gt; &quot; &apos;`) and numeric
//! character references (`&#NN;` / `&#xHH;`) are ever decoded. No network or
//! filesystem access is performed while parsing.
enum class EPlistDiagnosticCode {
	None,
	InputTooLarge,
	InvalidUtf8,
	MalformedXml,
	UnknownEntityReference,
	MaximumDepthExceeded,
	MaximumNodesExceeded,
	UnsupportedRootShape,
};

struct PlistParseDiagnostic final {
	EPlistDiagnosticCode code = EPlistDiagnosticCode::None;
	std::size_t byteOffset = 0;
	std::wstring message;
};

class TextMatePlistGrammarLoader final {
public:
	TextMatePlistGrammarLoader() = delete;

	//! `utf8Source` is the raw file bytes, always interpreted as UTF-8. A
	//! leading `<?xml ...?>` processing instruction is recognized and skipped
	//! as opaque text (like any other `<? ... ?>`), but its `encoding="..."`
	//! attribute, if present, is never read — this loader does not transcode;
	//! it validates that the whole input is well-formed UTF-8 up front and
	//! fails with `EPlistDiagnosticCode::InvalidUtf8` otherwise. Real
	//! `.tmLanguage` files are UTF-8 in practice, so this has not been a
	//! limitation seen in the wild, but a grammar file genuinely encoded as
	//! e.g. UTF-16 or Latin-1 will be rejected rather than transcoded.
	//! Returns a failed `GrammarCompileResult` both when the XML/plist shape
	//! itself is malformed and when the resulting document has no usable
	//! `scopeName`; call `Parse` first if the caller needs to distinguish
	//! "not valid plist XML at all" from "valid plist, invalid grammar" more
	//! precisely than `GrammarCompileResult::diagnostics` conveys.
	[[nodiscard]] static GrammarCompileResult Load(std::string_view utf8Source);

	//! Parses `utf8Source` as plist XML only, without compiling it into a
	//! `Grammar`. Exposed separately so callers/tests can inspect the raw
	//! `TextMateGrammarValue` tree or a precise `PlistParseDiagnostic`
	//! independent of grammar-compilation diagnostics.
	struct ParseResult final {
		std::optional<TextMateGrammarValue> value;
		std::optional<PlistParseDiagnostic> diagnostic;

		[[nodiscard]] bool Succeeded() const noexcept { return value.has_value() && !diagnostic.has_value(); }
	};
	[[nodiscard]] static ParseResult Parse(std::string_view utf8Source);
};

} // namespace textmate
