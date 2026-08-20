/*! @file
 * @brief Pure VS Code problemMatcher-shaped adapter from task output text to MarkerService requests.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/problems/MarkerService.h"

#include <sakura/uri/UriIdentity.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::tasks {

//! VS Code's `fileLocation` values this adapter can resolve without touching the filesystem.
//! `autoDetect` and `search` require probing candidate paths against disk, which this pure
//! model deliberately cannot do; a definition that names them is a typed, terminal failure
//! rather than a silent fallback to `Relative`.
enum class EProblemMatcherFileLocation : std::uint8_t {
	Absolute,
	Relative,
};

//! One entry of a VS Code `pattern` array. A single-pattern matcher is VS Code's "line" kind;
//! more than one pattern is the "multiline" kind. Group indices are 1-based, matching
//! VS Code's regex capture-group numbering (group 0, the whole match, is never a field source).
struct ProblemMatcherPattern final {
	//! ECMAScript-syntax regular expression source, matched against one output line at a time.
	std::wstring regexp;
	std::optional<int> file;
	std::optional<int> line;
	std::optional<int> column;
	std::optional<int> endLine;
	std::optional<int> endColumn;
	std::optional<int> severity;
	std::optional<int> code;
	std::optional<int> message;
	//! Only meaningful on the last pattern of a multiline chain: after a complete match, resume
	//! matching at this same (last) pattern instead of resetting to the first, so repeated
	//! detail lines under one unchanged file/line header keep producing more problems.
	bool loop = false;

	[[nodiscard]] bool operator==(const ProblemMatcherPattern&) const noexcept = default;
};

//! A copied value matcher definition: VS Code's `ProblemMatcher` shape, minus `background` and
//! `applyTo`, which belong to a future task-lifecycle adapter rather than this pure text-to-marker
//! translation.
struct ProblemMatcherDefinition final {
	//! `owner` from the VS Code matcher JSON, carried for parity with the upstream shape. The
	//! produced `MarkerCollectionIdentity` always comes from `ProblemMatcherRunContext::owner`
	//! and `collectionId` instead: only the caller knows the live `MarkerOwner` generation this
	//! run must be fenced against, so this field is descriptive metadata, not marker identity.
	std::wstring owner;
	//! `source`, copied onto every produced `ProblemMarker`.
	std::optional<std::wstring> source;
	problems::EMarkerSeverity defaultSeverity = problems::EMarkerSeverity::Error;
	EProblemMatcherFileLocation fileLocation = EProblemMatcherFileLocation::Relative;
	//! One entry for a "line" matcher; two or more, in match order, for a "multiline" matcher.
	std::vector<ProblemMatcherPattern> patterns;

	[[nodiscard]] bool operator==(const ProblemMatcherDefinition&) const noexcept = default;
};

enum class EProblemMatcherLookupStatus : std::uint8_t {
	Found,
	UnknownName,
};

struct ProblemMatcherLookupResult final {
	EProblemMatcherLookupStatus status = EProblemMatcherLookupStatus::UnknownName;
	std::optional<ProblemMatcherDefinition> definition;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EProblemMatcherLookupStatus::Found; }
};

//! Read-only catalog of VS Code's built-in `$name` problem matchers. Pure, static, no I/O.
class BuiltinProblemMatchers final {
public:
	//! `name` is matched exactly, including the leading `$` (e.g. `L"$msCompile"`).
	[[nodiscard]] static ProblemMatcherLookupResult Resolve(std::wstring_view name);
};

enum class EProblemMatchStatus : std::uint8_t {
	Ok,
	//! `patterns` is empty.
	EmptyPatternList,
	//! `patterns.size()` exceeds `Limits::maximumPatterns`.
	MaximumPatternsExceeded,
	//! A pattern's `regexp` failed to compile as an ECMAScript regular expression.
	InvalidRegexp,
	//! A field references a capture group the compiled regex does not have (group 0 is never a
	//! valid field source; every field index must be within `1..=mark_count()`).
	InvalidGroupIndex,
	//! No pattern in the chain declares `file`, `line`, or `message`, so a completed chain could
	//! never produce a well-formed marker. Checked once against the definition, never guessed
	//! per-match by silently dropping an incomplete result.
	MissingRequiredCaptureField,
	//! `fileLocation == Relative` but the caller supplied no workspace root to resolve against.
	MissingWorkspaceRoot,
	//! The captured file text did not resolve to a well-formed file URI.
	InvalidFileLocation,
	//! A `severity` capture group matched text that is not one of VS Code's recognized severity
	//! spellings (`error`, `warning`/`warn`, `info`/`information`). Never guessed as a default.
	InvalidSeverityValue,
	//! A `line`/`column`/`endLine`/`endColumn` capture did not parse as a positive integer.
	InvalidLineOrColumnValue,
	//! An output line was not well-formed UTF-8.
	InvalidUtf8Line,
	//! `lines.size()` exceeds `Limits::maximumLines`.
	MaximumLinesExceeded,
	//! One line exceeded `Limits::maximumLineLength` UTF-8 bytes.
	MaximumLineLengthExceeded,
	//! The total number of produced markers exceeded `Limits::maximumMarkersPerRun`.
	MaximumMarkersExceeded,
};

//! Bounded limits for one `ProcessOutputLines` call. Defaults mirror `MarkerService`'s own
//! per-resource/per-payload discipline so a pathological task cannot grow this adapter's working
//! set without a caller opting in.
struct ProblemMatcherEngineLimits final {
	std::size_t maximumLines = 20'000U;
	std::size_t maximumLineLength = 8'192U;
	std::size_t maximumMarkersPerRun = 4'096U;
	std::size_t maximumPatterns = 16U;
};

//! Caller-owned context this pure engine never derives for itself: marker identity and the
//! workspace root used to resolve a `Relative` `fileLocation`. No file I/O is performed to
//! validate `workspaceRoot`; it is trusted to already be an absolute, existing folder.
struct ProblemMatcherRunContext final {
	problems::MarkerOwner owner;
	//! Matches `MarkerCollectionIdentity::id` (a stable narrow-string collection id, e.g. the task label).
	std::string collectionId;
	std::optional<platform::uri::Uri> workspaceRoot;
};

//! One resource's produced problems, ready to hand to `MarkerService::Replace` unchanged.
struct ProblemMatchOutcome final {
	EProblemMatchStatus status = EProblemMatchStatus::EmptyPatternList;
	//! Set only for a per-line failure status (`InvalidUtf8Line`, `InvalidLineOrColumnValue`,
	//! `InvalidSeverityValue`, `InvalidFileLocation`, `MaximumLineLengthExceeded`); the index into
	//! the input `lines` that triggered it.
	std::optional<std::size_t> failingLineIndex;
	//! One entry per distinct resolved resource, ordered by resource comparison key for
	//! deterministic replay. Empty when `status != Ok` or when no line matched.
	std::vector<problems::ReplaceMarkersRequest> replacements;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EProblemMatchStatus::Ok; }
};

/*!
	@brief Stateless translator from raw task output lines to bounded `MarkerService` requests.

	`ProcessOutputLines` never touches a filesystem, process, HWND, timer, or thread: the caller
	supplies already-captured output lines and an already-resolved workspace root, and receives
	back value requests it can pass to `MarkerService::Replace` directly. An unsupported matcher
	shape (an unresolvable `$name`, an unsupported `fileLocation`, an out-of-range capture group, a
	non-numeric line/column, an unrecognized severity spelling, malformed UTF-8) is always a typed
	terminal failure; this class never guesses a default to keep processing going.
*/
class ProblemMatcherEngine final {
public:
	//! `lines` are raw task output lines, one array entry per line, encoded as UTF-8 bytes without
	//! a trailing line terminator. Processing stops at the first typed failure; earlier lines'
	//! completed matches are discarded rather than partially applied, matching this repository's
	//! "leave the last accepted state unchanged" convention for adapters feeding a pure model.
	[[nodiscard]] static ProblemMatchOutcome ProcessOutputLines(
		const ProblemMatcherDefinition& definition,
		const std::vector<std::string>& lines,
		const ProblemMatcherRunContext& context,
		const ProblemMatcherEngineLimits& limits = {});
};

} // namespace workbench::tasks
