/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>

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

} // namespace

WorkbenchContributionRegistry::WorkbenchContributionRegistry()
{
	const auto addPart = [this](std::string_view id, std::string_view title, const bool supportsVisibility = true) {
		m_snapshot.parts.push_back({ { std::string(id), std::string(title), supportsVisibility } });
	};
	const auto addContainer = [this](std::string_view id, std::string_view title,
		const EViewContainerLocation location, const std::int32_t order) {
		m_snapshot.viewContainers.push_back({ { std::string(id), std::string(title), location, order } });
	};
	const auto addView = [this](std::string_view id, std::string_view containerId,
		std::string_view title, const std::int32_t order) {
		m_snapshot.views.push_back({ { std::string(id), std::string(containerId), std::string(title), order } });
	};

	addPart(ids::part::Titlebar, "Title Bar");
	addPart(ids::part::Banner, "Banner");
	addPart(ids::part::Activitybar, "Activity Bar");
	addPart(ids::part::Sidebar, "Side Bar");
	addPart(ids::part::Panel, "Panel");
	addPart(ids::part::Auxiliarybar, "Auxiliary Bar");
	addPart(ids::part::Editor, "Editor", false);
	addPart(ids::part::Statusbar, "Status Bar");
	addPart(ids::part::Sessions, "Sessions");

	addContainer(ids::viewContainer::Explorer, "Explorer", EViewContainerLocation::Sidebar, 10);
	addContainer(ids::viewContainer::Search, "Search", EViewContainerLocation::Sidebar, 20);
	addContainer(ids::viewContainer::RunAndDebug, "Run and Debug", EViewContainerLocation::Sidebar, 30);
	addContainer(ids::viewContainer::SourceControl, "Source Control", EViewContainerLocation::Sidebar, 40);
	addContainer(ids::viewContainer::Problems, "Problems", EViewContainerLocation::Panel, 10);
	addContainer(ids::viewContainer::Output, "Output", EViewContainerLocation::Panel, 20);
	addContainer(ids::viewContainer::Terminal, "Terminal", EViewContainerLocation::Panel, 30);
	addContainer(ids::viewContainer::Ports, "Ports", EViewContainerLocation::Panel, 40);
	addContainer(ids::viewContainer::DebugConsole, "Debug Console", EViewContainerLocation::Panel, 50);

	addView(ids::view::Explorer, ids::viewContainer::Explorer, "Explorer", 10);
	addView(ids::view::Outline, ids::viewContainer::Explorer, "Outline", 20);
	addView(ids::view::Search, ids::viewContainer::Search, "Search", 10);
	addView(ids::view::DebugVariables, ids::viewContainer::RunAndDebug, "Variables", 10);
	addView(ids::view::DebugWatch, ids::viewContainer::RunAndDebug, "Watch", 20);
	addView(ids::view::DebugCallStack, ids::viewContainer::RunAndDebug, "Call Stack", 30);
	addView(ids::view::DebugLoadedScripts, ids::viewContainer::RunAndDebug, "Loaded Scripts", 40);
	addView(ids::view::DebugBreakpoints, ids::viewContainer::RunAndDebug, "Breakpoints", 50);
	addView(ids::view::SourceControl, ids::viewContainer::SourceControl, "Source Control", 10);
	addView(ids::view::Problems, ids::viewContainer::Problems, "Problems", 10);
	addView(ids::view::Output, ids::viewContainer::Output, "Output", 10);
	addView(ids::view::Terminal, ids::viewContainer::Terminal, "Terminal", 10);
	addView(ids::view::Ports, ids::viewContainer::Ports, "Ports", 10);
	addView(ids::view::DebugConsole, ids::viewContainer::DebugConsole, "Debug Console", 10);

	SortById(m_snapshot.parts);
	SortById(m_snapshot.viewContainers);
	SortById(m_snapshot.views);
}

bool WorkbenchContributionRegistry::IsValidStableId(const std::string_view value) noexcept
{
	return IsPrintableUtf8(value);
}

} // namespace workbench::layout
