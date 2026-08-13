/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/commands/ExplorerCommandArguments.h"

#include "workbench/commands/CommandArgumentsJson.h"

#include <utility>

namespace workbench::commands {

std::string BuildExplorerResourceArguments(const ExplorerResourceArguments& arguments)
{
	std::string json;
	json += '[';
	json::AppendQuoted(json, arguments.resourceUri);
	json += ']';
	return json;
}

std::optional<ExplorerResourceArguments> ParseExplorerResourceArguments(std::string_view argumentsJson)
{
	std::size_t index = 0;
	if (!json::Expect(argumentsJson, index, '[')) {
		return std::nullopt;
	}

	ExplorerResourceArguments arguments;
	json::SkipWhitespace(argumentsJson, index);
	if (!json::ReadWideString(argumentsJson, index, arguments.resourceUri)) {
		return std::nullopt;
	}
	if (arguments.resourceUri.empty() ||
		arguments.resourceUri.size() > kMaximumExplorerCommandStringLength) {
		// An empty resource is a command with no operand, which is not a
		// shorter request - it is no request at all.
		return std::nullopt;
	}
	json::SkipWhitespace(argumentsJson, index);
	if (index >= argumentsJson.size() || argumentsJson[index] != ']') {
		// A second element is upstream's multi-select list, which this
		// contract deliberately does not define; see the header.
		return std::nullopt;
	}
	++index;
	return json::AtEnd(argumentsJson, index) ? std::optional{ std::move(arguments) } : std::nullopt;
}

} // namespace workbench::commands
