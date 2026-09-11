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
#include <unordered_map>
#include <unordered_set>

namespace workbench::layout {
namespace {

constexpr std::size_t kMaxStableIdBytes = 160;
constexpr std::size_t kMaxContributionOwners = 64;
constexpr std::size_t kMaxOwnerContainers = 16;
constexpr std::size_t kMaxOwnerViews = 64;
constexpr std::uint64_t kMaxOwnerGeneration = INT64_MAX;

bool IsBuiltIn(const WorkbenchContributionOwner& owner) noexcept
{
	return owner.ownerId.empty() && owner.generation == 0;
}

//! One extension-contributed container, moved into the reserved order band.
//!
//! The fold happens here rather than at each caller so that both registration
//! paths - the trusted built-in batch and a SENP owner's declaration - are
//! subject to it. A package cannot opt out by choosing a different entry point.
WorkbenchViewContainerDescriptor Banded(const WorkbenchViewContainerDescriptor& descriptor)
{
	if (IsFirstPartyNavigationViewContainer(descriptor.id)) return descriptor;
	auto banded = descriptor;
	banded.order = ExtensionViewContainerOrder(descriptor.order);
	return banded;
}

void EraseOwner(WorkbenchContributionSnapshot& snapshot, const std::string_view ownerId)
{
	std::erase_if(snapshot.viewContainers, [ownerId](const auto& entry) { return entry.owner.ownerId == ownerId; });
	std::erase_if(snapshot.views, [ownerId](const auto& entry) { return entry.owner.ownerId == ownerId; });
	std::erase_if(snapshot.owners, [ownerId](const auto& owner) { return owner.ownerId == ownerId; });
}

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
		if constexpr (requires { left.descriptor.Id(); }) {
			return left.descriptor.Id() < right.descriptor.Id();
		} else {
			return left.descriptor.id < right.descriptor.id;
		}
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
	addContainer(ids::viewContainer::Extensions, "Extensions", EViewContainerLocation::Sidebar,
		kExtensionsViewContainerOrder, sideBars, "extensions");
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
		|| m_snapshot.revision >= UINT64_MAX - kMaxContributionOwners
		|| std::ranges::any_of(views, [](const auto& view) { return view.provider.empty(); })) return false;
	try {
		auto candidate = m_snapshot;
		++candidate.revision;
		candidate.viewContainers.reserve(candidate.viewContainers.size() + containers.size());
		candidate.views.reserve(candidate.views.size() + views.size());
		for (const auto& descriptor : containers) candidate.viewContainers.push_back({ Banded(descriptor) });
		for (const auto& descriptor : views) candidate.views.push_back({ descriptor });
		SortById(candidate.viewContainers);
		SortById(candidate.views);
		if (!IsValidContributionSnapshot(candidate)) return false;
		m_snapshot = std::move(candidate);
		m_extensionBatchRegistered = true;
		return true;
	} catch (const std::exception&) {
		// Every statement in this block is vector/string bookkeeping on data this
		// method owns; the only failure mode is a standard allocation exception.
		return false;
	}
}

bool WorkbenchContributionRegistry::IsValidStableId(const std::string_view value) noexcept
{
	return IsPrintableUtf8(value);
}

std::optional<std::string> WorkbenchContributionRegistry::ContainerIconName(const std::string_view manifestIcon)
{
	const auto alnum = [](const char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	};
	if (manifestIcon.starts_with("$(")) {
		if (manifestIcon.size() < 4 || manifestIcon.size() > 132 || !manifestIcon.ends_with(')')) return std::nullopt;
		const auto name = manifestIcon.substr(2, manifestIcon.size() - 3);
		if (!std::ranges::all_of(name, [&](const char c) { return alnum(c) || c == '-'; })) return std::nullopt;
		return std::string(name);
	}
	if (manifestIcon.size() > 260 || !(manifestIcon.ends_with(".svg") || manifestIcon.ends_with(".png"))) {
		return std::nullopt;
	}
	for (std::size_t start = 0;;) {
		const auto end = std::min(manifestIcon.find('/', start), manifestIcon.size());
		const auto segment = manifestIcon.substr(start, end - start);
		if (segment.empty() || segment == "." || segment == ".."
			|| !std::ranges::all_of(segment, [&](const char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; })) {
			return std::nullopt;
		}
		if (end == manifestIcon.size()) break;
		start = end + 1;
	}
	return std::string(manifestIcon);
}

std::uint64_t WorkbenchContributionRegistry::NextOwnerGeneration() const noexcept
{
	return m_lastOwnerGeneration >= kMaxOwnerGeneration ? 0 : m_lastOwnerGeneration + 1;
}

PrepareWorkbenchContributionsResult WorkbenchContributionRegistry::PrepareOwnerReplacement(
	WorkbenchContributionOwner replacement, const std::uint64_t expectedGeneration,
	const std::span<const WorkbenchViewContainerDescriptor> containers,
	const std::span<const WorkbenchViewDescriptor> views) const noexcept
{
	using Status = EWorkbenchContributionChangeStatus;
	if (!IsValidStableId(replacement.ownerId) || replacement.generation == 0
		|| replacement.generation > kMaxOwnerGeneration
		|| containers.size() > kMaxOwnerContainers || views.size() > kMaxOwnerViews
		|| std::ranges::any_of(views, [](const auto& view) { return view.provider.empty() || view.title.size() > 1024; })
		|| std::ranges::any_of(containers, [](const auto& container) { return container.title.size() > 1024; })) {
		return { Status::Invalid, {} };
	}
	const auto current = std::ranges::find(m_snapshot.owners, replacement.ownerId, &WorkbenchContributionOwner::ownerId);
	if ((current == m_snapshot.owners.end() ? 0 : current->generation) != expectedGeneration
		|| replacement.generation <= m_lastOwnerGeneration) return { Status::Conflict, {} };
	if (m_snapshot.revision >= UINT64_MAX - kMaxContributionOwners
		|| (current == m_snapshot.owners.end() && m_snapshot.owners.size() >= kMaxContributionOwners)) {
		return { Status::Exhausted, {} };
	}
	try {
		auto change = std::make_unique<PreparedWorkbenchContributions>();
		change->m_registry = this;
		change->m_baseRevision = m_snapshot.revision;
		change->m_lastGeneration = replacement.generation;
		change->m_snapshot = m_snapshot;
		auto& candidate = change->m_snapshot;
		EraseOwner(candidate, replacement.ownerId);
		candidate.owners.push_back(replacement);
		std::ranges::sort(candidate.owners, {}, &WorkbenchContributionOwner::ownerId);
		for (const auto& descriptor : containers) candidate.viewContainers.push_back({ Banded(descriptor), replacement });
		for (const auto& descriptor : views) candidate.views.push_back({ descriptor, replacement });
		SortById(candidate.viewContainers);
		SortById(candidate.views);
		++candidate.revision;
		std::unordered_map<std::string_view, const WorkbenchContributionOwner*> targets;
		for (const auto& container : candidate.viewContainers) targets.emplace(container.descriptor.id, &container.owner);
		for (const auto& view : views) {
			const auto target = targets.find(view.containerId);
			if (target != targets.end() && !IsBuiltIn(*target->second) && *target->second != replacement)
				return { Status::Unsupported, {} };
		}
		if (!IsValidContributionSnapshot(candidate)) return { Status::Invalid, {} };
		return { Status::Prepared, std::move(change) };
	} catch (...) {
		return { Status::Failed, {} };
	}
}

PrepareWorkbenchContributionsResult WorkbenchContributionRegistry::PrepareOwnerDisposal(
	const WorkbenchContributionOwner& owner) const noexcept
{
	using Status = EWorkbenchContributionChangeStatus;
	if (!IsOwnerCurrent(owner)) return { Status::Conflict, {} };
	if (m_snapshot.revision == UINT64_MAX) return { Status::Exhausted, {} };
	try {
		auto change = std::make_unique<PreparedWorkbenchContributions>();
		change->m_registry = this;
		change->m_baseRevision = m_snapshot.revision;
		change->m_lastGeneration = m_lastOwnerGeneration;
		change->m_snapshot = m_snapshot;
		EraseOwner(change->m_snapshot, owner.ownerId);
		++change->m_snapshot.revision;
		if (!IsValidContributionSnapshot(change->m_snapshot)) return { Status::Invalid, {} };
		return { Status::Prepared, std::move(change) };
	} catch (...) {
		return { Status::Failed, {} };
	}
}

bool WorkbenchContributionRegistry::CanCommit(
	const PreparedWorkbenchContributions& change) const noexcept
{
	return change.m_registry == this && change.m_baseRevision == m_snapshot.revision;
}

EWorkbenchContributionChangeStatus WorkbenchContributionRegistry::Commit(
	std::unique_ptr<PreparedWorkbenchContributions> change) noexcept
{
	using Status = EWorkbenchContributionChangeStatus;
	if (!change || change->m_registry != this) return Status::Invalid;
	if (!CanCommit(*change)) return Status::Conflict;
	// No allocation, callback, or native work remains after the revision check.
	m_snapshot = std::move(change->m_snapshot);
	m_lastOwnerGeneration = change->m_lastGeneration;
	return Status::Committed;
}

EWorkbenchContributionChangeStatus WorkbenchContributionRegistry::DisposeOwner(
	const WorkbenchContributionOwner& owner) noexcept
{
	using Status = EWorkbenchContributionChangeStatus;
	if (!IsOwnerCurrent(owner)) return Status::Conflict;
	if (m_snapshot.revision == UINT64_MAX) return Status::Exhausted;
	EraseOwner(m_snapshot, owner.ownerId);
	++m_snapshot.revision;
	return Status::Committed;
}

bool WorkbenchContributionRegistry::IsOwnerCurrent(const WorkbenchContributionOwner& owner) const noexcept
{
	return !IsBuiltIn(owner) && std::ranges::find(m_snapshot.owners, owner) != m_snapshot.owners.end();
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
		if (snapshot.owners.size() > kMaxContributionOwners) return false;
		std::unordered_map<std::string_view, std::uint64_t> ownerIds;
		for (const auto& owner : snapshot.owners) {
			if (!IsValidStableId(owner.ownerId) || owner.generation == 0 || owner.generation > kMaxOwnerGeneration
				|| !ownerIds.emplace(owner.ownerId, owner.generation).second) return false;
		}
		const auto validOwner = [&ownerIds](const WorkbenchContributionOwner& owner) {
			const auto found = ownerIds.find(owner.ownerId);
			return IsBuiltIn(owner) || (found != ownerIds.end() && found->second == owner.generation);
		};
		std::unordered_set<std::string_view> partIds;
		partIds.reserve(snapshot.parts.size());
		for (const auto& registered : snapshot.parts) {
			if (!IsValidStableId(registered.descriptor.Id())
				|| !partIds.emplace(registered.descriptor.Id()).second) return false;
		}

		std::unordered_map<std::string_view, const WorkbenchContributionOwner*> containerIds;
		containerIds.reserve(snapshot.viewContainers.size());
		for (const auto& registered : snapshot.viewContainers) {
			if (!IsValidViewContainerDescriptor(registered.descriptor)
				|| !validOwner(registered.owner)
				|| !containerIds.emplace(registered.descriptor.id, &registered.owner).second) return false;
		}

		std::unordered_set<std::string_view> viewIds;
		viewIds.reserve(snapshot.views.size());
		for (const auto& registered : snapshot.views) {
			if (!IsValidStableId(registered.descriptor.id)
				|| !validOwner(registered.owner)
				|| !IsValidStableId(registered.descriptor.containerId)
				|| (!registered.descriptor.provider.empty()
					&& !IsValidStableId(registered.descriptor.provider))
				|| !containerIds.contains(registered.descriptor.containerId)
				|| !viewIds.emplace(registered.descriptor.id).second) return false;
			const auto& containerOwner = *containerIds.at(registered.descriptor.containerId);
			// A built-in container may host extension Views. An extension cannot
			// borrow another owner's container and prevent its independent disposal.
			if (!IsBuiltIn(containerOwner) && containerOwner != registered.owner) return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace workbench::layout
