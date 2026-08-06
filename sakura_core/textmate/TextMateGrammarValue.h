/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace textmate {

//! A format-neutral parse tree for a TextMate grammar document.
//!
//! `.tmLanguage.json` (JSON/JSONC) and `.tmLanguage` (classic Apple property
//! list, XML) are two different serializations of the same grammar shape.
//! `TextMateGrammarValue` is the common tree both format-specific loaders
//! (`TextMateJsonGrammarLoader`, `TextMatePlistGrammarLoader`) produce, so the
//! single `TextMateGrammarCompiler` that turns a raw tree into a compiled
//! `Grammar` (see `TextMateGrammarModel.h`) never needs to know which source
//! format it came from. Object member order follows source document order,
//! which matters for `patterns` arrays (first-match-wins ordering) even
//! though it does not matter for `repository` lookups.
class TextMateGrammarValue final {
public:
	using Array = std::vector<TextMateGrammarValue>;
	//! Preserves source order; `repository`/pattern object keys are looked up
	//! by name, so ordinary map semantics are not required, but keeping
	//! source order makes diagnostics and round-trip testing predictable.
	using Object = std::vector<std::pair<std::wstring, TextMateGrammarValue>>;
	using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::wstring, Array, Object>;

	TextMateGrammarValue() = default;
	TextMateGrammarValue(std::nullptr_t) noexcept : m_value(std::monostate{}) {}
	TextMateGrammarValue(bool value) : m_value(value) {}
	TextMateGrammarValue(std::int64_t value) : m_value(value) {}
	TextMateGrammarValue(double value) : m_value(value) {}
	TextMateGrammarValue(std::wstring value) : m_value(std::move(value)) {}
	TextMateGrammarValue(Array value) : m_value(std::move(value)) {}
	TextMateGrammarValue(Object value) : m_value(std::move(value)) {}

	[[nodiscard]] bool IsNull() const noexcept { return std::holds_alternative<std::monostate>(m_value); }
	[[nodiscard]] bool IsBool() const noexcept { return std::holds_alternative<bool>(m_value); }
	[[nodiscard]] bool IsInteger() const noexcept { return std::holds_alternative<std::int64_t>(m_value); }
	[[nodiscard]] bool IsDouble() const noexcept { return std::holds_alternative<double>(m_value); }
	[[nodiscard]] bool IsString() const noexcept { return std::holds_alternative<std::wstring>(m_value); }
	[[nodiscard]] bool IsArray() const noexcept { return std::holds_alternative<Array>(m_value); }
	[[nodiscard]] bool IsObject() const noexcept { return std::holds_alternative<Object>(m_value); }

	[[nodiscard]] bool AsBool(bool fallback = false) const noexcept
	{
		const auto* value = std::get_if<bool>(&m_value);
		return value ? *value : fallback;
	}
	//! TextMate JSON grammars sometimes encode booleans as 0/1 integers
	//! (notably `applyEndPatternLast`), so integer truthiness is accepted too.
	[[nodiscard]] bool AsTruthy(bool fallback = false) const noexcept
	{
		if (const auto* value = std::get_if<bool>(&m_value)) return *value;
		if (const auto* value = std::get_if<std::int64_t>(&m_value)) return *value != 0;
		return fallback;
	}
	[[nodiscard]] const std::wstring* AsString() const noexcept { return std::get_if<std::wstring>(&m_value); }
	[[nodiscard]] std::wstring AsStringOr(std::wstring_view fallback) const
	{
		const auto* value = std::get_if<std::wstring>(&m_value);
		return value ? *value : std::wstring(fallback);
	}
	[[nodiscard]] const Array* AsArray() const noexcept { return std::get_if<Array>(&m_value); }
	[[nodiscard]] const Object* AsObject() const noexcept { return std::get_if<Object>(&m_value); }

	//! Linear lookup by key. Grammar objects (`patterns` entries, `repository`
	//! members, capture maps) are small enough that this is not a hot path;
	//! keeping `Object` an order-preserving vector instead of a map keeps
	//! `patterns` array semantics and `repository` semantics expressible with
	//! one type.
	[[nodiscard]] const TextMateGrammarValue* Find(std::wstring_view key) const noexcept
	{
		const auto* object = AsObject();
		if (!object) return nullptr;
		for (const auto& [memberKey, memberValue] : *object) {
			if (memberKey == key) return &memberValue;
		}
		return nullptr;
	}

	[[nodiscard]] const Storage& Value() const noexcept { return m_value; }

private:
	Storage m_value;
};

} // namespace textmate
