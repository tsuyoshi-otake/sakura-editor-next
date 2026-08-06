/*! @file
 * @brief Staging a selected part of a change, as upstream's `staging.ts` does it.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/scm/GitLineStaging.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace workbench::scm {

namespace {

//! `files.eol` is `auto` by default and resolves to the platform's terminator.
//! This product only runs on Windows, so a text with no evidence gets `\r\n`.
inline constexpr std::wstring_view kPlatformEol = L"\r\n";
inline constexpr std::wstring_view kLineFeed = L"\n";
inline constexpr std::wstring_view kCarriageReturnLineFeed = L"\r\n";

struct TextPosition final {
	int line{};
	int character{};
};

struct TextRange final {
	TextPosition start;
	TextPosition end;
};

[[nodiscard]] bool IsBefore(const TextPosition& left, const TextPosition& right) noexcept
{
	if (left.line != right.line) return left.line < right.line;
	return left.character < right.character;
}

//!
//! @brief `TextDocument.validatePosition`.
//!
//! A line past the end is the end of the last line, and a character past a
//! line's end is that line's end. Upstream's `lineAt` throws instead, but every
//! position reached here is derived from a change or a selection, and reading
//! out of bounds is the one outcome that must not be possible.
//!
[[nodiscard]] TextPosition ValidatePosition(const GitStagingText& text, int line, int character)
{
	const int count = text.LineCount();
	if (count == 0) return {};
	if (line < 0) return { 0, 0 };
	if (line >= count) {
		return { count - 1, static_cast<int>(text.lines[static_cast<std::size_t>(count) - 1U].size()) };
	}
	const int length = static_cast<int>(text.lines[static_cast<std::size_t>(line)].size());
	return { line, std::clamp(character, 0, length) };
}

[[nodiscard]] TextPosition LineStart(const GitStagingText& text, int line)
{
	return ValidatePosition(text, line, 0);
}

[[nodiscard]] TextPosition LineEnd(const GitStagingText& text, int line)
{
	return ValidatePosition(text, line, (std::numeric_limits<int>::max)());
}

//! `new Range(a, b)`, which swaps its arguments when they arrive out of order.
[[nodiscard]] TextRange MakeRange(const TextPosition& first, const TextPosition& second)
{
	if (IsBefore(second, first)) return { second, first };
	return { first, second };
}

//!
//! @brief `TextDocument.getText(range)` for an already validated range.
//!
//! A reversed range yields nothing. Upstream's `Range` constructor would have
//! swapped it and returned the text between, but upstream builds these ranges
//! from positions it has already proved ordered; the one place this product can
//! reach a reversed range is a change so malformed that returning its text would
//! be worse than returning none.
//!
void AppendText(std::wstring& out, const GitStagingText& text, const TextPosition& from, const TextPosition& to)
{
	if (text.lines.empty()) return;
	if (IsBefore(to, from)) return;

	const auto& lines = text.lines;
	const auto& firstLine = lines[static_cast<std::size_t>(from.line)];
	if (from.line == to.line) {
		out.append(firstLine, static_cast<std::size_t>(from.character),
			static_cast<std::size_t>(to.character - from.character));
		return;
	}

	out.append(firstLine, static_cast<std::size_t>(from.character));
	out.append(text.eol);
	for (int line = from.line + 1; line < to.line; ++line) {
		out.append(lines[static_cast<std::size_t>(line)]);
		out.append(text.eol);
	}
	out.append(lines[static_cast<std::size_t>(to.line)], 0U, static_cast<std::size_t>(to.character));
}

//!
//! @brief Upstream's `getModifiedRange`.
//!
//! A deletion has no modified text, so its range is the seam the deleted lines
//! used to occupy: a point between the two surviving lines. That point touches
//! both of them, which is why selecting either line stages the deletion.
//!
[[nodiscard]] TextRange GetModifiedRange(const GitStagingText& modified, const GitLineChange& change)
{
	if (change.modifiedEndLineNumber != 0) {
		return MakeRange(
			LineStart(modified, change.modifiedStartLineNumber - 1),
			LineEnd(modified, change.modifiedEndLineNumber - 1));
	}
	if (change.modifiedStartLineNumber == 0) {
		return MakeRange(LineEnd(modified, 0), LineStart(modified, 0));
	}
	if (modified.LineCount() == change.modifiedStartLineNumber) {
		const TextPosition end = LineEnd(modified, change.modifiedStartLineNumber - 1);
		return { end, end };
	}
	return MakeRange(
		LineEnd(modified, change.modifiedStartLineNumber - 1),
		LineStart(modified, change.modifiedStartLineNumber));
}

[[nodiscard]] bool IsOctalDigit(wchar_t character) noexcept
{
	return character >= L'0' && character <= L'7';
}

[[nodiscard]] bool IsLowerHexDigit(wchar_t character) noexcept
{
	return (character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f');
}

[[nodiscard]] std::wstring_view FirstLine(std::wstring_view text)
{
	const std::size_t end = text.find_first_of(L"\r\n");
	if (end == std::wstring_view::npos) return text;
	return text.substr(0, end);
}

[[nodiscard]] std::wstring_view TrimAscii(std::wstring_view text)
{
	constexpr std::wstring_view kBlank = L" \t\r\n";
	const std::size_t first = text.find_first_not_of(kBlank);
	if (first == std::wstring_view::npos) return {};
	return text.substr(first, text.find_last_not_of(kBlank) - first + 1U);
}

} // namespace

std::vector<GitLineChange> ToGitLineChanges(const GitLineDiff& diff)
{
	std::vector<GitLineChange> changes;
	changes.reserve(diff.changes.size());
	for (const auto& mapping : diff.changes) {
		GitLineChange change;
		if (mapping.original.startLineNumber == mapping.original.endLineNumberExclusive) {
			// An insertion: the original side keeps only the line it follows.
			change.originalStartLineNumber = mapping.original.startLineNumber - 1;
			change.originalEndLineNumber = 0;
		} else {
			change.originalStartLineNumber = mapping.original.startLineNumber;
			change.originalEndLineNumber = mapping.original.endLineNumberExclusive - 1;
		}
		if (mapping.modified.startLineNumber == mapping.modified.endLineNumberExclusive) {
			// A deletion: the modified side keeps only the line it follows.
			change.modifiedStartLineNumber = mapping.modified.startLineNumber - 1;
			change.modifiedEndLineNumber = 0;
		} else {
			change.modifiedStartLineNumber = mapping.modified.startLineNumber;
			change.modifiedEndLineNumber = mapping.modified.endLineNumberExclusive - 1;
		}
		changes.push_back(change);
	}
	return changes;
}

GitLineChange InvertGitLineChange(const GitLineChange& change)
{
	return GitLineChange{
		change.modifiedStartLineNumber,
		change.modifiedEndLineNumber,
		change.originalStartLineNumber,
		change.originalEndLineNumber,
	};
}

bool IsGitLineChangeBefore(const GitLineChange& left, const GitLineChange& right) noexcept
{
	if (left.modifiedStartLineNumber != right.modifiedStartLineNumber) {
		return left.modifiedStartLineNumber < right.modifiedStartLineNumber;
	}
	if (left.modifiedEndLineNumber != right.modifiedEndLineNumber) {
		return left.modifiedEndLineNumber < right.modifiedEndLineNumber;
	}
	if (left.originalStartLineNumber != right.originalStartLineNumber) {
		return left.originalStartLineNumber < right.originalStartLineNumber;
	}
	return left.originalEndLineNumber < right.originalEndLineNumber;
}

std::wstring DetectGitTextEol(std::wstring_view text)
{
	std::size_t carriageReturns = 0;
	std::size_t lineFeeds = 0;
	std::size_t pairs = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] == L'\r') {
			if (index + 1U < text.size() && text[index + 1U] == L'\n') {
				++pairs;
				++index;
			} else {
				++carriageReturns;
			}
		} else if (text[index] == L'\n') {
			++lineFeeds;
		}
	}

	const std::size_t total = carriageReturns + lineFeeds + pairs;
	if (total == 0) return std::wstring(kPlatformEol);
	const std::size_t withCarriageReturn = carriageReturns + pairs;
	return std::wstring(withCarriageReturn > total / 2U ? kCarriageReturnLineFeed : kLineFeed);
}

GitStagingText MakeGitStagingText(std::wstring_view text)
{
	return GitStagingText{ SplitGitDiffLines(text), DetectGitTextEol(text) };
}

std::vector<GitSelectedLines> NormalizeGitSelectedLines(
	std::vector<GitSelectedLines> selections, const GitStagingText& modified)
{
	const int count = modified.LineCount();
	if (count == 0) return {};

	std::vector<GitSelectedLines> widened;
	widened.reserve(selections.size());
	for (const auto& selection : selections) {
		int start = std::clamp(selection.startLine, 0, count - 1);
		int end = std::clamp(selection.endLine, 0, count - 1);
		if (end < start) std::swap(start, end);
		widened.push_back(GitSelectedLines{ start, end });
	}

	std::stable_sort(widened.begin(), widened.end(),
		[](const GitSelectedLines& left, const GitSelectedLines& right) noexcept {
			return left.startLine < right.startLine;
		});

	std::vector<GitSelectedLines> merged;
	merged.reserve(widened.size());
	for (const auto& span : widened) {
		// Upstream merges an adjacent span the same way; the union on an
		// overlapping one is this product's documented divergence.
		if (!merged.empty() && span.startLine <= merged.back().endLine + 1) {
			merged.back().endLine = (std::max)(merged.back().endLine, span.endLine);
			continue;
		}
		merged.push_back(span);
	}
	return merged;
}

std::optional<GitLineChange> IntersectGitLineChange(
	const GitStagingText& modified, const GitLineChange& change, const GitSelectedLines& selection)
{
	if (modified.lines.empty()) return std::nullopt;

	const TextRange modifiedRange = GetModifiedRange(modified, change);
	const TextRange selectionRange = MakeRange(
		LineStart(modified, selection.startLine), LineEnd(modified, selection.endLine));

	// `Range.intersection`: the later start, the earlier end, and nothing when
	// those cross. Touching at a single point is an intersection, which is what
	// attaches a deletion to both of the lines it sits between.
	const TextPosition start =
		IsBefore(selectionRange.start, modifiedRange.start) ? modifiedRange.start : selectionRange.start;
	const TextPosition end =
		IsBefore(modifiedRange.end, selectionRange.end) ? modifiedRange.end : selectionRange.end;
	if (IsBefore(end, start)) return std::nullopt;

	// A deletion has no modified lines to narrow, so it is staged whole.
	if (change.modifiedEndLineNumber == 0) return change;

	const int modifiedStartLineNumber = start.line + 1;
	const int modifiedEndLineNumber = end.line + 1;

	// Upstream's heuristic, verbatim: when both sides changed by the same number
	// of lines, assume they correspond line by line and narrow the original side
	// with the same offset. Otherwise there is no way to say which original lines
	// the selected modified lines replaced, and the whole original side is kept.
	if (change.originalEndLineNumber - change.originalStartLineNumber
		== change.modifiedEndLineNumber - change.modifiedStartLineNumber) {
		const int delta = modifiedStartLineNumber - change.modifiedStartLineNumber;
		const int length = modifiedEndLineNumber - modifiedStartLineNumber;
		return GitLineChange{
			change.originalStartLineNumber + delta,
			change.originalStartLineNumber + delta + length,
			modifiedStartLineNumber,
			modifiedEndLineNumber,
		};
	}

	return GitLineChange{
		change.originalStartLineNumber,
		change.originalEndLineNumber,
		modifiedStartLineNumber,
		modifiedEndLineNumber,
	};
}

std::vector<GitLineChange> SelectGitLineChanges(
	const GitStagingText& modified,
	const std::vector<GitLineChange>& changes,
	const std::vector<GitSelectedLines>& selections)
{
	std::vector<GitLineChange> selected;
	selected.reserve(changes.size());
	for (const auto& change : changes) {
		for (const auto& selection : selections) {
			auto intersected = IntersectGitLineChange(modified, change, selection);
			if (!intersected.has_value()) continue;
			selected.push_back(*intersected);
			break;
		}
	}
	return selected;
}

std::wstring ApplyGitLineChanges(
	const GitStagingText& original, const GitStagingText& modified, const std::vector<GitLineChange>& changes)
{
	std::wstring result;
	const int originalLineCount = original.LineCount();
	int currentLine = 0;

	for (const auto& change : changes) {
		const bool isInsertion = change.IsInsertion();
		const bool isDeletion = change.IsDeletion();

		int endLine = isInsertion ? change.originalStartLineNumber : change.originalStartLineNumber - 1;
		int endCharacter = 0;

		// A deletion that reaches the last line also removes the terminator that
		// ended the line before it, so the kept text has to stop at that line's
		// end rather than at the start of the first deleted line. Upstream cites
		// microsoft/vscode#59670 for this.
		if (isDeletion && change.originalEndLineNumber == originalLineCount) {
			endLine -= 1;
			endCharacter = LineEnd(original, endLine).character;
		}

		AppendText(result, original,
			ValidatePosition(original, currentLine, 0), ValidatePosition(original, endLine, endCharacter));

		if (!isDeletion) {
			int fromLine = change.modifiedStartLineNumber - 1;
			int fromCharacter = 0;

			// An insertion past the original's last line has to start after the
			// previous modified line's last character, so that the terminator it
			// contributes is the one that separates the two, not a second one.
			if (isInsertion && change.originalStartLineNumber == originalLineCount) {
				fromLine -= 1;
				fromCharacter = LineEnd(modified, fromLine).character;
			}

			AppendText(result, modified,
				ValidatePosition(modified, fromLine, fromCharacter),
				ValidatePosition(modified, change.modifiedEndLineNumber, 0));
		}

		currentLine = isInsertion ? change.originalStartLineNumber : change.originalEndLineNumber;
	}

	AppendText(result, original,
		ValidatePosition(original, currentLine, 0), ValidatePosition(original, originalLineCount, 0));
	return result;
}

EGitTextEncoding ClassifyGitTextEncoding(std::string_view bytes) noexcept
{
	// The same call `DecodeGitOutput` makes, with the same flag. Empty input has
	// no evidence either way and round-trips identically under both branches.
	if (bytes.empty()) return EGitTextEncoding::Utf8;
	const int required = ::MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
	return required > 0 ? EGitTextEncoding::Utf8 : EGitTextEncoding::Latin1Fallback;
}

std::optional<std::string> EncodeGitText(std::wstring_view text, EGitTextEncoding encoding)
{
	if (text.empty()) return std::string{};

	if (encoding == EGitTextEncoding::Latin1Fallback) {
		std::string bytes;
		bytes.reserve(text.size());
		for (const wchar_t character : text) {
			if (character > 0x00FF) return std::nullopt;
			bytes.push_back(static_cast<char>(static_cast<unsigned char>(character)));
		}
		return bytes;
	}

	const int required = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0) return std::nullopt;
	std::string bytes(static_cast<std::size_t>(required), '\0');
	const int written = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
		bytes.data(), required, nullptr, nullptr);
	if (written != required) return std::nullopt;
	return bytes;
}

std::vector<std::wstring> BuildGitHashObjectArguments(std::wstring_view path)
{
	return { L"hash-object", L"--stdin", L"-w", L"--path", std::wstring(path) };
}

std::vector<std::wstring> BuildGitHeadObjectDetailsArguments(std::wstring_view path)
{
	return { L"ls-tree", L"-l", L"HEAD", L"--", std::wstring(path) };
}

std::vector<std::wstring> BuildGitStagedObjectDetailsArguments(std::wstring_view path)
{
	return { L"ls-files", L"--stage", L"--", std::wstring(path) };
}

std::optional<std::wstring> ParseGitObjectMode(std::wstring_view text)
{
	// `ls-tree -l` and `ls-files --stage` both begin their line with the mode, so
	// one parser serves both of upstream's two lookups.
	const std::wstring_view line = FirstLine(text);
	const std::size_t end = line.find_first_of(L" \t");
	const std::wstring_view mode = end == std::wstring_view::npos ? line : line.substr(0, end);
	if (mode.size() != 6U) return std::nullopt;
	if (!std::all_of(mode.begin(), mode.end(), IsOctalDigit)) return std::nullopt;
	return std::wstring(mode);
}

std::vector<std::wstring> BuildGitUpdateIndexArguments(
	std::wstring_view mode, std::wstring_view object, std::wstring_view path, bool add)
{
	std::vector<std::wstring> arguments{ L"update-index" };
	// Upstream keeps an empty string in this slot when it is not adding. An empty
	// argument survives this product's quoting as `""`, which git would read as a
	// pathspec, so the argument is omitted instead. The command git receives is
	// the one upstream means.
	if (add) arguments.emplace_back(L"--add");
	arguments.emplace_back(L"--cacheinfo");
	arguments.emplace_back(mode);
	arguments.emplace_back(object);
	arguments.emplace_back(path);
	return arguments;
}

std::optional<std::wstring> ParseGitHashObjectName(std::wstring_view text)
{
	// Upstream passes `hash-object`'s stdout through untrimmed and lets git
	// ignore the trailing newline. Trimming and checking the shape here costs
	// nothing and keeps a garbled value from reaching `--cacheinfo` as an
	// argument. Both hash algorithms are accepted, because which one a
	// repository uses is the repository's choice, not this code's.
	const std::wstring_view name = TrimAscii(text);
	if (name.size() != 40U && name.size() != 64U) return std::nullopt;
	if (!std::all_of(name.begin(), name.end(), IsLowerHexDigit)) return std::nullopt;
	return std::wstring(name);
}

} // namespace workbench::scm
