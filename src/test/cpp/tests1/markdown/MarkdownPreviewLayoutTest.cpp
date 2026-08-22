/*! @file */
#include "pch.h"

#include "markdown/MarkdownInlineStyleRuns.h"
#include "markdown/MarkdownPreviewAsyncState.h"
#include "markdown/MarkdownPreviewCommandState.h"
#include "markdown/MarkdownPreviewLayout.h"
#include "markdown/MarkdownPreviewScrollMap.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace markdown {
namespace {

TEST(MarkdownPreviewLayout, HiddenPreviewLeavesTheWholeCentralRegionToTheEditor)
{
	const auto layout = CalculateMarkdownPreviewLayout(120, 1000, 96, false);
	EXPECT_EQ(120, layout.editorLeft);
	EXPECT_EQ(1000, layout.editorRight);
	EXPECT_EQ(0, layout.PreviewWidth());
	EXPECT_EQ(1000, layout.previewLeft);
}

TEST(MarkdownPreviewScrollMap, PreservesWrappedRowAndPixelOffsetAcrossReflow)
{
	struct Line { std::size_t sourceLine; int top; };
	const std::vector<Line> before{
		{ 4, 10 }, { 4, 30 }, { 4, 50 }, { 8, 70 },
	};
	const auto anchor = CapturePreviewScrollAnchor(before, 37);
	ASSERT_TRUE(anchor.has_value());
	EXPECT_EQ(4u, anchor->sourceLine);
	EXPECT_EQ(1u, anchor->sourceLineOrdinal);
	EXPECT_EQ(7, anchor->intraLineOffset);

	const std::vector<Line> after{
		{ 4, 12 }, { 4, 36 }, { 4, 60 }, { 4, 84 }, { 8, 108 },
	};
	EXPECT_EQ(43, RestorePreviewScrollAnchor(after, *anchor));
}

TEST(MarkdownPreviewScrollMap, ClampsMissingWrappedRowsAndFallsForward)
{
	struct Line { std::size_t sourceLine; int top; };
	const std::vector<Line> lines{
		{ 2, 10 }, { 2, 30 }, { 9, 50 },
	};
	EXPECT_EQ(35, RestorePreviewScrollAnchor(lines, { 2, 7, 5 }));
	EXPECT_EQ(55, RestorePreviewScrollAnchor(lines, { 7, 0, 5 }));
	EXPECT_EQ(55, RestorePreviewScrollAnchor(lines, { 20, 0, 5 }));
	EXPECT_FALSE(RestorePreviewScrollAnchor(std::vector<Line>{}, { 0, 0, 0 }).has_value());
}

TEST(MarkdownPreviewLayout, VisiblePreviewIsAnEditorSiblingBeforeTheRightBoundary)
{
	const auto layout = CalculateMarkdownPreviewLayout(120, 1600, 144, true);
	EXPECT_EQ(120, layout.editorLeft);
	EXPECT_GT(layout.EditorWidth(), 0);
	EXPECT_GT(layout.PreviewWidth(), 0);
	EXPECT_EQ(layout.editorRight, layout.dividerLeft);
	EXPECT_EQ(layout.dividerRight, layout.previewLeft);
	EXPECT_EQ(1600, layout.previewRight);
}

TEST(MarkdownPreviewLayout, ReservesExactlyOneThemedDividerDipBetweenTheSiblingPanes)
{
	const auto layout96 = CalculateMarkdownPreviewLayout(0, 1200, 96, true);
	const auto layout192 = CalculateMarkdownPreviewLayout(0, 2400, 192, true);
	EXPECT_EQ(1, layout96.dividerRight - layout96.dividerLeft);
	EXPECT_EQ(2, layout192.dividerRight - layout192.dividerLeft);
	EXPECT_EQ(layout96.editorRight, layout96.dividerLeft);
	EXPECT_EQ(layout192.previewLeft, layout192.dividerRight);
}

TEST(MarkdownPreviewLayout, VeryNarrowRegionsNeverProduceInvertedBounds)
{
	for (const int width : { 0, 1, 2, 7, 31 }) {
		const auto layout = CalculateMarkdownPreviewLayout(10, 10 + width, 192, true);
		EXPECT_LE(layout.editorLeft, layout.editorRight);
		EXPECT_LE(layout.dividerLeft, layout.dividerRight);
		EXPECT_LE(layout.previewLeft, layout.previewRight);
		EXPECT_EQ(10 + width, layout.previewRight);
	}
}

TEST(MarkdownPreviewLayout, CurrentGroupPreviewReplacesTheSourceEditor)
{
	const auto layout = CalculateMarkdownPreviewLayout(120, 1000, 144, PreviewPaneMode::Replacement);
	EXPECT_EQ(0, layout.EditorWidth());
	EXPECT_EQ(880, layout.PreviewWidth());
	EXPECT_EQ(120, layout.previewLeft);
	EXPECT_EQ(1000, layout.previewRight);
}

TEST(MarkdownInlineStyleRuns, NormalizesUnsortedOverlappingAndDuplicateSpansOnce)
{
	const std::vector<InlineSpan> spans{
		{ InlineKind::Strong, 2, 5, std::nullopt },
		{ InlineKind::Link, 0, 3, std::nullopt },
		{ InlineKind::Strong, 1, 10, std::nullopt },
		{ InlineKind::Emphasis, 4, 4, std::nullopt },
		{ InlineKind::Code, 6, 2, std::nullopt },
		{ InlineKind::Image, std::numeric_limits<std::size_t>::max() - 2, 10, std::nullopt },
	};
	const auto result = BuildInlineStyleRuns(12, spans);
	ASSERT_FALSE(result.runs.empty());
	EXPECT_EQ(10u, result.statistics.boundaryEventCount);
	EXPECT_EQ(result.runs.size(), result.statistics.outputRunCount);
	EXPECT_EQ(0u, result.runs.front().start);
	EXPECT_EQ(12u, result.runs.back().End());
	for (std::size_t index = 1; index < result.runs.size(); ++index) {
		EXPECT_EQ(result.runs[index - 1].End(), result.runs[index].start);
		EXPECT_GT(result.runs[index].length, 0u);
	}
	const auto at = [&](std::size_t offset) -> const InlineStyleRun& {
		const auto found = std::find_if(result.runs.begin(), result.runs.end(),
			[offset](const InlineStyleRun& run) { return run.start <= offset && offset < run.End(); });
		EXPECT_NE(result.runs.end(), found);
		return *found;
	};
	EXPECT_TRUE(at(1).Has(InlineStyleFlag::Strong));
	EXPECT_TRUE(at(1).Has(InlineStyleFlag::Link));
	EXPECT_TRUE(at(5).Has(InlineStyleFlag::Strong));
	EXPECT_TRUE(at(5).Has(InlineStyleFlag::Emphasis));
	EXPECT_TRUE(at(6).Has(InlineStyleFlag::Code));
	EXPECT_FALSE(at(11).Has(InlineStyleFlag::Strong));
}

TEST(MarkdownInlineStyleRuns, LongLineWithManyDuplicateSpansKeepsDrawBatchCountBounded)
{
	constexpr std::size_t textLength = 1'000'000;
	constexpr std::size_t spanCount = 20'000;
	std::vector<InlineSpan> spans;
	spans.reserve(spanCount);
	for (std::size_t index = 0; index < spanCount; ++index) {
		spans.push_back({ index % 2 == 0 ? InlineKind::Strong : InlineKind::Emphasis,
			100, textLength - 200, std::nullopt });
	}
	const auto result = BuildInlineStyleRuns(textLength, spans);
	EXPECT_EQ(spanCount * 2, result.statistics.boundaryEventCount);
	EXPECT_EQ(2u, result.statistics.boundaryGroupCount);
	ASSERT_EQ(3u, result.runs.size());
	EXPECT_EQ(3u, result.statistics.outputRunCount);
	EXPECT_EQ(textLength, result.runs.back().End());
	EXPECT_TRUE(result.runs[1].Has(InlineStyleFlag::Strong));
	EXPECT_TRUE(result.runs[1].Has(InlineStyleFlag::Emphasis));
}

TEST(MarkdownInlineStyleRuns, WrappedLineClippingVisitsOnlyIntersectingNormalizedRuns)
{
	const std::vector<InlineSpan> spans{
		{ InlineKind::Strong, 2, 8, std::nullopt },
		{ InlineKind::Link, 8, 8, std::nullopt },
	};
	const auto normalized = BuildInlineStyleRuns(20, spans);
	const auto clipped = ClipInlineStyleRuns(normalized.runs, 7, 6);
	ASSERT_EQ(3u, clipped.size());
	EXPECT_EQ(0u, clipped.front().start);
	EXPECT_EQ(6u, clipped.back().End());
	EXPECT_TRUE(clipped.front().Has(InlineStyleFlag::Strong));
	EXPECT_TRUE(clipped[1].Has(InlineStyleFlag::Strong));
	EXPECT_TRUE(clipped[1].Has(InlineStyleFlag::Link));
	EXPECT_TRUE(clipped.back().Has(InlineStyleFlag::Link));
}

TEST(MarkdownInlineStyleRuns, ExtendedNativeInlineKindsHaveExplicitFallbackStyles)
{
	const std::vector<InlineSpan> spans{
		{ InlineKind::Strikethrough, 0, 3, std::nullopt },
		{ InlineKind::Autolink, 3, 3, std::nullopt },
		{ InlineKind::Math, 6, 3, std::nullopt },
	};
	const auto result = BuildInlineStyleRuns(9, spans);
	ASSERT_EQ(3u, result.runs.size());
	EXPECT_TRUE(result.runs[0].Has(InlineStyleFlag::Strikethrough));
	EXPECT_TRUE(result.runs[1].Has(InlineStyleFlag::Link));
	EXPECT_TRUE(result.runs[2].Has(InlineStyleFlag::Code));
}

TEST(MarkdownPreviewCommandState, ExactToSideCommandsFailClosedWithoutASecondEditorGroup)
{
	MarkdownPreviewCommandState state;
	const auto dynamic = state.Apply(MarkdownPreviewCommand::ShowPreviewToSide, L"C:\\readme.md", true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup, dynamic.outcome);
	EXPECT_FALSE(state.IsVisible());
	const auto locked = state.Apply(MarkdownPreviewCommand::ShowLockedPreviewToSide, L"C:\\readme.md", true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup, locked.outcome);
	EXPECT_FALSE(state.IsVisible());
}

TEST(MarkdownPreviewCommandState, CurrentGroupAndLegacySiblingRemainDistinctPlacements)
{
	MarkdownPreviewCommandState state;
	const auto current = state.Apply(MarkdownPreviewCommand::ShowPreview, L"C:\\readme.md", true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, current.outcome);
	EXPECT_EQ(MarkdownPreviewPlacement::CurrentEditorGroup, state.Placement());
	EXPECT_TRUE(state.IsVisible());

	const auto sibling = state.ToggleNativeSibling(L"C:\\readme.md", true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, sibling.outcome);
	EXPECT_EQ(MarkdownPreviewPlacement::NativeSiblingPane, state.Placement());
	EXPECT_TRUE(state.IsVisible());
}

TEST(MarkdownPreviewCommandState, LockedPreviewKeepsIdentityAndDynamicPreviewFollowsIt)
{
	MarkdownPreviewCommandState state;
	MarkdownPreviewHostCapabilities capabilities;
	capabilities.sideEditorGroup = true;
	const auto locked = state.Apply(MarkdownPreviewCommand::ShowLockedPreviewToSide,
		L"C:\\one.md", true, capabilities);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, locked.outcome);
	EXPECT_TRUE(state.IsLocked());
	EXPECT_FALSE(state.ObserveActiveSource(L"C:\\two.md", true));
	EXPECT_EQ(L"C:\\one.md", state.SourceIdentity());

	const auto unlocked = state.Apply(MarkdownPreviewCommand::ToggleLock, L"C:\\two.md", true, capabilities);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, unlocked.outcome);
	EXPECT_TRUE(unlocked.identityChanged);
	EXPECT_EQ(L"C:\\two.md", state.SourceIdentity());
}

TEST(MarkdownPreviewCommandState, LockedPreviewRefreshCannotAdoptAnotherActiveDocument)
{
	MarkdownPreviewCommandState state;
	ASSERT_EQ(MarkdownPreviewCommandOutcome::Applied,
		state.Apply(MarkdownPreviewCommand::ShowPreview, L"C:\\one.md", true).outcome);
	ASSERT_EQ(MarkdownPreviewCommandOutcome::Applied,
		state.Apply(MarkdownPreviewCommand::ToggleLock, L"C:\\one.md", true).outcome);
	ASSERT_FALSE(state.ObserveActiveSource(L"C:\\two.md", true));
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnavailableLockedSource,
		state.Apply(MarkdownPreviewCommand::Refresh, L"C:\\two.md", true).outcome);
	EXPECT_EQ(L"C:\\one.md", state.SourceIdentity());
}

TEST(MarkdownPreviewCommandState, RefreshSecurityAndNonMarkdownPathsTerminateExplicitly)
{
	MarkdownPreviewCommandState state;
	EXPECT_EQ(MarkdownPreviewCommandOutcome::NotApplicable,
		state.Apply(MarkdownPreviewCommand::ShowPreview, L"C:\\readme.txt", false).outcome);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSecuritySelector,
		state.Apply(MarkdownPreviewCommand::ShowPreviewSecuritySelector, L"C:\\readme.md", true).outcome);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::NotApplicable,
		state.Apply(MarkdownPreviewCommand::Refresh, L"C:\\readme.md", true).outcome);
	ASSERT_EQ(MarkdownPreviewCommandOutcome::Applied,
		state.Apply(MarkdownPreviewCommand::ReopenAsPreview, L"C:\\readme.md", true).outcome);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::RefreshRequested,
		state.Apply(MarkdownPreviewCommand::Refresh, L"C:\\readme.md", true).outcome);
}

TEST(MarkdownPreviewAsyncState, CoalescesQueuedWorkToTheLatestImmutableSnapshot)
{
	MarkdownPreviewAsyncState state;
	const PreviewRenderKey first{ 1, 10 };
	const PreviewRenderKey latest{ 2, 11 };
	EXPECT_EQ(PreviewQueueAction::Queued, state.Queue(first));
	EXPECT_EQ(PreviewQueueAction::ReplacedPending, state.Queue(latest));
	ASSERT_TRUE(state.Pending().has_value());
	EXPECT_EQ(latest, *state.Pending());
	ASSERT_TRUE(state.TakeNext().has_value());
	EXPECT_EQ(latest, *state.InFlight());
}

TEST(MarkdownPreviewAsyncState, DiscardsCompletedWorkWhenANewerRevisionIsPending)
{
	MarkdownPreviewAsyncState state;
	const PreviewRenderKey first{ 1, 10 };
	const PreviewRenderKey latest{ 2, 11 };
	ASSERT_EQ(PreviewQueueAction::Queued, state.Queue(first));
	ASSERT_EQ(first, *state.TakeNext());
	ASSERT_EQ(PreviewQueueAction::Queued, state.Queue(latest));
	EXPECT_EQ(PreviewCompletionAction::DiscardStale, state.Complete(first, true));
	ASSERT_EQ(latest, *state.TakeNext());
	EXPECT_EQ(PreviewCompletionAction::Publish, state.Complete(latest, true));
	EXPECT_TRUE(state.IsCurrent(latest));
	state.MarkDelivered(latest);
	EXPECT_EQ(PreviewAsyncPhase::Idle, state.Phase());
}

TEST(MarkdownPreviewAsyncState, WrongCompletionCannotStealTheActiveRequest)
{
	MarkdownPreviewAsyncState state;
	const PreviewRenderKey active{ 8, 80 };
	ASSERT_EQ(PreviewQueueAction::Queued, state.Queue(active));
	ASSERT_EQ(active, *state.TakeNext());
	EXPECT_EQ(PreviewCompletionAction::DiscardStale,
		state.Complete({ 7, 70 }, true));
	ASSERT_TRUE(state.InFlight().has_value());
	EXPECT_EQ(active, *state.InFlight());
	EXPECT_EQ(PreviewCompletionAction::PublishFailure, state.Complete(active, false));
	EXPECT_EQ(PreviewAsyncPhase::Failed, state.Phase());
}

TEST(MarkdownPreviewAsyncState, CloseOwnsEveryPendingAndInFlightTerminal)
{
	MarkdownPreviewAsyncState state;
	const PreviewRenderKey active{ 4, 40 };
	ASSERT_EQ(PreviewQueueAction::Queued, state.Queue(active));
	ASSERT_EQ(active, *state.TakeNext());
	ASSERT_EQ(PreviewQueueAction::Queued, state.Queue({ 5, 50 }));
	state.Close();
	EXPECT_EQ(PreviewAsyncPhase::Closed, state.Phase());
	EXPECT_FALSE(state.Pending().has_value());
	EXPECT_FALSE(state.InFlight().has_value());
	EXPECT_EQ(PreviewCompletionAction::DiscardClosed, state.Complete(active, true));
	EXPECT_EQ(PreviewQueueAction::RejectedClosed, state.Queue({ 6, 60 }));
}

TEST(MarkdownPreviewScrollMap, MapsBothDirectionsWithoutMovingAnEditorCaret)
{
	struct Line { std::size_t sourceLine; int top; };
	const std::vector<Line> empty;
	EXPECT_FALSE(PreviewTopForSourceLine(empty, 0).has_value());
	EXPECT_FALSE(SourceLineForPreviewScroll(empty, 0).has_value());
	const std::vector<Line> lines{
		{ 0, 10 }, { 0, 30 }, { 4, 50 }, { 9, 70 },
	};
	EXPECT_EQ(10, PreviewTopForSourceLine(lines, 0));
	EXPECT_EQ(50, PreviewTopForSourceLine(lines, 3));
	EXPECT_EQ(70, PreviewTopForSourceLine(lines, 10));
	ASSERT_TRUE(SourceLineForPreviewScroll(lines, 0).has_value());
	EXPECT_EQ(0u, *SourceLineForPreviewScroll(lines, 0));
	EXPECT_EQ(0u, *SourceLineForPreviewScroll(lines, 49));
	EXPECT_EQ(4u, *SourceLineForPreviewScroll(lines, 50));
	EXPECT_EQ(9u, *SourceLineForPreviewScroll(lines, 1000));
}


/*!
	@brief A dragged width is honored, so the divider can actually be moved

	Without a width request the split is a fixed proportion, which is what made
	the divider look decorative: it was painted, but nothing could change it.
*/
TEST(MarkdownPreviewLayout, HonorsAUserDraggedPreviewWidth)
{
	const auto narrow = CalculateMarkdownPreviewLayout(0, 1200, 96, PreviewPaneMode::NativeSibling, 300);
	EXPECT_EQ(300, narrow.PreviewWidth());
	const auto wide = CalculateMarkdownPreviewLayout(0, 1200, 96, PreviewPaneMode::NativeSibling, 800);
	EXPECT_EQ(800, wide.PreviewWidth());
	EXPECT_EQ(1200, wide.previewRight);
	EXPECT_EQ(wide.dividerRight, wide.previewLeft);
	// A width request wider than the default proportion is allowed; only the
	// untouched default is capped at the preferred width.
	const auto byDefault = CalculateMarkdownPreviewLayout(0, 1200, 96, PreviewPaneMode::NativeSibling);
	EXPECT_GT(wide.PreviewWidth(), byDefault.PreviewWidth());
}

//! Zero means "default", so the caller cannot accidentally request a zero-width preview.
TEST(MarkdownPreviewLayout, TreatsAZeroWidthRequestAsTheDefaultProportion)
{
	const auto requested = CalculateMarkdownPreviewLayout(0, 1200, 96,
		PreviewPaneMode::NativeSibling, kPreviewDefaultWidthRequestDip);
	const auto byDefault = CalculateMarkdownPreviewLayout(0, 1200, 96, PreviewPaneMode::NativeSibling);
	EXPECT_EQ(byDefault.PreviewWidth(), requested.PreviewWidth());
	EXPECT_EQ(byDefault.editorRight, requested.editorRight);
}

//! An over-dragged pointer parks the divider at the limit instead of being ignored.
TEST(MarkdownPreviewLayout, ClampsADraggedWidthAgainstBothMinimums)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		const int right = ::MulDiv(1400, static_cast<int>(dpi), 96);
		const int minimumEditor = ::MulDiv(kMinimumEditorWidthDip, static_cast<int>(dpi), 96);
		const int minimumPreview = ::MulDiv(kMinimumPreviewWidthDip, static_cast<int>(dpi), 96);
		const auto collapsed = CalculateMarkdownPreviewLayout(0, right, dpi,
			PreviewPaneMode::NativeSibling, 1);
		EXPECT_GE(collapsed.PreviewWidth(), minimumPreview);
		const auto engulfed = CalculateMarkdownPreviewLayout(0, right, dpi,
			PreviewPaneMode::NativeSibling, 100000);
		EXPECT_GE(engulfed.EditorWidth(), minimumEditor);
		EXPECT_GT(engulfed.PreviewWidth(), 0);
	}
}

/*!
	@brief The pointer holds the divider, so the preview keeps what is to its right

	The result is expressed in device-independent pixels so the dragged width
	survives a move to a display with a different scale factor.
*/
TEST(MarkdownPreviewLayout, DerivesAWidthRequestFromThePointerPosition)
{
	EXPECT_EQ(400, RequestedPreviewWidthDipFromPointer(1200, 800, 96));
	EXPECT_EQ(400, RequestedPreviewWidthDipFromPointer(2400, 1600, 192));
	// Never zero: zero would silently mean "restore the default".
	EXPECT_EQ(1, RequestedPreviewWidthDipFromPointer(1200, 1200, 96));
	EXPECT_EQ(1, RequestedPreviewWidthDipFromPointer(1200, 4000, 96));
}

//! A dragged width must not resurrect a preview in a mode that has none.
TEST(MarkdownPreviewLayout, IgnoresADraggedWidthWhenThePreviewIsNotASibling)
{
	const auto hidden = CalculateMarkdownPreviewLayout(0, 1200, 96, PreviewPaneMode::Hidden, 800);
	EXPECT_EQ(0, hidden.PreviewWidth());
	EXPECT_EQ(1200, hidden.EditorWidth());
	const auto replacement = CalculateMarkdownPreviewLayout(0, 1200, 96,
		PreviewPaneMode::Replacement, 800);
	EXPECT_EQ(0, replacement.EditorWidth());
	EXPECT_EQ(1200, replacement.PreviewWidth());
}

} // namespace
} // namespace markdown
