#include "pch.h"

#include "workbench/activity/ActivityBarEntryProjection.h"
#include "workbench/layout/WorkbenchIds.h"

#include <array>
#include <utility>

namespace workbench::activity {
namespace {

layout::RegisteredWorkbenchViewContainer Container(std::string id,
	layout::EViewContainerLocation location, std::int32_t order)
{
	return { { .id = std::move(id), .title = "Container", .location = location, .order = order } };
}

TEST(ActivityBarEntryProjection, ProjectsTheRequestedSideBarLocation)
{
	layout::WorkbenchContributionSnapshot snapshot;
	snapshot.viewContainers = {
		Container("sidebar.first", layout::EViewContainerLocation::Sidebar, 10),
		Container("auxiliary.first", layout::EViewContainerLocation::AuxiliaryBar, 0),
		Container("sidebar.second", layout::EViewContainerLocation::Sidebar, 0),
	};
	const std::array renderable{ std::string_view("sidebar.first"), std::string_view("auxiliary.first"),
		std::string_view("sidebar.second") };
	ActivityBarProjectionOptions options{ .renderableBuiltins = renderable };

	const auto sidebar = ProjectActivityBarEntries(snapshot, options);
	ASSERT_EQ(2U, sidebar.size());
	EXPECT_EQ("sidebar.second", sidebar[0].id);
	EXPECT_EQ("sidebar.first", sidebar[1].id);

	const auto auxiliary = ProjectActivityBarEntries(snapshot, options,
		layout::EViewContainerLocation::AuxiliaryBar);
	ASSERT_EQ(1U, auxiliary.size());
	EXPECT_EQ("auxiliary.first", auxiliary[0].id);

	const auto panel = ProjectActivityBarEntries(snapshot, options,
		layout::EViewContainerLocation::Panel);
	EXPECT_TRUE(panel.empty());
}

TEST(ActivityBarEntryProjection, OptionsCanSelectTheAuxiliaryLocation)
{
	layout::WorkbenchContributionSnapshot snapshot;
	snapshot.viewContainers = {
		Container("sidebar", layout::EViewContainerLocation::Sidebar, 0),
		Container("auxiliary", layout::EViewContainerLocation::AuxiliaryBar, 0),
	};
	const std::array renderable{ std::string_view("sidebar"), std::string_view("auxiliary") };
	ActivityBarProjectionOptions options{
		.renderableBuiltins = renderable,
		.location = layout::EViewContainerLocation::AuxiliaryBar,
	};

	const auto entries = ProjectActivityBarEntries(snapshot, options);
	ASSERT_EQ(1U, entries.size());
	EXPECT_EQ("auxiliary", entries.front().id);
}

TEST(ActivityBarEntryProjection, ExtensionsUsesTheStableContainerIdAndCodicon)
{
	layout::WorkbenchContributionSnapshot snapshot;
	snapshot.viewContainers = {
		Container(std::string(layout::ids::viewContainer::Extensions),
			layout::EViewContainerLocation::Sidebar, 50),
	};
	const std::array renderable{ layout::ids::viewContainer::Extensions };
	ActivityBarProjectionOptions options{ .renderableBuiltins = renderable };

	const auto entries = ProjectActivityBarEntries(snapshot, options);
	ASSERT_EQ(1U, entries.size());
	EXPECT_EQ(layout::ids::viewContainer::Extensions, entries.front().id);
	EXPECT_EQ(L"extensions", entries.front().codicon);
}

} // namespace
} // namespace workbench::activity
