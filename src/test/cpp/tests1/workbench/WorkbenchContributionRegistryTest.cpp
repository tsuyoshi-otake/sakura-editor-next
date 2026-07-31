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

namespace workbench::layout {
namespace {

RegisterWorkbenchContributionsRequest Request(std::string operationId, WorkbenchContributionOwner owner,
	std::vector<WorkbenchViewContainerDescriptor> containers = {}, std::vector<WorkbenchViewDescriptor> views = {})
{
	return {
		.operation = { .operationId = std::move(operationId) },
		.owner = std::move(owner),
		.viewContainers = std::move(containers),
		.views = std::move(views),
	};
}

WorkbenchContributionOwner Owner(const char* id, std::uint64_t generation = 1)
{
	return { .ownerId = id, .generation = generation };
}

TEST(WorkbenchContributionRegistry, IncludesCanonicalVSCodePartsAndPanelContributions)
{
	WorkbenchContributionRegistry registry;
	const auto snapshot = registry.Snapshot();

	EXPECT_EQ(1U, snapshot.revision);
	EXPECT_EQ(9U, snapshot.parts.size());
	EXPECT_EQ(11U, snapshot.viewContainers.size());
	EXPECT_EQ(16U, snapshot.views.size());
	const auto containsContainer = [&snapshot](std::string_view id, EViewContainerLocation location) {
		return std::any_of(snapshot.viewContainers.begin(), snapshot.viewContainers.end(), [id, location](const auto& value) {
			return value.descriptor.id == id && value.descriptor.location == location && value.isBuiltin;
		});
	};
	EXPECT_TRUE(containsContainer(ids::viewContainer::Problems, EViewContainerLocation::Panel));
	EXPECT_TRUE(containsContainer(ids::viewContainer::Output, EViewContainerLocation::Panel));
	EXPECT_TRUE(containsContainer(ids::viewContainer::Terminal, EViewContainerLocation::Panel));
	EXPECT_TRUE(containsContainer(ids::viewContainer::Ports, EViewContainerLocation::Panel));
	EXPECT_TRUE(containsContainer(ids::viewContainer::DebugConsole, EViewContainerLocation::Panel));
	EXPECT_TRUE(containsContainer(ids::viewContainer::LegacyExtensionViewsAuxiliary, EViewContainerLocation::AuxiliaryBar));
	EXPECT_TRUE(containsContainer(ids::viewContainer::Search, EViewContainerLocation::Sidebar));
	EXPECT_TRUE(containsContainer(ids::viewContainer::RunAndDebug, EViewContainerLocation::Sidebar));
	EXPECT_TRUE(std::any_of(snapshot.parts.begin(), snapshot.parts.end(), [](const auto& value) {
		return value.descriptor.id == ids::part::Sessions;
	}));
	const auto containsView = [&snapshot](std::string_view id, std::string_view containerId) {
		return std::any_of(snapshot.views.begin(), snapshot.views.end(), [id, containerId](const auto& value) {
			return value.descriptor.id == id && value.descriptor.containerId == containerId;
		});
	};
	EXPECT_TRUE(containsView(ids::view::Outline, ids::viewContainer::Explorer));
	EXPECT_TRUE(containsView(ids::view::Search, ids::viewContainer::Search));
	EXPECT_TRUE(containsView(ids::view::DebugVariables, ids::viewContainer::RunAndDebug));
	EXPECT_TRUE(containsView(ids::view::DebugWatch, ids::viewContainer::RunAndDebug));
	EXPECT_TRUE(containsView(ids::view::DebugCallStack, ids::viewContainer::RunAndDebug));
	EXPECT_TRUE(containsView(ids::view::DebugLoadedScripts, ids::viewContainer::RunAndDebug));
	EXPECT_TRUE(containsView(ids::view::DebugBreakpoints, ids::viewContainer::RunAndDebug));
	EXPECT_TRUE(containsView(ids::view::Extensions, ids::viewContainer::Extensions));
	EXPECT_EQ(std::string("workbench.panel.markers.view"), std::string(ids::view::Problems));
	EXPECT_EQ(std::string("~remote.forwardedPorts"), std::string(ids::view::Ports));
}

TEST(WorkbenchContributionRegistry, ValidatesTheWholeBatchBeforeCommit)
{
	WorkbenchContributionRegistry registry;
	const auto before = registry.Snapshot();
	auto request = Request("bad-parent", Owner("sample.extension"),
		{ { .id = "sample.container", .title = "Container", .location = EViewContainerLocation::Sidebar } },
		{ { .id = "sample.view", .containerId = "missing.container", .title = "View" } });
	const auto result = registry.Register(request);

	EXPECT_EQ(EWorkbenchContributionOperationStatus::Rejected, result.status);
	EXPECT_EQ(EWorkbenchContributionOperationReason::UnknownViewContainer, result.reason);
	const auto after = registry.Snapshot();
	EXPECT_EQ(before.revision, after.revision);
	EXPECT_EQ(before.viewContainers.size(), after.viewContainers.size());
	EXPECT_EQ(before.views.size(), after.views.size());
}

TEST(WorkbenchContributionRegistry, ReplaysExactlyAndRejectsOperationIdReuse)
{
	WorkbenchContributionRegistry registry;
	auto request = Request("register-sample", Owner("sample.extension"),
		{ { .id = "sample.container", .title = "Container", .location = EViewContainerLocation::Sidebar } },
		{ { .id = "sample.view", .containerId = "sample.container", .title = "View" } });
	const auto committed = registry.Register(request);
	const auto replayed = registry.Register(request);
	request.viewContainers[0].canMove = false;
	const auto conflict = registry.Register(request);

	EXPECT_EQ(EWorkbenchContributionOperationStatus::Succeeded, committed.status);
	EXPECT_EQ(EWorkbenchContributionOperationStatus::Replayed, replayed.status);
	EXPECT_EQ(committed.revision, replayed.revision);
	EXPECT_EQ(EWorkbenchContributionOperationStatus::Conflict, conflict.status);
	EXPECT_EQ(EWorkbenchContributionOperationReason::OperationIdConflict, conflict.reason);
}

TEST(WorkbenchContributionRegistry, SortsSnapshotByStableIdRegardlessOfRegistrationOrder)
{
	WorkbenchContributionRegistry registry;
	auto request = Request("order", Owner("sample.extension"), {
		{ .id = "z.container", .title = "Z", .location = EViewContainerLocation::Sidebar },
		{ .id = "a.container", .title = "A", .location = EViewContainerLocation::Sidebar },
	});
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register(request).status);
	const auto snapshot = registry.Snapshot();
	auto first = std::find_if(snapshot.viewContainers.begin(), snapshot.viewContainers.end(), [](const auto& value) { return value.descriptor.id == "a.container"; });
	auto second = std::find_if(snapshot.viewContainers.begin(), snapshot.viewContainers.end(), [](const auto& value) { return value.descriptor.id == "z.container"; });
	ASSERT_NE(snapshot.viewContainers.end(), first);
	ASSERT_NE(snapshot.viewContainers.end(), second);
	EXPECT_LT(static_cast<std::size_t>(std::distance(snapshot.viewContainers.begin(), first)), static_cast<std::size_t>(std::distance(snapshot.viewContainers.begin(), second)));
}

TEST(WorkbenchContributionRegistry, ProtectsBuiltinsAndDisposesAnExactOwnerGeneration)
{
	WorkbenchContributionRegistry registry;
	const auto protectedResult = registry.DisposeOwner({ .operation = { .operationId = "dispose-builtin" }, .owner = { .ownerId = std::string(ids::BuiltinOwner) } });
	EXPECT_EQ(EWorkbenchContributionOperationStatus::Rejected, protectedResult.status);
	EXPECT_EQ(EWorkbenchContributionOperationReason::BuiltinProtected, protectedResult.reason);

	const auto owner = Owner("sample.extension", 7);
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register(Request("add", owner,
		{ { .id = "sample.container", .title = "Container", .location = EViewContainerLocation::Panel } })).status);
	const auto wrongGeneration = registry.DisposeOwner({ .operation = { .operationId = "wrong" }, .owner = Owner("sample.extension", 8) });
	EXPECT_EQ(EWorkbenchContributionOperationStatus::Conflict, wrongGeneration.status);
	EXPECT_EQ(EWorkbenchContributionOperationReason::OwnerGenerationConflict, wrongGeneration.reason);
	EXPECT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.DisposeOwner({ .operation = { .operationId = "dispose" }, .owner = owner }).status);
	const auto snapshot = registry.Snapshot();
	EXPECT_EQ(snapshot.viewContainers.end(), std::find_if(snapshot.viewContainers.begin(), snapshot.viewContainers.end(), [](const auto& value) { return value.descriptor.id == "sample.container"; }));
}

TEST(WorkbenchContributionRegistry, DeliversCommittedRevisionsInOrderAndIsolatesListenerExceptions)
{
	WorkbenchContributionRegistry registry;
	std::vector<std::uint64_t> revisions;
	ASSERT_TRUE(registry.Subscribe(Owner("observer"), [&revisions](const WorkbenchContributionChange& change) { revisions.push_back(change.revision); }));
	ASSERT_TRUE(registry.Subscribe(Owner("throwing-observer"), [](const WorkbenchContributionChange&) { throw std::runtime_error("expected"); }));
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register(Request("one", Owner("owner.one"),
		{ { .id = "one.container", .title = "One", .location = EViewContainerLocation::Sidebar } })).status);
	ASSERT_EQ(EWorkbenchContributionOperationStatus::Succeeded, registry.Register(Request("two", Owner("owner.two"),
		{ { .id = "two.container", .title = "Two", .location = EViewContainerLocation::Sidebar } })).status);

	ASSERT_EQ(2U, revisions.size());
	EXPECT_LT(revisions[0], revisions[1]);
}

} // namespace
} // namespace workbench::layout
