#include "pch.h"
#include "terminal/window/TerminalAgentPresence.h"
#include "terminal/window/TerminalTabPresentation.h"

#include <array>
#include <string>
#include <vector>

namespace {

using terminal::TerminalAgentDisplayName;
using terminal::TerminalAgentKind;
using terminal::TerminalAgentKindFromImageFileName;
using terminal::TerminalAgentKindFromTitle;
using terminal::TerminalAgentPresenceTracker;
using terminal::TerminalAgentSessionTitle;
using terminal::TerminalBackendJobProcess;
using terminal::TerminalBackendJobProcesses;

TerminalBackendJobProcess Process( std::uint32_t processId, std::uint64_t creationTime,
	std::wstring imageFileName )
{
	return { processId, creationTime, std::move(imageFileName) };
}

TerminalBackendJobProcesses Observation( std::vector<TerminalBackendJobProcess> processes )
{
	return { std::move(processes), false };
}

} // namespace

TEST(TerminalAgentPresence, ImageFileNamesAreExactAndCaseInsensitive)
{
	EXPECT_EQ(TerminalAgentKindFromImageFileName(L"claude.exe"), TerminalAgentKind::ClaudeCode);
	EXPECT_EQ(TerminalAgentKindFromImageFileName(L"CLAUDE.EXE"), TerminalAgentKind::ClaudeCode);
	EXPECT_EQ(TerminalAgentKindFromImageFileName(L"codex.exe"), TerminalAgentKind::Codex);
	EXPECT_EQ(TerminalAgentKindFromImageFileName(L"Devin.exe"), TerminalAgentKind::Devin);
	EXPECT_EQ(TerminalAgentKindFromImageFileName(L"OpenCode.exe"), TerminalAgentKind::OpenCode);
	// Node-hosted CLIs and lookalikes prove nothing by image name.
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L"node.exe"));
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L"opencode"));
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L"pwsh.exe"));
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L"claude"));
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L"claude.exe.bak"));
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L"myclaude.exe"));
	EXPECT_FALSE(TerminalAgentKindFromImageFileName(L""));
}

TEST(TerminalAgentPresence, TitleUsesTheUpstreamPatternsAndRejectsPaths)
{
	EXPECT_EQ(TerminalAgentKindFromTitle(L"Claude Code"), TerminalAgentKind::ClaudeCode);
	EXPECT_EQ(TerminalAgentKindFromTitle(L"  claudecode: project "), TerminalAgentKind::ClaudeCode);
	EXPECT_EQ(TerminalAgentKindFromTitle(L"Command Code"), TerminalAgentKind::CommandCode);
	EXPECT_EQ(TerminalAgentKindFromTitle(L"GitHub Copilot"), TerminalAgentKind::Copilot);
	EXPECT_EQ(TerminalAgentKindFromTitle(L"Gemini CLI"), TerminalAgentKind::Gemini);
	// A shell reporting its directory is never an agent, even when the
	// directory happens to contain a product name.
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"C:\\src\\claude code"));
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"/home/me/gemini"));
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"\\\\server\\copilot"));
	// Word boundaries: a longer word is not the product name.
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"copilots"));
	// Codex and Devin emit no OSC title, so no title proves them.
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"codex"));
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"devin"));
	EXPECT_FALSE(TerminalAgentKindFromTitle(L""));
	EXPECT_FALSE(TerminalAgentKindFromTitle(std::wstring(terminal::kMaximumTerminalAgentTitleLength + 1, L'x')));
}

TEST(TerminalAgentPresence, TheEarliestCreatedAgentProcessWinsOverLaterAgentsAndTheTitle)
{
	TerminalAgentPresenceTracker tracker;
	EXPECT_TRUE(tracker.ApplyTitle(1, L"Gemini"));
	// Claude Code started first and ran codex as a tool: still Claude Code, even
	// though codex is listed first and the title names another agent.
	EXPECT_TRUE(tracker.ApplyJobObservation(1, Observation({
		Process(40, 300, L"codex.exe"), Process(10, 100, L"pwsh.exe"), Process(20, 200, L"claude.exe") })));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::ClaudeCode);
	// Both agent processes stay tracked, so either one's exit is a hint.
	EXPECT_EQ(tracker.AgentProcessIds(1), (std::vector<std::uint32_t>{ 40, 20 }));
	// No agent process: the title decides.
	EXPECT_TRUE(tracker.ApplyJobObservation(1, Observation({
		Process(10, 100, L"pwsh.exe"), Process(11, 110, L"node.exe") })));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::Gemini);
	EXPECT_TRUE(tracker.ApplyTitle(1, L"C:\\work"));
	EXPECT_FALSE(tracker.Presence(1));
}

TEST(TerminalAgentPresence, TrackerReportsOnlyResolvedPresenceChanges)
{
	TerminalAgentPresenceTracker tracker;
	EXPECT_FALSE(tracker.ApplyTitle(1, L"C:\\work"));
	EXPECT_FALSE(tracker.Presence(1));

	EXPECT_TRUE(tracker.ApplyJobObservation(1, Observation({
		Process(10, 100, L"pwsh.exe"), Process(20, 200, L"devin.exe") })));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::Devin);
	EXPECT_EQ(tracker.AgentProcessIds(1), std::vector<std::uint32_t>{ 20 });

	// Same resolved kind: no change, even though the process set differs.
	EXPECT_FALSE(tracker.ApplyJobObservation(1, Observation({
		Process(20, 200, L"devin.exe"), Process(30, 300, L"node.exe") })));
	EXPECT_FALSE(tracker.ApplyTitle(1, L"C:\\other"));

	// The agent exited: only the shell remains.
	EXPECT_TRUE(tracker.ApplyJobObservation(1, Observation({ Process(10, 100, L"pwsh.exe") })));
	EXPECT_FALSE(tracker.Presence(1));
	EXPECT_TRUE(tracker.AgentProcessIds(1).empty());

	// A title-only agent appears and disappears with its title.
	EXPECT_TRUE(tracker.ApplyTitle(1, L"Claude Code"));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::ClaudeCode);
	EXPECT_TRUE(tracker.ApplyTitle(1, L"C:\\work"));
	EXPECT_FALSE(tracker.Presence(1));
}

TEST(TerminalAgentPresence, AMissingObservationClearsProcessEvidenceButKeepsTheTitle)
{
	TerminalAgentPresenceTracker tracker;
	EXPECT_TRUE(tracker.ApplyJobObservation(2, Observation({ Process(20, 200, L"codex.exe") })));
	EXPECT_EQ(tracker.Presence(2), TerminalAgentKind::Codex);
	// The session is no longer live: nothing observable remains.
	EXPECT_TRUE(tracker.ApplyJobObservation(2, std::nullopt));
	EXPECT_FALSE(tracker.Presence(2));
	EXPECT_TRUE(tracker.AgentProcessIds(2).empty());

	EXPECT_TRUE(tracker.ApplyTitle(2, L"Gemini"));
	EXPECT_FALSE(tracker.ApplyJobObservation(2, std::nullopt));
	EXPECT_EQ(tracker.Presence(2), TerminalAgentKind::Gemini);
}

TEST(TerminalAgentPresence, RetainDropsClosedTabsAndReportsLostPresence)
{
	TerminalAgentPresenceTracker tracker;
	EXPECT_TRUE(tracker.ApplyTitle(1, L"Claude Code"));
	EXPECT_FALSE(tracker.ApplyTitle(2, L"C:\\work"));
	EXPECT_TRUE(tracker.ApplyJobObservation(3, Observation({ Process(30, 300, L"devin.exe") })));

	// Dropping a tab without presence is not a presence change.
	const std::array<std::uint64_t, 2> withoutTwo{ 1, 3 };
	EXPECT_FALSE(tracker.Retain(withoutTwo));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::ClaudeCode);
	EXPECT_EQ(tracker.Presence(3), TerminalAgentKind::Devin);

	const std::array<std::uint64_t, 1> onlyThree{ 3 };
	EXPECT_TRUE(tracker.Retain(onlyThree));
	EXPECT_FALSE(tracker.Presence(1));
	// The dropped tab is erased, not merely blanked: retaining again reports no
	// further loss, and a new non-agent title for its ID is no change either.
	EXPECT_FALSE(tracker.Retain(onlyThree));
	EXPECT_FALSE(tracker.ApplyTitle(1, L"C:\\work"));
	EXPECT_EQ(tracker.Presence(3), TerminalAgentKind::Devin);

	// Closing the last tab drops it as well.
	EXPECT_TRUE(tracker.Retain({}));
	EXPECT_FALSE(tracker.Retain({}));
	EXPECT_FALSE(tracker.Presence(3));
	EXPECT_TRUE(tracker.AgentProcessIds(3).empty());
}

TEST(TerminalAgentPresence, PiIsRecognizedByItsOwnTitleWhileTheTabLabelKeepsVSCodesList)
{
	TerminalAgentPresenceTracker tracker;
	// Pi 1.3.0 writes `<U+03C0> - <folder>` and `<U+03C0> - <session> - <folder>`.
	EXPECT_TRUE(tracker.ApplyTitle(1, L"\u03C0 - sakura-editor-next"));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::Pi);
	// Pi's prefix wins over another product's name inside the folder name. The
	// report is the row's hover text changing; the presence stays Pi.
	EXPECT_TRUE(tracker.ApplyTitle(1, L" \u03C0 - refactor - gemini "));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::Pi);
	EXPECT_EQ(tracker.SessionTitle(1), L"\u03C0 - refactor - gemini");
	// The shell's directory title after Pi exits clears it.
	EXPECT_TRUE(tracker.ApplyTitle(1, L"C:\\work"));
	EXPECT_FALSE(tracker.Presence(1));
	// Only Pi's own prefix counts; job evidence still outranks it.
	EXPECT_FALSE(tracker.ApplyTitle(2, L"\u03C0"));
	EXPECT_FALSE(tracker.ApplyTitle(2, L"pi - work"));
	EXPECT_FALSE(tracker.ApplyTitle(2, L"x \u03C0 - work"));
	EXPECT_FALSE(tracker.ApplyTitle(2, std::wstring(L"\u03C0 - ")
		+ std::wstring(terminal::kMaximumTerminalAgentTitleLength, L'x')));
	EXPECT_TRUE(tracker.ApplyJobObservation(3, Observation({ Process(30, 300, L"claude.exe") })));
	// Only the hover text changes here; the job's Claude Code keeps the row.
	EXPECT_TRUE(tracker.ApplyTitle(3, L"\u03C0 - work"));
	EXPECT_EQ(tracker.Presence(3), TerminalAgentKind::ClaudeCode);
	EXPECT_FALSE(tracker.ApplyTitle(3, L"\u03C0 - work"));
	// Fork extension only: VS Code's tab label allow-list does not know Pi.
	EXPECT_FALSE(TerminalAgentKindFromTitle(L"\u03C0 - sakura-editor-next"));
	EXPECT_FALSE(terminal::IsRecognizedAgentCliTitle(L"\u03C0 - sakura-editor-next"));
	EXPECT_TRUE(terminal::IsRecognizedAgentCliTitle(L"Claude Code"));
}

TEST(TerminalAgentPresence, SessionTitleStripsStatusGlyphsAndRejectsShellTitles)
{
	// Claude Code: U+2733 when idle, U+25D0..U+25D3 while working.
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u2733 Claude Code"), L"Claude Code");
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u25D0 Fix login bug"), L"Fix login bug");
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u25D3 Fix login bug"), L"Fix login bug");
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u2810 Fix login bug"), L"Fix login bug");
	EXPECT_EQ(TerminalAgentSessionTitle(L" \u00B7 * Fix login bug \u3000"), L"Fix login bug");
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u25D0 \u30ED\u30B0\u30A4\u30F3\u4FEE\u6B63"),
		L"\u30ED\u30B0\u30A4\u30F3\u4FEE\u6B63");
	// Controls go, as in the tab label; a glyph after text is part of the name.
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u25D0 Fix\x07 bug \u2733"), L"Fix bug \u2733");
	// Pi's own title is kept whole: U+03C0 is a letter, not a status glyph.
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u03C0 - session - repo"), L"\u03C0 - session - repo");

	EXPECT_TRUE(TerminalAgentSessionTitle(L"").empty());
	EXPECT_TRUE(TerminalAgentSessionTitle(L"\u2733").empty());
	EXPECT_TRUE(TerminalAgentSessionTitle(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe").empty());
	EXPECT_TRUE(TerminalAgentSessionTitle(L"\u25D0 C:\\work").empty());
	EXPECT_TRUE(TerminalAgentSessionTitle(L"/home/user").empty());

	const std::wstring bounded(terminal::kMaximumTerminalAgentTitleLength, L'a');
	EXPECT_EQ(TerminalAgentSessionTitle(L"\u2733 " + bounded), bounded);
	EXPECT_TRUE(TerminalAgentSessionTitle(bounded + L"a").empty());
}

TEST(TerminalAgentPresence, TrackerReportsSessionTitleChangesOfAPresentAgentOnly)
{
	TerminalAgentPresenceTracker tracker;
	// No agent yet: the title is kept but nothing is reported.
	EXPECT_FALSE(tracker.ApplyTitle(1, L"\u25D0 Fix login bug"));
	EXPECT_EQ(tracker.SessionTitle(1), L"Fix login bug");

	EXPECT_TRUE(tracker.ApplyJobObservation(1, Observation({
		Process(10, 100, L"pwsh.exe"), Process(20, 200, L"claude.exe") })));
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::ClaudeCode);

	// Spinner frames and the idle glyph leave the session name unchanged.
	EXPECT_FALSE(tracker.ApplyTitle(1, L"\u25D3 Fix login bug"));
	EXPECT_FALSE(tracker.ApplyTitle(1, L"\u2733 Fix login bug"));

	EXPECT_TRUE(tracker.ApplyTitle(1, L"\u2733 Add tests"));
	EXPECT_EQ(tracker.SessionTitle(1), L"Add tests");
	EXPECT_TRUE(tracker.ApplyTitle(1, L"C:\\work"));
	EXPECT_TRUE(tracker.SessionTitle(1).empty());
	EXPECT_EQ(tracker.Presence(1), TerminalAgentKind::ClaudeCode);

	EXPECT_TRUE(tracker.SessionTitle(99).empty());
}

TEST(TerminalAgentPresence, DisplayNamesAreProductNames)
{
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::ClaudeCode), L"Claude Code");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::CommandCode), L"Command Code");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::Codex), L"Codex");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::Copilot), L"Copilot");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::Devin), L"Devin");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::Gemini), L"Gemini");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::OpenCode), L"OpenCode");
	EXPECT_EQ(TerminalAgentDisplayName(TerminalAgentKind::Pi), L"Pi");
}
