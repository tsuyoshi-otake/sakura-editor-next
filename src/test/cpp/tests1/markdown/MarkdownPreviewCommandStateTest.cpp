/*! @file */
#include "pch.h"

#include "markdown/MarkdownPreviewCommandState.h"
#include "window/DocumentTabActionLayout.h"

#include <array>
#include <string>

namespace markdown {
namespace {

constexpr wchar_t kReadme[] = L"C:\\work\\README.md";
constexpr wchar_t kNotes[] = L"C:\\work\\NOTES.md";

//! Every command this shell registers, in the upstream order of PARITY.md.
constexpr std::array<MarkdownPreviewCommand, 10> kAllCommands{
	MarkdownPreviewCommand::ShowPreview,
	MarkdownPreviewCommand::ShowPreviewToSide,
	MarkdownPreviewCommand::ShowLockedPreviewToSide,
	MarkdownPreviewCommand::ShowSource,
	MarkdownPreviewCommand::ShowPreviewSecuritySelector,
	MarkdownPreviewCommand::Refresh,
	MarkdownPreviewCommand::ToggleLock,
	MarkdownPreviewCommand::ReopenAsPreview,
	MarkdownPreviewCommand::ReopenAsSource,
	MarkdownPreviewCommand::TogglePreview,
};

TEST(MarkdownPreviewCommandState, EveryCommandTerminatesInATypedOutcomeFromAFreshState)
{
	for (const auto command : kAllCommands) {
		MarkdownPreviewCommandState state;
		const auto result = state.Apply(command, kReadme, true);
		switch (command) {
		case MarkdownPreviewCommand::ShowPreview:
		case MarkdownPreviewCommand::ReopenAsPreview:
		case MarkdownPreviewCommand::TogglePreview:
			EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, result.outcome);
			EXPECT_TRUE(state.IsVisible());
			EXPECT_EQ(MarkdownPreviewPlacement::CurrentEditorGroup, state.Placement());
			break;
		case MarkdownPreviewCommand::ShowPreviewToSide:
		case MarkdownPreviewCommand::ShowLockedPreviewToSide:
			EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup, result.outcome);
			EXPECT_FALSE(state.IsVisible());
			break;
		case MarkdownPreviewCommand::ShowPreviewSecuritySelector:
			EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSecuritySelector, result.outcome);
			break;
		case MarkdownPreviewCommand::ShowSource:
		case MarkdownPreviewCommand::ReopenAsSource:
		case MarkdownPreviewCommand::Refresh:
		case MarkdownPreviewCommand::ToggleLock:
			EXPECT_EQ(MarkdownPreviewCommandOutcome::NotApplicable, result.outcome);
			EXPECT_FALSE(state.IsVisible());
			break;
		}
	}
}

TEST(MarkdownPreviewCommandState, NoCommandOpensAPreviewForANonMarkdownOrUnnamedSource)
{
	for (const auto command : kAllCommands) {
		MarkdownPreviewCommandState notMarkdown;
		(void)notMarkdown.Apply(command, kReadme, false);
		EXPECT_FALSE(notMarkdown.IsVisible());

		MarkdownPreviewCommandState unnamed;
		(void)unnamed.Apply(command, L"", true);
		EXPECT_FALSE(unnamed.IsVisible());
	}
}

/*!
	The to-side commands are the boundary the whole placement model exists for.
	They must stay a typed unsupported result until a real second EditorGroup
	exists; aliasing them to the Sakura sibling pane is the exact divergence the
	repository forbids.
*/
TEST(MarkdownPreviewCommandState, ToSideCommandsNeverAliasTheNativeSiblingPane)
{
	MarkdownPreviewCommandState state;
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup,
		state.Apply(MarkdownPreviewCommand::ShowPreviewToSide, kReadme, true).outcome);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnsupportedSideEditorGroup,
		state.Apply(MarkdownPreviewCommand::ShowLockedPreviewToSide, kReadme, true).outcome);
	EXPECT_FALSE(state.IsVisible());
	EXPECT_NE(MarkdownPreviewPlacement::NativeSiblingPane, state.Placement());

	// With a host that really owns a side group, the same command applies there.
	const MarkdownPreviewHostCapabilities sideGroup{ true, false };
	const auto result = state.Apply(MarkdownPreviewCommand::ShowLockedPreviewToSide,
		kReadme, true, sideGroup);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, result.outcome);
	EXPECT_EQ(MarkdownPreviewPlacement::SideEditorGroup, state.Placement());
	EXPECT_TRUE(state.IsLocked());
}

TEST(MarkdownPreviewCommandState, TheDocumentTabButtonDispatchesTheSakuraSiblingToggle)
{
	// The button's declared action id must name the concept it actually invokes.
	EXPECT_STREQ(L"sakura.toggleMarkdownSiblingPreview", tabbar::kMarkdownPreviewCommandId);

	MarkdownPreviewCommandState state;
	const auto opened = state.ToggleNativeSibling(kReadme, true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, opened.outcome);
	EXPECT_TRUE(opened.presentationChanged);
	EXPECT_TRUE(state.IsVisible());
	EXPECT_EQ(MarkdownPreviewPlacement::NativeSiblingPane, state.Placement());
	EXPECT_EQ(std::wstring(kReadme), state.SourceIdentity());

	const auto closed = state.ToggleNativeSibling(kReadme, true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, closed.outcome);
	EXPECT_FALSE(state.IsVisible());
}

TEST(MarkdownPreviewCommandState, LockedPreviewKeepsItsSourceAndRefusesAForeignRefresh)
{
	MarkdownPreviewCommandState state;
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied,
		state.Apply(MarkdownPreviewCommand::ShowPreview, kReadme, true).outcome);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied,
		state.Apply(MarkdownPreviewCommand::ToggleLock, kReadme, true).outcome);
	EXPECT_TRUE(state.IsLocked());

	// A locked preview ignores the newly active document.
	EXPECT_FALSE(state.ObserveActiveSource(kNotes, true));
	EXPECT_EQ(std::wstring(kReadme), state.SourceIdentity());
	EXPECT_EQ(MarkdownPreviewCommandOutcome::UnavailableLockedSource,
		state.Apply(MarkdownPreviewCommand::Refresh, kNotes, true).outcome);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::RefreshRequested,
		state.Apply(MarkdownPreviewCommand::Refresh, kReadme, true).outcome);

	// Unlocking adopts the active document again.
	const auto unlocked = state.Apply(MarkdownPreviewCommand::ToggleLock, kNotes, true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, unlocked.outcome);
	EXPECT_TRUE(unlocked.identityChanged);
	EXPECT_FALSE(state.IsLocked());
	EXPECT_EQ(std::wstring(kNotes), state.SourceIdentity());
}

TEST(MarkdownPreviewCommandState, DynamicPreviewFollowsTheActiveMarkdownAndClosesOnANonMarkdownSource)
{
	MarkdownPreviewCommandState state;
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied,
		state.Apply(MarkdownPreviewCommand::ShowPreview, kReadme, true).outcome);

	EXPECT_FALSE(state.ObserveActiveSource(kReadme, true));
	EXPECT_TRUE(state.ObserveActiveSource(kNotes, true));
	EXPECT_EQ(std::wstring(kNotes), state.SourceIdentity());

	EXPECT_TRUE(state.ObserveActiveSource(L"C:\\work\\main.cpp", false));
	EXPECT_FALSE(state.IsVisible());
}

TEST(MarkdownPreviewCommandState, TogglingBackToSourceClearsTheLock)
{
	MarkdownPreviewCommandState state;
	(void)state.Apply(MarkdownPreviewCommand::ShowPreview, kReadme, true);
	(void)state.Apply(MarkdownPreviewCommand::ToggleLock, kReadme, true);

	const auto result = state.Apply(MarkdownPreviewCommand::TogglePreview, kReadme, true);
	EXPECT_EQ(MarkdownPreviewCommandOutcome::Applied, result.outcome);
	EXPECT_TRUE(result.presentationChanged);
	EXPECT_TRUE(result.lockChanged);
	EXPECT_FALSE(state.IsVisible());
	EXPECT_FALSE(state.IsLocked());
}

TEST(MarkdownPreviewCommandState, ResetReturnsToTheSourcePresentation)
{
	MarkdownPreviewCommandState state;
	(void)state.ToggleNativeSibling(kReadme, true);
	state.Reset();
	EXPECT_FALSE(state.IsVisible());
	EXPECT_FALSE(state.IsLocked());
	EXPECT_EQ(MarkdownPreviewPlacement::CurrentEditorGroup, state.Placement());
	EXPECT_TRUE(state.SourceIdentity().empty());
}

} // namespace
} // namespace markdown
