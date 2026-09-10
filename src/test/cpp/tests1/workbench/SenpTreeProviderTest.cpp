/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/tree/SenpTreeProvider.h"
#include <stdexcept>

namespace workbench::tree {
namespace {
TreeItem Leaf(std::wstring id)
{
	TreeItem item; item.id = std::move(id); item.label = item.id; return item;
}
TreeItem Branch(std::wstring id, bool expanded = false)
{
	auto item = Leaf(std::move(id)); item.collapsibleState = expanded ? TreeItemCollapsibleState::Expanded : TreeItemCollapsibleState::Collapsed; return item;
}
TreeLoadRequest Begin(TreeViewModel& model, std::wstring_view parent = L"", TreeLoadKind kind = TreeLoadKind::Initial)
{
	auto result = model.Begin(parent, kind);
	EXPECT_EQ(TreeResult::Accepted, result.result); EXPECT_TRUE(result.request);
	return result.request.value_or(TreeLoadRequest{});
}
TreeChildrenPage Page(std::wstring parent, std::vector<TreeItem> items, std::uint64_t revision = 1, std::wstring cursor = L"")
{
	TreeChildrenPage page; page.parentId = std::move(parent); page.items = std::move(items); page.revision = revision;
	page.nextCursor = std::move(cursor); page.state = page.nextCursor.empty() ? TreePageState::Complete : TreePageState::Partial;
	return page;
}
std::size_t CountBytes(const TreeViewModel& model, std::wstring_view parent = L"")
{
	const auto value = model.Node(parent).value();
	std::size_t units = value.item.id.size() + value.item.label.size() + value.item.description.size() + value.item.tooltip.size()
		+ value.item.icon.size() + value.item.commandId.size() + value.parentId.size() + value.nextCursor.size() + value.message.size();
	for (const auto& argument : value.item.arguments) units += argument.size();
	std::size_t bytes{};
	for (const auto& child : value.children) { units += child.size(); bytes += CountBytes(model, child); }
	return bytes + units * sizeof(wchar_t);
}

TEST(TreeViewModel, LoadsOnlyRootAndExpandedBranchesAndNeverFetchesLeaves)
{
	TreeViewModel model;
	EXPECT_EQ(std::vector<std::wstring>{ L"" }, model.Demand());
	auto request = Begin(model); EXPECT_TRUE(model.Demand().empty());
	ASSERT_EQ(TreeResult::Applied, model.Apply(request, Page(L"", { Branch(L"closed"), Branch(L"open", true), Leaf(L"leaf") })).result);
	EXPECT_EQ(std::vector<std::wstring>{ L"open" }, model.Demand());
	EXPECT_EQ(TreeResult::Invalid, model.Begin(L"leaf", TreeLoadKind::Initial).result);
	EXPECT_EQ(TreeResult::Invalid, model.Begin(L"closed", TreeLoadKind::Initial).result);
	EXPECT_EQ(TreeResult::Invalid, model.SetExpanded(L"leaf", true).result);
	EXPECT_EQ(TreeResult::Applied, model.SetExpanded(L"closed", true).result);
	EXPECT_EQ(2, model.Demand().size());
}
TEST(TreeViewModel, AppendsAnEmptyFilteredPageWithoutDroppingTheNextCursor)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Leaf(L"one") }, 8, L"next1")).result);
	ASSERT_TRUE(model.Select(L"one"));
	const auto next = Begin(model, L"", TreeLoadKind::NextPage); EXPECT_EQ(L"next1", next.cursor);
	ASSERT_EQ(TreeResult::Applied, model.Apply(next, Page(L"", {}, 8, L"next2")).result);
	EXPECT_EQ(TreeChildrenState::Partial, model.Node(L"")->state); EXPECT_EQ(L"one", model.Selection());
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"two") }, 8)).result);
	EXPECT_EQ((std::vector<std::wstring>{ L"one", L"two" }), model.Node(L"")->children);
	EXPECT_EQ(TreeChildrenState::Complete, model.Node(L"")->state);
	EXPECT_EQ(CountBytes(model), model.RetainedBytes());
}
TEST(TreeViewModel, RefreshKeepsStableSelectionAndUserExpansionAndRemovesOldDescendants)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Branch(L"folder", true), Branch(L"removed", true) })).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"folder"), Page(L"folder", { Leaf(L"selected") })).result);
	const auto removedLoad = Begin(model, L"removed");
	ASSERT_TRUE(model.Select(L"selected"));
	auto folder = Branch(L"folder"); folder.label = L"Renamed folder";
	const auto refreshed = model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { folder }, 2));
	ASSERT_EQ(TreeResult::Applied, refreshed.result);
	EXPECT_EQ(std::vector<std::uint64_t>{ removedLoad.ticket }, refreshed.cancelled);
	EXPECT_FALSE(model.Node(L"removed")); EXPECT_EQ(L"selected", model.Selection());
	EXPECT_TRUE(model.Node(L"folder")->expanded); EXPECT_EQ(L"Renamed folder", model.Node(L"folder")->item.label);
	EXPECT_EQ(CountBytes(model), model.RetainedBytes());
	EXPECT_EQ(TreeResult::Stale, model.Apply(removedLoad, Page(L"removed", { Leaf(L"late") })).result);
}
TEST(TreeViewModel, RejectsDuplicateForeignAncestorAndStalePagesAtomically)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Branch(L"folder", true), Leaf(L"sibling") }, 5, L"n")).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"folder"), Page(L"folder", { Leaf(L"child") }, 5)).result);
	for (const auto& id : { L"folder", L"sibling" }) {
		const auto result = model.Apply(Begin(model, L"folder", TreeLoadKind::Refresh), Page(L"folder", { Leaf(id) }, 6));
		EXPECT_EQ(TreeResult::Invalid, result.result); EXPECT_TRUE(model.Node(L"child")); EXPECT_EQ(3, model.ItemCount());
	}
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"folder", TreeLoadKind::Refresh), Page(L"folder", { Leaf(L"x"), Leaf(L"x") }, 6)).result);
	EXPECT_FALSE(model.Node(L"x"));
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"sibling") }, 5)).result);
	EXPECT_EQ(TreeResult::Stale, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"new") }, 6)).result);
	EXPECT_EQ(TreeLoadKind::Refresh, model.Node(L"")->retryKind);
	EXPECT_EQ(TreeResult::Stale, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Leaf(L"new") }, 4)).result);
	EXPECT_FALSE(model.Node(L"new")); EXPECT_EQ(0, model.PendingCount());
	EXPECT_EQ(CountBytes(model), model.RetainedBytes());
}
TEST(TreeViewModel, PageFailureAndRepeatedCursorPreserveTheLastSuccessfulSnapshot)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Leaf(L"old") }, 2, L"cursor")).result);
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"new") }, 2, L"cursor")).result);
	EXPECT_FALSE(model.Node(L"new")); EXPECT_TRUE(model.Node(L"old"));
	auto failed = Page(L"", {}, 2); failed.state = TreePageState::Failed; failed.message = L"Network failed";
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), std::move(failed)).result);
	EXPECT_EQ(TreeChildrenState::Failed, model.Node(L"")->state); EXPECT_TRUE(model.Node(L"")->hasSnapshot);
	EXPECT_EQ(L"Network failed", model.Node(L"")->message); EXPECT_TRUE(model.Demand().empty());
	EXPECT_EQ(CountBytes(model), model.RetainedBytes());
}
TEST(TreeViewModel, CollapseCancelsSubtreeLoadsAndMovesHiddenSelectionToItsParent)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Branch(L"outer", true) })).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"outer"), Page(L"outer", { Branch(L"inner", true) })).result);
	const auto load = Begin(model, L"inner"); ASSERT_TRUE(model.Select(L"inner"));
	const auto collapsed = model.SetExpanded(L"outer", false);
	EXPECT_EQ(std::vector<std::uint64_t>{ load.ticket }, collapsed.cancelled);
	EXPECT_EQ(L"outer", model.Selection()); EXPECT_EQ(0, model.PendingCount());
	EXPECT_FALSE(model.Select(L"inner")); EXPECT_TRUE(model.Demand().empty());
	EXPECT_EQ(TreeResult::Stale, model.Apply(load, Page(L"inner", { Leaf(L"late") })).result);
	ASSERT_EQ(TreeResult::Applied, model.SetExpanded(L"outer", true).result);
	EXPECT_EQ(std::vector<std::wstring>{ L"inner" }, model.Demand());
}
TEST(TreeViewModel, InvalidateAndCloseFenceEveryOldTicket)
{
	TreeViewModel model;
	const auto old = Begin(model);
	EXPECT_EQ(std::vector<std::uint64_t>{ old.ticket }, model.Invalidate());
	const auto fresh = Begin(model); EXPECT_NE(old.generation, fresh.generation); EXPECT_NE(old.ticket, fresh.ticket);
	EXPECT_EQ(TreeResult::Stale, model.Apply(old, Page(L"", { Leaf(L"stale") })).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(fresh, Page(L"", { Branch(L"folder", true) })).result);
	const auto child = Begin(model, L"folder"); EXPECT_EQ(std::vector<std::uint64_t>{ child.ticket }, model.Close());
	EXPECT_EQ(TreeResult::Stale, model.Fail(child, L"late failure"));
	EXPECT_EQ(TreeResult::Unavailable, model.Begin(L"", TreeLoadKind::Refresh).result);
	EXPECT_EQ(TreeChildrenState::Stopped, model.Node(L"")->state); EXPECT_EQ(0, model.ItemCount());
	EXPECT_TRUE(model.Close().empty()); EXPECT_EQ(0, model.RetainedBytes());
}
TEST(TreeViewModel, LimitsConcurrentLoadsAndDepthWithoutHidingAnAdmittedRequest)
{
	TreeViewModel model;
	std::vector<TreeItem> branches; for (int i = 0; i < 9; ++i) branches.push_back(Branch(L"branch" + std::to_wstring(i), true));
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", std::move(branches))).result);
	for (int i = 0; i < 8; ++i) (void)Begin(model, L"branch" + std::to_wstring(i));
	EXPECT_EQ(TreeResult::Busy, model.Begin(L"branch0", TreeLoadKind::Initial).result);
	EXPECT_EQ(TreeResult::Busy, model.Begin(L"branch8", TreeLoadKind::Initial).result);
	EXPECT_EQ(8, model.CancelAll().size()); EXPECT_EQ(0, model.PendingCount());
	std::wstring parent;
	TreeViewModel deep;
	for (std::size_t i = 0; i < kMaximumTreeDepth; ++i) {
		const auto id = L"depth" + std::to_wstring(i);
		ASSERT_EQ(TreeResult::Applied, deep.Apply(Begin(deep, parent), Page(parent, { Branch(id, true) })).result);
		parent = id;
	}
	EXPECT_EQ(TreeResult::LimitExceeded, deep.Apply(Begin(deep, parent), Page(parent, { Leaf(L"too-deep") })).result);
	EXPECT_EQ(kMaximumTreeDepth, deep.ItemCount()); EXPECT_EQ(0, deep.PendingCount());
}
TEST(TreeViewModel, ItemLimitAllowsReplacingTheOldTreeAndByteBudgetAccountsForMetadata)
{
	TreeViewModel model;
	for (int page = 0; page < 8; ++page) {
		std::vector<TreeItem> items;
		for (int i = 0; i < 250; ++i) items.push_back(Leaf(L"old" + std::to_wstring(page * 250 + i)));
		ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", page ? TreeLoadKind::NextPage : TreeLoadKind::Initial),
			Page(L"", std::move(items), 1, L"cursor" + std::to_wstring(page))).result);
	}
	EXPECT_EQ(kMaximumTreeItems, model.ItemCount());
	EXPECT_EQ(TreeResult::LimitExceeded, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"overflow") })).result);
	EXPECT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Leaf(L"replacement") }, 2)).result);
	EXPECT_EQ(1, model.ItemCount()); EXPECT_FALSE(model.Node(L"old0")); EXPECT_EQ(CountBytes(model), model.RetainedBytes());
	std::vector<TreeItem> heavy;
	for (int i = 0; i < 70; ++i) {
		auto item = Leaf(L"heavy" + std::to_wstring(i)); item.commandId = L"test.open"; item.arguments.assign(16, std::wstring(4096, L'x')); heavy.push_back(std::move(item));
	}
	EXPECT_EQ(TreeResult::LimitExceeded, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", std::move(heavy), 3)).result);
	EXPECT_TRUE(model.Node(L"replacement")); EXPECT_LE(model.RetainedBytes(), kMaximumTreeBytes);
}
TEST(TreeViewModel, BranchToLeafCancelsChildrenAndLeafToBranchAppliesItsInitialExpansion)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Branch(L"a", true) })).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"a"), Page(L"a", { Branch(L"b", true) }, 1, L"c")).result);
	const auto b = Begin(model, L"b");
	const auto changed = model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Leaf(L"a") }, 2));
	EXPECT_EQ(TreeResult::Applied, changed.result); EXPECT_EQ(std::vector<std::uint64_t>{ b.ticket }, changed.cancelled);
	EXPECT_FALSE(model.Node(L"b")); EXPECT_FALSE(model.Node(L"a")->expanded); EXPECT_EQ(CountBytes(model), model.RetainedBytes());
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Branch(L"a", true) }, 3)).result);
	EXPECT_TRUE(model.Node(L"a")->expanded); EXPECT_EQ(std::vector<std::wstring>{ L"a" }, model.Demand());
}
TEST(TreeViewModel, MalformedTextAndUnknownStateNeverReachTheNativeControl)
{
	TreeViewModel model;
	for (int shape = 0; shape < 5; ++shape) {
		auto item = Leaf(L"test");
		if (shape == 0) item.label = std::wstring(L"a\0b", 3);
		if (shape == 1) item.tooltip.assign(1, static_cast<wchar_t>(0xd800));
		if (shape == 2) item.label.clear();
		if (shape == 3) item.arguments = { L"undeclared" };
		if (shape == 4) item.collapsibleState = static_cast<TreeItemCollapsibleState>(255);
		EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { item })).result);
		EXPECT_EQ(0, model.ItemCount()); EXPECT_EQ(0, model.PendingCount());
	}
}

class TreeRuntimeProbe final : public ISenpTreeRuntime {
public:
	bool current{ true }, executeResult{ true }, invalidScope{}, delivering{};
	senp::AdmissionStatus admission{ senp::AdmissionStatus::Accepted };
	std::int64_t nextGeneration{};
	struct Call { senp::effect::TreeRequest request; senp::effect::OperationContext context; SenpTreeProvider::Time deadline; };
	std::vector<Call> submitted;
	std::vector<senp::effect::OperationContext> cancelled;
	std::vector<senp::effect::CommandInvoked> executed;
	bool IsCurrent() const noexcept override { return current; }
	bool CanSubmit() const noexcept override { return current && !delivering; }
	SenpTreeAdmission Submit(senp::effect::TreeRequest request, SenpTreeProvider::Time deadline) noexcept override
	{
		const auto generation = ++nextGeneration;
		senp::effect::OperationContext context{ L"s1:o" + std::to_wstring(generation), invalidScope ? 99 : 1, 2, 3, generation };
		submitted.push_back({ std::move(request), context, deadline }); return { admission, std::move(context) };
	}
	void Cancel(const senp::effect::OperationContext& context) noexcept override { cancelled.push_back(context); }
	bool Execute(senp::effect::CommandInvoked command) noexcept override { executed.push_back(std::move(command)); return executeResult; }
};
class SenpTreeProviderTest : public ::testing::Test {
protected:
	std::shared_ptr<TreeRuntimeProbe> runtime = std::make_shared<TreeRuntimeProbe>();
	SenpTreeProvider provider{ { L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime } };
	const SenpTreeProvider::Time now{};
	senp::effect::PublishTreePage Wire(std::wstring parent = L"", bool branch = false)
	{
		senp::effect::TreeItem item; item.id = L"item"; item.label = L"Actual item"; item.description = L"Description";
		item.commandId = L"test.open"; item.arguments = { L"detail" };
		item.collapsibleState = branch ? senp::effect::CollapsibleState::Collapsed : senp::effect::CollapsibleState::Leaf;
		return { L"test.tree", std::move(parent), { item }, L"", 1, senp::effect::PageStatus::Complete, L"" };
	}
};
TEST_F(SenpTreeProviderTest, VisibilityAdmissionAndDerivedOperationPageUseOneRequestGeneration)
{
	provider.Pump(now); EXPECT_TRUE(runtime->submitted.empty());
	provider.SetVisible(true, now); ASSERT_EQ(1, runtime->submitted.size());
	for (int i = 0; i < 20; ++i) provider.Pump(now);
	EXPECT_EQ(1, runtime->submitted.size());
	auto context = runtime->submitted[0].context; context.operationId = L"s1:o999";
	ASSERT_EQ(TreeResult::Applied, provider.Apply(context, Wire(), now));
	EXPECT_EQ(1, provider.Model().ItemCount()); EXPECT_FALSE(provider.NextDeadline());
	provider.SetVisible(false, now); provider.SetVisible(true, now);
	EXPECT_EQ(1, runtime->submitted.size());
	EXPECT_TRUE(provider.Execute(L"item")); ASSERT_EQ(1, runtime->executed.size()); EXPECT_EQ(L"detail", runtime->executed[0].arguments[0]);
}
TEST_F(SenpTreeProviderTest, HideAndRefreshCancelOldSubscriberAndFenceItsLatePage)
{
	provider.SetVisible(true, now); const auto old = runtime->submitted[0].context;
	provider.SetVisible(false, now); EXPECT_EQ(1, runtime->cancelled.size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(old, Wire(), now));
	provider.SetVisible(true, now); ASSERT_EQ(2, runtime->submitted.size());
	const auto fresh = runtime->submitted[1].context;
	provider.Refresh(now); ASSERT_EQ(3, runtime->submitted.size()); EXPECT_EQ(2, runtime->cancelled.size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(fresh, Wire(), now));
	EXPECT_EQ(TreeResult::Applied, provider.Apply(runtime->submitted.back().context, Wire(), now));
	EXPECT_EQ(1, provider.Model().ItemCount());
}
TEST_F(SenpTreeProviderTest, DemandRaisedWhileResultsAreDeliveredWaitsForTheNextPump)
{
	provider.SetVisible(true, now); ASSERT_EQ(1, runtime->submitted.size());
	ASSERT_EQ(TreeResult::Applied, provider.Apply(runtime->submitted[0].context, Wire(), now));
	runtime->delivering = true;
	provider.Refresh(now);
	EXPECT_EQ(1, runtime->submitted.size());
	EXPECT_NE(TreeChildrenState::Failed, provider.Model().Node(L"")->state);
	runtime->delivering = false;
	provider.Pump(now); ASSERT_EQ(2, runtime->submitted.size());
	EXPECT_EQ(TreeResult::Applied, provider.Apply(runtime->submitted[1].context, Wire(), now));
	EXPECT_EQ(1, provider.Model().ItemCount());
}
TEST_F(SenpTreeProviderTest, BusyAndDeadlineAreTerminalUntilExplicitRetry)
{
	runtime->admission = senp::AdmissionStatus::Busy; provider.SetVisible(true, now);
	EXPECT_EQ(TreeChildrenState::Failed, provider.Model().Node(L"")->state); EXPECT_FALSE(provider.NextDeadline());
	for (int i = 0; i < 50; ++i) provider.Pump(now);
	EXPECT_EQ(1, runtime->submitted.size());
	runtime->admission = senp::AdmissionStatus::Accepted; EXPECT_EQ(TreeResult::Accepted, provider.Retry(L"", now));
	ASSERT_TRUE(provider.NextDeadline()); const auto context = runtime->submitted.back().context;
	provider.Pump(now + SenpTreeProvider::kLoadLifetime);
	EXPECT_FALSE(provider.NextDeadline()); EXPECT_EQ(0, provider.Model().PendingCount()); EXPECT_EQ(1, runtime->cancelled.size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(context, Wire(), now + SenpTreeProvider::kLoadLifetime));
	EXPECT_EQ(2, runtime->submitted.size());
}
TEST_F(SenpTreeProviderTest, ScopeAndCommandValidationRejectBeforePublishingAndRevocationClearsRows)
{
	provider.SetVisible(true, now); auto context = runtime->submitted[0].context;
	for (int field = 0; field < 3; ++field) {
		auto old = context;
		if (field == 0) ++old.ownerGeneration; if (field == 1) ++old.workspaceRevision; if (field == 2) ++old.accountGeneration;
		EXPECT_EQ(TreeResult::Stale, provider.Apply(old, Wire(), now)); EXPECT_EQ(0, provider.Model().ItemCount());
	}
	auto forbidden = Wire(); forbidden.items[0].commandId = L"foreign.open";
	EXPECT_EQ(TreeResult::Invalid, provider.Apply(context, forbidden, now)); EXPECT_EQ(0, provider.Model().PendingCount());
	ASSERT_EQ(TreeResult::Accepted, provider.Retry(L"", now));
	ASSERT_EQ(TreeResult::Applied, provider.Apply(runtime->submitted.back().context, Wire(), now));
	runtime->current = false;
	EXPECT_FALSE(provider.Select(L"item")); EXPECT_TRUE(provider.Model().IsClosed()); EXPECT_EQ(0, provider.Model().ItemCount());
	EXPECT_FALSE(provider.Execute(L"item"));
}
TEST_F(SenpTreeProviderTest, NativeAdmissionMismatchCancelsTheAcceptedRequest)
{
	runtime->invalidScope = true; provider.SetVisible(true, now);
	EXPECT_EQ(1, runtime->cancelled.size()); EXPECT_EQ(0, provider.Model().PendingCount());
	EXPECT_EQ(TreeChildrenState::Failed, provider.Model().Node(L"")->state); EXPECT_FALSE(provider.NextDeadline());
}
TEST_F(SenpTreeProviderTest, CollapsingPendingChildrenRetainsTheParentAndRejectsLateResults)
{
	provider.SetVisible(true, now); ASSERT_EQ(TreeResult::Applied, provider.Apply(runtime->submitted[0].context, Wire(L"", true), now));
	EXPECT_EQ(1, runtime->submitted.size());
	ASSERT_EQ(TreeResult::Applied, provider.SetExpanded(L"item", true, now)); ASSERT_EQ(2, runtime->submitted.size());
	EXPECT_EQ(L"item", runtime->submitted[1].request.parentId);
	const auto child = runtime->submitted[1].context;
	ASSERT_EQ(TreeResult::Applied, provider.SetExpanded(L"item", false, now)); EXPECT_EQ(1, runtime->cancelled.size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(child, Wire(L"item"), now)); EXPECT_EQ(1, provider.Model().ItemCount());
}
TEST_F(SenpTreeProviderTest, RuntimeFailureDoesNotBecomeEmptyOrRestartItself)
{
	provider.SetVisible(true, now); const auto context = runtime->submitted[0].context;
	EXPECT_EQ(TreeResult::Applied, provider.Failed(context, senp::InvocationStatus::HostUnavailable, now));
	EXPECT_EQ(TreeChildrenState::Failed, provider.Model().Node(L"")->state);
	EXPECT_FALSE(provider.Model().Node(L"")->hasSnapshot); EXPECT_FALSE(provider.NextDeadline());
	provider.Pump(now); EXPECT_EQ(1, runtime->submitted.size()); EXPECT_EQ(1, runtime->cancelled.size());
	provider.Close(); provider.Close(); EXPECT_TRUE(provider.Model().IsClosed());
}
}
} // namespace workbench::tree
