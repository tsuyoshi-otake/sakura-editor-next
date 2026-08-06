/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/commands/ApiCommandArguments.h"

#include "workbench/commands/CommandArgumentsJson.h"

#include <utility>

namespace workbench::commands {
namespace {

//! One element of the array, bounded. An over-long string fails the whole list.
[[nodiscard]] bool ReadBoundedString(std::string_view text, std::size_t& index, std::wstring& value)
{
	json::SkipWhitespace(text, index);
	if (!json::ReadWideString(text, index, value)) {
		return false;
	}
	return value.size() <= kMaximumApiCommandStringLength;
}

//!
//! @brief Consume the array separator, or report that the array ended.
//!
//! `more` is true when another element follows. Upstream's optional trailing
//! arguments are simply absent from the serialized array, so ending early is a
//! valid list rather than a truncated one.
//!
[[nodiscard]] bool ReadSeparatorOrEnd(std::string_view text, std::size_t& index, bool& more) noexcept
{
	json::SkipWhitespace(text, index);
	if (index >= text.size()) {
		return false;
	}
	if (text[index] == ']') {
		++index;
		more = false;
		return true;
	}
	if (text[index] != ',') {
		return false;
	}
	++index;
	more = true;
	return true;
}

//! `{}` or `{"override":<bool>}`. Any other member is a request this does not
//! understand, and acting on the part it did understand would be a guess.
[[nodiscard]] bool ReadShowOptions(std::string_view text, std::size_t& index, std::optional<bool>& value)
{
	if (!json::Expect(text, index, '{')) {
		return false;
	}
	json::SkipWhitespace(text, index);
	if (index < text.size() && text[index] == '}') {
		++index;
		value.reset();
		return true;
	}
	std::string key;
	if (!json::ReadString(text, index, key) || key != "override") {
		return false;
	}
	if (!json::Expect(text, index, ':')) {
		return false;
	}
	json::SkipWhitespace(text, index);
	bool decoded = false;
	if (!json::ReadBoolean(text, index, decoded)) {
		return false;
	}
	value = decoded;
	return json::Expect(text, index, '}');
}

} // namespace

std::string BuildApiDiffArguments(const ApiDiffArguments& arguments)
{
	std::string json;
	json += '[';
	json::AppendQuoted(json, arguments.originalUri);
	json += ',';
	json::AppendQuoted(json, arguments.modifiedUri);
	json += ',';
	json::AppendQuoted(json, arguments.title);
	json += ']';
	return json;
}

std::optional<ApiDiffArguments> ParseApiDiffArguments(std::string_view argumentsJson)
{
	std::size_t index = 0;
	if (!json::Expect(argumentsJson, index, '[')) {
		return std::nullopt;
	}

	ApiDiffArguments arguments;
	if (!ReadBoundedString(argumentsJson, index, arguments.originalUri)) {
		return std::nullopt;
	}
	bool more = false;
	if (!ReadSeparatorOrEnd(argumentsJson, index, more) || !more) {
		// A comparison needs both sides. One URI is not a shorter `vscode.diff`.
		return std::nullopt;
	}
	if (!ReadBoundedString(argumentsJson, index, arguments.modifiedUri)) {
		return std::nullopt;
	}
	if (!ReadSeparatorOrEnd(argumentsJson, index, more)) {
		return std::nullopt;
	}
	if (more) {
		if (!ReadBoundedString(argumentsJson, index, arguments.title)) {
			return std::nullopt;
		}
		if (!ReadSeparatorOrEnd(argumentsJson, index, more) || more) {
			return std::nullopt;
		}
	}
	return json::AtEnd(argumentsJson, index) ? std::optional{ std::move(arguments) } : std::nullopt;
}

std::string BuildApiOpenArguments(const ApiOpenArguments& arguments)
{
	std::string json;
	json += '[';
	json::AppendQuoted(json, arguments.resourceUri);
	json += ',';
	// `JSON.stringify({ override: undefined })` is `{}`, so an absent override
	// travels as an empty object rather than as a member whose value is null.
	if (arguments.overrideEditor.has_value()) {
		json += "{\"override\":";
		json += *arguments.overrideEditor ? "true" : "false";
		json += '}';
	} else {
		json += "{}";
	}
	json += ',';
	json::AppendQuoted(json, arguments.label);
	json += ']';
	return json;
}

std::optional<ApiOpenArguments> ParseApiOpenArguments(std::string_view argumentsJson)
{
	std::size_t index = 0;
	if (!json::Expect(argumentsJson, index, '[')) {
		return std::nullopt;
	}

	ApiOpenArguments arguments;
	if (!ReadBoundedString(argumentsJson, index, arguments.resourceUri)) {
		return std::nullopt;
	}
	bool more = false;
	if (!ReadSeparatorOrEnd(argumentsJson, index, more)) {
		return std::nullopt;
	}
	if (more) {
		if (!ReadShowOptions(argumentsJson, index, arguments.overrideEditor)) {
			return std::nullopt;
		}
		if (!ReadSeparatorOrEnd(argumentsJson, index, more)) {
			return std::nullopt;
		}
	}
	if (more) {
		if (!ReadBoundedString(argumentsJson, index, arguments.label)) {
			return std::nullopt;
		}
		if (!ReadSeparatorOrEnd(argumentsJson, index, more) || more) {
			return std::nullopt;
		}
	}
	return json::AtEnd(argumentsJson, index) ? std::optional{ std::move(arguments) } : std::nullopt;
}

} // namespace workbench::commands
