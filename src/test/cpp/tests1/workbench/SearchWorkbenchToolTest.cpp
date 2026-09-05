/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <utility>

#include "workbench/search/CSearchWorkbenchTool.h"

namespace {

//! The geometry's "the font has not been measured yet" input, which selects the
//! CSS vertical padding as the fallback line height.
constexpr int kUnmeasuredLineHeight = 0;

int ScaleDip(int dip, unsigned int dpi)
{
	return ::MulDiv(dip, static_cast<int>(dpi == 0 ? 96u : dpi), 96);
}

void ExpectRect(const RECT& expected, const RECT& actual)
{
	EXPECT_EQ(expected.left, actual.left);
	EXPECT_EQ(expected.top, actual.top);
	EXPECT_EQ(expected.right, actual.right);
	EXPECT_EQ(expected.bottom, actual.bottom);
}

TEST(SearchWorkbenchToolGeometry, MatchesVsCodeSearchWidgetAtDefaultDpi)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 480, 320 }, 96, false, kUnmeasuredLineHeight);

	ExpectRect(RECT{ 2, 0, 468, 38 }, geometry.container);
	ExpectRect(RECT{ 20, 6, 468, 32 }, geometry.queryBox);
	ExpectRect(RECT{ 2, 6, 18, 32 }, geometry.toggleReplace);
	ExpectRect(RECT{ 21, 9, 384, 29 }, geometry.queryEdit);
	EXPECT_EQ(26, geometry.queryBox.bottom - geometry.queryBox.top);
	EXPECT_EQ(18, geometry.queryBox.left - geometry.container.left);
	EXPECT_EQ(16, geometry.toggleReplace.right - geometry.toggleReplace.left);
	EXPECT_EQ(6, geometry.queryBox.top - geometry.container.top);
	EXPECT_EQ(6, geometry.container.bottom - geometry.queryBox.bottom);
}

TEST(SearchWorkbenchToolGeometry, ReplaceRowUsesCssMarginsAndCentersNativeEdit)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 480, 320 }, 96, true, kUnmeasuredLineHeight);

	ExpectRect(RECT{ 20, 6, 468, 32 }, geometry.queryBox);
	ExpectRect(RECT{ 20, 38, 440, 64 }, geometry.replaceBox);
	ExpectRect(RECT{ 444, 39, 468, 63 }, geometry.replaceAll);
	ExpectRect(RECT{ 2, 6, 18, 64 }, geometry.toggleReplace);
	ExpectRect(RECT{ 21, 9, 384, 29 }, geometry.queryEdit);
	ExpectRect(RECT{ 21, 41, 396, 61 }, geometry.replaceEdit);
	EXPECT_EQ(6, geometry.replaceBox.top - geometry.queryBox.bottom);
	EXPECT_EQ(6, geometry.container.bottom - geometry.replaceBox.bottom);
	EXPECT_EQ(20, geometry.replaceEdit.bottom - geometry.replaceEdit.top);
	EXPECT_EQ(geometry.queryEdit.top - geometry.queryBox.top,
		geometry.queryBox.bottom - geometry.queryEdit.bottom);
	EXPECT_EQ(geometry.replaceEdit.top - geometry.replaceBox.top,
		geometry.replaceBox.bottom - geometry.replaceEdit.bottom);
}

TEST(SearchWorkbenchToolGeometry, PreservesCssRelationshipsAcrossSupportedDpi)
{
	constexpr RECT client{ 10, 20, 810, 620 };
	for (const unsigned int dpi : { 96u, 120u, 144u, 192u }) {
		const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
			client, dpi, true, kUnmeasuredLineHeight);
		const int inputHeight = ScaleDip(26, dpi);

		EXPECT_EQ(client.left + ScaleDip(2, dpi), geometry.container.left);
		EXPECT_EQ(client.right - ScaleDip(12, dpi), geometry.container.right);
		EXPECT_EQ(ScaleDip(6, dpi), geometry.queryBox.top - geometry.container.top);
		EXPECT_EQ(ScaleDip(18, dpi), geometry.queryBox.left - geometry.container.left);
		EXPECT_EQ(ScaleDip(16, dpi),
			geometry.toggleReplace.right - geometry.toggleReplace.left);
		EXPECT_EQ(inputHeight, geometry.queryBox.bottom - geometry.queryBox.top);
		EXPECT_EQ(ScaleDip(6, dpi), geometry.replaceBox.top - geometry.queryBox.bottom);
		EXPECT_EQ(inputHeight - 2 * ScaleDip(3, dpi),
			geometry.replaceEdit.bottom - geometry.replaceEdit.top);
		EXPECT_EQ(geometry.queryEdit.top - geometry.queryBox.top,
			geometry.queryBox.bottom - geometry.queryEdit.bottom);
		EXPECT_EQ(geometry.replaceEdit.top - geometry.replaceBox.top,
			geometry.replaceBox.bottom - geometry.replaceEdit.bottom);
		EXPECT_EQ(ScaleDip(6, dpi), geometry.container.bottom - geometry.replaceBox.bottom);
		EXPECT_LE(geometry.queryEdit.left, geometry.queryEdit.right);
		EXPECT_LE(geometry.replaceEdit.left, geometry.replaceEdit.right);
	}
}

TEST(SearchWorkbenchToolGeometry, MeasuredLineHeightCentersTheCaretLineNotJustTheHwnd)
{
	// The HWND being centered is not enough: a single-line EDIT top-anchors its
	// text inside a taller client, so a 20px control holding a 13px line leaves
	// 3px above the caret and 10px below it.  Sizing the control to the measured
	// line is what actually centers the text the user sees.
	constexpr int kLineHeight = 13;
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 480, 320 }, 96, true, kLineHeight);

	EXPECT_EQ(kLineHeight, geometry.queryEdit.bottom - geometry.queryEdit.top);
	EXPECT_EQ(kLineHeight, geometry.replaceEdit.bottom - geometry.replaceEdit.top);
	for (const auto& [box, edit] : {
			std::pair{ geometry.queryBox, geometry.queryEdit },
			std::pair{ geometry.replaceBox, geometry.replaceEdit } }) {
		const LONG above = edit.top - box.top;
		const LONG below = box.bottom - edit.bottom;
		EXPECT_EQ((box.bottom - box.top - kLineHeight) / 2, above);
		EXPECT_LE(std::abs(above - below), 1);
	}
}

TEST(SearchWorkbenchToolGeometry, MeasuredLineHeightStaysCenteredAcrossSupportedDpi)
{
	constexpr RECT client{ 10, 20, 810, 620 };
	for (const unsigned int dpi : { 96u, 120u, 144u, 192u }) {
		const int lineHeight = ScaleDip(13, dpi);
		const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
			client, dpi, true, lineHeight);

		EXPECT_EQ(lineHeight, geometry.queryEdit.bottom - geometry.queryEdit.top);
		EXPECT_EQ(lineHeight, geometry.replaceEdit.bottom - geometry.replaceEdit.top);
		EXPECT_LE(std::abs((geometry.queryEdit.top - geometry.queryBox.top)
			- (geometry.queryBox.bottom - geometry.queryEdit.bottom)), 1);
		// The horizontal contract must survive the vertical change.
		EXPECT_EQ(ScaleDip(1, dpi), geometry.queryEdit.left - geometry.queryBox.left);
		EXPECT_LE(geometry.queryEdit.right, geometry.queryBox.right);
	}
}

TEST(SearchWorkbenchToolGeometry, LineHeightTallerThanTheBoxIsClampedToTheBox)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 480, 320 }, 96, true, 999);

	EXPECT_EQ(geometry.queryBox.top, geometry.queryEdit.top);
	EXPECT_EQ(geometry.queryBox.bottom, geometry.queryEdit.bottom);
	EXPECT_EQ(geometry.replaceBox.top, geometry.replaceEdit.top);
	EXPECT_EQ(geometry.replaceBox.bottom, geometry.replaceEdit.bottom);
}

TEST(SearchWorkbenchToolGeometry, ClampsNarrowClientWithoutNegativeEditBounds)
{
	const auto geometry = workbench::search::CalculateSearchWidgetGeometry(
		RECT{ 0, 0, 32, 100 }, 192, true, kUnmeasuredLineHeight);

	EXPECT_LE(geometry.container.left, geometry.container.right);
	EXPECT_LE(geometry.queryEdit.left, geometry.queryEdit.right);
	EXPECT_LE(geometry.replaceEdit.left, geometry.replaceEdit.right);
	EXPECT_EQ(ScaleDip(26, 192) - 2 * ScaleDip(3, 192),
		geometry.queryEdit.bottom - geometry.queryEdit.top);
	EXPECT_EQ(ScaleDip(26, 192) - 2 * ScaleDip(3, 192),
		geometry.replaceEdit.bottom - geometry.replaceEdit.top);
}

} // namespace

#include "env/ShareDataTestSuite.hpp"
#include "workbench/search/WorkspaceSearchEngine.h"
#include "workbench/WorkerRetirementService.h"
#include <fstream>
#include <random>
#include "cxx/ResourceHolder.hpp"

class SearchRequestSafetyTest : public ::testing::Test, public env::ShareDataTestSuite {
protected:
    static void SetUpTestSuite() { SetUpShareData(); }
    static void TearDownTestSuite() { TearDownShareData(); }
    void SetUp() override {
        wchar_t temporary[MAX_PATH]{}, name[MAX_PATH]{};
        ASSERT_NE(0u, ::GetTempPathW(MAX_PATH, temporary));
        ASSERT_NE(0u, ::GetTempFileNameW(temporary, L"ssr", 0, name));
        ASSERT_TRUE(::DeleteFileW(name));
        ASSERT_TRUE(::CreateDirectoryW(name, nullptr));
        root = name;
        Write(std::string(600, 'a') + "needle\r\n");
    }
    void TearDown() override {
        EXPECT_TRUE(::DeleteFileW((root / L"input.txt").c_str()));
        EXPECT_TRUE(::RemoveDirectoryW(root.c_str()));
    }
    void Write(const std::string& content) {
        std::ofstream file(root / L"input.txt", std::ios::binary | std::ios::trunc);
        file << "\xef\xbb\xbf" << content;
        file.close();
        ASSERT_FALSE(file.fail());
    }
    std::filesystem::path root;
};

TEST_F(SearchRequestSafetyTest, LongLinePreviewContainsTheActualMatch)
{
    workbench::search::SearchQuery query;
    query.text = L"needle";
    const auto result = workbench::search::RunWorkspaceSearch(root.wstring(), query, {});
    ASSERT_EQ(1u, result.matchCount);
    const auto& match = result.files.front().matches.front();
    EXPECT_EQ(601, match.column);
    EXPECT_EQ(6, match.length);
    EXPECT_EQ(6, match.previewLength);
    EXPECT_EQ(L"needle", match.preview.substr(match.previewOffset, match.previewLength));
    EXPECT_LE(match.preview.size(), workbench::search::kSearchPreviewMaxLength);
}

TEST_F(SearchRequestSafetyTest, PreviewDoesNotSplitUtf16SurrogatePair)
{
    Write(std::string(249, 'a') + "\xf0\x9f\x98\x80" + std::string(400, 'a') + "needle");
    workbench::search::SearchQuery query;
    query.text = L"a";
    const auto result = workbench::search::RunWorkspaceSearch(root.wstring(), query, {});
    ASSERT_GT(result.matchCount, 0u);
    for (const auto& match : result.files.front().matches) {
        ASSERT_FALSE(match.preview.empty());
        EXPECT_FALSE(match.preview.front() >= 0xdc00 && match.preview.front() <= 0xdfff);
        EXPECT_FALSE(match.preview.back() >= 0xd800 && match.preview.back() <= 0xdbff);
    }
}

TEST_F(SearchRequestSafetyTest, ClearingQueryRejectsAlreadyPostedCompletion)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
    SCOPED_TRACE(scenario);
    using workbench::search::CSearchWorkbenchTool;
    const cxx::ResourceHolder<&::DestroyWindow> parent{::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 400, 300, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr)};
    ASSERT_NE(nullptr, parent.get());
    CSearchWorkbenchTool tool;
    ASSERT_TRUE(tool.Create(parent.get()));
    tool.SetRoot(root.wstring());
    tool.SetQueryText(L"needle");
    // Hold the actual posted completion in USER32's queue, then clear input.
    constexpr UINT completed = WM_APP + 0x5e1;
    const ULONGLONG deadline = ::GetTickCount64() + 10000;
    MSG message{};
    bool posted = false;
    while (::GetTickCount64() < deadline) {
        if (::PeekMessageW(&message, tool.GetHwnd(), completed, completed, PM_NOREMOVE)) {
            posted = true;
            break;
        }
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_POSTMESSAGE, 0);
    }
    ASSERT_TRUE(posted);
    if (scenario == 0) tool.SetQueryText(L"");
    if (scenario == 1) ::SetWindowTextW(::GetDlgItem(tool.GetHwnd(), 1), L"different");
    if (scenario == 2) tool.SetRoot(L"");
    if (scenario == 3) tool.Close();
    while (::PeekMessageW(&message, tool.GetHwnd(), completed, completed, PM_REMOVE)) {
        ::DispatchMessageW(&message);
    }
    EXPECT_EQ(0, ::SendMessageW(::GetDlgItem(tool.GetHwnd(), 3), LB_GETCOUNT, 0, 0));
    tool.Close();
    const ULONGLONG retiredDeadline = ::GetTickCount64() + 10000;
    auto& retirement = workbench::WorkerRetirementService::Instance();
    while (retirement.ReservedOrPendingCount() != 0 && ::GetTickCount64() < retiredDeadline) {
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_POSTMESSAGE, 0);
    }
    EXPECT_EQ(0u, retirement.ReservedOrPendingCount());
    }
}
TEST_F(SearchRequestSafetyTest, SeededPreviewWindowsPreserveSourceCoordinatesAndHitText)
{
    constexpr unsigned seed = 0x290;
    std::mt19937 random(seed);
    for (int sample = 0; sample < 64; ++sample) {
        SCOPED_TRACE(sample);
        const std::size_t prefix = random() % 2000;
        const std::size_t hitLength = 1 + random() % 400;
        const std::string needle(hitLength, 'n');
        Write(std::string(prefix, 'x') + needle + std::string(random() % 600, 'x'));
        workbench::search::SearchQuery query;
        query.text.assign(hitLength, L'n');
        const auto result = workbench::search::RunWorkspaceSearch(root.wstring(), query, {});
        ASSERT_EQ(1u, result.matchCount);
        const auto& match = result.files.front().matches.front();
        EXPECT_EQ(prefix + 1, static_cast<std::size_t>(match.column));
        EXPECT_EQ(hitLength, static_cast<std::size_t>(match.length));
        EXPECT_GT(match.previewLength, 0);
        ASSERT_GE(match.previewOffset, 0);
        EXPECT_LE(match.preview.size(), workbench::search::kSearchPreviewMaxLength);
        EXPECT_EQ(std::wstring(match.previewLength, L'n'),
            match.preview.substr(match.previewOffset, match.previewLength));
    }
}