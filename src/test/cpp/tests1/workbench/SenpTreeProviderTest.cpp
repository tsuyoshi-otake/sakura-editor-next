/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "workbench/tree/SenpTreeProvider.h"
#include "senp/SenpEffectProtocol.h"
#include <sakura/serialization/JsoncDocument.h>
#include <filesystem>
#include <fstream>
#include <set>
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
		+ value.item.icon.size() + value.item.commandId.size() + value.item.ContextValue().size() + value.parentId.size()
		+ value.nextCursor.size() + value.message.size();
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
TEST(TreeViewModel, AppendDropsAnOffsetShiftDuplicateAndKeepsTheRetainedPayload)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Leaf(L"a"), Leaf(L"b"), Leaf(L"c") }, 7, L"n1")).result);
	// An item created or closed between two offset fetches repeats the boundary
	// row on the next page; the appended copy is dropped and the retained node wins.
	auto shifted = Leaf(L"c"); shifted.label = L"c retitled";
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { shifted, Leaf(L"d") }, 7)).result);
	EXPECT_EQ((std::vector<std::wstring>{ L"a", L"b", L"c", L"d" }), model.Node(L"")->children);
	EXPECT_EQ(L"c", model.Node(L"c")->item.label);
	EXPECT_EQ(4, model.ItemCount());
	EXPECT_EQ(TreeChildrenState::Complete, model.Node(L"")->state);
	EXPECT_EQ(CountBytes(model), model.RetainedBytes());
}
TEST(TreeViewModel, AppendOfOnlyRetainedIdsStillAdvancesTheCursor)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Leaf(L"a"), Leaf(L"b") }, 3, L"n1")).result);
	// A whole page of retained ids carries no new rows, but its next cursor
	// still commits so Load more keeps making progress.
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"a"), Leaf(L"b") }, 3, L"n2")).result);
	EXPECT_EQ(2, model.ItemCount());
	EXPECT_EQ((std::vector<std::wstring>{ L"a", L"b" }), model.Node(L"")->children);
	EXPECT_EQ(L"n2", model.Node(L"")->nextCursor);
	EXPECT_EQ(TreeChildrenState::Partial, model.Node(L"")->state);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"c") }, 3)).result);
	EXPECT_EQ((std::vector<std::wstring>{ L"a", L"b", L"c" }), model.Node(L"")->children);
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
	EXPECT_EQ(std::vector<std::uint64_t>{ removedLoad.ticket }, refreshed.Cancelled());
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
	// An appended page may repeat a retained direct child of the same parent:
	// offset paging re-reads the boundary row, so the copy is dropped (#351).
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"sibling") }, 5, L"n2")).result);
	EXPECT_EQ((std::vector<std::wstring>{ L"folder", L"sibling" }), model.Node(L"")->children);
	EXPECT_EQ(3, model.ItemCount());
	// The same id retained under another parent is still a reparenting attempt
	// and fails the appended page atomically.
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", { Leaf(L"child") }, 5, L"n3")).result);
	EXPECT_EQ(L"folder", model.Node(L"child")->parentId);
	EXPECT_EQ(3, model.ItemCount());
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
TEST(TreeViewModel, HostRejectionsRecordTypedFailureReasons)
{
	TreeViewModel model;
	const auto reason = [&] { return model.Node(L"")->reason; };
	auto fail = [&](std::wstring_view message) {
		auto page = Page(L"", {}, 1); page.state = TreePageState::Failed; page.message = std::wstring(message);
		return page;
	};
	// A provider message is preserved verbatim and its reason names the provider.
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), fail(L"rate-limited")).result);
	EXPECT_EQ(TreeFailureReason::ProviderMessage, reason()); EXPECT_EQ(L"rate-limited", model.Node(L"")->message);
	// An empty provider message is the host's generic load failure, not verbatim text.
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), fail(L"")).result);
	EXPECT_EQ(TreeFailureReason::LoadFailed, reason()); EXPECT_TRUE(model.Node(L"")->message.empty());
	// A malformed page is a typed rejection, not a provider sentence.
	{
		auto bad = Page(L"other", {}, 1);
		EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), std::move(bad)).result);
		EXPECT_EQ(TreeFailureReason::InvalidPage, reason());
	}
}
TEST(TreeViewModel, EveryRejectedPathNamesItsOwnReason)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Branch(L"folder", true), Leaf(L"sibling") }, 5, L"n")).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"folder"), Page(L"folder", { Leaf(L"child") }, 5)).result);
	const auto reason = [&](std::wstring_view parent) { return model.Node(parent)->reason; };
	// Stale: a lower revision than the retained listing.
	EXPECT_EQ(TreeResult::Stale, model.Apply(Begin(model, L"folder", TreeLoadKind::Refresh), Page(L"folder", {}, 4)).result);
	EXPECT_EQ(TreeFailureReason::StaleListing, reason(L"folder"));
	// RepeatedCursor: the next page cursor repeats the request's own cursor.
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::NextPage), Page(L"", {}, 5, L"n")).result);
	EXPECT_EQ(TreeFailureReason::RepeatedCursor, reason(L""));
	// DuplicateIdentity: one id repeated inside a single page.
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Leaf(L"x"), Leaf(L"x") }, 6)).result);
	EXPECT_EQ(TreeFailureReason::DuplicateIdentity, reason(L""));
	// InvalidPage: an item that fails validation is not an identity conflict.
	{
		auto blank = Leaf(L"blank"); blank.label.clear();
		EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { blank }, 6)).result);
		EXPECT_EQ(TreeFailureReason::InvalidPage, reason(L""));
	}
	// MovedIdentity: an id already retained under another parent.
	EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Leaf(L"child") }, 6)).result);
	EXPECT_EQ(TreeFailureReason::MovedIdentity, reason(L""));
	// InvalidPage: a structurally malformed page (items on an Empty page).
	{
		auto bad = Page(L"", { Leaf(L"orphan") }, 6); bad.state = TreePageState::Empty;
		EXPECT_EQ(TreeResult::Invalid, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), std::move(bad)).result);
		EXPECT_EQ(TreeFailureReason::InvalidPage, reason(L""));
	}
	// LimitReached: a child admitted past the depth bound.
	{
		TreeViewModel deep;
		std::wstring parent;
		for (std::size_t i = 0; i < kMaximumTreeDepth; ++i) {
			const auto id = L"depth" + std::to_wstring(i);
			ASSERT_EQ(TreeResult::Applied, deep.Apply(Begin(deep, parent), Page(parent, { Branch(id, true) })).result);
			parent = id;
		}
		EXPECT_EQ(TreeResult::LimitExceeded, deep.Apply(Begin(deep, parent), Page(parent, { Leaf(L"too-deep") })).result);
		EXPECT_EQ(TreeFailureReason::LimitReached, deep.Node(parent)->reason);
	}
}
TEST(TreeViewModel, ProviderReasonAlwaysCarriesTextAndBeginClearsTheFailure)
{
	TreeViewModel model;
	const auto root = [&] { return *model.Node(L""); };
	// A valid provider message is kept verbatim under the provider reason.
	ASSERT_EQ(TreeResult::Applied, model.Fail(Begin(model), TreeFailureReason::ProviderMessage, L"Sign in to continue."));
	EXPECT_EQ(TreeFailureReason::ProviderMessage, root().reason); EXPECT_EQ(L"Sign in to continue.", root().message);
	// An invalid provider message is dropped and the failure becomes a plain load failure.
	ASSERT_EQ(TreeResult::Applied, model.Fail(Begin(model), TreeFailureReason::ProviderMessage, std::wstring(4097, L'x')));
	EXPECT_EQ(TreeChildrenState::Failed, root().state);
	EXPECT_EQ(TreeFailureReason::LoadFailed, root().reason); EXPECT_TRUE(root().message.empty());
	// A Failed page without a message is the same host reason.
	auto failed = Page(L"", {}); failed.state = TreePageState::Failed;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), std::move(failed)).result);
	EXPECT_EQ(TreeFailureReason::LoadFailed, root().reason); EXPECT_TRUE(root().message.empty());
	// A retry clears the recorded failure: a loading node carries no stale reason.
	ASSERT_EQ(TreeResult::Applied, model.Fail(Begin(model), TreeFailureReason::RequestTimedOut));
	ASSERT_EQ(TreeFailureReason::RequestTimedOut, root().reason);
	const auto retry = Begin(model);
	EXPECT_EQ(TreeChildrenState::Loading, root().state);
	EXPECT_EQ(TreeFailureReason::ProviderMessage, root().reason); EXPECT_TRUE(root().message.empty());
	(void)retry;
}
TEST(TreeViewModel, CollapseCancelsSubtreeLoadsAndMovesHiddenSelectionToItsParent)
{
	TreeViewModel model;
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model), Page(L"", { Branch(L"outer", true) })).result);
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"outer"), Page(L"outer", { Branch(L"inner", true) })).result);
	const auto load = Begin(model, L"inner"); ASSERT_TRUE(model.Select(L"inner"));
	const auto collapsed = model.SetExpanded(L"outer", false);
	EXPECT_EQ(std::vector<std::uint64_t>{ load.ticket }, collapsed.Cancelled());
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
	EXPECT_EQ(TreeResult::Stale, model.Fail(child, TreeFailureReason::ProviderMessage, L"late failure"));
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
	EXPECT_EQ(TreeResult::Applied, changed.result); EXPECT_EQ(std::vector<std::uint64_t>{ b.ticket }, changed.Cancelled());
	EXPECT_FALSE(model.Node(L"b")); EXPECT_FALSE(model.Node(L"a")->expanded); EXPECT_EQ(CountBytes(model), model.RetainedBytes());
	ASSERT_EQ(TreeResult::Applied, model.Apply(Begin(model, L"", TreeLoadKind::Refresh), Page(L"", { Branch(L"a", true) }, 3)).result);
	EXPECT_TRUE(model.Node(L"a")->expanded); EXPECT_EQ(std::vector<std::wstring>{ L"a" }, model.Demand());
}
TEST(TreeViewModel, MalformedTextAndUnknownStateNeverReachTheNativeControl)
{
	TreeViewModel model;
	for (int shape = 0; shape < 7; ++shape) {
		auto item = Leaf(L"test");
		if (shape == 0) item.label = std::wstring(L"a\0b", 3);
		if (shape == 1) item.tooltip.assign(1, static_cast<wchar_t>(0xd800));
		if (shape == 2) item.label.clear();
		if (shape == 3) item.arguments = { L"undeclared" };
		if (shape == 4) item.collapsibleState = static_cast<TreeItemCollapsibleState>(255);
		if (shape == 5) item.SetContextValue(std::wstring(1025, L'x'));
		if (shape == 6) item.SetContextValue(std::wstring(L"job\0x", 5));
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
	[[nodiscard]] const std::vector<Call>& Submitted() const noexcept { return m_submitted; }
	[[nodiscard]] const std::vector<senp::effect::OperationContext>& Cancelled() const noexcept { return m_cancelled; }
	[[nodiscard]] const std::vector<senp::effect::CommandInvoked>& Executed() const noexcept { return m_executed; }
	bool IsCurrent() const noexcept override { return current; }
	bool CanSubmit() const noexcept override { return current && !delivering; }
	SenpTreeAdmission Submit(senp::effect::TreeRequest request, SenpTreeProvider::Time deadline) noexcept override
	{
		const auto generation = ++nextGeneration;
		senp::effect::OperationContext context{ L"s1:o" + std::to_wstring(generation), invalidScope ? 99 : 1, 2, 3, generation };
		m_submitted.push_back({ std::move(request), context, deadline }); return { admission, std::move(context) };
	}
	void Cancel(const senp::effect::OperationContext& context) noexcept override { m_cancelled.push_back(context); }
	bool Execute(senp::effect::CommandInvoked command) noexcept override { m_executed.push_back(std::move(command)); return executeResult; }
private:
	std::vector<Call> m_submitted;
	std::vector<senp::effect::OperationContext> m_cancelled;
	std::vector<senp::effect::CommandInvoked> m_executed;
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
	provider.Pump(now); EXPECT_TRUE(runtime->Submitted().empty());
	provider.SetVisible(true, now); ASSERT_EQ(1, runtime->Submitted().size());
	for (int i = 0; i < 20; ++i) provider.Pump(now);
	EXPECT_EQ(1, runtime->Submitted().size());
	auto context = runtime->Submitted()[0].context; context.operationId = L"s1:o999";
	ASSERT_EQ(TreeResult::Applied, provider.Apply(context, Wire(), now));
	EXPECT_EQ(1, provider.Model().ItemCount()); EXPECT_FALSE(provider.NextDeadline());
	provider.SetVisible(false, now); provider.SetVisible(true, now);
	EXPECT_EQ(1, runtime->Submitted().size());
	EXPECT_TRUE(provider.Execute(L"item")); ASSERT_EQ(1, runtime->Executed().size()); EXPECT_EQ(L"detail", runtime->Executed()[0].arguments[0]);
}
TEST_F(SenpTreeProviderTest, HideAndRefreshCancelOldSubscriberAndFenceItsLatePage)
{
	provider.SetVisible(true, now); const auto old = runtime->Submitted()[0].context;
	provider.SetVisible(false, now); EXPECT_EQ(1, runtime->Cancelled().size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(old, Wire(), now));
	provider.SetVisible(true, now); ASSERT_EQ(2, runtime->Submitted().size());
	const auto fresh = runtime->Submitted()[1].context;
	provider.Refresh(now); ASSERT_EQ(3, runtime->Submitted().size()); EXPECT_EQ(2, runtime->Cancelled().size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(fresh, Wire(), now));
	EXPECT_EQ(TreeResult::Applied, provider.Apply(runtime->Submitted().back().context, Wire(), now));
	EXPECT_EQ(1, provider.Model().ItemCount());
}
TEST_F(SenpTreeProviderTest, DemandRaisedWhileResultsAreDeliveredWaitsForTheNextPump)
{
	provider.SetVisible(true, now); ASSERT_EQ(1, runtime->Submitted().size());
	ASSERT_EQ(TreeResult::Applied, provider.Apply(runtime->Submitted()[0].context, Wire(), now));
	runtime->delivering = true;
	provider.Refresh(now);
	EXPECT_EQ(1, runtime->Submitted().size());
	EXPECT_NE(TreeChildrenState::Failed, provider.Model().Node(L"")->state);
	runtime->delivering = false;
	provider.Pump(now); ASSERT_EQ(2, runtime->Submitted().size());
	EXPECT_EQ(TreeResult::Applied, provider.Apply(runtime->Submitted()[1].context, Wire(), now));
	EXPECT_EQ(1, provider.Model().ItemCount());
}
TEST_F(SenpTreeProviderTest, BusyAndDeadlineAreTerminalUntilExplicitRetry)
{
	runtime->admission = senp::AdmissionStatus::Busy; provider.SetVisible(true, now);
	EXPECT_EQ(TreeChildrenState::Failed, provider.Model().Node(L"")->state); EXPECT_FALSE(provider.NextDeadline());
	for (int i = 0; i < 50; ++i) provider.Pump(now);
	EXPECT_EQ(1, runtime->Submitted().size());
	runtime->admission = senp::AdmissionStatus::Accepted; EXPECT_EQ(TreeResult::Accepted, provider.Retry(L"", now));
	ASSERT_TRUE(provider.NextDeadline()); const auto context = runtime->Submitted().back().context;
	provider.Pump(now + SenpTreeProvider::kLoadLifetime);
	EXPECT_FALSE(provider.NextDeadline()); EXPECT_EQ(0, provider.Model().PendingCount()); EXPECT_EQ(1, runtime->Cancelled().size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(context, Wire(), now + SenpTreeProvider::kLoadLifetime));
	EXPECT_EQ(2, runtime->Submitted().size());
}
TEST_F(SenpTreeProviderTest, ScopeAndCommandValidationRejectBeforePublishingAndRevocationClearsRows)
{
	provider.SetVisible(true, now); auto context = runtime->Submitted()[0].context;
	for (int field = 0; field < 3; ++field) {
		auto old = context;
		if (field == 0) ++old.ownerGeneration; if (field == 1) ++old.workspaceRevision; if (field == 2) ++old.accountGeneration;
		EXPECT_EQ(TreeResult::Stale, provider.Apply(old, Wire(), now)); EXPECT_EQ(0, provider.Model().ItemCount());
	}
	auto forbidden = Wire(); forbidden.items[0].commandId = L"foreign.open";
	EXPECT_EQ(TreeResult::Invalid, provider.Apply(context, forbidden, now)); EXPECT_EQ(0, provider.Model().PendingCount());
	ASSERT_EQ(TreeResult::Accepted, provider.Retry(L"", now));
	ASSERT_EQ(TreeResult::Applied, provider.Apply(runtime->Submitted().back().context, Wire(), now));
	runtime->current = false;
	EXPECT_FALSE(provider.Select(L"item")); EXPECT_TRUE(provider.Model().IsClosed()); EXPECT_EQ(0, provider.Model().ItemCount());
	EXPECT_FALSE(provider.Execute(L"item"));
}
TEST_F(SenpTreeProviderTest, NativeAdmissionMismatchCancelsTheAcceptedRequest)
{
	runtime->invalidScope = true; provider.SetVisible(true, now);
	EXPECT_EQ(1, runtime->Cancelled().size()); EXPECT_EQ(0, provider.Model().PendingCount());
	EXPECT_EQ(TreeChildrenState::Failed, provider.Model().Node(L"")->state); EXPECT_FALSE(provider.NextDeadline());
}
TEST_F(SenpTreeProviderTest, CollapsingPendingChildrenRetainsTheParentAndRejectsLateResults)
{
	provider.SetVisible(true, now); ASSERT_EQ(TreeResult::Applied, provider.Apply(runtime->Submitted()[0].context, Wire(L"", true), now));
	EXPECT_EQ(1, runtime->Submitted().size());
	ASSERT_EQ(TreeResult::Applied, provider.SetExpanded(L"item", true, now)); ASSERT_EQ(2, runtime->Submitted().size());
	EXPECT_EQ(L"item", runtime->Submitted()[1].request.parentId);
	const auto child = runtime->Submitted()[1].context;
	ASSERT_EQ(TreeResult::Applied, provider.SetExpanded(L"item", false, now)); EXPECT_EQ(1, runtime->Cancelled().size());
	EXPECT_EQ(TreeResult::Stale, provider.Apply(child, Wire(L"item"), now)); EXPECT_EQ(1, provider.Model().ItemCount());
}
TEST_F(SenpTreeProviderTest, RuntimeFailureDoesNotBecomeEmptyOrRestartItself)
{
	provider.SetVisible(true, now); const auto context = runtime->Submitted()[0].context;
	EXPECT_EQ(TreeResult::Applied, provider.Failed(context, senp::InvocationStatus::HostUnavailable, now));
	EXPECT_EQ(TreeChildrenState::Failed, provider.Model().Node(L"")->state);
	EXPECT_FALSE(provider.Model().Node(L"")->hasSnapshot); EXPECT_FALSE(provider.NextDeadline());
	provider.Pump(now); EXPECT_EQ(1, runtime->Submitted().size()); EXPECT_EQ(1, runtime->Cancelled().size());
	provider.Close(); provider.Close(); EXPECT_TRUE(provider.Model().IsClosed());
}
TEST_F(SenpTreeProviderTest, InlineItemActionsMatchTheRowContextValueAndPassOnlyItsId)
{
	SenpTreeProvider rows{ { L"test.tree", { 1, 2, 3 }, { L"test.open", L"test.logs", L"test.rerun" }, runtime, {
		{ L"test.logs", L"View logs", L"resources/icons/light/logs.svg", { L"job", L"completed" }, {} },
		{ L"test.rerun", L"Re-run", L"$(sync)", {}, { L"job" } },
	} } };
	rows.SetVisible(true, now); ASSERT_EQ(1, runtime->Submitted().size());
	auto page = Wire(); page.items[0].contextValue = L"job completed";
	auto running = page.items[0]; running.id = L"running"; running.contextValue = L"job";
	auto plain = page.items[0]; plain.id = L"plain"; plain.contextValue.clear();
	page.items.push_back(running); page.items.push_back(plain);
	ASSERT_EQ(TreeResult::Applied, rows.Apply(runtime->Submitted()[0].context, page, now));
	// `=~` tokens are substrings of the contextValue; `==` compares all of it.
	const auto completed = rows.ItemActions(L"item");
	ASSERT_EQ(1, completed.size()); EXPECT_EQ(L"test.logs", completed[0].CommandId());
	const auto inProgress = rows.ItemActions(L"running");
	ASSERT_EQ(1, inProgress.size()); EXPECT_EQ(L"test.rerun", inProgress[0].CommandId());
	EXPECT_TRUE(rows.ItemActions(L"plain").empty());
	EXPECT_TRUE(rows.ItemActions(L"").empty()); EXPECT_TRUE(rows.ItemActions(L"missing").empty());
	EXPECT_FALSE(rows.ExecuteItemAction(L"running", L"test.logs"));
	EXPECT_FALSE(rows.ExecuteItemAction(L"item", L"test.open"));
	EXPECT_FALSE(rows.ExecuteItemAction(L"", L"test.logs"));
	EXPECT_TRUE(runtime->Executed().empty());
	ASSERT_TRUE(rows.ExecuteItemAction(L"item", L"test.logs"));
	ASSERT_EQ(1, runtime->Executed().size());
	EXPECT_EQ(L"test.logs", runtime->Executed()[0].commandId);
	EXPECT_EQ(std::vector<std::wstring>{ L"item" }, runtime->Executed()[0].arguments);
	EXPECT_EQ(L"item", rows.Model().Selection());
	rows.SetVisible(false, now);
	EXPECT_FALSE(rows.ExecuteItemAction(L"item", L"test.logs"));
	rows.SetVisible(true, now);
	runtime->current = false;
	EXPECT_FALSE(rows.ExecuteItemAction(L"item", L"test.logs"));
	EXPECT_EQ(1, runtime->Executed().size());
}
TEST_F(SenpTreeProviderTest, HostFailuresRecordTypedFailureReasons)
{
	// Busy and unavailable admissions name different reasons.
	runtime->admission = senp::AdmissionStatus::Busy; provider.SetVisible(true, now);
	EXPECT_EQ(TreeFailureReason::ProviderBusy, provider.Model().Node(L"")->reason);
	runtime->admission = senp::AdmissionStatus::Unavailable;
	SenpTreeProvider unavailable({ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
	unavailable.SetVisible(true, now);
	EXPECT_EQ(TreeFailureReason::ProviderUnavailable, unavailable.Model().Node(L"")->reason);
	unavailable.Close();
	// An out-of-scope admission context is an invalid request identity.
	runtime->admission = senp::AdmissionStatus::Accepted; runtime->invalidScope = true;
	SenpTreeProvider scoped({ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
	scoped.SetVisible(true, now);
	EXPECT_EQ(TreeFailureReason::InvalidRequestIdentity, scoped.Model().Node(L"")->reason);
	scoped.Close(); runtime->invalidScope = false;
	// A deadline exceeded request is a request timeout.
	SenpTreeProvider slow({ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
	slow.SetVisible(true, now);
	slow.Pump(now + SenpTreeProvider::kLoadLifetime);
	EXPECT_EQ(TreeFailureReason::RequestTimedOut, slow.Model().Node(L"")->reason);
	slow.Close();
	// A wire page the native model cannot convert is an invalid page.
	SenpTreeProvider invalid({ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
	invalid.SetVisible(true, now);
	auto bad = Wire(); bad.viewId = L"other.tree";
	EXPECT_EQ(TreeResult::Invalid, invalid.Apply(runtime->Submitted().back().context, std::move(bad), now));
	EXPECT_EQ(TreeFailureReason::InvalidPage, invalid.Model().Node(L"")->reason);
	invalid.Close();
	// Invocation terminals map to their own reasons.
	for (int i = 0; i < 3; ++i) {
		SenpTreeProvider failing({ L"test.tree", { 1, 2, 3 }, { L"test.open" }, runtime });
		failing.SetVisible(true, now);
		const auto context = runtime->Submitted().back().context;
		const auto status = i == 0 ? senp::InvocationStatus::TimedOut : i == 1 ? senp::InvocationStatus::Cancelled : senp::InvocationStatus::HostUnavailable;
		EXPECT_EQ(TreeResult::Applied, failing.Failed(context, status, now));
		EXPECT_EQ(i == 0 ? TreeFailureReason::ProviderTimedOut : i == 1 ? TreeFailureReason::Cancelled : TreeFailureReason::LoadFailed,
			failing.Model().Node(L"")->reason);
		failing.Close();
	}
}
TEST(SenpTreeProviderOptions, RejectsItemActionsARowCannotCarry)
{
	const auto runtime = std::make_shared<TreeRuntimeProbe>();
	const auto make = [&](std::vector<SenpTreeItemAction> actions) {
		std::vector<std::wstring> commands;
		for (int i = 0; i < 9; ++i) commands.push_back(L"test.act" + std::to_wstring(i));
		return std::make_unique<SenpTreeProvider>(SenpTreeProviderOptions{ L"test.tree", { 1, 2, 3 }, std::move(commands), runtime, std::move(actions) });
	};
	const auto action = [](int i) { return SenpTreeItemAction{ L"test.act" + std::to_wstring(i), L"Act", L"$(output)", { L"job" }, {} }; };
	std::vector<SenpTreeItemAction> eight;
	for (int i = 0; i < 8; ++i) eight.push_back(action(i));
	EXPECT_NO_THROW((void)make(eight));
	auto nine = eight; nine.push_back(action(8));
	EXPECT_THROW((void)make(nine), std::invalid_argument);
	auto undeclared = action(0); undeclared.SetCommandId(L"test.missing");
	EXPECT_THROW((void)make({ undeclared }), std::invalid_argument);
	EXPECT_THROW((void)make({ action(0), action(0) }), std::invalid_argument);
	auto iconless = action(0); iconless.SetIcon(L"");
	EXPECT_THROW((void)make({ iconless }), std::invalid_argument);
	auto emptyToken = action(0); emptyToken.SetEquals({ L"" });
	EXPECT_THROW((void)make({ emptyToken }), std::invalid_argument);
	auto unbounded = action(0); unbounded.SetContains(std::vector<std::wstring>(9, L"job"));
	EXPECT_THROW((void)make({ unbounded }), std::invalid_argument);
}

// The JSONL fixture is shared with the guest crates (rust/senp/fixtures). Each
// line is one scenario: the tool-completion inputs the guest consumes and the
// wire-shaped tree pages it is expected to publish, plus the retained child
// list after every page is applied in order.
std::filesystem::path TreePagingFixturePath()
{
	auto directory = std::filesystem::current_path();
	for (std::size_t level = 0; level < 8; ++level) {
		const auto path = directory / "rust/senp/fixtures/tree-paging.jsonl";
		if (std::filesystem::is_regular_file(path)) return path;
		const auto parent = directory.parent_path();
		if (parent == directory) break;
		directory = parent;
	}
	return {};
}
std::wstring FixtureText(const platform::serialization::JsoncValue::Object& fields, const wchar_t* name)
{
	return std::get<std::wstring>(fields.at(name).Value());
}
// The fixture's `page` member is a wire-shaped publishTreePage record (the same
// shape as the effect-protocol fixture). Locating its raw text in the line is
// plumbing; the page itself is decoded by the real senp::effect::Decode.
std::string RawPageJson(std::string_view line, std::size_t index)
{
	std::size_t search = 0;
	for (;;) {
		const auto at = line.find("\"page\":", search);
		if (at == std::string_view::npos) return {};
		const auto open = line.find('{', at);
		if (open == std::string_view::npos) return {};
		std::size_t depth = 0, end = open;
		bool inString = false, escape = false;
		for (; end < line.size(); ++end) {
			const char c = line[end];
			if (escape) { escape = false; continue; }
			if (inString && c == '\\') { escape = true; continue; }
			if (c == '"') inString = !inString;
			else if (!inString && c == '{') ++depth;
			else if (!inString && c == '}' && --depth == 0) { ++end; break; }
		}
		if (index-- == 0) return std::string(line.substr(open, end - open));
		search = end;
	}
}
TEST(SenpTreeProviderPagingFixture, SharedFixturePagesApplyThroughTheRealProviderConversion)
{
	std::ifstream input(TreePagingFixturePath(), std::ios::binary);
	ASSERT_TRUE(input.good());
	std::string line;
	std::size_t cases = 0;
	while (std::getline(input, line)) {
		SCOPED_TRACE(cases);
		const auto document = platform::serialization::CJsoncDocument::ParseStrict(line);
		ASSERT_TRUE(document.Succeeded());
		const auto& scenario = std::get<platform::serialization::JsoncValue::Object>(document.value->Value());
		const auto& pages = std::get<platform::serialization::JsoncValue::Array>(scenario.at(L"pages").Value());
		ASSERT_FALSE(pages.empty());
		std::vector<senp::effect::PublishTreePage> expected;
		std::set<std::wstring> declared;
		for (std::size_t index = 0; index < pages.size(); ++index) {
			const auto raw = RawPageJson(line, index);
			ASSERT_FALSE(raw.empty());
			// Wrap the page in a full effects envelope and let the real decoder
			// produce the DTO; the test never maps JSON members itself.
			const auto wire = std::string("{\"protocol\":2,\"sequence\":1,\"sessionGeneration\":1,\"body\":{\"type\":\"effects\",\"data\":{\"context\":{\"operationId\":\"fixture-1\",\"ownerGeneration\":1,\"workspaceRevision\":1,\"accountGeneration\":1,\"requestGeneration\":1},\"effects\":[{\"type\":\"publishTreePage\",\"data\":") + raw + "}]}}}";
			const auto envelope = senp::effect::Decode(wire);
			ASSERT_TRUE(envelope);
			const auto* message = std::get_if<senp::effect::EffectsMessage>(&envelope->body);
			ASSERT_TRUE(message);
			ASSERT_EQ(1U, message->effects.size());
			const auto* page = std::get_if<senp::effect::PublishTreePage>(&message->effects[0]);
			ASSERT_TRUE(page);
			expected.push_back(*page);
			for (const auto& item : expected.back().items) if (!item.commandId.empty()) declared.insert(item.commandId);
		}
		const auto runtime = std::make_shared<TreeRuntimeProbe>();
		SenpTreeProvider provider({ expected[0].viewId, { 1, 2, 3 }, { declared.begin(), declared.end() }, runtime });
		const SenpTreeProvider::Time now{};
		provider.SetVisible(true, now);
		for (std::size_t index = 0; index < pages.size(); ++index) {
			const auto& fields = std::get<platform::serialization::JsoncValue::Object>(pages[index].Value());
			const auto parentId = expected[index].parentId;
			if (index) ASSERT_EQ(TreeResult::Accepted, provider.LoadNext(parentId, now));
			ASSERT_EQ(index + 1, runtime->Submitted().size());
			const auto& submitted = runtime->Submitted()[index];
			EXPECT_EQ(parentId, submitted.request.parentId);
			EXPECT_EQ(FixtureText(fields, L"requestCursor"), submitted.request.cursor);
			ASSERT_EQ(TreeResult::Applied, provider.Apply(submitted.context, expected[index], now));
		}
		std::vector<std::wstring> children;
		for (const auto& entry : std::get<platform::serialization::JsoncValue::Array>(scenario.at(L"children").Value()))
			children.push_back(std::get<std::wstring>(entry.Value()));
		const auto* parent = provider.Model().Inspect(expected[0].parentId);
		ASSERT_TRUE(parent);
		EXPECT_EQ(children, parent->children);
		++cases;
	}
	EXPECT_EQ(2U, cases);
}
}
} // namespace workbench::tree
