/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/win32/BuiltinPartProjection.h"

#include "workbench/layout/WorkbenchIds.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::win32 {
namespace {

struct RequiredBuiltinPart {
	std::string_view id;
	layout::EWorkbenchPartPosition position;
};

constexpr std::array kRequiredBuiltinParts{
	RequiredBuiltinPart{ layout::ids::part::Sidebar, layout::EWorkbenchPartPosition::Left },
	RequiredBuiltinPart{ layout::ids::part::Panel, layout::EWorkbenchPartPosition::Bottom },
	RequiredBuiltinPart{ layout::ids::part::Auxiliarybar, layout::EWorkbenchPartPosition::Right },
};

[[nodiscard]] std::optional<std::size_t> RequiredPartIndex(std::string_view id) noexcept
{
	for (std::size_t index = 0; index < kRequiredBuiltinParts.size(); ++index) {
		if (id == kRequiredBuiltinParts[index].id) return index;
	}
	return std::nullopt;
}

[[nodiscard]] constexpr std::uint32_t LocationBit(
	layout::EWorkbenchViewContainerLocation location) noexcept
{
	return static_cast<std::uint32_t>(1u) << static_cast<std::uint32_t>(location);
}

//! VS Code renders the same composite bar in the Primary and Secondary Side Bar, so an
//! Activity Bar ViewContainer is valid in either one and moves between them.
constexpr std::uint32_t kSideBarLocations =
	LocationBit(layout::EWorkbenchViewContainerLocation::SideBar)
	| LocationBit(layout::EWorkbenchViewContainerLocation::AuxiliaryBar);
//! Panel containers stay in the Panel here. Moving the whole Panel to the Secondary Side
//! Bar is `workbench.action.movePanelToSecondarySideBar`, a separate unsupported gate.
constexpr std::uint32_t kPanelLocations =
	LocationBit(layout::EWorkbenchViewContainerLocation::Panel);

struct NativeSurfaceMapping {
	std::string_view containerId;
	std::string_view viewId;
	//! Every location whose native host can render this surface.
	std::uint32_t locations;
	BuiltinActiveSurface surface;
};

constexpr std::array kNativeSurfaceMappings{
	NativeSurfaceMapping{ layout::ids::viewContainer::Explorer, layout::ids::view::Explorer,
		kSideBarLocations, BuiltinActiveSurface::Explorer },
	NativeSurfaceMapping{ layout::ids::viewContainer::Explorer, layout::ids::view::Outline,
		kSideBarLocations, BuiltinActiveSurface::Outline },
	NativeSurfaceMapping{ layout::ids::viewContainer::SourceControl, layout::ids::view::SourceControl,
		kSideBarLocations, BuiltinActiveSurface::SourceControl },
	NativeSurfaceMapping{ layout::ids::viewContainer::Extensions, layout::ids::view::Extensions,
		kSideBarLocations, BuiltinActiveSurface::Extensions },
	NativeSurfaceMapping{ layout::ids::viewContainer::Terminal, layout::ids::view::Terminal,
		kPanelLocations, BuiltinActiveSurface::Terminal },
	NativeSurfaceMapping{ layout::ids::viewContainer::Problems, layout::ids::view::Problems,
		kPanelLocations, BuiltinActiveSurface::Problems },
	NativeSurfaceMapping{ layout::ids::viewContainer::Output, layout::ids::view::Output,
		kPanelLocations, BuiltinActiveSurface::Output },
};

template<typename T>
[[nodiscard]] const T* FindUniqueById(const std::vector<T>& states, std::string_view id,
	std::string T::* idMember, bool& duplicate) noexcept
{
	const T* found = nullptr;
	duplicate = false;
	for (const auto& state : states) {
		if (std::string_view(state.*idMember) != id) continue;
		if (found != nullptr) {
			duplicate = true;
			return nullptr;
		}
		found = &state;
	}
	return found;
}

[[nodiscard]] const NativeSurfaceMapping* FindNativeSurface(std::string_view containerId,
	std::string_view viewId) noexcept
{
	for (const auto& mapping : kNativeSurfaceMappings) {
		if (mapping.containerId == containerId && mapping.viewId == viewId) return &mapping;
	}
	return nullptr;
}

[[nodiscard]] std::string_view PartIdForLocation(layout::EWorkbenchViewContainerLocation location) noexcept
{
	switch (location) {
	case layout::EWorkbenchViewContainerLocation::SideBar: return layout::ids::part::Sidebar;
	case layout::EWorkbenchViewContainerLocation::Panel: return layout::ids::part::Panel;
	case layout::EWorkbenchViewContainerLocation::AuxiliaryBar: return layout::ids::part::Auxiliarybar;
	}
	return {};
}

struct ActiveLocation {
	layout::EWorkbenchViewContainerLocation location;
	const std::optional<std::string> layout::WorkbenchActiveContainerState::* activeId;
	std::optional<BuiltinActiveSurface> BuiltinActiveSurfaceProjection::* surface;
};

constexpr std::array kActiveLocations{
	ActiveLocation{ layout::EWorkbenchViewContainerLocation::SideBar,
		&layout::WorkbenchActiveContainerState::sideBar, &BuiltinActiveSurfaceProjection::sidebar },
	ActiveLocation{ layout::EWorkbenchViewContainerLocation::Panel,
		&layout::WorkbenchActiveContainerState::panel, &BuiltinActiveSurfaceProjection::panel },
	ActiveLocation{ layout::EWorkbenchViewContainerLocation::AuxiliaryBar,
		&layout::WorkbenchActiveContainerState::auxiliaryBar, &BuiltinActiveSurfaceProjection::auxiliaryBar },
};

[[nodiscard]] const ActiveLocation* FindActiveLocation(
	layout::EWorkbenchViewContainerLocation location) noexcept
{
	for (const auto& activeLocation : kActiveLocations) {
		if (activeLocation.location == location) return &activeLocation;
	}
	return nullptr;
}

[[nodiscard]] BuiltinActiveSurfaceProjectionResult NativeSurfaceFailure(
	EBuiltinActiveSurfaceProjectionStatus status) noexcept
{
	return { status, std::nullopt };
}

} // namespace

BuiltinPartProjectionResult ProjectBuiltinParts(const layout::WorkbenchLayoutStateSnapshot& snapshot)
{
	if (snapshot.schemaVersion != layout::kWorkbenchLayoutStateSchemaVersion) {
		return { EBuiltinPartProjectionStatus::UnsupportedSchema, std::nullopt };
	}

	std::array<const layout::WorkbenchPartState*, kRequiredBuiltinParts.size()> parts{};
	std::array<std::size_t, kRequiredBuiltinParts.size()> counts{};
	for (const auto& part : snapshot.parts) {
		const auto requiredIndex = RequiredPartIndex(part.partId);
		if (!requiredIndex) continue;
		++counts[*requiredIndex];
		if (counts[*requiredIndex] == 1) parts[*requiredIndex] = &part;
	}

	for (const auto count : counts) {
		if (count == 0) return { EBuiltinPartProjectionStatus::MissingRequiredPart, std::nullopt };
		if (count > 1) return { EBuiltinPartProjectionStatus::DuplicateRequiredPart, std::nullopt };
	}

	for (std::size_t index = 0; index < parts.size(); ++index) {
		const auto& part = *parts[index];
		if (part.position != kRequiredBuiltinParts[index].position) {
			return { EBuiltinPartProjectionStatus::UnsupportedPosition, std::nullopt };
		}
		if (part.committedExtentDip
			&& (*part.committedExtentDip == 0
				|| *part.committedExtentDip > layout::kMaximumWorkbenchLayoutCommittedExtentDip)) {
			return { EBuiltinPartProjectionStatus::InvalidExtent, std::nullopt };
		}
	}

	return {
		EBuiltinPartProjectionStatus::Succeeded,
		BuiltinPartProjection{
			{ parts[0]->visible, parts[0]->committedExtentDip },
			{ parts[1]->visible, parts[1]->committedExtentDip },
			{ parts[2]->visible, parts[2]->committedExtentDip },
		},
	};
}

BuiltinActiveSurfaceProjectionResult ProjectBuiltinActiveSurfaces(
	const layout::WorkbenchLayoutStateSnapshot& snapshot)
{
	if (snapshot.schemaVersion != layout::kWorkbenchLayoutStateSchemaVersion) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::UnsupportedSchema);
	}

	BuiltinActiveSurfaceProjection projection;
	for (const auto& activeLocation : kActiveLocations) {
		const auto& activeId = snapshot.activeContainers.*(activeLocation.activeId);
		if (!activeId) continue;

		bool duplicateContainer = false;
		const auto* container = FindUniqueById(snapshot.containers, *activeId,
			&layout::WorkbenchViewContainerState::containerId, duplicateContainer);
		if (duplicateContainer) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::DuplicateContainer);
		if (container == nullptr || container->location != activeLocation.location || !container->visible) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidActiveContainer);
		}
		if (!container->activeViewId) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidActiveView);
		}

		bool duplicateView = false;
		const auto* view = FindUniqueById(snapshot.views, *container->activeViewId,
			&layout::WorkbenchViewState::viewId, duplicateView);
		if (duplicateView) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::DuplicateView);
		if (view == nullptr || view->containerId != container->containerId || !view->visible) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidActiveView);
		}

		const auto* mapping = FindNativeSurface(container->containerId, view->viewId);
		if (mapping == nullptr || (mapping->locations & LocationBit(activeLocation.location)) == 0) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::UnsupportedSurface);
		}
		projection.*(activeLocation.surface) = mapping->surface;
	}

	const auto& focus = snapshot.focus;
	if (!focus.partId && !focus.containerId && !focus.viewId) {
		return { EBuiltinActiveSurfaceProjectionStatus::Succeeded, std::move(projection) };
	}
	if (!focus.viewId && !focus.containerId) {
		bool duplicatePart = false;
		const auto* part = FindUniqueById(snapshot.parts, *focus.partId,
			&layout::WorkbenchPartState::partId, duplicatePart);
		if (duplicatePart) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InconsistentHierarchy);
		if (part == nullptr || !part->visible) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
		}
		if (part->partId == layout::ids::part::Editor) projection.focus = BuiltinActiveSurface::Editor;
		// Other valid part-only focus states have no supported ViewContainer/View host.
		return { EBuiltinActiveSurfaceProjectionStatus::Succeeded, std::move(projection) };
	}

	const layout::WorkbenchViewContainerState* focusedContainer = nullptr;
	if (focus.containerId) {
		bool duplicateContainer = false;
		focusedContainer = FindUniqueById(snapshot.containers, *focus.containerId,
			&layout::WorkbenchViewContainerState::containerId, duplicateContainer);
		if (duplicateContainer) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::DuplicateContainer);
		if (focusedContainer == nullptr || !focusedContainer->visible) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
		}
	}

	const layout::WorkbenchViewState* focusedView = nullptr;
	if (focus.viewId) {
		bool duplicateView = false;
		focusedView = FindUniqueById(snapshot.views, *focus.viewId,
			&layout::WorkbenchViewState::viewId, duplicateView);
		if (duplicateView) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::DuplicateView);
		if (focusedView == nullptr || !focusedView->visible) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
		}
		if (focusedContainer != nullptr && focusedContainer->containerId != focusedView->containerId) {
			return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InconsistentHierarchy);
		}
		if (focusedContainer == nullptr) {
			bool duplicateContainer = false;
			focusedContainer = FindUniqueById(snapshot.containers, focusedView->containerId,
				&layout::WorkbenchViewContainerState::containerId, duplicateContainer);
			if (duplicateContainer) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::DuplicateContainer);
			if (focusedContainer == nullptr || !focusedContainer->visible) {
				return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
			}
		}
	}

	if (!focusedContainer->activeViewId) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
	}
	bool duplicateActiveView = false;
	const auto* activeView = FindUniqueById(snapshot.views, *focusedContainer->activeViewId,
		&layout::WorkbenchViewState::viewId, duplicateActiveView);
	if (duplicateActiveView) return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::DuplicateView);
	if (activeView == nullptr || !activeView->visible || activeView->containerId != focusedContainer->containerId) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
	}
	if (focusedView != nullptr && focusedView->viewId != activeView->viewId) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
	}

	const auto* mapping = FindNativeSurface(focusedContainer->containerId, activeView->viewId);
	if (mapping == nullptr || (mapping->locations & LocationBit(focusedContainer->location)) == 0) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::UnsupportedSurface);
	}
	// The container's own committed location decides the Part, so a relocated container
	// resolves against the Secondary Side Bar rather than its original home.
	const auto* activeLocation = FindActiveLocation(focusedContainer->location);
	if (activeLocation == nullptr) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InconsistentHierarchy);
	}
	const auto& activeId = snapshot.activeContainers.*(activeLocation->activeId);
	if (!activeId || *activeId != focusedContainer->containerId) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InvalidFocus);
	}

	const auto requiredPartId = PartIdForLocation(focusedContainer->location);
	const auto& focusPartId = focus.partId ? *focus.partId : requiredPartId;
	bool duplicatePart = false;
	const auto* part = FindUniqueById(snapshot.parts, focusPartId,
		&layout::WorkbenchPartState::partId, duplicatePart);
	if (duplicatePart || part == nullptr || !part->visible || part->partId != requiredPartId) {
		return NativeSurfaceFailure(EBuiltinActiveSurfaceProjectionStatus::InconsistentHierarchy);
	}

	projection.focus = mapping->surface;
	return { EBuiltinActiveSurfaceProjectionStatus::Succeeded, std::move(projection) };
}

BuiltinWorkbenchProjectionResult ProjectBuiltinWorkbench(
	const layout::WorkbenchLayoutStateSnapshot& snapshot)
{
	const auto parts = ProjectBuiltinParts(snapshot);
	if (!parts.Succeeded()) {
		return { EBuiltinWorkbenchProjectionStatus::PartProjectionFailed, std::nullopt };
	}

	const auto surfaces = ProjectBuiltinActiveSurfaces(snapshot);
	if (!surfaces.Succeeded()) {
		return { EBuiltinWorkbenchProjectionStatus::ActiveSurfaceProjectionFailed, std::nullopt };
	}

	return {
		EBuiltinWorkbenchProjectionStatus::Succeeded,
		BuiltinWorkbenchProjection{
			*parts.projection,
			*surfaces.projection,
		},
	};
}

} // namespace workbench::win32
