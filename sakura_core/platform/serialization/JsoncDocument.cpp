/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "platform/serialization/JsoncDocument.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <system_error>
#include <utility>

namespace platform::serialization {
namespace {

class JsoncParser final {
public:
	explicit JsoncParser(std::string_view input) : m_input(input) {}

	std::optional<JsoncValue> ParseDocument()
	{
		SkipTrivia();
		auto value = ParseValue(0);
		if (!value || m_diagnostic) return std::nullopt;
		SkipTrivia();
		if (m_position != m_input.size()) {
			Fail(EJsoncDiagnosticCode::UnexpectedToken, "unexpected token after root value");
			return std::nullopt;
		}
		return value;
	}

	std::optional<JsoncDiagnostic> TakeDiagnostic() { return std::move(m_diagnostic); }

private:
	bool Has(std::size_t count = 1) const noexcept { return count <= m_input.size() - m_position; }
	char Peek() const noexcept { return Has() ? m_input[m_position] : '\0'; }

	void Fail(EJsoncDiagnosticCode code, const char* message)
	{
		if (!m_diagnostic) m_diagnostic = JsoncDiagnostic { code, m_position, message };
	}

	bool Consume(char expected)
	{
		if (Peek() == expected) {
			++m_position;
			return true;
		}
		Fail(Has() ? EJsoncDiagnosticCode::UnexpectedToken : EJsoncDiagnosticCode::UnexpectedEndOfInput, "unexpected token");
		return false;
	}

	void SkipTrivia()
	{
		while (!m_diagnostic) {
			while (Peek() == ' ' || Peek() == '\t' || Peek() == '\r' || Peek() == '\n') ++m_position;
			if (Peek() != '/' || !Has(2)) return;
			if (m_input[m_position + 1] == '/') {
				m_position += 2;
				while (Has() && Peek() != '\r' && Peek() != '\n') ++m_position;
				continue;
			}
			if (m_input[m_position + 1] != '*') return;
			m_position += 2;
			while (Has(2) && !(Peek() == '*' && m_input[m_position + 1] == '/')) ++m_position;
			if (!Has(2)) {
				Fail(EJsoncDiagnosticCode::UnexpectedEndOfInput, "unterminated block comment");
				return;
			}
			m_position += 2;
		}
	}

	bool CountNode()
	{
		if (++m_nodes <= CJsoncDocument::kMaximumNodes) return true;
		Fail(EJsoncDiagnosticCode::MaximumNodesExceeded, "JSONC node limit exceeded");
		return false;
	}

	std::optional<JsoncValue> ParseValue(std::size_t depth)
	{
		SkipTrivia();
		if (depth > CJsoncDocument::kMaximumDepth) {
			Fail(EJsoncDiagnosticCode::MaximumDepthExceeded, "JSONC nesting depth exceeded");
			return std::nullopt;
		}
		if (!CountNode()) return std::nullopt;
		switch (Peek()) {
		case '{': return ParseObject(depth + 1);
		case '[': return ParseArray(depth + 1);
		case '"': {
			auto string = ParseString(false);
			return string ? std::optional<JsoncValue>(JsoncValue(std::move(*string))) : std::nullopt;
		}
		case 't': return ConsumeLiteral("true", JsoncValue(true));
		case 'f': return ConsumeLiteral("false", JsoncValue(false));
		case 'n': return ConsumeLiteral("null", JsoncValue(nullptr));
		default:
			if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) return ParseNumber();
			Fail(Has() ? EJsoncDiagnosticCode::UnexpectedToken : EJsoncDiagnosticCode::UnexpectedEndOfInput, "expected JSON value");
			return std::nullopt;
		}
	}

	std::optional<JsoncValue> ConsumeLiteral(std::string_view literal, JsoncValue value)
	{
		if (m_input.substr(m_position, literal.size()) != literal) {
			Fail(Has() ? EJsoncDiagnosticCode::UnexpectedToken : EJsoncDiagnosticCode::UnexpectedEndOfInput, "invalid JSON literal");
			return std::nullopt;
		}
		m_position += literal.size();
		return value;
	}

	static bool HexValue(char character, std::uint16_t& value) noexcept
	{
		if (character >= '0' && character <= '9') { value = static_cast<std::uint16_t>(character - '0'); return true; }
		if (character >= 'a' && character <= 'f') { value = static_cast<std::uint16_t>(character - 'a' + 10); return true; }
		if (character >= 'A' && character <= 'F') { value = static_cast<std::uint16_t>(character - 'A' + 10); return true; }
		return false;
	}

	bool AppendCodePoint(std::wstring& output, std::uint32_t codePoint)
	{
		if (codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
			Fail(EJsoncDiagnosticCode::InvalidUtf8, "invalid Unicode code point");
			return false;
		}
		if (codePoint <= 0xffffU) {
			output.push_back(static_cast<wchar_t>(codePoint));
			return true;
		}
		codePoint -= 0x10000U;
		output.push_back(static_cast<wchar_t>(0xd800U + (codePoint >> 10U)));
		output.push_back(static_cast<wchar_t>(0xdc00U + (codePoint & 0x3ffU)));
		return true;
	}

	bool DecodeUtf8(std::wstring& output)
	{
		const auto offset = m_position;
		const auto first = static_cast<unsigned char>(m_input[m_position++]);
		if (first < 0x80U) return AppendCodePoint(output, first);
		std::size_t continuationCount = 0;
		std::uint32_t codePoint = 0;
		std::uint32_t minimum = 0;
		if (first >= 0xc2U && first <= 0xdfU) { continuationCount = 1; codePoint = first & 0x1fU; minimum = 0x80U; }
		else if (first >= 0xe0U && first <= 0xefU) { continuationCount = 2; codePoint = first & 0x0fU; minimum = 0x800U; }
		else if (first >= 0xf0U && first <= 0xf4U) { continuationCount = 3; codePoint = first & 0x07U; minimum = 0x10000U; }
		else { m_position = offset; Fail(EJsoncDiagnosticCode::InvalidUtf8, "invalid UTF-8 leading byte"); return false; }
		if (!Has(continuationCount)) { m_position = offset; Fail(EJsoncDiagnosticCode::InvalidUtf8, "truncated UTF-8 sequence"); return false; }
		for (std::size_t index = 0; index < continuationCount; ++index) {
			const auto next = static_cast<unsigned char>(m_input[m_position++]);
			if ((next & 0xc0U) != 0x80U) { m_position = offset; Fail(EJsoncDiagnosticCode::InvalidUtf8, "invalid UTF-8 continuation byte"); return false; }
			codePoint = (codePoint << 6U) | (next & 0x3fU);
		}
		if (codePoint < minimum) { m_position = offset; Fail(EJsoncDiagnosticCode::InvalidUtf8, "overlong UTF-8 sequence"); return false; }
		return AppendCodePoint(output, codePoint);
	}

	std::optional<std::wstring> ParseString(bool objectKey)
	{
		if (!Consume('"')) return std::nullopt;
		std::wstring result;
		const auto maximum = objectKey ? CJsoncDocument::kMaximumObjectKeyLength : CJsoncDocument::kMaximumStringLength;
		while (Has()) {
			const auto character = Peek();
			if (character == '"') {
				++m_position;
				if (result.size() <= maximum) return result;
				Fail(objectKey ? EJsoncDiagnosticCode::MaximumKeyLengthExceeded : EJsoncDiagnosticCode::MaximumStringLengthExceeded, "JSONC string length limit exceeded");
				return std::nullopt;
			}
			if (static_cast<unsigned char>(character) < 0x20U) { Fail(EJsoncDiagnosticCode::UnexpectedToken, "unescaped control character in JSON string"); return std::nullopt; }
			if (character != '\\') {
				if (!DecodeUtf8(result)) return std::nullopt;
			} else {
				++m_position;
				if (!Has()) { Fail(EJsoncDiagnosticCode::UnexpectedEndOfInput, "truncated JSON escape"); return std::nullopt; }
				switch (m_input[m_position++]) {
				case '"': result.push_back(L'"'); break;
				case '\\': result.push_back(L'\\'); break;
				case '/': result.push_back(L'/'); break;
				case 'b': result.push_back(L'\b'); break;
				case 'f': result.push_back(L'\f'); break;
				case 'n': result.push_back(L'\n'); break;
				case 'r': result.push_back(L'\r'); break;
				case 't': result.push_back(L'\t'); break;
				case 'u': {
					if (!Has(4)) { Fail(EJsoncDiagnosticCode::UnexpectedEndOfInput, "truncated Unicode escape"); return std::nullopt; }
					std::uint16_t unit = 0;
					for (std::size_t index = 0; index < 4; ++index) { std::uint16_t digit = 0; if (!HexValue(m_input[m_position++], digit)) { Fail(EJsoncDiagnosticCode::InvalidEscape, "invalid Unicode escape"); return std::nullopt; } unit = static_cast<std::uint16_t>((unit << 4U) | digit); }
					if (unit >= 0xd800U && unit <= 0xdbffU) {
						if (!Has(6) || m_input[m_position] != '\\' || m_input[m_position + 1] != 'u') { Fail(EJsoncDiagnosticCode::InvalidEscape, "high surrogate must be followed by a low surrogate"); return std::nullopt; }
						m_position += 2;
						std::uint16_t low = 0;
						for (std::size_t index = 0; index < 4; ++index) { std::uint16_t digit = 0; if (!HexValue(m_input[m_position++], digit)) { Fail(EJsoncDiagnosticCode::InvalidEscape, "invalid Unicode escape"); return std::nullopt; } low = static_cast<std::uint16_t>((low << 4U) | digit); }
						if (low < 0xdc00U || low > 0xdfffU) { Fail(EJsoncDiagnosticCode::InvalidEscape, "invalid low surrogate"); return std::nullopt; }
						result.push_back(static_cast<wchar_t>(unit)); result.push_back(static_cast<wchar_t>(low));
					} else if (unit >= 0xdc00U && unit <= 0xdfffU) { Fail(EJsoncDiagnosticCode::InvalidEscape, "unexpected low surrogate"); return std::nullopt; }
					else result.push_back(static_cast<wchar_t>(unit));
					break;
				}
				default: Fail(EJsoncDiagnosticCode::InvalidEscape, "invalid JSON escape"); return std::nullopt;
				}
			}
			if (result.size() > maximum) { Fail(objectKey ? EJsoncDiagnosticCode::MaximumKeyLengthExceeded : EJsoncDiagnosticCode::MaximumStringLengthExceeded, "JSONC string length limit exceeded"); return std::nullopt; }
		}
		Fail(EJsoncDiagnosticCode::UnexpectedEndOfInput, "unterminated JSON string");
		return std::nullopt;
	}

	std::optional<JsoncValue> ParseObject(std::size_t depth)
	{
		if (!Consume('{')) return std::nullopt;
		JsoncValue::Object object;
		SkipTrivia();
		if (Peek() == '}') { ++m_position; return JsoncValue(std::move(object)); }
		while (!m_diagnostic) {
			SkipTrivia(); auto key = ParseString(true); if (!key) return std::nullopt;
			SkipTrivia(); if (!Consume(':')) return std::nullopt;
			auto value = ParseValue(depth); if (!value) return std::nullopt;
			if (!object.emplace(std::move(*key), std::move(*value)).second) { Fail(EJsoncDiagnosticCode::DuplicateKey, "duplicate JSON object key"); return std::nullopt; }
			SkipTrivia();
			if (Peek() == '}') { ++m_position; return JsoncValue(std::move(object)); }
			if (!Consume(',')) return std::nullopt;
			SkipTrivia();
			if (Peek() == '}') { ++m_position; return JsoncValue(std::move(object)); }
		}
		return std::nullopt;
	}

	std::optional<JsoncValue> ParseArray(std::size_t depth)
	{
		if (!Consume('[')) return std::nullopt;
		JsoncValue::Array array;
		SkipTrivia();
		if (Peek() == ']') { ++m_position; return JsoncValue(std::move(array)); }
		while (!m_diagnostic) {
			auto value = ParseValue(depth); if (!value) return std::nullopt;
			array.emplace_back(std::move(*value));
			SkipTrivia();
			if (Peek() == ']') { ++m_position; return JsoncValue(std::move(array)); }
			if (!Consume(',')) return std::nullopt;
			SkipTrivia();
			if (Peek() == ']') { ++m_position; return JsoncValue(std::move(array)); }
		}
		return std::nullopt;
	}

	std::optional<JsoncValue> ParseNumber()
	{
		const auto begin = m_position;
		if (Peek() == '-') ++m_position;
		if (Peek() == '0') { ++m_position; if (Peek() >= '0' && Peek() <= '9') { Fail(EJsoncDiagnosticCode::InvalidNumber, "leading zero in JSON number"); return std::nullopt; } }
		else if (Peek() >= '1' && Peek() <= '9') { do { ++m_position; } while (Peek() >= '0' && Peek() <= '9'); }
		else { Fail(EJsoncDiagnosticCode::InvalidNumber, "invalid JSON number"); return std::nullopt; }
		bool real = false;
		if (Peek() == '.') { real = true; ++m_position; const auto fraction = m_position; while (Peek() >= '0' && Peek() <= '9') ++m_position; if (fraction == m_position) { Fail(EJsoncDiagnosticCode::InvalidNumber, "missing JSON fractional digits"); return std::nullopt; } }
		if (Peek() == 'e' || Peek() == 'E') { real = true; ++m_position; if (Peek() == '+' || Peek() == '-') ++m_position; const auto exponent = m_position; while (Peek() >= '0' && Peek() <= '9') ++m_position; if (exponent == m_position) { Fail(EJsoncDiagnosticCode::InvalidNumber, "missing JSON exponent digits"); return std::nullopt; } }
		const auto text = m_input.substr(begin, m_position - begin);
		if (!real) { std::int64_t integer = 0; const auto converted = std::from_chars(text.data(), text.data() + text.size(), integer); if (converted.ec == std::errc{} && converted.ptr == text.data() + text.size()) return JsoncValue(integer); }
		double number = 0; const auto converted = std::from_chars(text.data(), text.data() + text.size(), number);
		if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || !std::isfinite(number)) { Fail(EJsoncDiagnosticCode::InvalidNumber, "JSON number is outside the supported finite range"); return std::nullopt; }
		return JsoncValue(number);
	}

	std::string_view m_input;
	std::size_t m_position = 0;
	std::size_t m_nodes = 0;
	std::optional<JsoncDiagnostic> m_diagnostic;
};

bool ValidateUtf8(std::string_view input, JsoncDiagnostic& diagnostic)
{
	for (std::size_t position = 0; position < input.size();) {
		const auto offset = position;
		const auto first = static_cast<unsigned char>(input[position++]);
		if (first < 0x80U) continue;
		std::size_t continuationCount = 0; std::uint32_t codePoint = 0; std::uint32_t minimum = 0;
		if (first >= 0xc2U && first <= 0xdfU) { continuationCount = 1; codePoint = first & 0x1fU; minimum = 0x80U; }
		else if (first >= 0xe0U && first <= 0xefU) { continuationCount = 2; codePoint = first & 0x0fU; minimum = 0x800U; }
		else if (first >= 0xf0U && first <= 0xf4U) { continuationCount = 3; codePoint = first & 0x07U; minimum = 0x10000U; }
		else { diagnostic = { EJsoncDiagnosticCode::InvalidUtf8, offset, "invalid UTF-8 leading byte" }; return false; }
		if (continuationCount > input.size() - position) { diagnostic = { EJsoncDiagnosticCode::InvalidUtf8, offset, "truncated UTF-8 sequence" }; return false; }
		for (std::size_t index = 0; index < continuationCount; ++index) { const auto next = static_cast<unsigned char>(input[position++]); if ((next & 0xc0U) != 0x80U) { diagnostic = { EJsoncDiagnosticCode::InvalidUtf8, offset, "invalid UTF-8 continuation byte" }; return false; } codePoint = (codePoint << 6U) | (next & 0x3fU); }
		if (codePoint < minimum || codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)) { diagnostic = { EJsoncDiagnosticCode::InvalidUtf8, offset, "invalid UTF-8 code point" }; return false; }
	}
	return true;
}

} // namespace

JsoncDocumentParseResult CJsoncDocument::Parse(std::string_view utf8)
{
	if (utf8.size() > kMaximumInputBytes) return { std::nullopt, JsoncDiagnostic { EJsoncDiagnosticCode::InputTooLarge, 0, "JSONC input exceeds the configured byte limit" } };
	JsoncDiagnostic utf8Diagnostic;
	if (!ValidateUtf8(utf8, utf8Diagnostic)) return { std::nullopt, std::move(utf8Diagnostic) };
	std::size_t byteOrderMarkBytes = 0;
	if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xefU && static_cast<unsigned char>(utf8[1]) == 0xbbU && static_cast<unsigned char>(utf8[2]) == 0xbfU) {
		byteOrderMarkBytes = 3;
		utf8.remove_prefix(byteOrderMarkBytes);
	}
	JsoncParser parser(utf8);
	auto value = parser.ParseDocument();
	if (value) return { std::move(value), std::nullopt };
	auto diagnostic = parser.TakeDiagnostic();
	if (!diagnostic) diagnostic = JsoncDiagnostic { EJsoncDiagnosticCode::UnexpectedToken, 0, "JSONC parsing failed" };
	diagnostic->byteOffset += byteOrderMarkBytes;
	return { std::nullopt, std::move(diagnostic) };
}

} // namespace platform::serialization
