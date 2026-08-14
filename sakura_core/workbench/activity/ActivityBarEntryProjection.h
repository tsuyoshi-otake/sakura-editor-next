/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/activity/ActivityBarModel.h"
#include "workbench/layout/WorkbenchContributionRegistry.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::activity {

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
};

/*!
	@brief Projects the Primary Side Bar's ViewContainers onto Activity Bar entries.

	Order follows the registry's `order` field, then the container id, so the strip is stable
	across restarts. Panel and Auxiliary
	Bar containers are skipped: VS Code's Activity Bar only ever shows the Primary Side Bar.
*/
[[nodiscard]] std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const ActivityBarProjectionOptions& options);

//! The bundled codicon Sakura renders for one of its own containers, empty when it has none.
[[nodiscard]] std::wstring_view BuiltinContainerCodicon(std::string_view containerId) noexcept;

} // namespace workbench::activity
