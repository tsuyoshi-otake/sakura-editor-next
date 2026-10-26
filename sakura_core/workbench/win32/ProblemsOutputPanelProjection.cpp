/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/win32/ProblemsOutputPanelProjection.h"

#include "workbench/output/OutputServiceTypes.h"
#include "workbench/problems/MarkerService.h"

#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace workbench::win32 {
namespace {

[[nodiscard]] std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return L"\xfffd";

	const auto sourceLength = static_cast<int>(value.size());
	const int characterCount = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength, nullptr, 0);
	if (characterCount <= 0) return L"\xfffd";

	std::wstring result(static_cast<std::size_t>(characterCount), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), sourceLength,
		result.data(), characterCount) != characterCount) {
		return L"\xfffd";
	}
	return result;
}

[[nodiscard]] EProblemsPanelSeverity ProjectSeverity(const problems::EMarkerSeverity severity) noexcept
{
	switch (severity) {
	case problems::EMarkerSeverity::Error: return EProblemsPanelSeverity::Error;
	case problems::EMarkerSeverity::Warning: return EProblemsPanelSeverity::Warning;
	case problems::EMarkerSeverity::Information: return EProblemsPanelSeverity::Information;
	case problems::EMarkerSeverity::Hint: return EProblemsPanelSeverity::Hint;
	}
	return EProblemsPanelSeverity::Error;
}

[[nodiscard]] bool ProblemsPanelEntryLess(const ProblemsPanelEntry& left, const ProblemsPanelEntry& right) noexcept
{
	return std::tie(left.resourceUri, left.range.startLine, left.range.startColumn, left.range.endLine,
		left.range.endColumn, left.severity, left.message, left.source) <
		std::tie(right.resourceUri, right.range.startLine, right.range.startColumn, right.range.endLine,
			right.range.endColumn, right.severity, right.message, right.source);
}

// ---------------------------------------------------------------------------
// Problems view (tree) support.
// ---------------------------------------------------------------------------

[[nodiscard]] wchar_t LowerChar(const wchar_t value) noexcept
{
	return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value - L'A' + L'a') : value;
}

[[nodiscard]] std::wstring LowerCopy(std::wstring_view value)
{
	std::wstring result{ value };
	std::transform(result.begin(), result.end(), result.begin(), LowerChar);
	return result;
}

//! VS Code's ResourceMarkers.name / description inputs.
struct ProblemsResourceLabel {
	std::wstring path;
	std::wstring name;
	std::wstring description;
};

//! Computes the file icon's label inputs from a canonical resource URI, the way
//! upstream's `resourceLabel.setFile(resource)` derives them: basename for the
//! name and the containing folder as the description.
[[nodiscard]] ProblemsResourceLabel ProblemsResourceLabelFor(std::wstring_view resourceUri)
{
	std::wstring path;
	const auto parsed = platform::uri::Uri::Parse(resourceUri);
	if (parsed.value && parsed.value->Scheme() == L"file") {
		if (const auto windows = parsed.value->ToWindowsPath(); windows.value) {
			path = *windows.value;
		}
	}
	if (path.empty()) path = parsed.value ? parsed.value->Path() : std::wstring(resourceUri);

	ProblemsResourceLabel label{ .path = path };
	const auto separator = path.find_last_of(L"/\\");
	label.name = separator == std::wstring::npos ? path : path.substr(separator + 1);
	label.description = separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
	return label;
}

[[nodiscard]] std::vector<std::wstring> SplitMarkerLines(std::wstring_view message)
{
	// strings.splitLines semantics: a trailing newline leaves a final empty line.
	std::vector<std::wstring> lines;
	std::size_t position = 0;
	for (;;) {
		const auto end = message.find_first_of(L"\r\n", position);
		if (end == std::wstring_view::npos) {
			lines.emplace_back(message.substr(position));
			break;
		}
		lines.emplace_back(message.substr(position, end - position));
		position = end + (message[end] == L'\r' && end + 1 < message.size() && message[end + 1] == L'\n' ? 2 : 1);
		if (position == message.size()) {
			lines.emplace_back();
			break;
		}
	}
	return lines;
}

[[nodiscard]] std::vector<std::wstring_view> PathSegments(std::wstring_view path)
{
	std::vector<std::wstring_view> segments;
	std::size_t position = 0;
	while (position <= path.size()) {
		const auto end = path.find_first_of(L"/\\", position);
		const auto segment = path.substr(position, end == std::wstring_view::npos ? end : end - position);
		if (!segment.empty()) segments.push_back(segment);
		if (end == std::wstring_view::npos) break;
		position = end + 1;
	}
	return segments;
}

//! Segment-level glob matching: `*` any in-segment run, `?` one character.
[[nodiscard]] bool GlobSegmentMatches(std::wstring_view pattern, std::wstring_view segment) noexcept
{
	std::size_t patternIndex = 0, segmentIndex = 0;
	std::size_t starPattern = std::wstring_view::npos, starSegment = 0;
	while (segmentIndex < segment.size()) {
		if (patternIndex < pattern.size()
			&& (pattern[patternIndex] == L'?' || pattern[patternIndex] == segment[segmentIndex])) {
			++patternIndex;
			++segmentIndex;
		} else if (patternIndex < pattern.size() && pattern[patternIndex] == L'*') {
			starPattern = patternIndex++;
			starSegment = segmentIndex;
		} else if (starPattern != std::wstring_view::npos) {
			patternIndex = starPattern + 1;
			segmentIndex = ++starSegment;
		} else {
			return false;
		}
	}
	while (patternIndex < pattern.size() && pattern[patternIndex] == L'*') ++patternIndex;
	return patternIndex == pattern.size();
}

//! Recursive run matcher: pattern segments against path segments starting at `si`.
[[nodiscard]] bool GlobRunMatches(const std::vector<std::wstring_view>& pattern,
	const std::vector<std::wstring_view>& path, const std::size_t pi, const std::size_t si) noexcept
{
	if (pi >= pattern.size()) return true;
	if (pattern[pi] == L"**") {
		for (std::size_t span = 0; si + span <= path.size(); ++span) {
			if (GlobRunMatches(pattern, path, pi + 1, si + span)) return true;
		}
		return false;
	}
	if (si >= path.size()) return false;
	if (!GlobSegmentMatches(pattern[pi], path[si])) return false;
	return GlobRunMatches(pattern, path, pi + 1, si + 1);
}

[[nodiscard]] int SeverityRank(const EProblemsPanelSeverity severity) noexcept
{
	// Upstream MarkerSeverity: Error=8, Warning=4, Info=2, Hint=1, compared descending.
	switch (severity) {
	case EProblemsPanelSeverity::Error: return 0;
	case EProblemsPanelSeverity::Warning: return 1;
	case EProblemsPanelSeverity::Information: return 2;
	case EProblemsPanelSeverity::Hint: return 3;
	}
	return 3;
}

//! Deterministic stand-in for localeCompare: case-insensitive, then ordinal.
[[nodiscard]] int CompareResourceText(const std::wstring& left, const std::wstring& right) noexcept
{
	const int insensitive = ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
		right.c_str(), static_cast<int>(right.size()), TRUE) - CSTR_EQUAL;
	if (insensitive != 0) return insensitive;
	return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
		right.c_str(), static_cast<int>(right.size()), FALSE) - CSTR_EQUAL;
}

[[nodiscard]] bool MarkerLess(const ProblemsPanelEntry& left, const ProblemsPanelEntry& right) noexcept
{
	const int leftRank = SeverityRank(left.severity), rightRank = SeverityRank(right.severity);
	if (leftRank != rightRank) return leftRank < rightRank;
	const int resource = CompareResourceText(left.resourceUri, right.resourceUri);
	if (resource != 0) return resource < 0;
	return std::tie(left.range.startLine, left.range.startColumn, left.range.endLine, left.range.endColumn)
		< std::tie(right.range.startLine, right.range.startColumn, right.range.endLine, right.range.endColumn);
}

//! IMarkerData.makeKey shape: source;code;severity;start;end;message.
[[nodiscard]] std::wstring MarkerDedupKey(const ProblemsPanelEntry& entry)
{
	std::wstring key;
	key.reserve(entry.message.size() + 64);
	key += entry.source;
	key += L';';
	key += entry.code;
	key += L';';
	key += std::to_wstring(static_cast<unsigned>(entry.severity));
	key += L';';
	for (const auto value : { entry.range.startLine, entry.range.startColumn,
		entry.range.endLine, entry.range.endColumn }) {
		key += std::to_wstring(value);
		key += L',';
	}
	key += entry.message;
	return key;
}

//! extUri identity: file URIs compare path-insensitively on Windows.
[[nodiscard]] bool SameProblemsResource(std::wstring_view left, std::wstring_view right) noexcept
{
	const auto parsedLeft = platform::uri::Uri::Parse(left);
	const auto parsedRight = platform::uri::Uri::Parse(right);
	if (parsedLeft.value && parsedRight.value) {
		return platform::uri::UriIdentityService::IsEqual(*parsedLeft.value, *parsedRight.value);
	}
	return left == right;
}

[[nodiscard]] std::wstring_view Basename(std::wstring_view path) noexcept
{
	const auto separator = path.find_last_of(L"/\\");
	return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

[[nodiscard]] void AppendJsonString(std::wstring& out, const std::wstring_view value)
{
	out += L'"';
	for (const wchar_t character : value) {
		switch (character) {
		case L'"': out += L"\\\""; break;
		case L'\\': out += L"\\\\"; break;
		case L'\n': out += L"\\n"; break;
		case L'\r': out += L"\\r"; break;
		case L'\t': out += L"\\t"; break;
		case L'\b': out += L"\\b"; break;
		case L'\f': out += L"\\f"; break;
		default:
			if (character < 0x20) {
				wchar_t buffer[8]{};
				(void)::swprintf(buffer, std::size(buffer), L"\\u%04x", static_cast<unsigned>(character));
				out += buffer;
			} else {
				out += character;
			}
		}
	}
	out += L'"';
}

[[nodiscard]] std::wstring JsonIndent(const unsigned depth)
{
	return std::wstring(depth, L'\t');
}

[[nodiscard]] unsigned JsonSeverity(const EProblemsPanelSeverity severity) noexcept
{
	switch (severity) {
	case EProblemsPanelSeverity::Error: return 8;
	case EProblemsPanelSeverity::Warning: return 4;
	case EProblemsPanelSeverity::Information: return 2;
	case EProblemsPanelSeverity::Hint: return 1;
	}
	return 8;
}

[[nodiscard]] std::wstring JsonUriPath(const std::wstring_view resourceUri)
{
	const auto parsed = platform::uri::Uri::Parse(resourceUri);
	return parsed.value ? parsed.value->Path() : std::wstring(resourceUri);
}

void AppendMarkerJson(std::wstring& out, const ProblemsPanelEntry& entry, const unsigned depth)
{
	out += L'{';
	auto field = [&](const wchar_t* name, const std::wstring& value, bool quoted) {
		out += L'\n';
		out += JsonIndent(depth + 1);
		out += L'"';
		out += name;
		out += L"\":";
		out += L' ';
		if (quoted) AppendJsonString(out, value);
		else out += value;
		out += L',';
	};
	// Upstream emits `...this.marker` then overrides `resource` with the URI path.
	field(L"resource", JsonUriPath(entry.resourceUri), true);
	if (!entry.owner.empty()) field(L"owner", entry.owner, true);
	if (!entry.code.empty()) field(L"code", entry.code, true);
	field(L"severity", std::to_wstring(JsonSeverity(entry.severity)), false);
	field(L"message", entry.message, true);
	if (!entry.source.empty()) field(L"source", entry.source, true);
	field(L"startLineNumber", std::to_wstring(static_cast<std::uint64_t>(entry.range.startLine) + 1), false);
	field(L"startColumn", std::to_wstring(static_cast<std::uint64_t>(entry.range.startColumn) + 1), false);
	field(L"endLineNumber", std::to_wstring(static_cast<std::uint64_t>(entry.range.endLine) + 1), false);
	field(L"endColumn", std::to_wstring(static_cast<std::uint64_t>(entry.range.endColumn) + 1), false);
	// `field` always appends a trailing comma; remove it before an optional array tail.
	out.pop_back();
	if (!entry.relatedInformation.empty()) {
		out += L',';
		out += L'\n';
		out += JsonIndent(depth + 1);
		out += L"\"relatedInformation\": [";
		for (std::size_t index = 0; index < entry.relatedInformation.size(); ++index) {
			const auto& related = entry.relatedInformation[index];
			out += index == 0 ? L'\n' : L',';
			if (index != 0) out += L'\n';
			out += JsonIndent(depth + 2);
			out += L'{';
			out += L'\n';
			out += JsonIndent(depth + 3);
			out += L"\"resource\": ";
			AppendJsonString(out, JsonUriPath(related.resourceUri));
			out += L',';
			out += L'\n';
			out += JsonIndent(depth + 3);
			out += L"\"message\": ";
			AppendJsonString(out, related.message);
			out += L',';
			out += L'\n';
			out += JsonIndent(depth + 3);
			out += L"\"startLineNumber\": " + std::to_wstring(static_cast<std::uint64_t>(related.range.startLine) + 1);
			out += L',';
			out += L'\n';
			out += JsonIndent(depth + 3);
			out += L"\"startColumn\": " + std::to_wstring(static_cast<std::uint64_t>(related.range.startColumn) + 1);
			out += L',';
			out += L'\n';
			out += JsonIndent(depth + 3);
			out += L"\"endLineNumber\": " + std::to_wstring(static_cast<std::uint64_t>(related.range.endLine) + 1);
			out += L',';
			out += L'\n';
			out += JsonIndent(depth + 3);
			out += L"\"endColumn\": " + std::to_wstring(static_cast<std::uint64_t>(related.range.endColumn) + 1);
			out += L'\n';
			out += JsonIndent(depth + 2);
			out += L'}';
		}
		out += L'\n';
		out += JsonIndent(depth + 1);
		out += L']';
	}
	out += L'\n';
	out += JsonIndent(depth);
	out += L'}';
}

[[nodiscard]] std::wstring TrimCopy(std::wstring_view value)
{
	while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.remove_prefix(1);
	while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.remove_suffix(1);
	return std::wstring{ value };
}

} // namespace

ProblemsPanelSnapshot ProjectProblemsPanel(const problems::ProblemsSnapshot& snapshot)
{
	ProblemsPanelSnapshot result{ .revision = snapshot.revision, .stopped = snapshot.stopped };
	for (const auto& resource : snapshot.resources) {
		const auto resourceUri = resource.resource.ToString();
		const auto owner = Utf8ToWide(resource.collection.id);
		for (const auto& marker : resource.markers) {
			ProblemsPanelEntry entry{
				.resourceUri = resourceUri,
				.range = { marker.range.startLine, marker.range.startColumn, marker.range.endLine, marker.range.endColumn },
				.severity = ProjectSeverity(marker.severity),
				.message = Utf8ToWide(marker.message),
				.source = marker.source ? Utf8ToWide(*marker.source) : std::wstring{},
				.code = marker.code ? Utf8ToWide(*marker.code) : std::wstring{},
				.owner = owner,
			};
			entry.relatedInformation.reserve(marker.relatedInformation.size());
			for (const auto& related : marker.relatedInformation) {
				entry.relatedInformation.push_back({
					.resourceUri = related.resource.ToString(),
					.range = { related.range.startLine, related.range.startColumn,
						related.range.endLine, related.range.endColumn },
					.message = Utf8ToWide(related.message),
				});
			}
			result.entries.push_back(std::move(entry));
		}
	}

	std::sort(result.entries.begin(), result.entries.end(), ProblemsPanelEntryLess);
	return result;
}

ProblemsViewFilterOptions ParseProblemsViewFilterText(std::wstring_view filterText)
{
	ProblemsViewFilterOptions options;
	std::wstring filter = TrimCopy(filterText);

	// Upstream SOURCE_FILTER_REGEX: /(!)?@source:("[^"]*"|[^\s,]+)(\s*)/i
	std::wstring remainder;
	std::size_t index = 0;
	for (; index < filter.size();) {
		const auto at = filter.find(L"@source:", index);
		bool negate = false;
		std::size_t marker = at;
		if (at == std::wstring::npos) {
			// Also accept a leading `!` before a later @source (handled on next pass).
			break;
		}
		if (at > 0 && filter[at - 1] == L'!') {
			negate = true;
			marker = at - 1;
		}
		remainder += filter.substr(index, marker - index);
		std::size_t begin = at + 8;
		std::wstring source;
		std::size_t end = begin;
		if (begin < filter.size() && filter[begin] == L'"') {
			const auto close = filter.find(L'"', begin + 1);
			if (close != std::wstring::npos) {
				source = filter.substr(begin + 1, close - begin - 1);
				end = close + 1;
			}
		} else {
			while (end < filter.size() && filter[end] != L' ' && filter[end] != L'\t' && filter[end] != L',') ++end;
			source = filter.substr(begin, end - begin);
		}
		while (end < filter.size() && (filter[end] == L' ' || filter[end] == L'\t')) ++end;
		(negate ? options.excludeSources : options.includeSources).push_back(LowerCopy(source));
		index = end;
	}
	remainder += filter.substr(index);
	filter = TrimCopy(remainder);

	options.negate = !filter.empty() && filter.front() == L'!';
	options.text = options.negate ? TrimCopy(std::wstring_view{ filter }.substr(1)) : filter;

	if (!filter.empty()) {
		for (std::size_t position = 0; position <= filter.size();) {
			const auto comma = filter.find(L',', position);
			auto piece = TrimCopy(std::wstring_view{ filter }.substr(position,
				comma == std::wstring::npos ? comma : comma - position));
			if (!piece.empty()) {
				if (piece.front() == L'!') {
					piece.erase(piece.begin());
					if (!piece.empty()) options.excludePatterns.push_back(std::move(piece));
				} else {
					options.includePatterns.push_back(std::move(piece));
				}
			}
			if (comma == std::wstring::npos) break;
			position = comma + 1;
		}
	}
	return options;
}

bool ProblemsViewMatchesFuzzy(const std::wstring_view query, const std::wstring_view candidate) noexcept
{
	if (query.empty()) return false;
	std::size_t candidateIndex = 0;
	for (const wchar_t character : query) {
		const wchar_t wanted = LowerChar(character);
		bool found = false;
		while (candidateIndex < candidate.size()) {
			if (LowerChar(candidate[candidateIndex]) == wanted) {
				++candidateIndex;
				found = true;
				break;
			}
			++candidateIndex;
		}
		if (!found) return false;
	}
	return true;
}

bool ProblemsViewGlobMatches(std::wstring_view pattern, const std::wstring_view resourcePath)
{
	if (pattern.empty()) return false;
	if (pattern.front() == L'.') {
		// setPattern converts ".js" into "*.js".
		pattern = pattern.substr(1);
		if (pattern.empty()) return false;
		const std::wstring starred = L"*" + std::wstring(pattern);
		return ProblemsViewGlobMatches(starred, resourcePath);
	}
	const auto patternSegments = PathSegments(pattern);
	const auto pathSegments = PathSegments(resourcePath);
	if (patternSegments.empty() || pathSegments.empty()) return false;
	for (std::size_t start = 0; start < pathSegments.size(); ++start) {
		if (GlobRunMatches(patternSegments, pathSegments, 0, start)) return true;
	}
	return false;
}

ProblemsViewTree ProjectProblemsViewTree(const ProblemsPanelSnapshot& snapshot,
	const ProblemsViewFilterState& state, const std::optional<std::wstring>& activeResourceUri)
{
	const auto options = ParseProblemsViewFilterText(state.text);
	ProblemsViewTree result{ .revision = snapshot.revision, .stopped = snapshot.stopped };

	// VS Code reads only Error/Warning/Info severities into the model; dedupe the
	// task-matcher/language-server double-report shape upstream removes (#244424).
	std::map<std::wstring, std::size_t> resourceIndex;
	std::set<std::wstring> dedup;
	for (const auto& entry : snapshot.entries) {
		const bool counted = entry.severity == EProblemsPanelSeverity::Error
			|| entry.severity == EProblemsPanelSeverity::Warning
			|| entry.severity == EProblemsPanelSeverity::Information;
		if (!counted) continue;
		const auto key = entry.resourceUri + L'\n' + MarkerDedupKey(entry);
		if (!dedup.insert(key).second) continue;
		++result.totalMarkers;
		switch (entry.severity) {
		case EProblemsPanelSeverity::Error: ++result.errors; break;
		case EProblemsPanelSeverity::Warning: ++result.warnings; break;
		case EProblemsPanelSeverity::Information: ++result.infos; break;
		default: break;
		}
		if (state.activeFileOnly) {
			if (!activeResourceUri || !SameProblemsResource(entry.resourceUri, *activeResourceUri)) continue;
		}
		const auto [found, inserted] = resourceIndex.try_emplace(entry.resourceUri, result.resources.size());
		if (inserted) {
			const auto label = ProblemsResourceLabelFor(entry.resourceUri);
			result.resources.push_back({ .resourceUri = entry.resourceUri, .path = label.path,
				.name = label.name, .description = label.description });
		}
		auto& markers = result.resources[found->second].markers;
		markers.push_back({ .entry = entry, .lines = SplitMarkerLines(entry.message) });
	}
	for (auto& resource : result.resources) {
		std::stable_sort(resource.markers.begin(), resource.markers.end(),
			[](const ProblemsViewMarker& left, const ProblemsViewMarker& right) {
				return MarkerLess(left.entry, right.entry);
			});
	}
	std::stable_sort(result.resources.begin(), result.resources.end(),
		[](const ProblemsViewResource& left, const ProblemsViewResource& right) {
			const int leftRank = left.markers.empty() ? 3 : SeverityRank(left.markers.front().entry.severity);
			const int rightRank = right.markers.empty() ? 3 : SeverityRank(right.markers.front().entry.severity);
			if (leftRank != rightRank) return leftRank < rightRank;
			const int path = CompareResourceText(left.path, right.path);
			if (path != 0) return path < 0;
			return CompareResourceText(left.name, right.name) < 0;
		});

	// VS Code Filter: resources first (glob excludes/includes, basename fuzzy),
	// then markers (severity, source, text), then related information.
	std::vector<ProblemsViewResource> visible;
	visible.reserve(result.resources.size());
	for (auto& resource : result.resources) {
		bool excluded = false;
		for (const auto& pattern : options.excludePatterns) {
			if (ProblemsViewGlobMatches(pattern, resource.path)) { excluded = true; break; }
		}
		if (excluded) continue;
		bool resourceMatched = false;
		for (const auto& pattern : options.includePatterns) {
			if (ProblemsViewGlobMatches(pattern, resource.path)) { resourceMatched = true; break; }
		}
		if (!resourceMatched && !options.text.empty() && !options.negate
			&& ProblemsViewMatchesFuzzy(options.text, resource.name)) {
			resourceMatched = true;
		}
		// parentVisibility: resourceMatched means the resource is visible regardless
		// of children (TreeVisibility.Visible); otherwise Recurse.
		const bool parentVisible = resourceMatched;
		std::vector<ProblemsViewMarker> kept;
		for (auto& marker : resource.markers) {
			const auto severity = marker.entry.severity;
			const bool severityShown = (severity == EProblemsPanelSeverity::Error && state.showErrors)
				|| (severity == EProblemsPanelSeverity::Warning && state.showWarnings)
				|| (severity == EProblemsPanelSeverity::Information && state.showInfos);
			if (!severityShown) continue;
			const auto source = LowerCopy(marker.entry.source);
			if (!options.includeSources.empty() || !options.excludeSources.empty()) {
				if (!source.empty() && std::ranges::find(options.excludeSources, source) != options.excludeSources.end()) continue;
				if (!options.includeSources.empty()
					&& (source.empty() || std::ranges::find(options.includeSources, source) == options.includeSources.end())) continue;
			}
			if (options.text.empty()) {
				kept.push_back(std::move(marker));
			} else {
				bool matched = ProblemsViewMatchesFuzzy(options.text, marker.entry.source)
					|| ProblemsViewMatchesFuzzy(options.text, marker.entry.code);
				if (!matched) {
					for (const auto& line : marker.lines) {
						if (ProblemsViewMatchesFuzzy(options.text, line)) { matched = true; break; }
					}
				}
				if (matched && !options.negate) {
					kept.push_back(std::move(marker));
				} else if (matched && options.negate && parentVisible) {
					kept.push_back(std::move(marker));
				} else if (!matched && options.negate) {
					kept.push_back(std::move(marker));
				} else if (parentVisible) {
					kept.push_back(std::move(marker));
				}
			}
		}
		for (auto& marker : kept) {
			// Related information keeps its own fuzzy pass; a marker row is still
			// shown when it has no visible related rows.
			if (!options.text.empty()) {
				auto& related = marker.entry.relatedInformation;
				const bool negate = options.negate;
				related.erase(std::remove_if(related.begin(), related.end(), [&](const auto& item) {
					const bool matched = ProblemsViewMatchesFuzzy(options.text, Basename(item.resourceUri))
						|| ProblemsViewMatchesFuzzy(options.text, item.message);
					if (negate) return matched && !parentVisible;
					return !matched && !parentVisible;
				}), related.end());
			}
		}
		if (parentVisible || !kept.empty()) {
			resource.markers = std::move(kept);
			visible.push_back(std::move(resource));
		}
	}
	result.resources = std::move(visible);
	for (const auto& resource : result.resources) {
		result.visibleMarkers += static_cast<std::uint32_t>(resource.markers.size());
	}

	if (result.visibleMarkers == 0) {
		if (state.activeFileOnly) {
			const bool activeHasMarkers = activeResourceUri
				&& std::ranges::any_of(snapshot.entries, [&](const auto& entry) {
					return SameProblemsResource(entry.resourceUri, *activeResourceUri);
				});
			result.emptyState = activeHasMarkers ? EProblemsViewEmptyState::Filtered
				: EProblemsViewEmptyState::ActiveFile;
		} else {
			result.emptyState = result.totalMarkers > 0 ? EProblemsViewEmptyState::Filtered
				: EProblemsViewEmptyState::Workspace;
		}
	}
	return result;
}

std::wstring SerializeProblemsViewMarker(const ProblemsPanelEntry& entry)
{
	std::wstring result;
	AppendMarkerJson(result, entry, 0);
	return result;
}

std::wstring BuildProblemsViewCopyText(const std::vector<const ProblemsPanelEntry*>& entries)
{
	std::wstring result{ L"[" };
	bool first = true;
	for (const auto* entry : entries) {
		if (entry == nullptr) continue;
		if (!first) result += L',';
		first = false;
		AppendMarkerJson(result, *entry, 0);
	}
	result += L']';
	return result;
}

OutputPanelSnapshot ProjectOutputPanel(const output::OutputServiceSnapshot& snapshot)
{
	OutputPanelSnapshot result{
		.revision = snapshot.revision,
		.stopped = snapshot.stopped,
		.activeChannelId = snapshot.activeChannelId,
	};
	result.channels.reserve(snapshot.channels.size());
	for (const auto& channel : snapshot.channels) {
		result.channels.push_back({
			.channelId = channel.channelId,
			.label = Utf8ToWide(channel.label),
			.projectedText = Utf8ToWide(channel.projectedText),
			.kind = channel.kind,
			.ownerId = channel.owner.ownerId,
			.visible = channel.visible,
			.lastShowPreservedFocus = channel.lastShowPreservedFocus,
		});
	}
	std::sort(result.channels.begin(), result.channels.end(), [](const OutputPanelChannel& left, const OutputPanelChannel& right) {
		return left.channelId < right.channelId;
	});
	return result;
}

namespace {

//! Upstream's `splitGlobAware` for the Output filter: commas inside double
//! quotes do not split. A trailing unclosed quote keeps the rest of the text
//! in the last piece, the same as upstream's scan.
[[nodiscard]] std::vector<std::wstring> SplitOutputFilterGlobAware(std::wstring_view text)
{
	std::vector<std::wstring> pieces;
	std::wstring piece;
	bool quoted = false;
	for (const wchar_t character : text) {
		if (character == L'"') {
			quoted = !quoted;
			piece.push_back(character);
			continue;
		}
		if (character == L',' && !quoted) {
			pieces.push_back(std::move(piece));
			piece.clear();
			continue;
		}
		piece.push_back(character);
	}
	pieces.push_back(std::move(piece));
	return pieces;
}

[[nodiscard]] std::wstring TrimOutputFilterPiece(std::wstring piece)
{
	const auto isSpace = [](const wchar_t character) {
		return character == L' ' || character == L'\t';
	};
	const auto first = std::find_if_not(piece.begin(), piece.end(), isSpace);
	const auto last = std::find_if_not(piece.rbegin(), piece.rend(), isSpace).base();
	if (first >= last) return {};
	return std::wstring(first, last);
}

[[nodiscard]] int CompareOutputChannelLabels(const OutputPanelChannel& left,
	const OutputPanelChannel& right) noexcept
{
	// Upstream sorts channel descriptors by label (locale-aware); the native
	// projection keeps the comparison deterministic and case-insensitive.
	return ::CompareStringOrdinal(left.label.data(), static_cast<int>(left.label.size()),
		right.label.data(), static_cast<int>(right.label.size()), TRUE) - CSTR_EQUAL;
}

} // namespace

OutputViewFilterOptions ParseOutputViewFilterText(const std::wstring_view filterText)
{
	OutputViewFilterOptions options;
	for (auto& piece : SplitOutputFilterGlobAware(filterText)) {
		auto pattern = TrimOutputFilterPiece(std::move(piece));
		if (pattern.empty()) continue;
		// Negation reads the trimmed piece itself; a `!` inside double quotes is
		// literal include text. Surrounding quotes strip after that check.
		const bool exclude = pattern.front() == L'!';
		if (exclude) pattern.erase(pattern.begin());
		if (pattern.size() >= 2 && pattern.front() == L'"' && pattern.back() == L'"') {
			pattern = pattern.substr(1, pattern.size() - 2);
		}
		if (pattern.empty()) continue;
		(exclude ? options.excludePatterns : options.includePatterns)
			.push_back(std::move(pattern));
	}
	return options;
}

OutputViewFilteredText ApplyOutputViewFilter(
	const std::wstring_view projectedText, const OutputViewFilterOptions& options)
{
	OutputViewFilteredText result;
	const auto matches = [](std::wstring_view line, std::wstring_view pattern) {
		return !pattern.empty() && line.find(pattern) != std::wstring_view::npos;
	};
	std::size_t cursor = 0;
	while (cursor <= projectedText.size()) {
		const auto newline = projectedText.find(L'\n', cursor);
		const auto end = newline == std::wstring_view::npos ? projectedText.size() : newline;
		auto line = projectedText.substr(cursor, end - cursor);
		if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
		cursor = end + 1;

		const auto excluded = std::any_of(options.excludePatterns.begin(),
			options.excludePatterns.end(), [&](const std::wstring& pattern) {
				return matches(line, pattern);
			});
		const auto included = options.includePatterns.empty()
			|| std::any_of(options.includePatterns.begin(), options.includePatterns.end(),
				[&](const std::wstring& pattern) { return matches(line, pattern); });
		if (excluded || !included) {
			++result.hiddenLineCount;
			continue;
		}
		if (!result.text.empty()) result.text += L"\r\n";
		const std::uint32_t lineOffset = static_cast<std::uint32_t>(result.text.size());
		result.text += line;
		for (const auto& pattern : options.includePatterns) {
			for (std::size_t position = line.find(pattern); position != std::wstring_view::npos;
				position = line.find(pattern, position + pattern.size())) {
				result.matchRanges.push_back({
					lineOffset + static_cast<std::uint32_t>(position),
					static_cast<std::uint32_t>(pattern.size()) });
			}
		}
		if (newline == std::wstring_view::npos) break;
	}
	return result;
}

OutputViewChannelGroups GroupOutputViewChannels(const std::vector<OutputPanelChannel>& channels)
{
	// VS Code partitions channel descriptors by `extensionId` into the
	// `0_ext_outputchannels`, `1_core_outputchannels`, and
	// `2_user_outputchannels` menu groups. This service has no extension-id or
	// user field; its owner ids use `core.*` for workbench-internal producers,
	// so the core group is the `core.` prefix and every other owner (including
	// the built-in Git provider's `workbench.scm.git`, which is an extension
	// channel upstream) lands in the extension group.
	const auto byLabel = [&channels](const std::size_t left, const std::size_t right) {
		const int order = CompareOutputChannelLabels(channels[left], channels[right]);
		return order != 0 ? order < 0 : left < right;
	};
	OutputViewChannelGroups groups;
	for (std::size_t index = 0; index < channels.size(); ++index) {
		if (channels[index].ownerId.starts_with("core.")) {
			groups.coreChannels.push_back(index);
		} else {
			groups.extensionChannels.push_back(index);
		}
	}
	std::sort(groups.extensionChannels.begin(), groups.extensionChannels.end(), byLabel);
	std::sort(groups.coreChannels.begin(), groups.coreChannels.end(), byLabel);
	return groups;
}

} // namespace workbench::win32
