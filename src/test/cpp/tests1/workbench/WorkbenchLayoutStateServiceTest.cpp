/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/layout/WorkbenchLayoutStateService.h"

namespace workbench::layout {
namespace {

WorkbenchContributionOwner Owner(const char* id, std::uint64_t generation = 1)
{
	return { .ownerId = id, .generation = generation };
}

WorkbenchContributionSnapshot Contribute(WorkbenchContributionRegistry& registry)
{
	const auto registered = registry.Register({
		.operation = { .operationId = "add-layout-sample" },
		.owner = Owner("layout.sample"),
		.viewContainers = {
			{ .id = "sample.movable", .title = "Movable", .location = EViewContainerLocation::Sidebar, .order = 40 },
			{ .id = "sample.fixed", .title = "Fixed", .location = EViewContainerLocation::Panel, .order = 50, .canMove = false },
		},
		.views = {
			{ .id = "sample.movable.view", .containerId = "sample.movable", .title = "Movable view", .order = 20, .canToggleVisibility = true, .canMove = true },
			{ .id = "sample.fixed.view", .containerId = "sample.fixed", .title = "Fixed view", .order = 10, .canToggleVisibility = false, .canMove = false },
		},
	});
	EXPECT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registered.status);
	return registry.Snapshot();
}

const WorkbenchPartState& Part(const WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	const auto found = std::find_if(snapshot.parts.begin(), snapshot.parts.end(), [id](const auto& value) { return value.partId == id; });
	EXPECT_NE(snapshot.parts.end(), found);
	return *found;
}

const WorkbenchViewContainerState& Container(const WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	const auto found = std::find_if(snapshot.containers.begin(), snapshot.containers.end(), [id](const auto& value) { return value.containerId == id; });
	EXPECT_NE(snapshot.containers.end(), found);
	return *found;
}

const WorkbenchViewState& View(const WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	const auto found = std::find_if(snapshot.views.begin(), snapshot.views.end(), [id](const auto& value) { return value.viewId == id; });
	EXPECT_NE(snapshot.views.end(), found);
	return *found;
}

bool HasPart(const WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	return std::any_of(snapshot.parts.begin(), snapshot.parts.end(), [id](const auto& value) { return value.partId == id; });
}

bool HasContainer(const WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	return std::any_of(snapshot.containers.begin(), snapshot.containers.end(), [id](const auto& value) { return value.containerId == id; });
}

bool HasView(const WorkbenchLayoutStateSnapshot& snapshot, std::string_view id)
{
	return std::any_of(snapshot.views.begin(), snapshot.views.end(), [id](const auto& value) { return value.viewId == id; });
}

TEST(WorkbenchLayoutStateService, InitializesCanonicalIdsAndKeepsRightPartSeparateFromOutlineView)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto snapshot = state.Snapshot();

	EXPECT_EQ(0U, snapshot.revision);
	EXPECT_EQ(EWorkbenchPartPosition::Right, Part(snapshot, ids::part::Auxiliarybar).position);
	EXPECT_EQ(EWorkbenchPartPosition::Left, Part(snapshot, ids::part::Sidebar).position);
	EXPECT_TRUE(Part(snapshot, ids::part::Sidebar).visible);
	EXPECT_FALSE(Part(snapshot, ids::part::Panel).visible);
	EXPECT_FALSE(Part(snapshot, ids::part::Auxiliarybar).visible);
	EXPECT_EQ(EWorkbenchViewContainerLocation::AuxiliaryBar, Container(snapshot, ids::viewContainer::LegacyExtensionViewsAuxiliary).location);
	EXPECT_EQ(std::string(ids::view::Explorer), *Container(snapshot, ids::viewContainer::Explorer).activeViewId);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer), *snapshot.activeContainers.sideBar);
	EXPECT_EQ(std::string(ids::viewContainer::Problems), *snapshot.activeContainers.panel);
	EXPECT_EQ(std::string(ids::viewContainer::LegacyExtensionViewsAuxiliary),
		*snapshot.activeContainers.auxiliaryBar);
	EXPECT_NE(std::string(ids::part::Auxiliarybar), std::string(ids::viewContainer::LegacyExtensionViewsAuxiliary));
}

TEST(WorkbenchLayoutStateService, ContainerAndViewActivationPreserveSelectionAndSwitchTheLocationOwner)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());

	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateView({ .operation = { .operationId = "select-outline" },
			.viewId = std::string(ids::view::Outline) }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateContainer({ .operation = { .operationId = "select-scm" },
			.containerId = std::string(ids::viewContainer::SourceControl) }).status);
	EXPECT_EQ(std::string(ids::viewContainer::SourceControl),
		*state.Snapshot().activeContainers.sideBar);

	const auto explorer = state.ActivateContainer({
		.operation = { .operationId = "restore-explorer" },
		.containerId = std::string(ids::viewContainer::Explorer),
	});
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, explorer.status);
	EXPECT_EQ(std::string(ids::view::Outline),
		*Container(explorer.snapshot, ids::viewContainer::Explorer).activeViewId);

	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateContainer({ .operation = { .operationId = "select-scm-again" },
			.containerId = std::string(ids::viewContainer::SourceControl) }).status);
	const auto alreadySelectedView = state.ActivateView({
		.operation = { .operationId = "activate-selected-explorer-view" },
		.viewId = std::string(ids::view::Outline),
	});
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, alreadySelectedView.status);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer),
		*alreadySelectedView.snapshot.activeContainers.sideBar);
	EXPECT_FALSE(alreadySelectedView.snapshot.focus.viewId);
}

TEST(WorkbenchLayoutStateService, FocusRequiresAVisibleActiveCoherentHierarchy)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(Contribute(registry));

	const auto wrongPart = state.SetFocus({
		.operation = { .operationId = "focus-wrong-part" },
		.focus = { .partId = std::string(ids::part::Editor),
			.containerId = std::string(ids::viewContainer::Explorer),
			.viewId = std::string(ids::view::Explorer) },
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, wrongPart.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::InconsistentHierarchy, wrongPart.reason);

	const auto wrongContainer = state.SetFocus({
		.operation = { .operationId = "focus-wrong-container" },
		.focus = { .containerId = std::string(ids::viewContainer::Explorer),
			.viewId = std::string(ids::view::SourceControl) },
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, wrongContainer.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::InconsistentHierarchy, wrongContainer.reason);

	const auto inactive = state.SetFocus({
		.operation = { .operationId = "focus-inactive" },
		.focus = { .containerId = std::string(ids::viewContainer::SourceControl) },
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, inactive.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::TargetNotActive, inactive.reason);

	const auto hiddenPanel = state.SetFocus({
		.operation = { .operationId = "focus-hidden-panel" },
		.focus = { .containerId = std::string(ids::viewContainer::Problems),
			.viewId = std::string(ids::view::Problems) },
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, hiddenPanel.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::TargetNotVisible, hiddenPanel.reason);
}

TEST(WorkbenchLayoutStateService, ToggleActivateAndFocusAreSeparateStateTransitions)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(Contribute(registry));
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPartVisibility({ .operation = { .operationId = "show-panel" }, .partId = std::string(ids::part::Panel), .visible = true }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPartVisibility({ .operation = { .operationId = "hide-panel" }, .partId = std::string(ids::part::Panel), .visible = false }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetViewVisibility({ .operation = { .operationId = "hide-movable" }, .viewId = "sample.movable.view", .visible = false }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateView({ .operation = { .operationId = "activate-movable" }, .viewId = "sample.movable.view" }).status);
	auto snapshot = state.Snapshot();
	EXPECT_FALSE(Part(snapshot, ids::part::Panel).visible);
	EXPECT_EQ("sample.movable.view", *Container(snapshot, "sample.movable").activeViewId);
	EXPECT_TRUE(View(snapshot, "sample.movable.view").visible);
	EXPECT_FALSE(snapshot.focus.viewId);

	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetFocus({ .operation = { .operationId = "focus-movable" }, .focus = { .containerId = "sample.movable", .viewId = "sample.movable.view" } }).status);
	snapshot = state.Snapshot();
	EXPECT_EQ("sample.movable", *snapshot.focus.containerId);
	EXPECT_EQ("sample.movable.view", *snapshot.focus.viewId);
	EXPECT_FALSE(Part(snapshot, ids::part::Panel).visible);
}

TEST(WorkbenchLayoutStateService, HidingOrMovingTheFocusedHierarchyFallsBackToEditor)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(Contribute(registry));
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateView({ .operation = { .operationId = "activate-focused-sample" },
			.viewId = "sample.movable.view" }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetFocus({ .operation = { .operationId = "focus-sample-before-hide" },
			.focus = { .partId = std::string(ids::part::Sidebar),
				.containerId = "sample.movable", .viewId = "sample.movable.view" } }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPartVisibility({ .operation = { .operationId = "hide-focused-sidebar" },
			.partId = std::string(ids::part::Sidebar), .visible = false }).status);
	EXPECT_EQ(std::string(ids::part::Editor), *state.Snapshot().focus.partId);
	EXPECT_FALSE(state.Snapshot().focus.containerId);

	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPartVisibility({ .operation = { .operationId = "show-sidebar-again" },
			.partId = std::string(ids::part::Sidebar), .visible = true }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetFocus({ .operation = { .operationId = "focus-sample-before-move" },
			.focus = { .partId = std::string(ids::part::Sidebar),
				.containerId = "sample.movable", .viewId = "sample.movable.view" } }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.MoveView({ .operation = { .operationId = "move-focused-sample" },
			.viewId = "sample.movable.view", .targetContainerId = "sample.fixed", .order = -3 }).status);
	EXPECT_EQ(std::string(ids::part::Editor), *state.Snapshot().focus.partId);
	EXPECT_FALSE(state.Snapshot().focus.containerId);
	EXPECT_FALSE(state.Snapshot().focus.viewId);
}

TEST(WorkbenchLayoutStateService, AppliesCapabilitiesAndUsesStableViewMoveState)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(Contribute(registry));
	const auto moved = state.MoveView({ .operation = { .operationId = "move-movable" }, .viewId = "sample.movable.view", .targetContainerId = "sample.fixed", .order = -3 });
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, moved.status);
	EXPECT_EQ("sample.fixed", View(state.Snapshot(), "sample.movable.view").containerId);
	EXPECT_EQ(-3, View(state.Snapshot(), "sample.movable.view").order);

	const auto fixed = state.MoveView({ .operation = { .operationId = "move-fixed" }, .viewId = "sample.fixed.view", .targetContainerId = "sample.movable", .order = 4 });
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Unsupported, fixed.status);
	const auto container = state.MoveContainer({ .operation = { .operationId = "move-container" }, .containerId = "sample.movable", .location = EWorkbenchViewContainerLocation::Panel, .order = -1 });
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, container.status);
	EXPECT_EQ(EWorkbenchViewContainerLocation::Panel, Container(state.Snapshot(), "sample.movable").location);
	EXPECT_EQ(-1, Container(state.Snapshot(), "sample.movable").order);
	const auto fixedContainer = state.MoveContainer({ .operation = { .operationId = "move-fixed-container" }, .containerId = "sample.fixed", .location = EWorkbenchViewContainerLocation::SideBar, .order = 1 });
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Unsupported, fixedContainer.status);
}

TEST(WorkbenchLayoutStateService, EnforcesCasAndExactBoundedReplay)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const SetWorkbenchPartExtentRequest request{ .operation = { .operationId = "set-sidebar-extent", .expectedRevision = 0 },
		.partId = std::string(ids::part::Sidebar), .committedExtentDip = 300 };
	const auto committed = state.SetPartExtent(request);
	const auto replayed = state.SetPartExtent(request);
	const auto stale = state.SetPartVisibility({ .operation = { .operationId = "stale", .expectedRevision = 0 }, .partId = std::string(ids::part::Panel), .visible = false });
	auto conflicting = request;
	conflicting.committedExtentDip = 301;
	const auto conflict = state.SetPartExtent(conflicting);

	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, committed.status);
	EXPECT_TRUE(replayed.replayed);
	EXPECT_EQ(committed.revision, replayed.revision);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Conflict, stale.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::RevisionConflict, stale.reason);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::OperationIdConflict, conflict.reason);
	EXPECT_EQ(1U, state.Snapshot().revision);
}

TEST(WorkbenchLayoutStateService, RejectsInvalidPersistedEnumsWithoutMutation)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto invalidAlignment = state.SetPanelAlignment({ .operation = { .operationId = "invalid-alignment" },
		.alignment = static_cast<EWorkbenchPanelAlignment>(255) });
	const auto invalidLocation = state.MoveContainer({ .operation = { .operationId = "invalid-location" },
		.containerId = std::string(ids::viewContainer::Explorer), .location = static_cast<EWorkbenchViewContainerLocation>(255) });
	const auto invalidExtent = state.SetPartExtent({ .operation = { .operationId = "invalid-extent" },
		.partId = std::string(ids::part::Sidebar), .committedExtentDip = kMaximumWorkbenchLayoutCommittedExtentDip + 1U });
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, invalidAlignment.status);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, invalidLocation.status);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, invalidExtent.status);
	EXPECT_EQ(0U, state.Snapshot().revision);
}

TEST(WorkbenchLayoutStateService, ReconcilesDisposedViewsToTheDeterministicRemainingActiveView)
{
	WorkbenchContributionRegistry registry;
	const auto owner = Owner("layout.reconcile.container");
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-reconcile" }, .owner = owner,
		.viewContainers = { { .id = "reconcile.container", .title = "Reconcile", .location = EViewContainerLocation::Sidebar } },
		.views = {
			{ .id = "reconcile.a", .containerId = "reconcile.container", .title = "A", .order = 10 },
		},
	}).status);
	const auto removableOwner = Owner("layout.reconcile.removable");
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-reconcile-b" }, .owner = removableOwner,
		.views = { { .id = "reconcile.b", .containerId = "reconcile.container", .title = "B", .order = 20 } },
	}).status);
	WorkbenchLayoutStateService state(registry.Snapshot());
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateView({ .operation = { .operationId = "activate-b" }, .viewId = "reconcile.b" }).status);
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded,
		registry.DisposeOwner({ .operation = { .operationId = "dispose-reconcile" }, .owner = removableOwner }).status);
	const auto reconciled = state.Reconcile(registry.Snapshot(), { .operation = { .operationId = "reconcile" } });
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, reconciled.status);
	const auto snapshot = state.Snapshot();
	EXPECT_EQ(snapshot.views.end(), std::find_if(snapshot.views.begin(), snapshot.views.end(), [](const auto& view) { return view.viewId == "reconcile.b"; }));
	EXPECT_EQ("reconcile.a", *Container(snapshot, "reconcile.container").activeViewId);
}

TEST(WorkbenchLayoutStateService, ReconcileFallsBackAfterActiveContainerDisposalWithoutResurrection)
{
	WorkbenchContributionRegistry registry;
	const auto owner = Owner("layout.active.temporary");
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-active-temporary" }, .owner = owner,
		.viewContainers = {
			{ .id = "active.temporary", .title = "Temporary",
				.location = EViewContainerLocation::Sidebar, .order = -100 },
		},
		.views = {
			{ .id = "active.temporary.view", .containerId = "active.temporary",
				.title = "Temporary view" },
		},
	}).status);
	WorkbenchLayoutStateService state(registry.Snapshot());
	ASSERT_EQ("active.temporary", *state.Snapshot().activeContainers.sideBar);
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.DisposeOwner({
		.operation = { .operationId = "dispose-active-temporary" }, .owner = owner,
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), {
			.operation = { .operationId = "reconcile-active-temporary" },
		}).status);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer),
		*state.Snapshot().activeContainers.sideBar);

	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "readd-active-temporary" },
		.owner = Owner("layout.active.temporary", 2),
		.viewContainers = {
			{ .id = "active.temporary", .title = "Temporary",
				.location = EViewContainerLocation::Sidebar, .order = 100 },
		},
		.views = {
			{ .id = "active.temporary.view", .containerId = "active.temporary",
				.title = "Temporary view" },
		},
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), {
			.operation = { .operationId = "reconcile-readd-active-temporary" },
		}).status);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer),
		*state.Snapshot().activeContainers.sideBar);
}

TEST(WorkbenchLayoutStateService, DeferredActiveContainerIntentMaterializesOnRegistration)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	auto persisted = state.Snapshot();
	persisted.containers.push_back({
		.containerId = "deferred.active.container",
		.location = EWorkbenchViewContainerLocation::SideBar,
		.order = -200,
		.visible = true,
		.activeViewId = "deferred.active.view",
	});
	persisted.views.push_back({
		.viewId = "deferred.active.view",
		.containerId = "deferred.active.container",
		.order = 1,
		.visible = true,
	});
	persisted.activeContainers.sideBar = "deferred.active.container";
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded,
		state.HydrateInitialState(persisted).status);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer),
		*state.Snapshot().activeContainers.sideBar);
	EXPECT_EQ("deferred.active.container",
		*state.MementoSnapshot().activeContainers.sideBar);

	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-deferred-active" },
		.owner = Owner("layout.deferred.active"),
		.viewContainers = {
			{ .id = "deferred.active.container", .title = "Deferred",
				.location = EViewContainerLocation::Sidebar },
		},
		.views = {
			{ .id = "deferred.active.view", .containerId = "deferred.active.container",
				.title = "Deferred view" },
		},
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), {
			.operation = { .operationId = "reconcile-deferred-active" },
		}).status);
	EXPECT_EQ("deferred.active.container",
		*state.Snapshot().activeContainers.sideBar);
}

TEST(WorkbenchLayoutStateService, RegisteredViewPlacementWaitsForAnUnknownContainer)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	auto persisted = state.Snapshot();
	persisted.containers.push_back({
		.containerId = "deferred.placement.container",
		.location = EWorkbenchViewContainerLocation::SideBar,
		.order = 200,
		.visible = true,
	});
	auto outline = std::find_if(persisted.views.begin(), persisted.views.end(),
		[](const auto& view) { return view.viewId == ids::view::Outline; });
	ASSERT_NE(persisted.views.end(), outline);
	outline->containerId = "deferred.placement.container";
	outline->order = -50;
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded,
		state.HydrateInitialState(persisted).status);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer),
		View(state.Snapshot(), ids::view::Outline).containerId);
	EXPECT_EQ("deferred.placement.container",
		View(state.MementoSnapshot(), ids::view::Outline).containerId);

	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-placement-container" },
		.owner = Owner("layout.deferred.placement"),
		.viewContainers = {
			{ .id = "deferred.placement.container", .title = "Deferred placement",
				.location = EViewContainerLocation::Sidebar },
		},
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), {
			.operation = { .operationId = "reconcile-placement-container" },
		}).status);
	EXPECT_EQ("deferred.placement.container",
		View(state.Snapshot(), ids::view::Outline).containerId);
	EXPECT_EQ(-50, View(state.Snapshot(), ids::view::Outline).order);
}

TEST(WorkbenchLayoutStateService, DeliversOrderedNotificationsAndIsolatesThrowingListeners)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	std::vector<std::uint64_t> revisions;
	auto throwing = state.Subscribe([](const WorkbenchLayoutChangeBatch&) { throw std::runtime_error("expected"); });
	auto recorder = state.Subscribe([&revisions](const WorkbenchLayoutChangeBatch& batch) { revisions.push_back(batch.revision); });
	ASSERT_TRUE(throwing && recorder);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPartVisibility({ .operation = { .operationId = "show" }, .partId = std::string(ids::part::Panel), .visible = true }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPanelAlignment({ .operation = { .operationId = "align" }, .alignment = EWorkbenchPanelAlignment::Right }).status);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2 }), revisions);
	EXPECT_TRUE(std::is_sorted(revisions.begin(), revisions.end()));
}

TEST(WorkbenchLayoutStateService, QueuesObserverTriggeredMutationWithoutRecursiveDelivery)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	std::vector<std::uint64_t> revisions;
	bool delivering = false;
	bool recursivelyDelivered = false;
	std::optional<WorkbenchLayoutOperationResult> nestedResult;
	auto subscription = state.Subscribe([&](const WorkbenchLayoutChangeBatch& batch) {
		if (delivering) recursivelyDelivered = true;
		delivering = true;
		revisions.push_back(batch.revision);
		if (batch.revision == 1) {
			nestedResult = state.SetPartVisibility({
				.operation = { .operationId = "observer-hide-panel", .expectedRevision = batch.revision },
				.partId = std::string(ids::part::Panel),
				.visible = false,
			});
		}
		delivering = false;
	});
	ASSERT_TRUE(subscription);

	const auto initial = state.SetPartVisibility({
		.operation = { .operationId = "observer-show-panel", .expectedRevision = 0 },
		.partId = std::string(ids::part::Panel),
		.visible = true,
	});

	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, initial.status);
	ASSERT_TRUE(nestedResult.has_value());
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, nestedResult->status);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2 }), revisions);
	EXPECT_FALSE(recursivelyDelivered);
	EXPECT_FALSE(Part(state.Snapshot(), ids::part::Panel).visible);
}

TEST(WorkbenchLayoutStateService, HydratesValidMementoAtomicallyWithoutRevisionOrNotification)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	std::vector<WorkbenchLayoutChangeBatch> notifications;
	auto subscription = state.Subscribe([&](const auto& batch) { notifications.push_back(batch); });
	ASSERT_TRUE(subscription);
	auto persisted = state.Snapshot();
	persisted.parts = { { .partId = std::string(ids::part::Sidebar), .visible = false,
		.position = EWorkbenchPartPosition::Left, .committedExtentDip = 320 } };
	persisted.containers = { { .containerId = "deferred.container", .location = EWorkbenchViewContainerLocation::Panel,
		.order = -7, .visible = true, .activeViewId = "deferred.view" } };
	persisted.views = { { .viewId = "deferred.view", .containerId = "deferred.container", .order = -9, .visible = false } };
	persisted.focus = { .containerId = "deferred.container", .viewId = "deferred.view" };
	persisted.panelAlignment = EWorkbenchPanelAlignment::Right;

	const auto hydrated = state.HydrateInitialState(persisted);
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, hydrated.status);
	EXPECT_EQ(0U, hydrated.snapshot.revision);
	EXPECT_FALSE(Part(hydrated.snapshot, ids::part::Sidebar).visible);
	EXPECT_EQ(320U, *Part(hydrated.snapshot, ids::part::Sidebar).committedExtentDip);
	EXPECT_TRUE(HasContainer(hydrated.snapshot, "deferred.container"));
	EXPECT_TRUE(HasView(hydrated.snapshot, "deferred.view"));
	EXPECT_FALSE(hydrated.snapshot.focus.viewId);
	EXPECT_EQ(EWorkbenchPanelAlignment::Right, hydrated.snapshot.panelAlignment);
	EXPECT_TRUE(notifications.empty());
}

TEST(WorkbenchLayoutStateService, HydrationResolvesActiveViewAfterPersistedViewPlacement)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(Contribute(registry));
	auto persisted = state.Snapshot();
	auto moved = std::find_if(persisted.views.begin(), persisted.views.end(),
		[](const auto& view) { return view.viewId == "sample.movable.view"; });
	auto target = std::find_if(persisted.containers.begin(), persisted.containers.end(),
		[](const auto& container) { return container.containerId == "sample.fixed"; });
	ASSERT_NE(persisted.views.end(), moved);
	ASSERT_NE(persisted.containers.end(), target);
	moved->containerId = "sample.fixed";
	moved->order = -25;
	target->activeViewId = "sample.movable.view";

	const auto hydrated = state.HydrateInitialState(persisted);
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, hydrated.status);
	EXPECT_EQ("sample.fixed", View(hydrated.snapshot, "sample.movable.view").containerId);
	EXPECT_EQ(-25, View(hydrated.snapshot, "sample.movable.view").order);
	ASSERT_TRUE(Container(hydrated.snapshot, "sample.fixed").activeViewId);
	EXPECT_EQ("sample.movable.view", *Container(hydrated.snapshot, "sample.fixed").activeViewId);
}

TEST(WorkbenchLayoutStateService, MementoSnapshotPreservesDeferredFocusAndActiveViewIntent)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	auto persisted = state.Snapshot();
	auto explorer = std::find_if(persisted.containers.begin(), persisted.containers.end(),
		[](const auto& container) { return container.containerId == ids::viewContainer::Explorer; });
	ASSERT_NE(persisted.containers.end(), explorer);
	explorer->activeViewId = "deferred.explorer.view";
	persisted.views.push_back({ .viewId = "deferred.explorer.view",
		.containerId = std::string(ids::viewContainer::Explorer), .order = -41, .visible = true });
	persisted.focus = { .containerId = std::string(ids::viewContainer::Explorer),
		.viewId = "deferred.explorer.view" };
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, state.HydrateInitialState(persisted).status);

	const auto live = state.Snapshot();
	ASSERT_TRUE(Container(live, ids::viewContainer::Explorer).activeViewId);
	EXPECT_NE("deferred.explorer.view", *Container(live, ids::viewContainer::Explorer).activeViewId);
	EXPECT_FALSE(live.focus.viewId);
	const auto memento = state.MementoSnapshot();
	ASSERT_TRUE(Container(memento, ids::viewContainer::Explorer).activeViewId);
	EXPECT_EQ("deferred.explorer.view", *Container(memento, ids::viewContainer::Explorer).activeViewId);
	ASSERT_TRUE(memento.focus.viewId);
	EXPECT_EQ("deferred.explorer.view", *memento.focus.viewId);

	WorkbenchLayoutStateService restored(registry.Snapshot());
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, restored.HydrateInitialState(memento).status);
	const auto roundTripped = restored.MementoSnapshot();
	EXPECT_EQ("deferred.explorer.view", *Container(roundTripped, ids::viewContainer::Explorer).activeViewId);
	EXPECT_EQ("deferred.explorer.view", *roundTripped.focus.viewId);
}

TEST(WorkbenchLayoutStateService, InvalidHydrationLeavesDefaultStateUntouched)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto defaults = state.Snapshot();
	auto invalid = defaults;
	invalid.parts = {
		{ .partId = std::string(ids::part::Sidebar), .visible = false, .position = EWorkbenchPartPosition::Left },
		{ .partId = std::string(ids::part::Sidebar), .visible = true, .position = EWorkbenchPartPosition::Left },
	};
	const auto hydrated = state.HydrateInitialState(invalid);
	EXPECT_EQ(EWorkbenchLayoutHydrationStatus::InvalidSnapshot, hydrated.status);
	const auto after = state.Snapshot();
	EXPECT_EQ(defaults.revision, after.revision);
	EXPECT_EQ(defaults.parts, after.parts);
	EXPECT_EQ(defaults.containers, after.containers);
	EXPECT_EQ(defaults.views, after.views);
	EXPECT_EQ(defaults.panelAlignment, after.panelAlignment);
	EXPECT_EQ(defaults.focus, after.focus);
}

TEST(WorkbenchLayoutStateService, DeferredMementoEntriesSurviveUnrelatedReconcileAndMaterializeOnRegistration)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	auto persisted = state.Snapshot();
	persisted.parts.clear();
	persisted.containers = { { .containerId = "deferred.container", .location = EWorkbenchViewContainerLocation::Panel,
		.order = -7, .visible = false, .activeViewId = "deferred.view" } };
	persisted.views = { { .viewId = "deferred.view", .containerId = "deferred.container", .order = -9, .visible = false } };
	persisted.focus = { .containerId = "deferred.container", .viewId = "deferred.view" };
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, state.HydrateInitialState(persisted).status);

	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-unrelated" }, .owner = Owner("layout.unrelated"),
		.viewContainers = { { .id = "unrelated.container", .title = "Unrelated", .location = EViewContainerLocation::Sidebar } },
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), { .operation = { .operationId = "reconcile-unrelated" } }).status);
	EXPECT_TRUE(HasContainer(state.Snapshot(), "deferred.container"));
	EXPECT_TRUE(HasView(state.Snapshot(), "deferred.view"));

	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-deferred" }, .owner = Owner("layout.deferred"),
		.viewContainers = { { .id = "deferred.container", .title = "Deferred", .location = EViewContainerLocation::Sidebar } },
		.views = { { .id = "deferred.view", .containerId = "deferred.container", .title = "Deferred view", .order = 30 } },
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), { .operation = { .operationId = "reconcile-deferred" } }).status);
	const auto snapshot = state.Snapshot();
	EXPECT_EQ(EWorkbenchViewContainerLocation::Panel, Container(snapshot, "deferred.container").location);
	EXPECT_EQ(-7, Container(snapshot, "deferred.container").order);
	EXPECT_FALSE(Container(snapshot, "deferred.container").visible);
	EXPECT_FALSE(Container(snapshot, "deferred.container").activeViewId);
	EXPECT_EQ(-9, View(snapshot, "deferred.view").order);
	EXPECT_FALSE(View(snapshot, "deferred.view").visible);
	EXPECT_FALSE(snapshot.focus.viewId);
	const auto memento = state.MementoSnapshot();
	EXPECT_EQ("deferred.view", *Container(memento, "deferred.container").activeViewId);
	EXPECT_EQ("deferred.view", *memento.focus.viewId);
}

TEST(WorkbenchLayoutStateService, DisposedRegisteredContributionIsRemovedAndFocusFallsBackWithoutDeferredResurrection)
{
	WorkbenchContributionRegistry registry;
	const auto owner = Owner("layout.temporary");
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "add-temporary" }, .owner = owner,
		.viewContainers = { { .id = "temporary.container", .title = "Temporary", .location = EViewContainerLocation::Panel } },
		.views = { { .id = "temporary.view", .containerId = "temporary.container", .title = "Temporary view" } },
	}).status);
	WorkbenchLayoutStateService state(registry.Snapshot());
	auto persisted = state.Snapshot();
	auto panel = std::find_if(persisted.parts.begin(), persisted.parts.end(),
		[](const auto& part) { return part.partId == ids::part::Panel; });
	ASSERT_NE(persisted.parts.end(), panel);
	panel->visible = true;
	persisted.activeContainers.panel = "temporary.container";
	persisted.focus = { .containerId = "temporary.container", .viewId = "temporary.view" };
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, state.HydrateInitialState(persisted).status);
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.DisposeOwner({
		.operation = { .operationId = "dispose-temporary" }, .owner = owner }).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), { .operation = { .operationId = "reconcile-dispose-temporary" } }).status);
	const auto snapshot = state.Snapshot();
	EXPECT_FALSE(HasContainer(snapshot, "temporary.container"));
	EXPECT_FALSE(HasView(snapshot, "temporary.view"));
	EXPECT_TRUE(snapshot.focus.containerId || snapshot.focus.partId);
	EXPECT_FALSE(snapshot.focus.viewId && *snapshot.focus.viewId == "temporary.view");

	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register({
		.operation = { .operationId = "readd-temporary" }, .owner = Owner("layout.temporary", 2),
		.viewContainers = { { .id = "temporary.container", .title = "Temporary", .location = EViewContainerLocation::Sidebar } },
		.views = { { .id = "temporary.view", .containerId = "temporary.container", .title = "Temporary view", .order = 99 } },
	}).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.Reconcile(registry.Snapshot(), { .operation = { .operationId = "reconcile-readd-temporary" } }).status);
	EXPECT_EQ(EWorkbenchViewContainerLocation::SideBar, Container(state.Snapshot(), "temporary.container").location);
	EXPECT_EQ(99, View(state.Snapshot(), "temporary.view").order);
}

TEST(WorkbenchLayoutStateService, RehydrateAfterSuccessfulInitialHydrationIsExplicitlyRejected)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto persisted = state.Snapshot();
	ASSERT_EQ(EWorkbenchLayoutHydrationStatus::Succeeded, state.HydrateInitialState(persisted).status);
	EXPECT_EQ(EWorkbenchLayoutHydrationStatus::AlreadyHydrated, state.HydrateInitialState(persisted).status);
}

} // namespace
} // namespace workbench::layout
