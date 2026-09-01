#include "pch.h"

#include "workbench/activity/CActivityBar.h"

#include <utility>

namespace workbench::activity {
namespace {

TEST(ActivityBarClickBehavior, OnlyDefaultVerticalPlacementTogglesThePrimarySideBar)
{
	EXPECT_EQ(EActivityBarActiveIconClickBehavior::TogglePrimarySideBar,
		ResolveActivityBarActiveIconClickBehavior(ActivityBarLocation::Default));
	EXPECT_EQ(EActivityBarActiveIconClickBehavior::FocusActiveView,
		ResolveActivityBarActiveIconClickBehavior(ActivityBarLocation::Top));
	EXPECT_EQ(EActivityBarActiveIconClickBehavior::FocusActiveView,
		ResolveActivityBarActiveIconClickBehavior(ActivityBarLocation::Bottom));
	EXPECT_EQ(EActivityBarActiveIconClickBehavior::TogglePrimarySideBar,
		ResolveActivityBarActiveIconClickBehavior(static_cast<ActivityBarLocation>(255)));
}

ActivityBarEntry ViewContainer(std::string id)
{
	return { .id = std::move(id), .label = L"View", .codicon = L"files" };
}

ActivityBarEntry GlobalAction(std::string id)
{
	return {
		.id = std::move(id),
		.label = L"Global",
		.codicon = L"settings-gear",
		.kind = ActivityBarEntryKind::GlobalAction,
	};
}

TEST(ActivityBarModel, VerticalLayoutPinsGlobalActionsToTheBottom)
{
	ActivityBarModel model;
	model.SetEntries({ ViewContainer("one"), ViewContainer("two"), GlobalAction("accounts"), GlobalAction("manage") });
	model.SetViewport(100, 200, 96);

	EXPECT_EQ((ActivityBarRect{ 0, 0, 42, 42 }), model.GetButton(0).bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 42, 42, 84 }), model.GetButton(1).bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 116, 42, 158 }), model.GetButton(2).bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 158, 42, 200 }), model.GetButton(3).bounds);
	EXPECT_EQ("accounts", model.HitTest(20, 120));
}

TEST(ActivityBarModel, HorizontalLayoutUsesNormalOrderForGlobalActions)
{
	ActivityBarModel model(ActivityBarOrientation::Horizontal);
	model.SetEntries({ ViewContainer("one"), ViewContainer("two"), GlobalAction("accounts") });
	model.SetViewport(200, 50, 96);

	EXPECT_EQ((ActivityBarRect{ 4, 0, 30, 35 }), model.GetButton(0).bounds);
	EXPECT_EQ((ActivityBarRect{ 30, 0, 56, 35 }), model.GetButton(1).bounds);
	EXPECT_EQ((ActivityBarRect{ 56, 0, 82, 35 }), model.GetButton(2).bounds);
	EXPECT_EQ("accounts", model.HitTest(60, 20));
	EXPECT_EQ(ActivityBarOrientation::Horizontal, model.GetOrientation());
	EXPECT_EQ(16, model.GetIconSizePixels());
	EXPECT_EQ(26, model.GetItemFootprintPixels());
	EXPECT_EQ(4, model.GetOuterInsetPixels());
	EXPECT_EQ(35, model.GetPreferredHeightPixels());
}

TEST(ActivityBarModel, HorizontalCompactGeometryScalesAtSupportedDpis)
{
	struct Expectation {
		unsigned int dpi;
		int inset;
		int item;
		int height;
		int icon;
	};
	for (const auto expected : {
		Expectation{ 96, 4, 26, 35, 16 },
		Expectation{ 144, 6, 39, 53, 24 },
		Expectation{ 192, 8, 52, 70, 32 },
	}) {
		ActivityBarModel model(ActivityBarOrientation::Horizontal);
		model.SetEntries({ ViewContainer("one"), ViewContainer("two") });
		model.SetViewport(400, 100, expected.dpi);

		EXPECT_EQ((ActivityBarRect{ expected.inset, 0,
			expected.inset + expected.item, expected.height }), model.GetButton(0).bounds);
		EXPECT_EQ((ActivityBarRect{ expected.inset + expected.item, 0,
			expected.inset + 2 * expected.item, expected.height }), model.GetButton(1).bounds);
		EXPECT_EQ(expected.icon, model.GetIconSizePixels());
		EXPECT_EQ(expected.item, model.GetItemFootprintPixels());
		EXPECT_EQ(expected.inset, model.GetOuterInsetPixels());
		EXPECT_EQ(expected.height, model.GetPreferredHeightPixels());
	}
}

TEST(ActivityBarModel, SwitchingOrientationRestoresVerticalMetricsAndReflows)
{
	ActivityBarModel model(ActivityBarOrientation::Horizontal);
	model.SetEntries({ ViewContainer("one"), ViewContainer("two") });
	model.SetViewport(200, 200, 96);
	model.SetOrientation(ActivityBarOrientation::Vertical);

	EXPECT_EQ((ActivityBarRect{ 0, 0, 42, 42 }), model.GetButton(0).bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 42, 42, 84 }), model.GetButton(1).bounds);
	EXPECT_EQ(20, model.GetIconSizePixels());
	EXPECT_EQ(42, model.GetItemFootprintPixels());
	EXPECT_EQ(0, model.GetOuterInsetPixels());
	EXPECT_EQ(42, model.GetPreferredWidthPixels());
}

TEST(ActivityBarModel, KeyboardFocusDirectionIsIndependentOfOrientation)
{
	ActivityBarModel model(ActivityBarOrientation::Horizontal);
	model.SetEntries({ ViewContainer("one"), ViewContainer("two"), ViewContainer("three") });
	model.SetViewport(200, 50, 96);

	EXPECT_EQ("one", model.MoveFocus(1));
	EXPECT_EQ("two", model.MoveFocus(1));
	EXPECT_EQ("one", model.MoveFocus(-1));
	EXPECT_EQ("three", model.MoveFocus(-1));
	EXPECT_EQ("three", model.FocusEdge(-1));
	EXPECT_EQ("one", model.FocusEdge(1));
}

TEST(ActivityBarModel, VerticalDropGapsExcludePinnedGlobalActions)
{
	ActivityBarModel model;
	model.SetEntries({ ViewContainer("one"), ViewContainer("two"), GlobalAction("accounts") });
	model.SetViewport(42, 200, 96);

	EXPECT_EQ(0U, model.ContainerInsertionIndexAt(20, 0));
	EXPECT_EQ(1U, model.ContainerInsertionIndexAt(20, 21));
	EXPECT_EQ(1U, model.ContainerInsertionIndexAt(20, 42));
	EXPECT_EQ(2U, model.ContainerInsertionIndexAt(20, 180));
	EXPECT_FALSE(model.ContainerInsertionIndexAt(-1, 20).has_value());
	EXPECT_FALSE(model.ContainerInsertionIndexAt(20, 200).has_value());
	EXPECT_EQ((ActivityBarRect{ 6, 41, 36, 43 }), model.ContainerInsertionMarker(1));
}

TEST(ActivityBarModel, HorizontalDropGapsUseTheCompactContainerAxis)
{
	ActivityBarModel model(ActivityBarOrientation::Horizontal);
	model.SetEntries({ ViewContainer("one"), ViewContainer("two"), GlobalAction("accounts") });
	model.SetViewport(120, 35, 96);

	EXPECT_EQ(0U, model.ContainerInsertionIndexAt(4, 20));
	EXPECT_EQ(1U, model.ContainerInsertionIndexAt(17, 20));
	EXPECT_EQ(2U, model.ContainerInsertionIndexAt(70, 20));
	EXPECT_EQ((ActivityBarRect{ 29, 6, 31, 29 }), model.ContainerInsertionMarker(1));
}

TEST(ActivityBarDrag, NativeMouseGesturePublishesAContainerGapWithoutInvokingTheEntry)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		100, 100, 100, 200, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	int invokeCount = 0;
	std::string draggedContainer;
	POINT dropPoint{};
	{
		CActivityBar bar([&invokeCount](std::string_view) { ++invokeCount; });
		bar.SetContainerDragCallback([&](const std::string_view containerId, const POINT point) {
			draggedContainer = containerId;
			dropPoint = point;
		});
		ASSERT_TRUE(bar.Create(parent, ::GetModuleHandleW(nullptr)));
		bar.SetEntries({ ViewContainer("one"), ViewContainer("two"), GlobalAction("accounts") });
		bar.Layout({ 0, 0, 42, 160 }, 96);

		(void)::SendMessageW(bar.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(21, 21));
		(void)::SendMessageW(bar.GetHwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(21, 84));
		(void)::SendMessageW(bar.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(21, 84));

		EXPECT_EQ("one", draggedContainer);
		EXPECT_EQ(0, invokeCount);
		ASSERT_TRUE(bar.ContainerInsertionIndexAtScreenPoint(dropPoint).has_value());
		EXPECT_EQ(2U, *bar.ContainerInsertionIndexAtScreenPoint(dropPoint));
	}
	EXPECT_TRUE(::DestroyWindow(parent));
}

} // namespace
} // namespace workbench::activity
