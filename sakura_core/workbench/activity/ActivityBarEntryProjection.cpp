/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/activity/ActivityBarEntryProjection.h"

#include "util/string_ex.h"
#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <utility>

namespace workbench::activity {
namespace {

//! Glyph identity for Sakura's own containers. Accessible labels come from the registry's
//! localized title, so this table stays purely about which codicon.ttf glyph to draw.
constexpr std::array kBuiltinCodicons{
	std::pair{ layout::ids::viewContainer::Explorer, std::wstring_view(L"files") },
	std::pair{ layout::ids::viewContainer::Search, std::wstring_view(L"search") },
	std::pair{ layout::ids::viewContainer::RunAndDebug, std::wstring_view(L"debug-alt") },
	std::pair{ layout::ids::viewContainer::SourceControl, std::wstring_view(L"source-control") },
	std::pair{ layout::ids::viewContainer::Extensions, std::wstring_view(L"extensions") },
};

} // namespace

std::wstring_view BuiltinContainerCodicon(std::string_view containerId) noexcept
{
	for (const auto& [id, codicon] : kBuiltinCodicons) {
		if (id == containerId) return codicon;
	}
	return {};
}

std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const ActivityBarProjectionOptions& options)
{
	std::vector<const layout::RegisteredWorkbenchViewContainer*> rendered;
	rendered.reserve(snapshot.viewContainers.size());
	for (const auto& container : snapshot.viewContainers) {
		if (container.descriptor.location != layout::EViewContainerLocation::Sidebar) continue;
		if (container.isBuiltin
			&& std::ranges::find(options.renderableBuiltins, container.descriptor.id)
				== options.renderableBuiltins.end()) {
			continue;
		}
		rendered.push_back(&container);
	}

	std::ranges::sort(rendered, [](const auto* left, const auto* right) {
		if (left->descriptor.order != right->descriptor.order) {
			return left->descriptor.order < right->descriptor.order;
		}
		return left->descriptor.id < right->descriptor.id;
	});

	std::vector<ActivityBarEntry> entries;
	entries.reserve(rendered.size());
	for (const auto* container : rendered) {
		const auto& descriptor = container->descriptor;
		std::wstring codicon;
		if (container->isBuiltin) {
			codicon = BuiltinContainerCodicon(descriptor.id);
		} else if (options.extensionCodicon) {
			codicon = options.extensionCodicon(descriptor.id);
		}
		entries.push_back({
			.id = descriptor.id,
			// A container with no title would render as a blank tooltip and an unreadable
			// accessible name, so fall back to the id the extension chose for itself.
			.label = u8stowcs(descriptor.title.empty() ? descriptor.id : descriptor.title),
			.codicon = std::move(codicon),
			.builtin = container->isBuiltin,
		});
	}
	return entries;
}

} // namespace workbench::activity
