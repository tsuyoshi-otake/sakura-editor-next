/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/viewcontainer/ViewContainerPageRegistry.h"

namespace workbench::viewcontainer {
namespace {
layout::WorkbenchContributionSnapshot TwoViews(std::string secondProvider = "senp.tree")
{
	layout::WorkbenchContributionRegistry registry;
	const std::array containers{ layout::WorkbenchViewContainerDescriptor{
		.id = "sample.senp", .title = "SENP Sample",
		.location = layout::EViewContainerLocation::Sidebar, .icon = "beaker",
		.supportedLocations = { layout::EViewContainerLocation::Sidebar,
			layout::EViewContainerLocation::AuxiliaryBar } } };
	const std::array views{
		layout::WorkbenchViewDescriptor{ .id = "sample.projects", .containerId = "sample.senp",
			.title = "Projects", .order = 10, .provider = "senp.tree" },
		layout::WorkbenchViewDescriptor{ .id = "sample.states", .containerId = "sample.senp",
			.title = "States", .order = 20, .provider = std::move(secondProvider) },
	};
	EXPECT_TRUE(registry.RegisterExtensionContributions(containers, views));
	auto snapshot = registry.Snapshot();
	std::erase_if(snapshot.viewContainers, [](const auto& entry) {
		return entry.descriptor.id != "sample.senp";
	});
	std::erase_if(snapshot.views, [](const auto& entry) {
		return entry.descriptor.containerId != "sample.senp";
	});
	return snapshot;
}

TEST(HostViewContainerProjection, GroupsMultipleViewsIntoOneNativeContainerFactory)
{
	int factoryCalls{};
	std::string requestedContainer;
	const std::array providers{ HostViewProviderDescriptor{
		.id = "senp.tree",
		.factoryForContainer = [&](std::string_view containerId) {
			++factoryCalls;
			requestedContainer = containerId;
			return ViewContainerPageFactory{ [] { return std::unique_ptr<IViewContainerPage>{}; } };
		},
	} };
	const auto projected = ProjectHostViewPages(TwoViews(), providers);
	ASSERT_EQ(EHostViewPageProjectionStatus::Projected, projected.status);
	ASSERT_EQ(1U, projected.descriptors.size());
	EXPECT_EQ("sample.senp", projected.descriptors[0].containerId);
	EXPECT_EQ("sample.senp", requestedContainer);
	EXPECT_EQ(1, factoryCalls);
	EXPECT_TRUE(projected.descriptors[0].supportedLocations.Contains(
		layout::EViewContainerLocation::AuxiliaryBar));
}

TEST(HostViewContainerProjection, RejectsMixedOrAmbiguousContainerOwnership)
{
	const auto grouped = [](std::string_view) {
		return ViewContainerPageFactory{ [] { return std::unique_ptr<IViewContainerPage>{}; } };
	};
	const std::array mixedProviders{
		HostViewProviderDescriptor{ .id = "senp.tree", .factoryForContainer = grouped },
		HostViewProviderDescriptor{ .id = "foreign.tree", .factoryForContainer = grouped },
	};
	EXPECT_EQ(EHostViewPageProjectionStatus::DuplicateContainerId,
		ProjectHostViewPages(TwoViews("foreign.tree"), mixedProviders).status);
	const std::array ambiguous{ HostViewProviderDescriptor{
		.id = "senp.tree",
		.factory = [] { return std::unique_ptr<IViewContainerPage>{}; },
		.factoryForContainer = grouped,
	} };
	EXPECT_EQ(EHostViewPageProjectionStatus::InvalidProvider,
		ProjectHostViewPages(TwoViews(), ambiguous).status);
}
}
} // namespace workbench::viewcontainer
