/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/activity/ActivityBarEntryProjection.h"

#include "sakura_rc.h"

#include "util/string_ex.h"
#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <utility>

namespace workbench::activity {
namespace {

layout::EViewContainerLocation ContributionLocation(
	const layout::EWorkbenchViewContainerLocation location) noexcept
{
	switch (location) {
	case layout::EWorkbenchViewContainerLocation::SideBar:
		return layout::EViewContainerLocation::Sidebar;
	case layout::EWorkbenchViewContainerLocation::Panel:
		return layout::EViewContainerLocation::Panel;
	case layout::EWorkbenchViewContainerLocation::AuxiliaryBar:
		return layout::EViewContainerLocation::AuxiliaryBar;
	}
	return layout::EViewContainerLocation::Panel;
}

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

std::uint32_t ResolveBuiltinActivityTitleResourceId(std::string_view containerId) noexcept
{
	if (containerId == layout::ids::viewContainer::Explorer) return STR_WORKBENCH_ACTIVITY_EXPLORER;
	if (containerId == layout::ids::viewContainer::Search) return STR_WORKBENCH_ACTIVITY_SEARCH;
	if (containerId == layout::ids::viewContainer::SourceControl) return STR_WORKBENCH_ACTIVITY_SOURCE_CONTROL;
	if (containerId == layout::ids::viewContainer::Extensions) return STR_WORKBENCH_EXTENSIONS_TITLE;
	return 0;
}

std::uint32_t ResolveGlobalActivityTitleResourceId(std::string_view actionId) noexcept
{
	if (actionId == kAccountsActivityId) return STR_WORKBENCH_ACTIVITY_ACCOUNTS;
	if (actionId == kManageActivityId) return STR_WORKBENCH_ACTIVITY_MANAGE;
	return 0;
}

std::uint32_t ResolveActivityTitleResourceId(std::string_view activityId) noexcept
{
	if (const auto resourceId = ResolveBuiltinActivityTitleResourceId(activityId); resourceId != 0) {
		return resourceId;
	}
	return ResolveGlobalActivityTitleResourceId(activityId);
}

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
	return ProjectActivityBarEntries(snapshot, options, options.location);
}

std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const ActivityBarProjectionOptions& options,
	const layout::EViewContainerLocation requestedLocation)
{
	// The Activity Bar is a composite bar for a side-bar location. Panel is a
	// different Part and must fail closed rather than being shown as a plausible
	// but unopenable Activity Bar entry.
	if (requestedLocation != layout::EViewContainerLocation::Sidebar
		&& requestedLocation != layout::EViewContainerLocation::AuxiliaryBar) {
		return {};
	}
	struct RenderedContainer {
		const layout::RegisteredWorkbenchViewContainer* contribution = nullptr;
		std::int32_t order = 0;
	};
	std::vector<RenderedContainer> rendered;
	rendered.reserve(snapshot.viewContainers.size());
	for (const auto& container : snapshot.viewContainers) {
		auto location = container.descriptor.location;
		auto order = container.descriptor.order;
		if (options.layoutState != nullptr) {
			const auto state = std::ranges::find(options.layoutState->containers,
				container.descriptor.id,
				&layout::WorkbenchViewContainerState::containerId);
			if (state != options.layoutState->containers.end()) {
				location = ContributionLocation(state->location);
				order = state->order;
			}
		}
		if (location != requestedLocation) continue;
		if (std::ranges::find(options.renderableBuiltins, container.descriptor.id)
				== options.renderableBuiltins.end()) {
			continue;
		}
		rendered.push_back({ &container, order });
	}

	std::ranges::sort(rendered, [](const auto& left, const auto& right) {
		if (left.order != right.order) {
			return left.order < right.order;
		}
		return left.contribution->descriptor.id < right.contribution->descriptor.id;
	});

	std::vector<ActivityBarEntry> entries;
	entries.reserve(rendered.size());
	for (const auto& container : rendered) {
		const auto& descriptor = container.contribution->descriptor;
		const auto fallback = u8stowcs(descriptor.title.empty() ? descriptor.id : descriptor.title);
		const auto label = options.titleResolver
			? options.titleResolver(descriptor.id, fallback)
			: fallback;
		entries.push_back({
			.id = descriptor.id,
			.label = label.empty() ? fallback : label,
			.codicon = descriptor.icon.empty()
				? std::wstring(BuiltinContainerCodicon(descriptor.id)) : u8stowcs(descriptor.icon),
		});
	}
	return entries;
}

std::optional<std::vector<std::string>> ReorderActivityBarContainers(
	const std::span<const ActivityBarEntry> entries,
	const std::string_view draggedContainerId,
	std::size_t insertionIndex)
{
	if (draggedContainerId.empty()) return std::nullopt;
	std::vector<std::string> ordered;
	ordered.reserve(entries.size());
	for (const auto& entry : entries) {
		if (entry.visible && !entry.IsGlobalAction()) ordered.push_back(entry.id);
	}
	const auto source = std::ranges::find(ordered, draggedContainerId);
	if (source == ordered.end()) return std::nullopt;
	const auto sourceIndex = static_cast<std::size_t>(source - ordered.begin());
	insertionIndex = std::min(insertionIndex, ordered.size());
	ordered.erase(source);
	if (sourceIndex < insertionIndex) --insertionIndex;
	ordered.insert(ordered.begin() + static_cast<std::ptrdiff_t>(insertionIndex),
		std::string(draggedContainerId));
	return ordered;
}

std::vector<ActivityBarEntry> ProjectActivityBarEntries(
	const layout::WorkbenchContributionSnapshot& snapshot,
	const layout::EViewContainerLocation requestedLocation,
	const ActivityBarProjectionOptions& options)
{
	return ProjectActivityBarEntries(snapshot, options, requestedLocation);
}

void AppendGlobalActivityActions(std::vector<ActivityBarEntry>& entries)
{
	AppendGlobalActivityActions(entries, ActivityBarTitleResolver{});
}

void AppendGlobalActivityActions(std::vector<ActivityBarEntry>& entries,
	const ActivityBarTitleResolver& titleResolver)
{
	const auto resolveLabel = [&titleResolver](std::string_view id, std::wstring_view fallback) {
		if (titleResolver) {
			if (const auto localized = titleResolver(id, fallback); !localized.empty()) return localized;
		}
		return std::wstring(fallback);
	};
	const auto alreadyPresent = [&entries](std::string_view id) {
		return std::ranges::find(entries, id, &ActivityBarEntry::id) != entries.end();
	};
	if (!alreadyPresent(kAccountsActivityId)) {
		entries.push_back({
			.id = std::string(kAccountsActivityId),
			.label = resolveLabel(kAccountsActivityId, L"Accounts"),
			.codicon = L"account",
			.kind = ActivityBarEntryKind::GlobalAction,
		});
	}
	if (!alreadyPresent(kManageActivityId)) {
		entries.push_back({
			.id = std::string(kManageActivityId),
			.label = resolveLabel(kManageActivityId, L"Manage"),
			.codicon = L"settings-gear",
			.kind = ActivityBarEntryKind::GlobalAction,
		});
	}
}

} // namespace workbench::activity
