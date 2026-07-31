/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/tasks/TaskTerminalSessionFactory.h"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace {

using workbench::tasks::CTaskTerminalSessionFactory;
using workbench::tasks::ETaskExecutionKind;
using workbench::tasks::ETaskSessionExitKind;
using workbench::tasks::ITaskExecutionSession;
using workbench::tasks::ITaskShellLaunchPolicy;
using workbench::tasks::PowerShellTaskShellLaunchPolicy;
using workbench::tasks::TaskExecutionSessionCallbacks;
using workbench::tasks::TaskShellLaunchResult;
using workbench::tasks::TaskTerminalLaunchRequest;
using workbench::tasks::TerminalSessionCreator;

struct RecordingBackendState final {
	std::mutex mutex;
	terminal::TerminalLaunchOptions launch;
	terminal::TerminalStartResult startResult = terminal::TerminalStartResult::Success();
	terminal::TerminalBackendReadStatus readStatus = terminal::TerminalBackendReadStatus::EndOfFile;
	std::uint32_t readError{};
	std::vector<std::uint8_t> output;
	terminal::TerminalBackendExitResult exit{ terminal::TerminalBackendExitStatus::Exited, 0, 0 };
	unsigned int gracefulCloseRequests{};
	unsigned int forcedCloseRequests{};
	unsigned int closeRequests{};
};

class RecordingBackend final : public terminal::ITerminalBackend {
public:
	explicit RecordingBackend(std::shared_ptr<RecordingBackendState> state) : m_state(std::move(state)) {}

	terminal::TerminalStartResult Start(const terminal::TerminalLaunchOptions& options) override
	{
		std::lock_guard lock(m_state->mutex);
		m_state->launch = options;
		return m_state->startResult;
	}
	terminal::TerminalBackendReadResult ReadOutput(std::span<std::uint8_t> destination, std::chrono::milliseconds) override
	{
		std::lock_guard lock(m_state->mutex);
		if (!m_state->output.empty()) {
			const auto count = std::min(destination.size(), m_state->output.size());
			std::copy_n(m_state->output.begin(), count, destination.begin());
			m_state->output.erase(m_state->output.begin(), m_state->output.begin() + static_cast<std::ptrdiff_t>(count));
			return { terminal::TerminalBackendReadStatus::Data, count, 0 };
		}
		return { m_state->readStatus, 0, m_state->readError };
	}
	terminal::TerminalBackendWriteResult WriteInput(std::span<const std::uint8_t> source) override
	{
		return { terminal::TerminalBackendWriteStatus::Completed, source.size(), 0 };
	}
	terminal::TerminalBackendOperationResult Resize(terminal::TerminalSize) override { return { true, 0 }; }
	void RequestGracefulClose() noexcept override
	{
		std::lock_guard lock(m_state->mutex);
		++m_state->gracefulCloseRequests;
		m_state->readStatus = terminal::TerminalBackendReadStatus::EndOfFile;
	}
	terminal::TerminalBackendExitResult WaitForExit(std::chrono::milliseconds) noexcept override
	{
		std::lock_guard lock(m_state->mutex);
		return m_state->exit;
	}
	void ForceTerminate() noexcept override
	{
		std::lock_guard lock(m_state->mutex);
		++m_state->forcedCloseRequests;
		m_state->readStatus = terminal::TerminalBackendReadStatus::EndOfFile;
	}
	void Close() noexcept override
	{
		std::lock_guard lock(m_state->mutex);
		++m_state->closeRequests;
	}

private:
	std::shared_ptr<RecordingBackendState> m_state;
};

class FakeProfiles final : public terminal::ITerminalProfileProvider {
public:
	terminal::PowerShellDiscoveryResult DiscoverProfiles() override { return discovery; }
	void InvalidateProfileCache() noexcept override {}
	terminal::PowerShellDiscoveryResult discovery;
};

class RejectingPolicy final : public ITaskShellLaunchPolicy {
public:
	TaskShellLaunchResult Resolve(std::wstring_view, std::span<const std::wstring>, std::wstring_view, terminal::TerminalSize) override
	{
		return TaskShellLaunchResult::Failure(912, L"policy rejected request");
	}
};

struct CompletionCapture final {
	void Receive(workbench::tasks::TaskSessionExit value)
	{
		std::lock_guard lock(mutex);
		exits.emplace_back(std::move(value));
		condition.notify_all();
	}
	bool WaitFor(std::size_t count)
	{
		std::unique_lock lock(mutex);
		return condition.wait_for(lock, std::chrono::seconds(2), [&] { return exits.size() >= count; });
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::vector<workbench::tasks::TaskSessionExit> exits;
};

TerminalSessionCreator MakeCreator(const std::shared_ptr<RecordingBackendState>& state)
{
	return [state](terminal::TerminalSessionCallbacks callbacks) {
		return std::make_unique<terminal::CTerminalSession>(std::make_unique<RecordingBackend>(state), std::move(callbacks));
	};
}

TaskTerminalLaunchRequest ProcessRequest()
{
	TaskTerminalLaunchRequest request;
	request.executionKind = ETaskExecutionKind::Process;
	request.terminalLaunchOptions.executablePath = L"C:\\tools\\runner.exe";
	request.terminalLaunchOptions.arguments = { L"", L"with space", L"a'b", L"C:\\tail\\" };
	request.terminalLaunchOptions.workingDirectory = L"C:\\work";
	request.terminalLaunchOptions.initialSize = { 101, 41 };
	return request;
}

TaskTerminalLaunchRequest ShellRequest()
{
	TaskTerminalLaunchRequest request;
	request.executionKind = ETaskExecutionKind::Shell;
	request.shellCommand = L"Write-Output";
	request.shellArguments = { L"", L"with space", L"a'b", L"C:\\tail\\" };
	request.terminalLaunchOptions.workingDirectory = L"C:\\work";
	request.terminalLaunchOptions.initialSize = { 101, 41 };
	return request;
}

TEST(PowerShellTaskShellLaunchPolicy, PreservesCommandFragmentAndQuotesEveryArgumentToken)
{
	FakeProfiles profiles;
	profiles.discovery.defaultCandidate = terminal::TerminalProfile{ L"C:\\Program Files\\PowerShell\\pwsh.exe" };
	PowerShellTaskShellLaunchPolicy policy(profiles);
	const std::vector<std::wstring> arguments{ L"", L"with space", L"a'b", L"C:\\tail\\" };
	const auto result = policy.Resolve(L"Write-Output", arguments, L"C:\\work", { 101, 41 });

	ASSERT_TRUE(result.succeeded);
	EXPECT_EQ(L"C:\\Program Files\\PowerShell\\pwsh.exe", result.options.executablePath);
	EXPECT_EQ((std::vector<std::wstring>{ L"-NoLogo", L"-Command", L"Write-Output '' 'with space' 'a''b' 'C:\\tail\\'" }), result.options.arguments);
	EXPECT_EQ(L"C:\\work", result.options.workingDirectory);
	EXPECT_EQ(101, result.options.initialSize.columns);
	EXPECT_EQ(41, result.options.initialSize.rows);
}

TEST(PowerShellTaskShellLaunchPolicy, RejectsNulAndMissingProfile)
{
	FakeProfiles profiles;
	PowerShellTaskShellLaunchPolicy policy(profiles);
	EXPECT_FALSE(policy.Resolve(L"echo", {}, L"C:\\work", { 80, 24 }).succeeded);
	profiles.discovery.defaultCandidate = terminal::TerminalProfile{ L"pwsh.exe" };
	EXPECT_FALSE(policy.Resolve(std::wstring(L"echo\0bad", 8), {}, L"C:\\work", { 80, 24 }).succeeded);
}

TEST(CTaskTerminalSessionFactory, ProcessLaunchPreservesExecutableAndArgumentTokens)
{
	auto state = std::make_shared<RecordingBackendState>();
	state->exit.exitCode = 23;
	CompletionCapture capture;
	CTaskTerminalSessionFactory factory({}, MakeCreator(state));
	auto session = factory.Create(TaskExecutionSessionCallbacks{ [&](auto value) { capture.Receive(std::move(value)); } });

	ASSERT_TRUE(session->Start(ProcessRequest()).succeeded);
	ASSERT_TRUE(capture.WaitFor(1));
	{
		std::lock_guard lock(state->mutex);
		EXPECT_EQ(L"C:\\tools\\runner.exe", state->launch.executablePath);
		EXPECT_EQ((std::vector<std::wstring>{ L"", L"with space", L"a'b", L"C:\\tail\\" }), state->launch.arguments);
		EXPECT_EQ(L"C:\\work", state->launch.workingDirectory);
	}
	EXPECT_EQ(ETaskSessionExitKind::Exited, capture.exits.front().kind);
	EXPECT_EQ(23, capture.exits.front().exitCode);
}

TEST(CTaskTerminalSessionFactory, ShellUsesPolicyAndDoesNotAllowMixedProcessFields)
{
	FakeProfiles profiles;
	profiles.discovery.defaultCandidate = terminal::TerminalProfile{ L"pwsh.exe" };
	auto state = std::make_shared<RecordingBackendState>();
	auto policy = std::make_shared<PowerShellTaskShellLaunchPolicy>(profiles);
	CTaskTerminalSessionFactory factory(policy, MakeCreator(state));
	CompletionCapture capture;
	auto session = factory.Create(TaskExecutionSessionCallbacks{ [&](auto value) { capture.Receive(std::move(value)); } });
	ASSERT_TRUE(session->Start(ShellRequest()).succeeded);
	ASSERT_TRUE(capture.WaitFor(1));
	{
		std::lock_guard lock(state->mutex);
		EXPECT_EQ(L"pwsh.exe", state->launch.executablePath);
		EXPECT_EQ((std::vector<std::wstring>{ L"-NoLogo", L"-Command", L"Write-Output '' 'with space' 'a''b' 'C:\\tail\\'" }), state->launch.arguments);
	}
	auto mixed = ShellRequest();
	mixed.terminalLaunchOptions.executablePath = L"must-not-pass.exe";
	auto second = factory.Create({});
	EXPECT_FALSE(second->Start(mixed).succeeded);
}

TEST(CTaskTerminalSessionFactory, StartFailureAndPolicyFailureDoNotPublishAnExit)
{
	auto state = std::make_shared<RecordingBackendState>();
	state->startResult = terminal::TerminalStartResult::Failure(700, L"start rejected");
	CompletionCapture capture;
	CTaskTerminalSessionFactory factory({}, MakeCreator(state));
	auto session = factory.Create(TaskExecutionSessionCallbacks{ [&](auto value) { capture.Receive(std::move(value)); } });
	EXPECT_FALSE(session->Start(ProcessRequest()).succeeded);
	EXPECT_FALSE(capture.WaitFor(1));

	CTaskTerminalSessionFactory rejected(std::make_shared<RejectingPolicy>(), MakeCreator(state));
	auto shellSession = rejected.Create(TaskExecutionSessionCallbacks{ [&](auto value) { capture.Receive(std::move(value)); } });
	EXPECT_FALSE(shellSession->Start(ShellRequest()).succeeded);
	EXPECT_FALSE(capture.WaitFor(1));
}

TEST(CTaskTerminalSessionFactory, MapsNaturalCancelFailureAndCloseExactlyOnceAfterQuiescence)
{
	// Natural process exit retains the actual process exit code.
	{
		auto state = std::make_shared<RecordingBackendState>();
		state->exit.exitCode = 42;
		CompletionCapture capture;
		CTaskTerminalSessionFactory factory({}, MakeCreator(state));
		auto session = factory.Create({ [&](auto value) { capture.Receive(std::move(value)); } });
		ASSERT_TRUE(session->Start(ProcessRequest()).succeeded);
		ASSERT_TRUE(capture.WaitFor(1));
		EXPECT_EQ(ETaskSessionExitKind::Exited, capture.exits.front().kind);
		EXPECT_EQ(42, capture.exits.front().exitCode);
		EXPECT_EQ(workbench::tasks::ETaskExecutionSessionCloseKind::Closed,
			session->WaitForClose(std::chrono::steady_clock::now() + std::chrono::seconds(1)).kind);
		EXPECT_EQ(1U, capture.exits.size());
	}
	// Semantic cancellation wins over the terminal's forced/closed completion kind.
	{
		auto state = std::make_shared<RecordingBackendState>();
		state->readStatus = terminal::TerminalBackendReadStatus::Timeout;
		state->exit.exitCode = 99;
		CompletionCapture capture;
		CTaskTerminalSessionFactory factory({}, MakeCreator(state));
		auto session = factory.Create({ [&](auto value) { capture.Receive(std::move(value)); } });
		ASSERT_TRUE(session->Start(ProcessRequest()).succeeded);
		session->RequestCancel();
		ASSERT_TRUE(capture.WaitFor(1));
		EXPECT_EQ(ETaskSessionExitKind::Cancelled, capture.exits.front().kind);
		EXPECT_EQ(99, capture.exits.front().exitCode);
		EXPECT_EQ(1U, capture.exits.size());
	}
	// An I/O failure maps to Failed once, and an ordinary close is not cancellation.
	{
		auto state = std::make_shared<RecordingBackendState>();
		state->readStatus = terminal::TerminalBackendReadStatus::Failed;
		state->readError = 901;
		CompletionCapture capture;
		CTaskTerminalSessionFactory factory({}, MakeCreator(state));
		auto session = factory.Create({ [&](auto value) { capture.Receive(std::move(value)); } });
		ASSERT_TRUE(session->Start(ProcessRequest()).succeeded);
		ASSERT_TRUE(capture.WaitFor(1));
		EXPECT_EQ(ETaskSessionExitKind::Failed, capture.exits.front().kind);
		EXPECT_EQ(1U, capture.exits.size());
	}
}

TEST(CTaskTerminalSessionFactory, ExplicitCloseLeavesTerminalStateFinalizationToTaskExecutionService)
{
	auto state = std::make_shared<RecordingBackendState>();
	state->readStatus = terminal::TerminalBackendReadStatus::Timeout;
	state->exit.exitCode = 31;
	CompletionCapture capture;
	CTaskTerminalSessionFactory factory({}, MakeCreator(state));
	auto session = factory.Create({ [&](auto value) { capture.Receive(std::move(value)); } });
	ASSERT_TRUE(session->Start(ProcessRequest()).succeeded);

	session->BeginClose();
	EXPECT_EQ(workbench::tasks::ETaskExecutionSessionCloseKind::Closed,
		session->WaitForClose(std::chrono::steady_clock::now() + std::chrono::seconds(1)).kind);
	std::lock_guard lock(capture.mutex);
	EXPECT_TRUE(capture.exits.empty());
}

TEST(CTaskTerminalSessionFactoryIntegration, ConPtyProcessPublishesTheRealExitCode)
{
	wchar_t systemDirectory[MAX_PATH]{};
	const UINT length = ::GetSystemDirectoryW(
		systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
	ASSERT_GT(length, 0U);
	ASSERT_LT(length, std::size(systemDirectory));

	auto request = ProcessRequest();
	request.terminalLaunchOptions.executablePath.assign(systemDirectory, length);
	request.terminalLaunchOptions.executablePath += L"\\cmd.exe";
	request.terminalLaunchOptions.arguments = { L"/d", L"/q", L"/c", L"exit /b 37" };
	request.terminalLaunchOptions.workingDirectory.clear();

	CompletionCapture capture;
	CTaskTerminalSessionFactory factory(nullptr);
	auto session = factory.Create({ [&](auto value) { capture.Receive(std::move(value)); } });
	const auto started = session->Start(request);
	ASSERT_TRUE(started.succeeded) << "Create ConPTY task process failed with " << started.errorCode;
	ASSERT_TRUE(capture.WaitFor(1));
	{
		std::lock_guard lock(capture.mutex);
		ASSERT_EQ(1U, capture.exits.size());
		EXPECT_EQ(ETaskSessionExitKind::Exited, capture.exits.front().kind);
		EXPECT_EQ(37U, capture.exits.front().exitCode);
	}
	EXPECT_EQ(workbench::tasks::ETaskExecutionSessionCloseKind::Closed,
		session->WaitForClose(std::chrono::steady_clock::now() + std::chrono::seconds(1)).kind);
}

TEST(CTaskTerminalSessionFactory, OutputAvailableDrainsBoundedTerminalQueueIntoSink)
{
	auto state = std::make_shared<RecordingBackendState>();
	state->output = { 'o', 'u', 't' };
	state->exit.exitCode = 0;
	std::mutex outputMutex;
	std::vector<std::uint8_t> output;
	CompletionCapture capture;
	CTaskTerminalSessionFactory factory({}, MakeCreator(state), [&](std::span<const std::uint8_t> bytes) {
		std::lock_guard lock(outputMutex);
		output.insert(output.end(), bytes.begin(), bytes.end());
	});
	auto session = factory.Create({ [&](auto value) { capture.Receive(std::move(value)); } });
	ASSERT_TRUE(session->Start(ProcessRequest()).succeeded);
	ASSERT_TRUE(capture.WaitFor(1));
	std::lock_guard lock(outputMutex);
	EXPECT_EQ((std::vector<std::uint8_t>{ 'o', 'u', 't' }), output);
}

} // namespace
