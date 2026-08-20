/*! @file */
#include "pch.h"

#include "markdown/MarkdownPreviewAsyncState.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace markdown {
namespace {

constexpr PreviewRenderKey kKeyA{ 1, 1 };
constexpr PreviewRenderKey kKeyB{ 1, 2 };
constexpr PreviewRenderKey kKeyC{ 2, 1 };

enum class Step : std::uint8_t {
	QueueA,
	QueueB,
	QueueC,
	TakeNext,
	CompleteInFlightSucceeded,
	CompleteInFlightFailed,
	CompleteForeign,
	MarkDeliveredLastPublished,
	MarkDeliveryFailedLastPublished,
	Close,
};

constexpr std::array<Step, 10> kAlphabet{
	Step::QueueA, Step::QueueB, Step::QueueC, Step::TakeNext,
	Step::CompleteInFlightSucceeded, Step::CompleteInFlightFailed, Step::CompleteForeign,
	Step::MarkDeliveredLastPublished, Step::MarkDeliveryFailedLastPublished, Step::Close,
};

[[nodiscard]] const char* Name(Step step)
{
	switch (step) {
	case Step::QueueA: return "Queue(A)";
	case Step::QueueB: return "Queue(B)";
	case Step::QueueC: return "Queue(C)";
	case Step::TakeNext: return "TakeNext";
	case Step::CompleteInFlightSucceeded: return "Complete(inFlight, ok)";
	case Step::CompleteInFlightFailed: return "Complete(inFlight, failed)";
	case Step::CompleteForeign: return "Complete(foreign)";
	case Step::MarkDeliveredLastPublished: return "MarkDelivered(published)";
	case Step::MarkDeliveryFailedLastPublished: return "MarkDeliveryFailed(published)";
	case Step::Close: return "Close";
	}
	return "?";
}

[[nodiscard]] std::string Trace(const std::vector<Step>& steps, std::size_t upTo)
{
	std::string text;
	for (std::size_t index = 0; index <= upTo && index < steps.size(); ++index) {
		if (!text.empty()) text += " -> ";
		text += Name(steps[index]);
	}
	return text;
}

/*!
	@brief Replays every sequence over the transition alphabet up to `depth`

	This is the model-based check a hand-written scenario list cannot give: the
	interesting cases are the stale publish and the revive-after-close paths that
	nobody thinks to write down. Every transition is followed by the structural
	invariants the one-worker contract depends on.
*/
void ExploreEverySequence(std::size_t depth)
{
	std::vector<Step> steps(depth, Step::QueueA);
	std::size_t total = 1;
	for (std::size_t index = 0; index < depth; ++index) total *= kAlphabet.size();

	for (std::size_t sequence = 0; sequence < total; ++sequence) {
		auto remainder = sequence;
		for (std::size_t index = 0; index < depth; ++index) {
			steps[index] = kAlphabet[remainder % kAlphabet.size()];
			remainder /= kAlphabet.size();
		}

		MarkdownPreviewAsyncState state;
		bool closed = false;
		bool publishedIsLatest = false;
		std::optional<PreviewRenderKey> published;

		for (std::size_t index = 0; index < depth; ++index) {
			const auto trace = [&steps, index] { return Trace(steps, index); };
			const auto latestBefore = state.LatestRequested();

			switch (steps[index]) {
			case Step::QueueA:
			case Step::QueueB:
			case Step::QueueC: {
				const auto key = steps[index] == Step::QueueA ? kKeyA
					: (steps[index] == Step::QueueB ? kKeyB : kKeyC);
				const auto action = state.Queue(key);
				if (closed) {
					ASSERT_EQ(PreviewQueueAction::RejectedClosed, action) << trace();
					ASSERT_FALSE(state.Pending().has_value()) << trace();
				} else {
					// A newer request supersedes whatever was published before it.
					publishedIsLatest = false;
					ASSERT_NE(PreviewQueueAction::RejectedClosed, action) << trace();
					ASSERT_EQ(key, state.Pending()) << trace();
					ASSERT_EQ(key, state.LatestRequested()) << trace();
				}
				break;
			}

			case Step::TakeNext: {
				const auto taken = state.TakeNext();
				if (closed) ASSERT_FALSE(taken.has_value()) << trace();
				if (taken) {
					ASSERT_EQ(taken, state.InFlight()) << trace();
					ASSERT_FALSE(state.Pending().has_value()) << trace();
					ASSERT_EQ(PreviewAsyncPhase::Running, state.Phase()) << trace();
				}
				break;
			}

			case Step::CompleteInFlightSucceeded:
			case Step::CompleteInFlightFailed: {
				const bool succeeded = steps[index] == Step::CompleteInFlightSucceeded;
				const auto inFlight = state.InFlight();
				if (!inFlight) break;
				const auto pendingBefore = state.Pending();
				const auto action = state.Complete(*inFlight, succeeded);
				if (closed) {
					ASSERT_EQ(PreviewCompletionAction::DiscardClosed, action) << trace();
					break;
				}
				const bool publishable = !pendingBefore && latestBefore == inFlight;
				if (publishable) {
					ASSERT_EQ(succeeded ? PreviewCompletionAction::Publish
						: PreviewCompletionAction::PublishFailure, action) << trace();
					published = inFlight;
					publishedIsLatest = true;
				} else {
					// A result the user can no longer be looking at must never reach
					// the window, however it completed.
					ASSERT_EQ(PreviewCompletionAction::DiscardStale, action) << trace();
				}
				ASSERT_FALSE(state.InFlight().has_value()) << trace();
				break;
			}

			case Step::CompleteForeign: {
				const auto inFlight = state.InFlight();
				const PreviewRenderKey foreign{ 99, 99 };
				const auto action = state.Complete(foreign, true);
				ASSERT_EQ(closed ? PreviewCompletionAction::DiscardClosed
					: PreviewCompletionAction::DiscardStale, action) << trace();
				if (!closed) ASSERT_EQ(inFlight, state.InFlight()) << trace();
				break;
			}

			case Step::MarkDeliveredLastPublished: {
				if (!published) break;
				state.MarkDelivered(*published);
				if (closed) ASSERT_EQ(PreviewAsyncPhase::Closed, state.Phase()) << trace();
				break;
			}

			case Step::MarkDeliveryFailedLastPublished: {
				if (!published) break;
				const auto inFlightBefore = state.InFlight();
				const auto pendingBefore = state.Pending();
				state.MarkDeliveryFailed(*published);
				// A failed delivery must not resurrect the generation as work.
				ASSERT_EQ(inFlightBefore, state.InFlight()) << trace();
				ASSERT_EQ(pendingBefore, state.Pending()) << trace();
				if (closed) ASSERT_EQ(PreviewAsyncPhase::Closed, state.Phase()) << trace();
				break;
			}

			case Step::Close:
				state.Close();
				closed = true;
				break;
			}

			if (closed) {
				ASSERT_EQ(PreviewAsyncPhase::Closed, state.Phase()) << trace();
				ASSERT_FALSE(state.Pending().has_value()) << trace();
				ASSERT_FALSE(state.InFlight().has_value()) << trace();
				ASSERT_FALSE(state.IsCurrent(kKeyA)) << trace();
				ASSERT_FALSE(state.IsCurrent(kKeyB)) << trace();
				ASSERT_FALSE(state.IsCurrent(kKeyC)) << trace();
			}
			if (state.Phase() == PreviewAsyncPhase::Running) {
				ASSERT_TRUE(state.InFlight().has_value()) << trace();
			}
			if (state.Pending()) {
				ASSERT_EQ(state.Pending(), state.LatestRequested()) << trace();
			}
			if (publishedIsLatest && !closed) {
				// A published result stays the newest one until something newer is
				// requested; it is never overtaken while it is still the latest.
				ASSERT_EQ(published, state.LatestRequested()) << trace();
			}
		}
	}
}

TEST(MarkdownPreviewAsyncState, EverySequenceOfSixTransitionsPreservesTheWorkerInvariants)
{
	ExploreEverySequence(6);
}

TEST(MarkdownPreviewAsyncState, ARepeatedlyEditedDocumentRendersOnlyTheLatestRevision)
{
	MarkdownPreviewAsyncState state;
	EXPECT_EQ(PreviewQueueAction::Queued, state.Queue(kKeyA));
	EXPECT_EQ(kKeyA, state.TakeNext());
	EXPECT_EQ(PreviewQueueAction::Queued, state.Queue(kKeyB));
	EXPECT_EQ(PreviewQueueAction::ReplacedPending, state.Queue(kKeyC));

	// The in-flight render is now two revisions old and must be discarded.
	EXPECT_EQ(PreviewCompletionAction::DiscardStale, state.Complete(kKeyA, true));
	EXPECT_EQ(PreviewAsyncPhase::Queued, state.Phase());

	EXPECT_EQ(kKeyC, state.TakeNext());
	EXPECT_EQ(PreviewCompletionAction::Publish, state.Complete(kKeyC, true));
	EXPECT_TRUE(state.IsCurrent(kKeyC));
	state.MarkDelivered(kKeyC);
	EXPECT_EQ(PreviewAsyncPhase::Idle, state.Phase());
}

TEST(MarkdownPreviewAsyncState, ClosingIsTerminalAndNothingPublishesAfterIt)
{
	MarkdownPreviewAsyncState state;
	EXPECT_EQ(PreviewQueueAction::Queued, state.Queue(kKeyA));
	EXPECT_EQ(kKeyA, state.TakeNext());
	state.Close();

	EXPECT_EQ(PreviewAsyncPhase::Closed, state.Phase());
	EXPECT_EQ(PreviewQueueAction::RejectedClosed, state.Queue(kKeyB));
	EXPECT_FALSE(state.TakeNext().has_value());
	EXPECT_EQ(PreviewCompletionAction::DiscardClosed, state.Complete(kKeyA, true));
	state.MarkDelivered(kKeyA);
	state.MarkDeliveryFailed(kKeyA);
	EXPECT_EQ(PreviewAsyncPhase::Closed, state.Phase());
}

TEST(MarkdownPreviewAsyncState, AFailedDeliveryDoesNotReviveTheGeneration)
{
	MarkdownPreviewAsyncState state;
	EXPECT_EQ(PreviewQueueAction::Queued, state.Queue(kKeyA));
	EXPECT_EQ(kKeyA, state.TakeNext());
	EXPECT_EQ(PreviewCompletionAction::Publish, state.Complete(kKeyA, true));

	state.MarkDeliveryFailed(kKeyA);
	EXPECT_EQ(PreviewAsyncPhase::Failed, state.Phase());
	EXPECT_FALSE(state.InFlight().has_value());
	EXPECT_FALSE(state.Pending().has_value());

	// Delivering the same key again after the failure must do nothing.
	state.MarkDelivered(kKeyA);
	EXPECT_EQ(PreviewAsyncPhase::Failed, state.Phase());
}

} // namespace
} // namespace markdown
