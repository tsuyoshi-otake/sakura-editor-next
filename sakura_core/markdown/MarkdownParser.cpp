/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "MarkdownParser.h"

#include <algorithm>
#include <utility>

namespace markdown {
namespace {

[[nodiscard]] bool IsSpace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t';
}

[[nodiscard]] std::wstring_view TrimLeft(std::wstring_view value) noexcept
{
	while (!value.empty() && IsSpace(value.front())) {
		value.remove_prefix(1);
	}
	return value;
}

[[nodiscard]] std::wstring_view TrimRight(std::wstring_view value) noexcept
{
	while (!value.empty() && IsSpace(value.back())) {
		value.remove_suffix(1);
	}
	return value;
}

[[nodiscard]] std::wstring_view Trim(std::wstring_view value) noexcept
{
	return TrimRight(TrimLeft(value));
}

[[nodiscard]] std::wstring_view StripUpToThreeSpaces(std::wstring_view value) noexcept
{
	std::size_t count = 0;
	while (count < value.size() && count < 3 && value[count] == L' ') {
		++count;
	}
	return value.substr(count);
}

[[nodiscard]] std::size_t CountIndent(std::wstring_view value) noexcept
{
	std::size_t count = 0;
	while (count < value.size() && IsSpace(value[count])) {
		++count;
	}
	return count;
}

struct ParsedText {
	std::wstring text;
	std::vector<InlineSpan> links;
};

[[nodiscard]] ParsedText ParseInlineLinks(std::wstring_view source)
{
	ParsedText result;
	result.text.reserve(source.size());

	for (std::size_t index = 0; index < source.size();) {
		if (source[index] != L'[') {
			result.text.push_back(source[index++]);
			continue;
		}

		const auto labelEnd = source.find(L']', index + 1);
		if (labelEnd == std::wstring_view::npos || labelEnd + 1 >= source.size()
			|| source[labelEnd + 1] != L'(') {
			result.text.push_back(source[index++]);
			continue;
		}
		const auto targetEnd = source.find(L')', labelEnd + 2);
		if (targetEnd == std::wstring_view::npos) {
			result.text.push_back(source[index++]);
			continue;
		}

		const auto label = source.substr(index + 1, labelEnd - index - 1);
		const auto start = result.text.size();
		result.text.append(label);
		if (!label.empty()) {
			result.links.push_back({ InlineKind::Link, start, label.size() });
		}
		index = targetEnd + 1;
	}
	return result;
}

[[nodiscard]] Block MakeTextBlock(BlockKind kind, std::wstring_view source,
	int level = 0, std::wstring marker = {})
{
	auto parsed = ParseInlineLinks(source);
	Block result;
	result.kind = kind;
	result.level = level;
	result.marker = std::move(marker);
	result.text = std::move(parsed.text);
	result.inlineSpans = std::move(parsed.links);
	return result;
}

[[nodiscard]] bool IsFenceStart(std::wstring_view line, wchar_t* marker, std::size_t* markerCount) noexcept
{
	line = StripUpToThreeSpaces(line);
	if (line.empty() || (line.front() != L'`' && line.front() != L'~')) {
		return false;
	}
	const wchar_t candidate = line.front();
	std::size_t count = 0;
	while (count < line.size() && line[count] == candidate) {
		++count;
	}
	if (count < 3) {
		return false;
	}
	*marker = candidate;
	*markerCount = count;
	return true;
}

[[nodiscard]] bool IsFenceClose(std::wstring_view line, wchar_t marker, std::size_t markerCount) noexcept
{
	line = StripUpToThreeSpaces(line);
	std::size_t count = 0;
	while (count < line.size() && line[count] == marker) {
		++count;
	}
	return count >= markerCount && Trim(line.substr(count)).empty();
}

[[nodiscard]] bool IsHorizontalRule(std::wstring_view line) noexcept
{
	line = Trim(line);
	wchar_t marker = L'\0';
	std::size_t count = 0;
	for (const auto value : line) {
		if (IsSpace(value)) {
			continue;
		}
		if (value != L'-' && value != L'*' && value != L'_') {
			return false;
		}
		if (marker == L'\0') {
			marker = value;
		}
		if (marker != value) {
			return false;
		}
		++count;
	}
	return count >= 3;
}

struct HeadingMatch {
	int level = 0;
	std::wstring_view text;
};

[[nodiscard]] bool ParseHeading(std::wstring_view line, HeadingMatch* match) noexcept
{
	line = StripUpToThreeSpaces(line);
	std::size_t hashes = 0;
	while (hashes < line.size() && hashes < 6 && line[hashes] == L'#') {
		++hashes;
	}
	if (hashes == 0 || (hashes < line.size() && !IsSpace(line[hashes]))) {
		return false;
	}
	auto content = Trim(line.substr(hashes));
	const auto trailingStart = content.find_last_not_of(L" \t#");
	if (trailingStart != std::wstring_view::npos && trailingStart + 1 < content.size()) {
		const auto possibleClosing = content.substr(trailingStart + 1);
		if (possibleClosing.find_first_not_of(L" \t#") == std::wstring_view::npos
			&& content[trailingStart] != L'#') {
			content = TrimRight(content.substr(0, trailingStart + 1));
		}
	}
	match->level = static_cast<int>(hashes);
	match->text = content;
	return true;
}

struct ListMatch {
	BlockKind kind = BlockKind::Paragraph;
	int level = 0;
	std::wstring marker;
	std::wstring_view text;
};

[[nodiscard]] bool ParseListItem(std::wstring_view line, ListMatch* match)
{
	const auto indent = CountIndent(line);
	line.remove_prefix(indent);
	if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'*' || line[0] == L'+') && IsSpace(line[1])) {
		match->kind = BlockKind::BulletListItem;
		match->level = static_cast<int>(indent / 2);
		match->marker = L"\x2022 ";
		match->text = TrimLeft(line.substr(2));
		return true;
	}

	std::size_t numberEnd = 0;
	while (numberEnd < line.size() && line[numberEnd] >= L'0' && line[numberEnd] <= L'9') {
		++numberEnd;
	}
	if (numberEnd == 0 || numberEnd + 1 >= line.size()
		|| (line[numberEnd] != L'.' && line[numberEnd] != L')') || !IsSpace(line[numberEnd + 1])) {
		return false;
	}
	match->kind = BlockKind::OrderedListItem;
	match->level = static_cast<int>(indent / 2);
	match->marker.assign(line.substr(0, numberEnd + 1));
	match->marker.push_back(L' ');
	match->text = TrimLeft(line.substr(numberEnd + 2));
	return true;
}

[[nodiscard]] bool IsBlockStart(std::wstring_view line) noexcept
{
	wchar_t marker = L'\0';
	std::size_t markerCount = 0;
	if (IsFenceStart(line, &marker, &markerCount) || IsHorizontalRule(line)) {
		return true;
	}
	HeadingMatch heading;
	if (ParseHeading(line, &heading)) {
		return true;
	}
	line = StripUpToThreeSpaces(line);
	if (!line.empty() && line.front() == L'>') {
		return true;
	}
	ListMatch list;
	return ParseListItem(line, &list);
}

[[nodiscard]] std::vector<std::wstring_view> SplitLines(std::wstring_view source)
{
	std::vector<std::wstring_view> lines;
	std::size_t lineStart = 0;
	while (lineStart < source.size()) {
		auto lineEnd = source.find(L'\n', lineStart);
		if (lineEnd == std::wstring_view::npos) {
			lineEnd = source.size();
		}
		auto line = source.substr(lineStart, lineEnd - lineStart);
		if (!line.empty() && line.back() == L'\r') {
			line.remove_suffix(1);
		}
		lines.push_back(line);
		if (lineEnd == source.size()) {
			break;
		}
		lineStart = lineEnd + 1;
	}
	return lines;
}

} // namespace

Document ParseMarkdown(std::wstring_view source)
{
	Document document;
	const auto lines = SplitLines(source);

	for (std::size_t index = 0; index < lines.size();) {
		const auto line = lines[index];
		if (Trim(line).empty()) {
			++index;
			continue;
		}

		wchar_t fenceMarker = L'\0';
		std::size_t fenceMarkerCount = 0;
		if (IsFenceStart(line, &fenceMarker, &fenceMarkerCount)) {
			std::wstring code;
			bool firstLine = true;
			++index;
			while (index < lines.size() && !IsFenceClose(lines[index], fenceMarker, fenceMarkerCount)) {
				if (!firstLine) {
					code.push_back(L'\n');
				}
				code.append(lines[index]);
				firstLine = false;
				++index;
			}
			if (index < lines.size()) {
				++index;
			}
			document.blocks.push_back({ BlockKind::CodeBlock, 0, {}, std::move(code), {} });
			continue;
		}

		if (IsHorizontalRule(line)) {
			document.blocks.push_back({ BlockKind::HorizontalRule, 0, {}, {}, {} });
			++index;
			continue;
		}

		HeadingMatch heading;
		if (ParseHeading(line, &heading)) {
			document.blocks.push_back(MakeTextBlock(BlockKind::Heading, heading.text, heading.level));
			++index;
			continue;
		}

		auto quote = StripUpToThreeSpaces(line);
		if (!quote.empty() && quote.front() == L'>') {
			quote.remove_prefix(1);
			if (!quote.empty() && quote.front() == L' ') {
				quote.remove_prefix(1);
			}
			document.blocks.push_back(MakeTextBlock(BlockKind::BlockQuote, quote));
			++index;
			continue;
		}

		ListMatch list;
		if (ParseListItem(line, &list)) {
			document.blocks.push_back(MakeTextBlock(list.kind, list.text, list.level, std::move(list.marker)));
			++index;
			continue;
		}

		std::wstring paragraph;
		while (index < lines.size() && !Trim(lines[index]).empty() && !IsBlockStart(lines[index])) {
			if (!paragraph.empty()) {
				paragraph.push_back(L' ');
			}
			paragraph.append(Trim(lines[index]));
			++index;
		}
		if (!paragraph.empty()) {
			document.blocks.push_back(MakeTextBlock(BlockKind::Paragraph, paragraph));
		}
	}

	return document;
}

} // namespace markdown
