/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "env/ShareDataTestSuite.hpp"
#include "outline/CDlgFuncList.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace workbench::viewcontainer {
namespace {

class ContributedPageTransaction : public testing::Test, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite() { SetUpShareData(); }
	static void TearDownTestSuite() { TearDownShareData(); }
};

[[nodiscard]] ViewContainerPageDescriptor Descriptor(std::string id)
{
	return {
		.containerId = std::move(id),
		.supportedLocations = { layout::EViewContainerLocation::Sidebar },
		.factory = [] { return std::unique_ptr<IViewContainerPage>{}; },
	};
}

TEST_F(ContributedPageTransaction, CommitsOnePreparedStartupBatch)
{
	CDlgFuncList dialog;
	CViewContainerPages pages(dialog);
	auto prepared = pages.PrepareContributedPages({ Descriptor("sample.first"),
		Descriptor("sample.second") });

	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered, prepared.Status());
	ASSERT_EQ(2U, prepared.PreparedCount());
	ASSERT_TRUE(pages.CanCommit(prepared));
	EXPECT_TRUE(pages.PageIds().empty());
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered,
		pages.Commit(std::move(prepared)).status);
	EXPECT_TRUE(pages.PageIds().empty());
	EXPECT_FALSE(pages.CanCommit(prepared));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		pages.Commit(std::move(prepared)).status);

	auto duplicate = pages.PrepareContributedPages({ Descriptor("sample.first") });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::DuplicateContainerId, duplicate.Status());
}

TEST_F(ContributedPageTransaction, RejectsStalePreparedStartupBatch)
{
	CDlgFuncList dialog;
	CViewContainerPages pages(dialog);
	auto stale = pages.PrepareContributedPages({ Descriptor("sample.stale") });
	auto current = pages.PrepareContributedPages({ Descriptor("sample.current") });
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		pages.Commit(std::move(current)).status);

	EXPECT_FALSE(pages.CanCommit(stale));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		pages.Commit(std::move(stale)).status);
	auto staleId = pages.PrepareContributedPages({ Descriptor("sample.stale") });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered, staleId.Status());
}

TEST_F(ContributedPageTransaction, KeepsRegisterApiAsTransactionalConvenience)
{
	CDlgFuncList dialog;
	CViewContainerPages pages(dialog);
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered,
		pages.RegisterContributedPages({ Descriptor("sample.current") }).status);
	EXPECT_EQ(EViewContainerPageRegistrationStatus::DuplicateContainerId,
		pages.RegisterContributedPages({ Descriptor("sample.current") }).status);
	EXPECT_EQ(EViewContainerPageRegistrationStatus::NotApplicable,
		pages.RegisterContributedPages({}).status);
}

TEST_F(ContributedPageTransaction, PublishesBatchIntoCreatedRegistry)
{
	const auto ownerHandle = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 320, 240, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	using WindowOwner = std::unique_ptr<std::remove_pointer_t<decltype(ownerHandle)>,
		decltype(&::DestroyWindow)>;
	WindowOwner owner(ownerHandle, &::DestroyWindow);
	ASSERT_NE(nullptr, owner.get());
	CDlgFuncList dialog;
	CViewContainerPages pages(dialog);
	auto startupStale = pages.PrepareContributedPages({ Descriptor("sample.stale") });
	ASSERT_TRUE(pages.Create(owner.get()));
	EXPECT_FALSE(pages.CanCommit(startupStale));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		pages.Commit(std::move(startupStale)).status);
	EXPECT_FALSE(pages.Contains("sample.stale"));
	auto prepared = pages.PrepareContributedPages({ Descriptor("sample.dynamic") });

	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered, prepared.Status());
	ASSERT_TRUE(pages.CanCommit(prepared));
	EXPECT_FALSE(pages.Contains("sample.dynamic"));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered,
		pages.Commit(std::move(prepared)).status);
	EXPECT_TRUE(pages.Contains("sample.dynamic"));
	const auto pageIds = pages.PageIds();
	EXPECT_NE(pageIds.end(), std::ranges::find(pageIds, "sample.dynamic"));
	pages.Close();
}

TEST_F(ContributedPageTransaction, EmptyOutlinePagePaintsTheSelectedSideBarColor)
{
	const HWND ownerHandle = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 320, 240, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	using WindowOwner = std::unique_ptr<std::remove_pointer_t<decltype(ownerHandle)>,
		decltype(&::DestroyWindow)>;
	WindowOwner owner(ownerHandle, &::DestroyWindow);
	ASSERT_NE(nullptr, owner.get());
	CDlgFuncList dialog;
	CViewContainerPages pages(dialog);
	ASSERT_TRUE(pages.Create(owner.get()));

	HWND explorerPage = nullptr;
	for (HWND window = ::FindWindowExW(owner.get(), nullptr,
		L"SakuraEditorNext.ViewContainerPage", nullptr); window != nullptr;
		window = ::FindWindowExW(owner.get(), window,
			L"SakuraEditorNext.ViewContainerPage", nullptr)) {
		if (::FindWindowExW(window, nullptr, L"SakuraNativeExplorerTool", nullptr)) {
			explorerPage = window;
			break;
		}
	}
	ASSERT_NE(nullptr, explorerPage);
	pages.LayoutPage(pageIds::Explorer, RECT{ 0, 0, 16, 16 });
	ASSERT_EQ(nullptr, ::FindWindowExW(explorerPage, nullptr, L"#32770", nullptr));

	struct PaintBuffer final {
		HDC dc{ ::CreateCompatibleDC(nullptr) };
		HBITMAP bitmap{};
		HGDIOBJ previous{};
		PaintBuffer() {
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = 16;
			info.bmiHeader.biHeight = -16;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (dc && bitmap) previous = ::SelectObject(dc, bitmap);
		}
		~PaintBuffer() {
			if (previous && previous != HGDI_ERROR) ::SelectObject(dc, previous);
			if (bitmap) ::DeleteObject(bitmap);
			if (dc) ::DeleteDC(dc);
		}
	} buffer;
	ASSERT_NE(nullptr, buffer.dc);
	ASSERT_NE(nullptr, buffer.bitmap);
	ASSERT_NE(nullptr, buffer.previous);
	ASSERT_NE(HGDI_ERROR, buffer.previous);
	for (const theme::ThemeColor color : { theme::ThemeColor{ 0x29, 0x31, 0x34 },
		theme::ThemeColor{ 0xF8, 0xF8, 0xF8 } }) {
		theme::ThemePalette palette;
		palette.sideBar = color;
		pages.SetPalette(palette);
		ASSERT_TRUE(::PatBlt(buffer.dc, 0, 0, 16, 16, WHITENESS));
		::SendMessageW(explorerPage, WM_PRINTCLIENT,
			reinterpret_cast<WPARAM>(buffer.dc), 0);
		EXPECT_EQ(color.ToColorRef(), ::GetPixel(buffer.dc, 8, 8));
	}
	pages.Close();
}

} // namespace
} // namespace workbench::viewcontainer
