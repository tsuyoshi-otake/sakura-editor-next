/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace platform::serialization {

//! A bounded, UI-independent JSONC value. Object member order is deterministic.
class JsoncValue final {
public:
	using Array = std::vector<JsoncValue>;
	using Object = std::map<std::wstring, JsoncValue, std::less<>>;
	using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::wstring, Array, Object>;

	JsoncValue() = default;
	JsoncValue(std::nullptr_t) noexcept : m_value(std::monostate{}) {}
	JsoncValue(bool value) : m_value(value) {}
	JsoncValue(std::int64_t value) : m_value(value) {}
	JsoncValue(double value) : m_value(value) {}
	JsoncValue(std::wstring value) : m_value(std::move(value)) {}
	JsoncValue(Array value) : m_value(std::move(value)) {}
	JsoncValue(Object value) : m_value(std::move(value)) {}

	[[nodiscard]] const Storage& Value() const noexcept { return m_value; }

private:
	Storage m_value;
};

//! A parse failure never returns a document value. Byte offsets always reference
//! the original caller-provided input, including an optional UTF-8 BOM.
enum class EJsoncDiagnosticCode : std::uint8_t {
	None,
	InputTooLarge,
	InvalidUtf8,
	UnexpectedToken,
	UnexpectedEndOfInput,
	InvalidEscape,
	InvalidNumber,
	DuplicateKey,
	MaximumDepthExceeded,
	MaximumNodesExceeded,
	MaximumStringLengthExceeded,
	MaximumKeyLengthExceeded,
};

struct JsoncDiagnostic final {
	EJsoncDiagnosticCode code = EJsoncDiagnosticCode::None;
	std::size_t byteOffset = 0;
	std::string message;
};

struct JsoncDocumentParseResult final {
	std::optional<JsoncValue> value;
	std::optional<JsoncDiagnostic> diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return value.has_value() && !diagnostic.has_value(); }
};

//! Side-effect-free JSONC reader. It accepts line/block comments and trailing
//! commas, rejects duplicate object keys, and validates UTF-8 before parsing.
class CJsoncDocument final {
public:
	static constexpr std::size_t kMaximumInputBytes = 1024U * 1024U;
	static constexpr std::size_t kMaximumDepth = 64U;
	static constexpr std::size_t kMaximumNodes = 65536U;
	static constexpr std::size_t kMaximumStringLength = 1024U * 1024U;
	static constexpr std::size_t kMaximumObjectKeyLength = 64U * 1024U;

	[[nodiscard]] static JsoncDocumentParseResult Parse(std::string_view utf8);
};

} // namespace platform::serialization
