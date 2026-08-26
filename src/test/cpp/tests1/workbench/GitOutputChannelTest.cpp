/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workbench/output/IOutputService.h"
#include "workbench/output/OutputService.h"
#include "workbench/scm/GitOutputChannel.h"

using namespace workbench::scm;
using namespace workbench::output;

namespace {

[[nodiscard]] GitExecutionResult Ok(std::string_view standardError = {})
{
	GitExecutionResult result;
	result.status = EGitExecutionStatus::Succeeded;
	result.exitCode = 0;
	result.standardError.assign(standardError);
	return result;
}

const OutputChannelSnapshot* FindGitChannel(const OutputServiceSnapshot& snapshot)
{
	for (const auto& channel : snapshot.channels) {
		if (channel.channelId == std::string(kGitOutputChannelId)) {
			return &channel;
		}
	}
	return nullptr;
}

class FakeOutputProvider final : public IOutputService {
public:
	FakeOutputProvider()
	{
		snapshot.revision = 1;
	}

	OutputOperationResult CreateChannel(const OutputCreateChannelRequest& request) override
	{
		createRequests.push_back(request);
		if (!createResult.Succeeded()) return createResult;

		OutputChannelSnapshot channel;
		channel.channelId = request.channelId;
		channel.label = request.label;
		channel.owner = request.owner;
		channel.kind = request.kind;
		channel.metadata = request.metadata;
		snapshot.channels.push_back(std::move(channel));
		OutputOperationResult result = createResult;
		result.revision = ++snapshot.revision;
		return result;
	}

	OutputOperationResult AppendOutput(const OutputTextMutationRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult ReplaceOutput(const OutputTextMutationRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult AppendLog(const OutputLogMutationRequest& request) override
	{
		appendLogRequests.push_back(request);
		if (!appendLogResult.Succeeded()) return appendLogResult;

		for (auto& channel : snapshot.channels) {
			if (channel.channelId == request.channelId && channel.owner == request.owner
				&& channel.kind == EOutputChannelKind::Log) {
				channel.logEntries.insert(channel.logEntries.end(), request.entries.begin(), request.entries.end());
				break;
			}
		}
		OutputOperationResult result = appendLogResult;
		result.revision = ++snapshot.revision;
		return result;
	}

	OutputOperationResult Clear(const OutputChannelMutationRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult Show(const OutputShowChannelRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult Hide(const OutputChannelMutationRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult Dispose(const OutputChannelMutationRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult DisposeOwner(const OutputDisposeOwnerRequest&) override
	{
		return UnsupportedResult();
	}

	OutputOperationResult Stop() noexcept override
	{
		snapshot.stopped = true;
		OutputOperationResult result;
		result.status = EOutputOperationStatus::Succeeded;
		result.revision = ++snapshot.revision;
		return result;
	}

	OutputProviderHealthSnapshot Health() const noexcept override
	{
		OutputProviderHealthSnapshot health;
		health.kind = EOutputProviderKind::Cpp;
		health.factoryStatus = EOutputProviderFactoryStatus::Created;
		health.lifecycle = snapshot.stopped
			? EOutputProviderLifecycle::Stopped
			: EOutputProviderLifecycle::Ready;
		health.initializationStage = EOutputProviderInitializationStage::Ready;
		health.compiledIn = true;
		health.available = !snapshot.stopped;
		health.counters.initializationAttempts = 1;
		health.counters.snapshotCalls = snapshotCallCount;
		health.currentRevision = snapshot.revision;
		return health;
	}

	OutputServiceSnapshot Snapshot() const override
	{
		++snapshotCallCount;
		return snapshot;
	}

	std::optional<OutputServiceSubscriptionId> Subscribe(OutputServiceListener) override
	{
		return std::nullopt;
	}

	void Unsubscribe(OutputServiceSubscriptionId) noexcept override
	{
	}

	OutputServiceSnapshot snapshot;
	mutable std::size_t snapshotCallCount{};
	std::vector<OutputCreateChannelRequest> createRequests;
	std::vector<OutputLogMutationRequest> appendLogRequests;
	OutputOperationResult createResult{
		.status = EOutputOperationStatus::Succeeded,
		.reason = EOutputOperationReason::None,
	};
	OutputOperationResult appendLogResult{
		.status = EOutputOperationStatus::Succeeded,
		.reason = EOutputOperationReason::None,
	};

private:
	static OutputOperationResult UnsupportedResult() noexcept
	{
		return {
			.status = EOutputOperationStatus::Rejected,
			.reason = EOutputOperationReason::None,
		};
	}
};

} // namespace

TEST(GitOutputChannel, IdAndLabelMatchUpstreamsBuiltInGitChannel)
{
	// Verified against microsoft/vscode's extensions/git/src/main.ts, which
	// creates the channel with window.createOutputChannel('Git', { log: true }).
	EXPECT_EQ("git", std::string(kGitOutputChannelId));
	EXPECT_EQ("Git", std::string(kGitOutputChannelLabel));
}

TEST(GitOutputChannel, EnsureCreatesALogChannelOnceAndIsIdempotentAcrossDifferentOperationIds)
{
	OutputService service;
	const OutputOwner owner{ .ownerId = "workbench.scm.git", .generation = 1 };

	const auto first = EnsureGitOutputChannel(service, owner, "create-1");
	ASSERT_TRUE(first.Succeeded());

	const auto snapshotAfterFirst = service.Snapshot();
	const auto* channel = FindGitChannel(snapshotAfterFirst);
	ASSERT_NE(nullptr, channel);
	EXPECT_EQ(EOutputChannelKind::Log, channel->kind);
	EXPECT_EQ(std::string(kGitOutputChannelLabel), channel->label);

	// A second Ensure call with a *different* operationId must still report
	// success (the channel already exists) rather than surfacing the raw
	// OutputService::CreateChannel Conflict/InvalidChannelId a naive replay-only
	// implementation would produce.
	const auto second = EnsureGitOutputChannel(service, owner, "create-2");
	EXPECT_TRUE(second.Succeeded());
	EXPECT_EQ(snapshotAfterFirst.revision, service.Snapshot().revision);
}

TEST(GitOutputChannel, ConsumerUsesProviderSnapshotCreateAndAppendLogWithoutConcreteService)
{
	FakeOutputProvider service;
	const OutputOwner owner{ .ownerId = "workbench.scm.git", .generation = 1 };

	const auto created = EnsureGitOutputChannel(service, owner, "create");
	ASSERT_TRUE(created.Succeeded());
	ASSERT_EQ(1U, service.snapshotCallCount);
	ASSERT_EQ(1U, service.createRequests.size());
	EXPECT_EQ(std::string(kGitOutputChannelId), service.createRequests[0].channelId);
	EXPECT_EQ(EOutputChannelKind::Log, service.createRequests[0].kind);

	std::vector<OutputLogEntry> entries;
	entries.push_back({ .level = EOutputLogLevel::Info, .message = "> git status [1ms]" });
	const auto appended = AppendGitOutputLogEntries(service, owner, "append", entries);
	ASSERT_TRUE(appended.Succeeded());
	ASSERT_EQ(1U, service.appendLogRequests.size());
	EXPECT_EQ(std::string(kGitOutputChannelId), service.appendLogRequests[0].channelId);

	const auto snapshot = service.Snapshot();
	EXPECT_EQ(2U, service.snapshotCallCount);
	const auto* channel = FindGitChannel(snapshot);
	ASSERT_NE(nullptr, channel);
	ASSERT_EQ(1U, channel->logEntries.size());
	EXPECT_EQ(entries[0].message, channel->logEntries[0].message);
}

TEST(GitOutputChannel, AppendBeforeChannelCreationIsChannelNotFound)
{
	OutputService service;
	const OutputOwner owner{ .ownerId = "workbench.scm.git", .generation = 1 };

	std::vector<OutputLogEntry> entries;
	entries.push_back({ .level = EOutputLogLevel::Info, .message = "> git status" });

	const auto result = AppendGitOutputLogEntries(service, owner, "append-1", entries);
	EXPECT_EQ(EOutputOperationStatus::NotApplicable, result.status);
	EXPECT_EQ(EOutputOperationReason::ChannelNotFound, result.reason);
}

TEST(GitOutputChannel, AppendAfterEnsureUpdatesGenerationAndRespectsExpectedRevision)
{
	OutputService service;
	const OutputOwner owner{ .ownerId = "workbench.scm.git", .generation = 1 };
	ASSERT_TRUE(EnsureGitOutputChannel(service, owner, "create").Succeeded());
	const auto beforeAppend = service.Snapshot();

	std::vector<OutputLogEntry> entries;
	entries.push_back({ .level = EOutputLogLevel::Info, .message = "> git status [1ms]" });
	const auto appended = AppendGitOutputLogEntries(service, owner, "append", entries, beforeAppend.revision);
	ASSERT_TRUE(appended.Succeeded());

	// The snapshot is returned by value, so it must outlive the pointer
	// FindGitChannel hands back.
	const auto snapshot = service.Snapshot();
	const auto* channel = FindGitChannel(snapshot);
	ASSERT_NE(nullptr, channel);
	ASSERT_EQ(1U, channel->logEntries.size());
	EXPECT_EQ("> git status [1ms]", channel->logEntries[0].message);

	// A stale expectedRevision must be rejected, not silently reordered.
	std::vector<OutputLogEntry> more;
	more.push_back({ .level = EOutputLogLevel::Info, .message = "> git status [2ms]" });
	const auto stale = AppendGitOutputLogEntries(service, owner, "append-stale", more, beforeAppend.revision);
	EXPECT_EQ(EOutputOperationStatus::StaleRevision, stale.status);
}

TEST(GitOutputChannel, AppendRejectsOperationIdCollisionWithDifferentPayloadAndReplaysAnExactRepeat)
{
	OutputService service;
	const OutputOwner owner{ .ownerId = "workbench.scm.git", .generation = 1 };
	ASSERT_TRUE(EnsureGitOutputChannel(service, owner, "create").Succeeded());

	std::vector<OutputLogEntry> entries;
	entries.push_back({ .level = EOutputLogLevel::Info, .message = "> git fetch [3ms]" });
	ASSERT_EQ(EOutputOperationStatus::Succeeded, AppendGitOutputLogEntries(service, owner, "op-1", entries).status);

	// Exact replay of the same operationId with the same payload is a no-op Replayed.
	EXPECT_EQ(EOutputOperationStatus::Replayed, AppendGitOutputLogEntries(service, owner, "op-1", entries).status);

	// Same operationId, different payload: Conflict, not a silent second append.
	std::vector<OutputLogEntry> different;
	different.push_back({ .level = EOutputLogLevel::Info, .message = "> git fetch [4ms]" });
	EXPECT_EQ(EOutputOperationStatus::Conflict, AppendGitOutputLogEntries(service, owner, "op-1", different).status);

	// The snapshot is returned by value, so it must outlive the pointer
	// FindGitChannel hands back.
	const auto snapshot = service.Snapshot();
	const auto* channel = FindGitChannel(snapshot);
	ASSERT_NE(nullptr, channel);
	EXPECT_EQ(1U, channel->logEntries.size());
}

TEST(GitOutputChannel, BuildEntriesAlwaysIncludesTheCommandHeaderAndOmitsStdoutByDefault)
{
	const std::vector<std::wstring> arguments{ L"-C", L"C:\\repo", L"status" };
	const auto entries = BuildGitOutputLogEntries(arguments, std::chrono::milliseconds(42), Ok());

	ASSERT_EQ(1U, entries.size());
	EXPECT_EQ(EOutputLogLevel::Info, entries[0].level);
	EXPECT_EQ("> git -C C:\\repo status [42ms]", entries[0].message);
}

TEST(GitOutputChannel, BuildEntriesAddsAStderrEntryOnlyWhenStderrIsNonEmpty)
{
	const std::vector<std::wstring> arguments{ L"-C", L"C:\\repo", L"fetch" };
	const auto entries = BuildGitOutputLogEntries(arguments, std::chrono::milliseconds(7),
		Ok("fatal: unable to access 'https://example.invalid/': timeout\n"));

	ASSERT_EQ(2U, entries.size());
	EXPECT_EQ("> git -C C:\\repo fetch [7ms]", entries[0].message);
	EXPECT_EQ(EOutputLogLevel::Info, entries[1].level);
	EXPECT_EQ("fatal: unable to access 'https://example.invalid/': timeout", entries[1].message);
}

TEST(GitOutputChannel, BuildEntriesPreservesEmbeddedNewlinesButDropsTrailingBlankLines)
{
	const std::vector<std::wstring> arguments{ L"-C", L"C:\\repo", L"push" };
	const auto entries = BuildGitOutputLogEntries(arguments, std::chrono::milliseconds(9),
		Ok("remote: line one\r\nremote: line two\r\n\r\n\r\n"));

	ASSERT_EQ(2U, entries.size());
	EXPECT_EQ("remote: line one\nremote: line two", entries[1].message);
}

TEST(GitOutputChannel, RunGitLoggedWithoutAServiceBehavesLikeABareRunGitCall)
{
	// An invalid request (empty working directory) returns InvalidRequest
	// without spawning a process, so this exercises the null-sink passthrough
	// path deterministically and without a git.exe dependency.
	GitExecutionRequest request;
	request.arguments = { L"status" };

	GitOutputSink sink; // service == nullptr
	const auto result = RunGitLogged(request, nullptr, sink);
	EXPECT_EQ(EGitExecutionStatus::InvalidRequest, result.status);
}

TEST(GitOutputChannel, RunGitLoggedSkipsLoggingWhenTheOperationIdFactoryIsExhaustedButStillReturnsTheGitResult)
{
	OutputService service;
	const OutputOwner owner{ .ownerId = "workbench.scm.git", .generation = 1 };

	GitExecutionRequest request; // invalid: no working directory, no process spawned
	request.arguments = { L"status" };

	GitOutputSink sink;
	sink.service = &service;
	sink.owner = owner;
	sink.createOperationId = "create";
	sink.nextAppendOperationId = []() -> std::optional<std::string> { return std::nullopt; };

	const auto result = RunGitLogged(request, nullptr, sink);
	EXPECT_EQ(EGitExecutionStatus::InvalidRequest, result.status);

	// EnsureGitOutputChannel still runs (it is unconditional), but no log entry
	// is appended because the append-operation-id factory was exhausted.
	// The snapshot is returned by value, so it must outlive the pointer
	// FindGitChannel hands back.
	const auto snapshot = service.Snapshot();
	const auto* channel = FindGitChannel(snapshot);
	ASSERT_NE(nullptr, channel);
	EXPECT_TRUE(channel->logEntries.empty());
}
