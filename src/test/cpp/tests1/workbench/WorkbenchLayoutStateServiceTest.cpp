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

struct WorkbenchLayoutStateServiceTestAccess {
	static void SetTransactionChangeHook(WorkbenchLayoutStateService& service,
		std::function<bool(std::size_t)> hook)
	{
		service.m_transactionChangeHookForTesting = std::move(hook);
	}
};

namespace {

WorkbenchContributionSnapshot Contribute(WorkbenchContributionRegistry& registry)
{
	auto snapshot = registry.Snapshot();
	snapshot.viewContainers.push_back({ { .id = "sample.movable", .title = "Movable", .location = EViewContainerLocation::Sidebar, .order = 40,
		.supportedLocations = { EViewContainerLocation::Sidebar, EViewContainerLocation::Panel } } });
	snapshot.viewContainers.push_back({ { .id = "sample.fixed", .title = "Fixed", .location = EViewContainerLocation::Panel, .order = 50,
		.supportedLocations = { EViewContainerLocation::Panel } } });
	snapshot.views.push_back({ { .id = "sample.movable.view", .containerId = "sample.movable", .title = "Movable view", .order = 20, .canToggleVisibility = true, .canMove = true } });
	snapshot.views.push_back({ { .id = "sample.fixed.view", .containerId = "sample.fixed", .title = "Fixed view", .order = 10, .canToggleVisibility = false, .canMove = false } });
	return snapshot;
}

const WorkbenchViewContainerDescriptor& ContainerDescriptor(
	const WorkbenchContributionSnapshot& snapshot, std::string_view id)
{
	const auto found = std::find_if(snapshot.viewContainers.begin(), snapshot.viewContainers.end(),
		[id](const auto& value) { return value.descriptor.id == id; });
	EXPECT_NE(snapshot.viewContainers.end(), found);
	return found->descriptor;
}

WorkbenchViewContainerDescriptor& ContainerDescriptor(
	WorkbenchContributionSnapshot& snapshot, std::string_view id)
{
	const auto found = std::find_if(snapshot.viewContainers.begin(), snapshot.viewContainers.end(),
		[id](const auto& value) { return value.descriptor.id == id; });
	EXPECT_NE(snapshot.viewContainers.end(), found);
	return found->descriptor;
}

void ExpectSameSnapshot(const WorkbenchLayoutStateSnapshot& expected,
	const WorkbenchLayoutStateSnapshot& actual)
{
	EXPECT_EQ(expected.schemaVersion, actual.schemaVersion);
	EXPECT_EQ(expected.generation, actual.generation);
	EXPECT_EQ(expected.revision, actual.revision);
	EXPECT_EQ(expected.parts, actual.parts);
	EXPECT_EQ(expected.containers, actual.containers);
	EXPECT_EQ(expected.views, actual.views);
	EXPECT_EQ(expected.activeContainers, actual.activeContainers);
	EXPECT_EQ(expected.panelAlignment, actual.panelAlignment);
	EXPECT_EQ(expected.focus, actual.focus);
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

std::vector<WorkbenchLayoutTransactionChange> AtomicLayoutChanges()
{
	return {
		WorkbenchLayoutSetPartVisibilityChange{
			.partId = std::string(ids::part::Auxiliarybar), .visible = true },
		WorkbenchLayoutMoveContainerChange{
			.containerId = std::string(ids::viewContainer::Explorer),
			.location = EWorkbenchViewContainerLocation::AuxiliaryBar, .order = 72 },
		WorkbenchLayoutActivateViewChange{ .viewId = std::string(ids::view::Outline) },
		WorkbenchLayoutSetFocusChange{ .focus = {
			.partId = std::string(ids::part::Auxiliarybar),
			.containerId = std::string(ids::viewContainer::Explorer),
			.viewId = std::string(ids::view::Outline) } },
	};
}

TEST(WorkbenchLayoutTransaction, CommitsOneRevisionAndOneOrderedCallbackThenReplaysExactly)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	std::vector<WorkbenchLayoutChangeBatch> notifications;
	auto subscription = state.Subscribe(
		[&notifications](const auto& batch) { notifications.push_back(batch); });
	ASSERT_TRUE(subscription);
	const ApplyWorkbenchLayoutTransactionRequest request{
		.operation = { .operationId = "atomic-layout-success", .expectedRevision = 0 },
		.changes = AtomicLayoutChanges(),
	};

	const auto committed = state.ApplyTransaction(request);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, committed.status);
	EXPECT_EQ(1U, committed.revision);
	EXPECT_FALSE(committed.replayed);
	EXPECT_FALSE(committed.failedChangeIndex);
	ASSERT_TRUE(committed.changeBatch);
	EXPECT_EQ(0U, committed.changeBatch->baseRevision);
	EXPECT_EQ(1U, committed.changeBatch->revision);
	ASSERT_EQ(6U, committed.changeBatch->changes.size());
	EXPECT_EQ(EWorkbenchLayoutChangeKind::PartVisibilityChanged,
		committed.changeBatch->changes[0].kind);
	EXPECT_EQ(EWorkbenchLayoutChangeKind::ContainerMoved,
		committed.changeBatch->changes[1].kind);
	EXPECT_EQ(EWorkbenchLayoutChangeKind::ContainerActivated,
		committed.changeBatch->changes[2].kind);
	EXPECT_EQ(EWorkbenchLayoutChangeKind::ContainerActivated,
		committed.changeBatch->changes[3].kind);
	EXPECT_EQ(EWorkbenchLayoutChangeKind::ViewRevealed,
		committed.changeBatch->changes[4].kind);
	EXPECT_EQ(EWorkbenchLayoutChangeKind::FocusChanged,
		committed.changeBatch->changes[5].kind);
	EXPECT_TRUE(Part(committed.snapshot, ids::part::Auxiliarybar).visible);
	EXPECT_EQ(EWorkbenchViewContainerLocation::AuxiliaryBar,
		Container(committed.snapshot, ids::viewContainer::Explorer).location);
	EXPECT_EQ(std::string(ids::view::Outline),
		*Container(committed.snapshot, ids::viewContainer::Explorer).activeViewId);
	EXPECT_EQ(std::string(ids::part::Auxiliarybar), *committed.snapshot.focus.partId);
	ASSERT_EQ(1U, notifications.size());
	EXPECT_EQ(committed.changeBatch->changes.size(), notifications.front().changes.size());

	const auto replayed = state.ApplyTransaction(request);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, replayed.status);
	EXPECT_TRUE(replayed.replayed);
	EXPECT_EQ(1U, replayed.revision);
	EXPECT_EQ(1U, notifications.size());
	ExpectSameSnapshot(committed.snapshot, state.Snapshot());

	auto conflicting = request;
	conflicting.changes.back() = WorkbenchLayoutSetFocusChange{
		.focus = { .partId = std::string(ids::part::Editor) } };
	const auto conflict = state.ApplyTransaction(conflicting);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::OperationIdConflict, conflict.reason);
	EXPECT_EQ(1U, notifications.size());
	ExpectSameSnapshot(committed.snapshot, state.Snapshot());
}

TEST(WorkbenchLayoutTransaction, InjectedFailureAtEveryIndexLeavesNoPartialStateOrIntent)
{
	WorkbenchContributionRegistry registry;
	const auto changes = AtomicLayoutChanges();
	for (std::size_t failureIndex = 0; failureIndex < changes.size(); ++failureIndex) {
		WorkbenchLayoutStateService state(registry.Snapshot());
		const auto before = state.Snapshot();
		const auto mementoBefore = state.MementoSnapshot();
		std::vector<WorkbenchLayoutChangeBatch> notifications;
		auto subscription = state.Subscribe(
			[&notifications](const auto& batch) { notifications.push_back(batch); });
		ASSERT_TRUE(subscription);
		WorkbenchLayoutStateServiceTestAccess::SetTransactionChangeHook(state,
			[failureIndex](const std::size_t index) { return index != failureIndex; });
		const ApplyWorkbenchLayoutTransactionRequest request{
			.operation = { .operationId = "atomic-index-failure", .expectedRevision = 0 },
			.changes = changes,
		};

		const auto failed = state.ApplyTransaction(request);
		EXPECT_EQ(EWorkbenchLayoutOperationStatus::Failed, failed.status);
		EXPECT_EQ(EWorkbenchLayoutOperationReason::InjectedFailure, failed.reason);
		ASSERT_TRUE(failed.failedChangeIndex);
		EXPECT_EQ(failureIndex, *failed.failedChangeIndex);
		EXPECT_FALSE(failed.changeBatch);
		ExpectSameSnapshot(before, failed.snapshot);
		ExpectSameSnapshot(before, state.Snapshot());
		ExpectSameSnapshot(mementoBefore, state.MementoSnapshot());
		EXPECT_TRUE(notifications.empty());

		// The failed attempt consumed neither state nor replay intent.
		WorkbenchLayoutStateServiceTestAccess::SetTransactionChangeHook(state, {});
		const auto retry = state.ApplyTransaction(request);
		EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, retry.status);
		EXPECT_FALSE(retry.replayed);
		EXPECT_EQ(1U, retry.revision);
		EXPECT_EQ(1U, notifications.size());
	}
}

TEST(WorkbenchLayoutTransaction, ThrowingIndexedPathIsTypedAndDoesNotConsumeReplayIntent)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto before = state.Snapshot();
	const auto mementoBefore = state.MementoSnapshot();
	std::vector<WorkbenchLayoutChangeBatch> notifications;
	auto subscription = state.Subscribe(
		[&notifications](const auto& batch) { notifications.push_back(batch); });
	ASSERT_TRUE(subscription);
	WorkbenchLayoutStateServiceTestAccess::SetTransactionChangeHook(state,
		[](const std::size_t index) {
			if (index == 2U) throw std::runtime_error("injected transaction failure");
			return true;
		});
	const ApplyWorkbenchLayoutTransactionRequest request{
		.operation = { .operationId = "atomic-index-throw", .expectedRevision = 0 },
		.changes = AtomicLayoutChanges(),
	};

	const auto failed = state.ApplyTransaction(request);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Failed, failed.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::InternalFailure, failed.reason);
	ASSERT_TRUE(failed.failedChangeIndex);
	EXPECT_EQ(2U, *failed.failedChangeIndex);
	ExpectSameSnapshot(before, state.Snapshot());
	ExpectSameSnapshot(mementoBefore, state.MementoSnapshot());
	EXPECT_TRUE(notifications.empty());

	WorkbenchLayoutStateServiceTestAccess::SetTransactionChangeHook(state, {});
	const auto retry = state.ApplyTransaction(request);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, retry.status);
	EXPECT_FALSE(retry.replayed);
	EXPECT_EQ(1U, notifications.size());
}

TEST(WorkbenchLayoutTransaction, RejectsInvalidUnknownUnsupportedAndStaleSequencesAtomically)
{
	WorkbenchContributionRegistry registry;
	const auto assertRejected = [&](const std::vector<WorkbenchLayoutTransactionChange>& changes,
		const EWorkbenchLayoutOperationStatus expectedStatus,
		const EWorkbenchLayoutOperationReason expectedReason,
		const std::size_t expectedIndex) {
		WorkbenchLayoutStateService state(registry.Snapshot());
		const auto before = state.Snapshot();
		const auto mementoBefore = state.MementoSnapshot();
		std::vector<WorkbenchLayoutChangeBatch> notifications;
		auto subscription = state.Subscribe(
			[&notifications](const auto& batch) { notifications.push_back(batch); });
		EXPECT_TRUE(subscription);
		const auto rejected = state.ApplyTransaction({
			.operation = { .operationId = "atomic-rejected", .expectedRevision = 0 },
			.changes = changes,
		});
		EXPECT_EQ(expectedStatus, rejected.status);
		EXPECT_EQ(expectedReason, rejected.reason);
		ASSERT_TRUE(rejected.failedChangeIndex);
		EXPECT_EQ(expectedIndex, *rejected.failedChangeIndex);
		ExpectSameSnapshot(before, state.Snapshot());
		ExpectSameSnapshot(mementoBefore, state.MementoSnapshot());
		EXPECT_TRUE(notifications.empty());
	};
	const WorkbenchLayoutTransactionChange validPrefix = WorkbenchLayoutSetPartVisibilityChange{
		.partId = std::string(ids::part::Panel), .visible = true };
	assertRejected({ validPrefix, WorkbenchLayoutSetPartVisibilityChange{
		.partId = std::string("bad\0part", 8), .visible = true } },
		EWorkbenchLayoutOperationStatus::Invalid,
		EWorkbenchLayoutOperationReason::InvalidRequest, 1U);
	assertRejected({ validPrefix, WorkbenchLayoutActivateContainerChange{
		.containerId = "unknown.container" } },
		EWorkbenchLayoutOperationStatus::UnknownId,
		EWorkbenchLayoutOperationReason::UnknownContainer, 1U);
	assertRejected({ validPrefix, WorkbenchLayoutMoveContainerChange{
		.containerId = std::string(ids::viewContainer::Explorer),
		.location = EWorkbenchViewContainerLocation::Panel, .order = 80 } },
		EWorkbenchLayoutOperationStatus::Unsupported,
		EWorkbenchLayoutOperationReason::CapabilityNotSupported, 1U);

	WorkbenchLayoutStateService staleState(registry.Snapshot());
	const auto before = staleState.Snapshot();
	const auto stale = staleState.ApplyTransaction({
		.operation = { .operationId = "atomic-stale", .expectedRevision = 9 },
		.changes = AtomicLayoutChanges(),
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Conflict, stale.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::RevisionConflict, stale.reason);
	EXPECT_FALSE(stale.failedChangeIndex);
	ExpectSameSnapshot(before, staleState.Snapshot());
	const auto corrected = staleState.ApplyTransaction({
		.operation = { .operationId = "atomic-stale", .expectedRevision = 0 },
		.changes = AtomicLayoutChanges(),
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, corrected.status);
	EXPECT_FALSE(corrected.replayed);
}

TEST(WorkbenchLayoutTransaction, RejectsEmptyAndOversizedInputAndKeepsReplayBounded)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot(), 1, 1);
	const auto before = state.Snapshot();
	const auto empty = state.ApplyTransaction({
		.operation = { .operationId = "atomic-empty", .expectedRevision = 0 },
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, empty.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::InvalidRequest, empty.reason);
	ExpectSameSnapshot(before, state.Snapshot());

	std::vector<WorkbenchLayoutTransactionChange> oversized(
		kMaxWorkbenchLayoutTransactionChanges + 1U,
		WorkbenchLayoutSetPartVisibilityChange{
			.partId = std::string(ids::part::Panel), .visible = true });
	const auto tooLarge = state.ApplyTransaction({
		.operation = { .operationId = "atomic-oversized", .expectedRevision = 0 },
		.changes = std::move(oversized),
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Invalid, tooLarge.status);
	ExpectSameSnapshot(before, state.Snapshot());

	const ApplyWorkbenchLayoutTransactionRequest first{
		.operation = { .operationId = "atomic-replay-first", .expectedRevision = 0 },
		.changes = { WorkbenchLayoutSetPartVisibilityChange{
			.partId = std::string(ids::part::Panel), .visible = true } },
	};
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, state.ApplyTransaction(first).status);
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ApplyTransaction({
			.operation = { .operationId = "atomic-replay-second", .expectedRevision = 1 },
			.changes = { WorkbenchLayoutSetPartVisibilityChange{
				.partId = std::string(ids::part::Panel), .visible = false } },
		}).status);
	const auto evicted = state.ApplyTransaction(first);
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Conflict, evicted.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::RevisionConflict, evicted.reason);
	EXPECT_FALSE(evicted.replayed);
	EXPECT_EQ(2U, state.Snapshot().revision);
}

TEST(WorkbenchContributionRegistry, BuiltinsDeclareOnlyValidatedSupportedLocations)
{
	WorkbenchContributionRegistry registry;
	const auto base = registry.Snapshot();
	EXPECT_EQ(base.viewContainers.end(), std::ranges::find_if(base.viewContainers, [](const auto& entry) {
		return entry.descriptor.id == ids::viewContainer::Projects;
	}));
	const std::array containers{ WorkbenchViewContainerDescriptor{
		.id = std::string(ids::viewContainer::Projects),
		.title = "Projects",
		.location = EViewContainerLocation::Sidebar,
		.order = 5,
		.icon = "project",
		.supportedLocations = { EViewContainerLocation::Sidebar,
			EViewContainerLocation::AuxiliaryBar },
	} };
	const std::array views{ WorkbenchViewDescriptor{
		.id = std::string(ids::view::Projects),
		.containerId = std::string(ids::viewContainer::Projects),
		.title = "Projects",
		.order = 10,
		.provider = "sakura.projects",
	} };
	ASSERT_TRUE(registry.RegisterExtensionContributions(containers, views));
	EXPECT_FALSE(registry.RegisterExtensionContributions(containers, views));
	const auto snapshot = registry.Snapshot();
	ASSERT_TRUE(WorkbenchContributionRegistry::IsValidContributionSnapshot(snapshot));
	EXPECT_EQ(2U, snapshot.revision);

	const auto& explorer = ContainerDescriptor(snapshot, ids::viewContainer::Explorer);
	EXPECT_TRUE(explorer.supportedLocations.Contains(EViewContainerLocation::Sidebar));
	EXPECT_TRUE(explorer.supportedLocations.Contains(EViewContainerLocation::AuxiliaryBar));
	EXPECT_FALSE(explorer.supportedLocations.Contains(EViewContainerLocation::Panel));

	const auto& agent = ContainerDescriptor(snapshot, ids::viewContainer::Projects);
	EXPECT_EQ(EViewContainerLocation::Sidebar, agent.location);
	EXPECT_TRUE(agent.supportedLocations.Contains(EViewContainerLocation::Sidebar));
	EXPECT_TRUE(agent.supportedLocations.Contains(EViewContainerLocation::AuxiliaryBar));
	EXPECT_FALSE(agent.supportedLocations.Contains(EViewContainerLocation::Panel));
	const auto worktree = std::ranges::find_if(snapshot.views, [](const auto& entry) {
		return entry.descriptor.id == ids::view::Projects;
	});
	ASSERT_NE(snapshot.views.end(), worktree);
	EXPECT_EQ(ids::viewContainer::Projects, worktree->descriptor.containerId);
	EXPECT_EQ("sakura.projects", worktree->descriptor.provider);
	EXPECT_EQ("project", agent.icon);

	const auto& problems = ContainerDescriptor(snapshot, ids::viewContainer::Problems);
	EXPECT_TRUE(problems.supportedLocations.Contains(EViewContainerLocation::Panel));
	EXPECT_FALSE(problems.supportedLocations.Contains(EViewContainerLocation::Sidebar));
	EXPECT_FALSE(problems.supportedLocations.Contains(EViewContainerLocation::AuxiliaryBar));
}

TEST(WorkbenchContributionRegistry, RejectsEmptyInvalidAndPartialDescriptorBatches)
{
	WorkbenchContributionRegistry registry;
	const auto original = registry.Snapshot();

	auto empty = original;
	auto& emptyDescriptor = ContainerDescriptor(empty, ids::viewContainer::Explorer);
	emptyDescriptor.supportedLocations = {};
	EXPECT_FALSE(WorkbenchContributionRegistry::IsValidContributionSnapshot(empty));

	auto invalid = original;
	auto& invalidDescriptor = ContainerDescriptor(invalid, ids::viewContainer::Explorer);
	invalidDescriptor.supportedLocations = { EViewContainerLocation::Sidebar,
		static_cast<EViewContainerLocation>(255) };
	EXPECT_FALSE(WorkbenchContributionRegistry::IsValidContributionSnapshot(invalid));

	auto missingDefault = original;
	auto& missingDefaultDescriptor = ContainerDescriptor(missingDefault, ids::viewContainer::Explorer);
	missingDefaultDescriptor.supportedLocations = { EViewContainerLocation::AuxiliaryBar };
	EXPECT_FALSE(WorkbenchContributionRegistry::IsValidContributionSnapshot(missingDefault));

	auto invalidDefault = original;
	auto& invalidDefaultDescriptor = ContainerDescriptor(invalidDefault, ids::viewContainer::Explorer);
	invalidDefaultDescriptor.location = static_cast<EViewContainerLocation>(255);
	EXPECT_FALSE(WorkbenchContributionRegistry::IsValidContributionSnapshot(invalidDefault));

	auto duplicate = original;
	duplicate.viewContainers.push_back(duplicate.viewContainers.front());
	EXPECT_FALSE(WorkbenchContributionRegistry::IsValidContributionSnapshot(duplicate));

	const auto unchanged = registry.Snapshot();
	EXPECT_EQ(original.parts.size(), unchanged.parts.size());
	EXPECT_EQ(original.viewContainers.size(), unchanged.viewContainers.size());
	EXPECT_EQ(original.views.size(), unchanged.views.size());
	EXPECT_EQ(ContainerDescriptor(original, ids::viewContainer::Explorer).id,
		ContainerDescriptor(unchanged, ids::viewContainer::Explorer).id);
}

TEST(WorkbenchContributionRegistry, InvalidReconcileBatchCannotPartiallyRegister)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto before = state.Snapshot();
	const auto mementoBefore = state.MementoSnapshot();
	std::vector<WorkbenchLayoutChangeBatch> notifications;
	auto subscription = state.Subscribe(
		[&notifications](const auto& batch) { notifications.push_back(batch); });
	ASSERT_TRUE(subscription);

	auto invalid = registry.Snapshot();
	ContainerDescriptor(invalid, ids::viewContainer::Explorer).supportedLocations = {};
	const auto result = state.Reconcile(invalid, {
		.operation = { .operationId = "reject-invalid-contribution-batch" },
	});

	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Failed, result.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::InternalFailure, result.reason);
	ExpectSameSnapshot(before, state.Snapshot());
	ExpectSameSnapshot(mementoBefore, state.MementoSnapshot());
	EXPECT_TRUE(notifications.empty());
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
	// The Auxiliary Bar Part exists as a physical right-side Part with no built-in
	// ViewContainer, exactly like VS Code's empty Secondary Side Bar. Outline stays an
	// Explorer view rather than the legacy tool's unrelated "right edge" alias.
	EXPECT_FALSE(std::any_of(snapshot.containers.begin(), snapshot.containers.end(),
		[](const auto& value) { return value.location == EWorkbenchViewContainerLocation::AuxiliaryBar; }));
	EXPECT_FALSE(snapshot.activeContainers.auxiliaryBar.has_value());
	EXPECT_EQ(std::string(ids::view::Explorer), *Container(snapshot, ids::viewContainer::Explorer).activeViewId);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer), *snapshot.activeContainers.sideBar);
	EXPECT_EQ(std::string(ids::viewContainer::Problems), *snapshot.activeContainers.panel);
	EXPECT_TRUE(HasView(snapshot, ids::view::Outline));
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

TEST(WorkbenchLayoutStateService, ActiveSideBarContainerIgnoresTheNestedViewSelection)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());

	// VS Code's Activity Bar click compares the clicked ViewContainer with
	// `getActivePaneComposite()`, never with the active View, so selecting Outline inside the
	// Explorer container must keep Explorer the active Primary Side Bar container. A view-level
	// comparison here would make the Explorer icon fail to collapse the Part.
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.ActivateView({ .operation = { .operationId = "select-outline" },
			.viewId = std::string(ids::view::Outline) }).status);
	const auto outlineActive = state.Snapshot();
	EXPECT_EQ(std::string(ids::view::Outline),
		*Container(outlineActive, ids::viewContainer::Explorer).activeViewId);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer), *outlineActive.activeContainers.sideBar);
	EXPECT_TRUE(Container(outlineActive, ids::viewContainer::Explorer).visible);
	EXPECT_EQ(EWorkbenchViewContainerLocation::SideBar,
		Container(outlineActive, ids::viewContainer::Explorer).location);
	EXPECT_TRUE(Part(outlineActive, ids::part::Sidebar).visible);

	// Hiding the Part is the whole effect of the toggle gesture: the container selection
	// survives so the next click can restore exactly the same Primary Side Bar content.
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded,
		state.SetPartVisibility({ .operation = { .operationId = "collapse-sidebar" },
			.partId = std::string(ids::part::Sidebar), .visible = false }).status);
	const auto collapsed = state.Snapshot();
	EXPECT_FALSE(Part(collapsed, ids::part::Sidebar).visible);
	EXPECT_EQ(std::string(ids::viewContainer::Explorer), *collapsed.activeContainers.sideBar);
	EXPECT_EQ(std::string(ids::view::Outline),
		*Container(collapsed, ids::viewContainer::Explorer).activeViewId);
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

TEST(WorkbenchLayoutStateService, UnsupportedContainerLocationPreservesAllIntentBeforeAllowedMove)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto before = state.Snapshot();
	const auto mementoBefore = state.MementoSnapshot();
	std::vector<WorkbenchLayoutChangeBatch> notifications;
	auto subscription = state.Subscribe(
		[&notifications](const auto& batch) { notifications.push_back(batch); });
	ASSERT_TRUE(subscription);

	const auto unsupported = state.MoveContainer({
		.operation = { .operationId = "move-explorer-supported-contract", .expectedRevision = 0 },
		.containerId = std::string(ids::viewContainer::Explorer),
		.location = EWorkbenchViewContainerLocation::Panel,
		.order = 71,
	});
	EXPECT_EQ(EWorkbenchLayoutOperationStatus::Unsupported, unsupported.status);
	EXPECT_EQ(EWorkbenchLayoutOperationReason::CapabilityNotSupported, unsupported.reason);
	EXPECT_FALSE(unsupported.replayed);
	EXPECT_FALSE(unsupported.changeBatch.has_value());
	ExpectSameSnapshot(before, unsupported.snapshot);
	ExpectSameSnapshot(before, state.Snapshot());
	ExpectSameSnapshot(mementoBefore, state.MementoSnapshot());
	EXPECT_TRUE(notifications.empty());

	// Reusing the exact operation ID with a supported destination proves that the
	// rejected destination did not consume replay intent.
	const auto allowed = state.MoveContainer({
		.operation = { .operationId = "move-explorer-supported-contract", .expectedRevision = 0 },
		.containerId = std::string(ids::viewContainer::Explorer),
		.location = EWorkbenchViewContainerLocation::AuxiliaryBar,
		.order = 72,
	});
	ASSERT_EQ(EWorkbenchLayoutOperationStatus::Succeeded, allowed.status);
	EXPECT_FALSE(allowed.replayed);
	EXPECT_EQ(1U, allowed.revision);
	ASSERT_TRUE(allowed.changeBatch.has_value());
	EXPECT_EQ(0U, allowed.changeBatch->baseRevision);
	EXPECT_EQ(1U, allowed.changeBatch->revision);
	ASSERT_FALSE(allowed.changeBatch->changes.empty());
	EXPECT_EQ(EWorkbenchLayoutChangeKind::ContainerMoved,
		allowed.changeBatch->changes.front().kind);
	EXPECT_EQ(EWorkbenchViewContainerLocation::AuxiliaryBar,
		Container(allowed.snapshot, ids::viewContainer::Explorer).location);
	ASSERT_EQ(1U, notifications.size());
	EXPECT_EQ(0U, notifications.front().baseRevision);
	EXPECT_EQ(1U, notifications.front().revision);
	EXPECT_EQ(allowed.changeBatch->changes.size(), notifications.front().changes.size());
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

TEST(WorkbenchLayoutStateService, HydrationRejectsUnsupportedRegisteredLocationAtomically)
{
	WorkbenchContributionRegistry registry;
	WorkbenchLayoutStateService state(registry.Snapshot());
	const auto before = state.Snapshot();
	const auto mementoBefore = state.MementoSnapshot();
	std::vector<WorkbenchLayoutChangeBatch> notifications;
	auto subscription = state.Subscribe(
		[&notifications](const auto& batch) { notifications.push_back(batch); });
	ASSERT_TRUE(subscription);

	auto persisted = before;
	auto explorer = std::find_if(persisted.containers.begin(), persisted.containers.end(),
		[](const auto& container) { return container.containerId == ids::viewContainer::Explorer; });
	ASSERT_NE(persisted.containers.end(), explorer);
	explorer->location = EWorkbenchViewContainerLocation::Panel;
	explorer->order = -91;

	const auto hydrated = state.HydrateInitialState(persisted);
	EXPECT_EQ(EWorkbenchLayoutHydrationStatus::InvalidSnapshot, hydrated.status);
	ExpectSameSnapshot(before, hydrated.snapshot);
	ExpectSameSnapshot(before, state.Snapshot());
	ExpectSameSnapshot(mementoBefore, state.MementoSnapshot());
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
