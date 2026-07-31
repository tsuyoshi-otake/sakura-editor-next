/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/storage/CAtomicFileStorageService.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace platform::storage {
namespace {

StorageAddress ProfileAddress(std::string key, std::string owner = "workbench.layout")
{
	return { .scope = EStorageScope::Profile, .scopeId = "profile-a", .owner = std::move(owner), .key = std::move(key) };
}

StorageMutationRequest Put(std::string operationId, StorageAddress address, std::string value,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .operationId = std::move(operationId), .expectedRevision = expectedRevision,
		.mutations = { StorageMutation{ .address = std::move(address), .target = EStorageTarget::Machine, .value = std::move(value) } } };
}

struct FakeFileState final {
	bool writerLocked = false;
	bool failWrite = false;
	std::size_t writeCount = 0;
	std::map<std::wstring, std::vector<std::uint8_t>> files;
};

class FakeWriterLock final : public IAtomicFileStorageWriterLock {
public:
	explicit FakeWriterLock(std::shared_ptr<FakeFileState> state) : m_state(std::move(state)) {}
	~FakeWriterLock() override { m_state->writerLocked = false; }
private:
	std::shared_ptr<FakeFileState> m_state;
};

class FakeFileOperations final : public IAtomicFileStorageFileOperations {
public:
	FakeFileOperations() : state(std::make_shared<FakeFileState>()) {}
	bool PrepareDirectory(const std::filesystem::path&, std::string&) override { return true; }
	AtomicFileStorageWriterLockResult AcquireWriterLock(const std::filesystem::path&) override
	{
		if (state->writerLocked) return { EAtomicFileStorageWriterLockStatus::Busy, nullptr, "writer is already locked" };
		state->writerLocked = true;
		return { EAtomicFileStorageWriterLockStatus::Acquired, std::make_unique<FakeWriterLock>(state), {} };
	}
	bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, bool& found, std::string&) override
	{
		const auto file = state->files.find(path.wstring());
		if (file == state->files.end()) { found = false; bytes.clear(); return true; }
		found = true;
		bytes = file->second;
		return true;
	}
	bool WriteFileAtomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string& diagnostic) override
	{
		if (state->failWrite) { diagnostic = "injected write/flush/replace failure"; return false; }
		++state->writeCount;
		state->files[path.wstring()] = { bytes.begin(), bytes.end() };
		return true;
	}

	std::shared_ptr<FakeFileState> state;
};

TEST(AtomicFileStorageService, FaultBeforeAtomicReplacePreservesLastCommittedState)
{
	auto files = std::make_shared<FakeFileOperations>();
	CAtomicFileStorageService storage(L"durable-test", 11, 64, files);
	ASSERT_EQ(EAtomicFileStorageOpenStatus::Opened, storage.Open().status);
	const auto address = ProfileAddress("saved-value");
	ASSERT_EQ(EStorageMutationStatus::Succeeded, storage.Apply(Put("first", address, "committed", 0)).status);

	files->state->failWrite = true;
	const auto failed = storage.Apply(Put("second", address, "not-published", 1));
	EXPECT_EQ(EStorageMutationStatus::Failed, failed.status);
	EXPECT_EQ(1u, storage.Snapshot().revision);
	const auto beforeReopen = storage.Find(address);
	ASSERT_TRUE(beforeReopen);
	EXPECT_EQ("committed", beforeReopen->value);

	files->state->failWrite = false;
	storage.Close();
	CAtomicFileStorageService reopened(L"durable-test", 12, 64, files);
	ASSERT_EQ(EAtomicFileStorageOpenStatus::Opened, reopened.Open().status);
	const auto afterReopen = reopened.Find(address);
	ASSERT_TRUE(afterReopen);
	EXPECT_EQ("committed", afterReopen->value);
}

TEST(AtomicFileStorageService, CompletedOperationReplaysAfterReopenAndRejectsMismatchedReuse)
{
	auto files = std::make_shared<FakeFileOperations>();
	const auto address = ProfileAddress("replay");
	const auto request = Put("operation-1", address, "true", 0);
	{
		CAtomicFileStorageService storage(L"replay-test", 3, 64, files);
		ASSERT_TRUE(storage.Open().Succeeded());
		EXPECT_EQ(EStorageMutationStatus::Succeeded, storage.Apply(request).status);
	}

	CAtomicFileStorageService reopened(L"replay-test", 4, 64, files);
	ASSERT_TRUE(reopened.Open().Succeeded());
	const auto replay = reopened.Apply(request);
	EXPECT_EQ(EStorageMutationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(1u, replay.revision);
	ASSERT_TRUE(replay.changeBatch);
	EXPECT_EQ(4u, replay.changeBatch->generation);
	EXPECT_EQ(EStorageMutationStatus::Failed, reopened.Apply(Put("operation-1", address, "false", 1)).status);
}

TEST(AtomicFileStorageService, InvalidRequestIsNotPersistedButEmptyNoOpIsReplayable)
{
	auto files = std::make_shared<FakeFileOperations>();
	CAtomicFileStorageService storage(L"validation-replay-test", 8, 64, files);
	ASSERT_TRUE(storage.Open().Succeeded());
	const auto address = ProfileAddress("duplicate");
	StorageMutationRequest invalid{ .operationId = "reusable-operation", .mutations = {
		{ .address = address, .target = EStorageTarget::Machine, .value = "one" },
		{ .address = address, .target = EStorageTarget::Machine, .value = "two" },
	} };
	EXPECT_EQ(EStorageMutationStatus::Failed, storage.Apply(invalid).status);
	EXPECT_EQ(0u, files->state->writeCount);
	EXPECT_EQ(EStorageMutationStatus::Succeeded,
		storage.Apply(Put("reusable-operation", address, "valid", 0)).status);

	StorageMutationRequest empty{ .operationId = "empty-operation" };
	const auto noOp = storage.Apply(empty);
	EXPECT_EQ(EStorageMutationStatus::NotApplicable, noOp.status);
	storage.Close();

	CAtomicFileStorageService reopened(L"validation-replay-test", 9, 64, files);
	ASSERT_TRUE(reopened.Open().Succeeded());
	const auto replay = reopened.Apply(empty);
	EXPECT_EQ(EStorageMutationStatus::NotApplicable, replay.status);
	EXPECT_TRUE(replay.replayed);
}

TEST(AtomicFileStorageService, RejectsPersistedGenerationAheadOfCurrentAuthority)
{
	auto files = std::make_shared<FakeFileOperations>();
	const auto address = ProfileAddress("generation");
	{
		CAtomicFileStorageService storage(L"generation-test", 12, 64, files);
		ASSERT_TRUE(storage.Open().Succeeded());
		ASSERT_EQ(EStorageMutationStatus::Succeeded,
			storage.Apply(Put("generation-operation", address, "value", 0)).status);
	}

	CAtomicFileStorageService rolledBackAuthority(L"generation-test", 11, 64, files);
	EXPECT_EQ(EAtomicFileStorageOpenStatus::CorruptData, rolledBackAuthority.Open().status);
	EXPECT_FALSE(rolledBackAuthority.IsOpen());
}

TEST(AtomicFileStorageService, CorruptPersistedStateHasExplicitOpenOutcome)
{
	auto files = std::make_shared<FakeFileOperations>();
	CAtomicFileStorageService storage(L"corrupt-test", 1, 64, files);
	files->state->files[storage.StatePath().wstring()] = { 'n', 'o', 't', '-', 'a', '-', 's', 't', 'o', 'r', 'e' };

	const auto opened = storage.Open();
	EXPECT_EQ(EAtomicFileStorageOpenStatus::CorruptData, opened.status);
	EXPECT_FALSE(storage.IsOpen());
}

TEST(AtomicFileStorageService, EnforcesIpcLimitsAndRejectsSecretNamespace)
{
	auto files = std::make_shared<FakeFileOperations>();
	CAtomicFileStorageService storage(L"limit-test", 1, 64, files);
	ASSERT_TRUE(storage.Open().Succeeded());

	StorageMutationRequest oversized{ .operationId = "too-many", .mutations = {} };
	for (std::size_t index = 0; index <= CAtomicFileStorageService::kMaximumItems; ++index) {
		oversized.mutations.push_back({ .address = ProfileAddress("key-" + std::to_string(index)), .target = EStorageTarget::Machine, .value = "x" });
	}
	EXPECT_EQ(EStorageMutationStatus::Failed, storage.Apply(oversized).status);
	EXPECT_EQ(EStorageMutationStatus::Failed,
		storage.Apply(Put("too-large-value", ProfileAddress("large"), std::string(CAtomicFileStorageService::kMaximumStringBytes + 1, 'x'))).status);
	EXPECT_EQ(EStorageMutationStatus::Failed,
		storage.Apply(Put("secret-value", ProfileAddress("access-token", "secrets.extension"), "must-use-secret-storage")).status);
	EXPECT_TRUE(storage.Snapshot().entries.empty());
}

TEST(AtomicFileStorageService, HoldsAnExclusiveWriterLockForOpenLifetime)
{
	auto files = std::make_shared<FakeFileOperations>();
	CAtomicFileStorageService first(L"lock-test", 1, 64, files);
	CAtomicFileStorageService second(L"lock-test", 2, 64, files);
	EXPECT_EQ(EAtomicFileStorageOpenStatus::Opened, first.Open().status);
	EXPECT_EQ(EAtomicFileStorageOpenStatus::WriterBusy, second.Open().status);
	first.Close();
	EXPECT_EQ(EAtomicFileStorageOpenStatus::Opened, second.Open().status);
}

} // namespace
} // namespace platform::storage
