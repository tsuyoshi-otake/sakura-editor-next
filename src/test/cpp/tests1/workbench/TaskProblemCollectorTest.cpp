/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"
#include "workbench/tasks/TaskExecutionService.h"
#include "workbench/tasks/TaskProblemCollector.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace workbench::tasks {
namespace {

using workbench::output::EOutputChannelKind;
using workbench::output::OutputService;
using workbench::problems::MarkerService;
using workbench::problems::ProblemsSnapshotQuery;

platform::uri::Uri WindowsPath(const wchar_t* value)
{
	auto parsed = platform::uri::Uri::FromWindowsPath(value);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

platform::uri::Uri TasksArtifact(const wchar_t* windowsFolder)
{
	std::wstring text = WindowsPath(windowsFolder).ToString() + L"/.vscode/tasks.json";
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

struct FakeSessionState final {
	explicit FakeSessionState(TaskExecutionSessionCallbacks value) : callbacks(std::move(value)) {}

	void Emit(std::string_view text)
	{
		callbacks.output(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
	}
	void Exit(ETaskSessionExitKind kind, std::uint32_t exitCode = 0, std::wstring diagnostic = {})
	{
		callbacks.exited({ kind, exitCode, std::move(diagnostic) });
	}

	TaskExecutionSessionCallbacks callbacks;
	unsigned int cancelRequests{};
	unsigned int closeRequests{};
	unsigned int closeWaits{};
};

class FakeSession final : public ITaskExecutionSession {
public:
	explicit FakeSession(std::shared_ptr<FakeSessionState> state) : m_state(std::move(state)) {}

	TaskExecutionSessionStartResult Start(const TaskTerminalLaunchRequest&) override { return TaskExecutionSessionStartResult::Success(); }
	void RequestCancel() noexcept override { ++m_state->cancelRequests; }
	void BeginClose() noexcept override { ++m_state->closeRequests; }
	TaskExecutionSessionCloseResult WaitForClose(std::chrono::steady_clock::time_point) noexcept override
	{
		++m_state->closeWaits;
		return TaskExecutionSessionCloseResult::Closed();
	}

	std::shared_ptr<FakeSessionState> m_state;
};

class FakeFactory final : public ITaskExecutionSessionFactory {
public:
	std::unique_ptr<ITaskExecutionSession> Create(const TaskExecutionSessionCallbacks& callbacks) override
	{
		lastState = std::make_shared<FakeSessionState>(callbacks);
		states.push_back(lastState);
		return std::make_unique<FakeSession>(lastState);
	}

	std::shared_ptr<FakeSessionState> lastState;
	std::vector<std::shared_ptr<FakeSessionState>> states;
};

TaskConfigurationDefinition MatcherTask(std::wstring label, std::vector<std::wstring> matchers)
{
	TaskConfigurationDefinition definition;
	definition.label = std::move(label);
	definition.executionKind = ETaskExecutionKind::Process;
	definition.command = L"C:\\Tools\\cl.exe";
	definition.problemMatchers = std::move(matchers);
	definition.sourceUri = TasksArtifact(L"C:\\ws");
	return definition;
}

struct Harness final {
	Harness() : execution(factory, {}, MakeTaskRunObserverFactory(markers, output, book)) {}

	std::shared_ptr<FakeFactory> factory = std::make_shared<FakeFactory>();
	MarkerService markers;
	OutputService output;
	std::shared_ptr<CTaskMarkerGenerationBook> book = std::make_shared<CTaskMarkerGenerationBook>();
	TaskExecutionService execution;
};

std::string TasksChannelText(const OutputService& output)
{
	const auto snapshot = output.Snapshot();
	for (const auto& channel : snapshot.channels) {
		if (channel.channelId == std::string(kTasksOutputChannelId)) return channel.text;
	}
	return {};
}

TEST(TaskProblemCollector, TasksOutputChannelIsCreatedOnceAndAppendsLines)
{
	OutputService output;
	ASSERT_TRUE(EnsureTasksOutputChannel(output).Succeeded());
	ASSERT_TRUE(EnsureTasksOutputChannel(output).Succeeded());
	const auto snapshot = output.Snapshot();
	ASSERT_EQ(1U, snapshot.channels.size());
	const auto& channel = snapshot.channels.front();
	EXPECT_EQ(std::string(kTasksOutputChannelId), channel.channelId);
	EXPECT_EQ(std::string(kTasksOutputChannelLabel), channel.label);
	EXPECT_EQ(EOutputChannelKind::Output, channel.kind);

	ASSERT_TRUE(AppendTasksOutputLine(output, "first").Succeeded());
	ASSERT_TRUE(AppendTasksOutputLine(output, "second\n").Succeeded());
	EXPECT_EQ("first\nsecond\n", TasksChannelText(output));
}

TEST(TaskProblemCollector, DerivesWorkspaceRootFromTasksArtifactUriOnly)
{
	auto definition = MatcherTask(L"build", {});
	definition.sourceUri = TasksArtifact(L"C:\\ws\\proj");
	const auto root = DeriveTaskWorkspaceRootUri(definition);
	ASSERT_TRUE(root.has_value());
	EXPECT_EQ(WindowsPath(L"C:\\ws\\proj").ToString(), root->ToString());

	definition.sourceUri = std::nullopt;
	EXPECT_FALSE(DeriveTaskWorkspaceRootUri(definition).has_value());

	auto other = platform::uri::Uri::Parse(WindowsPath(L"C:\\ws").ToString() + L"/settings.json");
	definition.sourceUri = std::move(*other.value);
	EXPECT_FALSE(DeriveTaskWorkspaceRootUri(definition).has_value());
}

TEST(TaskProblemCollector, ObserverFactoryDeclinesTasksWithoutSupportedMatchers)
{
	OutputService output;
	MarkerService markers;
	const auto book = std::make_shared<CTaskMarkerGenerationBook>();
	const auto factory = MakeTaskRunObserverFactory(markers, output, book);

	EXPECT_EQ(nullptr, factory(1, MatcherTask(L"plain", {})));
	EXPECT_EQ(nullptr, factory(2, MatcherTask(L"unknown", { L"$doesNotExist" })));
	EXPECT_NE(std::string::npos, TasksChannelText(output).find("not supported"));
	EXPECT_NE(nullptr, factory(3, MatcherTask(L"cl", { L"$msCompile" })));
}

TEST(TaskProblemCollector, RunOutputPopulatesMarkersForMsCompile)
{
	Harness harness;
	const auto started = harness.execution.Start({ { "run.mscompile", std::nullopt },
		MatcherTask(L"compile", { L"$msCompile" }), {} });
	ASSERT_TRUE(started.Succeeded());
	auto* const session = harness.factory->lastState.get();
	ASSERT_NE(nullptr, session);
	session->Emit("C:\\ws\\main.cpp(12,5): error C2065: 'identifier': undeclared identifier\r\n");
	session->Exit(ETaskSessionExitKind::Exited, 2);

	const auto snapshot = harness.markers.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	const auto& resource = snapshot.resources.front();
	EXPECT_EQ(WindowsPath(L"C:\\ws\\main.cpp").ToString(), resource.resource.ToString());
	EXPECT_EQ("msCompile", resource.collection.owner.id);
	EXPECT_EQ(1U, resource.collection.owner.generation);
	EXPECT_EQ("task.compile", resource.collection.id);
	ASSERT_EQ(1U, resource.markers.size());
	const auto& marker = resource.markers.front();
	EXPECT_EQ(problems::EMarkerSeverity::Error, marker.severity);
	EXPECT_EQ(11U, marker.range.startLine);
	EXPECT_EQ(4U, marker.range.startColumn);
	EXPECT_EQ("'identifier': undeclared identifier", marker.message);
	ASSERT_TRUE(marker.code.has_value());
	EXPECT_EQ("C2065", *marker.code);
}

TEST(TaskProblemCollector, OutputAcrossChunkBoundariesSplitsAtNewline)
{
	Harness harness;
	const auto started = harness.execution.Start({ { "run.chunks", std::nullopt },
		MatcherTask(L"compile", { L"$msCompile" }), {} });
	ASSERT_TRUE(started.Succeeded());
	auto* const session = harness.factory->lastState.get();
	session->Emit("C:\\ws\\main.cpp(12,");
	session->Emit("5): error C2065: 'id': undeclared identifier\r\nrest without newline");
	session->Exit(ETaskSessionExitKind::Exited, 0);

	const auto snapshot = harness.markers.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	ASSERT_EQ(1U, snapshot.resources.front().markers.size());
	EXPECT_EQ(11U, snapshot.resources.front().markers.front().range.startLine);
}

TEST(TaskProblemCollector, RerunWithoutMatchesClearsPriorRunMarkers)
{
	Harness harness;
	const auto definition = MatcherTask(L"compile", { L"$msCompile" });

	const auto first = harness.execution.Start({ { "run.one", std::nullopt }, definition, {} });
	ASSERT_TRUE(first.Succeeded());
	harness.factory->lastState->Emit("C:\\ws\\main.cpp(9,1): error C1001: broken\r\n");
	harness.factory->lastState->Exit(ETaskSessionExitKind::Exited, 1);
	ASSERT_EQ(1U, harness.markers.Snapshot().resources.size());

	const auto rerun = harness.execution.Start({ { "run.two", std::nullopt }, definition, {} });
	ASSERT_TRUE(rerun.Succeeded());
	harness.factory->lastState->Emit("build succeeded\r\n");
	harness.factory->lastState->Exit(ETaskSessionExitKind::Exited, 0);
	EXPECT_TRUE(harness.markers.Snapshot().resources.empty())
		<< "a clean rerun must remove every marker the previous run delivered";
}

TEST(TaskProblemCollector, RerunDropsResourcesTheNewRunNoLongerReports)
{
	Harness harness;
	const auto definition = MatcherTask(L"compile", { L"$msCompile" });

	const auto first = harness.execution.Start({ { "run.alpha", std::nullopt }, definition, {} });
	ASSERT_TRUE(first.Succeeded());
	harness.factory->lastState->Emit(
		"C:\\ws\\alpha.cpp(3,1): error C1: a\r\nC:\\ws\\beta.cpp(4,2): error C2: b\r\n");
	harness.factory->lastState->Exit(ETaskSessionExitKind::Exited, 1);
	ASSERT_EQ(2U, harness.markers.Snapshot().resources.size());

	const auto rerun = harness.execution.Start({ { "run.beta", std::nullopt }, definition, {} });
	ASSERT_TRUE(rerun.Succeeded());
	harness.factory->lastState->Emit("C:\\ws\\beta.cpp(4,2): error C2: b\r\n");
	harness.factory->lastState->Exit(ETaskSessionExitKind::Exited, 1);

	const auto snapshot = harness.markers.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ(WindowsPath(L"C:\\ws\\beta.cpp").ToString(), snapshot.resources.front().resource.ToString());
	EXPECT_EQ(2U, snapshot.resources.front().collection.owner.generation);
}

TEST(TaskProblemCollector, DelayedOlderRunTerminalCannotOverwriteNewerRunMarkers)
{
	Harness harness;
	const auto definition = MatcherTask(L"compile", { L"$msCompile" });

	// Two runs race: the older run's terminal callback lands after the newer
	// run already adopted its generation. Its replaces must be fenced stale.
	const auto older = harness.execution.Start({ { "run.older", std::nullopt }, definition, {} });
	ASSERT_TRUE(older.Succeeded());
	const auto olderSession = harness.factory->lastState;
	const auto newer = harness.execution.Start({ { "run.newer", std::nullopt }, definition, {} });
	ASSERT_TRUE(newer.Succeeded());
	const auto newerSession = harness.factory->lastState;

	newerSession->Emit("C:\\ws\\newer.cpp(5,1): error C9: current\r\n");
	newerSession->Exit(ETaskSessionExitKind::Exited, 1);
	olderSession->Emit("C:\\ws\\older.cpp(6,1): error C8: stale\r\n");
	olderSession->Exit(ETaskSessionExitKind::Exited, 1);

	const auto snapshot = harness.markers.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ(WindowsPath(L"C:\\ws\\newer.cpp").ToString(), snapshot.resources.front().resource.ToString());
	EXPECT_EQ(2U, snapshot.resources.front().collection.owner.generation);
}

TEST(TaskProblemCollector, TerminalizedRunObserverIgnoresLateDirectDeliveries)
{
	MarkerService markers;
	OutputService output;
	const auto book = std::make_shared<CTaskMarkerGenerationBook>();
	const auto observerFactory = MakeTaskRunObserverFactory(markers, output, book);
	const auto definition = MatcherTask(L"compile", { L"$msCompile" });

	const auto older = observerFactory(1, definition);
	const auto newer = observerFactory(2, definition);
	ASSERT_NE(nullptr, older);
	ASSERT_NE(nullptr, newer);
	newer->OnTerminal(ETaskExecutionRunState::Exited, 0, {});
	older->OnTerminal(ETaskExecutionRunState::Exited, 0, {});
	EXPECT_TRUE(markers.Snapshot().resources.empty());

	// A retained older observer that receives late bytes and a repeated
	// terminal call must not write under its already-stale generation.
	const std::string lateBytes = "C:\\ws\\stale.cpp(1,1): error C1: stale\r\n";
	older->OnOutput(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(lateBytes.data()), lateBytes.size()));
	older->OnTerminal(ETaskExecutionRunState::Exited, 0, {});
	EXPECT_TRUE(markers.Snapshot().resources.empty());
}

TEST(TaskProblemCollector, RelativeMatcherWithoutWorkspaceRootFailsTypedAndPreservesMarkers)
{
	Harness harness;
	auto definition = MatcherTask(L"gcc-task", { L"$gcc" });
	definition.sourceUri = std::nullopt; // workspace-artifact task: no folder to resolve against

	const auto started = harness.execution.Start({ { "run.gcc", std::nullopt }, definition, {} });
	ASSERT_TRUE(started.Succeeded());
	harness.factory->lastState->Emit("src/main.cpp:10:5: error: missing semicolon\r\n");
	harness.factory->lastState->Exit(ETaskSessionExitKind::Exited, 1);

	EXPECT_TRUE(harness.markers.Snapshot().resources.empty());
	EXPECT_NE(std::string::npos, TasksChannelText(harness.output).find("MissingWorkspaceRoot"));
}

TEST(TaskProblemCollector, GccOutputResolvesRelativeToTasksFolder)
{
	Harness harness;
	const auto definition = MatcherTask(L"gcc-task", { L"$gcc" });

	const auto started = harness.execution.Start({ { "run.gcc.relative", std::nullopt }, definition, {} });
	ASSERT_TRUE(started.Succeeded());
	harness.factory->lastState->Emit("src/main.cpp:10:5: error: missing semicolon\r\n");
	harness.factory->lastState->Exit(ETaskSessionExitKind::Exited, 1);

	const auto snapshot = harness.markers.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ(WindowsPath(L"C:\\ws\\src\\main.cpp").ToString(), snapshot.resources.front().resource.ToString());
	EXPECT_EQ("gcc", snapshot.resources.front().collection.owner.id);
}

TEST(TaskProblemCollector, CancelledAndClosedRunsStillApplyCollectedOutput)
{
	Harness harness;
	const auto definition = MatcherTask(L"compile", { L"$msCompile" });

	const auto cancelled = harness.execution.Start({ { "run.cancelled", std::nullopt }, definition, {} });
	ASSERT_TRUE(cancelled.Succeeded());
	const auto cancelledSession = harness.factory->lastState;
	cancelledSession->Emit("C:\\ws\\cancelled.cpp(7,1): error C7: seen\r\n");
	ASSERT_TRUE(harness.execution.Cancel({ { "run.cancel", std::nullopt }, *cancelled.runId }).Succeeded());
	cancelledSession->Exit(ETaskSessionExitKind::Cancelled, 130);
	const auto afterCancel = harness.markers.Snapshot();
	ASSERT_EQ(1U, afterCancel.resources.size());
	EXPECT_EQ(WindowsPath(L"C:\\ws\\cancelled.cpp").ToString(), afterCancel.resources.front().resource.ToString());

	const auto closed = harness.execution.Start({ { "run.closed", std::nullopt }, definition, {} });
	ASSERT_TRUE(closed.Succeeded());
	harness.factory->lastState->Emit("C:\\ws\\closed.cpp(8,2): error C8: seen\r\n");
	ASSERT_TRUE(harness.execution.Close({ { "run.close", std::nullopt }, *closed.runId }).Succeeded());
	const auto snapshot = harness.markers.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ(WindowsPath(L"C:\\ws\\closed.cpp").ToString(), snapshot.resources.front().resource.ToString());
	EXPECT_EQ(2U, snapshot.resources.front().collection.owner.generation);
}

} // namespace
} // namespace workbench::tasks
