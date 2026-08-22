/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "view/CEditView_RenderingState.h"

#include <gtest/gtest.h>

namespace editor::rendering {
namespace {

constexpr workbench::rendering::FrameSurfaceId kSurfaceId =
	EditorViewSurfaceId(2, false);

TEST(CEditViewRenderState, StableIdentitySeparatesPaneAndMinimap)
{
	EXPECT_NE(EditorViewSurfaceId(0, false), EditorViewSurfaceId(0, true));
	EXPECT_NE(EditorViewSurfaceId(0, false), EditorViewSurfaceId(1, false));
	EXPECT_EQ(EditorViewSurfaceId(4, false), EditorViewSurfaceId(4, false));
}

TEST(CEditViewRenderState, LatestTicketWinsAndStaleTicketCannotPublish)
{
	CEditViewRenderState state(kSurfaceId);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());

	const auto first = state.RequestFrame();
	ASSERT_TRUE(first.ticket.has_value());
	ASSERT_TRUE(state.CommitGdiFrame(*first.ticket).Accepted());
	EXPECT_TRUE(state.Snapshot().hasLastGoodBitmap);

	ASSERT_TRUE(state.MarkDamage(EEditViewDamage::BaseText).Accepted());
	const auto old = state.RequestFrame();
	ASSERT_TRUE(old.ticket.has_value());
	ASSERT_TRUE(state.MarkDamage(EEditViewDamage::Selection).Accepted());
	const auto latest = state.RequestFrame();
	ASSERT_TRUE(latest.ticket.has_value());
	EXPECT_EQ(EEditViewRenderStatus::Stale,
		state.CommitGdiFrame(*old.ticket).status);
	EXPECT_TRUE(state.CommitGdiFrame(*latest.ticket).Accepted());
	EXPECT_TRUE(state.Snapshot().hasLastGoodBitmap);
}

TEST(CEditViewRenderState, DamageLayersAdvanceIndependently)
{
	CEditViewRenderState state(kSurfaceId + 1);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());
	const auto initial = state.RequestFrame();
	ASSERT_TRUE(initial.ticket.has_value());
	ASSERT_TRUE(state.CommitGdiFrame(*initial.ticket).Accepted());

	const auto before = state.Snapshot().damage;
	ASSERT_TRUE(state.MarkDamage(
		EEditViewDamage::Selection | EEditViewDamage::Caret).Accepted());
	const auto after = state.Snapshot().damage;
	EXPECT_TRUE(after.Has(EEditViewDamage::Selection));
	EXPECT_TRUE(after.Has(EEditViewDamage::Caret));
	EXPECT_EQ(before.generation[0], after.generation[0]);
	EXPECT_EQ(before.generation[3], after.generation[3]);
	EXPECT_EQ(before.generation[1] + 1, after.generation[1]);
	EXPECT_EQ(before.generation[2] + 1, after.generation[2]);
	EXPECT_EQ(before.committedGeneration, after.committedGeneration);

	ASSERT_TRUE(state.RequestFrame().ticket.has_value());
	const auto committed = state.CommitGdiFrame();
	ASSERT_TRUE(committed.has_value());
	EXPECT_FALSE(committed->damage.HasPending());
	EXPECT_EQ(committed->damage.generation,
		committed->damage.committedGeneration);
}

TEST(CEditViewRenderState, FailedPaintPreservesLastGoodBitmapAndPendingDamage)
{
	CEditViewRenderState state(kSurfaceId + 2);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());
	ASSERT_TRUE(state.CommitGdiFrame(state.RequestFrame().ticket.value()).Accepted());
	ASSERT_TRUE(state.MarkDamage(EEditViewDamage::Ime).Accepted());
	const auto ticket = state.RequestFrame();
	ASSERT_TRUE(ticket.ticket.has_value());

	EXPECT_EQ(EEditViewRenderStatus::PaintFailed,
		state.CommitGdiFrame(*ticket.ticket, false).status);
	const auto failed = state.Snapshot();
	EXPECT_TRUE(failed.hasLastGoodBitmap);
	EXPECT_TRUE(failed.damage.Has(EEditViewDamage::Ime));
	EXPECT_TRUE(state.CommitGdiFrame(*ticket.ticket, true).Accepted());
}

TEST(CEditViewRenderState, EpochChangesWithdrawPendingWithoutLosingLastGoodBitmap)
{
	CEditViewRenderState state(kSurfaceId + 3);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());
	const auto initial = state.RequestFrame();
	ASSERT_TRUE(initial.ticket.has_value());
	ASSERT_TRUE(state.CommitGdiFrame(*initial.ticket).Accepted());
	ASSERT_TRUE(state.MarkDamage(EEditViewDamage::BaseText).Accepted());
	const auto stale = state.RequestFrame();
	ASSERT_TRUE(stale.ticket.has_value());
	ASSERT_TRUE(state.NotifyLayout().Accepted());
	EXPECT_FALSE(state.IsCurrent(*stale.ticket));
	EXPECT_TRUE(state.Snapshot().hasLastGoodBitmap);
	EXPECT_TRUE(state.Snapshot().damage.HasPending());
	EXPECT_EQ(EEditViewRenderStatus::Stale,
		state.CommitGdiFrame(*stale.ticket).status);
}

TEST(CEditViewRenderState, GiantLineWorkIsExplicitlyCapped)
{
	EXPECT_EQ(0U, CEditViewRenderState::CapLineWork(0));
	EXPECT_EQ(CEditViewRenderState::kMaximumLineWorkItems,
		CEditViewRenderState::CapLineWork(
			CEditViewRenderState::kMaximumLineWorkItems + 1));
}

TEST(CEditViewRenderState, PaintQuantumRetainsCursorAcrossTurns)
{
	CEditViewRenderState state(kSurfaceId + 4);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());
	const EditViewPaintViewport viewport{
		.contentGeneration = 7,
		.layoutEpoch = 3,
		.layoutTop = 10,
		.layoutBottom = 40,
		.viewLeftColumn = 0,
		.viewRightColumn = 120,
	};

	const auto first = state.BeginPaintQuantum(viewport);
	EXPECT_EQ(CEditViewRenderState::kPaintQuantum, first.limit);
	EXPECT_TRUE(state.ConsumePaintWork(first.limit - 1));
	state.SavePaintCursor(22, 4096, 8192);
	const auto cursor = state.PaintCursor();
	ASSERT_TRUE(cursor.has_value());
	EXPECT_EQ(22, cursor->layoutLine);
	EXPECT_EQ(4096, cursor->logicOffset);
	EXPECT_EQ(8192, cursor->drawColumn);

	const auto second = state.BeginPaintQuantum(viewport);
	EXPECT_EQ(CEditViewRenderState::kPaintQuantum, second.remaining);
	EXPECT_TRUE(state.IsPaintCursorFor(22));
	state.CompletePaintCursor();
	EXPECT_FALSE(state.HasPaintContinuation());
}

TEST(CEditViewRenderState, ViewportChangeDropsPaintCursor)
{
	CEditViewRenderState state(kSurfaceId + 5);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());
	const EditViewPaintViewport viewport{
		.contentGeneration = 1,
		.layoutEpoch = 1,
		.layoutTop = 0,
		.layoutBottom = 20,
		.viewLeftColumn = 0,
		.viewRightColumn = 80,
	};
	(void)state.BeginPaintQuantum(viewport);
	state.SavePaintCursor(5, 120, 240);

	const EditViewPaintViewport scrolled = {
		.contentGeneration = viewport.contentGeneration,
		.layoutEpoch = viewport.layoutEpoch,
		.layoutTop = viewport.layoutTop + 1,
		.layoutBottom = viewport.layoutBottom + 1,
		.viewLeftColumn = viewport.viewLeftColumn,
		.viewRightColumn = viewport.viewRightColumn,
	};
	(void)state.BeginPaintQuantum(scrolled);
	EXPECT_FALSE(state.HasPaintContinuation());
	EXPECT_EQ(0U, state.PaintQuantumConsumed());
}

TEST(CEditViewRenderState, PartialCommitLeavesDamageUntilCursorCompletes)
{
	CEditViewRenderState state(kSurfaceId + 6);
	ASSERT_TRUE(state.Open("workbench.editor.pane", true).Accepted());
	ASSERT_TRUE(state.CommitGdiFrame(state.RequestFrame().ticket.value()).Accepted());
	ASSERT_TRUE(state.MarkDamage(EEditViewDamage::BaseText).Accepted());
	const auto partial = state.RequestFrame();
	ASSERT_TRUE(partial.ticket.has_value());

	const EditViewPaintViewport viewport{
		.contentGeneration = state.Snapshot().surface.contentGeneration,
		.layoutEpoch = 1,
		.layoutTop = 0,
		.layoutBottom = 20,
		.viewLeftColumn = 0,
		.viewRightColumn = 80,
	};
	(void)state.BeginPaintQuantum(viewport);
	state.SavePaintCursor(4, 64, 128);
	ASSERT_TRUE(state.CommitGdiFrame(*partial.ticket).Accepted());
	EXPECT_TRUE(state.Snapshot().damage.HasPending());

	const auto completed = state.RequestFrame();
	ASSERT_TRUE(completed.ticket.has_value());
	state.CompletePaintCursor();
	ASSERT_TRUE(state.CommitGdiFrame(*completed.ticket).Accepted());
	EXPECT_FALSE(state.Snapshot().damage.HasPending());
}

} // namespace
} // namespace editor::rendering
