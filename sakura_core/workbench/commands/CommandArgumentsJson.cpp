/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/commands/CommandArgumentsJson.h"

#include <cstdint>

namespace workbench::commands::json {
namespace {

[[nodiscard]] bool ReadHexQuad(std::string_view text, std::size_t& index, std::uint32_t& value) noexcept
{
	if (index + 4 > text.size()) {
		return false;
	}
	value = 0;
	for (int digit = 0; digit < 4; ++digit) {
		const char character = text[index + static_cast<std::size_t>(digit)];
		std::uint32_t nibble = 0;
		if (character >= '0' && character <= '9') {
			nibble = static_cast<std::uint32_t>(character - '0');
		} else if (character >= 'a' && character <= 'f') {
			nibble = static_cast<std::uint32_t>(character - 'a') + 10u;
		} else if (character >= 'A' && character <= 'F') {
			nibble = static_cast<std::uint32_t>(character - 'A') + 10u;
		} else {
			return false;
		}
		value = (value << 4) | nibble;
	}
	index += 4;
	return true;
}

void AppendCodePointUtf8(std::string& target, std::uint32_t codePoint)
{
	if (codePoint < 0x80u) {
		target += static_cast<char>(codePoint);
	} else if (codePoint < 0x800u) {
		target += static_cast<char>(0xC0u | (codePoint >> 6));
		target += static_cast<char>(0x80u | (codePoint & 0x3Fu));
	} else if (codePoint < 0x10000u) {
		target += static_cast<char>(0xE0u | (codePoint >> 12));
		target += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
		target += static_cast<char>(0x80u | (codePoint & 0x3Fu));
	} else {
		target += static_cast<char>(0xF0u | (codePoint >> 18));
		target += static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu));
		target += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
		target += static_cast<char>(0x80u | (codePoint & 0x3Fu));
	}
}

} // namespace

std::string ToUtf8(std::wstring_view text)
{
	if (text.empty()) {
		return {};
	}
	const int required = ::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0) {
		return {};
	}
	std::string result(static_cast<std::size_t>(required), '\0');
	::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
	return result;
}

std::optional<std::wstring> ToWideStrict(std::string_view text)
{
	if (text.empty()) {
		return std::wstring{};
	}
	const int required = ::MultiByteToWideChar(
		CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (required <= 0) {
		return std::nullopt;
	}
	std::wstring result(static_cast<std::size_t>(required), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
			result.data(), required)
		<= 0) {
		return std::nullopt;
	}
	return result;
}

void AppendEscaped(std::string& target, std::string_view text)
{
	for (const char raw : text) {
		const auto byte = static_cast<unsigned char>(raw);
		switch (raw) {
		case '"':
			target += "\\\"";
			break;
		case '\\':
			target += "\\\\";
			break;
		default:
			if (byte < 0x20u) {
				// Only the control range needs escaping; every other byte is
				// already valid UTF-8 and travels as itself.
				constexpr char kHex[] = "0123456789abcdef";
				target += "\\u00";
				target += kHex[(byte >> 4) & 0x0Fu];
				target += kHex[byte & 0x0Fu];
			} else {
				target += raw;
			}
			break;
		}
	}
}

void AppendQuoted(std::string& target, std::wstring_view text)
{
	target += '"';
	AppendEscaped(target, ToUtf8(text));
	target += '"';
}

void SkipWhitespace(std::string_view text, std::size_t& index) noexcept
{
	while (index < text.size()
		&& (text[index] == ' ' || text[index] == '\t' || text[index] == '\r' || text[index] == '\n')) {
		++index;
	}
}

bool ReadString(std::string_view text, std::size_t& index, std::string& value)
{
	if (index >= text.size() || text[index] != '"') {
		return false;
	}
	++index;
	value.clear();
	while (index < text.size()) {
		const char character = text[index];
		if (character == '"') {
			++index;
			return true;
		}
		if (character != '\\') {
			if (static_cast<unsigned char>(character) < 0x20u) {
				return false;
			}
			value += character;
			++index;
			continue;
		}
		++index;
		if (index >= text.size()) {
			return false;
		}
		const char escape = text[index++];
		switch (escape) {
		case '"': value += '"'; break;
		case '\\': value += '\\'; break;
		case '/': value += '/'; break;
		case 'b': value += '\b'; break;
		case 'f': value += '\f'; break;
		case 'n': value += '\n'; break;
		case 'r': value += '\r'; break;
		case 't': value += '\t'; break;
		case 'u': {
			std::uint32_t unit = 0;
			if (!ReadHexQuad(text, index, unit)) {
				return false;
			}
			if (unit >= 0xDC00u && unit <= 0xDFFFu) {
				// A trailing surrogate with no leading one names no character.
				return false;
			}
			if (unit >= 0xD800u && unit <= 0xDBFFu) {
				if (index + 2 > text.size() || text[index] != '\\' || text[index + 1] != 'u') {
					return false;
				}
				index += 2;
				std::uint32_t low = 0;
				if (!ReadHexQuad(text, index, low) || low < 0xDC00u || low > 0xDFFFu) {
					return false;
				}
				unit = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
			}
			AppendCodePointUtf8(value, unit);
			break;
		}
		default:
			return false;
		}
	}
	return false;
}

bool ReadWideString(std::string_view text, std::size_t& index, std::wstring& value)
{
	std::string raw;
	if (!ReadString(text, index, raw)) {
		return false;
	}
	auto wide = ToWideStrict(raw);
	if (!wide.has_value()) {
		return false;
	}
	value = *std::move(wide);
	return true;
}

bool ReadBoolean(std::string_view text, std::size_t& index, bool& value) noexcept
{
	if (text.substr(index).starts_with("true")) {
		index += 4;
		value = true;
		return true;
	}
	if (text.substr(index).starts_with("false")) {
		index += 5;
		value = false;
		return true;
	}
	return false;
}

bool Expect(std::string_view text, std::size_t& index, char character) noexcept
{
	SkipWhitespace(text, index);
	if (index >= text.size() || text[index] != character) {
		return false;
	}
	++index;
	return true;
}

bool AtEnd(std::string_view text, std::size_t& index) noexcept
{
	SkipWhitespace(text, index);
	return index == text.size();
}

} // namespace workbench::commands::json
