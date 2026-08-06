/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "workbench/activity/ActivityBarEntryProjection.h"
#include "workbench/activity/ActivityBarModel.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/layout/WorkbenchIds.h"

namespace workbench::activity {
namespace {

namespace ids = workbench::layout::ids::viewContainer;

//! The three built-in containers the native side bar can render, in registry order.
constexpr std::array kRenderable{
	std::string_view(ids::Explorer),
	std::string_view(ids::SourceControl),
	std::string_view(ids::Extensions),
};

[[nodiscard]] ActivityBarEntry MakeEntry(std::string_view id, std::wstring_view label, bool builtin = true)
{
	return { .id = std::string(id), .label = std::wstring(label),
		.codicon = std::wstring(BuiltinContainerCodicon(id)), .builtin = builtin };
}

[[nodiscard]] std::vector<ActivityBarEntry> BuiltinEntries()
{
	return {
		MakeEntry(ids::Explorer, L"Explorer"),
		MakeEntry(ids::SourceControl, L"Source Control"),
		MakeEntry(ids::Extensions, L"Extensions"),
	};
}

TEST(ActivityBarModel, Uses42DipWidthAndSquareVerticalButtonsAtDpi)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetViewport(500, 400, 144);

	EXPECT_EQ(63, model.GetPreferredWidthPixels());
	ASSERT_EQ(3U, model.GetButtonCount());
	const auto explorer = model.GetButton(0);
	const auto sourceControl = model.GetButton(1);
	const auto extensions = model.GetButton(2);
	EXPECT_EQ((ActivityBarRect{ 0, 0, 63, 63 }), explorer.bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 63, 63, 126 }), sourceControl.bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 126, 63, 189 }), extensions.bounds);
	EXPECT_EQ(ids::Explorer, model.HitTest(62, 62));
	EXPECT_EQ(ids::SourceControl, model.HitTest(10, 100));
	EXPECT_EQ(ids::Extensions, model.HitTest(10, 150));
	EXPECT_TRUE(model.HitTest(10, 210).empty());
	EXPECT_TRUE(model.HitTest(63, 10).empty());
}

TEST(ActivityBarModel, MapsEveryRenderedContainerToItsBundledCodicon)
{
	for (const auto id : kRenderable) {
		const auto name = BuiltinContainerCodicon(id);
		EXPECT_FALSE(name.empty()) << id;
		EXPECT_TRUE(workbench::icons::FindCodiconGlyph(name).has_value()) << id;
	}
	EXPECT_TRUE(BuiltinContainerCodicon("claude-code").empty());
}

TEST(ActivityBarModel, ExposesIndependentVisualStateForProviders)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetViewport(42, 200);
	model.SetSelectedItem(ids::SourceControl);
	model.SetHoveredItem(ids::Explorer);
	model.SetPressedItem(ids::Explorer);
	model.SetFocusedItem(ids::Explorer);

	const auto explorer = model.GetButton(0);
	const auto sourceControl = model.GetButton(1);
	EXPECT_TRUE(explorer.hovered);
	EXPECT_TRUE(explorer.pressed);
	EXPECT_TRUE(explorer.focused);
	EXPECT_TRUE(sourceControl.selected);
	EXPECT_EQ(ids::Explorer, model.GetFocusedItem());
}

TEST(ActivityBarModel, FocusNavigationSkipsDisabledItemsAndWraps)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetViewport(42, 200);
	model.SetItemEnabled(ids::SourceControl, false);
	model.SetFocusedItem(ids::Explorer);

	EXPECT_EQ(ids::Extensions, model.MoveFocus(1));
	EXPECT_EQ(ids::Explorer, model.MoveFocus(-1));
	EXPECT_FALSE(model.GetButton(1).enabled);
	EXPECT_TRUE(model.HitTest(10, 60).empty());
	EXPECT_EQ(ids::Extensions, model.HitTest(10, 100));
}

TEST(ActivityBarModel, InvokeOnlyReturnsEnabledRequestedItemAndDoesNotChangeSelection)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetViewport(42, 200);
	model.SetSelectedItem(ids::Explorer);
	model.SetFocusedItem(ids::SourceControl);

	EXPECT_EQ(ids::SourceControl, model.InvokeFocused());
	EXPECT_EQ(ids::Explorer, model.GetSelectedItem());
	model.SetItemEnabled(ids::SourceControl, false);
	EXPECT_TRUE(model.Invoke(ids::SourceControl).empty());
	EXPECT_TRUE(model.InvokeFocused().empty());
}

TEST(ActivityBarModel, ClampsShortClientsWithoutInvertedButtonBounds)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetViewport(-1, 20, 192);

	EXPECT_EQ(84, model.GetPreferredWidthPixels());
	for (std::size_t index = 0; index < model.GetButtonCount(); ++index) {
		const auto bounds = model.GetButton(index).bounds;
		EXPECT_GE(bounds.left, 0);
		EXPECT_GE(bounds.top, 0);
		EXPECT_GE(bounds.right, bounds.left);
		EXPECT_GE(bounds.bottom, bounds.top);
		EXPECT_LE(bounds.bottom, 20);
	}
}

TEST(ActivityBarModel, KeepsSelectionByContainerIdWhenAnExtensionInsertsAnEntryAbove)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetViewport(42, 400);
	model.SetSelectedItem(ids::Extensions);
	ASSERT_EQ(2U, model.IndexOf(ids::Extensions));

	auto grown = BuiltinEntries();
	grown.insert(grown.begin(), MakeEntry("claude-code", L"Claude Code", false));
	model.SetEntries(std::move(grown));

	// Position moved, identity did not: the user's selected container must not change
	// because an extension registered a container that sorts above it.
	EXPECT_EQ(3U, model.IndexOf(ids::Extensions));
	EXPECT_EQ(ids::Extensions, model.GetSelectedItem());
	EXPECT_TRUE(model.GetButton(3).selected);
}

TEST(ActivityBarModel, DropsSelectionWhenItsContainerDisappears)
{
	ActivityBarModel model;
	model.SetEntries(BuiltinEntries());
	model.SetSelectedItem(ids::SourceControl);
	model.SetFocusedItem(ids::SourceControl);

	model.SetEntries({ MakeEntry(ids::Explorer, L"Explorer") });

	EXPECT_TRUE(model.GetSelectedItem().empty());
	EXPECT_TRUE(model.GetFocusedItem().empty());
	EXPECT_EQ(ids::Explorer, model.FocusEdge(1));
	EXPECT_EQ(ids::Explorer, model.FocusEdge(-1));
}

//! Registers one contributed side-bar container, the way the extension bridge does.
void ContributeContainer(layout::WorkbenchContributionRegistry& registry, std::string_view ownerId,
	std::string_view containerId, std::string_view title, std::int32_t order)
{
	const auto result = registry.Register({
		.operation = { .operationId = std::string(containerId) + ".register" },
		.owner = { .ownerId = std::string(ownerId), .generation = 1 },
		.viewContainers = { {
			.id = std::string(containerId),
			.title = std::string(title),
			.location = layout::EViewContainerLocation::Sidebar,
			.order = order,
		} },
	});
	ASSERT_EQ(layout::EWorkbenchContributionOperationStatus::Succeeded, result.status);
}

TEST(ActivityBarEntryProjection, RendersContributedContainersAndSkipsUnrenderableBuiltins)
{
	layout::WorkbenchContributionRegistry registry;
	ASSERT_NO_FATAL_FAILURE(
		ContributeContainer(registry, "Anthropic.claude-code", "claude-code", "Claude Code", 100));

	const auto entries = ProjectActivityBarEntries(registry.Snapshot(), {
		.renderableBuiltins = kRenderable,
		.extensionCodicon = [](std::string_view) -> std::wstring { return {}; },
	});

	// Search and Run and Debug are declared but have no native page, so projecting them
	// would put buttons on the strip that open nothing.
	ASSERT_EQ(4U, entries.size());
	EXPECT_EQ(ids::Explorer, entries[0].id);
	EXPECT_EQ(ids::SourceControl, entries[1].id);
	EXPECT_EQ(ids::Extensions, entries[2].id);
	EXPECT_EQ("claude-code", entries[3].id);
	EXPECT_EQ(L"Claude Code", entries[3].label);
	EXPECT_FALSE(entries[3].builtin);
	// Claude Code ships an image, not a codicon; the control falls back to an initial tile.
	EXPECT_TRUE(entries[3].codicon.empty());
	EXPECT_EQ(L"files", entries[0].codicon);
}

TEST(ActivityBarEntryProjection, TakesTheDeclaredCodiconForAContributedContainer)
{
	layout::WorkbenchContributionRegistry registry;
	ASSERT_NO_FATAL_FAILURE(ContributeContainer(registry, "acme.tools", "acme-tools", "Acme Tools", 5));

	const auto entries = ProjectActivityBarEntries(registry.Snapshot(), {
		.renderableBuiltins = kRenderable,
		.extensionCodicon = [](std::string_view containerId) -> std::wstring {
			return containerId == "acme-tools" ? L"beaker" : std::wstring{};
		},
	});

	ASSERT_FALSE(entries.empty());
	// order 5 は Explorer の 10 より小さいが、寄与コンテナは常に組み込みの下。
	// VS Code も同じで、拡張が order で組み込みの上へ割り込むことはできない。
	EXPECT_EQ("acme-tools", entries.back().id);
	EXPECT_EQ(L"beaker", entries.back().codicon);
}

/*!
	A manifest has no `order` for a ViewContainer, so every contributed container arrives with
	the default 0 while the built-ins carry 10..50. Sorting on `order` alone therefore puts
	every extension icon above Explorer, which is not where VS Code puts them.
*/
TEST(ActivityBarEntryProjection, PlacesAContributedContainerBelowTheBuiltinsDespiteItsDefaultOrder)
{
	layout::WorkbenchContributionRegistry registry;
	ASSERT_NO_FATAL_FAILURE(
		ContributeContainer(registry, "Anthropic.claude-code", "claude-code", "Claude Code", 0));
	ASSERT_NO_FATAL_FAILURE(ContributeContainer(registry, "acme.tools", "acme-tools", "Acme Tools", 0));

	const auto entries = ProjectActivityBarEntries(registry.Snapshot(), {
		.renderableBuiltins = kRenderable,
		.extensionCodicon = [](std::string_view) -> std::wstring { return {}; },
	});

	ASSERT_EQ(5U, entries.size());
	EXPECT_EQ(ids::Explorer, entries[0].id);
	EXPECT_EQ(ids::SourceControl, entries[1].id);
	EXPECT_EQ(ids::Extensions, entries[2].id);
	// 同順位の寄与コンテナ同士は ID で安定させる。
	EXPECT_EQ("acme-tools", entries[3].id);
	EXPECT_EQ("claude-code", entries[4].id);
}

//! Secondary Side Bar のコンテナはアクティビティバーに出ない。VS Code と同じで、
//! コンテナの場所は 1 つだけであり、左端は Primary Side Bar の投影だから。
TEST(ActivityBarEntryProjection, SkipsAContributedContainerThatLivesOutsideThePrimarySideBar)
{
	layout::WorkbenchContributionRegistry registry;
	const auto result = registry.Register({
		.operation = { .operationId = "claude-code-secondary.register" },
		.owner = { .ownerId = "Anthropic.claude-code", .generation = 1 },
		.viewContainers = { {
			.id = "claude-code-secondary",
			.title = "Claude Code",
			.location = layout::EViewContainerLocation::AuxiliaryBar,
		} },
	});
	ASSERT_EQ(layout::EWorkbenchContributionOperationStatus::Succeeded, result.status);

	const auto entries = ProjectActivityBarEntries(registry.Snapshot(), {
		.renderableBuiltins = kRenderable,
		.extensionCodicon = [](std::string_view) -> std::wstring { return {}; },
	});

	EXPECT_TRUE(std::ranges::none_of(entries,
		[](const auto& entry) { return entry.id == "claude-code-secondary"; }));
	EXPECT_EQ(3U, entries.size());
}

TEST(IconMetrics, UsesSharedOpticalSizesAndDpiStableBounds)
{
	using namespace workbench::icons;
	EXPECT_EQ(20, ScaleDip(kActivityIconDip, 96));
	EXPECT_EQ(30, ScaleDip(kActivityIconDip, 144));
	EXPECT_EQ(16, ScaleDip(kStatusIconDip, 96));
	EXPECT_EQ(24, ScaleDip(kStatusIconDip, 144));
	EXPECT_EQ(1, LineStrokePixels(96));
	EXPECT_EQ(2, LineStrokePixels(192));
	EXPECT_EQ(24, StatusTextInsetPixels(96));
	EXPECT_EQ(8, StatusItemHorizontalPaddingPixels(96));
	EXPECT_EQ(12, StatusItemHorizontalPaddingPixels(144));
	EXPECT_EQ(8, StatusItemPartWidthPaddingPixels(96));
	EXPECT_EQ(12, StatusItemPartWidthPaddingPixels(144));
	EXPECT_EQ((IconRect{ 11, 11, 31, 31 }), CenteredIconBounds({ 0, 0, 42, 42 }, kActivityIconDip, 96));
	EXPECT_EQ((IconRect{ 4, 4, 20, 20 }), LeadingStatusIconBounds({ 0, 0, 100, 24 }, 96));
}

TEST(Codicons, EveryNativeWorkbenchIconRendersPixels)
{
	using Icon = workbench::icons::codicons::Icon;
	constexpr Icon icons[] = {
		Icon::Layout,
		Icon::LayoutSidebarLeft,
		Icon::LayoutPanel,
		Icon::LayoutSidebarRight,
		Icon::Account,
		Icon::Gear,
		Icon::ChromeMinimize,
		Icon::ChromeMaximize,
		Icon::ChromeRestore,
		Icon::ChromeClose,
		Icon::GitBranch,
		Icon::Target,
		Icon::Newline,
		Icon::Code,
		Icon::FileBinary,
		Icon::RecordSmall,
		Icon::Insert,
		Icon::ZoomIn,
		Icon::File,
		Icon::OpenPreview,
		Icon::ChevronDown,
		Icon::Close,
		Icon::CloseAll,
	};

	BITMAPINFO bitmapInfo{};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = 32;
	bitmapInfo.bmiHeader.biHeight = -32;
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;
	void* pixels = nullptr;
	const HDC dc = ::CreateCompatibleDC(nullptr);
	ASSERT_NE(nullptr, dc);
	const HBITMAP bitmap = ::CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
	ASSERT_NE(nullptr, bitmap);
	ASSERT_NE(nullptr, pixels);
	const HGDIOBJ oldBitmap = ::SelectObject(dc, bitmap);

	for (const auto icon : icons) {
		std::memset(pixels, 0, 32 * 32 * sizeof(std::uint32_t));
		workbench::icons::codicons::Draw(dc, { 8, 8, 24, 24 }, icon, RGB(1, 2, 3));
		const auto* values = static_cast<const std::uint32_t*>(pixels);
		EXPECT_TRUE(std::any_of(values, values + 32 * 32,
			[](std::uint32_t value) noexcept { return value != 0; }));
	}

	::SelectObject(dc, oldBitmap);
	::DeleteObject(bitmap);
	::DeleteDC(dc);
}

} // namespace
} // namespace workbench::activity
