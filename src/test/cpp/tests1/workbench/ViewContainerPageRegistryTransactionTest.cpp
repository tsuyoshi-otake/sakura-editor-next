/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/viewcontainer/ViewContainerPageRegistry.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace workbench::viewcontainer {
namespace {

[[nodiscard]] ViewContainerPageDescriptor Descriptor(std::string id)
{
	return {
		.containerId = std::move(id),
		.supportedLocations = { layout::EViewContainerLocation::Sidebar },
		.factory = [] { return std::unique_ptr<IViewContainerPage>{}; },
	};
}

TEST(ViewContainerPageRegistryTransaction, PublishesPreparedBatchExactlyOnce)
{
	ViewContainerPageRegistry registry;
	auto prepared = registry.PrepareBatch({ Descriptor("sample.first"),
		Descriptor("sample.second") });

	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered, prepared.Status());
	ASSERT_EQ(2U, prepared.PreparedCount());
	ASSERT_TRUE(registry.CanCommit(prepared));
	EXPECT_EQ(nullptr, registry.Find("sample.first"));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.Commit(std::move(prepared)).status);
	EXPECT_NE(nullptr, registry.Find("sample.first"));
	EXPECT_NE(nullptr, registry.Find("sample.second"));
	EXPECT_FALSE(registry.CanCommit(prepared));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		registry.Commit(std::move(prepared)).status);
	EXPECT_EQ(2U, registry.Size());
}

TEST(ViewContainerPageRegistryTransaction, RejectsStaleAndForeignPreparedBatches)
{
	ViewContainerPageRegistry registry;
	ViewContainerPageRegistry other;
	auto stale = registry.PrepareBatch({ Descriptor("sample.stale") });
	auto foreign = registry.PrepareBatch({ Descriptor("sample.foreign") });
	EXPECT_FALSE(other.CanCommit(foreign));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		other.Commit(std::move(foreign)).status);
	EXPECT_TRUE(registry.CanCommit(foreign));
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ Descriptor("sample.current") }).status);

	EXPECT_FALSE(registry.CanCommit(stale));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		registry.Commit(std::move(stale)).status);
	EXPECT_EQ(nullptr, registry.Find("sample.stale"));
	EXPECT_FALSE(registry.CanCommit(foreign));
	EXPECT_EQ(nullptr, registry.Find("sample.foreign"));
	EXPECT_EQ(1U, registry.Size());
}

TEST(ViewContainerPageRegistryTransaction, InvalidPreparationLeavesRegistryUnchanged)
{
	ViewContainerPageRegistry registry;
	auto prepared = registry.PrepareBatch({ Descriptor("sample.duplicate"),
		Descriptor("sample.duplicate") });

	EXPECT_EQ(EViewContainerPageRegistrationStatus::DuplicateContainerId, prepared.Status());
	EXPECT_FALSE(prepared.Succeeded());
	EXPECT_FALSE(registry.CanCommit(prepared));
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Failed,
		registry.Commit(std::move(prepared)).status);
	EXPECT_EQ(0U, registry.Size());
}

} // namespace
} // namespace workbench::viewcontainer
