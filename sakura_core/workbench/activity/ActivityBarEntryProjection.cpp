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
	std::vector<const layout::RegisteredWorkbenchViewContainer*> rendered;
	rendered.reserve(snapshot.viewContainers.size());
	for (const auto& container : snapshot.viewContainers) {
		if (container.descriptor.location != requestedLocation) continue;
		if (std::ranges::find(options.renderableBuiltins, container.descriptor.id)
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
		const auto fallback = u8stowcs(descriptor.title.empty() ? descriptor.id : descriptor.title);
		const auto label = options.titleResolver
			? options.titleResolver(descriptor.id, fallback)
			: fallback;
		entries.push_back({
			.id = descriptor.id,
			.label = label.empty() ? fallback : label,
			.codicon = std::wstring(BuiltinContainerCodicon(descriptor.id)),
		});
	}
	return entries;
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
