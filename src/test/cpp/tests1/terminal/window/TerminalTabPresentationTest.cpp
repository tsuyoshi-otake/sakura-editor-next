#include "pch.h"
#include "terminal/window/TerminalTabPresentation.h"

#include <string>

namespace {

using terminal::IsRecognizedAgentCliTitle;
using terminal::ResolveTerminalTabDropdownPresentation;
using terminal::ResolveTerminalTabListPresentation;
using terminal::ResolveTerminalTabPresentation;
using terminal::ShouldShowTerminalTabs;
using terminal::TerminalTabPresentationContext;
using terminal::TerminalTabPresentationSettings;
using terminal::TerminalTabPresentationSnapshot;
using terminal::TerminalTabsHideCondition;
using terminal::TerminalTabsLocation;
using terminal::TerminalTabsShowCondition;

//! A pwsh-shaped context: the process announces its working directory through
//! OSC 0, which is what made every tab render as a path before this resolver.
TerminalTabPresentationContext PwshContext()
{
	TerminalTabPresentationContext context;
	context.processName = L"pwsh";
	context.sequenceTitle = L"C:\\Program Files\\PowerShell\\7";
	context.recognizedAgentCli = IsRecognizedAgentCliTitle(context.sequenceTitle);
	return context;
}

TerminalTabPresentationContext AgentCliContext()
{
	TerminalTabPresentationContext context;
	context.processName = L"pwsh";
	context.sequenceTitle = L"Claude Code";
	context.recognizedAgentCli = IsRecognizedAgentCliTitle(context.sequenceTitle);
	return context;
}

TEST(TerminalTabPresentation, DefaultTitleUsesProcessNotSequence)
{
	const auto resolved = ResolveTerminalTabPresentation({}, PwshContext());
	EXPECT_EQ(L"pwsh", resolved.title);
}

TEST(TerminalTabPresentation, ExplicitSequenceUsesOscTitle)
{
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${sequence}";
	const auto resolved = ResolveTerminalTabPresentation(settings, PwshContext());
	EXPECT_EQ(L"C:\\Program Files\\PowerShell\\7", resolved.title);
}

TEST(TerminalTabPresentation, KnownAgentCliUsesSequenceWhenAllowed)
{
	const auto resolved = ResolveTerminalTabPresentation({}, AgentCliContext());
	EXPECT_EQ(L"Claude Code", resolved.title);
}

TEST(TerminalTabPresentation, KnownAgentCliUsesConfiguredTitleWhenDisallowed)
{
	TerminalTabPresentationSettings settings;
	settings.allowAgentCliTitle = false;
	const auto resolved = ResolveTerminalTabPresentation(settings, AgentCliContext());
	EXPECT_EQ(L"pwsh", resolved.title);
}

TEST(TerminalTabPresentation, PathLikeOscTitleIsNotAnAgentCli)
{
	EXPECT_FALSE(IsRecognizedAgentCliTitle(L"C:\\Program Files\\PowerShell\\7"));
	EXPECT_FALSE(IsRecognizedAgentCliTitle(L"/home/user/src"));
	EXPECT_FALSE(IsRecognizedAgentCliTitle(L""));
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"  claude code  "));
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"Claude Code: project"));
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"Gemini: repo"));
}

TEST(TerminalTabPresentation, AgentCliRecognitionMatchesUpstreamTitlePatterns)
{
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"ClaudeCode"));
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"command   code"));
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"GitHub Copilot"));
	EXPECT_TRUE(IsRecognizedAgentCliTitle(L"Gemini CLI"));
	EXPECT_FALSE(IsRecognizedAgentCliTitle(L"Codex"));
	EXPECT_FALSE(IsRecognizedAgentCliTitle(L"notcopilotbinary"));
}

TEST(TerminalTabPresentation, DropdownAndTabListUseSameResolvedTitle)
{
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${process}";
	settings.descriptionTemplate = L"${process}";
	const TerminalTabPresentationSnapshot snapshot {
		L"pwsh", L"PowerShell", L"", L"C:\\workspace" };

	// CTerminalTool's list painter and session dropdown call distinct surface
	// wrappers over the same snapshot projection. A mutant that changes one
	// wrapper's source or fallback therefore changes this observable comparison.
	const auto listPresentation = ResolveTerminalTabListPresentation(settings, snapshot);
	const auto dropdownPresentation = ResolveTerminalTabDropdownPresentation(settings, snapshot);
	EXPECT_EQ(listPresentation, dropdownPresentation);
	EXPECT_EQ(L"pwsh", listPresentation.title);
	EXPECT_EQ(L"pwsh", dropdownPresentation.description);
}

TEST(TerminalTabPresentation, TemplateControlCharactersAreRemoved)
{
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"left\r${process}${separator}\nright\t";
	settings.separator = L"\r - \n";
	auto context = PwshContext();
	context.cwdFolder = L"repo";
	EXPECT_EQ(L"leftpwsh - right", ResolveTerminalTabPresentation(settings, context).title);
}

TEST(TerminalTabPresentation, TypedTabVisibilityUsesTheRequestedCount)
{
	TerminalTabPresentationSettings settings;
	settings.hideCondition = TerminalTabsHideCondition::SingleTerminal;
	EXPECT_FALSE(ShouldShowTerminalTabs(settings, 0, 0));
	EXPECT_FALSE(ShouldShowTerminalTabs(settings, 1, 1));
	EXPECT_TRUE(ShouldShowTerminalTabs(settings, 2, 1));

	settings.hideCondition = TerminalTabsHideCondition::SingleGroup;
	EXPECT_FALSE(ShouldShowTerminalTabs(settings, 3, 1));
	EXPECT_TRUE(ShouldShowTerminalTabs(settings, 2, 2));

	settings.hideCondition = TerminalTabsHideCondition::Never;
	EXPECT_TRUE(ShouldShowTerminalTabs(settings, 0, 0));
	settings.tabsEnabled = false;
	EXPECT_FALSE(ShouldShowTerminalTabs(settings, 10, 10));
}

TEST(TerminalTabPresentation, TypedConfigurationParsingRejectsUnknownValues)
{
	EXPECT_EQ(TerminalTabsHideCondition::SingleGroup,
		terminal::ParseTerminalTabsHideCondition(L"singleGroup"));
	EXPECT_EQ(TerminalTabsLocation::Left, terminal::ParseTerminalTabsLocation(L"left"));
	EXPECT_EQ(TerminalTabsShowCondition::SingleTerminalOrNarrow,
		terminal::ParseTerminalTabsShowCondition(L"singleTerminalOrNarrow"));
	EXPECT_FALSE(terminal::ParseTerminalTabsHideCondition(L"groups"));
	EXPECT_FALSE(terminal::ParseTerminalTabsLocation(L"middle"));
	EXPECT_FALSE(terminal::ParseTerminalTabsShowCondition(L"compact"));
}

TEST(TerminalTabPresentation, ShowActiveAndActionsUseUpstreamGroupConditions)
{
	EXPECT_TRUE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::Always, 4, false));
	EXPECT_TRUE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::SingleTerminal, 1, false));
	EXPECT_FALSE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::SingleTerminal, 2, true));
	EXPECT_TRUE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::SingleTerminalOrNarrow, 2, true));
	EXPECT_FALSE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::Never, 1, true));

	// Split panes are multiple terminal instances in one terminal group, so
	// the header actions remain visible for the default policy.
	EXPECT_TRUE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::SingleTerminalOrNarrow, 1, false));
	EXPECT_FALSE(terminal::ShouldShowTerminalTabPolicy(
		TerminalTabsShowCondition::SingleTerminalOrNarrow, 2, false));
}

TEST(TerminalTabPresentation, SplitGroupKeepsActiveHeaderIdentityVisible)
{
	EXPECT_TRUE(terminal::ShouldShowActiveTerminalHeader(
		TerminalTabsShowCondition::SingleTerminalOrNarrow, 2, false, true));
	EXPECT_FALSE(terminal::ShouldShowActiveTerminalHeader(
		TerminalTabsShowCondition::SingleTerminalOrNarrow, 2, false, false));

	// An explicit policy is not overridden by the split-group projection.
	EXPECT_FALSE(terminal::ShouldShowActiveTerminalHeader(
		TerminalTabsShowCondition::SingleTerminal, 2, false, true));
	EXPECT_FALSE(terminal::ShouldShowActiveTerminalHeader(
		TerminalTabsShowCondition::Never, 2, false, true));
	EXPECT_TRUE(terminal::ShouldShowActiveTerminalHeader(
		TerminalTabsShowCondition::Always, 2, false, true));
}

TEST(TerminalTabPresentation, RowGeometryStaysBoundedAndDropsDescriptionFirst)
{
	for( int rowWidth = 1; rowWidth <= 240; ++rowWidth ) {
		const terminal::TerminalTabRowLayoutInput input {
			{ 10, 20, 10 + rowWidth, 48 }, 144, true, true, true, true };
		const auto layout = terminal::CalculateTerminalTabRowLayout(input);
		const auto inside = [row = input.Row()](const terminal::TerminalTabPresentationRect& rect) {
			if( rect.Width() == 0 || rect.Height() == 0 ) return true;
			return rect.left >= row.left && rect.top >= row.top
				&& rect.right <= row.right && rect.bottom <= row.bottom
				&& rect.left <= rect.right && rect.top <= rect.bottom;
		};
		EXPECT_TRUE(inside(layout.SplitIndent()));
		EXPECT_TRUE(inside(layout.Icon()));
		EXPECT_TRUE(inside(layout.Title()));
		EXPECT_TRUE(inside(layout.Description()));
		EXPECT_TRUE(inside(layout.Status()));
		if( layout.Title().Width() > 0 && layout.Description().Width() > 0 ) {
			EXPECT_LE(layout.Title().right, layout.Description().left);
		}
	}
	const terminal::TerminalTabRowLayoutInput narrow {
		{ 0, 0, 40, 24 }, 96, false, true, true, true };
	const auto narrowLayout = terminal::CalculateTerminalTabRowLayout(narrow);
	EXPECT_GT(narrowLayout.Title().Width(), 0);
	EXPECT_EQ(0, narrowLayout.Status().Width());
}

TEST(TerminalTabPresentation, EmptyVariablesDoNotLeaveSeparators)
{
	// The default description with every variable unavailable resolves empty
	// rather than to a bare separator.
	const auto resolved = ResolveTerminalTabPresentation({}, PwshContext());
	EXPECT_TRUE(resolved.description.empty());
}

TEST(TerminalTabPresentation, EmptyMiddleVariableCollapsesAdjacentSeparators)
{
	auto context = PwshContext();
	context.task = L"Build";
	context.cwdFolder = L"repo";
	const auto resolved = ResolveTerminalTabPresentation({}, context);
	EXPECT_EQ(L"Build - repo", resolved.description);
}

TEST(TerminalTabPresentation, LeadingAndTrailingSeparatorsAreTrimmed)
{
	auto context = PwshContext();
	context.cwdFolder = L"repo";
	EXPECT_EQ(L"repo", ResolveTerminalTabPresentation({}, context).description);

	auto trailing = PwshContext();
	trailing.task = L"Build";
	EXPECT_EQ(L"Build", ResolveTerminalTabPresentation({}, trailing).description);
}

TEST(TerminalTabPresentation, DescriptionMayResolveToEmpty)
{
	TerminalTabPresentationSettings settings;
	settings.descriptionTemplate = L"${cwdFolder}";
	EXPECT_TRUE(ResolveTerminalTabPresentation(settings, PwshContext()).description.empty());
}

TEST(TerminalTabPresentation, EmptyTitleFallsBackToProcess)
{
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${cwdFolder}";
	EXPECT_EQ(L"pwsh", ResolveTerminalTabPresentation(settings, PwshContext()).title);
}

TEST(TerminalTabPresentation, ControlCharactersAreRemoved)
{
	auto context = PwshContext();
	context.sequenceTitle = L"Cl\ram\nSpoof\tTab";
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${sequence}";
	EXPECT_EQ(L"ClamSpoofTab", ResolveTerminalTabPresentation(settings, context).title);
}

TEST(TerminalTabPresentation, OutputIsBounded)
{
	auto context = PwshContext();
	context.sequenceTitle.assign(terminal::kMaximumTerminalTabTextLength * 4, L'x');
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${sequence}";
	EXPECT_EQ(terminal::kMaximumTerminalTabTextLength,
		ResolveTerminalTabPresentation(settings, context).title.size());
}

TEST(TerminalTabPresentation, UnknownVariablesAreOmittedAndTitleFallsBack)
{
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${notAVariable}";
	EXPECT_EQ(L"pwsh", ResolveTerminalTabPresentation(settings, PwshContext()).title);
}

TEST(TerminalTabPresentation, CwdFallsBackToTheInitialWorkingDirectory)
{
	auto context = PwshContext();
	context.initialCwd = L"C:\\workspace";
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${cwd}";
	EXPECT_EQ(L"C:\\workspace", ResolveTerminalTabPresentation(settings, context).title);

	context.currentCwd = L"C:\\workspace\\src";
	EXPECT_EQ(L"C:\\workspace\\src", ResolveTerminalTabPresentation(settings, context).title);
}

TEST(TerminalTabPresentation, StaticTextKeepsItsOwnSpacing)
{
	auto context = PwshContext();
	context.shellType = L"pwsh";
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${process} (${shellType})";
	EXPECT_EQ(L"pwsh (pwsh)", ResolveTerminalTabPresentation(settings, context).title);
}

TEST(TerminalTabPresentation, ResolutionIsDeterministic)
{
	auto context = PwshContext();
	context.task = L"Build";
	context.cwdFolder = L"repo";
	EXPECT_EQ(ResolveTerminalTabPresentation({}, context), ResolveTerminalTabPresentation({}, context));
}

} // namespace
