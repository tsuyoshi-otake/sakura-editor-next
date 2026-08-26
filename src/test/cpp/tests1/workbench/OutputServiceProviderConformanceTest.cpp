/* Provider-neutral conformance tests for the OutputService authority boundary. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/output/IOutputService.h"
#include "workbench/output/OutputService.h"
#include "workbench/output/OutputServiceRustProvider.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::output {
namespace {

OutputOwner ConformanceOwner(std::string ownerId, const std::uint64_t generation = 1)
{
	return { .ownerId = std::move(ownerId), .generation = generation };
}

OutputOperation ConformanceOperation(
	std::string operationId,
	const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .operationId = std::move(operationId), .expectedRevision = expectedRevision };
}

OutputCreateChannelRequest ConformanceCreate(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	const EOutputChannelKind kind = EOutputChannelKind::Output)
{
	return {
		.operation = ConformanceOperation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.label = "Conformance channel",
		.kind = kind,
		.metadata = { .languageId = std::string("plaintext"), .source = std::string("conformance") },
	};
}

OutputTextMutationRequest ConformanceText(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	std::string text,
	const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return {
		.operation = ConformanceOperation(std::move(operationId), expectedRevision),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.text = std::move(text),
	};
}

OutputLogMutationRequest ConformanceLog(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	std::vector<OutputLogEntry> entries)
{
	return {
		.operation = ConformanceOperation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.entries = std::move(entries),
	};
}

OutputChannelMutationRequest ConformanceChannel(
	std::string operationId,
	OutputOwner owner,
	std::string channelId)
{
	return {
		.operation = ConformanceOperation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
	};
}

OutputShowChannelRequest ConformanceShow(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	const bool preserveFocus = false)
{
	return {
		.operation = ConformanceOperation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.preserveFocus = preserveFocus,
	};
}

OutputDisposeOwnerRequest ConformanceDisposeOwner(
	std::string operationId,
	OutputOwner owner)
{
	return {
		.operation = ConformanceOperation(std::move(operationId)),
		.owner = std::move(owner),
	};
}

struct ProviderPair final {
	std::unique_ptr<OutputService> cpp;
	std::unique_ptr<OutputServiceRustProvider> rust;
};

ProviderPair MakeProviderPair(const OutputServiceLimits& limits = {})
{
	return {
		.cpp = std::make_unique<OutputService>(limits),
		.rust = std::make_unique<OutputServiceRustProvider>(limits),
	};
}

void AssertRustProviderReady(const ProviderPair& providers)
{
	ASSERT_TRUE(OutputServiceRustProvider::IsCompiledIn());
	ASSERT_TRUE(providers.rust->IsAvailable())
		<< "Rust provider diagnostics state="
		<< static_cast<unsigned>(providers.rust->Diagnostics().state)
		<< " fault=" << static_cast<unsigned>(providers.rust->Diagnostics().fault);
}

void ExpectResultParity(
	const OutputOperationResult& expected,
	const OutputOperationResult& actual)
{
	EXPECT_EQ(expected.status, actual.status);
	EXPECT_EQ(expected.reason, actual.reason);
	EXPECT_EQ(expected.revision, actual.revision);
	EXPECT_EQ(expected.callbackDrainDeferred, actual.callbackDrainDeferred);
}

void ExpectExpectedResult(
	const OutputOperationResult& result,
	const EOutputOperationStatus status,
	const EOutputOperationReason reason,
	const std::uint64_t revision)
{
	EXPECT_EQ(status, result.status);
	EXPECT_EQ(reason, result.reason);
	EXPECT_EQ(revision, result.revision);
}

void ExpectSnapshotParity(
	const OutputServiceSnapshot& expected,
	const OutputServiceSnapshot& actual)
{
	EXPECT_EQ(expected.revision, actual.revision);
	EXPECT_EQ(expected.stopped, actual.stopped);
	// droppedNotificationCount is advisory and belongs to the C++ dispatcher
	// owned by each provider. It is intentionally not part of authority parity.
	EXPECT_EQ(expected.activeChannelId, actual.activeChannelId);
	ASSERT_EQ(expected.channels.size(), actual.channels.size());
	for (std::size_t index = 0; index < expected.channels.size(); ++index) {
		const auto& expectedChannel = expected.channels[index];
		const auto& actualChannel = actual.channels[index];
		EXPECT_EQ(expectedChannel.channelId, actualChannel.channelId);
		EXPECT_EQ(expectedChannel.label, actualChannel.label);
		EXPECT_EQ(expectedChannel.owner, actualChannel.owner);
		EXPECT_EQ(expectedChannel.kind, actualChannel.kind);
		EXPECT_EQ(expectedChannel.metadata, actualChannel.metadata);
		EXPECT_EQ(expectedChannel.visible, actualChannel.visible);
		EXPECT_EQ(expectedChannel.lastShowPreservedFocus, actualChannel.lastShowPreservedFocus);
		EXPECT_EQ(expectedChannel.droppedCharacterCount, actualChannel.droppedCharacterCount);
		EXPECT_EQ(expectedChannel.text, actualChannel.text);
		EXPECT_EQ(expectedChannel.logEntries, actualChannel.logEntries);
		EXPECT_EQ(expectedChannel.projectedText, actualChannel.projectedText);
	}
}

void ExpectSnapshotsExactlyEqual(
	const OutputServiceSnapshot& expected,
	const OutputServiceSnapshot& actual)
{
	ExpectSnapshotParity(expected, actual);
	EXPECT_EQ(expected.droppedNotificationCount, actual.droppedNotificationCount);
}

template <typename Mutation>
OutputOperationResult CompareMutation(
	ProviderPair& providers,
	const char* description,
	Mutation&& mutation)
{
	SCOPED_TRACE(description);
	const auto cppResult = mutation(*providers.cpp);
	const auto rustResult = mutation(*providers.rust);
	ExpectResultParity(cppResult, rustResult);
	ExpectSnapshotParity(providers.cpp->Snapshot(), providers.rust->Snapshot());
	return cppResult;
}

#if defined(SAKURA_OUTPUT_BACKEND_RUST)

TEST(OutputServiceProviderConformance, EveryAcceptedMutationKindMatchesCppAuthority)
{
	auto providers = MakeProviderPair();
	AssertRustProviderReady(providers);
	const auto owner = ConformanceOwner("all-kinds.owner");

	const auto createOutput = ConformanceCreate(
		"all-kinds.create-output", owner, "z.output", EOutputChannelKind::Output);
	const auto createOutputResult = CompareMutation(providers, "Create output channel", [&](IOutputService& service) {
		return service.CreateChannel(createOutput);
	});
	ExpectExpectedResult(createOutputResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 2);

	const auto createLog = ConformanceCreate(
		"all-kinds.create-log", owner, "a.log", EOutputChannelKind::Log);
	const auto createLogResult = CompareMutation(providers, "Create log channel", [&](IOutputService& service) {
		return service.CreateChannel(createLog);
	});
	ExpectExpectedResult(createLogResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);
	const auto twoChannelCppSnapshot = providers.cpp->Snapshot();
	const auto twoChannelRustSnapshot = providers.rust->Snapshot();
	ExpectSnapshotsExactlyEqual(twoChannelCppSnapshot, twoChannelRustSnapshot);
	ASSERT_EQ(2U, twoChannelCppSnapshot.channels.size());
	EXPECT_EQ("a.log", twoChannelCppSnapshot.channels[0].channelId);
	EXPECT_EQ("z.output", twoChannelCppSnapshot.channels[1].channelId);

	const auto appendOutput = ConformanceText(
		"all-kinds.append-output", owner, "z.output", "first output");
	const auto appendOutputResult = CompareMutation(providers, "Append output", [&](IOutputService& service) {
		return service.AppendOutput(appendOutput);
	});
	ExpectExpectedResult(appendOutputResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 4);

	const auto replaceOutput = ConformanceText(
		"all-kinds.replace-output", owner, "z.output", "replacement output");
	const auto replaceOutputResult = CompareMutation(providers, "Replace output", [&](IOutputService& service) {
		return service.ReplaceOutput(replaceOutput);
	});
	ExpectExpectedResult(replaceOutputResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 5);

	const auto appendLog = ConformanceLog(
		"all-kinds.append-log", owner, "a.log",
		{ OutputLogEntry{ .level = EOutputLogLevel::Warning,
			.message = "warning", .source = std::string("conformance") } });
	const auto appendLogResult = CompareMutation(providers, "Append structured log", [&](IOutputService& service) {
		return service.AppendLog(appendLog);
	});
	ExpectExpectedResult(appendLogResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 6);

	const auto clearLog = ConformanceChannel("all-kinds.clear", owner, "a.log");
	const auto clearResult = CompareMutation(providers, "Clear log channel", [&](IOutputService& service) {
		return service.Clear(clearLog);
	});
	ExpectExpectedResult(clearResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 7);

	const auto showOutput = ConformanceShow("all-kinds.show", owner, "z.output", true);
	const auto showResult = CompareMutation(providers, "Show output channel", [&](IOutputService& service) {
		return service.Show(showOutput);
	});
	ExpectExpectedResult(showResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 8);

	const auto hideOutput = ConformanceChannel("all-kinds.hide", owner, "z.output");
	const auto hideResult = CompareMutation(providers, "Hide output channel", [&](IOutputService& service) {
		return service.Hide(hideOutput);
	});
	ExpectExpectedResult(hideResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 9);

	const auto disposeOutput = ConformanceChannel("all-kinds.dispose", owner, "z.output");
	const auto disposeResult = CompareMutation(providers, "Dispose output channel", [&](IOutputService& service) {
		return service.Dispose(disposeOutput);
	});
	ExpectExpectedResult(disposeResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 10);

	const auto disposeOwner = ConformanceDisposeOwner("all-kinds.dispose-owner", owner);
	const auto disposeOwnerResult = CompareMutation(providers, "Dispose owner", [&](IOutputService& service) {
		return service.DisposeOwner(disposeOwner);
	});
	ExpectExpectedResult(disposeOwnerResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 11);

	const auto cppSnapshot = providers.cpp->Snapshot();
	const auto rustSnapshot = providers.rust->Snapshot();
	ExpectSnapshotsExactlyEqual(cppSnapshot, rustSnapshot);
	ExpectSnapshotParity(cppSnapshot, rustSnapshot);
	EXPECT_TRUE(cppSnapshot.channels.empty());
}

TEST(OutputServiceProviderConformance, ReplayConflictStaleRevisionAndNotApplicableAreTyped)
{
	auto providers = MakeProviderPair();
	AssertRustProviderReady(providers);
	const auto owner = ConformanceOwner("result.owner");

	const auto create = ConformanceCreate("result.create", owner, "result.output");
	const auto created = CompareMutation(providers, "Initial create", [&](IOutputService& service) {
		return service.CreateChannel(create);
	});
	ExpectExpectedResult(created, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 2);

	const auto replayed = CompareMutation(providers, "Exact replay", [&](IOutputService& service) {
		return service.CreateChannel(create);
	});
	ExpectExpectedResult(replayed, EOutputOperationStatus::Replayed,
		EOutputOperationReason::None, 2);

	const auto operationConflict = ConformanceCreate(
		"result.create", owner, "different.output");
	const auto conflict = CompareMutation(providers, "Operation ID conflict", [&](IOutputService& service) {
		return service.CreateChannel(operationConflict);
	});
	ExpectExpectedResult(conflict, EOutputOperationStatus::Conflict,
		EOutputOperationReason::OperationIdConflict, 2);

	const auto stale = ConformanceText(
		"result.stale", owner, "result.output", "stale", 1);
	const auto staleResult = CompareMutation(providers, "Stale expected revision", [&](IOutputService& service) {
		return service.AppendOutput(stale);
	});
	ExpectExpectedResult(staleResult, EOutputOperationStatus::StaleRevision,
		EOutputOperationReason::ExpectedRevisionMismatch, 2);

	const auto invalidOwner = ConformanceText(
		"result.invalid-owner", {}, "result.output", "valid payload");
	const auto invalidOwnerResult = CompareMutation(providers, "Rejected invalid owner", [&](IOutputService& service) {
		return service.AppendOutput(invalidOwner);
	});
	ExpectExpectedResult(invalidOwnerResult, EOutputOperationStatus::Rejected,
		EOutputOperationReason::InvalidOwner, 2);

	const auto emptyAppend = ConformanceText(
		"result.empty", owner, "result.output", "");
	const auto emptyResult = CompareMutation(providers, "Not applicable empty append", [&](IOutputService& service) {
		return service.AppendOutput(emptyAppend);
	});
	ExpectExpectedResult(emptyResult, EOutputOperationStatus::NotApplicable,
		EOutputOperationReason::None, 2);

	const auto missingClear = ConformanceChannel(
		"result.missing-clear", owner, "missing.channel");
	const auto missingResult = CompareMutation(providers, "Not applicable missing channel", [&](IOutputService& service) {
		return service.Clear(missingClear);
	});
	ExpectExpectedResult(missingResult, EOutputOperationStatus::NotApplicable,
		EOutputOperationReason::ChannelNotFound, 2);

	const auto createLog = ConformanceCreate(
		"result.create-log", owner, "result.log", EOutputChannelKind::Log);
	const auto logResult = CompareMutation(providers, "Create kind mismatch fixture", [&](IOutputService& service) {
		return service.CreateChannel(createLog);
	});
	ExpectExpectedResult(logResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);

	const auto wrongKind = ConformanceText(
		"result.wrong-kind", owner, "result.log", "not output");
	const auto wrongKindResult = CompareMutation(providers, "Rejected channel kind mismatch", [&](IOutputService& service) {
		return service.AppendOutput(wrongKind);
	});
	ExpectExpectedResult(wrongKindResult, EOutputOperationStatus::Rejected,
		EOutputOperationReason::ChannelKindMismatch, 3);
}

TEST(OutputServiceProviderConformance, OwnerGenerationReplacementAndFencingMatch)
{
	auto providers = MakeProviderPair();
	AssertRustProviderReady(providers);
	const auto firstGeneration = ConformanceOwner("generation.owner", 1);
	const auto secondGeneration = ConformanceOwner("generation.owner", 2);
	const auto thirdGeneration = ConformanceOwner("generation.owner", 3);

	const auto createOldOutput = ConformanceCreate(
		"generation.create-old-output", firstGeneration, "old.output");
	const auto oldOutputResult = CompareMutation(providers, "Create generation one output", [&](IOutputService& service) {
		return service.CreateChannel(createOldOutput);
	});
	ExpectExpectedResult(oldOutputResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 2);

	const auto createOldLog = ConformanceCreate(
		"generation.create-old-log", firstGeneration, "old.log", EOutputChannelKind::Log);
	const auto oldLogResult = CompareMutation(providers, "Create second generation one channel", [&](IOutputService& service) {
		return service.CreateChannel(createOldLog);
	});
	ExpectExpectedResult(oldLogResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);

	const auto createNewOutput = ConformanceCreate(
		"generation.create-new-output", secondGeneration, "new.output");
	const auto replacementResult = CompareMutation(providers, "Replace all channels on new generation", [&](IOutputService& service) {
		return service.CreateChannel(createNewOutput);
	});
	ExpectExpectedResult(replacementResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 4);
	ASSERT_EQ(1U, providers.cpp->Snapshot().channels.size());
	EXPECT_EQ("new.output", providers.cpp->Snapshot().channels.front().channelId);

	const auto lateOldAppend = ConformanceText(
		"generation.late-old-append", firstGeneration, "old.output", "fenced");
	const auto lateOldAppendResult = CompareMutation(providers, "Removed old channel is fenced", [&](IOutputService& service) {
		return service.AppendOutput(lateOldAppend);
	});
	ExpectExpectedResult(lateOldAppendResult, EOutputOperationStatus::NotApplicable,
		EOutputOperationReason::ChannelNotFound, 4);

	const auto lateOldCreate = ConformanceCreate(
		"generation.late-old-create", firstGeneration, "late.old");
	const auto lateOldCreateResult = CompareMutation(providers, "Old generation create is fenced", [&](IOutputService& service) {
		return service.CreateChannel(lateOldCreate);
	});
	ExpectExpectedResult(lateOldCreateResult, EOutputOperationStatus::Conflict,
		EOutputOperationReason::OwnerGenerationConflict, 4);

	const auto wrongDispose = ConformanceDisposeOwner(
		"generation.wrong-dispose", firstGeneration);
	const auto wrongDisposeResult = CompareMutation(providers, "Old generation dispose is fenced", [&](IOutputService& service) {
		return service.DisposeOwner(wrongDispose);
	});
	ExpectExpectedResult(wrongDisposeResult, EOutputOperationStatus::Conflict,
		EOutputOperationReason::OwnerGenerationConflict, 4);

	const auto disposeNew = ConformanceDisposeOwner(
		"generation.dispose-new", secondGeneration);
	const auto disposeNewResult = CompareMutation(providers, "Dispose current generation", [&](IOutputService& service) {
		return service.DisposeOwner(disposeNew);
	});
	ExpectExpectedResult(disposeNewResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 5);

	const auto repeatedDispose = ConformanceDisposeOwner(
		"generation.repeated-dispose", secondGeneration);
	const auto repeatedDisposeResult = CompareMutation(providers, "Repeated owner dispose is not applicable", [&](IOutputService& service) {
		return service.DisposeOwner(repeatedDispose);
	});
	ExpectExpectedResult(repeatedDisposeResult, EOutputOperationStatus::NotApplicable,
		EOutputOperationReason::ChannelNotFound, 5);

	const auto sameGenerationCreate = ConformanceCreate(
		"generation.same-generation-create", secondGeneration, "same-generation");
	const auto sameGenerationResult = CompareMutation(providers, "Disposed generation cannot be reused", [&](IOutputService& service) {
		return service.CreateChannel(sameGenerationCreate);
	});
	ExpectExpectedResult(sameGenerationResult, EOutputOperationStatus::Conflict,
		EOutputOperationReason::OwnerGenerationConflict, 5);

	const auto createNewest = ConformanceCreate(
		"generation.create-newest", thirdGeneration, "newest.output");
	const auto newestResult = CompareMutation(providers, "Adopt a newer generation", [&](IOutputService& service) {
		return service.CreateChannel(createNewest);
	});
	ExpectExpectedResult(newestResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 6);
}

TEST(OutputServiceProviderConformance, BoundedLimitsAndUtf8TruncationMatch)
{
	OutputServiceLimits limits;
	limits.maximumOwners = 1;
	limits.maximumChannels = 2;
	limits.maximumTextBytesPerChannel = 5;
	limits.maximumPayloadBytes = 4;
	limits.maximumLogEntriesPerChannel = 1;
	limits.maximumRememberedOperations = 32;
	auto providers = MakeProviderPair(limits);
	AssertRustProviderReady(providers);
	const auto owner = ConformanceOwner("bounded.owner");

	const auto createOutput = ConformanceCreate(
		"bounded.create-output", owner, "bounded.output");
	const auto createOutputResult = CompareMutation(providers, "Create bounded output", [&](IOutputService& service) {
		return service.CreateChannel(createOutput);
	});
	ExpectExpectedResult(createOutputResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 2);

	const auto firstText = ConformanceText(
		"bounded.first-text", owner, "bounded.output", "ab");
	const auto firstTextResult = CompareMutation(providers, "Append initial text", [&](IOutputService& service) {
		return service.AppendOutput(firstText);
	});
	ExpectExpectedResult(firstTextResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);

	const auto unicodeText = ConformanceText(
		"bounded.unicode-text", owner, "bounded.output", "\xE3\x81\x82");
	const auto unicodeTextResult = CompareMutation(providers, "Append UTF-8 text", [&](IOutputService& service) {
		return service.AppendOutput(unicodeText);
	});
	ExpectExpectedResult(unicodeTextResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 4);

	const auto suffixText = ConformanceText(
		"bounded.suffix-text", owner, "bounded.output", "x");
	const auto suffixTextResult = CompareMutation(providers, "Truncate at UTF-8 boundary", [&](IOutputService& service) {
		return service.AppendOutput(suffixText);
	});
	ExpectExpectedResult(suffixTextResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 5);
	const auto truncated = providers.cpp->Snapshot();
	ASSERT_EQ(1U, truncated.channels.size());
	EXPECT_EQ("b\xE3\x81\x82x", truncated.channels.front().text);
	EXPECT_EQ(1U, truncated.channels.front().droppedCharacterCount);

	const auto oversizedText = ConformanceText(
		"bounded.oversized", owner, "bounded.output", "12345");
	const auto oversizedResult = CompareMutation(providers, "Reject oversized payload", [&](IOutputService& service) {
		return service.AppendOutput(oversizedText);
	});
	ExpectExpectedResult(oversizedResult, EOutputOperationStatus::Rejected,
		EOutputOperationReason::PayloadLimitExceeded, 5);

	const auto invalidUtf8 = ConformanceText(
		"bounded.invalid-utf8", owner, "bounded.output", std::string("\xC3", 1));
	const auto invalidUtf8Result = CompareMutation(providers, "Reject invalid UTF-8", [&](IOutputService& service) {
		return service.AppendOutput(invalidUtf8);
	});
	ExpectExpectedResult(invalidUtf8Result, EOutputOperationStatus::Rejected,
		EOutputOperationReason::InvalidPayload, 5);

	const auto createLog = ConformanceCreate(
		"bounded.create-log", owner, "bounded.log", EOutputChannelKind::Log);
	const auto createLogResult = CompareMutation(providers, "Create bounded log", [&](IOutputService& service) {
		return service.CreateChannel(createLog);
	});
	ExpectExpectedResult(createLogResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 6);

	const auto tooManyLogs = ConformanceLog(
		"bounded.too-many-logs", owner, "bounded.log",
		{ OutputLogEntry{ .message = "one" }, OutputLogEntry{ .message = "two" } });
	const auto tooManyLogsResult = CompareMutation(providers, "Reject excessive log entries", [&](IOutputService& service) {
		return service.AppendLog(tooManyLogs);
	});
	ExpectExpectedResult(tooManyLogsResult, EOutputOperationStatus::Rejected,
		EOutputOperationReason::LogEntryLimitExceeded, 6);

	const auto oneLog = ConformanceLog(
		"bounded.one-log", owner, "bounded.log",
		{ OutputLogEntry{ .level = EOutputLogLevel::Info, .message = "one" } });
	const auto oneLogResult = CompareMutation(providers, "Accept one bounded log entry", [&](IOutputService& service) {
		return service.AppendLog(oneLog);
	});
	ExpectExpectedResult(oneLogResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 7);

	const auto secondLog = ConformanceLog(
		"bounded.second-log", owner, "bounded.log",
		{ OutputLogEntry{ .level = EOutputLogLevel::Error, .message = "two" } });
	const auto secondLogResult = CompareMutation(providers, "Bound log retention to one entry", [&](IOutputService& service) {
		return service.AppendLog(secondLog);
	});
	ExpectExpectedResult(secondLogResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 8);

	const auto channelLimit = ConformanceCreate(
		"bounded.channel-limit", owner, "third.channel");
	const auto channelLimitResult = CompareMutation(providers, "Reject channel limit", [&](IOutputService& service) {
		return service.CreateChannel(channelLimit);
	});
	ExpectExpectedResult(channelLimitResult, EOutputOperationStatus::Rejected,
		EOutputOperationReason::ChannelLimitExceeded, 8);

	const auto ownerLimit = ConformanceCreate(
		"bounded.owner-limit", ConformanceOwner("other.owner"), "other.channel");
	const auto ownerLimitResult = CompareMutation(providers, "Reject owner limit", [&](IOutputService& service) {
		return service.CreateChannel(ownerLimit);
	});
	ExpectExpectedResult(ownerLimitResult, EOutputOperationStatus::Rejected,
		EOutputOperationReason::OwnerLimitExceeded, 8);
}

TEST(OutputServiceProviderConformance, SnapshotDeterminismStopAndPostStopResultsMatch)
{
	auto providers = MakeProviderPair();
	AssertRustProviderReady(providers);
	const auto owner = ConformanceOwner("stop.owner");
	const auto create = ConformanceCreate("stop.create", owner, "stop.output");
	const auto createResult = CompareMutation(providers, "Create before stop", [&](IOutputService& service) {
		return service.CreateChannel(create);
	});
	ExpectExpectedResult(createResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 2);
	const auto append = ConformanceText("stop.append", owner, "stop.output", "before stop");
	const auto appendResult = CompareMutation(providers, "Append before stop", [&](IOutputService& service) {
		return service.AppendOutput(append);
	});
	ExpectExpectedResult(appendResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);

	const auto cppBeforeStop = providers.cpp->Snapshot();
	const auto cppRepeatedBeforeStop = providers.cpp->Snapshot();
	ExpectSnapshotsExactlyEqual(cppBeforeStop, cppRepeatedBeforeStop);
	const auto rustBeforeStop = providers.rust->Snapshot();
	const auto rustRepeatedBeforeStop = providers.rust->Snapshot();
	ExpectSnapshotsExactlyEqual(rustBeforeStop, rustRepeatedBeforeStop);
	ExpectSnapshotParity(cppBeforeStop, rustBeforeStop);

	const auto stopResult = CompareMutation(providers, "First Stop", [&](IOutputService& service) {
		return service.Stop();
	});
	ExpectExpectedResult(stopResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 4);
	EXPECT_FALSE(stopResult.callbackDrainDeferred);

	const auto repeatedStopResult = CompareMutation(providers, "Repeated Stop", [&](IOutputService& service) {
		return service.Stop();
	});
	ExpectExpectedResult(repeatedStopResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 4);
	EXPECT_FALSE(repeatedStopResult.callbackDrainDeferred);

	const auto postStopCreate = ConformanceCreate("stop.post-create", owner, "post-create");
	const auto postStopCreateResult = CompareMutation(providers, "Post-stop CreateChannel", [&](IOutputService& service) {
		return service.CreateChannel(postStopCreate);
	});
	ExpectExpectedResult(postStopCreateResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopAppend = ConformanceText("stop.post-append", owner, "stop.output", "ignored");
	const auto postStopAppendResult = CompareMutation(providers, "Post-stop AppendOutput", [&](IOutputService& service) {
		return service.AppendOutput(postStopAppend);
	});
	ExpectExpectedResult(postStopAppendResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopReplace = ConformanceText("stop.post-replace", owner, "stop.output", "ignored");
	const auto postStopReplaceResult = CompareMutation(providers, "Post-stop ReplaceOutput", [&](IOutputService& service) {
		return service.ReplaceOutput(postStopReplace);
	});
	ExpectExpectedResult(postStopReplaceResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopLog = ConformanceLog(
		"stop.post-log", owner, "stop.log", { OutputLogEntry{ .message = "ignored" } });
	const auto postStopLogResult = CompareMutation(providers, "Post-stop AppendLog", [&](IOutputService& service) {
		return service.AppendLog(postStopLog);
	});
	ExpectExpectedResult(postStopLogResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopChannel = ConformanceChannel("stop.post-channel", owner, "stop.output");
	const auto postStopClearResult = CompareMutation(providers, "Post-stop Clear", [&](IOutputService& service) {
		return service.Clear(postStopChannel);
	});
	ExpectExpectedResult(postStopClearResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopShow = ConformanceShow("stop.post-show", owner, "stop.output");
	const auto postStopShowResult = CompareMutation(providers, "Post-stop Show", [&](IOutputService& service) {
		return service.Show(postStopShow);
	});
	ExpectExpectedResult(postStopShowResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopHideResult = CompareMutation(providers, "Post-stop Hide", [&](IOutputService& service) {
		return service.Hide(postStopChannel);
	});
	ExpectExpectedResult(postStopHideResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopDisposeResult = CompareMutation(providers, "Post-stop Dispose", [&](IOutputService& service) {
		return service.Dispose(postStopChannel);
	});
	ExpectExpectedResult(postStopDisposeResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto postStopDisposeOwner = ConformanceDisposeOwner("stop.post-dispose-owner", owner);
	const auto postStopDisposeOwnerResult = CompareMutation(providers, "Post-stop DisposeOwner", [&](IOutputService& service) {
		return service.DisposeOwner(postStopDisposeOwner);
	});
	ExpectExpectedResult(postStopDisposeOwnerResult, EOutputOperationStatus::Stopped,
		EOutputOperationReason::None, 4);

	const auto cppStopped = providers.cpp->Snapshot();
	const auto rustStopped = providers.rust->Snapshot();
	ExpectSnapshotsExactlyEqual(cppStopped, rustStopped);
	ExpectSnapshotParity(cppStopped, rustStopped);
	EXPECT_TRUE(cppStopped.stopped);
	EXPECT_TRUE(cppStopped.channels.empty());
}

void RunAdvisoryFifoAndReentrancyConformance(IOutputService& service)
{
	std::vector<OutputServiceChange> changes;
	std::size_t callbackDepth{};
	std::size_t maximumCallbackDepth{};
	bool reentrantMutationStarted{};
	OutputOperationResult reentrantMutationResult{};
	const auto owner = ConformanceOwner("advisory.owner");

	ASSERT_TRUE(service.Subscribe([](const OutputServiceChange&) {
		throw std::runtime_error("advisory listener failure");
	}));
	const auto recordingSubscription = service.Subscribe([&](const OutputServiceChange& change) {
		++callbackDepth;
		maximumCallbackDepth = std::max(maximumCallbackDepth, callbackDepth);
		changes.push_back(change);
		if (!reentrantMutationStarted && change.revision == 2) {
			reentrantMutationStarted = true;
			reentrantMutationResult = service.AppendOutput(
				ConformanceText("advisory.reentrant-append", owner, "advisory.output", "reentrant"));
		}
		--callbackDepth;
	});
	ASSERT_TRUE(recordingSubscription);

	const auto createResult = service.CreateChannel(
		ConformanceCreate("advisory.create", owner, "advisory.output"));
	ASSERT_EQ(EOutputOperationStatus::Succeeded, createResult.status);
	EXPECT_EQ(EOutputOperationReason::None, createResult.reason);
	EXPECT_TRUE(reentrantMutationStarted);
	ExpectExpectedResult(reentrantMutationResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);
	EXPECT_EQ(1U, maximumCallbackDepth);
	ASSERT_EQ(2U, changes.size());
	EXPECT_EQ(2U, changes[0].revision);
	EXPECT_EQ(EOutputChangeKind::ChannelCreated, changes[0].kind);
	EXPECT_EQ("advisory.output", changes[0].channelId);
	EXPECT_EQ(3U, changes[1].revision);
	EXPECT_EQ(EOutputChangeKind::ContentAppended, changes[1].kind);
	EXPECT_EQ("advisory.output", changes[1].channelId);
	EXPECT_EQ(std::optional<std::string>("advisory.output"), changes[0].activeChannelId);
	EXPECT_EQ(std::optional<std::string>("advisory.output"), changes[1].activeChannelId);

	service.Unsubscribe(*recordingSubscription);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, service.Stop().status);
}

void RunCallbackOriginStopConformance(IOutputService& service)
{
	std::optional<OutputOperationResult> callbackStop;
	std::optional<OutputOperationResult> callbackRepeatedStop;
	std::size_t callbacks{};
	const auto subscription = service.Subscribe([&](const OutputServiceChange&) {
		++callbacks;
		if (callbacks != 1) return;
		callbackStop = service.Stop();
		callbackRepeatedStop = service.Stop();
	});
	ASSERT_TRUE(subscription);

	const auto create = service.CreateChannel(
		ConformanceCreate("callback-stop.create", ConformanceOwner("callback-stop.owner"), "callback-stop.output"));
	ASSERT_EQ(EOutputOperationStatus::Succeeded, create.status);
	ASSERT_TRUE(callbackStop.has_value());
	ASSERT_TRUE(callbackRepeatedStop.has_value());
	ExpectExpectedResult(*callbackStop, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);
	ExpectExpectedResult(*callbackRepeatedStop, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);
	EXPECT_TRUE(callbackStop->callbackDrainDeferred);
	EXPECT_TRUE(callbackRepeatedStop->callbackDrainDeferred);
	EXPECT_EQ(1U, callbacks);

	// The callback-origin call is deliberately deferred. An external retry is
	// the quiescent terminal observation and must no longer report deferral.
	const auto externalRetry = service.Stop();
	ExpectExpectedResult(externalRetry, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);
	EXPECT_FALSE(externalRetry.callbackDrainDeferred);
}

TEST(OutputServiceProviderConformance, AdvisoryCallbacksAreFifoNonRecursiveAndContainThrowingListeners)
{
	auto providers = MakeProviderPair();
	AssertRustProviderReady(providers);
	RunAdvisoryFifoAndReentrancyConformance(*providers.cpp);
	RunAdvisoryFifoAndReentrancyConformance(*providers.rust);
}

TEST(OutputServiceProviderConformance, CallbackOriginStopDefersAndExternalRetryCompletes)
{
	auto providers = MakeProviderPair();
	AssertRustProviderReady(providers);
	RunCallbackOriginStopConformance(*providers.cpp);
	RunCallbackOriginStopConformance(*providers.rust);
}

struct BoundedNotificationOutcome final {
	OutputOperationResult createResult{};
	OutputOperationResult appendResult{};
	OutputOperationResult replaceResult{};
	OutputOperationResult stopResult{};
	std::size_t blockerCallbackCount{};
	std::size_t unsubscribedCallbackCount{};
	bool externalStopBlocked{};
	OutputServiceSnapshot snapshot;
};

std::optional<BoundedNotificationOutcome> RunBoundedNotificationConformance(
	IOutputService& service)
{
	using namespace std::chrono_literals;
	std::mutex callbackMutex;
	std::condition_variable callbackCondition;
	bool callbackEntered{};
	bool releaseCallback{};
	std::size_t blockerCallbackCount{};
	std::size_t unsubscribedCallbackCount{};

	const auto blocker = service.Subscribe([&](const OutputServiceChange&) {
		std::unique_lock lock(callbackMutex);
		++blockerCallbackCount;
		if (blockerCallbackCount == 1) {
			callbackEntered = true;
			callbackCondition.notify_all();
			callbackCondition.wait(lock, [&] { return releaseCallback; });
		}
	});
	if (!blocker) {
		ADD_FAILURE() << "bounded notification blocker subscription failed";
		return std::nullopt;
	}
	const auto toUnsubscribe = service.Subscribe([&](const OutputServiceChange&) {
		++unsubscribedCallbackCount;
	});
	if (!toUnsubscribe) {
		ADD_FAILURE() << "bounded notification unsubscribe subscription failed";
		service.Unsubscribe(*blocker);
		return std::nullopt;
	}

	OutputOperationResult createResult{};
	const auto owner = ConformanceOwner("bounded-notification.owner");
	std::thread creator([&] {
		createResult = service.CreateChannel(
			ConformanceCreate("bounded-notification.create", owner, "bounded-notification.output"));
	});
	bool enteredInTime{};
	{
		std::unique_lock lock(callbackMutex);
		enteredInTime = callbackCondition.wait_for(lock, 2s, [&] { return callbackEntered; });
	}
	if (!enteredInTime) {
		{
			std::lock_guard lock(callbackMutex);
			releaseCallback = true;
		}
		callbackCondition.notify_all();
		creator.join();
		service.Unsubscribe(*blocker);
		service.Unsubscribe(*toUnsubscribe);
		(void)service.Stop();
		ADD_FAILURE() << "bounded notification callback did not start in time";
		return std::nullopt;
	}

	// The first callback is active. The second mutation occupies the one
	// pending slot, while the third accepted mutation drops only its advisory
	// notification. Removing the second listener while the first callback is
	// active must prevent its copied subscription ID from being invoked.
	service.Unsubscribe(*toUnsubscribe);
	const auto appendResult = service.AppendOutput(
		ConformanceText("bounded-notification.append", owner,
			"bounded-notification.output", "append"));
	const auto replaceResult = service.ReplaceOutput(
		ConformanceText("bounded-notification.replace", owner,
			"bounded-notification.output", "replace"));

	std::mutex stopMutex;
	std::condition_variable stopCondition;
	bool stopStarted{};
	bool stopFinished{};
	std::optional<OutputOperationResult> stopResult;
	std::thread stopper([&] {
		{
			std::lock_guard lock(stopMutex);
			stopStarted = true;
		}
		stopCondition.notify_all();
		const auto result = service.Stop();
		{
			std::lock_guard lock(stopMutex);
			stopResult = result;
			stopFinished = true;
		}
		stopCondition.notify_all();
	});
	bool stopBlocked{};
	{
		std::unique_lock lock(stopMutex);
		const auto started = stopCondition.wait_for(lock, 2s, [&] { return stopStarted; });
		if (!started) {
			lock.unlock();
			{
				std::lock_guard callbackLock(callbackMutex);
				releaseCallback = true;
			}
			callbackCondition.notify_all();
			creator.join();
			stopper.join();
			ADD_FAILURE() << "external Stop did not start in time";
			return std::nullopt;
		}
		stopBlocked = !stopCondition.wait_for(lock, 100ms, [&] { return stopFinished; });
	}

	{
		std::lock_guard lock(callbackMutex);
		releaseCallback = true;
	}
	callbackCondition.notify_all();
	creator.join();
	stopper.join();
	if (!stopResult) {
		ADD_FAILURE() << "external Stop did not return";
		return std::nullopt;
	}

	return BoundedNotificationOutcome{
		.createResult = createResult,
		.appendResult = appendResult,
		.replaceResult = replaceResult,
		.stopResult = *stopResult,
		.blockerCallbackCount = blockerCallbackCount,
		.unsubscribedCallbackCount = unsubscribedCallbackCount,
		.externalStopBlocked = stopBlocked,
		.snapshot = service.Snapshot(),
	};
}

TEST(OutputServiceProviderConformance, BoundedAdvisoryDropUnsubscribeAndExternalStopMatch)
{
	OutputServiceLimits limits;
	limits.maximumPendingNotifications = 1;
	limits.maximumSubscriptions = 4;
	auto providers = MakeProviderPair(limits);
	AssertRustProviderReady(providers);
	const auto cpp = RunBoundedNotificationConformance(*providers.cpp);
	const auto rust = RunBoundedNotificationConformance(*providers.rust);
	ASSERT_TRUE(cpp.has_value());
	ASSERT_TRUE(rust.has_value());

	ExpectResultParity(cpp->createResult, rust->createResult);
	ExpectResultParity(cpp->appendResult, rust->appendResult);
	ExpectResultParity(cpp->replaceResult, rust->replaceResult);
	ExpectResultParity(cpp->stopResult, rust->stopResult);
	ExpectExpectedResult(cpp->createResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 2);
	ExpectExpectedResult(cpp->appendResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 3);
	ExpectExpectedResult(cpp->replaceResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 4);
	ExpectExpectedResult(cpp->stopResult, EOutputOperationStatus::Succeeded,
		EOutputOperationReason::None, 5);
	EXPECT_TRUE(cpp->externalStopBlocked);
	EXPECT_TRUE(rust->externalStopBlocked);
	EXPECT_EQ(1U, cpp->blockerCallbackCount);
	EXPECT_EQ(1U, rust->blockerCallbackCount);
	EXPECT_EQ(0U, cpp->unsubscribedCallbackCount);
	EXPECT_EQ(0U, rust->unsubscribedCallbackCount);
	EXPECT_EQ(1U, cpp->snapshot.droppedNotificationCount);
	EXPECT_EQ(cpp->snapshot.droppedNotificationCount, rust->snapshot.droppedNotificationCount);
	ExpectSnapshotsExactlyEqual(cpp->snapshot, rust->snapshot);
	EXPECT_TRUE(cpp->snapshot.stopped);
	EXPECT_TRUE(cpp->snapshot.channels.empty());
}

#endif

#if !defined(SAKURA_OUTPUT_BACKEND_RUST)

TEST(OutputServiceRustProvider, IsExplicitlyUnavailableWithoutFunctionalFallback)
{
	static_assert(!OutputServiceRustProvider::IsCompiledIn());
	OutputServiceRustProvider provider;
	EXPECT_FALSE(provider.IsCompiledIn());
	EXPECT_FALSE(provider.IsAvailable());
	const auto diagnostics = provider.Diagnostics();
	EXPECT_EQ(EOutputServiceRustProviderAvailability::Unavailable, diagnostics.availability);
	EXPECT_EQ(EOutputServiceRustProviderState::Unavailable, diagnostics.state);
	EXPECT_EQ(EOutputServiceRustProviderFault::Unavailable, diagnostics.fault);

	const auto before = provider.Snapshot();
	const auto create = provider.CreateChannel(
		ConformanceCreate("unavailable.create", ConformanceOwner("unavailable.owner"), "unavailable.output"));
	ExpectExpectedResult(create, EOutputOperationStatus::Rejected,
		EOutputOperationReason::InvalidPayload, 1);
	const auto after = provider.Snapshot();
	ExpectSnapshotsExactlyEqual(before, after);
	EXPECT_TRUE(after.channels.empty());
	EXPECT_FALSE(provider.Subscribe([](const OutputServiceChange&) {}));
}

#endif

} // namespace
} // namespace workbench::output
