/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include <sakura/uri/UriIdentity.h>
#include "UriIdentityInternal.h"

#include <algorithm>
#include <cwctype>

namespace platform::uri {
namespace {

using internal::IsAsciiAlpha;
using internal::IsSchemeCharacter;
using internal::IsUnreserved;
using internal::ToLowerAscii;

bool IsSubDelimiter(wchar_t value) noexcept
{
	switch (value) {
	case L'!': case L'$': case L'&': case L'\'': case L'(': case L')':
	case L'*': case L'+': case L',': case L';': case L'=':
		return true;
	default:
		return false;
	}
}

std::wstring ToLowerInvariant(std::wstring_view value)
{
	std::wstring lowered;
	lowered.reserve(value.size());
	for (const wchar_t character : value) {
		// URI の ASCII scheme/host は必ず正規化する。非 ASCII は CRT の basic wide
		// lower-case を使う。ファイル実体への照会はしない。
		lowered.push_back(character <= 0x7f ? ToLowerAscii(character) : static_cast<wchar_t>(std::towlower(character)));
	}
	return lowered;
}

bool IsValidUtf16(std::wstring_view value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto character = static_cast<std::uint32_t>(value[index]);
		if constexpr (sizeof(wchar_t) == 2) {
			if (character >= 0xd800 && character <= 0xdbff) {
				if (index + 1 >= value.size()) {
					return false;
				}
				const auto trailing = static_cast<std::uint32_t>(value[++index]);
				if (trailing < 0xdc00 || trailing > 0xdfff) {
					return false;
				}
			} else if (character >= 0xdc00 && character <= 0xdfff) {
				return false;
			}
		} else if ((character >= 0xd800 && character <= 0xdfff) || character > 0x10ffff) {
			return false;
		}
	}
	return true;
}

bool HasControlCharacter(std::wstring_view value) noexcept
{
	for (const wchar_t character : value) {
		if (character <= 0x1f || character == 0x7f) {
			return true;
		}
	}
	return false;
}

bool ValidateScheme(std::wstring_view scheme) noexcept
{
	if (scheme.empty() || !IsAsciiAlpha(scheme.front())) {
		return false;
	}
	for (const wchar_t character : scheme) {
		if (!IsSchemeCharacter(character)) {
			return false;
		}
	}
	return true;
}

bool ValidateComponent(std::wstring_view value) noexcept
{
	return IsValidUtf16(value) && !HasControlCharacter(value);
}

bool IsRawUriCharacterInvalid(wchar_t value) noexcept
{
	return value <= 0x20 || value == 0x7f || value == L'\\';
}

bool ContainsInvalidRawUriCharacter(std::wstring_view value) noexcept
{
	for (const wchar_t character : value) {
		if (IsRawUriCharacterInvalid(character)) {
			return true;
		}
	}
	return false;
}

int HexValue(wchar_t value) noexcept
{
	if (value >= L'0' && value <= L'9') return value - L'0';
	if (value >= L'a' && value <= L'f') return value - L'a' + 10;
	if (value >= L'A' && value <= L'F') return value - L'A' + 10;
	return -1;
}

bool AppendCodePoint(std::wstring& output, std::uint32_t codePoint) noexcept
{
	if (codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
		return false;
	}
	if constexpr (sizeof(wchar_t) == 2) {
		if (codePoint <= 0xffff) {
			output.push_back(static_cast<wchar_t>(codePoint));
		} else {
			codePoint -= 0x10000;
			output.push_back(static_cast<wchar_t>(0xd800 + (codePoint >> 10)));
			output.push_back(static_cast<wchar_t>(0xdc00 + (codePoint & 0x3ff)));
		}
	} else {
		output.push_back(static_cast<wchar_t>(codePoint));
	}
	return true;
}

bool DecodeUtf8(std::string_view bytes, std::wstring& output) noexcept
{
	for (std::size_t index = 0; index < bytes.size();) {
		const auto first = static_cast<unsigned char>(bytes[index++]);
		std::uint32_t codePoint = 0;
		std::size_t trailing = 0;
		if (first <= 0x7f) {
			codePoint = first;
		} else if (first >= 0xc2 && first <= 0xdf) {
			codePoint = first & 0x1f;
			trailing = 1;
		} else if (first >= 0xe0 && first <= 0xef) {
			codePoint = first & 0x0f;
			trailing = 2;
		} else if (first >= 0xf0 && first <= 0xf4) {
			codePoint = first & 0x07;
			trailing = 3;
		} else {
			return false;
		}
		if (index + trailing > bytes.size()) {
			return false;
		}
		for (std::size_t offset = 0; offset < trailing; ++offset) {
			const auto next = static_cast<unsigned char>(bytes[index++]);
			if ((next & 0xc0) != 0x80) {
				return false;
			}
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		if ((trailing == 1 && codePoint < 0x80) ||
			(trailing == 2 && codePoint < 0x800) ||
			(trailing == 3 && codePoint < 0x10000) ||
			!AppendCodePoint(output, codePoint)) {
			return false;
		}
	}
	return true;
}

void AppendUtf8(std::wstring_view input, std::string& output)
{
	for (std::size_t index = 0; index < input.size(); ++index) {
		std::uint32_t codePoint = static_cast<std::uint32_t>(input[index]);
		if constexpr (sizeof(wchar_t) == 2) {
			if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
				const auto trailing = static_cast<std::uint32_t>(input[++index]);
				codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (trailing - 0xdc00);
			}
		}
		if (codePoint <= 0x7f) {
			output.push_back(static_cast<char>(codePoint));
		} else if (codePoint <= 0x7ff) {
			output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
		} else if (codePoint <= 0xffff) {
			output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
		} else {
			output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
		}
	}
}

enum class EUriComponent {
	Authority,
	Path,
	Query,
	Fragment,
};

bool IsAllowedLiteral(wchar_t character, EUriComponent component) noexcept
{
	if (IsUnreserved(character) || IsSubDelimiter(character)) {
		return true;
	}
	switch (component) {
	case EUriComponent::Authority:
		return character == L':' || character == L'@' || character == L'[' || character == L']';
	case EUriComponent::Path:
		return character == L':' || character == L'@' || character == L'/';
	case EUriComponent::Query:
	case EUriComponent::Fragment:
		return character == L':' || character == L'@' || character == L'/' || character == L'?';
	}
	return false;
}

std::wstring EncodeComponent(std::wstring_view input, EUriComponent component)
{
	static constexpr wchar_t digits[] = L"0123456789ABCDEF";
	std::wstring encoded;
	for (std::size_t index = 0; index < input.size();) {
		const wchar_t character = input[index];
		if (character <= 0x7f && IsAllowedLiteral(character, component)) {
			encoded.push_back(character);
			++index;
			continue;
		}
		std::size_t end = index + 1;
		if constexpr (sizeof(wchar_t) == 2) {
			if (static_cast<std::uint32_t>(character) >= 0xd800 && static_cast<std::uint32_t>(character) <= 0xdbff) {
				++end;
			}
		}
		std::string bytes;
		AppendUtf8(input.substr(index, end - index), bytes);
		for (const unsigned char byte : bytes) {
			encoded.push_back(L'%');
			encoded.push_back(digits[byte >> 4]);
			encoded.push_back(digits[byte & 0x0f]);
		}
		index = end;
	}
	return encoded;
}

struct DecodedComponentResult {
	std::optional<std::wstring> value;
	EUriParseError error = EUriParseError::None;

	explicit operator bool() const noexcept { return value.has_value(); }
};

DecodedComponentResult DecodeRawComponent(std::wstring_view encoded, EUriParseError componentError)
{
	if (ContainsInvalidRawUriCharacter(encoded) || !IsValidUtf16(encoded)) {
		return { std::nullopt, componentError };
	}

	std::wstring decoded;
	decoded.reserve(encoded.size());
	for (std::size_t index = 0; index < encoded.size();) {
		if (encoded[index] != L'%') {
			decoded.push_back(encoded[index++]);
			continue;
		}

		std::string bytes;
		while (index < encoded.size() && encoded[index] == L'%') {
			if (index + 2 >= encoded.size()) {
				return { std::nullopt, EUriParseError::InvalidPercentEncoding };
			}
			const int high = HexValue(encoded[index + 1]);
			const int low = HexValue(encoded[index + 2]);
			if (high < 0 || low < 0) {
				return { std::nullopt, EUriParseError::InvalidPercentEncoding };
			}
			bytes.push_back(static_cast<char>((high << 4) | low));
			index += 3;
		}
		if (!DecodeUtf8(bytes, decoded)) {
			return { std::nullopt, EUriParseError::InvalidUtf8 };
		}
	}
	return { std::move(decoded), EUriParseError::None };
}

bool StartsWithCaseInsensitive(std::wstring_view value, std::wstring_view prefix) noexcept
{
	if (value.size() < prefix.size()) {
		return false;
	}
	for (std::size_t index = 0; index < prefix.size(); ++index) {
		if (ToLowerAscii(value[index]) != ToLowerAscii(prefix[index])) {
			return false;
		}
	}
	return true;
}

std::wstring ReplaceSlashWithBackslash(std::wstring value)
{
	for (auto& character : value) {
		if (character == L'/') {
			character = L'\\';
		}
	}
	return value;
}

void AppendKeyPart(std::wstring& key, std::wstring_view value)
{
	key.append(std::to_wstring(value.size()));
	key.push_back(L':');
	key.append(value);
	key.push_back(internal::kComparisonKeySeparator);
}

} // namespace

Uri::Uri(
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::optional<std::wstring> query,
	std::optional<std::wstring> fragment,
	bool hasAuthority
) noexcept
	: m_scheme(std::move(scheme))
	, m_authority(std::move(authority))
	, m_path(std::move(path))
	, m_query(std::move(query))
	, m_fragment(std::move(fragment))
	, m_hasAuthority(hasAuthority)
{
}

UriParseResult Uri::FromComponents(
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::optional<std::wstring> query,
	std::optional<std::wstring> fragment,
	bool hasAuthority
)
{
	if (!ValidateScheme(scheme)) {
		return { std::nullopt, EUriParseError::InvalidScheme };
	}
	if (!ValidateComponent(authority)) {
		return { std::nullopt, EUriParseError::InvalidAuthority };
	}
	if (!hasAuthority && !authority.empty()) {
		return { std::nullopt, EUriParseError::InvalidAuthority };
	}
	if (hasAuthority && !path.empty() && path.front() != L'/') {
		return { std::nullopt, EUriParseError::InvalidPath };
	}
	if (!ValidateComponent(path)) {
		return { std::nullopt, EUriParseError::InvalidPath };
	}
	if (query && !ValidateComponent(*query)) {
		return { std::nullopt, EUriParseError::InvalidQuery };
	}
	if (fragment && !ValidateComponent(*fragment)) {
		return { std::nullopt, EUriParseError::InvalidFragment };
	}
	return { Uri(ToLowerInvariant(scheme), std::move(authority), std::move(path), std::move(query), std::move(fragment), hasAuthority), EUriParseError::None };
}

UriParseResult Uri::Parse(std::wstring_view text)
{
	if (text.empty()) {
		return { std::nullopt, EUriParseError::EmptyInput };
	}
	const auto schemeEnd = text.find(L':');
	const auto firstDelimiter = text.find_first_of(L"/?#");
	if (schemeEnd == std::wstring_view::npos || (firstDelimiter != std::wstring_view::npos && schemeEnd > firstDelimiter)) {
		return { std::nullopt, EUriParseError::MissingScheme };
	}
	const auto scheme = text.substr(0, schemeEnd);
	if (!ValidateScheme(scheme)) {
		return { std::nullopt, EUriParseError::InvalidScheme };
	}

	std::size_t position = schemeEnd + 1;
	bool hasAuthority = false;
	std::wstring authority;
	if (text.substr(position, 2) == L"//") {
		hasAuthority = true;
		position += 2;
		const auto authorityEnd = text.find_first_of(L"/?#", position);
		const auto rawAuthority = text.substr(position, authorityEnd == std::wstring_view::npos ? std::wstring_view::npos : authorityEnd - position);
		auto decodedAuthority = DecodeRawComponent(rawAuthority, EUriParseError::InvalidAuthority);
		if (!decodedAuthority) {
			return { std::nullopt, decodedAuthority.error };
		}
		authority = std::move(*decodedAuthority.value);
		position = authorityEnd == std::wstring_view::npos ? text.size() : authorityEnd;
	}

	const auto queryStart = text.find(L'?', position);
	const auto fragmentStart = text.find(L'#', position);
	const auto pathEnd = std::min(
		queryStart == std::wstring_view::npos ? text.size() : queryStart,
		fragmentStart == std::wstring_view::npos ? text.size() : fragmentStart
	);
	std::wstring path;
	auto decodedPath = DecodeRawComponent(text.substr(position, pathEnd - position), EUriParseError::InvalidPath);
	if (!decodedPath) {
		return { std::nullopt, decodedPath.error };
	}
	path = std::move(*decodedPath.value);

	std::optional<std::wstring> query;
	if (queryStart != std::wstring_view::npos && (fragmentStart == std::wstring_view::npos || queryStart < fragmentStart)) {
		const auto queryEnd = fragmentStart == std::wstring_view::npos ? text.size() : fragmentStart;
		auto decodedQuery = DecodeRawComponent(text.substr(queryStart + 1, queryEnd - queryStart - 1), EUriParseError::InvalidQuery);
		if (!decodedQuery) {
			return { std::nullopt, decodedQuery.error };
		}
		query = std::move(*decodedQuery.value);
	}

	std::optional<std::wstring> fragment;
	if (fragmentStart != std::wstring_view::npos) {
		auto decodedFragment = DecodeRawComponent(text.substr(fragmentStart + 1), EUriParseError::InvalidFragment);
		if (!decodedFragment) {
			return { std::nullopt, decodedFragment.error };
		}
		fragment = std::move(*decodedFragment.value);
	}

	return FromComponents(std::wstring(scheme), std::move(authority), std::move(path), std::move(query), std::move(fragment), hasAuthority);
}

UriParseResult Uri::FromWindowsPath(std::wstring_view windowsPath)
{
	if (windowsPath.empty() || !IsValidUtf16(windowsPath) || HasControlCharacter(windowsPath)) {
		return { std::nullopt, EUriParseError::InvalidWindowsPath };
	}
	// Win32 device namespaces are not file resources and must never escape into
	// the local filesystem provider through an apparently ordinary UNC URI.
	if (StartsWithCaseInsensitive(windowsPath, L"\\\\.\\")) {
		return { std::nullopt, EUriParseError::InvalidWindowsPath };
	}
	if (StartsWithCaseInsensitive(windowsPath, L"\\\\?\\UNC\\")) {
		windowsPath.remove_prefix(8);
		std::wstring normalUnc = L"\\\\";
		normalUnc.append(windowsPath);
		return FromWindowsPath(normalUnc);
	}
	if (StartsWithCaseInsensitive(windowsPath, L"\\\\?\\")) {
		windowsPath.remove_prefix(4);
	}

	if (windowsPath.size() >= 2 && windowsPath[0] == L'\\' && windowsPath[1] == L'\\') {
		const auto firstSeparator = windowsPath.find_first_of(L"\\/", 2);
		if (firstSeparator == std::wstring_view::npos || firstSeparator == 2 || firstSeparator + 1 >= windowsPath.size()
			|| windowsPath[firstSeparator + 1] == L'\\' || windowsPath[firstSeparator + 1] == L'/') {
			return { std::nullopt, EUriParseError::InvalidWindowsPath };
		}
		std::wstring authority(windowsPath.substr(2, firstSeparator - 2));
		std::wstring path(windowsPath.substr(firstSeparator));
		for (auto& character : path) {
			if (character == L'\\') {
				character = L'/';
			}
		}
		return FromComponents(L"file", std::move(authority), std::move(path), std::nullopt, std::nullopt, true);
	}

	if (windowsPath.size() < 3 || !IsAsciiAlpha(windowsPath[0]) || windowsPath[1] != L':' || (windowsPath[2] != L'\\' && windowsPath[2] != L'/')) {
		return { std::nullopt, EUriParseError::InvalidWindowsPath };
	}
	std::wstring path = L"/";
	path.append(windowsPath);
	for (auto& character : path) {
		if (character == L'\\') {
			character = L'/';
		}
	}
	return FromComponents(L"file", L"", std::move(path), std::nullopt, std::nullopt, true);
}

std::wstring Uri::ToString() const
{
	std::wstring result = m_scheme;
	result.push_back(L':');
	if (m_hasAuthority) {
		result.append(L"//");
		result.append(EncodeComponent(m_authority, EUriComponent::Authority));
	}
	result.append(EncodeComponent(m_path, EUriComponent::Path));
	if (m_query) {
		result.push_back(L'?');
		result.append(EncodeComponent(*m_query, EUriComponent::Query));
	}
	if (m_fragment) {
		result.push_back(L'#');
		result.append(EncodeComponent(*m_fragment, EUriComponent::Fragment));
	}
	return result;
}

Uri::WindowsPathResult Uri::ToWindowsPath() const
{
	if (m_scheme != L"file") {
		return { std::nullopt, EUriWindowsPathError::NotFileUri };
	}
	if (!m_hasAuthority || m_authority.empty() || ToLowerInvariant(m_authority) == L"localhost") {
		if (m_path.size() < 3 || m_path[0] != L'/' || !IsAsciiAlpha(m_path[1]) || m_path[2] != L':') {
			return { std::nullopt, EUriWindowsPathError::InvalidFileUri };
		}
		std::wstring path(m_path.substr(1));
		return { ReplaceSlashWithBackslash(std::move(path)), EUriWindowsPathError::None };
	}
	if (m_path.size() < 2 || m_path.front() != L'/' || m_authority.find_first_of(L"\\/#?") != std::wstring::npos) {
		return { std::nullopt, EUriWindowsPathError::InvalidFileUri };
	}
	std::wstring path = L"\\\\";
	path.append(m_authority);
	path.push_back(L'\\');
	path.append(m_path.substr(1));
	return { ReplaceSlashWithBackslash(std::move(path)), EUriWindowsPathError::None };
}

std::wstring UriIdentityService::MakeComparisonKey(const Uri& uri, ENonFileUriCasePolicy nonFilePolicy)
{
	const bool isFile = uri.Scheme() == L"file";
	const bool foldAll = !isFile && nonFilePolicy == ENonFileUriCasePolicy::CaseInsensitive;
	const bool isLocalFileAuthority = isFile
		&& (uri.Authority().empty() || ToLowerInvariant(uri.Authority()) == L"localhost");
	std::wstring key;
	key.reserve(uri.ToString().size() + 32);
	AppendKeyPart(key, ToLowerInvariant(uri.Scheme()));
	// On Windows, file:/C:/x, file:///C:/x, and file://localhost/C:/x
	// address the same local resource.  Preserve authority syntax for non-file
	// schemes while canonicalizing these local file aliases.
	key.push_back(isFile ? L'1' : (uri.HasAuthority() ? L'1' : L'0'));
	key.push_back(internal::kComparisonKeySeparator);
	AppendKeyPart(key, isLocalFileAuthority ? std::wstring{}
		: ((isFile || foldAll) ? ToLowerInvariant(uri.Authority()) : uri.Authority()));
	AppendKeyPart(key, (isFile || foldAll) ? ToLowerInvariant(uri.Path()) : uri.Path());
	key.push_back(uri.Query().has_value() ? L'1' : L'0');
	key.push_back(internal::kComparisonKeySeparator);
	const std::wstring query = uri.Query() ? *uri.Query() : std::wstring{};
	AppendKeyPart(key, foldAll ? ToLowerInvariant(query) : query);
	key.push_back(uri.Fragment().has_value() ? L'1' : L'0');
	key.push_back(internal::kComparisonKeySeparator);
	const std::wstring fragment = uri.Fragment() ? *uri.Fragment() : std::wstring{};
	AppendKeyPart(key, foldAll ? ToLowerInvariant(fragment) : fragment);
	return key;
}

bool UriIdentityService::IsEqual(const Uri& left, const Uri& right, ENonFileUriCasePolicy nonFilePolicy)
{
	return MakeComparisonKey(left, nonFilePolicy) == MakeComparisonKey(right, nonFilePolicy);
}

} // namespace platform::uri
