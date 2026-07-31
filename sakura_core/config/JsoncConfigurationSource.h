/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/IConfigurationService.h"
#include "platform/serialization/JsoncDocument.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace config {

//! Stable diagnostic categories for a JSONC configuration document.  Parsing
//! is deliberately separate from persistence so a bad external edit cannot
//! replace the last accepted source model.
enum class EJsoncConfigurationDiagnosticCode : std::uint8_t {
	None,
	InputTooLarge,
	InvalidUtf8,
	UnexpectedToken,
	UnexpectedEndOfInput,
	InvalidEscape,
	InvalidNumber,
	DuplicateKey,
	RootMustBeObject,
	MaximumDepthExceeded,
	MaximumNodesExceeded,
	MaximumStringLengthExceeded,
	MaximumKeyLengthExceeded,
	InvalidLanguageOverride,
};

struct JsoncConfigurationDiagnostic final {
	EJsoncConfigurationDiagnosticCode code = EJsoncConfigurationDiagnosticCode::None;
	std::size_t byteOffset = 0;
	std::string message;
};

//! Revisions held by a file-backed configuration source after its last
//! accepted atomic replacement. Supplying this set enables CAS for each
//! independent language source and lets a later document explicitly clear
//! language overrides that it no longer contains. Omitting the whole set is
//! an unconditional replacement request.
struct JsoncConfigurationSourceRevisions final {
	std::uint64_t baseRevision = 0;
	std::map<std::wstring, std::uint64_t, std::less<>> languageRevisions;
};

//! A fully parsed source replacement set.  The first replacement always belongs
//! to the supplied source.  Each top-level "[language]" object is emitted as
//! a distinct LanguageOverride replacement with the same workspace/profile
//! identity, ready for an atomic service batch commit.
struct JsoncConfigurationParseResult final {
	std::vector<ConfigurationSourceReplacement> replacements;
	std::string operationId;
	std::optional<JsoncConfigurationDiagnostic> diagnostic;

	bool Succeeded() const noexcept { return !diagnostic.has_value(); }
};

//! Result of parsing and atomically applying one JSONC document.  A parser
//! diagnostic means that IConfigurationService was never called.
struct JsoncConfigurationApplyResult final {
	std::optional<JsoncConfigurationDiagnostic> diagnostic;
	ConfigurationBatchResult result;

	bool Parsed() const noexcept { return !diagnostic.has_value(); }
};

//! Bounded, side-effect-free JSONC reader for configuration persistence
//! adapters.  It intentionally has no file, window, shared-memory, or UI
//! dependency.  Callers must atomically submit `replacements` to the
//! configuration service only after Succeeded() is true.
class CJsoncConfigurationSource final {
public:
	static constexpr std::size_t kMaximumInputBytes = platform::serialization::CJsoncDocument::kMaximumInputBytes;
	static constexpr std::size_t kMaximumDepth = platform::serialization::CJsoncDocument::kMaximumDepth;
	static constexpr std::size_t kMaximumNodes = platform::serialization::CJsoncDocument::kMaximumNodes;
	static constexpr std::size_t kMaximumStringLength = platform::serialization::CJsoncDocument::kMaximumStringLength;
	static constexpr std::size_t kMaximumObjectKeyLength = platform::serialization::CJsoncDocument::kMaximumObjectKeyLength;

	//! Parses an UTF-8 JSONC configuration document into complete replacement
	//! models. Comments and trailing commas are accepted; duplicate JSON object
	//! keys are not. `revisions` supplies independent CAS values for the base
	//! and language sources; tracked language sources absent from this document
	//! become explicit empty replacements. `operationId` is retained on the
	//! result for the receiving batch API, which remains the sole CAS owner.
	static JsoncConfigurationParseResult Parse(
		std::string_view utf8,
		const ConfigurationSource& source,
		std::string operationId,
		std::optional<JsoncConfigurationSourceRevisions> revisions = std::nullopt
	);

	//! Adapts an already parsed JSONC object into the same complete replacement
	//! batch as Parse. This lets a .code-workspace `settings` member enter the
	//! configuration service atomically without serializing it or admitting any
	//! sibling workspace members as configuration keys.
	static JsoncConfigurationParseResult ParseObject(
		const platform::serialization::JsoncValue::Object& object,
		const ConfigurationSource& source,
		std::string operationId,
		std::optional<JsoncConfigurationSourceRevisions> revisions = std::nullopt
	);

	//! Calls ReplaceSources exactly once after a successful parse.  It never
	//! invokes the service for malformed, oversized, or otherwise invalid JSONC.
	static JsoncConfigurationApplyResult Apply(
		IConfigurationService& service,
		std::string_view utf8,
		const ConfigurationSource& source,
		std::string operationId,
		std::optional<JsoncConfigurationSourceRevisions> revisions = std::nullopt
	);

	//! Applies an extracted JSONC object using the same one-document atomic batch
	//! and independent source CAS semantics as Apply. Callers retain the returned
	//! ConfigurationBatchResult revisions for the next invocation.
	static JsoncConfigurationApplyResult ApplyObject(
		IConfigurationService& service,
		const platform::serialization::JsoncValue::Object& object,
		const ConfigurationSource& source,
		std::string operationId,
		std::optional<JsoncConfigurationSourceRevisions> revisions = std::nullopt
	);
};

} // namespace config
