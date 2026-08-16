/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/commands/WorkbenchCommandRegistry.h"

#include <cwctype>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::editor {

using WorkbenchCommandTitleResolver =
	std::function<std::wstring(const commands::WorkbenchCommandDescriptor&)>;

//! Presentation data for commands contributed by the native workbench palette.
struct WorkbenchCommandPaletteItem {
	//! Own all text copied from the registry; the overlay can outlive one search result.
	std::string id;
	std::wstring label;
	std::wstring detail;
};

namespace detail {

inline std::wstring LowercaseCommandPaletteText(std::wstring_view text)
{
	std::wstring lowered(text);
	for (auto& character : lowered) {
		character = static_cast<wchar_t>(std::towlower(character));
	}
	return lowered;
}

inline std::wstring WidenCommandText(std::string_view text)
{
	std::wstring widened;
	widened.reserve(text.size());
	for (const unsigned char character : text) {
		widened.push_back(static_cast<wchar_t>(character));
	}
	return widened;
}

inline bool MatchesCommandPaletteQuery(const WorkbenchCommandPaletteItem& item,
	std::wstring_view query)
{
	if (query.empty()) return true;
	const auto loweredQuery = LowercaseCommandPaletteText(query);
	const auto loweredLabel = LowercaseCommandPaletteText(item.label);
	const auto loweredId = LowercaseCommandPaletteText(WidenCommandText(item.id));
	return loweredLabel.find(loweredQuery) != std::wstring::npos
		|| loweredId.find(loweredQuery) != std::wstring::npos;
}

inline bool IsStableCommandId(std::wstring_view candidate, std::string_view stableId) noexcept
{
	if (candidate.size() != stableId.size()) return false;
	for (std::size_t index = 0; index < stableId.size(); ++index) {
		if (candidate[index] != static_cast<wchar_t>(stableId[index])) return false;
	}
	return true;
}

} // namespace detail

//! Returns every registered native command bound to CommandPalette that matches the query.
inline std::vector<WorkbenchCommandPaletteItem> SearchRegisteredCommandPalette(
	const commands::WorkbenchCommandRegistry& registry, std::wstring_view query,
	const WorkbenchCommandTitleResolver& titleResolver = {})
{
	const auto descriptors = registry.EnumerateSurface(commands::EWorkbenchCommandSurface::CommandPalette);
	std::vector<WorkbenchCommandPaletteItem> result;
	result.reserve(descriptors.size());
	for (const auto& descriptor : descriptors) {
		WorkbenchCommandPaletteItem item{
			.id = descriptor.id,
			.label = titleResolver ? titleResolver(descriptor) : detail::WidenCommandText(descriptor.title),
			.detail = L"Sakura Editor",
		};
		if (item.label.empty()) item.label = detail::WidenCommandText(descriptor.title);
		if (detail::MatchesCommandPaletteQuery(item, query)) {
			result.push_back(std::move(item));
		}
	}
	return result;
}

//! Routes a registered native palette selection by its stable VS Code command identifier.
//! Unknown or non-palette IDs are left for extension-command dispatch.
template <typename Dispatcher>
bool DispatchRegisteredCommandPaletteSelection(const commands::WorkbenchCommandRegistry& registry,
	std::wstring_view commandId, Dispatcher&& dispatcher)
{
	for (const auto& descriptor : registry.EnumerateSurface(
		commands::EWorkbenchCommandSurface::CommandPalette)) {
		if (detail::IsStableCommandId(commandId, descriptor.id)) {
			std::forward<Dispatcher>(dispatcher)(descriptor.id);
			return true;
		}
	}
	return false;
}

} // namespace workbench::editor
