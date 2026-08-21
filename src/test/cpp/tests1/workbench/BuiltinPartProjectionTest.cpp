/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/win32/BuiltinPartProjection.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace {

using workbench::layout::EWorkbenchPartPosition;
using workbench::layout::WorkbenchLayoutStateSnapshot;
using workbench::layout::WorkbenchPartState;
using workbench::layout::WorkbenchViewContainerState;
using workbench::layout::WorkbenchViewState;
using workbench::win32::BuiltinActiveSurface;
using workbench::win32::EBuiltinWorkbenchProjectionStatus;
using workbench::win32::EBuiltinActiveSurfaceProjectionStatus;
using workbench::win32::EBuiltinPartProjectionStatus;
using workbench::win32::ProjectBuiltinActiveSurfaces;
using workbench::win32::ProjectBuiltinParts;
using workbench::win32::ProjectBuiltinWorkbench;

WorkbenchLayoutStateSnapshot Sample()
{
	WorkbenchLayoutStateSnapshot snapshot;
	snapshot.parts = {
		{ std::string(workbench::layout::ids::part::Panel), false, EWorkbenchPartPosition::Bottom, std::nullopt },
		{ "publisher.future.part", false, static_cast<EWorkbenchPartPosition>(255),
			workbench::layout::kMaximumWorkbenchLayoutCommittedExtentDip + 1U },
		{ std::string(workbench::layout::ids::part::Auxiliarybar), true, EWorkbenchPartPosition::Right, 360U },
		{ std::string(workbench::layout::ids::part::Sidebar), true, EWorkbenchPartPosition::Left, 280U },
	};
	return snapshot;
}

void ExpectNoProjection(const workbench::win32::BuiltinPartProjectionResult& result)
{
	EXPECT_FALSE(result.Succeeded());
	EXPECT_FALSE(result.projection.has_value());
}

WorkbenchLayoutStateSnapshot ActiveSurfaceSample()
{
	auto snapshot = Sample();
	snapshot.containers = {
		{ std::string(workbench::layout::ids::viewContainer::Explorer),
			workbench::layout::EWorkbenchViewContainerLocation::SideBar, 0, true,
			std::string(workbench::layout::ids::view::Explorer) },
		{ std::string(workbench::layout::ids::viewContainer::Terminal),
			workbench::layout::EWorkbenchViewContainerLocation::Panel, 0, true,
			std::string(workbench::layout::ids::view::Terminal) },
	};
	snapshot.views = {
		{ std::string(workbench::layout::ids::view::Explorer),
			std::string(workbench::layout::ids::viewContainer::Explorer), 0, true },
		{ std::string(workbench::layout::ids::view::Terminal),
			std::string(workbench::layout::ids::viewContainer::Terminal), 0, true },
	};
	// VS Code's Secondary Side Bar starts empty: it holds a ViewContainer only after the
	// user moves one there, so the default sample has no active auxiliary container.
	snapshot.activeContainers = {
		.sideBar = std::string(workbench::layout::ids::viewContainer::Explorer),
		.panel = std::string(workbench::layout::ids::viewContainer::Terminal),
	};
	return snapshot;
}

void ExpectNoActiveSurfaceProjection(const workbench::win32::BuiltinActiveSurfaceProjectionResult& result)
{
	EXPECT_FALSE(result.Succeeded());
	EXPECT_FALSE(result.projection.has_value());
}

void SelectActiveSurface(WorkbenchLayoutStateSnapshot& snapshot, std::string_view containerId,
	std::string_view viewId, workbench::layout::EWorkbenchViewContainerLocation location)
{
	snapshot.containers = { { std::string(containerId), location, 0, true, std::string(viewId) } };
	snapshot.views = { { std::string(viewId), std::string(containerId), 0, true } };
	snapshot.activeContainers = {};
	switch (location) {
	case workbench::layout::EWorkbenchViewContainerLocation::SideBar:
		snapshot.activeContainers.sideBar = std::string(containerId); break;
	case workbench::layout::EWorkbenchViewContainerLocation::Panel:
		snapshot.activeContainers.panel = std::string(containerId); break;
	case workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar:
		snapshot.activeContainers.auxiliaryBar = std::string(containerId); break;
	}
}

} // namespace

TEST(BuiltinPartProjection, ProjectsBuiltinsOrderIndependentlyAndIgnoresUnrelatedParts)
{
	const auto snapshot = Sample();
	auto reordered = snapshot;
	std::ranges::reverse(reordered.parts);

	const auto first = ProjectBuiltinParts(snapshot);
	const auto second = ProjectBuiltinParts(reordered);
	ASSERT_TRUE(first.Succeeded());
	ASSERT_TRUE(second.Succeeded());
	ASSERT_TRUE(first.projection.has_value());
	ASSERT_TRUE(second.projection.has_value());
	EXPECT_EQ(*first.projection, *second.projection);

	EXPECT_TRUE(first.projection->left.visible);
	ASSERT_TRUE(first.projection->left.committedExtentDip.has_value());
	EXPECT_EQ(280U, *first.projection->left.committedExtentDip);
	EXPECT_FALSE(first.projection->bottom.visible);
	EXPECT_FALSE(first.projection->bottom.committedExtentDip.has_value());
	EXPECT_TRUE(first.projection->right.visible);
	ASSERT_TRUE(first.projection->right.committedExtentDip.has_value());
	EXPECT_EQ(360U, *first.projection->right.committedExtentDip);
}

TEST(BuiltinPartProjection, RejectsUnsupportedSchemaWithoutProjection)
{
	auto snapshot = Sample();
	snapshot.schemaVersion = workbench::layout::kWorkbenchLayoutStateSchemaVersion + 1U;

	const auto result = ProjectBuiltinParts(snapshot);
	EXPECT_EQ(EBuiltinPartProjectionStatus::UnsupportedSchema, result.status);
	ExpectNoProjection(result);
}

TEST(BuiltinPartProjection, RejectsMissingRequiredPartWithoutProjection)
{
	auto snapshot = Sample();
	snapshot.parts.erase(std::ranges::find(snapshot.parts,
		std::string(workbench::layout::ids::part::Panel), &WorkbenchPartState::partId));

	const auto result = ProjectBuiltinParts(snapshot);
	EXPECT_EQ(EBuiltinPartProjectionStatus::MissingRequiredPart, result.status);
	ExpectNoProjection(result);
}

TEST(BuiltinPartProjection, RejectsDuplicateRequiredPartWithoutProjection)
{
	auto snapshot = Sample();
	snapshot.parts.push_back({ std::string(workbench::layout::ids::part::Sidebar), true,
		EWorkbenchPartPosition::Left, 300U });

	const auto result = ProjectBuiltinParts(snapshot);
	EXPECT_EQ(EBuiltinPartProjectionStatus::DuplicateRequiredPart, result.status);
	ExpectNoProjection(result);
}

TEST(BuiltinPartProjection, RejectsUnsupportedRequiredPositionWithoutProjection)
{
	auto snapshot = Sample();
	for (auto& part : snapshot.parts) {
		if (part.partId == workbench::layout::ids::part::Panel) {
			part.position = EWorkbenchPartPosition::Right;
		}
	}

	const auto result = ProjectBuiltinParts(snapshot);
	EXPECT_EQ(EBuiltinPartProjectionStatus::UnsupportedPosition, result.status);
	ExpectNoProjection(result);
}

TEST(BuiltinPartProjection, RejectsOversizedRequiredExtentWithoutProjection)
{
	auto snapshot = Sample();
	for (auto& part : snapshot.parts) {
		if (part.partId == workbench::layout::ids::part::Auxiliarybar) {
			part.committedExtentDip = workbench::layout::kMaximumWorkbenchLayoutCommittedExtentDip + 1U;
		}
	}

	const auto result = ProjectBuiltinParts(snapshot);
	EXPECT_EQ(EBuiltinPartProjectionStatus::InvalidExtent, result.status);
	ExpectNoProjection(result);
}

TEST(BuiltinPartProjection, RejectsZeroRequiredExtentWithoutProjection)
{
	auto snapshot = Sample();
	for (auto& part : snapshot.parts) {
		if (part.partId == workbench::layout::ids::part::Sidebar) {
			part.committedExtentDip = 0U;
		}
	}

	const auto result = ProjectBuiltinParts(snapshot);
	EXPECT_EQ(EBuiltinPartProjectionStatus::InvalidExtent, result.status);
	ExpectNoProjection(result);
}

TEST(BuiltinPartProjection, ProjectsAllSupportedActiveSurfacesWithoutAssigningFocus)
{
	const auto result = ProjectBuiltinActiveSurfaces(ActiveSurfaceSample());
	ASSERT_TRUE(result.Succeeded());
	ASSERT_TRUE(result.projection.has_value());
	ASSERT_TRUE(result.projection->sidebar.has_value());
	EXPECT_EQ(BuiltinActiveSurface::Explorer, *result.projection->sidebar);
	ASSERT_TRUE(result.projection->panel.has_value());
	EXPECT_EQ(BuiltinActiveSurface::Terminal, *result.projection->panel);
	EXPECT_FALSE(result.projection->auxiliaryBar.has_value());
	EXPECT_FALSE(result.projection->focus.has_value());
}

TEST(BuiltinPartProjection, ProjectsEverySupportedContainerViewPair)
{
	struct ExpectedSurface {
		std::string_view containerId;
		std::string_view viewId;
		workbench::layout::EWorkbenchViewContainerLocation location;
		BuiltinActiveSurface surface;
	};
	constexpr std::array expected{
		ExpectedSurface{ workbench::layout::ids::viewContainer::Explorer,
			workbench::layout::ids::view::Explorer,
			workbench::layout::EWorkbenchViewContainerLocation::SideBar, BuiltinActiveSurface::Explorer },
		ExpectedSurface{ workbench::layout::ids::viewContainer::Explorer,
			workbench::layout::ids::view::Outline,
			workbench::layout::EWorkbenchViewContainerLocation::SideBar, BuiltinActiveSurface::Outline },
		ExpectedSurface{ workbench::layout::ids::viewContainer::Search,
			workbench::layout::ids::view::Search,
			workbench::layout::EWorkbenchViewContainerLocation::SideBar, BuiltinActiveSurface::Search },
		ExpectedSurface{ workbench::layout::ids::viewContainer::SourceControl,
			workbench::layout::ids::view::SourceControl,
			workbench::layout::EWorkbenchViewContainerLocation::SideBar, BuiltinActiveSurface::SourceControl },
		ExpectedSurface{ workbench::layout::ids::viewContainer::Terminal,
			workbench::layout::ids::view::Terminal,
			workbench::layout::EWorkbenchViewContainerLocation::Panel, BuiltinActiveSurface::Terminal },
		ExpectedSurface{ workbench::layout::ids::viewContainer::Problems,
			workbench::layout::ids::view::Problems,
			workbench::layout::EWorkbenchViewContainerLocation::Panel, BuiltinActiveSurface::Problems },
		ExpectedSurface{ workbench::layout::ids::viewContainer::Output,
			workbench::layout::ids::view::Output,
			workbench::layout::EWorkbenchViewContainerLocation::Panel, BuiltinActiveSurface::Output },
	};

	for (const auto& expectedSurface : expected) {
		auto snapshot = ActiveSurfaceSample();
		SelectActiveSurface(snapshot, expectedSurface.containerId, expectedSurface.viewId, expectedSurface.location);
		const auto result = ProjectBuiltinActiveSurfaces(snapshot);
		ASSERT_TRUE(result.Succeeded()) << expectedSurface.containerId;
		ASSERT_TRUE(result.projection.has_value());
		switch (expectedSurface.location) {
		case workbench::layout::EWorkbenchViewContainerLocation::SideBar:
			ASSERT_TRUE(result.projection->sidebar.has_value());
			EXPECT_EQ(expectedSurface.surface, *result.projection->sidebar); break;
		case workbench::layout::EWorkbenchViewContainerLocation::Panel:
			ASSERT_TRUE(result.projection->panel.has_value());
			EXPECT_EQ(expectedSurface.surface, *result.projection->panel); break;
		case workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar:
			ASSERT_TRUE(result.projection->auxiliaryBar.has_value());
			EXPECT_EQ(expectedSurface.surface, *result.projection->auxiliaryBar); break;
		}
		EXPECT_FALSE(result.projection->focus.has_value());
	}
}

TEST(BuiltinPartProjection, ProjectsExplicitCoherentFocusButNeverInfersItFromActivation)
{
	auto absentFocus = ActiveSurfaceSample();
	const auto withoutFocus = ProjectBuiltinActiveSurfaces(absentFocus);
	ASSERT_TRUE(withoutFocus.Succeeded());
	ASSERT_TRUE(withoutFocus.projection.has_value());
	EXPECT_FALSE(withoutFocus.projection->focus.has_value());

	auto explicitFocus = std::move(absentFocus);
	explicitFocus.focus = {
		.partId = std::string(workbench::layout::ids::part::Sidebar),
		.containerId = std::string(workbench::layout::ids::viewContainer::Explorer),
		.viewId = std::string(workbench::layout::ids::view::Explorer),
	};
	const auto withFocus = ProjectBuiltinActiveSurfaces(explicitFocus);
	ASSERT_TRUE(withFocus.Succeeded());
	ASSERT_TRUE(withFocus.projection.has_value());
	ASSERT_TRUE(withFocus.projection->focus.has_value());
	EXPECT_EQ(BuiltinActiveSurface::Explorer, *withFocus.projection->focus);
}

TEST(BuiltinPartProjection, ProjectsEditorOnlyFallbackFocusToTheFocusOnlyEditorSurface)
{
	auto snapshot = ActiveSurfaceSample();
	snapshot.parts.push_back({ std::string(workbench::layout::ids::part::Editor), true,
		EWorkbenchPartPosition::Center, std::nullopt });
	snapshot.focus.partId = std::string(workbench::layout::ids::part::Editor);

	const auto result = ProjectBuiltinActiveSurfaces(snapshot);
	ASSERT_TRUE(result.Succeeded());
	ASSERT_TRUE(result.projection.has_value());
	ASSERT_TRUE(result.projection->focus.has_value());
	EXPECT_EQ(BuiltinActiveSurface::Editor, *result.projection->focus);
}

TEST(BuiltinPartProjection, RejectsUnsupportedActiveSurfaceWithoutAffectingPhysicalProjection)
{
	struct UnsupportedSurface {
		std::string_view containerId;
		std::string_view viewId;
		workbench::layout::EWorkbenchViewContainerLocation location;
	};
	constexpr std::array unsupported{
		UnsupportedSurface{ workbench::layout::ids::viewContainer::RunAndDebug,
			workbench::layout::ids::view::DebugVariables, workbench::layout::EWorkbenchViewContainerLocation::SideBar },
		UnsupportedSurface{ workbench::layout::ids::viewContainer::Ports, workbench::layout::ids::view::Ports,
			workbench::layout::EWorkbenchViewContainerLocation::Panel },
		UnsupportedSurface{ workbench::layout::ids::viewContainer::DebugConsole,
			workbench::layout::ids::view::DebugConsole, workbench::layout::EWorkbenchViewContainerLocation::Panel },
		// A Panel container stays in the Panel here. Relocating the whole Panel is
		// `workbench.action.movePanelToSecondarySideBar`, a separate unsupported gate.
		UnsupportedSurface{ workbench::layout::ids::viewContainer::Terminal,
			workbench::layout::ids::view::Terminal,
			workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar },
	};

	for (const auto& unsupportedSurface : unsupported) {
		auto snapshot = ActiveSurfaceSample();
		SelectActiveSurface(snapshot, unsupportedSurface.containerId, unsupportedSurface.viewId,
			unsupportedSurface.location);
		const auto logical = ProjectBuiltinActiveSurfaces(snapshot);
		EXPECT_EQ(EBuiltinActiveSurfaceProjectionStatus::UnsupportedSurface, logical.status);
		ExpectNoActiveSurfaceProjection(logical);

		const auto physical = ProjectBuiltinParts(snapshot);
		EXPECT_TRUE(physical.Succeeded());
	}
}

//! An extension-contributed ViewContainer is outside this projector's vocabulary, not a
//! malformed snapshot. `WorkbenchLayoutStateService` activates each location's first visible
//! container, so installing one extension makes its container the active container of a Part;
//! failing here would abort every later layout projection for the rest of the session, which is
//! exactly the Issue #29 defect (every Part toggle, Outline reveal, and focus apply no-opped).
TEST(BuiltinPartProjection, LeavesContributedActiveViewContainersUnprojectedInsteadOfFailing)
{
	constexpr std::array locations{
		workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar,
		workbench::layout::EWorkbenchViewContainerLocation::SideBar,
		workbench::layout::EWorkbenchViewContainerLocation::Panel,
	};

	for (const auto location : locations) {
		auto snapshot = ActiveSurfaceSample();
		SelectActiveSurface(snapshot, "claude-sidebar-secondary", "claude.view.sidebar", location);

		const auto result = ProjectBuiltinActiveSurfaces(snapshot);
		ASSERT_EQ(EBuiltinActiveSurfaceProjectionStatus::Succeeded, result.status);
		ASSERT_TRUE(result.projection.has_value());
		// No built-in surface can name a contributed container, so every slot stays unset and
		// the window layer resolves the page from the layout state itself.
		EXPECT_FALSE(result.projection->sidebar.has_value());
		EXPECT_FALSE(result.projection->auxiliaryBar.has_value());
		EXPECT_FALSE(result.projection->panel.has_value());
		EXPECT_FALSE(result.projection->focus.has_value());

		// The whole composite must survive too: this is what the Window adapter actually calls.
		const auto composite = ProjectBuiltinWorkbench(snapshot);
		EXPECT_TRUE(composite.Succeeded());
	}
}

//! A contributed container may also hold focus, and focus is the other half of the same rule.
TEST(BuiltinPartProjection, LeavesFocusOnAContributedViewContainerUnprojectedInsteadOfFailing)
{
	auto snapshot = ActiveSurfaceSample();
	SelectActiveSurface(snapshot, "claude-sidebar-secondary", "claude.view.sidebar",
		workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar);
	snapshot.focus = {
		.partId = std::string(workbench::layout::ids::part::Auxiliarybar),
		.containerId = std::string("claude-sidebar-secondary"),
		.viewId = std::string("claude.view.sidebar"),
	};

	const auto result = ProjectBuiltinActiveSurfaces(snapshot);
	ASSERT_EQ(EBuiltinActiveSurfaceProjectionStatus::Succeeded, result.status);
	ASSERT_TRUE(result.projection.has_value());
	EXPECT_FALSE(result.projection->focus.has_value());
	EXPECT_FALSE(result.projection->auxiliaryBar.has_value());
}

//! Skipping contributed containers must not weaken the built-in boundary: a container the
//! product itself declares but cannot render still fails closed, in every location.
TEST(BuiltinPartProjection, StillFailsClosedForBuiltinViewContainersWithNoNativeSurface)
{
	constexpr std::array unimplemented{
		std::pair{ workbench::layout::ids::viewContainer::RunAndDebug,
			workbench::layout::ids::view::DebugVariables },
	};

	for (const auto& [containerId, viewId] : unimplemented) {
		for (const auto location : { workbench::layout::EWorkbenchViewContainerLocation::SideBar,
				workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar }) {
			auto snapshot = ActiveSurfaceSample();
			SelectActiveSurface(snapshot, containerId, viewId, location);
			const auto result = ProjectBuiltinActiveSurfaces(snapshot);
			EXPECT_EQ(EBuiltinActiveSurfaceProjectionStatus::UnsupportedSurface, result.status);
			ExpectNoActiveSurfaceProjection(result);
		}
	}
}

//! VS Code's `CompositeDragAndDrop` moves an Activity Bar ViewContainer between the
//! Primary and the Secondary Side Bar, so the same container/view pair must project into
//! whichever side bar currently owns it -- and into that one only.
TEST(BuiltinPartProjection, ProjectsSideBarContainersMovedIntoTheSecondarySideBar)
{
	constexpr std::array movable{
		std::pair{ workbench::layout::ids::viewContainer::Explorer, BuiltinActiveSurface::Explorer },
		std::pair{ workbench::layout::ids::viewContainer::SourceControl, BuiltinActiveSurface::SourceControl },
	};
	constexpr std::array views{
		workbench::layout::ids::view::Explorer,
		workbench::layout::ids::view::SourceControl,
	};

	for (std::size_t index = 0; index < movable.size(); ++index) {
		auto snapshot = ActiveSurfaceSample();
		SelectActiveSurface(snapshot, movable[index].first, views[index],
			workbench::layout::EWorkbenchViewContainerLocation::AuxiliaryBar);

		const auto result = ProjectBuiltinActiveSurfaces(snapshot);
		ASSERT_TRUE(result.Succeeded());
		ASSERT_TRUE(result.projection.has_value());
		ASSERT_TRUE(result.projection->auxiliaryBar.has_value());
		EXPECT_EQ(movable[index].second, *result.projection->auxiliaryBar);
		// One ViewContainer has exactly one location, so the Primary Side Bar must not
		// keep claiming a container the model moved away from it.
		EXPECT_FALSE(result.projection->sidebar.has_value());
	}
}

TEST(BuiltinPartProjection, RejectsMalformedActiveAndFocusHierarchiesWithoutPartialProjection)
{
	auto activeMismatch = ActiveSurfaceSample();
	activeMismatch.views.front().containerId = std::string(workbench::layout::ids::viewContainer::Terminal);
	const auto malformedActive = ProjectBuiltinActiveSurfaces(activeMismatch);
	EXPECT_EQ(EBuiltinActiveSurfaceProjectionStatus::InvalidActiveView, malformedActive.status);
	ExpectNoActiveSurfaceProjection(malformedActive);

	auto focusMismatch = ActiveSurfaceSample();
	focusMismatch.focus = {
		.partId = std::string(workbench::layout::ids::part::Sidebar),
		.containerId = std::string(workbench::layout::ids::viewContainer::Explorer),
		.viewId = std::string(workbench::layout::ids::view::Terminal),
	};
	const auto malformedFocus = ProjectBuiltinActiveSurfaces(focusMismatch);
	EXPECT_EQ(EBuiltinActiveSurfaceProjectionStatus::InconsistentHierarchy, malformedFocus.status);
	ExpectNoActiveSurfaceProjection(malformedFocus);

	auto omittedPartFocus = ActiveSurfaceSample();
	omittedPartFocus.focus = {
		.containerId = std::string(workbench::layout::ids::viewContainer::Explorer),
		.viewId = std::string(workbench::layout::ids::view::Explorer),
	};
	for (auto& part : omittedPartFocus.parts) {
		if (part.partId == workbench::layout::ids::part::Sidebar) part.visible = false;
	}
	const auto hiddenImpliedPart = ProjectBuiltinActiveSurfaces(omittedPartFocus);
	EXPECT_EQ(EBuiltinActiveSurfaceProjectionStatus::InconsistentHierarchy, hiddenImpliedPart.status);
	ExpectNoActiveSurfaceProjection(hiddenImpliedPart);
}

TEST(BuiltinPartProjection, ProjectsActiveSurfacesOrderIndependently)
{
	const auto snapshot = ActiveSurfaceSample();
	auto reordered = snapshot;
	std::ranges::reverse(reordered.parts);
	std::ranges::reverse(reordered.containers);
	std::ranges::reverse(reordered.views);

	const auto first = ProjectBuiltinActiveSurfaces(snapshot);
	const auto second = ProjectBuiltinActiveSurfaces(reordered);
	ASSERT_TRUE(first.Succeeded());
	ASSERT_TRUE(second.Succeeded());
	ASSERT_TRUE(first.projection.has_value());
	ASSERT_TRUE(second.projection.has_value());
	EXPECT_EQ(*first.projection, *second.projection);
}

TEST(BuiltinPartProjection, ProjectsCompleteWorkbenchAtomicallyFromOneSnapshot)
{
	const auto result = ProjectBuiltinWorkbench(ActiveSurfaceSample());
	ASSERT_TRUE(result.Succeeded());
	ASSERT_TRUE(result.projection.has_value());
	EXPECT_TRUE(result.projection->parts.left.visible);
	EXPECT_FALSE(result.projection->parts.bottom.visible);
	ASSERT_TRUE(result.projection->surfaces.sidebar.has_value());
	EXPECT_EQ(BuiltinActiveSurface::Explorer, *result.projection->surfaces.sidebar);
	ASSERT_TRUE(result.projection->surfaces.panel.has_value());
	EXPECT_EQ(BuiltinActiveSurface::Terminal, *result.projection->surfaces.panel);
}

TEST(BuiltinPartProjection, DoesNotExposePhysicalProjectionWhenActiveSurfaceProjectionFails)
{
	auto snapshot = ActiveSurfaceSample();
	// Debug Console is a real VS Code ViewContainer with no native surface here, so it is the
	// honest way to fail this projection; an extension-contributed container is not, because it
	// is outside this projector's vocabulary rather than an unimplemented capability.
	SelectActiveSurface(snapshot, workbench::layout::ids::viewContainer::DebugConsole,
		workbench::layout::ids::view::DebugConsole,
		workbench::layout::EWorkbenchViewContainerLocation::Panel);

	const auto result = ProjectBuiltinWorkbench(snapshot);
	EXPECT_EQ(EBuiltinWorkbenchProjectionStatus::ActiveSurfaceProjectionFailed, result.status);
	EXPECT_FALSE(result.Succeeded());
	EXPECT_FALSE(result.projection.has_value());
}
