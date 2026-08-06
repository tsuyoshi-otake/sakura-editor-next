/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionWorkbenchUi.h"

TEST(CExtensionDiagnostics, MapsDiagnosticsToUriUnderlinesAndProblemsAndCleansOwnership)
{
	CExtensionDiagnostics diagnostics;
	ASSERT_TRUE(diagnostics.Set(L"lint.one", 3, L"main", L"file:///a.md", {
		{ .range = { { 1, 2 }, { 1, 5 } }, .message = L"warning", .severity = EExtensionDiagnosticSeverity::Warning },
		{ .range = { { 0, 0 }, { 0, 1 } }, .message = L"error", .severity = EExtensionDiagnosticSeverity::Error },
	}));
	ASSERT_TRUE(diagnostics.Set(L"spell.two", 9, L"spelling", L"file:///a.md", {
		{ .range = { { 2, 1 }, { 2, 4 } }, .message = L"misspelling", .severity = EExtensionDiagnosticSeverity::Information },
	}));
	const auto underlines = diagnostics.ForUri(L"file:///a.md");
	ASSERT_EQ(3u, underlines.size());
	EXPECT_EQ(EExtensionDiagnosticSeverity::Error, underlines.front().severity);
	CExtensionProblemsPane pane(diagnostics);
	EXPECT_EQ(3u, pane.Snapshot(L"file:///a.md").size());
	diagnostics.RemoveOwnedBy(L"lint.one", 3);
	EXPECT_EQ(1u, pane.Snapshot().size());
	diagnostics.Clear();
	EXPECT_TRUE(pane.Snapshot().empty());
}

TEST(CExtensionProblemsPane, FiltersByUriWithoutDiscardingOtherDiagnostics)
{
	CExtensionDiagnostics diagnostics;
	ASSERT_TRUE(diagnostics.Set(L"sample", 1, L"main", L"file:///a", {
		{ .range = { { 0, 0 }, { 0, 1 } }, .message = L"a" },
	}));
	ASSERT_TRUE(diagnostics.Set(L"sample", 1, L"main", L"file:///b", {
		{ .range = { { 0, 0 }, { 0, 1 } }, .message = L"b" },
	}));
	CExtensionProblemsPane pane(diagnostics);
	EXPECT_EQ(1u, pane.Snapshot(L"file:///a").size());
	EXPECT_EQ(2u, pane.Snapshot().size());
}

TEST(CExtensionQuickInput, ResolvesSelectionCancelAndHostLossExactlyOnce)
{
	CExtensionQuickInput quickInput(2);
	auto first = quickInput.Show({
		.kind = EExtensionQuickInputKind::QuickPick, .extensionId = L"sample", .generation = 1,
		.canPickMany = true, .items = { { 0, L"A" }, { 1, L"B" } },
	});
	auto second = quickInput.Show({
		.kind = EExtensionQuickInputKind::InputBox, .extensionId = L"sample", .generation = 1,
	});
	ASSERT_TRUE(first && second);
	EXPECT_FALSE(quickInput.Show({ .extensionId = L"overflow", .generation = 1 }).has_value());
	EXPECT_TRUE(quickInput.Resolve(*first, { 0, 1 }, std::nullopt));
	EXPECT_FALSE(quickInput.Resolve(*first, {}, std::nullopt));
	quickInput.RemoveOwnedBy(L"sample", 1, EExtensionQuickInputState::HostLost);
	EXPECT_EQ(EExtensionQuickInputState::Accepted, quickInput.TakeCompletion(*first)->state);
	EXPECT_EQ(EExtensionQuickInputState::HostLost, quickInput.TakeCompletion(*second)->state);
}

TEST(CExtensionOutputChannel, EnforcesOwnershipAndBoundsRetainedOutput)
{
	CExtensionOutputChannel output(8);
	ASSERT_TRUE(output.Create({ .handle = L"out:1", .extensionId = L"sample", .generation = 2, .name = L"Sample" }));
	EXPECT_FALSE(output.Append(L"out:1", L"other", 2, L"bad"));
	ASSERT_TRUE(output.Append(L"out:1", L"sample", 2, L"1234567890"));
	ASSERT_TRUE(output.SetVisible(L"out:1", L"sample", 2, true));
	const auto channels = output.Snapshot();
	ASSERT_EQ(1u, channels.size());
	EXPECT_EQ(L"34567890", channels[0].text);
	EXPECT_EQ(2u, channels[0].droppedCharacters);
	EXPECT_TRUE(channels[0].visible);
	output.RemoveOwnedBy(L"sample", 2);
	EXPECT_TRUE(output.Snapshot().empty());
}

TEST(CExtensionProgress, HasExplicitStartReportCancelEndAndCleanupStates)
{
	CExtensionProgressCenter progress;
	ASSERT_TRUE(progress.Start({
		.handle = L"progress:1", .extensionId = L"sample", .generation = 5,
		.title = L"Working", .cancellable = true,
	}));
	EXPECT_TRUE(progress.Report(L"progress:1", L"sample", 5, L"Half", 50));
	EXPECT_TRUE(progress.RequestCancel(L"progress:1"));
	ASSERT_EQ(1u, progress.Snapshot().size());
	EXPECT_TRUE(progress.Snapshot()[0].cancelRequested);
	EXPECT_TRUE(progress.End(L"progress:1", L"sample", 5));
	EXPECT_TRUE(progress.Snapshot().empty());
}

TEST(CExtensionHoverCenter, HasNoResultUntilPublishedAndClearRestoresNoResult)
{
	CExtensionHoverCenter hover;

	// Nothing published yet: distinct from a real "no content" answer, which is
	// SExtensionHoverResult{.empty = true} carried inside a real snapshot, not
	// nullopt (see the struct's own doc comment in CExtensionWorkbenchUi.h).
	EXPECT_FALSE(hover.Snapshot().has_value());

	SExtensionHoverResult result;
	result.documentId = { .editorProcessId = 4321, .localDocumentId = 1 };
	result.position = { .line = 7, .character = 3 };
	result.markdown = L"**bold**";
	result.empty = false;
	hover.Publish(result);

	const auto published = hover.Snapshot();
	ASSERT_TRUE(published.has_value());
	EXPECT_EQ(result.documentId, published->documentId);
	EXPECT_EQ(result.position, published->position);
	EXPECT_EQ(L"**bold**", published->markdown);
	EXPECT_FALSE(published->empty);

	// A later publish (e.g. a subsequent request at a new position) replaces the
	// prior snapshot outright; the center never merges two results.
	SExtensionHoverResult empty;
	empty.documentId = result.documentId;
	empty.position = { .line = 9, .character = 0 };
	empty.empty = true;
	hover.Publish(empty);
	const auto replaced = hover.Snapshot();
	ASSERT_TRUE(replaced.has_value());
	EXPECT_TRUE(replaced->markdown.empty());
	EXPECT_TRUE(replaced->empty);

	// Clear (mouse left the word, or a new request superseded this one before any
	// response arrived) returns to "no result yet", not to an empty-but-present one.
	hover.Clear();
	EXPECT_FALSE(hover.Snapshot().has_value());
}
