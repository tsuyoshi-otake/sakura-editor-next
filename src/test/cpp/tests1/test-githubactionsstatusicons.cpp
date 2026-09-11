/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/icons/GitHubActionsStatusIcons.h"
#include "workbench/icons/GitHubActionsContainerIcon.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <string>

// The GitHub Actions status icons are the upstream extension's SVGs, drawn
// with GDI paths for the Tree View rows that name them by path. These tests
// render into a 32x32 white DIB, so one SVG unit of a 16-unit view box is two
// pixels, and read the result back. GDI samples a pixel at its integer device
// coordinate rather than its centre, so pixel (x, y) reads SVG (x / 2, y / 2);
// every probe below keeps at least one pixel clear of a shape boundary.

namespace github_actions = workbench::icons::github_actions;
using workbench::icons::IconRect;

namespace {

constexpr int kSize = 32;
constexpr COLORREF kWhite = RGB(255, 255, 255);

class Canvas {
public:
	explicit Canvas(int size = kSize) : size_(size)
	{
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = size_;
		info.bmiHeader.biHeight = -size_;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		dc_ = ::CreateCompatibleDC(nullptr);
		bitmap_ = ::CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
		if (dc_ != nullptr && bitmap_ != nullptr) previous_ = ::SelectObject(dc_, bitmap_);
		Clear();
	}
	Canvas(const Canvas&) = delete;
	Canvas& operator=(const Canvas&) = delete;
	~Canvas()
	{
		if (previous_ != nullptr) ::SelectObject(dc_, previous_);
		if (bitmap_ != nullptr) ::DeleteObject(bitmap_);
		if (dc_ != nullptr) ::DeleteDC(dc_);
	}

	[[nodiscard]] bool Valid() const noexcept { return dc_ != nullptr && bits_ != nullptr && previous_ != nullptr; }
	[[nodiscard]] HDC Dc() const noexcept { return dc_; }

	void Clear() noexcept
	{
		if (bits_ == nullptr) return;
		::GdiFlush();
		std::fill_n(static_cast<DWORD*>(bits_), size_ * size_, 0x00FFFFFFU);
	}

	[[nodiscard]] COLORREF Pixel(int x, int y) const noexcept
	{
		::GdiFlush();
		const DWORD value = static_cast<const DWORD*>(bits_)[y * size_ + x];
		return RGB((value >> 16) & 0xFFU, (value >> 8) & 0xFFU, value & 0xFFU);
	}

	[[nodiscard]] bool Blank() const noexcept
	{
		for (int y = 0; y < size_; ++y) {
			for (int x = 0; x < size_; ++x) {
				if (Pixel(x, y) != kWhite) return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool Draw(std::wstring_view name, bool light) const noexcept
	{
		return github_actions::Draw(dc_, IconRect{ 0, 0, size_, size_ }, name, light);
	}

	[[nodiscard]] bool DrawContainer(std::wstring_view name, COLORREF color) const noexcept
	{
		return github_actions::DrawContainerIcon(dc_, IconRect{ 0, 0, size_, size_ }, name, color);
	}

private:
	int size_ = kSize;
	HDC dc_ = nullptr;
	HBITMAP bitmap_ = nullptr;
	HGDIOBJ previous_ = nullptr;
	void* bits_ = nullptr;
};

[[nodiscard]] bool Near(COLORREF actual, COLORREF expected, int tolerance)
{
	return std::abs(GetRValue(actual) - GetRValue(expected)) <= tolerance
		&& std::abs(GetGValue(actual) - GetGValue(expected)) <= tolerance
		&& std::abs(GetBValue(actual) - GetBValue(expected)) <= tolerance;
}

constexpr std::wstring_view kRunSuccess = L"resources/icons/workflowruns/wr_success.svg";
constexpr std::wstring_view kRunInProgress = L"resources/icons/workflowruns/wr_inprogress.svg";
constexpr std::wstring_view kStepQueued = L"resources/icons/steps/step_queued.svg";

} // namespace

TEST(GitHubActionsStatusIcons, OnlyTheImportedPathsResolve)
{
	// Eight run states, six step states, and the job-log action glyph.
	EXPECT_EQ(github_actions::StatusIcons().size(), 15U);
	EXPECT_FALSE(github_actions::FindStatusIcon(L"resources/icons/light/logs.svg").empty());
	for (const auto& entry : github_actions::StatusIcons()) {
		EXPECT_TRUE(github_actions::IsIconPath(entry.Name()));
		EXPECT_FALSE(github_actions::FindStatusIcon(entry.Name()).empty()) << std::wstring(entry.Name());
	}
	// A codicon name is not a path, and a path this build did not import
	// resolves to nothing rather than to a substitute.
	EXPECT_FALSE(github_actions::IsIconPath(L"gear"));
	EXPECT_FALSE(github_actions::IsIconPath(L""));
	EXPECT_TRUE(github_actions::FindStatusIcon(L"gear").empty());
	EXPECT_TRUE(github_actions::FindStatusIcon(L"resources/icons/workflowruns/wr_warning.svg").empty());
	EXPECT_TRUE(github_actions::FindStatusIcon(L"resources/icons/light/workflowruns/wr_success.svg").empty());
}

TEST(GitHubActionsStatusIcons, StepQueuedKeepsItsThemeSpecificRings)
{
	const auto layers = github_actions::FindStatusIcon(kStepQueued);
	ASSERT_EQ(layers.size(), 2U);
	EXPECT_TRUE(github_actions::AppliesTo(layers[0], true));
	EXPECT_FALSE(github_actions::AppliesTo(layers[0], false));
	EXPECT_FALSE(github_actions::AppliesTo(layers[1], true));
	EXPECT_TRUE(github_actions::AppliesTo(layers[1], false));
}

TEST(GitHubActionsStatusIcons, EveryIconDrawsInBothThemesAndRestoresTheDc)
{
	Canvas canvas;
	ASSERT_TRUE(canvas.Valid());
	for (const bool light : { true, false }) {
		for (const auto& entry : github_actions::StatusIcons()) {
			canvas.Clear();
			EXPECT_TRUE(canvas.Draw(entry.Name(), light)) << std::wstring(entry.Name());
			EXPECT_FALSE(canvas.Blank()) << std::wstring(entry.Name());
			EXPECT_EQ(::GetGraphicsMode(canvas.Dc()), GM_COMPATIBLE);
			EXPECT_EQ(::GetPolyFillMode(canvas.Dc()), ALTERNATE);
		}
	}
}

TEST(GitHubActionsStatusIcons, UnknownPathsAndEmptyBoxesDrawNothing)
{
	Canvas canvas;
	ASSERT_TRUE(canvas.Valid());
	EXPECT_FALSE(canvas.Draw(L"resources/icons/workflowruns/wr_warning.svg", true));
	EXPECT_FALSE(github_actions::Draw(canvas.Dc(), IconRect{ 4, 4, 4, 20 }, kRunSuccess, true));
	EXPECT_FALSE(github_actions::Draw(nullptr, IconRect{ 0, 0, kSize, kSize }, kRunSuccess, true));
	EXPECT_TRUE(canvas.Blank());
}

TEST(GitHubActionsStatusIcons, SuccessUsesTheUpstreamLightAndDarkGreens)
{
	Canvas canvas;
	ASSERT_TRUE(canvas.Valid());
	// SVG (8, 3): inside the circle, above the check mark cut-out.
	ASSERT_TRUE(canvas.Draw(kRunSuccess, true));
	EXPECT_EQ(canvas.Pixel(16, 6), RGB(0x1A, 0x7F, 0x37));
	EXPECT_EQ(canvas.Pixel(0, 0), kWhite);
	canvas.Clear();
	ASSERT_TRUE(canvas.Draw(kRunSuccess, false));
	EXPECT_EQ(canvas.Pixel(16, 6), RGB(0x3F, 0xB9, 0x50));
	// The check mark itself is a hole showing the row background.
	EXPECT_EQ(canvas.Pixel(14, 18), kWhite);
}

TEST(GitHubActionsStatusIcons, InProgressRingIsHalfOpaqueOverTheBackground)
{
	Canvas canvas;
	ASSERT_TRUE(canvas.Valid());
	ASSERT_TRUE(canvas.Draw(kRunInProgress, true));
	// SVG (6, 2), radius 6.3, lies in the 50%-opacity ring (5.25..7) left of
	// the solid arc.
	EXPECT_TRUE(Near(canvas.Pixel(12, 4), RGB(223, 195, 127), 2));
	// The centre dot and the arc are solid. SVG (10, 2.5), radius 5.9, is in
	// the arc, which covers the ring's upper-right quadrant.
	EXPECT_EQ(canvas.Pixel(16, 16), RGB(0xBF, 0x87, 0x00));
	EXPECT_EQ(canvas.Pixel(20, 5), RGB(0xBF, 0x87, 0x00));
	// Between the dot and the ring, the background is untouched.
	EXPECT_EQ(canvas.Pixel(16, 7), kWhite);
}

TEST(GitHubActionsStatusIcons, StepQueuedDrawsOnlyTheActiveThemesRing)
{
	Canvas canvas;
	ASSERT_TRUE(canvas.Valid());
	// Row 1 is SVG radius 7.5, inside only the light ring (6.5..8); row 4 is
	// radius 6, inside only the dark ring (5.6875..7). Row 3 would sit on the
	// light ring's inner edge.
	ASSERT_TRUE(canvas.Draw(kStepQueued, true));
	EXPECT_EQ(canvas.Pixel(16, 1), RGB(0x57, 0x60, 0x6A));
	EXPECT_EQ(canvas.Pixel(16, 4), kWhite);
	canvas.Clear();
	ASSERT_TRUE(canvas.Draw(kStepQueued, false));
	EXPECT_EQ(canvas.Pixel(16, 1), kWhite);
	EXPECT_EQ(canvas.Pixel(16, 4), RGB(0x8B, 0x94, 0x9E));
}

// The Activity Bar icon is upstream's 24-unit explorer.svg (the Octicons
// workflow glyph). A 48x48 canvas makes one SVG unit two pixels again.
TEST(GitHubActionsContainerIcon, OnlyTheManifestPathIsAContainerIcon)
{
	EXPECT_TRUE(github_actions::IsContainerIcon(L"resources/icons/light/explorer.svg"));
	EXPECT_TRUE(github_actions::IsIconPath(github_actions::kContainerIconPath));
	// The two vocabularies are addressed separately: a container icon is not a
	// Tree row status icon, and the reverse.
	EXPECT_TRUE(github_actions::FindStatusIcon(github_actions::kContainerIconPath).empty());
	EXPECT_FALSE(github_actions::IsContainerIcon(kRunSuccess));
	EXPECT_FALSE(github_actions::IsContainerIcon(L"resources/icons/dark/explorer.svg"));
	EXPECT_FALSE(github_actions::IsContainerIcon(L"play-circle"));
	EXPECT_FALSE(github_actions::IsContainerIcon(L""));
}

TEST(GitHubActionsContainerIcon, DrawsTheWorkflowGlyphInTheCallersColour)
{
	constexpr COLORREF kIcon = RGB(0x33, 0x66, 0x99);
	Canvas canvas(48);
	ASSERT_TRUE(canvas.Valid());
	ASSERT_TRUE(canvas.DrawContainer(github_actions::kContainerIconPath, kIcon));
	// Top-left box: its left border (SVG x 1..2.5) is filled, its hole is not.
	EXPECT_EQ(canvas.Pixel(3, 12), kIcon);
	EXPECT_EQ(canvas.Pixel(12, 12), kWhite);
	// Bottom-right box: its right border (SVG x 21.5..23) and its hole.
	EXPECT_EQ(canvas.Pixel(44, 36), kIcon);
	EXPECT_EQ(canvas.Pixel(35, 36), kWhite);
	// The connector (SVG x 5.5..7) that leaves the top-left box downwards.
	EXPECT_EQ(canvas.Pixel(12, 27), kIcon);
	// Nothing is drawn in the empty top-right quarter.
	EXPECT_EQ(canvas.Pixel(36, 12), kWhite);
	EXPECT_EQ(::GetGraphicsMode(canvas.Dc()), GM_COMPATIBLE);
	EXPECT_EQ(::GetPolyFillMode(canvas.Dc()), ALTERNATE);
}

TEST(GitHubActionsContainerIcon, OtherNamesAndEmptyBoxesDrawNothing)
{
	Canvas canvas(48);
	ASSERT_TRUE(canvas.Valid());
	EXPECT_FALSE(canvas.DrawContainer(kRunSuccess, RGB(0, 0, 0)));
	EXPECT_FALSE(canvas.DrawContainer(L"resources/icons/other.svg", RGB(0, 0, 0)));
	EXPECT_FALSE(github_actions::DrawContainerIcon(canvas.Dc(), IconRect{ 4, 4, 4, 20 },
		github_actions::kContainerIconPath, RGB(0, 0, 0)));
	EXPECT_FALSE(github_actions::DrawContainerIcon(nullptr, IconRect{ 0, 0, 48, 48 },
		github_actions::kContainerIconPath, RGB(0, 0, 0)));
	EXPECT_TRUE(canvas.Blank());
}
