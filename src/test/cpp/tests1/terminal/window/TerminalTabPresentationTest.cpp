#include "pch.h"
#include "terminal/window/TerminalTabPresentation.h"

#include <string>

namespace {

using terminal::IsRecognizedAgentCliTitle;
using terminal::ResolveTerminalTabPresentation;
using terminal::TerminalTabPresentationContext;
using terminal::TerminalTabPresentationSettings;

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

TEST(TerminalTabPresentation, UnknownVariablesStayLiteral)
{
	TerminalTabPresentationSettings settings;
	settings.titleTemplate = L"${notAVariable}";
	EXPECT_EQ(L"${notAVariable}", ResolveTerminalTabPresentation(settings, PwshContext()).title);
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
