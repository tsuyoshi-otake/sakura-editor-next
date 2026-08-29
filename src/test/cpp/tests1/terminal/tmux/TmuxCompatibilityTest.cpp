/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/tmux/TmuxArgumentParser.h"
#include "terminal/tmux/TmuxCli.h"
#include "terminal/tmux/TmuxCommandDispatcher.h"
#include "terminal/tmux/TmuxFormatEvaluator.h"
#include "terminal/tmux/TmuxTargetResolver.h"

#include <string>

namespace {

using namespace terminal::tmux;

template<typename Id>
Id MakeId(std::uint64_t value)
{
	Id id;
	id.value = value;
	return id;
}

class FakeRuntimePort final : public ITmuxRuntimePort {
public:
	FakeRuntimePort()
	{
		m_snapshot.revision.value = 1;
		m_snapshot.activeSession = MakeId<terminal::TerminalSessionId>(1);
		TmuxSessionView session;
		session.id = MakeId<terminal::TerminalSessionId>(1);
		session.name = "dev";
		session.index = 0;
		session.active = true;
		session.attached = true;
		TmuxWindowView window;
		window.id = MakeId<terminal::TerminalWindowId>(2);
		window.name = "main";
		window.index = 0;
		window.active = true;
		TmuxPaneView activePane;
		activePane.id = MakeId<terminal::TerminalPaneId>(3);
		activePane.instanceId = MakeId<terminal::TerminalInstanceId>(10);
		activePane.index = 0;
		activePane.active = true;
		activePane.currentCommand = "pwsh";
		activePane.title = "PowerShell";
		activePane.coordinate.sessionId = session.id;
		activePane.coordinate.windowId = window.id;
		activePane.coordinate.paneId = activePane.id;
		activePane.coordinate.instanceId = activePane.instanceId;
		TmuxPaneView secondPane = activePane;
		secondPane.id = MakeId<terminal::TerminalPaneId>(4);
		secondPane.instanceId = MakeId<terminal::TerminalInstanceId>(11);
		secondPane.index = 1;
		secondPane.active = false;
		secondPane.currentCommand = "cmd";
		secondPane.coordinate.paneId = secondPane.id;
		secondPane.coordinate.instanceId = secondPane.instanceId;
		window.panes = { activePane, secondPane };
		session.windows.push_back(std::move(window));
		m_snapshot.sessions.push_back(std::move(session));
	}

	[[nodiscard]] TmuxRuntimeSnapshot Snapshot() const override { return m_snapshot; }

	[[nodiscard]] TmuxRuntimeResult CreateSession(const TmuxCreateSessionRequest&) override
	{
		return Success();
	}
	[[nodiscard]] TmuxRuntimeResult CreateTerminalWindow(const TmuxCreateWindowRequest&) override
	{
		++createWindowCalls;
		return WithWindow(20);
	}
	[[nodiscard]] TmuxRuntimeResult SplitWindow(const TmuxSplitWindowRequest&) override
	{
		++splitCalls;
		return WithPane(5);
	}
	[[nodiscard]] TmuxRuntimeResult SelectWindow(const TmuxSelectRequest&) override
	{
		++selectWindowCalls;
		return Success();
	}
	[[nodiscard]] TmuxRuntimeResult SelectPane(const TmuxSelectRequest&) override
	{
		++selectPaneCalls;
		return Success();
	}
	[[nodiscard]] TmuxRuntimeResult ClosePane(const TmuxCloseRequest&) override
	{
		++closePaneCalls;
		return Success();
	}
	[[nodiscard]] TmuxRuntimeResult CloseWindow(const TmuxCloseRequest&) override
	{
		++closeWindowCalls;
		return Success();
	}
	[[nodiscard]] TmuxRuntimeResult CloseSession(const TmuxCloseRequest&) override
	{
		++closeSessionCalls;
		return Success();
	}
	[[nodiscard]] TmuxRuntimeResult SendKeys(const TmuxInputBatch& batch) override
	{
		lastInput = batch;
		return Success();
	}
	[[nodiscard]] TmuxCaptureResult CapturePane(const TmuxCaptureRequest&) override
	{
		if (captureIncomplete) return TmuxCaptureResult{ TmuxRuntimeCode::Succeeded, {}, false, false, true };
		return TmuxCaptureResult{ TmuxRuntimeCode::Succeeded, { { "alpha" }, { "beta" } }, true, false, false };
	}
	[[nodiscard]] TmuxRuntimeResult WaitFor(const TmuxWaitRequest& request) override
	{
		lastWait = request;
		return Success();
	}

	static TmuxRuntimeResult Success()
	{
		TmuxRuntimeResult result;
		result.code = TmuxRuntimeCode::Succeeded;
		return result;
	}
	static TmuxRuntimeResult WithWindow(std::uint64_t id)
	{
		auto result = Success();
		result.windowId = MakeId<terminal::TerminalWindowId>(id);
		return result;
	}
	static TmuxRuntimeResult WithPane(std::uint64_t id)
	{
		auto result = Success();
		result.paneId = MakeId<terminal::TerminalPaneId>(id);
		return result;
	}

	TmuxRuntimeSnapshot m_snapshot;
	TmuxInputBatch lastInput;
	TmuxWaitRequest lastWait;
	bool captureIncomplete{};
	int createWindowCalls{};
	int splitCalls{};
	int selectWindowCalls{};
	int selectPaneCalls{};
	int closePaneCalls{};
	int closeWindowCalls{};
	int closeSessionCalls{};
};

TEST(TmuxCompatibility, ParserAcceptsCanonicalAliasesAndRejectsUnsupportedSurface)
{
	TmuxCommand command;
	EXPECT_EQ(TmuxParseCode::Succeeded, TmuxArgumentParser::Parse({ "ls" }, command).code);
	EXPECT_EQ(TmuxCommandKind::ListSessions, command.kind);
	EXPECT_EQ("list-sessions", command.canonicalName);

	EXPECT_EQ(TmuxParseCode::Succeeded,
		TmuxArgumentParser::Parse({ "send", "-l", "-t%3", "hello world" }, command).code);
	EXPECT_EQ(TmuxCommandKind::SendKeys, command.kind);
	ASSERT_TRUE(command.literal);
	ASSERT_TRUE(command.target);
	EXPECT_EQ("%3", *command.target);
	ASSERT_EQ(1u, command.operands.size());

	EXPECT_EQ(TmuxParseCode::UnsupportedSurface,
		TmuxArgumentParser::Parse({ "capture-pane", "-t", "%3" }, command).code);
	EXPECT_EQ(TmuxParseCode::Succeeded, TmuxArgumentParser::Parse({ "-V" }, command).code);
}

TEST(TmuxCompatibility, ResolverUsesExactStableIdsAndScopedIndexes)
{
	FakeRuntimePort fake;
	const auto& snapshot = fake.m_snapshot;

	const auto pane = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, "%4");
	ASSERT_TRUE(pane.Succeeded());
	EXPECT_TRUE(pane.target.usedStableIdentity);
	EXPECT_EQ(4u, pane.target.paneId.value);

	const auto scoped = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, "dev:main.1");
	ASSERT_TRUE(scoped.Succeeded());
	EXPECT_FALSE(scoped.target.usedStableIdentity);
	EXPECT_EQ(4u, scoped.target.paneId.value);

	const auto stale = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, "%99");
	EXPECT_EQ(TmuxTargetCode::TargetMissing, stale.code);
}

TEST(TmuxCompatibility, FormatSubsetExpandsEscapesAndFiltersWithoutExecution)
{
	FakeRuntimePort fake;
	const auto& session = fake.m_snapshot.sessions.front();
	const auto& window = session.windows.front();
	const auto& pane = window.panes.front();
	const TmuxFormatContext context{ &session, &window, &pane };

	const auto rendered = TmuxFormatEvaluator::Evaluate(
		"#{session_name}:#{window_id}:#{pane_id} ##", context);
	ASSERT_TRUE(rendered.Succeeded());
	EXPECT_EQ("dev:@2:%3 #", rendered.value);

	const auto filter = TmuxFormatEvaluator::EvaluateFilter(
		"#{==:#{pane_current_command},pwsh}", context);
	ASSERT_TRUE(filter.Succeeded());
	EXPECT_TRUE(filter.value);

	EXPECT_EQ(TmuxFormatCode::UnknownVariable,
		TmuxFormatEvaluator::Evaluate("#{not_a_variable}", context).code);
	EXPECT_EQ(TmuxFormatCode::InvalidFormat,
		TmuxFormatEvaluator::Evaluate("#(echo unsafe)", context).code);
}

TEST(TmuxCompatibility, DispatcherTargetsNonActivePaneAndMapsCaptureBoundedFailure)
{
	FakeRuntimePort fake;
	TmuxCommand command;
	ASSERT_EQ(TmuxParseCode::Succeeded,
		TmuxArgumentParser::Parse({ "send-keys", "-t", "%4", "C-c", "Enter" }, command).code);
	TmuxCommandDispatcher dispatcher(fake);
	const auto sent = dispatcher.Dispatch(command);
	ASSERT_TRUE(sent.Succeeded());
	ASSERT_EQ(2u, fake.lastInput.tokens.size());
	EXPECT_EQ(TmuxInputTokenKind::NamedKey, fake.lastInput.tokens.front().kind);
	EXPECT_EQ("C-c", fake.lastInput.tokens.front().text);
	EXPECT_EQ(4u, fake.lastInput.target.paneId.value);

	ASSERT_EQ(TmuxParseCode::Succeeded,
		TmuxArgumentParser::Parse({ "capturep", "-p", "-t", "%3" }, command).code);
	const auto captured = dispatcher.Dispatch(command);
	ASSERT_TRUE(captured.Succeeded());
	EXPECT_EQ("alpha\nbeta\n", captured.stdoutText);

	fake.captureIncomplete = true;
	const auto bounded = dispatcher.Dispatch(command);
	EXPECT_EQ(TmuxCommandResultCode::ResourceExhausted, bounded.code);
	EXPECT_TRUE(bounded.stdoutText.empty());
}

TEST(TmuxCompatibility, CliVersionAndFailureMappingAreHonestAndContentFree)
{
	FakeRuntimePort fake;
	const auto version = TmuxCli::Run({ "-V" }, fake);
	EXPECT_EQ(0, version.exitCode);
	EXPECT_EQ("sakura-tmux 0.1 (tmux 3.7c command subset; not upstream tmux)\n", version.stdoutText);
	EXPECT_TRUE(version.stderrText.empty());

	const auto missing = TmuxCli::Run({ "has", "-t", "$99" }, fake);
	EXPECT_EQ(1, missing.exitCode);
	EXPECT_TRUE(missing.stdoutText.empty());
	EXPECT_EQ("sakura-tmux: target-missing\n", missing.stderrText);

	const auto attach = TmuxCli::Run({ "new" }, fake);
	EXPECT_EQ(1, attach.exitCode);
	EXPECT_EQ("sakura-tmux: attach-unsupported\n", attach.stderrText);
}

TEST(TmuxCompatibility, WaitAliasesPreserveOperationAndChannel)
{
	FakeRuntimePort fake;
	const auto response = TmuxCli::Run({ "wait", "-S", "ready" }, fake);
	EXPECT_EQ(0, response.exitCode);
	EXPECT_EQ(TmuxWaitOperation::Signal, fake.lastWait.operation);
	EXPECT_EQ("ready", fake.lastWait.channel);
}

} // namespace
