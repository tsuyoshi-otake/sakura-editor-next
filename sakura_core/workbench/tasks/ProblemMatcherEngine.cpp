/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/tasks/ProblemMatcherEngine.h"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <map>
#include <regex>
#include <utility>

namespace workbench::tasks {
namespace {

//! Strict UTF-8 -> UTF-16 decode. Unlike a best-effort diagnostic decoder, malformed input is a
//! hard failure: task output is untrusted external text and this adapter must never substitute a
//! replacement character and keep matching against corrupted content.
bool TryDecodeStrictUtf8(const std::string_view value, std::wstring& out)
{
	out.clear();
	out.reserve(value.size());
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			out.push_back(static_cast<wchar_t>(first));
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) {
			continuationCount = 1;
			codePoint = first & 0x1fU;
		} else if (first >= 0xe0 && first <= 0xef) {
			continuationCount = 2;
			codePoint = first & 0x0fU;
		} else if (first >= 0xf0 && first <= 0xf4) {
			continuationCount = 3;
			codePoint = first & 0x07U;
		} else {
			return false;
		}
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0U) != 0x80U) return false;
			codePoint = (codePoint << 6) | (next & 0x3fU);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) return false;
		if (codePoint > 0xffffU) {
			const auto adjusted = codePoint - 0x10000U;
			out.push_back(static_cast<wchar_t>(0xd800U + (adjusted >> 10)));
			out.push_back(static_cast<wchar_t>(0xdc00U + (adjusted & 0x3ffU)));
		} else {
			out.push_back(static_cast<wchar_t>(codePoint));
		}
		index += continuationCount + 1;
	}
	return true;
}

//! `ProblemMarker::message`/`code`/`source` are UTF-8 `std::string`; captured text is decoded
//! UTF-16 already known to be well-formed (it came from `TryDecodeStrictUtf8`), so this encode
//! side cannot itself fail on well-formed input.
std::string EncodeUtf8(const std::wstring_view wide)
{
	std::string out;
	out.reserve(wide.size());
	for (std::size_t index = 0; index < wide.size(); ++index) {
		std::uint32_t codePoint = wide[index];
		if (codePoint >= 0xd800U && codePoint <= 0xdbffU && index + 1 < wide.size()) {
			const std::uint32_t low = wide[index + 1];
			if (low >= 0xdc00U && low <= 0xdfffU) {
				codePoint = 0x10000U + ((codePoint - 0xd800U) << 10) + (low - 0xdc00U);
				++index;
			}
		}
		if (codePoint < 0x80U) {
			out.push_back(static_cast<char>(codePoint));
		} else if (codePoint < 0x800U) {
			out.push_back(static_cast<char>(0xc0U | (codePoint >> 6)));
			out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else if (codePoint < 0x10000U) {
			out.push_back(static_cast<char>(0xe0U | (codePoint >> 12)));
			out.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3fU)));
			out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		} else {
			out.push_back(static_cast<char>(0xf0U | (codePoint >> 18)));
			out.push_back(static_cast<char>(0x80U | ((codePoint >> 12) & 0x3fU)));
			out.push_back(static_cast<char>(0x80U | ((codePoint >> 6) & 0x3fU)));
			out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
		}
	}
	return out;
}

//! Requires every character to be an ASCII digit; a captured line/column with surrounding text,
//! a sign, or non-ASCII digits is a typed failure rather than a best-effort `wcstol` parse.
std::optional<std::uint32_t> ParsePositiveInteger(const std::wstring_view text)
{
	if (text.empty() || text.size() > 10) return std::nullopt;
	std::string narrow;
	narrow.reserve(text.size());
	for (const wchar_t character : text) {
		if (character < L'0' || character > L'9') return std::nullopt;
		narrow.push_back(static_cast<char>(character));
	}
	std::uint64_t value{};
	const auto result = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
	if (result.ec != std::errc{} || result.ptr != narrow.data() + narrow.size()) return std::nullopt;
	if (value == 0 || value > 0xffffffffU) return std::nullopt;
	return static_cast<std::uint32_t>(value);
}

std::optional<problems::EMarkerSeverity> ParseSeverityWord(const std::wstring_view text)
{
	std::wstring lowered(text);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](const wchar_t character) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character))); });
	if (lowered == L"error") return problems::EMarkerSeverity::Error;
	if (lowered == L"warning" || lowered == L"warn") return problems::EMarkerSeverity::Warning;
	if (lowered == L"info" || lowered == L"information") return problems::EMarkerSeverity::Information;
	if (lowered == L"hint") return problems::EMarkerSeverity::Hint;
	return std::nullopt;
}

//! Joins a matcher-captured, possibly-relative file path onto the workspace root without any
//! filesystem access: pure string normalization followed by `Uri::FromWindowsPath`.
std::optional<platform::uri::Uri> ResolveAbsoluteFile(
	const EProblemMatcherFileLocation fileLocation, const std::wstring& captured,
	const std::optional<platform::uri::Uri>& workspaceRoot)
{
	if (fileLocation == EProblemMatcherFileLocation::Absolute) {
		auto parsed = platform::uri::Uri::FromWindowsPath(captured);
		return parsed ? std::optional<platform::uri::Uri>(std::move(*parsed.value)) : std::nullopt;
	}

	if (!workspaceRoot) return std::nullopt;
	auto rootPath = workspaceRoot->ToWindowsPath();
	if (!rootPath) return std::nullopt;
	std::wstring joined = *rootPath.value;
	while (!joined.empty() && (joined.back() == L'\\' || joined.back() == L'/')) joined.pop_back();
	std::wstring relative(captured);
	std::replace(relative.begin(), relative.end(), L'/', L'\\');
	while (!relative.empty() && relative.front() == L'\\') relative.erase(relative.begin());
	if (relative.empty()) return std::nullopt;
	joined.push_back(L'\\');
	joined += relative;
	auto parsed = platform::uri::Uri::FromWindowsPath(joined);
	return parsed ? std::optional<platform::uri::Uri>(std::move(*parsed.value)) : std::nullopt;
}

//! Field values accumulated while walking a multiline pattern chain. Each successfully matched
//! pattern instance overwrites only the fields it actually captures, so an earlier pattern's
//! `file`/`line` survives into a later pattern that only captures `severity`/`code`/`message`.
struct AccumulatedFields final {
	std::optional<platform::uri::Uri> fileUri;
	std::optional<std::uint32_t> line;
	std::optional<std::uint32_t> column;
	std::optional<std::uint32_t> endLine;
	std::optional<std::uint32_t> endColumn;
	std::optional<problems::EMarkerSeverity> severity;
	std::optional<std::string> code;
	std::optional<std::string> message;
};

struct CompiledPattern final {
	const ProblemMatcherPattern* definition{};
	std::wregex regex;
};

} // namespace

ProblemMatcherLookupResult BuiltinProblemMatchers::Resolve(const std::wstring_view name)
{
	ProblemMatcherDefinition definition;
	definition.defaultSeverity = problems::EMarkerSeverity::Error;
	definition.fileLocation = EProblemMatcherFileLocation::Absolute;

	if (name == L"$msCompile") {
		definition.owner = L"msCompile";
		definition.source = L"msCompile";
		ProblemMatcherPattern pattern;
		pattern.regexp =
			LR"(^(?:\d+>)?(.*?)\((\d+)(?:,(\d+))?\)\s*:\s*(error|warning|info)\s+([A-Za-z]+\d+)\s*:\s*(.*)$)";
		pattern.file = 1;
		pattern.line = 2;
		pattern.column = 3;
		pattern.severity = 4;
		pattern.code = 5;
		pattern.message = 6;
		definition.patterns = { pattern };
		return { EProblemMatcherLookupStatus::Found, std::move(definition) };
	}

	if (name == L"$gcc") {
		definition.owner = L"gcc";
		definition.source = L"gcc";
		ProblemMatcherPattern pattern;
		pattern.regexp = LR"(^(.*?):(\d+):(\d+):\s*(warning|error)\s*:\s*(.*)$)";
		pattern.file = 1;
		pattern.line = 2;
		pattern.column = 3;
		pattern.severity = 4;
		pattern.message = 5;
		definition.patterns = { pattern };
		return { EProblemMatcherLookupStatus::Found, std::move(definition) };
	}

	if (name == L"$tsc") {
		definition.owner = L"typescript";
		definition.source = L"ts";
		ProblemMatcherPattern pattern;
		pattern.regexp = LR"(^(.*?)\((\d+),(\d+)\):\s*(error|warning)\s+(TS\d+)\s*:\s*(.*)$)";
		pattern.file = 1;
		pattern.line = 2;
		pattern.column = 3;
		pattern.severity = 4;
		pattern.code = 5;
		pattern.message = 6;
		definition.patterns = { pattern };
		return { EProblemMatcherLookupStatus::Found, std::move(definition) };
	}

	return { EProblemMatcherLookupStatus::UnknownName, std::nullopt };
}

ProblemMatchOutcome ProblemMatcherEngine::ProcessOutputLines(
	const ProblemMatcherDefinition& definition, const std::vector<std::string>& lines,
	const ProblemMatcherRunContext& context, const ProblemMatcherEngineLimits& limits)
{
	if (definition.patterns.empty()) return { EProblemMatchStatus::EmptyPatternList };
	if (definition.patterns.size() > limits.maximumPatterns) return { EProblemMatchStatus::MaximumPatternsExceeded };
	if (definition.fileLocation == EProblemMatcherFileLocation::Relative && !context.workspaceRoot) {
		return { EProblemMatchStatus::MissingWorkspaceRoot };
	}
	if (lines.size() > limits.maximumLines) return { EProblemMatchStatus::MaximumLinesExceeded };

	{
		bool hasFile = false;
		bool hasLine = false;
		bool hasMessage = false;
		for (const auto& pattern : definition.patterns) {
			hasFile = hasFile || pattern.file.has_value();
			hasLine = hasLine || pattern.line.has_value();
			hasMessage = hasMessage || pattern.message.has_value();
		}
		if (!hasFile || !hasLine || !hasMessage) return { EProblemMatchStatus::MissingRequiredCaptureField };
	}

	std::vector<CompiledPattern> compiled;
	compiled.reserve(definition.patterns.size());
	for (const auto& pattern : definition.patterns) {
		CompiledPattern entry;
		entry.definition = &pattern;
		try {
			entry.regex = std::wregex(pattern.regexp, std::regex::ECMAScript);
		} catch (const std::regex_error&) {
			return { EProblemMatchStatus::InvalidRegexp };
		}
		const auto groupCount = static_cast<std::size_t>(entry.regex.mark_count());
		const auto checkIndex = [groupCount](const std::optional<int>& field) {
			return field && (*field < 1 || static_cast<std::size_t>(*field) > groupCount);
		};
		if (checkIndex(pattern.file) || checkIndex(pattern.line) || checkIndex(pattern.column)
			|| checkIndex(pattern.endLine) || checkIndex(pattern.endColumn) || checkIndex(pattern.severity)
			|| checkIndex(pattern.code) || checkIndex(pattern.message)) {
			return { EProblemMatchStatus::InvalidGroupIndex };
		}
		compiled.push_back(std::move(entry));
	}

	std::map<std::wstring, problems::ReplaceMarkersRequest, std::less<>> grouped;
	std::size_t totalMarkers{};
	std::size_t patternIndex{};
	AccumulatedFields state;

	const auto emitFieldError = [](const EProblemMatchStatus status, const std::size_t lineIndex) {
		ProblemMatchOutcome outcome;
		outcome.status = status;
		outcome.failingLineIndex = lineIndex;
		return outcome;
	};

	for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
		if (lines[lineIndex].size() > limits.maximumLineLength) {
			return emitFieldError(EProblemMatchStatus::MaximumLineLengthExceeded, lineIndex);
		}
		std::wstring line;
		if (!TryDecodeStrictUtf8(lines[lineIndex], line)) {
			return emitFieldError(EProblemMatchStatus::InvalidUtf8Line, lineIndex);
		}

		for (;;) {
			const auto& compiledPattern = compiled[patternIndex];
			std::wsmatch match;
			if (!std::regex_search(line, match, compiledPattern.regex)) {
				if (patternIndex != 0) {
					patternIndex = 0;
					state = AccumulatedFields{};
					continue;
				}
				break;
			}

			const auto& fieldDefinition = *compiledPattern.definition;
			const auto captured = [&match](const int groupIndex) { return match[groupIndex].str(); };

			if (fieldDefinition.file) {
				auto resolved = ResolveAbsoluteFile(definition.fileLocation, captured(*fieldDefinition.file), context.workspaceRoot);
				if (!resolved) return emitFieldError(EProblemMatchStatus::InvalidFileLocation, lineIndex);
				state.fileUri = std::move(resolved);
			}
			if (fieldDefinition.line) {
				const auto value = ParsePositiveInteger(captured(*fieldDefinition.line));
				if (!value) return emitFieldError(EProblemMatchStatus::InvalidLineOrColumnValue, lineIndex);
				state.line = value;
			}
			if (fieldDefinition.column) {
				const auto value = ParsePositiveInteger(captured(*fieldDefinition.column));
				if (!value) return emitFieldError(EProblemMatchStatus::InvalidLineOrColumnValue, lineIndex);
				state.column = value;
			}
			if (fieldDefinition.endLine) {
				const auto value = ParsePositiveInteger(captured(*fieldDefinition.endLine));
				if (!value) return emitFieldError(EProblemMatchStatus::InvalidLineOrColumnValue, lineIndex);
				state.endLine = value;
			}
			if (fieldDefinition.endColumn) {
				const auto value = ParsePositiveInteger(captured(*fieldDefinition.endColumn));
				if (!value) return emitFieldError(EProblemMatchStatus::InvalidLineOrColumnValue, lineIndex);
				state.endColumn = value;
			}
			if (fieldDefinition.severity) {
				const auto value = ParseSeverityWord(captured(*fieldDefinition.severity));
				if (!value) return emitFieldError(EProblemMatchStatus::InvalidSeverityValue, lineIndex);
				state.severity = value;
			}
			if (fieldDefinition.code) {
				state.code = EncodeUtf8(captured(*fieldDefinition.code));
			}
			if (fieldDefinition.message) {
				state.message = EncodeUtf8(captured(*fieldDefinition.message));
			}

			const bool isLastPattern = (patternIndex + 1 == compiled.size());
			if (isLastPattern) {
				if (state.fileUri && state.line && state.message) {
					const std::uint32_t startLine = *state.line - 1;
					const std::uint32_t startColumn = state.column ? (*state.column - 1) : 0;
					const std::uint32_t endLine = state.endLine ? (*state.endLine - 1) : startLine;
					const std::uint32_t endColumn = state.endColumn ? (*state.endColumn - 1)
						: (endLine == startLine ? startColumn + 1 : startColumn);

					problems::ProblemMarker marker;
					marker.range = { startLine, startColumn, endLine, endColumn };
					marker.severity = state.severity.value_or(definition.defaultSeverity);
					marker.message = *state.message;
					marker.code = state.code;
					marker.source = definition.source ? std::optional<std::string>(EncodeUtf8(*definition.source)) : std::nullopt;

					if (++totalMarkers > limits.maximumMarkersPerRun) {
						return emitFieldError(EProblemMatchStatus::MaximumMarkersExceeded, lineIndex);
					}

					const auto key = platform::uri::UriIdentityService::MakeComparisonKey(*state.fileUri);
					auto found = grouped.find(key);
					if (found == grouped.end()) {
						// `platform::uri::Uri` has no default constructor, so the request is
						// aggregate-initialized rather than default-constructed and assigned.
						problems::ReplaceMarkersRequest request{
							{ context.owner, context.collectionId },
							*state.fileUri,
							std::nullopt,
							{},
						};
						found = grouped.emplace(key, std::move(request)).first;
					}
					found->second.markers.push_back(std::move(marker));
				}

				if (fieldDefinition.loop) {
					// Stay on the last pattern; retained fields (notably `file`) carry into the
					// next match so repeated detail lines under one header keep producing markers.
				} else {
					patternIndex = 0;
					state = AccumulatedFields{};
				}
			} else {
				++patternIndex;
			}
			break;
		}
	}

	ProblemMatchOutcome outcome;
	outcome.status = EProblemMatchStatus::Ok;
	outcome.replacements.reserve(grouped.size());
	for (auto& [key, request] : grouped) {
		outcome.replacements.push_back(std::move(request));
	}
	// `MakeComparisonKey` is an identity key, not a sort key: it is
	// length-prefixed, so ordering by it would order resources by path
	// length. Order the emitted requests by their canonical URI instead,
	// which is both deterministic and the order a reader expects.
	std::sort(outcome.replacements.begin(), outcome.replacements.end(),
		[](const problems::ReplaceMarkersRequest& left, const problems::ReplaceMarkersRequest& right) {
			return left.resource.ToString() < right.resource.ToString();
		});
	return outcome;
}

} // namespace workbench::tasks
