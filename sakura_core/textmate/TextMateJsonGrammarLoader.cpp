/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/TextMateJsonGrammarLoader.h"

#include <utility>

#include "sakura/serialization/JsoncDocument.h"

namespace textmate {

namespace {

//! Converts one parsed `platform::serialization::JsoncValue` node into the
//! format-neutral `TextMateGrammarValue` tree `TextMateGrammarCompiler`
//! consumes.
//!
//! `Array` order is preserved exactly (both sides use an order-preserving
//! `std::vector`), which is what matters for `patterns` (first-match-wins).
//! `Object` order is *not* preserved: `JsoncValue::Object` is
//! `std::map<std::wstring, JsoncValue, std::less<>>` (key-sorted), so by the
//! time this function sees it, the JsoncDocument boundary has already
//! discarded the source document's textual member order. This loader cannot
//! recover it without bypassing the mandated `JsoncDocument` boundary, which
//! is not permitted; see `textmate/CLAUDE.md` "Known gaps". It has no
//! observed functional effect because grammar semantics never depend on
//! object member order — only `repository` *name* and `patterns` *array
//! position* are load-bearing, and both survive this conversion intact.
[[nodiscard]] TextMateGrammarValue ConvertJsoncValue(const platform::serialization::JsoncValue& node)
{
	using platform::serialization::JsoncValue;

	return std::visit(
		[](auto&& value) -> TextMateGrammarValue {
			using T = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				return TextMateGrammarValue(nullptr);
			} else if constexpr (std::is_same_v<T, bool>) {
				return TextMateGrammarValue(value);
			} else if constexpr (std::is_same_v<T, std::int64_t>) {
				return TextMateGrammarValue(value);
			} else if constexpr (std::is_same_v<T, double>) {
				return TextMateGrammarValue(value);
			} else if constexpr (std::is_same_v<T, std::wstring>) {
				return TextMateGrammarValue(value);
			} else if constexpr (std::is_same_v<T, JsoncValue::Array>) {
				TextMateGrammarValue::Array array;
				array.reserve(value.size());
				for (const auto& element : value) {
					array.push_back(ConvertJsoncValue(element));
				}
				return TextMateGrammarValue(std::move(array));
			} else if constexpr (std::is_same_v<T, JsoncValue::Object>) {
				TextMateGrammarValue::Object object;
				object.reserve(value.size());
				for (const auto& [key, element] : value) {
					object.emplace_back(key, ConvertJsoncValue(element));
				}
				return TextMateGrammarValue(std::move(object));
			} else {
				// Unreachable: Storage only ever holds the six alternatives above.
				return TextMateGrammarValue(nullptr);
			}
		},
		node.Value());
}

[[nodiscard]] std::wstring DescribeJsoncDiagnostic(const platform::serialization::JsoncDiagnostic& diagnostic)
{
	using platform::serialization::EJsoncDiagnosticCode;

	std::wstring code;
	switch (diagnostic.code) {
		case EJsoncDiagnosticCode::None: code = L"None"; break;
		case EJsoncDiagnosticCode::InputTooLarge: code = L"InputTooLarge"; break;
		case EJsoncDiagnosticCode::InvalidUtf8: code = L"InvalidUtf8"; break;
		case EJsoncDiagnosticCode::UnexpectedToken: code = L"UnexpectedToken"; break;
		case EJsoncDiagnosticCode::UnexpectedEndOfInput: code = L"UnexpectedEndOfInput"; break;
		case EJsoncDiagnosticCode::InvalidEscape: code = L"InvalidEscape"; break;
		case EJsoncDiagnosticCode::InvalidNumber: code = L"InvalidNumber"; break;
		case EJsoncDiagnosticCode::DuplicateKey: code = L"DuplicateKey"; break;
		case EJsoncDiagnosticCode::MaximumDepthExceeded: code = L"MaximumDepthExceeded"; break;
		case EJsoncDiagnosticCode::MaximumNodesExceeded: code = L"MaximumNodesExceeded"; break;
		case EJsoncDiagnosticCode::MaximumStringLengthExceeded: code = L"MaximumStringLengthExceeded"; break;
		case EJsoncDiagnosticCode::MaximumKeyLengthExceeded: code = L"MaximumKeyLengthExceeded"; break;
	}

	std::wstring result = L"JSON parse failed at byte offset ";
	result += std::to_wstring(diagnostic.byteOffset);
	result += L" (";
	result += code;
	result += L"): ";
	// `JsoncDiagnostic::message` is narrow (UTF-8/ASCII); DecodeUtf8ToWide would
	// pull in TextMateUtf8.h for a single diagnostic string, so convert inline
	// via the same MultiByteToWideChar path used there is unnecessary here:
	// diagnostic messages are ASCII by construction in JsoncDocument's
	// implementation, so a byte-for-byte widen is sufficient and avoids an
	// extra include for this one call site.
	result.append(diagnostic.message.begin(), diagnostic.message.end());
	return result;
}

} // namespace

GrammarCompileResult TextMateJsonGrammarLoader::Load(std::string_view utf8Source)
{
	const platform::serialization::JsoncDocumentParseResult parseResult = platform::serialization::CJsoncDocument::Parse(utf8Source);

	if (!parseResult.Succeeded()) {
		GrammarCompileResult result;
		GrammarCompileDiagnostic diagnostic;
		diagnostic.message = parseResult.diagnostic.has_value()
			? DescribeJsoncDiagnostic(*parseResult.diagnostic)
			: L"JSON parse failed with no diagnostic detail (unexpected: JsoncDocumentParseResult contract "
			  L"guarantees a diagnostic whenever value is absent).";
		result.diagnostics.push_back(std::move(diagnostic));
		return result;
	}

	const TextMateGrammarValue root = ConvertJsoncValue(*parseResult.value);
	return TextMateGrammarCompiler::Compile(root);
}

} // namespace textmate
