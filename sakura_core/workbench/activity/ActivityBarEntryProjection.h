/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/activity/ActivityBarModel.h"
#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <span>
#include <functional>
#include <cstdint>
#include <optional>
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

	The contribution registry is the authority on which ViewContainers exist and their defaults;
	the optional layout snapshot owns the live/persisted location and order. The projection still
	knows nothing about which built-in container the native workbench can actually render, so the
	composition supplies that final capability boundary.
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
	//! Live/persisted placement and order. Null uses contribution defaults during bootstrap.
	const layout::WorkbenchLayoutStateSnapshot* layoutState = nullptr;
};

/*!
@brief Projects one requested side-bar location's ViewContainers onto Activity Bar entries.

	Order and location follow the layout snapshot when present, falling back to contribution
	defaults during bootstrap. Equal orders use the stable container id. Panel containers are
	skipped; Sidebar and AuxiliaryBar are separate physical hosts.
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

/*! @brief Returns the visible ViewContainer ids after one same-bar drop.

	`insertionIndex` is a gap in the pre-drop ViewContainer-only strip. Invisible
	entries and GlobalCompositeBar actions never participate. Invalid drags fail closed.
*/
[[nodiscard]] std::optional<std::vector<std::string>> ReorderActivityBarContainers(
	std::span<const ActivityBarEntry> entries,
	std::string_view draggedContainerId,
	std::size_t insertionIndex);

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
