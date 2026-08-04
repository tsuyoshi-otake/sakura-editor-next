/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "MarkdownParser.h"
#include "util/CpuDispatch.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <utility>

namespace markdown {
namespace {

constexpr std::size_t kMaximumInlineDepth = 32;
constexpr std::size_t kInlineWorkPerCharacter = 64;
constexpr std::size_t kUtf16VectorThreshold = 64;

struct ParseContext {
	const ParseOptions& options;
	std::size_t imageReferences = 0;
	CpuDispatch::FindUtf16Function findMarkdownInlineSpecial = nullptr;
};

[[nodiscard]] bool CanDescendInline(const ParseContext& context, std::size_t depth) noexcept
{
	return depth < std::min(kMaximumInlineDepth, context.options.limits.maximumInlineDepth);
}

[[nodiscard]] bool IsSpace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t';
}

[[nodiscard]] bool IsAsciiAlpha(wchar_t value) noexcept
{
	return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z');
}

[[nodiscard]] bool IsAsciiAlphaNumeric(wchar_t value) noexcept
{
	return IsAsciiAlpha(value) || (value >= L'0' && value <= L'9');
}

enum class AngleAutolinkKind {
	None,
	Uri,
	Email,
};

[[nodiscard]] bool IsEmailLocalCharacter(wchar_t value) noexcept
{
	if (IsAsciiAlphaNumeric(value)) return true;
	return std::wstring_view(L".!#$%&'*+/=?^_`{|}~-").find(value) != std::wstring_view::npos;
}

[[nodiscard]] bool IsEmailDomainCharacter(wchar_t value) noexcept
{
	return IsAsciiAlphaNumeric(value) || value == L'-' || value == L'.' || value == L'_';
}

[[nodiscard]] bool IsValidEmailAddress(std::wstring_view candidate,
	bool requireDomainPeriod, bool allowDomainUnderscore) noexcept
{
	const auto at = candidate.find(L'@');
	if (at == std::wstring_view::npos || at == 0 || at + 1 >= candidate.size()
		|| candidate.find(L'@', at + 1) != std::wstring_view::npos) return false;
	for (std::size_t index = 0; index < at; ++index) {
		if (!IsEmailLocalCharacter(candidate[index])) return false;
	}
	bool foundPeriod = false;
	std::size_t labelStart = at + 1;
	for (std::size_t index = labelStart; index <= candidate.size(); ++index) {
		if (index < candidate.size() && candidate[index] != L'.') {
			if (!IsEmailDomainCharacter(candidate[index])
				|| (!allowDomainUnderscore && candidate[index] == L'_')) return false;
			continue;
		}
		if (index == labelStart || !IsAsciiAlphaNumeric(candidate[labelStart])
			|| !IsAsciiAlphaNumeric(candidate[index - 1])) return false;
		if (index < candidate.size()) foundPeriod = true;
		labelStart = index + 1;
	}
	return !requireDomainPeriod || foundPeriod;
}

[[nodiscard]] AngleAutolinkKind ClassifyAngleAutolink(std::wstring_view candidate) noexcept
{
	const auto colon = candidate.find(L':');
	if (colon >= 2 && colon <= 32 && IsAsciiAlpha(candidate.front())) {
		bool validScheme = true;
		for (std::size_t index = 1; index < colon; ++index) {
			if (!IsAsciiAlphaNumeric(candidate[index]) && candidate[index] != L'+'
				&& candidate[index] != L'.' && candidate[index] != L'-') {
				validScheme = false;
				break;
			}
		}
		bool safeBody = true;
		for (std::size_t index = colon + 1; index < candidate.size(); ++index) {
			const auto value = candidate[index];
			if (value <= static_cast<wchar_t>(0x20) || value == static_cast<wchar_t>(0x7f)
				|| value == L'<' || value == L'>') {
				safeBody = false;
				break;
			}
		}
		if (validScheme && safeBody) {
			return AngleAutolinkKind::Uri;
		}
	}
	return IsValidEmailAddress(candidate, false, false)
		? AngleAutolinkKind::Email : AngleAutolinkKind::None;
}

[[nodiscard]] bool IsMarkdownInlineSpecial(wchar_t value) noexcept
{
	switch (value) {
	case L'\\':
	case L'`':
	case L'!':
	case L'[':
	case L'*':
	case L'_':
	case L'~':
	case L'<':
	case L'&':
	case L'$':
		return true;
	default:
		return false;
	}
}

[[nodiscard]] std::size_t FindMarkdownInlineSpecial(
	std::wstring_view source, std::size_t start, CpuDispatch::FindUtf16Function vectorScan) noexcept
{
	const auto remaining = source.size() - start;
	if (remaining >= kUtf16VectorThreshold) {
		return start + vectorScan(source.data() + start, remaining);
	}
	for (std::size_t index = start; index < source.size(); ++index) {
		if (IsMarkdownInlineSpecial(source[index])) return index;
	}
	return source.size();
}

[[nodiscard]] std::size_t FindCrOrLf(std::wstring_view source, std::size_t start,
	CpuDispatch::FindUtf16Function vectorScan) noexcept
{
	const auto remaining = source.size() - start;
	if (remaining >= kUtf16VectorThreshold) {
		return start + vectorScan(source.data() + start, remaining);
	}
	for (std::size_t index = start; index < source.size(); ++index) {
		if (source[index] == L'\r' || source[index] == L'\n') return index;
	}
	return source.size();
}

[[nodiscard]] std::wstring_view TrimLeft(std::wstring_view value) noexcept
{
	while (!value.empty() && IsSpace(value.front())) value.remove_prefix(1);
	return value;
}

[[nodiscard]] std::wstring_view TrimRight(std::wstring_view value) noexcept
{
	while (!value.empty() && IsSpace(value.back())) value.remove_suffix(1);
	return value;
}

[[nodiscard]] std::wstring_view Trim(std::wstring_view value) noexcept
{
	return TrimRight(TrimLeft(value));
}

[[nodiscard]] std::wstring ToLowerAscii(std::wstring_view value)
{
	std::wstring result;
	result.reserve(value.size());
	for (const auto ch : value) {
		result.push_back(ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch);
	}
	return result;
}

[[nodiscard]] bool EqualsAsciiInsensitive(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		const auto lhs = left[index] >= L'A' && left[index] <= L'Z'
			? static_cast<wchar_t>(left[index] - L'A' + L'a') : left[index];
		const auto rhs = right[index] >= L'A' && right[index] <= L'Z'
			? static_cast<wchar_t>(right[index] - L'A' + L'a') : right[index];
		if (lhs != rhs) return false;
	}
	return true;
}

[[nodiscard]] std::wstring_view StripUpToThreeSpaces(std::wstring_view value) noexcept
{
	std::size_t count = 0;
	while (count < value.size() && count < 3 && value[count] == L' ') ++count;
	return value.substr(count);
}

[[nodiscard]] std::size_t CountIndent(std::wstring_view value) noexcept
{
	std::size_t count = 0;
	while (count < value.size() && IsSpace(value[count])) ++count;
	return count;
}

[[nodiscard]] bool IsHexDigit(wchar_t value) noexcept
{
	return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f')
		|| (value >= L'A' && value <= L'F');
}

[[nodiscard]] unsigned int HexValue(wchar_t value) noexcept
{
	if (value >= L'0' && value <= L'9') return static_cast<unsigned int>(value - L'0');
	if (value >= L'a' && value <= L'f') return static_cast<unsigned int>(value - L'a' + 10);
	return static_cast<unsigned int>(value - L'A' + 10);
}

void AppendCodePoint(std::wstring& output, unsigned int value)
{
	if (value == 0 || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
		output.push_back(L'\xfffd');
		return;
	}
#if WCHAR_MAX >= 0x10ffff
	output.push_back(static_cast<wchar_t>(value));
#else
	if (value <= 0xffffU) {
		output.push_back(static_cast<wchar_t>(value));
	} else {
		value -= 0x10000U;
		output.push_back(static_cast<wchar_t>(0xd800U + (value >> 10U)));
		output.push_back(static_cast<wchar_t>(0xdc00U + (value & 0x3ffU)));
	}
#endif
}

[[nodiscard]] bool DecodeEntityAt(std::wstring_view source, std::size_t index,
	std::wstring& output, std::size_t* consumed)
{
	if (index >= source.size() || source[index] != L'&') return false;
	const auto searchLength = std::min<std::size_t>(16, source.size() - index - 1);
	const auto relativeEnd = source.substr(index + 1, searchLength).find(L';');
	if (relativeEnd == std::wstring_view::npos) return false;
	const auto end = index + 1 + relativeEnd;
	const auto name = source.substr(index + 1, end - index - 1);
	if (name == L"amp") output.push_back(L'&');
	else if (name == L"lt") output.push_back(L'<');
	else if (name == L"gt") output.push_back(L'>');
	else if (name == L"quot") output.push_back(L'\"');
	else if (name == L"apos") output.push_back(L'\'');
	else if (name == L"nbsp") output.push_back(L' ');
	else if (name.size() >= 2 && name.front() == L'#') {
		const bool hexadecimal = name.size() >= 3 && (name[1] == L'x' || name[1] == L'X');
		const auto digits = name.substr(hexadecimal ? 2 : 1);
		if (digits.empty()) return false;
		unsigned int value = 0;
		for (const auto digit : digits) {
			if ((hexadecimal && !IsHexDigit(digit)) || (!hexadecimal && (digit < L'0' || digit > L'9'))) return false;
			const auto number = hexadecimal ? HexValue(digit) : static_cast<unsigned int>(digit - L'0');
			if (value > (0x10ffffU - number) / (hexadecimal ? 16U : 10U)) return false;
			value = value * (hexadecimal ? 16U : 10U) + number;
		}
		AppendCodePoint(output, value);
	} else {
		return false;
	}
	*consumed = end - index + 1;
	return true;
}

[[nodiscard]] std::wstring DecodeEntities(std::wstring_view source)
{
	std::wstring result;
	result.reserve(source.size());
	for (std::size_t index = 0; index < source.size();) {
		std::size_t consumed = 0;
		if (DecodeEntityAt(source, index, result, &consumed)) index += consumed;
		else result.push_back(source[index++]);
	}
	return result;
}

struct HtmlAttribute {
	std::wstring name;
	std::wstring value;
};

struct HtmlTag {
	std::wstring name;
	std::vector<HtmlAttribute> attributes;
	bool closing = false;
	bool selfClosing = false;
};

[[nodiscard]] bool ParseHtmlTag(std::wstring_view body, HtmlTag* tag)
{
	body = Trim(body);
	if (body.empty()) return false;
	if (body.front() == L'/') {
		tag->closing = true;
		body.remove_prefix(1);
		body = TrimLeft(body);
	}
	if (body.empty() || !IsAsciiAlpha(body.front())) return false;
	std::size_t nameEnd = 1;
	while (nameEnd < body.size() && (IsAsciiAlphaNumeric(body[nameEnd]) || body[nameEnd] == L'-')) ++nameEnd;
	tag->name = ToLowerAscii(body.substr(0, nameEnd));
	body.remove_prefix(nameEnd);
	while (!body.empty()) {
		body = TrimLeft(body);
		if (body.empty()) break;
		if (body.front() == L'/') {
			tag->selfClosing = true;
			body.remove_prefix(1);
			continue;
		}
		if (!IsAsciiAlpha(body.front())) {
			body.remove_prefix(1);
			continue;
		}
		std::size_t attributeEnd = 1;
		while (attributeEnd < body.size()
			&& (IsAsciiAlphaNumeric(body[attributeEnd]) || body[attributeEnd] == L'-' || body[attributeEnd] == L':')) {
			++attributeEnd;
		}
		HtmlAttribute attribute;
		attribute.name = ToLowerAscii(body.substr(0, attributeEnd));
		body.remove_prefix(attributeEnd);
		body = TrimLeft(body);
		if (!body.empty() && body.front() == L'=') {
			body.remove_prefix(1);
			body = TrimLeft(body);
			if (!body.empty() && (body.front() == L'\"' || body.front() == L'\'')) {
				const auto quote = body.front();
				body.remove_prefix(1);
				const auto valueEnd = body.find(quote);
				if (valueEnd == std::wstring_view::npos) {
					attribute.value.assign(body);
					body = {};
				} else {
					attribute.value.assign(body.substr(0, valueEnd));
					body.remove_prefix(valueEnd + 1);
				}
			} else {
				std::size_t valueEnd = 0;
				while (valueEnd < body.size() && !IsSpace(body[valueEnd]) && body[valueEnd] != L'/') ++valueEnd;
				attribute.value.assign(body.substr(0, valueEnd));
				body.remove_prefix(valueEnd);
			}
		}
		attribute.value = DecodeEntities(attribute.value);
		tag->attributes.push_back(std::move(attribute));
	}
	return true;
}

[[nodiscard]] std::wstring GetAttribute(const HtmlTag& tag, std::wstring_view name)
{
	for (const auto& attribute : tag.attributes) {
		if (EqualsAsciiInsensitive(attribute.name, name)) return attribute.value;
	}
	return {};
}

[[nodiscard]] bool IsDangerousHtmlContainer(std::wstring_view name) noexcept
{
	return name == L"script" || name == L"style" || name == L"iframe" || name == L"object"
		|| name == L"embed" || name == L"svg" || name == L"math" || name == L"form"
		|| name == L"video" || name == L"audio" || name == L"canvas";
}

[[nodiscard]] bool IsVoidHtmlTag(std::wstring_view name) noexcept
{
	return name == L"br" || name == L"hr" || name == L"img" || name == L"meta"
		|| name == L"link" || name == L"input" || name == L"source";
}

[[nodiscard]] std::wstring EscapeMarkdownLabel(std::wstring_view value)
{
	std::wstring result;
	for (const auto ch : value) {
		if (ch == L'\\' || ch == L'[' || ch == L']') result.push_back(L'\\');
		result.push_back(ch);
	}
	return result;
}

[[nodiscard]] std::wstring EscapeMarkdownDestination(std::wstring_view value)
{
	std::wstring result;
	for (const auto ch : value) {
		if (ch == L'\\' || ch == L'<') result.push_back(L'\\');
		if (ch == L'>') result.append(L"%3E");
		else result.push_back(ch);
	}
	return result;
}

struct HtmlFrame {
	HtmlTag tag;
	std::wstring content;
};

[[nodiscard]] bool IsFenceStart(std::wstring_view line, wchar_t* marker,
	std::size_t* markerCount, std::wstring_view* info = nullptr) noexcept;
[[nodiscard]] bool IsFenceClose(
	std::wstring_view line, wchar_t marker, std::size_t markerCount) noexcept;

[[nodiscard]] bool IsSourceLineStart(std::wstring_view source, std::size_t index) noexcept
{
	if (index == 0) return true;
	if (source[index - 1] == L'\n') return true;
	return source[index - 1] == L'\r' && (index >= source.size() || source[index] != L'\n');
}

[[nodiscard]] std::size_t MakeSanitizerCodeSearchBudget(std::size_t inputLength) noexcept
{
	constexpr std::size_t kWorkPerCharacter = 8;
	constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
	if (inputLength > (maximum - kWorkPerCharacter) / kWorkPerCharacter) return maximum;
	return (inputLength + 1) * kWorkPerCharacter;
}

[[nodiscard]] std::size_t FindClosingBacktickRun(std::wstring_view source, std::size_t start,
	std::size_t markerCount, std::size_t* remainingWork, bool* budgetExceeded) noexcept
{
	for (std::size_t index = start; index < source.size();) {
		if (*remainingWork == 0) {
			*budgetExceeded = true;
			return std::wstring_view::npos;
		}
		--*remainingWork;
		if (source[index] != L'`') {
			++index;
			continue;
		}
		const auto runStart = index;
		while (index < source.size() && source[index] == L'`') {
			if (*remainingWork == 0) {
				*budgetExceeded = true;
				return std::wstring_view::npos;
			}
			--*remainingWork;
			++index;
		}
		if (index - runStart == markerCount) return index;
	}
	return std::wstring_view::npos;
}

[[nodiscard]] std::wstring PrefixLines(std::wstring_view source, std::wstring_view prefix)
{
	std::wstring result;
	std::size_t start = 0;
	while (start <= source.size()) {
		const auto end = source.find(L'\n', start);
		const auto line = Trim(source.substr(start, end == std::wstring_view::npos ? source.size() - start : end - start));
		if (!line.empty()) {
			result.append(prefix);
			result.append(line);
		}
		result.push_back(L'\n');
		if (end == std::wstring_view::npos) break;
		start = end + 1;
	}
	return result;
}

[[nodiscard]] std::wstring RenderHtmlTable(std::wstring_view encoded)
{
	constexpr wchar_t kRow = L'\x001d';
	constexpr wchar_t kCell = L'\x001e';
	constexpr wchar_t kHeaderCell = L'\x001f';
	std::vector<std::vector<std::wstring>> rows;
	std::size_t rowStart = 0;
	while (rowStart < encoded.size()) {
		const auto rowEnd = encoded.find(kRow, rowStart);
		const auto row = encoded.substr(rowStart,
			rowEnd == std::wstring_view::npos ? encoded.size() - rowStart : rowEnd - rowStart);
		std::vector<std::wstring> cells;
		for (std::size_t index = 0; index < row.size();) {
			if (row[index] != kCell && row[index] != kHeaderCell) {
				++index;
				continue;
			}
			const auto cellStart = ++index;
			while (index < row.size() && row[index] != kCell && row[index] != kHeaderCell) ++index;
			cells.emplace_back(Trim(row.substr(cellStart, index - cellStart)));
		}
		if (!cells.empty()) {
			rows.push_back(std::move(cells));
		}
		if (rowEnd == std::wstring_view::npos) break;
		rowStart = rowEnd + 1;
	}
	if (rows.empty()) return {};
	std::wstring result(L"\n");
	auto appendRow = [&result](const std::vector<std::wstring>& row) {
		result.push_back(L'|');
		for (const auto& cell : row) {
			result.push_back(L' ');
			for (const auto ch : cell) {
				if (ch == L'|') result.push_back(L'\\');
				result.push_back(ch);
			}
			result.append(L" |");
		}
		result.push_back(L'\n');
	};
	appendRow(rows.front());
	result.push_back(L'|');
	for (std::size_t column = 0; column < rows.front().size(); ++column) result.append(L" --- |");
	result.push_back(L'\n');
	for (std::size_t row = 1; row < rows.size(); ++row) appendRow(rows[row]);
	result.push_back(L'\n');
	return result;
}

[[nodiscard]] std::wstring RenderHtmlFrame(const HtmlFrame& frame, std::wstring_view parentName)
{
	const auto& name = frame.tag.name;
	if (IsDangerousHtmlContainer(name)) return {};
	if (name == L"img") {
		const auto source = GetAttribute(frame.tag, L"src");
		if (source.empty()) return {};
		const auto alt = GetAttribute(frame.tag, L"alt");
		return L"\n![" + EscapeMarkdownLabel(alt) + L"](<" + EscapeMarkdownDestination(source) + L">)\n";
	}
	if (name == L"br") return L"\n";
	if (name == L"hr") return L"\n---\n";
	if (name == L"meta" || name == L"link" || name == L"input" || name == L"source") return {};
	if (name == L"strong" || name == L"b") return L"**" + frame.content + L"**";
	if (name == L"em" || name == L"i") return L"*" + frame.content + L"*";
	if (name == L"code") return parentName == L"pre" ? frame.content : L"`" + frame.content + L"`";
	if (name == L"pre") return L"\n```\n" + frame.content + L"\n```\n";
	if (name == L"a") {
		const auto href = GetAttribute(frame.tag, L"href");
		if (href.empty() || frame.content.find(L"![") != std::wstring::npos) return frame.content;
		return L"[" + frame.content + L"](<" + EscapeMarkdownDestination(href) + L">)";
	}
	if (name.size() == 2 && name.front() == L'h' && name[1] >= L'1' && name[1] <= L'6') {
		const std::wstring prefix(static_cast<std::size_t>(name[1] - L'0'), L'#');
		auto content = std::wstring_view(frame.content);
		while (!content.empty() && std::iswspace(content.front()) != 0) content.remove_prefix(1);
		while (!content.empty() && std::iswspace(content.back()) != 0) content.remove_suffix(1);
		// Keep HTML hero images as their own block. Prefixing the generated image
		// Markdown would turn it into heading text and prevent native projection.
		if (content.starts_with(L"![") && content.ends_with(L")")) {
			return L"\n" + std::wstring(content) + L"\n";
		}
		return L"\n" + PrefixLines(content, prefix + L" ") + L"\n";
	}
	if (name == L"blockquote") return L"\n" + PrefixLines(frame.content, L"> ") + L"\n";
	if (name == L"li") return L"\n" + std::wstring(parentName == L"ol" ? L"1. " : L"- ") + frame.content + L"\n";
	if (name == L"ul" || name == L"ol") return L"\n" + frame.content + L"\n";
	if (name == L"td") return std::wstring(1, L'\x001e') + frame.content;
	if (name == L"th") return std::wstring(1, L'\x001f') + frame.content;
	if (name == L"tr") return frame.content + std::wstring(1, L'\x001d');
	if (name == L"table") return RenderHtmlTable(frame.content);
	if (name == L"p" || name == L"div" || name == L"section" || name == L"article"
		|| name == L"header" || name == L"footer" || name == L"main" || name == L"center"
		|| name == L"figure" || name == L"figcaption" || name == L"details" || name == L"summary") {
		return L"\n" + frame.content + L"\n";
	}
	// Unknown wrappers are stripped. Their text remains inert and visible.
	return frame.content;
}

[[nodiscard]] std::wstring SanitizeHtmlToMarkdown(
	std::wstring_view source, std::size_t maximumDepth)
{
	HtmlFrame root;
	std::vector<HtmlFrame> stack;
	stack.push_back(std::move(root));
	std::size_t suppressedDepth = 0;
	std::wstring suppressedRootName;
	wchar_t fenceMarker = L'\0';
	std::size_t fenceMarkerCount = 0;
	std::size_t codeSearchBudget = MakeSanitizerCodeSearchBudget(source.size());
	for (std::size_t index = 0; index < source.size();) {
		// Markdown code is already inert source. Preserve it exactly so the HTML
		// projection cannot reinterpret literal tags inside code spans or fences.
		if (suppressedDepth == 0 && stack.size() == 1 && IsSourceLineStart(source, index)) {
			const auto lineEnd = FindCrOrLf(source, index, CpuDispatch::Get().findCrOrLfUtf16);
			const auto line = source.substr(index, lineEnd - index);
			std::size_t nextLine = lineEnd;
			if (nextLine < source.size()) {
				nextLine += source[nextLine] == L'\r' && nextLine + 1 < source.size()
					&& source[nextLine + 1] == L'\n' ? 2 : 1;
			}
			if (fenceMarker != L'\0') {
				stack.back().content.append(source.substr(index, nextLine - index));
				if (IsFenceClose(line, fenceMarker, fenceMarkerCount)) {
					fenceMarker = L'\0';
					fenceMarkerCount = 0;
				}
				index = nextLine;
				continue;
			}
			wchar_t candidateMarker = L'\0';
			std::size_t candidateCount = 0;
			std::wstring_view ignoredInfo;
			if (IsFenceStart(line, &candidateMarker, &candidateCount, &ignoredInfo)) {
				fenceMarker = candidateMarker;
				fenceMarkerCount = candidateCount;
				stack.back().content.append(source.substr(index, nextLine - index));
				index = nextLine;
				continue;
			}
		}
		if (suppressedDepth == 0 && stack.size() == 1 && source[index] == L'`') {
			std::size_t markerCount = 1;
			while (index + markerCount < source.size() && source[index + markerCount] == L'`') {
				++markerCount;
			}
			bool budgetExceeded = false;
			const auto codeEnd = FindClosingBacktickRun(source, index + markerCount,
				markerCount, &codeSearchBudget, &budgetExceeded);
			if (codeEnd != std::wstring_view::npos) {
				stack.back().content.append(source.substr(index, codeEnd - index));
				index = codeEnd;
				continue;
			}
			if (budgetExceeded) {
				// Literal projection is the fail-closed terminal when adversarial
				// unmatched delimiters exhaust the linear search allowance.
				stack.back().content.append(source.substr(index));
				index = source.size();
				continue;
			}
		}
		if (source.substr(index, std::min<std::size_t>(4, source.size() - index)) == L"<!--") {
			const auto end = source.find(L"-->", index + 4);
			index = end == std::wstring_view::npos ? source.size() : end + 3;
			continue;
		}
		if (suppressedDepth != 0) {
			if (source[index] != L'<') {
				++index;
				continue;
			}
			const auto suppressedClose = source.find(L'>', index + 1);
			if (suppressedClose == std::wstring_view::npos) break;
			HtmlTag suppressedTag;
			if (ParseHtmlTag(source.substr(index + 1, suppressedClose - index - 1), &suppressedTag)
				&& !suppressedTag.selfClosing && !IsVoidHtmlTag(suppressedTag.name)) {
				if (suppressedTag.closing) {
					if (suppressedDepth > 1 || suppressedTag.name == suppressedRootName) --suppressedDepth;
					if (suppressedDepth == 0) suppressedRootName.clear();
				}
				else ++suppressedDepth;
			}
			index = suppressedClose + 1;
			continue;
		}
		if (source[index] != L'<') {
			stack.back().content.push_back(source[index++]);
			continue;
		}
		const auto close = source.find(L'>', index + 1);
		if (close == std::wstring_view::npos) {
			// No later character can close an HTML tag, so the entire suffix is
			// inert literal text. Appending it once avoids quadratic rescans of a
			// hostile suffix containing many '<' characters.
			stack.back().content.append(source.substr(index));
			break;
		}
		const auto possibleAutolink = source.substr(index + 1, close - index - 1);
		if (ClassifyAngleAutolink(possibleAutolink) != AngleAutolinkKind::None) {
			stack.back().content.push_back(L'<');
			stack.back().content.append(possibleAutolink);
			stack.back().content.push_back(L'>');
			index = close + 1;
			continue;
		}
		HtmlTag tag;
		if (!ParseHtmlTag(source.substr(index + 1, close - index - 1), &tag)) {
			// Declarations and malformed tags are discarded instead of exposed as source.
			index = close + 1;
			continue;
		}
		index = close + 1;
		if (tag.closing) {
			std::size_t match = stack.size();
			for (std::size_t candidate = stack.size(); candidate > 1; --candidate) {
				if (stack[candidate - 1].tag.name == tag.name) {
					match = candidate - 1;
					break;
				}
			}
			if (match == stack.size()) continue;
			while (stack.size() - 1 >= match) {
				auto frame = std::move(stack.back());
				stack.pop_back();
				stack.back().content.append(RenderHtmlFrame(frame, stack.back().tag.name));
			}
			continue;
		}
		HtmlFrame frame;
		frame.tag = std::move(tag);
		if (frame.tag.selfClosing || IsVoidHtmlTag(frame.tag.name)) {
			stack.back().content.append(RenderHtmlFrame(frame, stack.back().tag.name));
		} else if (stack.size() - 1 >= maximumDepth) {
			// Drop the complete over-deep subtree. This keeps hostile nesting bounded
			// without ever exposing the contents of an over-deep dangerous element.
			suppressedDepth = 1;
			suppressedRootName = frame.tag.name;
		} else {
			stack.push_back(std::move(frame));
		}
	}
	while (stack.size() > 1) {
		auto frame = std::move(stack.back());
		stack.pop_back();
		stack.back().content.append(RenderHtmlFrame(frame, stack.back().tag.name));
	}
	return stack.front().content;
}

[[nodiscard]] std::wstring DecodePercentPath(std::wstring_view value)
{
	std::wstring result;
	result.reserve(value.size());
	for (std::size_t index = 0; index < value.size();) {
		if (value[index] == L'%' && index + 2 < value.size()
			&& IsHexDigit(value[index + 1]) && IsHexDigit(value[index + 2])) {
			const auto decoded = static_cast<wchar_t>((HexValue(value[index + 1]) << 4U) | HexValue(value[index + 2]));
			if (decoded == L'\0') return {};
			result.push_back(decoded);
			index += 3;
		} else {
			result.push_back(value[index++]);
		}
	}
	return result;
}

[[nodiscard]] std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
	// Parsing is deliberately I/O-free. The image loader must repeat containment
	// checks after opening the resource so reparse points cannot bypass this
	// lexical policy boundary.
	return path.lexically_normal();
}

[[nodiscard]] bool PathComponentEqual(const std::filesystem::path& left, const std::filesystem::path& right)
{
	return EqualsAsciiInsensitive(left.native(), right.native());
}

[[nodiscard]] bool IsPathInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	const auto normalizedRoot = NormalizePath(root);
	const auto normalizedCandidate = NormalizePath(candidate);
	auto rootPart = normalizedRoot.begin();
	auto candidatePart = normalizedCandidate.begin();
	for (; rootPart != normalizedRoot.end(); ++rootPart, ++candidatePart) {
		if (candidatePart == normalizedCandidate.end() || !PathComponentEqual(*rootPart, *candidatePart)) return false;
	}
	return true;
}

[[nodiscard]] ResourceReference ResolveResource(std::wstring_view rawTarget, ResourceUse use,
	ParseContext& context)
{
	ResourceReference result;
	result.use = use;
	result.original.assign(Trim(rawTarget));
	if (use == ResourceUse::Image) {
		if (context.imageReferences >= context.options.limits.maximumImages) {
			result.disposition = ResourceDisposition::LimitExceeded;
			return result;
		}
		++context.imageReferences;
	}
	const auto& options = context.options;
	auto target = Trim(rawTarget);
	if (target.size() >= 2 && target.front() == L'<' && target.back() == L'>') {
		target.remove_prefix(1);
		target.remove_suffix(1);
		target = Trim(target);
	}
	if (target.empty()) return result;
	if (target.front() == L'#') {
		result.disposition = ResourceDisposition::Fragment;
		return result;
	}
	if (target.starts_with(L"//")) {
		result.disposition = ResourceDisposition::ExternalBlocked;
		return result;
	}
	std::size_t schemeEnd = 0;
	if (IsAsciiAlpha(target.front())) {
		for (schemeEnd = 1; schemeEnd < target.size(); ++schemeEnd) {
			if (target[schemeEnd] == L':') break;
			if (!IsAsciiAlphaNumeric(target[schemeEnd]) && target[schemeEnd] != L'+' && target[schemeEnd] != L'-'
				&& target[schemeEnd] != L'.') {
				schemeEnd = 0;
				break;
			}
		}
		if (schemeEnd >= target.size() || target[schemeEnd] != L':') schemeEnd = 0;
	}
	const bool windowsDrive = schemeEnd == 1 && target.size() > 2
		&& (target[2] == L'\\' || target[2] == L'/');
	if (schemeEnd != 0 && !windowsDrive) {
		const auto scheme = ToLowerAscii(target.substr(0, schemeEnd));
		result.disposition = scheme == L"http" || scheme == L"https" || scheme == L"mailto"
			? ResourceDisposition::ExternalBlocked : ResourceDisposition::UnsafeSchemeBlocked;
		return result;
	}
	if (options.documentPath.empty()) return result;
	auto pathText = target;
	const auto suffix = pathText.find_first_of(L"?#");
	if (suffix != std::wstring_view::npos) pathText = pathText.substr(0, suffix);
	const auto decoded = DecodePercentPath(pathText);
	if (decoded.empty()) return result;
	const auto document = NormalizePath(std::filesystem::path(options.documentPath));
	const auto documentDirectory = document.parent_path();
	std::filesystem::path allowedRoot = documentDirectory;
	bool documentInWorkspace = false;
	if (!options.workspaceRoot.empty()) {
		const auto workspace = NormalizePath(std::filesystem::path(options.workspaceRoot));
		documentInWorkspace = IsPathInside(workspace, document);
		if (documentInWorkspace) allowedRoot = workspace;
	}
	std::filesystem::path requested(decoded);
	std::filesystem::path candidate;
	const bool rootRelative = !requested.has_root_name() && requested.has_root_directory();
	if (rootRelative) {
		if (documentInWorkspace) {
			candidate = allowedRoot / requested.relative_path();
		} else {
			result.disposition = ResourceDisposition::OutsideAllowedRoots;
			return result;
		}
	} else if (requested.is_absolute()) {
		candidate = requested;
	} else {
		candidate = documentDirectory / requested;
	}
	candidate = NormalizePath(candidate);
	if (!IsPathInside(allowedRoot, candidate)) {
		result.disposition = ResourceDisposition::OutsideAllowedRoots;
		return result;
	}
	result.disposition = ResourceDisposition::ResolvedLocal;
	result.resolvedPath = candidate.native();
	result.allowedRoot = NormalizePath(allowedRoot).native();
	return result;
}

struct ParsedText {
	std::wstring text;
	std::vector<InlineSpan> spans;
	bool limitExceeded = false;
};

struct InlineWorkBudget {
	std::size_t remaining = 0;
	bool exceeded = false;

	[[nodiscard]] bool Consume(std::size_t count = 1) noexcept
	{
		if (count > remaining) {
			remaining = 0;
			exceeded = true;
			return false;
		}
		remaining -= count;
		return true;
	}
};

[[nodiscard]] std::size_t MakeInlineWorkBudget(std::size_t inputLength) noexcept
{
	constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
	if (inputLength > (maximum - kInlineWorkPerCharacter) / kInlineWorkPerCharacter) return maximum;
	return (inputLength + 1) * kInlineWorkPerCharacter;
}

[[nodiscard]] std::size_t FindMarkerRun(std::wstring_view source, wchar_t marker,
	std::size_t markerCount, std::size_t start, InlineWorkBudget& budget,
	bool exactLength = false) noexcept
{
	std::size_t index = start;
	while (index < source.size()) {
		if (!budget.Consume()) return std::wstring_view::npos;
		if (source[index] != marker) {
			++index;
			continue;
		}
		const auto runStart = index;
		while (index < source.size() && source[index] == marker) {
			if (!budget.Consume()) return std::wstring_view::npos;
			++index;
		}
		const auto runLength = index - runStart;
		if ((exactLength && runLength == markerCount) || (!exactLength && runLength >= markerCount)) {
			return runStart;
		}
	}
	return std::wstring_view::npos;
}

[[nodiscard]] bool ParseDestination(std::wstring_view source, std::size_t openParen,
	std::size_t* closingParen, std::wstring_view* destination, InlineWorkBudget& budget)
{
	if (openParen >= source.size() || source[openParen] != L'(') return false;
	std::size_t index = openParen + 1;
	while (index < source.size() && IsSpace(source[index])) {
		if (!budget.Consume()) return false;
		++index;
	}
	if (index >= source.size()) return false;
	if (source[index] == L'<') {
		std::size_t end = index + 1;
		while (end < source.size() && source[end] != L'>') {
			if (!budget.Consume()) return false;
			++end;
		}
		if (end >= source.size()) return false;
		*destination = source.substr(index + 1, end - index - 1);
		index = end + 1;
	} else {
		const auto start = index;
		int nesting = 0;
		while (index < source.size()) {
			if (!budget.Consume()) return false;
			if (source[index] == L'\\' && index + 1 < source.size()) {
				index += 2;
				continue;
			}
			if (source[index] == L'(') ++nesting;
			else if (source[index] == L')') {
				if (nesting == 0) break;
				--nesting;
			}
			if (nesting == 0 && IsSpace(source[index])) break;
			++index;
		}
		if (index == start) return false;
		*destination = source.substr(start, index - start);
	}
	while (index < source.size() && IsSpace(source[index])) {
		if (!budget.Consume()) return false;
		++index;
	}
	if (index < source.size() && (source[index] == L'\"' || source[index] == L'\'')) {
		const auto quote = source[index++];
		while (index < source.size() && source[index] != quote) {
			if (!budget.Consume()) return false;
			if (source[index] == L'\\' && index + 1 < source.size()) ++index;
			++index;
		}
		if (index >= source.size()) return false;
		++index;
		while (index < source.size() && IsSpace(source[index])) {
			if (!budget.Consume()) return false;
			++index;
		}
	}
	if (index >= source.size() || source[index] != L')') return false;
	*closingParen = index;
	return true;
}

[[nodiscard]] std::size_t FindClosingBracket(
	std::wstring_view source, std::size_t start, InlineWorkBudget& budget)
{
	for (std::size_t index = start; index < source.size(); ++index) {
		if (!budget.Consume()) return std::wstring_view::npos;
		if (source[index] == L'\\' && index + 1 < source.size()) {
			++index;
			continue;
		}
		if (source[index] == L']') return index;
	}
	return std::wstring_view::npos;
}

void AppendNestedText(ParsedText& result, ParsedText nested, std::size_t start)
{
	result.text.append(nested.text);
	for (auto& span : nested.spans) {
		span.start += start;
		result.spans.push_back(std::move(span));
	}
	result.limitExceeded = result.limitExceeded || nested.limitExceeded;
}

[[nodiscard]] bool MakeAngleAutolinkTarget(std::wstring_view candidate,
	std::wstring* target, AngleAutolinkKind* kind)
{
	*kind = ClassifyAngleAutolink(candidate);
	if (*kind == AngleAutolinkKind::Uri) {
		target->assign(candidate);
		return true;
	}
	if (*kind != AngleAutolinkKind::Email) return false;
	target->assign(L"mailto:");
	target->append(candidate);
	return true;
}

[[nodiscard]] ParsedText ParseInlineInternal(std::wstring_view source, ParseContext& context,
	std::size_t depth, InlineWorkBudget& budget)
{
	ParsedText result;
	result.text.reserve(source.size());
	for (std::size_t index = 0; index < source.size();) {
		if (budget.exceeded) {
			result.text.append(source.substr(index));
			result.limitExceeded = true;
			break;
		}
		const auto special = FindMarkdownInlineSpecial(source, index, context.findMarkdownInlineSpecial);
		if (special > index) {
			const auto count = special - index;
			if (!budget.Consume(count)) continue;
			result.text.append(source.substr(index, count));
			index = special;
			continue;
		}
		if (source[index] == L'\\' && index + 1 < source.size()
			&& source[index + 1] >= L'!' && source[index + 1] <= L'~'
			&& !IsAsciiAlphaNumeric(source[index + 1])) {
			if (!budget.Consume(2)) continue;
			result.text.push_back(source[index + 1]);
			index += 2;
			continue;
		}
		if (source[index] == L'`') {
			std::size_t markerCount = 1;
			while (index + markerCount < source.size() && source[index + markerCount] == L'`') {
				if (!budget.Consume()) break;
				++markerCount;
			}
			const auto close = FindMarkerRun(source, L'`', markerCount, index + markerCount, budget);
			if (close != std::wstring_view::npos) {
				const auto start = result.text.size();
				auto code = source.substr(index + markerCount, close - index - markerCount);
				if (code.size() >= 2 && code.front() == L' ' && code.back() == L' ') {
					code.remove_prefix(1);
					code.remove_suffix(1);
				}
				result.text.append(code);
				result.spans.push_back({ InlineKind::Code, start, result.text.size() - start, std::nullopt });
				index = close + markerCount;
				continue;
			}
		}
		const bool image = source[index] == L'!' && index + 1 < source.size() && source[index + 1] == L'[';
		if (image || source[index] == L'[') {
			const auto labelStart = index + (image ? 2 : 1);
			const auto labelEnd = FindClosingBracket(source, labelStart, budget);
			std::size_t destinationEnd = 0;
			std::wstring_view destination;
			if (labelEnd != std::wstring_view::npos && labelEnd + 1 < source.size()
				&& ParseDestination(source, labelEnd + 1, &destinationEnd, &destination, budget)) {
				const auto start = result.text.size();
				auto label = CanDescendInline(context, depth)
					? ParseInlineInternal(source.substr(labelStart, labelEnd - labelStart), context, depth + 1, budget)
					: ParsedText{ std::wstring(source.substr(labelStart, labelEnd - labelStart)), {}, true };
				AppendNestedText(result, std::move(label), start);
				const auto kind = image ? InlineKind::Image : InlineKind::Link;
				const auto use = image ? ResourceUse::Image : ResourceUse::Link;
				result.spans.push_back({ kind, start, result.text.size() - start,
					ResolveResource(destination, use, context) });
				index = destinationEnd + 1;
				continue;
			}
		}
		if (source[index] == L'<') {
			std::size_t close = index + 1;
			while (close < source.size() && source[close] != L'>') {
				if (!budget.Consume()) break;
				++close;
			}
			if (!budget.exceeded && close < source.size()) {
				const auto candidate = source.substr(index + 1, close - index - 1);
				std::wstring target;
				AngleAutolinkKind autolinkKind = AngleAutolinkKind::None;
				if (MakeAngleAutolinkTarget(candidate, &target, &autolinkKind)) {
					const auto display = candidate;
					const auto start = result.text.size();
					result.text.append(display);
					result.spans.push_back({ InlineKind::Autolink, start, display.size(),
						ResolveResource(target, ResourceUse::Link, context) });
					index = close + 1;
					continue;
				}
			}
		}
		if ((source[index] == L'~' || source[index] == L'*' || source[index] == L'_')
			&& !CanDescendInline(context, depth)) result.limitExceeded = true;
		if (source[index] == L'~' && (index == 0 || source[index - 1] != L'~')
			&& CanDescendInline(context, depth)) {
			std::size_t count = 1;
			while (index + count < source.size() && source[index + count] == L'~') ++count;
			if (count <= 2) {
				const auto close = FindMarkerRun(source, L'~', count, index + count, budget, true);
				if (close != std::wstring_view::npos && close > index + count
					&& !IsSpace(source[index + count]) && !IsSpace(source[close - 1])) {
					const auto start = result.text.size();
					auto content = ParseInlineInternal(source.substr(index + count, close - index - count),
						context, depth + 1, budget);
					AppendNestedText(result, std::move(content), start);
					result.spans.push_back({ InlineKind::Strikethrough,
						start, result.text.size() - start, std::nullopt });
					index = close + count;
					continue;
				}
			}
		}
		if (source[index] == L'$') {
			const std::size_t count = index + 1 < source.size() && source[index + 1] == L'$' ? 2 : 1;
			const auto close = FindMarkerRun(source, L'$', count, index + count, budget);
			if (close != std::wstring_view::npos && close > index + count
				&& !IsSpace(source[index + count]) && !IsSpace(source[close - 1])) {
				const auto start = result.text.size();
				result.text.append(source.substr(index + count, close - index - count));
				result.spans.push_back({ InlineKind::Math, start,
					result.text.size() - start, std::nullopt });
				index = close + count;
				continue;
			}
		}
		if ((source[index] == L'*' || source[index] == L'_') && CanDescendInline(context, depth)) {
			const auto marker = source[index];
			const std::size_t count = index + 1 < source.size() && source[index + 1] == marker ? 2 : 1;
			const auto close = FindMarkerRun(source, marker, count, index + count, budget);
			if (close != std::wstring_view::npos && close > index + count) {
				const auto start = result.text.size();
				auto content = ParseInlineInternal(source.substr(index + count, close - index - count),
					context, depth + 1, budget);
				AppendNestedText(result, std::move(content), start);
				result.spans.push_back({ count == 2 ? InlineKind::Strong : InlineKind::Emphasis,
					start, result.text.size() - start, std::nullopt });
				index = close + count;
				continue;
			}
		}
		std::size_t consumed = 0;
		if (DecodeEntityAt(source, index, result.text, &consumed)) {
			(void)budget.Consume(consumed);
			index += consumed;
		} else {
			(void)budget.Consume();
			result.text.push_back(source[index++]);
		}
	}
	result.limitExceeded = result.limitExceeded || budget.exceeded;
	return result;
}

[[nodiscard]] bool IsExtendedUrlBoundary(wchar_t value) noexcept
{
	return std::iswspace(value) != 0 || value == L'*' || value == L'_'
		|| value == L'~' || value == L'(';
}

[[nodiscard]] bool IsValidExtendedDomain(std::wstring_view domain) noexcept
{
	std::size_t periods = 0;
	std::size_t lastPeriod = std::wstring_view::npos;
	std::size_t previousPeriod = std::wstring_view::npos;
	std::size_t segmentStart = 0;
	for (std::size_t index = 0; index <= domain.size(); ++index) {
		if (index < domain.size() && domain[index] != L'.') {
			if (!IsAsciiAlphaNumeric(domain[index]) && domain[index] != L'_'
				&& domain[index] != L'-') return false;
			continue;
		}
		if (index == segmentStart) return false;
		if (index < domain.size()) {
			++periods;
			previousPeriod = lastPeriod;
			lastPeriod = index;
		}
		segmentStart = index + 1;
	}
	if (periods == 0) return false;
	const auto penultimateStart = previousPeriod == std::wstring_view::npos ? 0 : previousPeriod + 1;
	for (std::size_t index = penultimateStart; index < domain.size(); ++index) {
		if (domain[index] == L'_') return false;
	}
	return true;
}

[[nodiscard]] std::size_t TrimExtendedUrlEnd(
	std::wstring_view text, std::size_t start, std::size_t end) noexcept
{
	while (end > start && std::wstring_view(L"?!.,:*_~").find(text[end - 1]) != std::wstring_view::npos) --end;
	std::size_t opening = 0;
	std::size_t closing = 0;
	for (std::size_t index = start; index < end; ++index) {
		if (text[index] == L'(') ++opening;
		else if (text[index] == L')') ++closing;
	}
	while (end > start && text[end - 1] == L')' && closing > opening) {
		--end;
		--closing;
	}
	return end;
}

[[nodiscard]] bool ParseExtendedUrl(std::wstring_view text, std::size_t start,
	std::wstring_view prefix, bool insertHttpScheme, std::size_t* end, std::wstring* target)
{
	if (!text.substr(start).starts_with(prefix)) return false;
	const auto domainStart = start + prefix.size();
	std::size_t domainEnd = domainStart;
	while (domainEnd < text.size() && (IsAsciiAlphaNumeric(text[domainEnd])
		|| text[domainEnd] == L'_' || text[domainEnd] == L'-' || text[domainEnd] == L'.')) ++domainEnd;
	if (!IsValidExtendedDomain(text.substr(domainStart, domainEnd - domainStart))) return false;
	std::size_t candidateEnd = domainEnd;
	while (candidateEnd < text.size() && std::iswspace(text[candidateEnd]) == 0
		&& text[candidateEnd] != L'<') ++candidateEnd;
	candidateEnd = TrimExtendedUrlEnd(text, start, candidateEnd);
	if (candidateEnd <= domainEnd) candidateEnd = domainEnd;
	*end = candidateEnd;
	target->clear();
	if (insertHttpScheme) target->append(L"http://");
	target->append(text.substr(start, candidateEnd - start));
	return true;
}

[[nodiscard]] bool ParseExtendedEmail(std::wstring_view text, std::size_t start,
	std::wstring_view protocol, std::size_t* end, std::wstring* target)
{
	const auto addressStart = start + protocol.size();
	if (!protocol.empty() && !text.substr(start).starts_with(protocol)) return false;
	if (addressStart >= text.size() || !IsEmailLocalCharacter(text[addressStart])) return false;
	std::size_t at = addressStart;
	while (at < text.size() && IsEmailLocalCharacter(text[at]) && text[at] != L'@') ++at;
	if (at >= text.size() || text[at] != L'@') return false;
	std::size_t candidateEnd = at + 1;
	while (candidateEnd < text.size() && IsEmailDomainCharacter(text[candidateEnd])) ++candidateEnd;
	while (candidateEnd > at + 1 && text[candidateEnd - 1] == L'.') --candidateEnd;
	const auto address = text.substr(addressStart, candidateEnd - addressStart);
	if (!IsValidEmailAddress(address, true, true)) return false;
	*end = candidateEnd;
	target->clear();
	if (protocol.empty()) target->append(L"mailto:");
	target->append(text.substr(start, candidateEnd - start));
	return true;
}

void AddBareAutolinks(ParsedText& result, ParseContext& context)
{
	const std::wstring_view text(result.text);
	std::vector<const InlineSpan*> protectedSpans;
	protectedSpans.reserve(result.spans.size());
	for (const auto& span : result.spans) {
		if (span.kind == InlineKind::Code || span.kind == InlineKind::Link
			|| span.kind == InlineKind::Image || span.kind == InlineKind::Autolink
			|| span.kind == InlineKind::Math) protectedSpans.push_back(&span);
	}
	std::vector<InlineSpan> autolinks;
	std::size_t protectedIndex = 0;
	for (std::size_t index = 0; index < text.size();) {
		const bool urlBoundary = index == 0 || IsExtendedUrlBoundary(text[index - 1]);
		std::size_t end = index;
		std::wstring target;
		bool matched = false;
		if (urlBoundary) {
			matched = ParseExtendedUrl(text, index, L"http://", false, &end, &target)
				|| ParseExtendedUrl(text, index, L"https://", false, &end, &target)
				|| ParseExtendedUrl(text, index, L"www.", true, &end, &target)
				|| ParseExtendedEmail(text, index, L"mailto:", &end, &target)
				|| ParseExtendedEmail(text, index, L"xmpp:", &end, &target);
		}
		if (!matched && IsEmailLocalCharacter(text[index])
			&& (index == 0 || !IsEmailLocalCharacter(text[index - 1]))) {
			matched = ParseExtendedEmail(text, index, {}, &end, &target);
		}
		if (!matched || end <= index) {
			++index;
			continue;
		}
		while (protectedIndex < protectedSpans.size()
			&& protectedSpans[protectedIndex]->start + protectedSpans[protectedIndex]->length <= index) ++protectedIndex;
		bool overlaps = false;
		for (auto candidate = protectedIndex; candidate < protectedSpans.size()
			&& protectedSpans[candidate]->start < end; ++candidate) {
			if (protectedSpans[candidate]->start + protectedSpans[candidate]->length > index) {
				overlaps = true;
				break;
			}
		}
		if (!overlaps) {
			autolinks.push_back({ InlineKind::Autolink, index, end - index,
				ResolveResource(target, ResourceUse::Link, context) });
		}
		index = end;
	}
	if (autolinks.empty()) return;
	std::vector<InlineSpan> merged;
	merged.reserve(result.spans.size() + autolinks.size());
	std::size_t existing = 0;
	std::size_t added = 0;
	while (existing < result.spans.size() || added < autolinks.size()) {
		if (added == autolinks.size()
			|| (existing < result.spans.size() && result.spans[existing].start <= autolinks[added].start)) {
			merged.push_back(std::move(result.spans[existing++]));
		} else {
			merged.push_back(std::move(autolinks[added++]));
		}
	}
	result.spans = std::move(merged);
}

[[nodiscard]] ParsedText ParseInline(std::wstring_view source, ParseContext& context)
{
	InlineWorkBudget budget{ MakeInlineWorkBudget(source.size()) };
	auto result = ParseInlineInternal(source, context, 0, budget);
	AddBareAutolinks(result, context);
	return result;
}

[[nodiscard]] Block MakeTextBlock(BlockKind kind, std::wstring_view source, ParseContext& context,
	int level = 0, std::wstring marker = {}, std::size_t sourceLine = 0)
{
	auto parsed = ParseInline(source, context);
	Block result;
	result.kind = kind;
	result.level = level;
	result.marker = std::move(marker);
	result.text = std::move(parsed.text);
	result.inlineSpans = std::move(parsed.spans);
	if (parsed.limitExceeded) result.fallbackKind = NativeFallbackKind::LimitExceeded;
	result.sourceLine = sourceLine;
	return result;
}

[[nodiscard]] bool IsFenceStart(std::wstring_view line, wchar_t* marker,
	std::size_t* markerCount, std::wstring_view* info) noexcept
{
	line = StripUpToThreeSpaces(line);
	if (line.empty() || (line.front() != L'`' && line.front() != L'~')) return false;
	const wchar_t candidate = line.front();
	std::size_t count = 0;
	while (count < line.size() && line[count] == candidate) ++count;
	if (count < 3) return false;
	const auto candidateInfo = Trim(line.substr(count));
	if (candidate == L'`' && candidateInfo.find(L'`') != std::wstring_view::npos) return false;
	*marker = candidate;
	*markerCount = count;
	if (info != nullptr) *info = candidateInfo;
	return true;
}

[[nodiscard]] std::wstring NormalizeFenceLanguage(std::wstring_view info)
{
	info = Trim(info);
	const auto end = info.find_first_of(L" \t");
	return ToLowerAscii(info.substr(0, end));
}

[[nodiscard]] bool IsFenceClose(std::wstring_view line, wchar_t marker, std::size_t markerCount) noexcept
{
	line = StripUpToThreeSpaces(line);
	std::size_t count = 0;
	while (count < line.size() && line[count] == marker) ++count;
	return count >= markerCount && Trim(line.substr(count)).empty();
}

[[nodiscard]] bool IsHorizontalRule(std::wstring_view line) noexcept
{
	line = Trim(line);
	wchar_t marker = L'\0';
	std::size_t count = 0;
	for (const auto value : line) {
		if (IsSpace(value)) continue;
		if (value != L'-' && value != L'*' && value != L'_') return false;
		if (marker == L'\0') marker = value;
		if (marker != value) return false;
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
	while (hashes < line.size() && hashes < 6 && line[hashes] == L'#') ++hashes;
	if (hashes == 0 || (hashes < line.size() && !IsSpace(line[hashes]))) return false;
	auto content = Trim(line.substr(hashes));
	while (!content.empty() && content.back() == L'#') content.remove_suffix(1);
	match->level = static_cast<int>(hashes);
	match->text = TrimRight(content);
	return true;
}

[[nodiscard]] int ParseSetextLevel(std::wstring_view line) noexcept
{
	line = Trim(line);
	if (line.empty()) return 0;
	const auto marker = line.front();
	if (marker != L'=' && marker != L'-') return 0;
	for (const auto ch : line) if (ch != marker && !IsSpace(ch)) return 0;
	return line.size() >= 3 ? (marker == L'=' ? 1 : 2) : 0;
}

struct ListMatch {
	BlockKind kind = BlockKind::Paragraph;
	int level = 0;
	std::wstring marker;
	std::wstring_view text;
	TaskListState taskListState = TaskListState::NotTask;
};

void ParseTaskListMarker(ListMatch* match) noexcept
{
	if (match->text.size() < 3 || match->text[0] != L'[' || match->text[2] != L']'
		|| (match->text.size() > 3 && !IsSpace(match->text[3]))) return;
	if (match->text[1] == L' ') match->taskListState = TaskListState::Unchecked;
	else if (match->text[1] == L'x' || match->text[1] == L'X') match->taskListState = TaskListState::Checked;
	else return;
	match->text = TrimLeft(match->text.substr(3));
}

[[nodiscard]] bool ParseListItem(std::wstring_view line, ListMatch* match)
{
	const auto indent = CountIndent(line);
	line.remove_prefix(indent);
	if (line.size() >= 2 && (line[0] == L'-' || line[0] == L'*' || line[0] == L'+') && IsSpace(line[1])) {
		match->kind = BlockKind::BulletListItem;
		match->level = static_cast<int>(indent / 2);
		match->marker = L"\x2022 ";
		match->text = TrimLeft(line.substr(2));
		ParseTaskListMarker(match);
		return true;
	}
	std::size_t numberEnd = 0;
	while (numberEnd < line.size() && line[numberEnd] >= L'0' && line[numberEnd] <= L'9') ++numberEnd;
	if (numberEnd == 0 || numberEnd + 1 >= line.size()
		|| (line[numberEnd] != L'.' && line[numberEnd] != L')') || !IsSpace(line[numberEnd + 1])) return false;
	match->kind = BlockKind::OrderedListItem;
	match->level = static_cast<int>(indent / 2);
	match->marker.assign(line.substr(0, numberEnd + 1));
	match->marker.push_back(L' ');
	match->text = TrimLeft(line.substr(numberEnd + 2));
	ParseTaskListMarker(match);
	return true;
}

[[nodiscard]] std::vector<std::wstring_view> SplitLines(std::wstring_view source)
{
	std::vector<std::wstring_view> lines;
	const auto vectorScan = CpuDispatch::Get().findCrOrLfUtf16;
	std::size_t lineStart = 0;
	while (lineStart < source.size()) {
		const auto lineEnd = FindCrOrLf(source, lineStart, vectorScan);
		lines.push_back(source.substr(lineStart, lineEnd - lineStart));
		if (lineEnd == source.size()) break;
		lineStart = lineEnd + (source[lineEnd] == L'\r' && lineEnd + 1 < source.size()
			&& source[lineEnd + 1] == L'\n' ? 2 : 1);
	}
	return lines;
}

[[nodiscard]] std::vector<std::wstring> SplitTableCells(std::wstring_view line)
{
	line = Trim(line);
	if (!line.empty() && line.front() == L'|') line.remove_prefix(1);
	if (!line.empty() && line.back() == L'|') line.remove_suffix(1);
	std::vector<std::wstring> cells;
	std::wstring cell;
	bool escaped = false;
	std::size_t codeTicks = 0;
	for (std::size_t index = 0; index < line.size(); ++index) {
		const auto ch = line[index];
		if (escaped) {
			cell.push_back(ch);
			escaped = false;
			continue;
		}
		if (ch == L'\\') {
			escaped = true;
			cell.push_back(ch);
			continue;
		}
		if (ch == L'`') {
			codeTicks = codeTicks == 0 ? 1 : 0;
			cell.push_back(ch);
			continue;
		}
		if (ch == L'|' && codeTicks == 0) {
			cells.emplace_back(Trim(cell));
			cell.clear();
		} else {
			cell.push_back(ch);
		}
	}
	if (escaped) cell.push_back(L'\\');
	cells.emplace_back(Trim(cell));
	return cells;
}

[[nodiscard]] bool ParseTableDelimiter(std::wstring_view line, std::vector<TableAlignment>* alignments)
{
	const auto cells = SplitTableCells(line);
	if (cells.empty()) return false;
	std::vector<TableAlignment> parsed;
	for (const auto& rawCell : cells) {
		auto cell = Trim(rawCell);
		const bool left = !cell.empty() && cell.front() == L':';
		const bool right = !cell.empty() && cell.back() == L':';
		if (left) cell.remove_prefix(1);
		if (right && !cell.empty()) cell.remove_suffix(1);
		cell = Trim(cell);
		if (cell.size() < 3 || cell.find_first_not_of(L'-') != std::wstring_view::npos) return false;
		parsed.push_back(left && right ? TableAlignment::Center
			: (right ? TableAlignment::Right : (left ? TableAlignment::Left : TableAlignment::Default)));
	}
	*alignments = std::move(parsed);
	return true;
}

[[nodiscard]] TableRow MakeTableRow(std::wstring_view line, bool header, ParseContext& context)
{
	TableRow row;
	row.header = header;
	for (const auto& source : SplitTableCells(line)) {
		auto parsed = ParseInline(source, context);
		row.cells.push_back({ std::move(parsed.text), std::move(parsed.spans) });
	}
	return row;
}

[[nodiscard]] bool TryMakeStandaloneImage(std::wstring_view line, ParseContext& context,
	std::size_t sourceLine, Block* block)
{
	line = Trim(line);
	if (!line.starts_with(L"![")) return false;
	InlineWorkBudget budget{ MakeInlineWorkBudget(line.size()) };
	const auto labelEnd = FindClosingBracket(line, 2, budget);
	if (labelEnd == std::wstring_view::npos || labelEnd + 1 >= line.size()) return false;
	std::size_t destinationEnd = 0;
	std::wstring_view destination;
	if (!ParseDestination(line, labelEnd + 1, &destinationEnd, &destination, budget)
		|| !Trim(line.substr(destinationEnd + 1)).empty()) return false;
	block->kind = BlockKind::Image;
	block->image = ImageNode{ DecodeEntities(line.substr(2, labelEnd - 2)),
		ResolveResource(destination, ResourceUse::Image, context) };
	if (budget.exceeded) block->fallbackKind = NativeFallbackKind::LimitExceeded;
	block->sourceLine = sourceLine;
	return true;
}

[[nodiscard]] bool IsMathBlockLine(std::wstring_view line) noexcept
{
	line = Trim(line);
	return line == L"$$" || (line.size() > 4 && line.starts_with(L"$$") && line.ends_with(L"$$"));
}

[[nodiscard]] bool IsMermaidContainerStart(std::wstring_view line) noexcept
{
	return Trim(line) == L":::mermaid";
}

[[nodiscard]] bool IsBlockStart(std::wstring_view line)
{
	wchar_t marker = L'\0';
	std::size_t markerCount = 0;
	if (IsFenceStart(line, &marker, &markerCount) || IsHorizontalRule(line)
		|| IsMathBlockLine(line) || IsMermaidContainerStart(line)) return true;
	HeadingMatch heading;
	if (ParseHeading(line, &heading)) return true;
	line = StripUpToThreeSpaces(line);
	if (!line.empty() && (line.front() == L'>' || line.starts_with(L"!["))) return true;
	ListMatch list;
	return ParseListItem(line, &list);
}

[[nodiscard]] std::wstring JoinLines(const std::vector<std::wstring_view>& lines,
	std::size_t begin, std::size_t end)
{
	std::wstring result;
	for (std::size_t index = begin; index < end; ++index) {
		if (index != begin) result.push_back(L'\n');
		result.append(lines[index]);
	}
	return result;
}

[[nodiscard]] bool HasUnsupportedFrontMatterSyntax(std::wstring_view name, std::wstring_view value) noexcept
{
	name = Trim(name);
	value = Trim(value);
	auto hasIndicator = [](std::wstring_view text, wchar_t indicator) noexcept {
		for (std::size_t index = 0; index < text.size(); ++index) {
			if (text[index] == indicator && (index == 0 || IsSpace(text[index - 1]))) return true;
		}
		return false;
	};
	if (name == L"<<" || name.starts_with(L"!") || name.starts_with(L"&") || name.starts_with(L"*")
		|| name.starts_with(L"%") || value.starts_with(L"!") || value.starts_with(L"&")
		|| value.starts_with(L"*") || value.starts_with(L"[") || value.starts_with(L"{")
		|| value.starts_with(L"|") || value.starts_with(L">")
		|| hasIndicator(value, L'!') || hasIndicator(value, L'&') || hasIndicator(value, L'*')) return true;
	return false;
}

void ParseFrontMatterFields(std::wstring_view raw, const ParseOptions& options, Block* block)
{
	const auto lines = SplitLines(raw);
	for (const auto sourceLine : lines) {
		auto line = TrimRight(sourceLine);
		auto content = TrimLeft(line);
		if (content.empty() || content.front() == L'#') continue;
		if (block->frontMatterFields.size() >= options.limits.maximumFrontMatterFields) {
			block->fallbackKind = NativeFallbackKind::LimitExceeded;
			return;
		}
		const auto indentation = line.size() - content.size();
		if (content.starts_with(L"-")) {
			if (content.size() > 1 && !IsSpace(content[1])) {
				block->fallbackKind = NativeFallbackKind::UnsupportedSyntax;
				continue;
			}
			auto value = Trim(content.substr(1));
			if (HasUnsupportedFrontMatterSyntax(L"-", value)) {
				block->fallbackKind = NativeFallbackKind::UnsupportedSyntax;
				continue;
			}
			block->frontMatterFields.push_back({ std::wstring(indentation, L' ') + L"-", std::wstring(value) });
			continue;
		}
		const auto separator = content.find(L':');
		if (separator == std::wstring_view::npos || separator == 0) {
			block->fallbackKind = NativeFallbackKind::UnsupportedSyntax;
			continue;
		}
		auto name = TrimRight(content.substr(0, separator));
		auto value = Trim(content.substr(separator + 1));
		if (HasUnsupportedFrontMatterSyntax(name, value)) {
			block->fallbackKind = NativeFallbackKind::UnsupportedSyntax;
			continue;
		}
		block->frontMatterFields.push_back({ std::wstring(indentation, L' ') + std::wstring(name),
			std::wstring(value) });
	}
}

struct FrontMatterMatch {
	bool matched = false;
	std::size_t consumedLines = 0;
	Block block;
};

[[nodiscard]] FrontMatterMatch ParseInitialFrontMatter(
	const std::vector<std::wstring_view>& lines, const ParseOptions& options)
{
	FrontMatterMatch match;
	if (lines.empty() || Trim(lines.front()) != L"---") return match;
	const auto maximumEnd = options.limits.maximumFrontMatterLines == (std::numeric_limits<std::size_t>::max)()
		? lines.size() : std::min(lines.size(), options.limits.maximumFrontMatterLines + 1);
	std::size_t closing = 1;
	while (closing < maximumEnd && Trim(lines[closing]) != L"---") ++closing;
	bool limitExceeded = false;
	if (closing >= maximumEnd) {
		if (maximumEnd >= lines.size()) return match;
		limitExceeded = true;
	}
	match.matched = true;
	const auto bodyEnd = limitExceeded ? maximumEnd : closing;
	match.consumedLines = limitExceeded ? maximumEnd : closing + 1;
	match.block.kind = BlockKind::FrontMatter;
	match.block.frontMatterMode = options.frontMatterMode;
	match.block.sourceLine = 0;
	auto raw = JoinLines(lines, 1, bodyEnd);
	ParseFrontMatterFields(raw, options, &match.block);
	if (options.frontMatterMode == FrontMatterMode::CodeBlock) match.block.text = std::move(raw);
	if (limitExceeded) match.block.fallbackKind = NativeFallbackKind::LimitExceeded;
	return match;
}

[[nodiscard]] std::size_t CharacterOffsetAfterLines(std::wstring_view source, std::size_t lineCount) noexcept
{
	const auto vectorScan = CpuDispatch::Get().findCrOrLfUtf16;
	std::size_t offset = 0;
	for (std::size_t line = 0; line < lineCount && offset < source.size(); ++line) {
		const auto newline = FindCrOrLf(source, offset, vectorScan);
		if (newline == source.size()) return source.size();
		offset = newline + (source[newline] == L'\r' && newline + 1 < source.size()
			&& source[newline + 1] == L'\n' ? 2 : 1);
	}
	return offset;
}

} // namespace

Document ParseMarkdown(std::wstring_view source, const ParseOptions& options)
{
	Document document;
	if (source.size() > options.limits.maximumInputCharacters) {
		auto cappedLength = options.limits.maximumInputCharacters;
#if WCHAR_MAX < 0x10ffff
		if (cappedLength > 0 && cappedLength < source.size()
			&& source[cappedLength - 1] >= static_cast<wchar_t>(0xd800)
			&& source[cappedLength - 1] <= static_cast<wchar_t>(0xdbff)
			&& source[cappedLength] >= static_cast<wchar_t>(0xdc00)
			&& source[cappedLength] <= static_cast<wchar_t>(0xdfff)) --cappedLength;
#endif
		source = source.substr(0, cappedLength);
		document.completion = ParseCompletion::InputLimitReached;
	}
	ParseContext context{ options, 0, CpuDispatch::Get().findMarkdownInlineSpecialUtf16 };
	std::size_t sourceLineOffset = 0;
	const auto originalLines = SplitLines(source);
	const auto frontMatter = ParseInitialFrontMatter(originalLines, options);
	if (frontMatter.matched) {
		if (document.blocks.size() >= options.limits.maximumBlocks) {
			if (document.completion == ParseCompletion::Complete) {
				document.completion = ParseCompletion::BlockLimitReached;
			}
			return document;
		}
		document.blocks.push_back(frontMatter.block);
		sourceLineOffset = frontMatter.consumedLines;
		source.remove_prefix(CharacterOffsetAfterLines(source, frontMatter.consumedLines));
	}
	const auto sanitized = SanitizeHtmlToMarkdown(source, options.limits.maximumHtmlDepth);
	const auto lines = SplitLines(sanitized);
	for (std::size_t index = 0; index < lines.size();) {
		const auto line = lines[index];
		if (Trim(line).empty()) {
			++index;
			continue;
		}
		if (document.blocks.size() >= options.limits.maximumBlocks) {
			if (document.completion == ParseCompletion::Complete) {
				document.completion = ParseCompletion::BlockLimitReached;
			}
			break;
		}
		wchar_t fenceMarker = L'\0';
		std::size_t fenceMarkerCount = 0;
		std::wstring_view fenceInfo;
		if (IsFenceStart(line, &fenceMarker, &fenceMarkerCount, &fenceInfo)) {
			const auto sourceLine = sourceLineOffset + index;
			std::wstring code;
			bool firstLine = true;
			++index;
			while (index < lines.size() && !IsFenceClose(lines[index], fenceMarker, fenceMarkerCount)) {
				if (!firstLine) code.push_back(L'\n');
				code.append(lines[index]);
				firstLine = false;
				++index;
			}
			if (index < lines.size()) ++index;
			Block block;
			block.language = NormalizeFenceLanguage(fenceInfo);
			if (block.language == L"math") {
				block.kind = BlockKind::Math;
				block.fallbackKind = NativeFallbackKind::LiteralSource;
			} else if (block.language == L"mermaid") {
				block.kind = BlockKind::MermaidDiagram;
				block.fallbackKind = NativeFallbackKind::LiteralSource;
			} else {
				block.kind = BlockKind::CodeBlock;
			}
			block.text = std::move(code);
			block.sourceLine = sourceLine;
			document.blocks.push_back(std::move(block));
			continue;
		}
		if (IsMermaidContainerStart(line)) {
			const auto sourceLine = sourceLineOffset + index;
			std::wstring diagram;
			++index;
			while (index < lines.size() && Trim(lines[index]) != L":::") {
				if (!diagram.empty()) diagram.push_back(L'\n');
				diagram.append(lines[index++]);
			}
			if (index < lines.size()) ++index;
			Block block;
			block.kind = BlockKind::MermaidDiagram;
			block.language = L"mermaid";
			block.text = std::move(diagram);
			block.fallbackKind = NativeFallbackKind::LiteralSource;
			block.sourceLine = sourceLine;
			document.blocks.push_back(std::move(block));
			continue;
		}
		if (IsMathBlockLine(line)) {
			const auto sourceLine = sourceLineOffset + index;
			const auto trimmed = Trim(line);
			std::wstring expression;
			if (trimmed == L"$$") {
				++index;
				while (index < lines.size() && Trim(lines[index]) != L"$$") {
					if (!expression.empty()) expression.push_back(L'\n');
					expression.append(lines[index++]);
				}
				if (index < lines.size()) ++index;
			} else {
				expression.assign(trimmed.substr(2, trimmed.size() - 4));
				++index;
			}
			Block block;
			block.kind = BlockKind::Math;
			block.language = L"math";
			block.text = std::move(expression);
			block.fallbackKind = NativeFallbackKind::LiteralSource;
			block.sourceLine = sourceLine;
			document.blocks.push_back(std::move(block));
			continue;
		}
		if (IsHorizontalRule(line)) {
			Block block;
			block.kind = BlockKind::HorizontalRule;
			block.sourceLine = sourceLineOffset + index;
			document.blocks.push_back(std::move(block));
			++index;
			continue;
		}
		HeadingMatch heading;
		if (ParseHeading(line, &heading)) {
			document.blocks.push_back(MakeTextBlock(BlockKind::Heading, heading.text, context,
				heading.level, {}, sourceLineOffset + index));
			++index;
			continue;
		}
		if (index + 1 < lines.size()) {
			const auto setextLevel = ParseSetextLevel(lines[index + 1]);
			if (setextLevel != 0) {
				document.blocks.push_back(MakeTextBlock(BlockKind::Heading, Trim(line), context,
					setextLevel, {}, sourceLineOffset + index));
				index += 2;
				continue;
			}
			std::vector<TableAlignment> alignments;
			if (ParseTableDelimiter(lines[index + 1], &alignments)) {
				Block table;
				table.kind = BlockKind::Table;
				table.sourceLine = sourceLineOffset + index;
				table.tableAlignments = std::move(alignments);
				table.tableRows.push_back(MakeTableRow(line, true, context));
				index += 2;
				while (index < lines.size() && !Trim(lines[index]).empty()
					&& lines[index].find(L'|') != std::wstring_view::npos) {
					table.tableRows.push_back(MakeTableRow(lines[index], false, context));
					++index;
				}
				document.blocks.push_back(std::move(table));
				continue;
			}
		}
		Block image;
		if (TryMakeStandaloneImage(line, context, sourceLineOffset + index, &image)) {
			document.blocks.push_back(std::move(image));
			++index;
			continue;
		}
		auto quote = StripUpToThreeSpaces(line);
		if (!quote.empty() && quote.front() == L'>') {
			quote.remove_prefix(1);
			if (!quote.empty() && quote.front() == L' ') quote.remove_prefix(1);
			document.blocks.push_back(MakeTextBlock(BlockKind::BlockQuote, quote, context,
				0, {}, sourceLineOffset + index));
			++index;
			continue;
		}
		ListMatch list;
		if (ParseListItem(line, &list)) {
			auto block = MakeTextBlock(list.kind, list.text, context,
				list.level, std::move(list.marker), sourceLineOffset + index);
			block.taskListState = list.taskListState;
			document.blocks.push_back(std::move(block));
			++index;
			continue;
		}
		const auto sourceLine = sourceLineOffset + index;
		std::wstring paragraph;
		while (index < lines.size() && !Trim(lines[index]).empty() && !IsBlockStart(lines[index])) {
			if (index + 1 < lines.size()) {
				std::vector<TableAlignment> ignored;
				if (ParseSetextLevel(lines[index + 1]) != 0 || ParseTableDelimiter(lines[index + 1], &ignored)) break;
			}
			if (!paragraph.empty()) paragraph.push_back(L' ');
			paragraph.append(Trim(lines[index]));
			++index;
		}
		if (!paragraph.empty()) {
			document.blocks.push_back(MakeTextBlock(BlockKind::Paragraph, paragraph, context, 0, {}, sourceLine));
		} else {
			// Every branch must advance even for malformed input.
			++index;
		}
	}
	return document;
}

LiveUpdateAction PreviewLiveUpdateModel::Observe(int revision) noexcept
{
	if (revision == m_renderedRevision && !m_pending) return LiveUpdateAction::None;
	if (!m_pending || revision != m_observedRevision) {
		m_observedRevision = revision;
		m_pending = true;
		return LiveUpdateAction::AwaitStableRevision;
	}
	return LiveUpdateAction::Render;
}

void PreviewLiveUpdateModel::Commit(int revision) noexcept
{
	m_renderedRevision = revision;
	m_observedRevision = revision;
	m_pending = false;
}

void PreviewLiveUpdateModel::Reset() noexcept
{
	m_renderedRevision = -1;
	m_observedRevision = -1;
	m_pending = false;
}

} // namespace markdown
