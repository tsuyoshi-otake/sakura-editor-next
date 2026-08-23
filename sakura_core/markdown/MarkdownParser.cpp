/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "MarkdownParser.h"
#include "util/CpuDispatch.h"
#include "util/Utf16BenchmarkTelemetry.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <utility>

namespace markdown {
namespace {

constexpr std::size_t kMaximumInlineDepth = 32;
constexpr std::size_t kInlineWorkPerCharacter = 64;

struct ParseContext {
	const ParseOptions& options;
	std::size_t imageReferences = 0;
	CpuDispatch::FindUtf16Function findMarkdownInlineSpecial = nullptr;
	std::size_t findMarkdownInlineSpecialMinimumLength = 64;
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
	std::wstring_view source, std::size_t start, CpuDispatch::FindUtf16Function vectorScan,
	std::size_t vectorMinimumLength) noexcept
{
	const auto remaining = source.size() - start;
	std::size_t offset = remaining;
	std::string_view implementationPath = "scalar";
	if (remaining >= vectorMinimumLength) {
		offset = vectorScan(source.data() + start, remaining);
		implementationPath = "simd";
	} else {
		for (std::size_t index = 0; index < remaining; ++index) {
			if (IsMarkdownInlineSpecial(source[start + index])) {
				offset = index;
				break;
			}
		}
	}
	SAKURA_UTF16_BENCHMARK_RECORD(
		"markdown", remaining, offset, source.data() + start,
		CpuDispatch::Get().isa, implementationPath);
	return start + offset;
}

[[nodiscard]] std::size_t FindCrOrLf(std::wstring_view source, std::size_t start,
	CpuDispatch::FindUtf16Function vectorScan, std::size_t vectorMinimumLength) noexcept
{
	const auto remaining = source.size() - start;
	std::size_t offset = remaining;
	std::string_view implementationPath = "scalar";
	if (remaining >= vectorMinimumLength) {
		offset = vectorScan(source.data() + start, remaining);
		implementationPath = "simd";
	} else {
		for (std::size_t index = 0; index < remaining; ++index) {
			if (source[start + index] == L'\r' || source[start + index] == L'\n') {
				offset = index;
				break;
			}
		}
	}
	SAKURA_UTF16_BENCHMARK_RECORD(
		"crlf", remaining, offset, source.data() + start,
		CpuDispatch::Get().isa, implementationPath);
	return start + offset;
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

/*!
	@brief The named HTML entities the preview decodes

	GitHub decodes the full HTML5 named-character-reference list (about 2,200
	names). Shipping that table for a preview pane is not worth its size, so this
	is the practical subset: every named reference in HTML 4, plus the Greek,
	arrow, and typographic names that appear in real documents. A name outside
	the table stays literal, exactly as an unknown reference does upstream, so
	the boundary fails closed and is visible in the rendered text.

	Kept sorted by name for the binary search below.
*/
constexpr struct { const wchar_t* name; unsigned int value; } kNamedEntities[] = {
	{ L"AElig", 0x00c6U },
	{ L"Aacute", 0x00c1U },
	{ L"Acirc", 0x00c2U },
	{ L"Agrave", 0x00c0U },
	{ L"Aring", 0x00c5U },
	{ L"Atilde", 0x00c3U },
	{ L"Auml", 0x00c4U },
	{ L"Ccedil", 0x00c7U },
	{ L"ETH", 0x00d0U },
	{ L"Eacute", 0x00c9U },
	{ L"Ecirc", 0x00caU },
	{ L"Egrave", 0x00c8U },
	{ L"Euml", 0x00cbU },
	{ L"Iacute", 0x00cdU },
	{ L"Icirc", 0x00ceU },
	{ L"Igrave", 0x00ccU },
	{ L"Iuml", 0x00cfU },
	{ L"Ntilde", 0x00d1U },
	{ L"Oacute", 0x00d3U },
	{ L"Ocirc", 0x00d4U },
	{ L"Ograve", 0x00d2U },
	{ L"Oslash", 0x00d8U },
	{ L"Otilde", 0x00d5U },
	{ L"Ouml", 0x00d6U },
	{ L"Uacute", 0x00daU },
	{ L"Ucirc", 0x00dbU },
	{ L"Ugrave", 0x00d9U },
	{ L"Uuml", 0x00dcU },
	{ L"Yacute", 0x00ddU },
	{ L"aacute", 0x00e1U },
	{ L"acirc", 0x00e2U },
	{ L"acute", 0x00b4U },
	{ L"aelig", 0x00e6U },
	{ L"agrave", 0x00e0U },
	{ L"alpha", 0x03b1U },
	{ L"amp", 0x0026U },
	{ L"and", 0x2227U },
	{ L"ang", 0x2220U },
	{ L"apos", 0x0027U },
	{ L"aring", 0x00e5U },
	{ L"asymp", 0x2248U },
	{ L"atilde", 0x00e3U },
	{ L"auml", 0x00e4U },
	{ L"beta", 0x03b2U },
	{ L"brvbar", 0x00a6U },
	{ L"bull", 0x2022U },
	{ L"cap", 0x2229U },
	{ L"ccedil", 0x00e7U },
	{ L"cedil", 0x00b8U },
	{ L"cent", 0x00a2U },
	{ L"check", 0x2713U },
	{ L"chi", 0x03c7U },
	{ L"circ", 0x02c6U },
	{ L"clubs", 0x2663U },
	{ L"cong", 0x2245U },
	{ L"copy", 0x00a9U },
	{ L"cup", 0x222aU },
	{ L"curren", 0x00a4U },
	{ L"dagger", 0x2020U },
	{ L"darr", 0x2193U },
	{ L"deg", 0x00b0U },
	{ L"delta", 0x03b4U },
	{ L"diams", 0x2666U },
	{ L"divide", 0x00f7U },
	{ L"eacute", 0x00e9U },
	{ L"ecirc", 0x00eaU },
	{ L"egrave", 0x00e8U },
	{ L"empty", 0x2205U },
	{ L"emsp", 0x2003U },
	{ L"ensp", 0x2002U },
	{ L"epsilon", 0x03b5U },
	{ L"equiv", 0x2261U },
	{ L"eta", 0x03b7U },
	{ L"eth", 0x00f0U },
	{ L"euml", 0x00ebU },
	{ L"euro", 0x20acU },
	{ L"exist", 0x2203U },
	{ L"forall", 0x2200U },
	{ L"frac12", 0x00bdU },
	{ L"frac14", 0x00bcU },
	{ L"frac34", 0x00beU },
	{ L"gamma", 0x03b3U },
	{ L"ge", 0x2265U },
	{ L"gt", 0x003eU },
	{ L"harr", 0x2194U },
	{ L"hearts", 0x2665U },
	{ L"hellip", 0x2026U },
	{ L"iacute", 0x00edU },
	{ L"icirc", 0x00eeU },
	{ L"iexcl", 0x00a1U },
	{ L"igrave", 0x00ecU },
	{ L"infin", 0x221eU },
	{ L"int", 0x222bU },
	{ L"iota", 0x03b9U },
	{ L"iquest", 0x00bfU },
	{ L"isin", 0x2208U },
	{ L"iuml", 0x00efU },
	{ L"kappa", 0x03baU },
	{ L"lambda", 0x03bbU },
	{ L"laquo", 0x00abU },
	{ L"larr", 0x2190U },
	{ L"ldquo", 0x201cU },
	{ L"le", 0x2264U },
	{ L"lsaquo", 0x2039U },
	{ L"lsquo", 0x2018U },
	{ L"lt", 0x003cU },
	{ L"macr", 0x00afU },
	{ L"mdash", 0x2014U },
	{ L"micro", 0x00b5U },
	{ L"middot", 0x00b7U },
	{ L"minus", 0x2212U },
	{ L"mu", 0x03bcU },
	{ L"nabla", 0x2207U },
	{ L"nbsp", 0x00a0U },
	{ L"ndash", 0x2013U },
	{ L"ne", 0x2260U },
	{ L"ni", 0x220bU },
	{ L"not", 0x00acU },
	{ L"notin", 0x2209U },
	{ L"nsub", 0x2284U },
	{ L"ntilde", 0x00f1U },
	{ L"nu", 0x03bdU },
	{ L"oacute", 0x00f3U },
	{ L"ocirc", 0x00f4U },
	{ L"ograve", 0x00f2U },
	{ L"oline", 0x203eU },
	{ L"omega", 0x03c9U },
	{ L"omicron", 0x03bfU },
	{ L"oplus", 0x2295U },
	{ L"or", 0x2228U },
	{ L"ordf", 0x00aaU },
	{ L"ordm", 0x00baU },
	{ L"oslash", 0x00f8U },
	{ L"otilde", 0x00f5U },
	{ L"otimes", 0x2297U },
	{ L"ouml", 0x00f6U },
	{ L"para", 0x00b6U },
	{ L"part", 0x2202U },
	{ L"permil", 0x2030U },
	{ L"perp", 0x22a5U },
	{ L"phi", 0x03c6U },
	{ L"pi", 0x03c0U },
	{ L"piv", 0x03d6U },
	{ L"plusmn", 0x00b1U },
	{ L"pound", 0x00a3U },
	{ L"prime", 0x2032U },
	{ L"prod", 0x220fU },
	{ L"prop", 0x221dU },
	{ L"psi", 0x03c8U },
	{ L"quot", 0x0022U },
	{ L"radic", 0x221aU },
	{ L"raquo", 0x00bbU },
	{ L"rarr", 0x2192U },
	{ L"rdquo", 0x201dU },
	{ L"reg", 0x00aeU },
	{ L"rho", 0x03c1U },
	{ L"rsaquo", 0x203aU },
	{ L"rsquo", 0x2019U },
	{ L"sbquo", 0x201aU },
	{ L"sdot", 0x22c5U },
	{ L"sect", 0x00a7U },
	{ L"shy", 0x00adU },
	{ L"sigma", 0x03c3U },
	{ L"sigmaf", 0x03c2U },
	{ L"sim", 0x223cU },
	{ L"spades", 0x2660U },
	{ L"sub", 0x2282U },
	{ L"sube", 0x2286U },
	{ L"sum", 0x2211U },
	{ L"sup", 0x2283U },
	{ L"sup1", 0x00b9U },
	{ L"sup2", 0x00b2U },
	{ L"sup3", 0x00b3U },
	{ L"supe", 0x2287U },
	{ L"szlig", 0x00dfU },
	{ L"tau", 0x03c4U },
	{ L"there4", 0x2234U },
	{ L"theta", 0x03b8U },
	{ L"thinsp", 0x2009U },
	{ L"thorn", 0x00feU },
	{ L"tilde", 0x02dcU },
	{ L"times", 0x00d7U },
	{ L"trade", 0x2122U },
	{ L"uacute", 0x00faU },
	{ L"uarr", 0x2191U },
	{ L"ucirc", 0x00fbU },
	{ L"ugrave", 0x00f9U },
	{ L"uml", 0x00a8U },
	{ L"upsilon", 0x03c5U },
	{ L"uuml", 0x00fcU },
	{ L"weierp", 0x2118U },
	{ L"xi", 0x03beU },
	{ L"yacute", 0x00fdU },
	{ L"yen", 0x00a5U },
	{ L"yuml", 0x00ffU },
	{ L"zeta", 0x03b6U },
};

[[nodiscard]] bool LookupNamedEntity(std::wstring_view name, unsigned int* value) noexcept
{
	std::size_t low = 0;
	std::size_t high = std::size(kNamedEntities);
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		const auto order = name.compare(kNamedEntities[middle].name);
		if (order == 0) {
			*value = kNamedEntities[middle].value;
			return true;
		}
		if (order < 0) high = middle; else low = middle + 1;
	}
	return false;
}

/*!
	@brief The emoji shortcodes the preview understands

	GitHub accepts about 1,800 `:name:` shortcodes. A preview pane does not
	justify shipping that list, so this is the subset that actually appears in
	README and changelog prose. An unlisted shortcode stays literal, which is
	what GitHub itself does for a name it does not know, so the boundary is
	visible rather than silently swallowed.

	Kept sorted by name for the binary search below.
*/
constexpr struct { const wchar_t* name; unsigned int value; } kEmojiShortcodes[] = {
	{ L"+1", 0x1f44dU },
	{ L"-1", 0x1f44eU },
	{ L"100", 0x1f4afU },
	{ L"art", 0x1f3a8U },
	{ L"beetle", 0x1f41eU },
	{ L"bell", 0x1f514U },
	{ L"blue_book", 0x1f4d8U },
	{ L"book", 0x1f4d6U },
	{ L"bookmark", 0x1f516U },
	{ L"boom", 0x1f4a5U },
	{ L"bug", 0x1f41bU },
	{ L"bulb", 0x1f4a1U },
	{ L"calendar", 0x1f4c5U },
	{ L"check", 0x02714U },
	{ L"checkered_flag", 0x1f3c1U },
	{ L"clap", 0x1f44fU },
	{ L"computer", 0x1f4bbU },
	{ L"construction", 0x1f6a7U },
	{ L"dart", 0x1f3afU },
	{ L"exclamation", 0x02757U },
	{ L"eyes", 0x1f440U },
	{ L"fire", 0x1f525U },
	{ L"gear", 0x02699U },
	{ L"gem", 0x1f48eU },
	{ L"gift", 0x1f381U },
	{ L"green_book", 0x1f4d7U },
	{ L"hammer", 0x1f528U },
	{ L"heart", 0x02764U },
	{ L"heavy_check_mark", 0x02714U },
	{ L"hourglass", 0x0231bU },
	{ L"information_source", 0x02139U },
	{ L"key", 0x1f511U },
	{ L"lock", 0x1f512U },
	{ L"loud_sound", 0x1f50aU },
	{ L"mag", 0x1f50dU },
	{ L"memo", 0x1f4ddU },
	{ L"package", 0x1f4e6U },
	{ L"page_facing_up", 0x1f4c4U },
	{ L"pencil", 0x1f4ddU },
	{ L"pushpin", 0x1f4ccU },
	{ L"question", 0x02753U },
	{ L"recycle", 0x0267bU },
	{ L"rocket", 0x1f680U },
	{ L"scroll", 0x1f4dcU },
	{ L"shipit", 0x1f41fU },
	{ L"smile", 0x1f604U },
	{ L"sparkles", 0x02728U },
	{ L"star", 0x02b50U },
	{ L"tada", 0x1f389U },
	{ L"thumbsdown", 0x1f44eU },
	{ L"thumbsup", 0x1f44dU },
	{ L"triangular_flag_on_post", 0x1f6a9U },
	{ L"warning", 0x026a0U },
	{ L"wrench", 0x1f527U },
	{ L"x", 0x0274cU },
	{ L"zap", 0x026a1U },
};

[[nodiscard]] bool LookupEmojiShortcode(std::wstring_view name, unsigned int* value) noexcept
{
	std::size_t low = 0;
	std::size_t high = std::size(kEmojiShortcodes);
	while (low < high) {
		const auto middle = low + (high - low) / 2;
		const auto order = name.compare(kEmojiShortcodes[middle].name);
		if (order == 0) {
			*value = kEmojiShortcodes[middle].value;
			return true;
		}
		if (order < 0) high = middle; else low = middle + 1;
	}
	return false;
}

[[nodiscard]] bool DecodeEntityAt(std::wstring_view source, std::size_t index,
	std::wstring& output, std::size_t* consumed)
{
	if (index >= source.size() || source[index] != L'&') return false;
	// The longest name in the table is eight characters; the numeric form needs
	// room for a full code point, so the window stays wide enough for both.
	const auto searchLength = std::min<std::size_t>(16, source.size() - index - 1);
	const auto relativeEnd = source.substr(index + 1, searchLength).find(L';');
	if (relativeEnd == std::wstring_view::npos) return false;
	const auto end = index + 1 + relativeEnd;
	const auto name = source.substr(index + 1, end - index - 1);
	unsigned int named = 0;
	if (LookupNamedEntity(name, &named)) {
		AppendCodePoint(output, named);
	}
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
		|| name == L"video" || name == L"audio" || name == L"canvas"
		// Native preview has no form/control surface. Dropping these containers
		// prevents an HTML document from pretending that an interactive control
		// was created while keeping the parser's failure-closed boundary explicit.
		|| name == L"button" || name == L"select" || name == L"option"
		|| name == L"optgroup" || name == L"textarea" || name == L"template";
}

[[nodiscard]] bool IsVoidHtmlTag(std::wstring_view name) noexcept
{
	return name == L"br" || name == L"hr" || name == L"img" || name == L"meta"
		|| name == L"link" || name == L"input" || name == L"source"
		|| name == L"area" || name == L"base" || name == L"col" || name == L"wbr";
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
	constexpr wchar_t kCaptionStart = L'\x001b';
	constexpr wchar_t kCaptionEnd = L'\x001c';
	std::wstring tableContent;
	std::wstring caption;
	// A caption is projected to a normal paragraph immediately before the
	// native table. Keeping it outside the row/cell sentinels means the existing
	// Table block model stays small while caption text is no longer discarded.
	for (std::size_t index = 0; index < encoded.size();) {
		if (encoded[index] != kCaptionStart) {
			tableContent.push_back(encoded[index++]);
			continue;
		}
		const auto end = encoded.find(kCaptionEnd, index + 1);
		if (end == std::wstring_view::npos) break;
		const auto value = Trim(encoded.substr(index + 1, end - index - 1));
		if (!value.empty()) {
			if (!caption.empty()) caption.push_back(L'\n');
			caption.append(value);
		}
		index = end + 1;
	}
	std::vector<std::vector<std::wstring>> rows;
	std::size_t rowStart = 0;
	while (rowStart < tableContent.size()) {
		const auto rowEnd = tableContent.find(kRow, rowStart);
		const auto row = tableContent.substr(rowStart,
			rowEnd == std::wstring_view::npos ? tableContent.size() - rowStart : rowEnd - rowStart);
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
	if (rows.empty()) return caption.empty() ? std::wstring{} : L"\n" + caption + L"\n";
	std::wstring result = caption.empty() ? L"\n" : L"\n" + caption + L"\n\n";
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

[[nodiscard]] std::wstring MakeHtmlCodeSpan(std::wstring_view content)
{
	std::size_t longestRun = 0;
	for (std::size_t index = 0; index < content.size();) {
		if (content[index] != L'`') {
			++index;
			continue;
		}
		const auto start = index++;
		while (index < content.size() && content[index] == L'`') ++index;
		longestRun = std::max(longestRun, index - start);
	}
	const auto markerCount = longestRun == (std::numeric_limits<std::size_t>::max)()
		? longestRun : std::max<std::size_t>(1, longestRun + 1);
	const std::wstring marker(markerCount, L'`');
	return marker + std::wstring(content) + marker;
}

[[nodiscard]] std::wstring MakeHtmlPreBlock(std::wstring_view content)
{
	// Decode character references as HTML would before placing the text into a
	// native fenced-code block. Pick a fence longer than every literal backtick
	// run so hostile/preformatted content cannot terminate its own projection.
	const auto decoded = DecodeEntities(content);
	std::size_t longestRun = 0;
	for (std::size_t index = 0; index < decoded.size();) {
		if (decoded[index] != L'`') {
			++index;
			continue;
		}
		const auto start = index++;
		while (index < decoded.size() && decoded[index] == L'`') ++index;
		longestRun = std::max(longestRun, index - start);
	}
	const auto markerCount = longestRun == (std::numeric_limits<std::size_t>::max)()
		? longestRun : std::max<std::size_t>(3, longestRun + 1);
	const std::wstring marker(markerCount, L'`');
	return L"\n" + marker + L"\n" + decoded + L"\n" + marker + L"\n";
}

[[nodiscard]] std::wstring RenderHtmlFrame(const HtmlFrame& frame, std::wstring_view parentName)
{
	const auto& name = frame.tag.name;
	if (IsDangerousHtmlContainer(name)) return {};
	if (name == L"img") {
		const auto source = GetAttribute(frame.tag, L"src");
		if (source.empty()) return {};
		const auto alt = GetAttribute(frame.tag, L"alt");
		return L"![" + EscapeMarkdownLabel(alt) + L"](<" + EscapeMarkdownDestination(source) + L">)";
	}
	// The Markdown paragraph builder treats two trailing spaces as a hard line
	// break. A bare newline would be normalized to a space and make `<br>` look
	// as if the tag had been removed.
	if (name == L"br") return L"  \n";
	if (name == L"hr") return L"\n---\n";
	if (name == L"meta" || name == L"link" || name == L"input" || name == L"source") return {};
	if (name == L"strong" || name == L"b") return L"**" + frame.content + L"**";
	if (name == L"em" || name == L"i") return L"*" + frame.content + L"*";
	if (name == L"del" || name == L"s" || name == L"strike") {
		return L"~~" + frame.content + L"~~";
	}
	if (name == L"kbd" || name == L"samp" || name == L"var") {
		return MakeHtmlCodeSpan(frame.content);
	}
	if (name == L"code") return parentName == L"pre" ? frame.content : MakeHtmlCodeSpan(frame.content);
	if (name == L"pre") return MakeHtmlPreBlock(frame.content);
	if (name == L"a") {
		const auto href = GetAttribute(frame.tag, L"href");
		if (href.empty() || frame.content.find(L"![") != std::wstring::npos) return frame.content;
		return L"[" + EscapeMarkdownLabel(frame.content) + L"](<" + EscapeMarkdownDestination(href) + L">)";
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
	if (name == L"dl") return L"\n" + frame.content + L"\n";
	if (name == L"dt") return L"\n**" + frame.content + L"**\n";
	if (name == L"dd") return L"\n- " + frame.content + L"\n";
	if (name == L"td") return std::wstring(1, L'\x001e') + frame.content;
	if (name == L"th") return std::wstring(1, L'\x001f') + frame.content;
	if (name == L"tr") return frame.content + std::wstring(1, L'\x001d');
	if (name == L"caption" && parentName == L"table") {
		return std::wstring(1, L'\x001b') + frame.content + std::wstring(1, L'\x001c');
	}
	if (name == L"thead" || name == L"tbody" || name == L"tfoot") return frame.content;
	if (name == L"table") return RenderHtmlTable(frame.content);
	if (name == L"p" || name == L"div" || name == L"section" || name == L"article"
		|| name == L"header" || name == L"footer" || name == L"main" || name == L"center"
		|| name == L"nav" || name == L"aside" || name == L"address"
		|| name == L"figure" || name == L"figcaption" || name == L"details" || name == L"summary") {
		return L"\n" + frame.content + L"\n";
	}
	// Inline semantic wrappers that carry no native CSS equivalent are still
	// supported as safe text containers. Attributes (including style and event
	// handlers) are deliberately ignored, so these tags cannot execute or
	// inject a second renderer into the preview.
	if (name == L"abbr" || name == L"bdi" || name == L"bdo" || name == L"cite"
		|| name == L"dfn" || name == L"ins" || name == L"mark" || name == L"q"
		|| name == L"small" || name == L"span" || name == L"sub" || name == L"sup"
		|| name == L"time" || name == L"u") {
		return frame.content;
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
	const auto& dispatch = CpuDispatch::Get();
	for (std::size_t index = 0; index < source.size();) {
		// Markdown code is already inert source. Preserve it exactly so the HTML
		// projection cannot reinterpret literal tags inside code spans or fences.
		if (suppressedDepth == 0 && stack.size() == 1 && IsSourceLineStart(source, index)) {
			const auto lineEnd = FindCrOrLf(source, index, dispatch.findCrOrLfUtf16,
				dispatch.utf16ScanPolicy.crOrLfMinimumLength);
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
		if (scheme == L"https" && use == ResourceUse::Image) {
			result.disposition = ResourceDisposition::ResolvedHttps;
		} else {
			result.disposition = scheme == L"http" || scheme == L"https" || scheme == L"mailto"
				? ResourceDisposition::ExternalBlocked : ResourceDisposition::UnsafeSchemeBlocked;
		}
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

/*!
	@name CommonMark emphasis, resolved with a delimiter stack

	Emphasis cannot be paired by scanning forward for the next matching run:
	`**a *b* c**` pairs the inner run with the run before `b`, which a forward
	search from the outer `**` never sees, and `***x***` needs one run to supply
	both a strong and an emphasis delimiter. CommonMark specifies a delimiter
	stack processed closer-by-closer, looking *backwards* for the nearest opener,
	and GitHub renders exactly that. The runs are appended to the text as literal
	characters while scanning and the consumed ones are cut out afterwards, so
	spans recorded by other inline rules are remapped once at the end.
*/
///@{
struct InlineDelimiter {
	std::size_t textPos = 0;   //!< Offset of the run's first character in the text
	std::size_t count = 0;     //!< Characters still unconsumed
	std::size_t original = 0;  //!< Run length before any pairing, for the rule of three
	wchar_t marker = L'*';
	bool canOpen = false;
	bool canClose = false;
	bool active = true;
};

[[nodiscard]] bool IsUnicodeWhitespace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t' || value == L'\n' || value == L'\r'
		|| value == L'\f' || value == L'\v';
}

[[nodiscard]] bool IsUnicodePunctuation(wchar_t value) noexcept
{
	return std::iswpunct(value) != 0;
}

//! Classifies one delimiter run per CommonMark's left/right-flanking definitions.
void ClassifyDelimiterRun(std::wstring_view source, std::size_t runStart, std::size_t runLength,
	wchar_t marker, bool* canOpen, bool* canClose) noexcept
{
	const wchar_t before = runStart == 0 ? L'\n' : source[runStart - 1];
	const wchar_t after = runStart + runLength >= source.size() ? L'\n' : source[runStart + runLength];
	const bool whitespaceBefore = IsUnicodeWhitespace(before);
	const bool whitespaceAfter = IsUnicodeWhitespace(after);
	const bool punctuationBefore = IsUnicodePunctuation(before);
	const bool punctuationAfter = IsUnicodePunctuation(after);
	const bool leftFlanking = !whitespaceAfter
		&& (!punctuationAfter || whitespaceBefore || punctuationBefore);
	const bool rightFlanking = !whitespaceBefore
		&& (!punctuationBefore || whitespaceAfter || punctuationAfter);
	if (marker == L'_') {
		// Intraword underscores stay literal, which is why snake_case_names survive.
		*canOpen = leftFlanking && (!rightFlanking || punctuationBefore);
		*canClose = rightFlanking && (!leftFlanking || punctuationAfter);
	} else {
		*canOpen = leftFlanking;
		*canClose = rightFlanking;
	}
}

//! Pairs the collected runs, recording the resulting spans and the cut ranges.
void ResolveInlineDelimiters(std::vector<InlineDelimiter>& delimiters,
	std::vector<InlineSpan>& spans, std::vector<std::pair<std::size_t, std::size_t>>& removals)
{
	for (std::size_t closerIndex = 0; closerIndex < delimiters.size(); ++closerIndex) {
		while (delimiters[closerIndex].active && delimiters[closerIndex].canClose
			&& delimiters[closerIndex].count != 0) {
			std::size_t openerIndex = closerIndex;
			bool found = false;
			while (openerIndex != 0) {
				--openerIndex;
				const auto& candidate = delimiters[openerIndex];
				if (!candidate.active || candidate.count == 0) continue;
				if (candidate.marker != delimiters[closerIndex].marker) continue;
				if (!candidate.canOpen) continue;
				// CommonMark's rule of three: a run that can both open and close
				// may not pair when the original lengths sum to a multiple of
				// three, unless both lengths already are.
				if ((delimiters[closerIndex].canOpen || candidate.canClose)
					&& (candidate.original + delimiters[closerIndex].original) % 3 == 0
					&& (candidate.original % 3 != 0 || delimiters[closerIndex].original % 3 != 0)) {
					continue;
				}
				found = true;
				break;
			}
			if (!found) break;
			auto& opener = delimiters[openerIndex];
			auto& closer = delimiters[closerIndex];
			const std::size_t use = closer.marker == L'~'
				? std::min<std::size_t>(std::min(opener.count, closer.count), 2u)
				: (opener.count >= 2 && closer.count >= 2 ? 2u : 1u);
			const std::size_t openerCut = opener.textPos + opener.count - use;
			const std::size_t closerCut = closer.textPos;
			const InlineKind kind = closer.marker == L'~'
				? InlineKind::Strikethrough
				: (use == 2 ? InlineKind::Strong : InlineKind::Emphasis);
			spans.push_back({ kind, openerCut + use, closerCut - openerCut - use, std::nullopt });
			removals.emplace_back(openerCut, use);
			removals.emplace_back(closerCut, use);
			opener.count -= use;
			closer.count -= use;
			closer.textPos += use;
			if (opener.count == 0) opener.active = false;
			if (closer.count == 0) closer.active = false;
			// Everything between the pair can no longer match anything.
			for (std::size_t between = openerIndex + 1; between < closerIndex; ++between) {
				delimiters[between].active = false;
			}
		}
	}
}

//! Cuts the consumed delimiter characters out and remaps every recorded span.
void ApplyInlineDelimiterRemovals(ParsedText& result,
	std::vector<std::pair<std::size_t, std::size_t>> removals)
{
	if (removals.empty()) return;
	std::sort(removals.begin(), removals.end());
	std::wstring text;
	text.reserve(result.text.size());
	std::vector<std::size_t> cutStart;
	std::vector<std::size_t> cutEnd;
	cutStart.reserve(removals.size());
	cutEnd.reserve(removals.size());
	std::size_t copied = 0;
	for (const auto& removal : removals) {
		if (removal.first < copied) continue;
		text.append(result.text, copied, removal.first - copied);
		cutStart.push_back(removal.first);
		cutEnd.push_back(removal.first + removal.second);
		copied = removal.first + removal.second;
	}
	if (copied < result.text.size()) text.append(result.text, copied, result.text.size() - copied);
	const auto map = [&cutStart, &cutEnd](std::size_t offset) noexcept {
		std::size_t removed = 0;
		for (std::size_t i = 0; i < cutStart.size(); ++i) {
			if (cutEnd[i] <= offset) {
				removed += cutEnd[i] - cutStart[i];
			} else if (cutStart[i] < offset) {
				// Inside a cut: those characters are gone, so collapse to its start.
				removed += offset - cutStart[i];
			} else {
				break;
			}
		}
		return offset - removed;
	};
	for (auto& span : result.spans) {
		const auto start = map(span.start);
		const auto end = map(span.start + span.length);
		span.start = start;
		span.length = end > start ? end - start : 0;
	}
	result.text = std::move(text);
	result.spans.erase(
		std::remove_if(result.spans.begin(), result.spans.end(),
			[](const InlineSpan& span) {
				return span.length == 0 && span.kind != InlineKind::Image;
			}),
		result.spans.end());
}
///@}

/*!
	@brief Replaces `:name:` shortcodes in already-parsed text

	This runs after inline parsing rather than inside it because ':' is not one
	of the characters the vectorized special-character scan stops on, and adding
	it would cost every document a slower scan for a rare construct. Code and
	math spans are skipped, matching GitHub, which leaves a shortcode inside
	backticks alone.
*/
void ApplyEmojiShortcodes(ParsedText& result)
{
	if (result.text.find(L':') == std::wstring::npos) return;
	const auto isVerbatim = [&result](std::size_t offset) {
		for (const auto& span : result.spans) {
			if ((span.kind == InlineKind::Code || span.kind == InlineKind::Math)
				&& offset >= span.start && offset < span.start + span.length) {
				return true;
			}
		}
		return false;
	};
	std::wstring text;
	text.reserve(result.text.size());
	// Each edit is (offset in the original text, replaced length, produced length).
	std::vector<std::array<std::size_t, 3>> edits;
	std::size_t index = 0;
	while (index < result.text.size()) {
		if (result.text[index] != L':' || isVerbatim(index)) {
			text.push_back(result.text[index++]);
			continue;
		}
		const auto closing = result.text.find(L':', index + 1);
		unsigned int codePoint = 0;
		const std::wstring_view name = closing == std::wstring::npos
			? std::wstring_view{}
			: std::wstring_view{ result.text }.substr(index + 1, closing - index - 1);
		if (name.empty() || name.find_first_of(L" \t\n") != std::wstring_view::npos
			|| !LookupEmojiShortcode(name, &codePoint)) {
			text.push_back(result.text[index++]);
			continue;
		}
		const auto before = text.size();
		AppendCodePoint(text, codePoint);
		edits.push_back({ index, closing + 1 - index, text.size() - before });
		index = closing + 1;
	}
	if (edits.empty()) return;
	const auto map = [&edits](std::size_t offset) noexcept {
		std::ptrdiff_t shift = 0;
		for (const auto& edit : edits) {
			if (edit[0] + edit[1] <= offset) {
				shift += static_cast<std::ptrdiff_t>(edit[2]) - static_cast<std::ptrdiff_t>(edit[1]);
			} else if (edit[0] < offset) {
				// Inside a replaced shortcode: collapse to where it now starts.
				shift += static_cast<std::ptrdiff_t>(edit[0]) - static_cast<std::ptrdiff_t>(offset);
			} else {
				break;
			}
		}
		return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(offset) + shift);
	};
	for (auto& span : result.spans) {
		const auto start = map(span.start);
		const auto end = map(span.start + span.length);
		span.start = start;
		span.length = end > start ? end - start : 0;
	}
	result.text = std::move(text);
}

[[nodiscard]] ParsedText ParseInlineInternal(std::wstring_view source, ParseContext& context,
	std::size_t depth, InlineWorkBudget& budget)
{
	ParsedText result;
	result.text.reserve(source.size());
	std::vector<InlineDelimiter> delimiters;
	for (std::size_t index = 0; index < source.size();) {
		if (budget.exceeded) {
			result.text.append(source.substr(index));
			result.limitExceeded = true;
			break;
		}
		const auto special = FindMarkdownInlineSpecial(source, index,
			context.findMarkdownInlineSpecial, context.findMarkdownInlineSpecialMinimumLength);
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
		if (source[index] == L'~' || source[index] == L'*' || source[index] == L'_') {
			std::size_t runLength = 1;
			while (index + runLength < source.size() && source[index + runLength] == source[index]) {
				++runLength;
			}
			if (!CanDescendInline(context, depth)) {
				// The configured depth limit keeps the run literal rather than
				// rendering it anyway, so the boundary stays visible.
				result.limitExceeded = true;
			} else if (source[index] != L'~' || runLength <= 2) {
				// GFM strikethrough is one or two tildes; longer runs are literal.
				bool canOpen = false;
				bool canClose = false;
				ClassifyDelimiterRun(source, index, runLength, source[index], &canOpen, &canClose);
				delimiters.push_back({ result.text.size(), runLength, runLength,
					source[index], canOpen, canClose, true });
			}
			(void)budget.Consume(runLength);
			result.text.append(source.substr(index, runLength));
			index += runLength;
			continue;
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
		std::size_t consumed = 0;
		if (DecodeEntityAt(source, index, result.text, &consumed)) {
			(void)budget.Consume(consumed);
			index += consumed;
		} else {
			(void)budget.Consume();
			result.text.push_back(source[index++]);
		}
	}
	std::vector<std::pair<std::size_t, std::size_t>> removals;
	ResolveInlineDelimiters(delimiters, result.spans, removals);
	ApplyInlineDelimiterRemovals(result, std::move(removals));
	ApplyEmojiShortcodes(result);
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
	const auto& dispatch = CpuDispatch::Get();
	const auto vectorScan = dispatch.findCrOrLfUtf16;
	const auto vectorMinimumLength = dispatch.utf16ScanPolicy.crOrLfMinimumLength;
	std::size_t lineStart = 0;
	while (lineStart < source.size()) {
		const auto lineEnd = FindCrOrLf(source, lineStart, vectorScan, vectorMinimumLength);
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
		// GFM requires at least one dash per column, so `|:-|-:|` is a valid header.
		if (cell.empty() || cell.find_first_not_of(L'-') != std::wstring_view::npos) return false;
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
	block->images.push_back(ImageNode{ DecodeEntities(line.substr(2, labelEnd - 2)),
		ResolveResource(destination, ResourceUse::Image, context) });
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
	const auto& dispatch = CpuDispatch::Get();
	const auto vectorScan = dispatch.findCrOrLfUtf16;
	const auto vectorMinimumLength = dispatch.utf16ScanPolicy.crOrLfMinimumLength;
	std::size_t offset = 0;
	for (std::size_t line = 0; line < lineCount && offset < source.size(); ++line) {
		const auto newline = FindCrOrLf(source, offset, vectorScan, vectorMinimumLength);
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
	const auto& parseDispatch = CpuDispatch::Get();
	ParseContext context{ options, 0, parseDispatch.findMarkdownInlineSpecialUtf16,
		parseDispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength };
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
			++index;
			// CommonMark keeps consecutive image-only lines in one paragraph. The
			// native preview models that paragraph as a row instead of promoting
			// every image to an unrelated vertical block.
			while (index < lines.size() && !Trim(lines[index]).empty()) {
				Block following;
				if (!TryMakeStandaloneImage(lines[index], context,
					sourceLineOffset + index, &following)) break;
				if (!following.images.empty()) {
					image.images.push_back(std::move(following.images.front()));
				}
				++index;
			}
			document.blocks.push_back(std::move(image));
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
		// A four-space indent after a blank line is CommonMark's indented code
		// block. It is checked after the list rules because a nested list item
		// is also indented, and it requires the blank line because an indented
		// code block may not interrupt a paragraph.
		if (CountIndent(line) >= 4 && (index == 0 || Trim(lines[index - 1]).empty())) {
			const auto sourceLine = sourceLineOffset + index;
			std::wstring code;
			bool firstLine = true;
			std::size_t lastContentLine = index;
			for (std::size_t scan = index; scan < lines.size(); ++scan) {
				const auto blank = Trim(lines[scan]).empty();
				if (!blank && CountIndent(lines[scan]) < 4) break;
				if (!firstLine) code.push_back(L'\n');
				code.append(blank ? std::wstring_view{} : lines[scan].substr(4));
				firstLine = false;
				if (!blank) lastContentLine = scan;
			}
			// Trailing blank lines belong to the document, not to the code.
			while (!code.empty() && code.back() == L'\n') code.pop_back();
			index = lastContentLine + 1;
			Block block;
			block.kind = BlockKind::CodeBlock;
			block.text = std::move(code);
			block.sourceLine = sourceLine;
			document.blocks.push_back(std::move(block));
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
			if (!paragraph.empty() && paragraph.back() != L'\n') {
				paragraph.push_back(L' ');
			}
			// GFM hard break: two or more trailing spaces, or a trailing backslash.
			// The break is carried into the text as a newline, which the renderer
			// turns into a forced line box.
			auto content = lines[index];
			bool hardBreak = false;
			if (content.size() >= 2 && content.substr(content.size() - 2) == L"  ") {
				hardBreak = true;
			} else if (!content.empty() && content.back() == L'\\') {
				hardBreak = true;
				content.remove_suffix(1);
			}
			paragraph.append(Trim(content));
			if (hardBreak && index + 1 < lines.size() && !Trim(lines[index + 1]).empty()) {
				paragraph.push_back(L'\n');
			}
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
