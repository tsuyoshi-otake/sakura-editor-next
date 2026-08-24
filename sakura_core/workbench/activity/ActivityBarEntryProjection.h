/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/activity/ActivityBarModel.h"
#include "workbench/layout/WorkbenchContributionRegistry.h"

#include <span>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::activity {

//! Resolves an Activity Bar entry's stable id to its localized presentation label.
using ActivityBarTitleResolver = std::function<std::wstring(
	std::string_view activityId, std::wstring_view fallback)>;

[[nodiscard]] std::uint32_t ResolveBuiltinActivityTitleResourceId(std::string_view containerId) noexcept;
//! Resolves a GlobalCompositeBar action id (Accounts or Manage) to its resource id.
[[nodiscard]] std::uint32_t ResolveGlobalActivityTitleResourceId(std::string_view actionId) noexcept;
//! Resolves either a built-in ViewContainer or a GlobalCompositeBar action id.
[[nodiscard]] std::uint32_t ResolveActivityTitleResourceId(std::string_view activityId) noexcept;

/*!
	@brief What the projection needs beyond the layout registry itself.

	The layout registry is the single authority on which ViewContainers exist and where they
	live, but it deliberately knows nothing about glyphs or about which built-in container the
	native workbench can actually render. Both gaps are filled here by the composition, so this
	file never has to depend on the side bar page pool.
*/
struct ActivityBarProjectionOptions {
	/*!
		@brief Built-in containers the workbench can really open.

		The registry declares VS Code's full built-in set, including Search and Run and Debug,
		which Sakura has no side bar page for yet. Rendering their icons would put buttons on
		the Activity Bar that open nothing, so the composition passes only what it can show.
	*/
	std::span<const std::string_view> renderableBuiltins;
	//! Resolves a container's display title at presentation time. Empty means registry fallback.
	ActivityBarTitleResolver titleResolver;
	//! Requested ViewContainer location. Only Sidebar and AuxiliaryBar are Activity Bar hosts.
	layout::EViewContainerLocation location = layout::EViewContainerLocation::Sidebar;
};

/*!
@brief Projects one requested side-bar location's ViewContainers onto Activity Bar entries.

	Order follows the registry's `order` field, then the container id, so the strip is stable
	across restarts. Panel containers are skipped; Sidebar and AuxiliaryBar are separate
	physical side-bar hosts and can be projected independently.
*/
[[nodiscard]] std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const ActivityBarProjectionOptions& options);

//! Projects the requested side-bar location without requiring a second options object.
[[nodiscard]] std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const ActivityBarProjectionOptions& options,
	layout::EViewContainerLocation requestedLocation);

//! Argument-order convenience overload for composition code that treats location as primary.
[[nodiscard]] std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	layout::EViewContainerLocation requestedLocation,
	const ActivityBarProjectionOptions& options);

//! The bundled codicon Sakura renders for one of its own containers, empty when it has none.
[[nodiscard]] std::wstring_view BuiltinContainerCodicon(std::string_view containerId) noexcept;

/*!
	@brief Appends VS Code's GlobalCompositeBar actions (Accounts, then Manage).

	These are not ViewContainers. They pin to the bottom of a vertical Activity Bar and
	open menus rather than activating a Side Bar page.
*/
void AppendGlobalActivityActions(std::vector<ActivityBarEntry>& entries);

//! Appends GlobalCompositeBar actions with labels supplied by the composition boundary.
void AppendGlobalActivityActions(std::vector<ActivityBarEntry>& entries,
	const ActivityBarTitleResolver& titleResolver);

} // namespace workbench::activity
