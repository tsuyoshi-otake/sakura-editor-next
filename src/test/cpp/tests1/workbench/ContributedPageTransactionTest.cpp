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

} // namespace
} // namespace workbench::viewcontainer
