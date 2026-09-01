#include "pch.h"

#include "workbench/activity/ActivityBarEntryProjection.h"
#include "workbench/layout/WorkbenchIds.h"
#include "sakura_rc.h"

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

TEST(ActivityBarEntryProjection, LiveLayoutOverridesContributionOrderAndLocation)
{
	layout::WorkbenchContributionSnapshot snapshot;
	snapshot.viewContainers = {
		Container("projects", layout::EViewContainerLocation::Sidebar, 5),
		Container("explorer", layout::EViewContainerLocation::Sidebar, 10),
		Container("search", layout::EViewContainerLocation::Sidebar, 20),
	};
	layout::WorkbenchLayoutStateSnapshot layoutState;
	layoutState.containers = {
		{ .containerId = "projects", .location = layout::EWorkbenchViewContainerLocation::SideBar, .order = 30 },
		{ .containerId = "explorer", .location = layout::EWorkbenchViewContainerLocation::AuxiliaryBar, .order = 10 },
		{ .containerId = "search", .location = layout::EWorkbenchViewContainerLocation::SideBar, .order = 0 },
	};
	const std::array renderable{
		std::string_view("projects"), std::string_view("explorer"), std::string_view("search") };
	ActivityBarProjectionOptions options{
		.renderableBuiltins = renderable,
		.layoutState = &layoutState,
	};

	const auto primary = ProjectActivityBarEntries(snapshot, options);
	ASSERT_EQ(2U, primary.size());
	EXPECT_EQ("search", primary[0].id);
	EXPECT_EQ("projects", primary[1].id);
	const auto auxiliary = ProjectActivityBarEntries(snapshot, options,
		layout::EViewContainerLocation::AuxiliaryBar);
	ASSERT_EQ(1U, auxiliary.size());
	EXPECT_EQ("explorer", auxiliary[0].id);
}

TEST(ActivityBarEntryProjection, ReordersOnlyVisibleViewContainers)
{
	std::vector<ActivityBarEntry> entries{
		{ .id = "projects", .label = L"Projects" },
		{ .id = "hidden", .label = L"Hidden", .visible = false },
		{ .id = "explorer", .label = L"Explorer" },
		{ .id = std::string(kAccountsActivityId), .label = L"Accounts",
			.kind = ActivityBarEntryKind::GlobalAction },
	};

	const auto moved = ReorderActivityBarContainers(entries, "projects", 2);
	ASSERT_TRUE(moved.has_value());
	EXPECT_EQ((std::vector<std::string>{ "explorer", "projects" }), *moved);
	EXPECT_FALSE(ReorderActivityBarContainers(entries, "missing", 0).has_value());
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
	EXPECT_EQ(STR_WORKBENCH_EXTENSIONS_TITLE,
		ResolveBuiltinActivityTitleResourceId(layout::ids::viewContainer::Extensions));
}

TEST(ActivityBarEntryProjection, ProjectsUsesOneStableContainerEntryAndProjectCodicon)
{
	layout::WorkbenchContributionRegistry registry;
	const std::array containers{ layout::WorkbenchViewContainerDescriptor{
		.id = std::string(layout::ids::viewContainer::Projects),
		.title = "Projects",
		.location = layout::EViewContainerLocation::Sidebar,
		.order = 5,
		.icon = "project",
		.supportedLocations = { layout::EViewContainerLocation::Sidebar,
			layout::EViewContainerLocation::AuxiliaryBar },
	} };
	const std::array views{ layout::WorkbenchViewDescriptor{
		.id = std::string(layout::ids::view::Projects),
		.containerId = std::string(layout::ids::viewContainer::Projects),
		.title = "Projects",
		.order = 10,
		.provider = "sakura.projects",
	} };
	ASSERT_TRUE(registry.RegisterExtensionContributions(containers, views));
	const auto snapshot = registry.Snapshot();
	const std::array renderable{ layout::ids::viewContainer::Projects };
	ActivityBarProjectionOptions options{ .renderableBuiltins = renderable };

	const auto entries = ProjectActivityBarEntries(snapshot, options);
	ASSERT_EQ(1U, entries.size());
	EXPECT_EQ(layout::ids::viewContainer::Projects, entries.front().id);
	EXPECT_EQ(L"project", entries.front().codicon);
	EXPECT_EQ(L"Projects", entries.front().label);
}

TEST(ActivityBarEntryProjection, InjectedResolverLocalizesBuiltinLabelWithoutChangingIdentity)
{
	layout::WorkbenchContributionSnapshot snapshot;
	snapshot.viewContainers = {
		Container(std::string(layout::ids::viewContainer::Extensions),
			layout::EViewContainerLocation::Sidebar, 50),
	};
	const std::array renderable{ layout::ids::viewContainer::Extensions };
	const ActivityBarTitleResolver resolver = [](std::string_view id, std::wstring_view fallback) {
		if (id == layout::ids::viewContainer::Extensions) return std::wstring(L"拡張機能");
		return std::wstring(fallback);
	};
	ActivityBarProjectionOptions options{
		.renderableBuiltins = renderable,
		.titleResolver = resolver,
	};

	const auto entries = ProjectActivityBarEntries(snapshot, options);
	ASSERT_EQ(1U, entries.size());
	EXPECT_EQ(layout::ids::viewContainer::Extensions, entries.front().id);
	EXPECT_EQ(L"拡張機能", entries.front().label);
	EXPECT_EQ(L"extensions", entries.front().codicon);
}

TEST(ActivityBarEntryProjection, InjectedResolverLocalizesGlobalActionsWithStableIdsAndCodicons)
{
	const ActivityBarTitleResolver resolver = [](std::string_view id, std::wstring_view fallback) {
		if (id == kAccountsActivityId) return std::wstring(L"アカウント");
		if (id == kManageActivityId) return std::wstring(L"管理");
		return std::wstring(fallback);
	};
	std::vector<ActivityBarEntry> entries;

	AppendGlobalActivityActions(entries, resolver);
	ASSERT_EQ(2U, entries.size());
	EXPECT_EQ(kAccountsActivityId, entries[0].id);
	EXPECT_EQ(L"アカウント", entries[0].label);
	EXPECT_EQ(L"account", entries[0].codicon);
	EXPECT_EQ(ActivityBarEntryKind::GlobalAction, entries[0].kind);
	EXPECT_EQ(kManageActivityId, entries[1].id);
	EXPECT_EQ(L"管理", entries[1].label);
	EXPECT_EQ(L"settings-gear", entries[1].codicon);
	EXPECT_EQ(ActivityBarEntryKind::GlobalAction, entries[1].kind);
	EXPECT_EQ(STR_WORKBENCH_ACTIVITY_ACCOUNTS,
		ResolveGlobalActivityTitleResourceId(kAccountsActivityId));
	EXPECT_EQ(STR_WORKBENCH_ACTIVITY_MANAGE,
		ResolveActivityTitleResourceId(kManageActivityId));

	// Reprojection must not duplicate the stable GlobalCompositeBar actions.
	AppendGlobalActivityActions(entries, resolver);
	EXPECT_EQ(2U, entries.size());
}

} // namespace
} // namespace workbench::activity
