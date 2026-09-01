/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace workbench::layout {
namespace {

constexpr std::size_t kMaxStableIdBytes = 160;

bool IsPrintableUtf8(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaxStableIdBytes) return false;
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if (first < 0x21 || first == 0x7f) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) {
			continuationCount = 1;
			codePoint = first & 0x1f;
		} else if (first >= 0xe0 && first <= 0xef) {
			continuationCount = 2;
			codePoint = first & 0x0f;
		} else if (first >= 0xf0 && first <= 0xf4) {
			continuationCount = 3;
			codePoint = first & 0x07;
		} else {
			return false;
		}
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)
			|| (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

template<typename T>
void SortById(std::vector<T>& values)
{
	std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
		return left.descriptor.id < right.descriptor.id;
	});
}

[[nodiscard]] bool IsValidLocation(EViewContainerLocation value) noexcept
{
	return value == EViewContainerLocation::Sidebar || value == EViewContainerLocation::Panel
		|| value == EViewContainerLocation::AuxiliaryBar;
}

} // namespace

WorkbenchContributionRegistry::WorkbenchContributionRegistry()
{
	const auto addPart = [this](std::string_view id, std::string_view title, const bool supportsVisibility = true) {
		m_snapshot.parts.push_back({ { std::string(id), std::string(title), supportsVisibility } });
	};
	const auto addContainer = [this](std::string_view id, std::string_view title,
		const EViewContainerLocation location, const std::int32_t order,
		SupportedViewContainerLocations supportedLocations, std::string_view icon = {}) {
		m_snapshot.viewContainers.push_back({ { .id = std::string(id), .title = std::string(title),
			.location = location, .order = order, .icon = std::string(icon),
			.supportedLocations = supportedLocations } });
	};
	const auto addView = [this](std::string_view id, std::string_view containerId,
		std::string_view title, const std::int32_t order) {
		m_snapshot.views.push_back({ { std::string(id), std::string(containerId), std::string(title), order } });
	};

	addPart(ids::part::Titlebar, "Title Bar");
	addPart(ids::part::Activitybar, "Activity Bar");
	addPart(ids::part::Sidebar, "Side Bar");
	addPart(ids::part::Panel, "Panel");
	addPart(ids::part::Auxiliarybar, "Auxiliary Bar");
	addPart(ids::part::Editor, "Editor", false);
	addPart(ids::part::Statusbar, "Status Bar");
	addPart(ids::part::Sessions, "Sessions");

	const SupportedViewContainerLocations sideBars{
		EViewContainerLocation::Sidebar, EViewContainerLocation::AuxiliaryBar };
	const SupportedViewContainerLocations panel{ EViewContainerLocation::Panel };
	addContainer(ids::viewContainer::Explorer, "Explorer", EViewContainerLocation::Sidebar, 10, sideBars, "files");
	addContainer(ids::viewContainer::Search, "Search", EViewContainerLocation::Sidebar, 20, sideBars, "search");
	addContainer(ids::viewContainer::RunAndDebug, "Run and Debug", EViewContainerLocation::Sidebar, 30, sideBars, "debug-alt");
	addContainer(ids::viewContainer::SourceControl, "Source Control", EViewContainerLocation::Sidebar, 40, sideBars, "source-control");
	addContainer(ids::viewContainer::Extensions, "Extensions", EViewContainerLocation::Sidebar, 50, sideBars, "extensions");
	addContainer(ids::viewContainer::Problems, "Problems", EViewContainerLocation::Panel, 10, panel);
	addContainer(ids::viewContainer::Output, "Output", EViewContainerLocation::Panel, 20, panel);
	addContainer(ids::viewContainer::Terminal, "Terminal", EViewContainerLocation::Panel, 30, panel);
	addContainer(ids::viewContainer::Ports, "Ports", EViewContainerLocation::Panel, 40, panel);
	addContainer(ids::viewContainer::DebugConsole, "Debug Console", EViewContainerLocation::Panel, 50, panel);

	addView(ids::view::Explorer, ids::viewContainer::Explorer, "Explorer", 10);
	addView(ids::view::Outline, ids::viewContainer::Explorer, "Outline", 20);
	addView(ids::view::Search, ids::viewContainer::Search, "Search", 10);
	addView(ids::view::DebugVariables, ids::viewContainer::RunAndDebug, "Variables", 10);
	addView(ids::view::DebugWatch, ids::viewContainer::RunAndDebug, "Watch", 20);
	addView(ids::view::DebugCallStack, ids::viewContainer::RunAndDebug, "Call Stack", 30);
	addView(ids::view::DebugLoadedScripts, ids::viewContainer::RunAndDebug, "Loaded Scripts", 40);
	addView(ids::view::DebugBreakpoints, ids::viewContainer::RunAndDebug, "Breakpoints", 50);
	// `workbench.scm` is the current VS Code Changes View. Repositories and Graph
	// are rendered as non-selectable native siblings until the layout registry can
	// model VS Code's provider-driven visibility conditions.
	addView(ids::view::SourceControl, ids::viewContainer::SourceControl, "Changes", 10);
	addView(ids::view::ExtensionsInstalled, ids::viewContainer::Extensions, "Installed", 10);
	addView(ids::view::Problems, ids::viewContainer::Problems, "Problems", 10);
	addView(ids::view::Output, ids::viewContainer::Output, "Output", 10);
	addView(ids::view::Terminal, ids::viewContainer::Terminal, "Terminal", 10);
	addView(ids::view::Ports, ids::viewContainer::Ports, "Ports", 10);
	addView(ids::view::DebugConsole, ids::viewContainer::DebugConsole, "Debug Console", 10);

	SortById(m_snapshot.parts);
	SortById(m_snapshot.viewContainers);
	SortById(m_snapshot.views);
	if (!IsValidContributionSnapshot(m_snapshot))
		throw std::logic_error("invalid built-in workbench contributions");
}

bool WorkbenchContributionRegistry::RegisterExtensionContributions(
	const std::span<const WorkbenchViewContainerDescriptor> containers,
	const std::span<const WorkbenchViewDescriptor> views)
{
	if (m_extensionBatchRegistered || (containers.empty() && views.empty())
		|| m_snapshot.revision == (std::numeric_limits<std::uint64_t>::max)()
		|| std::ranges::any_of(views, [](const auto& view) { return view.provider.empty(); })) return false;
	try {
		auto candidate = m_snapshot;
		++candidate.revision;
		candidate.viewContainers.reserve(candidate.viewContainers.size() + containers.size());
		candidate.views.reserve(candidate.views.size() + views.size());
		for (const auto& descriptor : containers) candidate.viewContainers.push_back({ descriptor });
		for (const auto& descriptor : views) candidate.views.push_back({ descriptor });
		SortById(candidate.viewContainers);
		SortById(candidate.views);
		if (!IsValidContributionSnapshot(candidate)) return false;
		m_snapshot = std::move(candidate);
		m_extensionBatchRegistered = true;
		return true;
	} catch (...) {
		return false;
	}
}

bool WorkbenchContributionRegistry::IsValidStableId(const std::string_view value) noexcept
{
	return IsPrintableUtf8(value);
}

bool WorkbenchContributionRegistry::IsValidViewContainerDescriptor(
	const WorkbenchViewContainerDescriptor& descriptor) noexcept
{
	return IsValidStableId(descriptor.id) && IsValidLocation(descriptor.location)
		&& (descriptor.icon.empty() || IsValidStableId(descriptor.icon))
		&& descriptor.supportedLocations.IsValid()
		&& descriptor.supportedLocations.Contains(descriptor.location);
}

bool WorkbenchContributionRegistry::IsValidContributionSnapshot(
	const WorkbenchContributionSnapshot& snapshot) noexcept
{
	try {
		std::unordered_set<std::string_view> partIds;
		partIds.reserve(snapshot.parts.size());
		for (const auto& registered : snapshot.parts) {
			if (!IsValidStableId(registered.descriptor.id)
				|| !partIds.emplace(registered.descriptor.id).second) return false;
		}

		std::unordered_set<std::string_view> containerIds;
		containerIds.reserve(snapshot.viewContainers.size());
		for (const auto& registered : snapshot.viewContainers) {
			if (!IsValidViewContainerDescriptor(registered.descriptor)
				|| !containerIds.emplace(registered.descriptor.id).second) return false;
		}

		std::unordered_set<std::string_view> viewIds;
		viewIds.reserve(snapshot.views.size());
		for (const auto& registered : snapshot.views) {
			if (!IsValidStableId(registered.descriptor.id)
				|| !IsValidStableId(registered.descriptor.containerId)
				|| (!registered.descriptor.provider.empty()
					&& !IsValidStableId(registered.descriptor.provider))
				|| !containerIds.contains(registered.descriptor.containerId)
				|| !viewIds.emplace(registered.descriptor.id).second) return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace workbench::layout
