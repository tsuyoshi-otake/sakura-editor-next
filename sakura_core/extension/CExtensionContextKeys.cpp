/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionContextKeys.h"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <optional>
#include <regex>
#include <vector>

namespace {

bool IsValidKey(std::wstring_view key)
{
	if (key.empty() || key.size() > 512) return false;
	return std::none_of(key.begin(), key.end(), [](wchar_t value) {
		return value == L'\0' || std::iswspace(value) != 0;
	});
}

bool Truthy(const ExtensionContextValue& value)
{
	if (const auto* item = std::get_if<bool>(&value)) return *item;
	if (const auto* item = std::get_if<std::int64_t>(&value)) return *item != 0;
	if (const auto* item = std::get_if<double>(&value)) return *item != 0.0;
	if (const auto* item = std::get_if<std::wstring>(&value)) return !item->empty();
	return false;
}

std::wstring ToText(const ExtensionContextValue& value)
{
	if (const auto* item = std::get_if<bool>(&value)) return *item ? L"true" : L"false";
	if (const auto* item = std::get_if<std::int64_t>(&value)) return std::to_wstring(*item);
	if (const auto* item = std::get_if<double>(&value)) return std::to_wstring(*item);
	if (const auto* item = std::get_if<std::wstring>(&value)) return *item;
	return {};
}

enum class TokenKind {
	End,
	Identifier,
	Literal,
	Not,
	And,
	Or,
	Equal,
	NotEqual,
	Regex,
	LeftParen,
	RightParen,
	Invalid,
};

struct Token {
	TokenKind kind = TokenKind::Invalid;
	std::wstring text;
};

class Lexer final {
public:
	explicit Lexer(std::wstring_view source) : m_source(source) {}

	Token Next()
	{
		while (m_offset < m_source.size() && std::iswspace(m_source[m_offset])) ++m_offset;
		if (m_offset >= m_source.size()) return { TokenKind::End, {} };
		if (Consume(L"!==") || Consume(L"!=")) return { TokenKind::NotEqual, {} };
		if (Consume(L"===") || Consume(L"==")) return { TokenKind::Equal, {} };
		if (Consume(L"=~")) return { TokenKind::Regex, {} };
		if (Consume(L"&&")) return { TokenKind::And, {} };
		if (Consume(L"||")) return { TokenKind::Or, {} };
		const wchar_t current = m_source[m_offset++];
		switch (current) {
		case L'!': return { TokenKind::Not, {} };
		case L'(': return { TokenKind::LeftParen, {} };
		case L')': return { TokenKind::RightParen, {} };
		case L'\'':
		case L'"': return ReadQuoted(current);
		case L'/': return ReadRegex();
		default: break;
		}
		const std::size_t start = m_offset - 1;
		while (m_offset < m_source.size()) {
			const wchar_t value = m_source[m_offset];
			if (std::iswspace(value) || value == L'(' || value == L')' || value == L'!' ||
				value == L'=' || value == L'&' || value == L'|') break;
			++m_offset;
		}
		if (m_offset == start) return { TokenKind::Invalid, {} };
		return { TokenKind::Identifier, std::wstring(m_source.substr(start, m_offset - start)) };
	}

private:
	bool Consume(std::wstring_view value)
	{
		if (m_source.substr(m_offset, value.size()) != value) return false;
		m_offset += value.size();
		return true;
	}

	Token ReadQuoted(wchar_t quote)
	{
		std::wstring result;
		while (m_offset < m_source.size()) {
			const wchar_t value = m_source[m_offset++];
			if (value == quote) return { TokenKind::Literal, std::move(result) };
			if (value == L'\\' && m_offset < m_source.size()) result.push_back(m_source[m_offset++]);
			else result.push_back(value);
		}
		return { TokenKind::Invalid, {} };
	}

	Token ReadRegex()
	{
		std::wstring result;
		bool escaped = false;
		while (m_offset < m_source.size()) {
			const wchar_t value = m_source[m_offset++];
			if (!escaped && value == L'/') {
				if (m_offset < m_source.size() && m_source[m_offset] == L'i') {
					result.insert(result.begin(), L'\x0001');
					++m_offset;
				}
				return { TokenKind::Literal, std::move(result) };
			}
			escaped = !escaped && value == L'\\';
			result.push_back(value);
			if (value != L'\\') escaped = false;
		}
		return { TokenKind::Invalid, {} };
	}

	std::wstring_view m_source;
	std::size_t m_offset = 0;
};

class Parser final {
public:
	Parser(std::wstring_view source, const CExtensionContextKeys& values)
		: m_lexer(source), m_values(values), m_current(m_lexer.Next()) {}

	std::optional<bool> Parse()
	{
		auto result = ParseOr();
		if (!result || m_current.kind != TokenKind::End) return std::nullopt;
		return result;
	}

private:
	std::optional<bool> ParseOr()
	{
		auto left = ParseAnd();
		while (left && m_current.kind == TokenKind::Or) {
			Advance();
			auto right = ParseAnd();
			if (!right) return std::nullopt;
			left = *left || *right;
		}
		return left;
	}

	std::optional<bool> ParseAnd()
	{
		auto left = ParseUnary();
		while (left && m_current.kind == TokenKind::And) {
			Advance();
			auto right = ParseUnary();
			if (!right) return std::nullopt;
			left = *left && *right;
		}
		return left;
	}

	std::optional<bool> ParseUnary()
	{
		if (m_current.kind == TokenKind::Not) {
			Advance();
			auto value = ParseUnary();
			return value ? std::optional<bool>{ !*value } : std::nullopt;
		}
		if (m_current.kind == TokenKind::LeftParen) {
			Advance();
			auto value = ParseOr();
			if (!value || m_current.kind != TokenKind::RightParen) return std::nullopt;
			Advance();
			return value;
		}
		return ParseComparison();
	}

	std::optional<bool> ParseComparison()
	{
		if (m_current.kind != TokenKind::Identifier) return std::nullopt;
		const std::wstring key = m_current.text;
		const auto left = m_values.Get(key);
		Advance();
		const TokenKind operation = m_current.kind;
		if (operation != TokenKind::Equal && operation != TokenKind::NotEqual && operation != TokenKind::Regex) {
			return Truthy(left);
		}
		Advance();
		if (m_current.kind != TokenKind::Identifier && m_current.kind != TokenKind::Literal) return std::nullopt;
		const std::wstring right = m_current.text;
		Advance();
		const std::wstring leftText = ToText(left);
		if (operation == TokenKind::Equal) return leftText == right;
		if (operation == TokenKind::NotEqual) return leftText != right;
		try {
			bool insensitive = !right.empty() && right.front() == L'\x0001';
			const std::wstring_view pattern = insensitive ? std::wstring_view(right).substr(1) : std::wstring_view(right);
			const auto flags = std::regex_constants::ECMAScript |
				(insensitive ? std::regex_constants::icase : std::regex_constants::syntax_option_type{});
			return std::regex_search(leftText, std::wregex(pattern.begin(), pattern.end(), flags));
		} catch (const std::regex_error&) {
			return std::nullopt;
		}
	}

	void Advance() { m_current = m_lexer.Next(); }

	Lexer m_lexer;
	const CExtensionContextKeys& m_values;
	Token m_current;
};

} // namespace

bool CExtensionContextKeys::Set(std::wstring key, ExtensionContextValue value, std::wstring ownerExtensionId)
{
	if (!IsValidKey(key)) return false;
	std::unique_lock lock(m_mutex);
	m_values.insert_or_assign(std::move(key), Entry{ std::move(value), std::move(ownerExtensionId) });
	return true;
}

bool CExtensionContextKeys::Remove(std::wstring_view key, std::wstring_view ownerExtensionId)
{
	std::unique_lock lock(m_mutex);
	const auto found = m_values.find(std::wstring(key));
	if (found == m_values.end()) return false;
	if (!ownerExtensionId.empty() && found->second.ownerExtensionId != ownerExtensionId) return false;
	m_values.erase(found);
	return true;
}

void CExtensionContextKeys::RemoveOwnedBy(std::wstring_view ownerExtensionId)
{
	if (ownerExtensionId.empty()) return;
	std::unique_lock lock(m_mutex);
	std::erase_if(m_values, [ownerExtensionId](const auto& pair) {
		return pair.second.ownerExtensionId == ownerExtensionId;
	});
}

void CExtensionContextKeys::Clear()
{
	std::unique_lock lock(m_mutex);
	m_values.clear();
}

bool CExtensionContextKeys::Contains(std::wstring_view key) const
{
	std::shared_lock lock(m_mutex);
	return m_values.contains(std::wstring(key));
}

ExtensionContextValue CExtensionContextKeys::Get(std::wstring_view key) const
{
	std::shared_lock lock(m_mutex);
	const auto found = m_values.find(std::wstring(key));
	return found == m_values.end() ? ExtensionContextValue{} : found->second.value;
}

bool CExtensionContextKeys::Evaluate(std::wstring_view clause) const
{
	if (clause.empty()) return true;
	const auto value = Parser(clause, *this).Parse();
	return value.value_or(false);
}
