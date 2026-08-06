/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <string_view>

#include "textmate/TextMateGrammarCompiler.h"

namespace textmate {

//! Loads a `.tmLanguage.json` (or `.json`) grammar document. Parsing goes
//! through the existing bounded `platform::serialization::JsoncDocument`
//! boundary (comments/trailing commas accepted, 1 MiB input cap, 64-deep
//! nesting cap, and so on — see `JsoncDocument.h`) rather than a new parser;
//! this loader only re-shapes the parsed `JsoncValue` tree into the
//! format-neutral `TextMateGrammarValue` tree `TextMateGrammarCompiler`
//! consumes, then compiles it.
class TextMateJsonGrammarLoader final {
public:
	TextMateJsonGrammarLoader() = delete;

	//! `utf8Source` is the raw file bytes (UTF-8, optionally BOM-prefixed —
	//! `JsoncDocument` handles the BOM). Returns a failed
	//! `GrammarCompileResult` (`grammar == nullptr`) both when the JSON
	//! itself is malformed/oversized and when the resulting document has no
	//! usable `scopeName`; the two cases are distinguished by
	//! `GrammarCompileResult::diagnostics`.
	[[nodiscard]] static GrammarCompileResult Load(std::string_view utf8Source);
};

} // namespace textmate
