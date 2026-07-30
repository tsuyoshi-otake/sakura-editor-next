/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionViewRegistry.h"

#include <gtest/gtest.h>

namespace {

SExtensionViewDescriptor View(std::wstring handle, std::wstring id, std::uint64_t generation = 1)
{
	return { .handle = std::move(handle), .viewId = std::move(id), .title = L"Sample",
		.extensionId = L"sample.extension", .generation = generation };
}

SExtensionTreeItem Item(
	std::wstring handle,
	std::wstring viewHandle,
	std::wstring parent,
	std::wstring label,
	EExtensionTreeItemCollapsibleState collapsible = EExtensionTreeItemCollapsibleState::None)
{
	return { .handle = std::move(handle), .viewHandle = std::move(viewHandle),
		.parentHandle = std::move(parent), .label = std::move(label), .collapsibleState = collapsible };
}

} // namespace

TEST(CExtensionViewRegistry, EnforcesUniqueViewIdsAndGenerationOwnership)
{
	CExtensionViewRegistry registry;
	ASSERT_TRUE(registry.Register(View(L"view-1", L"sample.files")));
	EXPECT_FALSE(registry.Register(View(L"view-2", L"sample.files")));
	auto update = View(L"view-1", L"sample.files");
	update.title = L"Updated";
	EXPECT_TRUE(registry.Update(update));
	update.extensionId = L"other.extension";
	EXPECT_FALSE(registry.Update(update));
	EXPECT_FALSE(registry.Unregister(L"view-1", L"sample.extension", 2));
	EXPECT_TRUE(registry.Unregister(L"view-1", L"sample.extension", 1));
	EXPECT_TRUE(registry.Views().empty());
}

TEST(CExtensionTreeView, ReplacesChildrenAtomicallyAndDropsStaleDescendants)
{
	CExtensionViewRegistry registry;
	ASSERT_TRUE(registry.Register(View(L"view", L"sample.files")));
	EXPECT_FALSE(registry.HasChildrenSnapshot(L"view"));
	ASSERT_TRUE(registry.ReplaceChildren(L"view", L"", L"sample.extension", 1, {
		Item(L"root", L"view", L"", L"Root", EExtensionTreeItemCollapsibleState::Expanded) }));
	EXPECT_TRUE(registry.HasChildrenSnapshot(L"view"));
	ASSERT_TRUE(registry.ReplaceChildren(L"view", L"root", L"sample.extension", 1, {
		Item(L"child", L"view", L"root", L"Child", EExtensionTreeItemCollapsibleState::Collapsed) }));
	ASSERT_TRUE(registry.ReplaceChildren(L"view", L"child", L"sample.extension", 1, {
		Item(L"leaf", L"view", L"child", L"Leaf") }));
	EXPECT_TRUE(registry.HasChildrenSnapshot(L"view", L"child"));
	EXPECT_EQ((std::vector<std::wstring>{ L"root", L"child", L"leaf" }), registry.RevealPath(L"view", L"leaf"));

	ASSERT_TRUE(registry.ReplaceChildren(L"view", L"root", L"sample.extension", 1, {
		Item(L"replacement", L"view", L"root", L"Replacement") }));
	EXPECT_TRUE(registry.Children(L"view", L"child").empty());
	EXPECT_FALSE(registry.HasChildrenSnapshot(L"view", L"child"));
	EXPECT_TRUE(registry.RevealPath(L"view", L"leaf").empty());
	ASSERT_EQ(1u, registry.Children(L"view", L"root").size());
	EXPECT_EQ(L"replacement", registry.Children(L"view", L"root")[0].handle);
}

TEST(CExtensionTreeView, SelectionHonorsMultiplicityAndInvalidationIsTerminal)
{
	CExtensionViewRegistry registry;
	auto single = View(L"single", L"sample.single");
	ASSERT_TRUE(registry.Register(single));
	ASSERT_TRUE(registry.ReplaceChildren(L"single", L"", L"sample.extension", 1, {
		Item(L"one", L"single", L"", L"One"), Item(L"two", L"single", L"", L"Two") }));
	EXPECT_FALSE(registry.SetSelection(L"single", { L"one", L"two" }));
	EXPECT_TRUE(registry.SetSelection(L"single", { L"one" }));
	EXPECT_TRUE(registry.Invalidate(L"single"));
	EXPECT_FALSE(registry.HasChildrenSnapshot(L"single"));
	EXPECT_TRUE(registry.Selection(L"single").empty());
	EXPECT_TRUE(registry.Children(L"single").empty());
}

TEST(CExtensionViewRegistry, HostGenerationCleanupRemovesOnlyOwnedViews)
{
	CExtensionViewRegistry registry;
	ASSERT_TRUE(registry.Register(View(L"old", L"sample.old", 7)));
	ASSERT_TRUE(registry.Register(View(L"new", L"sample.new", 8)));
	registry.RemoveOwnedBy(L"sample.extension", 7);
	const auto views = registry.Views();
	ASSERT_EQ(1u, views.size());
	EXPECT_EQ(L"new", views[0].handle);
}
