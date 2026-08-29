/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/runtime/TerminalCollectionModel.h"

#include <optional>
#include <string>

namespace {

using namespace terminal::runtime::topology;
using terminal::TerminalPaneOrientation;
using terminal::TerminalPaneId;
using terminal::TerminalSessionId;
using terminal::TerminalTopologyRevision;
using terminal::TerminalWindowId;

TEST(TerminalCollectionModel, CreatesReadyToProjectSessionWindowAndPaneWithStrongMonotonicIds)
{
	TerminalCollectionModel model;
	const auto first = model.CreateSession("first");
	ASSERT_TRUE(first.Succeeded());
	ASSERT_TRUE(first.sessionId && first.windowId && first.paneId && first.instanceId);
	EXPECT_EQ(1U, first.sessionId->value);
	EXPECT_EQ(1U, first.windowId->value);
	EXPECT_EQ(1U, first.paneId->value);
	EXPECT_EQ(1U, first.instanceId->value);
	EXPECT_EQ(1U, first.revision.value);
	ASSERT_TRUE(model.ValidateInvariants());

	const auto second = model.CreateSession("second");
	ASSERT_TRUE(second.Succeeded());
	EXPECT_EQ(2U, second.sessionId->value);
	EXPECT_EQ(2U, second.windowId->value);
	EXPECT_EQ(2U, second.paneId->value);
	EXPECT_EQ(2U, second.instanceId->value);
	EXPECT_EQ(2U, second.revision.value);

	const auto closed = model.CloseSession(*first.sessionId, model.Revision());
	ASSERT_TRUE(closed.Succeeded());
	EXPECT_FALSE(model.FindSession(*first.sessionId));
	EXPECT_FALSE(model.FindWindow(*first.sessionId, *first.windowId));
	EXPECT_FALSE(model.FindPane(*first.paneId));
	ASSERT_TRUE(model.ValidateInvariants());

	const auto replacement = model.CreateSession("replacement");
	ASSERT_TRUE(replacement.Succeeded());
	EXPECT_GT(replacement.sessionId->value, first.sessionId->value);
	EXPECT_GT(replacement.windowId->value, first.windowId->value);
	EXPECT_GT(replacement.paneId->value, first.paneId->value);
	EXPECT_GT(replacement.instanceId->value, first.instanceId->value);
}

TEST(TerminalCollectionModel, FencesStaleRevisionAndNeverFallsBackFromStaleIdentity)
{
	TerminalCollectionModel model;
	const auto created = model.CreateSession("session");
	ASSERT_TRUE(created.Succeeded());
	const auto stale = model.CreateTerminalWindow(*created.sessionId, "stale", TerminalTopologyRevision{ 0 });
	EXPECT_EQ(ETerminalCollectionResultCode::StaleRevision, stale.code);
	EXPECT_EQ(1U, model.Snapshot().revision.value);

	const auto missing = model.SelectPane(TerminalPaneId{ 999 });
	EXPECT_EQ(ETerminalCollectionResultCode::TargetMissing, missing.code);
	const auto oldClose = model.ClosePane(*created.sessionId, *created.windowId, TerminalPaneId{ 999 }, model.Revision());
	EXPECT_EQ(ETerminalCollectionResultCode::TargetMissing, oldClose.code);
	EXPECT_EQ(1U, model.Snapshot().sessions.size());
	ASSERT_TRUE(model.ValidateInvariants());
}

TEST(TerminalCollectionModel, JoinsSameAxisAndNestsOrthogonalSplitsWhileSelectingNewPane)
{
	TerminalCollectionModel model;
	const auto created = model.CreateSession("session");
	ASSERT_TRUE(created.Succeeded());

	const auto right = model.SplitPane(*created.sessionId, *created.windowId, *created.paneId,
		TerminalPaneOrientation::Horizontal, model.Revision());
	ASSERT_TRUE(right.Succeeded());
	const auto firstSnapshot = model.FindWindow(*created.sessionId, *created.windowId);
	ASSERT_TRUE(firstSnapshot);
	ASSERT_EQ(TerminalLayoutNodeKind::Split, firstSnapshot->root.kind);
	ASSERT_EQ(2U, firstSnapshot->root.children.size());
	EXPECT_EQ(TerminalPaneOrientation::Horizontal, firstSnapshot->root.orientation);
	EXPECT_EQ(*right.paneId, firstSnapshot->root.children[1].pane.paneId);
	EXPECT_EQ(*right.paneId, firstSnapshot->activePane);

	const auto third = model.SplitPane(*created.sessionId, *created.windowId, *right.paneId,
		TerminalPaneOrientation::Horizontal, model.Revision());
	ASSERT_TRUE(third.Succeeded());
	const auto joined = model.FindWindow(*created.sessionId, *created.windowId);
	ASSERT_TRUE(joined);
	ASSERT_EQ(3U, joined->root.children.size());
	EXPECT_EQ(*third.paneId, joined->root.children[2].pane.paneId);
	for (const auto weight : joined->root.weights) EXPECT_GT(weight, 0U);

	const auto stacked = model.SplitPane(*created.sessionId, *created.windowId, *third.paneId,
		TerminalPaneOrientation::Vertical, model.Revision());
	ASSERT_TRUE(stacked.Succeeded());
	const auto nested = model.FindWindow(*created.sessionId, *created.windowId);
	ASSERT_TRUE(nested);
	ASSERT_EQ(3U, nested->root.children.size());
	ASSERT_EQ(TerminalLayoutNodeKind::Split, nested->root.children[2].kind);
	EXPECT_EQ(TerminalPaneOrientation::Vertical, nested->root.children[2].orientation);
	ASSERT_EQ(2U, nested->root.children[2].children.size());
	EXPECT_EQ(*stacked.paneId, nested->activePane);
	EXPECT_TRUE(model.ValidateInvariants());
}

TEST(TerminalCollectionModel, ClosingActivePaneCollapsesTreeAndChoosesDeterministicFallback)
{
	TerminalCollectionModel model;
	const auto created = model.CreateSession("session");
	ASSERT_TRUE(created.Succeeded());
	const auto split = model.SplitPane(*created.sessionId, *created.windowId, *created.paneId,
		TerminalPaneOrientation::Horizontal, model.Revision());
	ASSERT_TRUE(split.Succeeded());
	const auto close = model.ClosePane(*created.sessionId, *created.windowId, *split.paneId, model.Revision());
	ASSERT_TRUE(close.Succeeded());
	const auto window = model.FindWindow(*created.sessionId, *created.windowId);
	ASSERT_TRUE(window);
	EXPECT_EQ(TerminalLayoutNodeKind::Pane, window->root.kind);
	EXPECT_EQ(*created.paneId, window->activePane);
	EXPECT_EQ(1U, window->panes.size());
	EXPECT_TRUE(model.ValidateInvariants());

	const auto closeLast = model.ClosePane(*created.sessionId, *created.windowId, *created.paneId, model.Revision());
	ASSERT_TRUE(closeLast.Succeeded());
	EXPECT_FALSE(model.FindSession(*created.sessionId));
	EXPECT_FALSE(model.Snapshot().activeSession);
	EXPECT_TRUE(model.ValidateInvariants());
}

TEST(TerminalCollectionModel, SelectsPaneAcrossWindowsAndSessionsWithoutGeometryOrWindowState)
{
	TerminalCollectionModel model;
	const auto first = model.CreateSession("first");
	const auto secondWindow = model.CreateTerminalWindow(*first.sessionId, "second-window", model.Revision());
	ASSERT_TRUE(secondWindow.Succeeded());
	EXPECT_EQ(*secondWindow.sessionId, *first.sessionId);
	const auto selectFirst = model.SelectWindow(*first.sessionId, *first.windowId, model.Revision());
	ASSERT_TRUE(selectFirst.Succeeded());
	const auto selectSecondPane = model.SelectPane(*secondWindow.paneId, model.Revision());
	ASSERT_TRUE(selectSecondPane.Succeeded());
	const auto snapshot = model.Snapshot();
	ASSERT_TRUE(snapshot.activeSession);
	EXPECT_EQ(*first.sessionId, *snapshot.activeSession);
	ASSERT_EQ(1U, snapshot.sessions.size());
	EXPECT_EQ(*secondWindow.windowId, snapshot.sessions.front().activeWindow);
	EXPECT_TRUE(model.ValidateInvariants());
}

TEST(TerminalCollectionModel, RejectsInvalidNamesDuplicatesAndIdentityExhaustion)
{
	TerminalCollectionLimits limits;
	limits.maximumSessionId = 1;
	limits.maximumNameBytes = 4;
	TerminalCollectionModel model(limits);
	EXPECT_EQ(ETerminalCollectionResultCode::InvalidRequest, model.CreateSession("bad\x01").code);
	const auto first = model.CreateSession("one");
	ASSERT_TRUE(first.Succeeded());
	EXPECT_EQ(ETerminalCollectionResultCode::NameConflict, model.CreateSession("one").code);
	EXPECT_EQ(ETerminalCollectionResultCode::IdentityExhausted, model.CreateSession("two").code);
	EXPECT_TRUE(model.ValidateInvariants());
}

} // namespace
