/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/runtime/TerminalInputBatch.h"

#include <gtest/gtest.h>

namespace terminal {
namespace {

HarnessOperationId OperationId(std::uint8_t value)
{
	HarnessOperationId id;
	id.value[0] = value;
	return id;
}

TerminalInputBatch MakeBatch()
{
	TerminalInputBatch batch;
	batch.operationId = OperationId(1);
	batch.target.runtimeGeneration = TerminalRuntimeGeneration{ 1 };
	batch.target.instanceId = TerminalInstanceId{ 1 };
	batch.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
	return batch;
}

TEST(TerminalInputBatch, CommitsLiteralAndNamedKeyExactlyOnce)
{
	auto batch = MakeBatch();
	batch.actions.push_back({ TerminalInputActionKind::LiteralText, u"echo ready", TerminalNamedKey::Enter });
	batch.actions.push_back({ TerminalInputActionKind::NamedKey, {}, TerminalNamedKey::Enter });

	SakuraTerminalInputAdapter adapter;
	std::vector<std::uint8_t> committed;
	std::size_t commitCount{};
	TerminalInputBatchCommitter committer;
	const auto result = committer.EncodeAndCommit(batch, adapter, false,
		std::chrono::steady_clock::now(), [&](std::span<const std::uint8_t> bytes) {
			++commitCount;
			committed.assign(bytes.begin(), bytes.end());
			return TerminalQueueInputResult::Accepted;
		});

	EXPECT_EQ(TerminalInputResultCode::Accepted, result.code);
	EXPECT_EQ(1u, commitCount);
	EXPECT_EQ(std::vector<std::uint8_t>({ 'e', 'c', 'h', 'o', ' ', 'r', 'e', 'a', 'd', 'y', '\r' }), committed);
}

TEST(TerminalInputBatch, InvalidUtf16DoesNotPartiallyCommit)
{
	auto batch = MakeBatch();
	batch.actions.push_back({ TerminalInputActionKind::LiteralText, u"valid", TerminalNamedKey::Enter });
	batch.actions.push_back({ TerminalInputActionKind::LiteralText, std::u16string(1, u'\xd800'), TerminalNamedKey::Enter });

	SakuraTerminalInputAdapter adapter;
	std::size_t commitCount{};
	TerminalInputBatchCommitter committer;
	const auto result = committer.EncodeAndCommit(batch, adapter, false,
		std::chrono::steady_clock::now(), [&](std::span<const std::uint8_t>) {
			++commitCount;
			return TerminalQueueInputResult::Accepted;
		});

	EXPECT_EQ(TerminalInputResultCode::InvalidInput, result.code);
	EXPECT_EQ(0u, commitCount);
}

TEST(TerminalInputBatch, EncodesPasteMarkersBeforeTheSingleCommit)
{
	auto batch = MakeBatch();
	batch.actions.push_back({ TerminalInputActionKind::PasteText, u"two lines\n", TerminalNamedKey::Enter });

	SakuraTerminalInputAdapter adapter;
	std::string committed;
	TerminalInputBatchCommitter committer;
	const auto result = committer.EncodeAndCommit(batch, adapter, true,
		std::chrono::steady_clock::now(), [&](std::span<const std::uint8_t> bytes) {
			committed.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			return TerminalQueueInputResult::Accepted;
		});

	EXPECT_EQ(TerminalInputResultCode::Accepted, result.code);
	EXPECT_EQ("\x1b[200~two lines\n\x1b[201~", committed);
}

TEST(TerminalInputBatch, QueueFullIsTypedAndIsNotRetried)
{
	auto batch = MakeBatch();
	batch.actions.push_back({ TerminalInputActionKind::LiteralText, u"x", TerminalNamedKey::Enter });

	SakuraTerminalInputAdapter adapter;
	std::size_t commitCount{};
	TerminalInputBatchCommitter committer;
	const auto result = committer.EncodeAndCommit(batch, adapter, false,
		std::chrono::steady_clock::now(), [&](std::span<const std::uint8_t>) {
			++commitCount;
			return TerminalQueueInputResult::QueueFull;
		});

	EXPECT_EQ(TerminalInputResultCode::QueueFull, result.code);
	EXPECT_EQ(1u, commitCount);
}

TEST(TerminalInputBatch, OversizedEncodingDoesNotReachTheQueue)
{
	auto batch = MakeBatch();
	batch.actions.push_back({ TerminalInputActionKind::LiteralText, u"12345", TerminalNamedKey::Enter });

	SakuraTerminalInputAdapter adapter;
	std::size_t commitCount{};
	TerminalInputBatchLimits limits;
	limits.maximumEncodedBytes = 4;
	TerminalInputBatchCommitter committer(limits);
	const auto result = committer.EncodeAndCommit(batch, adapter, false,
		std::chrono::steady_clock::now(), [&](std::span<const std::uint8_t>) {
			++commitCount;
			return TerminalQueueInputResult::Accepted;
		});

	EXPECT_EQ(TerminalInputResultCode::InvalidInput, result.code);
	EXPECT_EQ(0u, commitCount);
}

} // namespace
} // namespace terminal
