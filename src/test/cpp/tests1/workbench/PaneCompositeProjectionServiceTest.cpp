/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/win32/PaneCompositeProjectionService.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Location = workbench::layout::EWorkbenchViewContainerLocation;
using HostState = workbench::win32::PaneCompositeHostState;
using ApplyStatus = workbench::win32::EPaneCompositeHostApplyStatus;

constexpr std::string_view kCompositeId = "publisher.test.composite";
constexpr std::string_view kCompositeViewId = "publisher.test.composite.view";

constexpr std::array<Location, workbench::win32::kPaneCompositeHostCount> kLocations{
	Location::SideBar,
	Location::Panel,
	Location::AuxiliaryBar,
};

constexpr std::size_t LocationIndex(const Location location)
{
	switch (location) {
	case Location::SideBar: return 0;
	case Location::Panel: return 1;
	case Location::AuxiliaryBar: return 2;
	}
	return kLocations.size();
}

std::string_view PartIdForLocation(const Location location)
{
	switch (location) {
	case Location::SideBar: return workbench::layout::ids::part::Sidebar;
	case Location::Panel: return workbench::layout::ids::part::Panel;
	case Location::AuxiliaryBar: return workbench::layout::ids::part::Auxiliarybar;
	}
	return {};
}

workbench::layout::WorkbenchLayoutStateSnapshot CompositeSnapshot(
	const Location location, const bool explicitFocus = false)
{
	using namespace workbench::layout;
	WorkbenchLayoutStateSnapshot snapshot;
	snapshot.generation = 17;
	snapshot.revision = 41;
	snapshot.parts = {
		{ std::string(ids::part::Sidebar), true, EWorkbenchPartPosition::Left, 280U },
		{ std::string(ids::part::Panel), true, EWorkbenchPartPosition::Bottom, 240U },
		{ std::string(ids::part::Auxiliarybar), true, EWorkbenchPartPosition::Right, 320U },
	};
	snapshot.containers = {
		{ std::string(kCompositeId), location, 10, true, std::string(kCompositeViewId) },
		{ std::string(ids::viewContainer::Terminal),
			EWorkbenchViewContainerLocation::Panel, 20, true,
			std::string(ids::view::Terminal) },
	};
	snapshot.views = {
		{ std::string(kCompositeViewId), std::string(kCompositeId), 10, true },
		{ std::string(ids::view::Terminal), std::string(ids::viewContainer::Terminal), 20, true },
	};
	if (location == Location::SideBar) snapshot.activeContainers.sideBar = kCompositeId;
	if (location == Location::AuxiliaryBar) snapshot.activeContainers.auxiliaryBar = kCompositeId;
	snapshot.activeContainers.panel = location == Location::Panel
		? std::optional<std::string>(kCompositeId)
		: std::optional<std::string>(ids::viewContainer::Terminal);
	if (explicitFocus) {
		snapshot.focus = {
			.partId = std::string(PartIdForLocation(location)),
			.containerId = std::string(kCompositeId),
			.viewId = std::string(kCompositeViewId),
		};
	}
	return snapshot;
}

HostState ProjectedHostState(
	const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot, const Location location)
{
	HostState state;
	state.location = location;
	state.partId = PartIdForLocation(location);
	const auto part = std::ranges::find(snapshot.parts, state.partId,
		&workbench::layout::WorkbenchPartState::partId);
	if (part != snapshot.parts.end()) {
		state.visible = part->visible;
		state.committedExtentDip = part->committedExtentDip;
	}
	const std::optional<std::string>* active = nullptr;
	switch (location) {
	case Location::SideBar: active = &snapshot.activeContainers.sideBar; break;
	case Location::Panel: active = &snapshot.activeContainers.panel; break;
	case Location::AuxiliaryBar: active = &snapshot.activeContainers.auxiliaryBar; break;
	}
	if (active != nullptr && *active) {
		state.activeContainerId = **active;
		state.attachedContainerId = **active;
	}
	return state;
}

class FakePaneCompositeHosts final {
public:
	explicit FakePaneCompositeHosts(
		const workbench::layout::WorkbenchLayoutStateSnapshot& initialSnapshot)
	{
		for (const auto location : kLocations) {
			const auto index = LocationIndex(location);
			states[index] = ProjectedHostState(initialSnapshot, location);
			initial[index] = states[index];
			if (states[index].attachedContainerId == kCompositeId) {
				compositeOwner = location;
				attachedCompositeInstances[index] = compositeInstance;
			}
		}
	}

	std::array<workbench::win32::PaneCompositeHostBinding,
		workbench::win32::kPaneCompositeHostCount> Bindings()
	{
		std::array<workbench::win32::PaneCompositeHostBinding,
			workbench::win32::kPaneCompositeHostCount> bindings;
		for (const auto location : kLocations) {
			const auto index = LocationIndex(location);
			auto& binding = bindings[index];
			binding.location = location;
			binding.supportsContainer = [this, location](const std::string_view containerId) {
				const auto& ids = supported[LocationIndex(location)];
				return std::ranges::find(ids, containerId) != ids.end();
			};
			binding.canApply = [this, location](const HostState& state) {
				return state.location == location && state.partId == PartIdForLocation(location)
					&& (!state.activeContainerId
						|| Supports(location, *state.activeContainerId))
					&& (!state.attachedContainerId
						|| state.attachedContainerId == state.activeContainerId);
			};
			binding.readState = [this, index] {
				++readCalls[index];
				return std::optional<HostState>(states[index]);
			};
			binding.applyState = [this, location](const HostState& desired) {
				return Apply(location, desired);
			};
			binding.closeProjection = [this, index] {
				++closeCalls[index];
				return !failClose[index];
			};
		}
		return bindings;
	}

	bool Supports(const Location location, const std::string_view containerId) const
	{
		const auto& ids = supported[LocationIndex(location)];
		return std::ranges::find(ids, containerId) != ids.end();
	}

	ApplyStatus Apply(const Location location, const HostState& desired)
	{
		const auto index = LocationIndex(location);
		++applyCalls[index];
		if (compensating && lieDuringCompensation == location
			&& desired == initial[index]) {
			lieDuringCompensation.reset();
			return ApplyStatus::Applied;
		}
		if (failNext == location) {
			failNext.reset();
			compensating = true;
			return ApplyStatus::Failed;
		}
		if (lieNext == location) {
			lieNext.reset();
			compensating = true;
			return ApplyStatus::Applied;
		}
		if (partialNext == location) {
			partialNext.reset();
			states[index].visible = desired.visible;
			compensating = true;
			return ApplyStatus::Applied;
		}
		if (states[index].attachedContainerId == kCompositeId
			&& desired.attachedContainerId != states[index].attachedContainerId) {
			attachedCompositeInstances[index].reset();
			if (compositeOwner == location) compositeOwner.reset();
		}
		if (desired.attachedContainerId) {
			for (const auto other : kLocations) {
				const auto otherIndex = LocationIndex(other);
				if (otherIndex != index
					&& states[otherIndex].attachedContainerId == desired.attachedContainerId) {
					states[otherIndex].activeContainerId.reset();
					states[otherIndex].attachedContainerId.reset();
					if (*desired.attachedContainerId == kCompositeId) {
						attachedCompositeInstances[otherIndex].reset();
					}
				}
			}
			if (*desired.attachedContainerId == kCompositeId) {
				compositeOwner = location;
				attachedCompositeInstances[index] = compositeInstance;
			}
		}
		states[index] = desired;
		return ApplyStatus::Applied;
	}

	std::array<HostState, workbench::win32::kPaneCompositeHostCount> states;
	std::array<HostState, workbench::win32::kPaneCompositeHostCount> initial;
	std::array<std::vector<std::string>, workbench::win32::kPaneCompositeHostCount> supported{
		std::vector<std::string>{ std::string(kCompositeId) },
		std::vector<std::string>{ std::string(kCompositeId),
			std::string(workbench::layout::ids::viewContainer::Terminal) },
		std::vector<std::string>{ std::string(kCompositeId) },
	};
	std::array<int, workbench::win32::kPaneCompositeHostCount> applyCalls{};
	std::array<int, workbench::win32::kPaneCompositeHostCount> readCalls{};
	std::array<int, workbench::win32::kPaneCompositeHostCount> closeCalls{};
	std::array<bool, workbench::win32::kPaneCompositeHostCount> failClose{};
	const std::shared_ptr<const int> compositeInstance = std::make_shared<const int>(17);
	std::array<std::shared_ptr<const int>, workbench::win32::kPaneCompositeHostCount>
		attachedCompositeInstances;
	std::optional<Location> compositeOwner;
	std::optional<Location> failNext;
	std::optional<Location> lieNext;
	std::optional<Location> partialNext;
	std::optional<Location> lieDuringCompensation;
	bool compensating{};
};

TEST(PaneCompositeProjectionService, ProjectsThreePartsWithoutInferringFocus)
{
	const auto snapshot = CompositeSnapshot(Location::SideBar);
	FakePaneCompositeHosts hosts(snapshot);
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

	const auto prepared = service.Prepare(snapshot);
	ASSERT_TRUE(prepared.Succeeded());
	const auto committed = service.Commit(*prepared.token);
	ASSERT_EQ(workbench::win32::EPaneCompositeCommitStatus::Committed, committed.status);
	const auto* projection = service.LastCommittedProjection();
	ASSERT_NE(nullptr, projection);
	EXPECT_FALSE(projection->focus);
	EXPECT_EQ(snapshot.generation, projection->generation);
	EXPECT_EQ(snapshot.revision, projection->revision);
	for (const auto location : kLocations) {
		EXPECT_EQ(ProjectedHostState(snapshot, location),
			projection->hosts[LocationIndex(location)]);
	}
}

TEST(PaneCompositeProjectionService, FirstRunAndRestartHydrationKeepObservedNativeExtents)
{
	auto snapshot = CompositeSnapshot(Location::SideBar);
	for (auto& part : snapshot.parts) part.committedExtentDip.reset();
	FakePaneCompositeHosts hosts(snapshot);
	constexpr std::array<std::uint32_t, workbench::win32::kPaneCompositeHostCount>
		nativeExtents{ 280U, 240U, 320U };
	for (const auto location : kLocations) {
		const auto index = LocationIndex(location);
		hosts.states[index].committedExtentDip = nativeExtents[index];
		hosts.initial[index] = hosts.states[index];
	}

	{
		workbench::win32::PaneCompositeProjectionService firstRun(hosts.Bindings());
		const auto prepared = firstRun.Prepare(snapshot);
		ASSERT_TRUE(prepared.Succeeded());
		ASSERT_EQ(workbench::win32::EPaneCompositeCommitStatus::Committed,
			firstRun.Commit(*prepared.token).status);
		const auto* projection = firstRun.LastCommittedProjection();
		ASSERT_NE(nullptr, projection);
		for (const auto location : kLocations) {
			EXPECT_EQ(nativeExtents[LocationIndex(location)],
				projection->hosts[LocationIndex(location)].committedExtentDip);
		}
	}

	// A restarted service receives the same hydrated logical absence while the
	// existing native hosts retain their initialized extents. It must not invent
	// a mismatch or require the memento to persist default geometry.
	snapshot.generation++;
	snapshot.revision++;
	workbench::win32::PaneCompositeProjectionService restarted(hosts.Bindings());
	const auto prepared = restarted.Prepare(snapshot);
	ASSERT_TRUE(prepared.Succeeded());
	const auto committed = restarted.Commit(*prepared.token);
	ASSERT_EQ(workbench::win32::EPaneCompositeCommitStatus::Committed, committed.status);
	for (const auto location : kLocations) {
		ASSERT_TRUE(committed.finalStates[LocationIndex(location)]);
		EXPECT_EQ(nativeExtents[LocationIndex(location)],
			committed.finalStates[LocationIndex(location)]->committedExtentDip);
	}
}

TEST(PaneCompositeProjectionService, AcceptsTheCompleteNativeHostExtentDomain)
{
	for (const std::uint32_t extent : { 10'000U, 10'001U,
		workbench::layout::kMaximumWorkbenchLayoutCommittedExtentDip }) {
		const auto initial = CompositeSnapshot(Location::SideBar);
		auto desired = initial;
		for (auto& part : desired.parts) part.committedExtentDip = extent;
		FakePaneCompositeHosts hosts(initial);
		workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

		const auto prepared = service.Prepare(desired);
		ASSERT_TRUE(prepared.Succeeded()) << extent;
		const auto committed = service.Commit(*prepared.token);
		ASSERT_EQ(workbench::win32::EPaneCompositeCommitStatus::Committed,
			committed.status) << extent;
		for (const auto location : kLocations) {
			ASSERT_TRUE(committed.finalStates[LocationIndex(location)]);
			EXPECT_EQ(extent,
				committed.finalStates[LocationIndex(location)]->committedExtentDip);
		}
	}

	auto oversized = CompositeSnapshot(Location::SideBar);
	oversized.parts.front().committedExtentDip =
		workbench::layout::kMaximumWorkbenchLayoutCommittedExtentDip + 1U;
	FakePaneCompositeHosts hosts(CompositeSnapshot(Location::SideBar));
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());
	const auto rejected = service.Prepare(oversized);
	EXPECT_EQ(workbench::win32::EPaneCompositePrepareStatus::InvalidExtent,
		rejected.status);
	EXPECT_EQ((std::array<int, 3>{}), hosts.applyCalls);
}

TEST(PaneCompositeProjectionService, MovesOneGenericCompositeAcrossAllThreeHostsWithoutRecreation)
{
	auto snapshot = CompositeSnapshot(Location::SideBar, true);
	FakePaneCompositeHosts hosts(snapshot);
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());
	const auto physicalCompositeIdentity = hosts.compositeInstance;

	for (const auto location : kLocations) {
		snapshot = CompositeSnapshot(location, true);
		snapshot.revision += LocationIndex(location);
		const auto prepared = service.Prepare(snapshot);
		ASSERT_TRUE(prepared.Succeeded());
		ASSERT_EQ(workbench::win32::EPaneCompositeCommitStatus::Committed,
			service.Commit(*prepared.token).status);
		EXPECT_EQ(location, hosts.compositeOwner);
		const auto* projection = service.LastCommittedProjection();
		ASSERT_NE(nullptr, projection);
		ASSERT_TRUE(projection->focus);
		EXPECT_EQ(std::string(kCompositeId), projection->focus->containerId);
		EXPECT_EQ(location, projection->focus->location);
		for (const auto candidate : kLocations) {
			const auto& attached = hosts.attachedCompositeInstances[LocationIndex(candidate)];
			if (candidate == location) EXPECT_EQ(physicalCompositeIdentity, attached);
			else EXPECT_EQ(nullptr, attached);
		}
	}
}

TEST(PaneCompositeProjectionService, ReportsOnlyLocationsEveryHostCanActuallySupport)
{
	const auto snapshot = CompositeSnapshot(Location::SideBar);
	FakePaneCompositeHosts hosts(snapshot);
	hosts.supported[LocationIndex(Location::Panel)].erase(
		hosts.supported[LocationIndex(Location::Panel)].begin());
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

	const auto locations = service.SupportedLocations(kCompositeId);
	ASSERT_TRUE(locations.complete);
	EXPECT_TRUE(locations.Contains(Location::SideBar));
	EXPECT_FALSE(locations.Contains(Location::Panel));
	EXPECT_TRUE(locations.Contains(Location::AuxiliaryBar));
	const auto rejected = service.Prepare(CompositeSnapshot(Location::Panel));
	EXPECT_EQ(workbench::win32::EPaneCompositePrepareStatus::UnsupportedContainerLocation,
		rejected.status);
	EXPECT_EQ(Location::Panel, rejected.failedLocation);
	EXPECT_EQ((std::array<int, 3>{}), hosts.applyCalls);
}

TEST(PaneCompositeProjectionService, CompensatesAnExplicitHostFailureExactly)
{
	const auto initial = CompositeSnapshot(Location::SideBar);
	FakePaneCompositeHosts hosts(initial);
	hosts.compositeOwner = Location::SideBar;
	hosts.failNext = Location::Panel;
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

	const auto prepared = service.Prepare(CompositeSnapshot(Location::AuxiliaryBar));
	ASSERT_TRUE(prepared.Succeeded());
	const auto failed = service.Commit(*prepared.token);
	EXPECT_EQ(workbench::win32::EPaneCompositeCommitStatus::HostCommitFailedCompensated,
		failed.status);
	EXPECT_EQ(Location::Panel, failed.failedLocation);
	EXPECT_FALSE(failed.compensationFailedLocation);
	for (const auto location : kLocations) {
		EXPECT_EQ(hosts.initial[LocationIndex(location)], hosts.states[LocationIndex(location)]);
	}
}

TEST(PaneCompositeProjectionService, DetectsLyingAndPartialCallbacksByReadingAllHostsBack)
{
	for (const bool partial : { false, true }) {
		const auto initial = CompositeSnapshot(Location::SideBar);
		FakePaneCompositeHosts hosts(initial);
		hosts.compositeOwner = Location::SideBar;
		if (partial) hosts.partialNext = Location::AuxiliaryBar;
		else hosts.lieNext = Location::AuxiliaryBar;
		workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

		const auto prepared = service.Prepare(CompositeSnapshot(Location::AuxiliaryBar));
		ASSERT_TRUE(prepared.Succeeded());
		const auto failed = service.Commit(*prepared.token);
		EXPECT_EQ(
			workbench::win32::EPaneCompositeCommitStatus::HostVerificationFailedCompensated,
			failed.status);
		EXPECT_EQ(Location::AuxiliaryBar, failed.failedLocation);
		EXPECT_FALSE(failed.compensationFailedLocation);
		for (const auto location : kLocations) {
			EXPECT_EQ(hosts.initial[LocationIndex(location)],
				hosts.states[LocationIndex(location)]);
		}
	}
}

TEST(PaneCompositeProjectionService, FaultsWhenCompensationClaimsSuccessWithoutRestoringState)
{
	const auto initial = CompositeSnapshot(Location::SideBar);
	FakePaneCompositeHosts hosts(initial);
	hosts.compositeOwner = Location::SideBar;
	hosts.failNext = Location::Panel;
	hosts.lieDuringCompensation = Location::SideBar;
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

	const auto prepared = service.Prepare(CompositeSnapshot(Location::AuxiliaryBar));
	ASSERT_TRUE(prepared.Succeeded());
	const auto failed = service.Commit(*prepared.token);
	EXPECT_EQ(workbench::win32::EPaneCompositeCommitStatus::CompensationFailed,
		failed.status);
	EXPECT_EQ(Location::SideBar, failed.compensationFailedLocation);
	EXPECT_EQ(workbench::win32::EPaneCompositePrepareStatus::Faulted,
		service.Prepare(initial).status);
}

TEST(PaneCompositeProjectionService, CancellationAndRestartHaveExplicitTerminalLifetimes)
{
	const auto snapshot = CompositeSnapshot(Location::SideBar, true);
	FakePaneCompositeHosts hosts(snapshot);
	{
		workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());
		const auto prepared = service.Prepare(snapshot);
		ASSERT_TRUE(prepared.Succeeded());
		EXPECT_EQ(workbench::win32::EPaneCompositeCancelStatus::Cancelled,
			service.Cancel(*prepared.token));
		EXPECT_EQ(workbench::win32::EPaneCompositeCommitStatus::StalePreparation,
			service.Commit(*prepared.token).status);
		EXPECT_EQ(workbench::win32::EPaneCompositeCloseStatus::Closed, service.Close());
		EXPECT_EQ(workbench::win32::EPaneCompositeCloseStatus::AlreadyClosed, service.Close());
	}
	EXPECT_EQ((std::array<int, 3>{ 1, 1, 1 }), hosts.closeCalls);

	{
		workbench::win32::PaneCompositeProjectionService restarted(hosts.Bindings());
		const auto prepared = restarted.Prepare(snapshot);
		ASSERT_TRUE(prepared.Succeeded());
		EXPECT_EQ(workbench::win32::EPaneCompositeCommitStatus::Committed,
			restarted.Commit(*prepared.token).status);
	}
	EXPECT_EQ((std::array<int, 3>{ 2, 2, 2 }), hosts.closeCalls);
}

TEST(PaneCompositeProjectionService, RejectsIncoherentFocusBeforeAnyNativeMutation)
{
	auto snapshot = CompositeSnapshot(Location::SideBar, true);
	snapshot.focus.partId = std::string(workbench::layout::ids::part::Panel);
	FakePaneCompositeHosts hosts(CompositeSnapshot(Location::SideBar));
	workbench::win32::PaneCompositeProjectionService service(hosts.Bindings());

	const auto rejected = service.Prepare(snapshot);
	EXPECT_EQ(workbench::win32::EPaneCompositePrepareStatus::InvalidFocus, rejected.status);
	EXPECT_EQ((std::array<int, 3>{}), hosts.applyCalls);
}

} // namespace
