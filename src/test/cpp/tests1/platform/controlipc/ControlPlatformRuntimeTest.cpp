/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlPlatformRuntime.h"

#include <optional>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

std::string CanonicalPayload(std::string_view profileId, std::uint64_t generation)
{
	return "SakuraProfileAuthority/v1\nprofileId=" + std::string(profileId)
		+ "\ngeneration=" + std::to_string(generation) + "\n";
}

std::string AuthorityRecord(std::string_view profileId, std::uint64_t generation)
{
	const auto payload = CanonicalPayload(profileId, generation);
	std::uint64_t hash = 14695981039346656037ULL;
	for (const unsigned char byte : payload) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	static constexpr char kHex[] = "0123456789abcdef";
	std::string checksum(16, '0');
	for (std::size_t index = 0; index < checksum.size(); ++index) {
		checksum[index] = kHex[(hash >> ((checksum.size() - 1 - index) * 4)) & 0x0f];
	}
	return payload + "checksum=" + checksum + "\n";
}

class FakeAuthorityLock final : public profiles::IProfileAuthorityStoreLock {};

class FakeAuthorityBackend final : public profiles::IProfileAuthorityStoreBackend {
public:
	std::optional<std::string> record;
	std::size_t writes = 0;

	profiles::ProfileAuthorityStoreStatus EnsureMetadataDirectory(const std::filesystem::path&) override
	{
		return profiles::ProfileAuthorityStoreStatus::Succeeded;
	}

	profiles::ProfileAuthorityStoreStatus AcquireExclusiveLock(const std::filesystem::path&,
		std::unique_ptr<profiles::IProfileAuthorityStoreLock>& lock) override
	{
		lock = std::make_unique<FakeAuthorityLock>();
		return profiles::ProfileAuthorityStoreStatus::Succeeded;
	}

	profiles::ProfileAuthorityStoreStatus ReadRecord(const std::filesystem::path&, std::string& bytes,
		bool& exists) override
	{
		exists = record.has_value();
		bytes = record.value_or(std::string{});
		return profiles::ProfileAuthorityStoreStatus::Succeeded;
	}

	profiles::ProfileAuthorityStoreStatus GenerateOpaqueProfileId(
		profiles::ProfileAuthorityProfileId& profileId) override
	{
		profileId = kProfileId;
		return profiles::ProfileAuthorityStoreStatus::Succeeded;
	}

	profiles::ProfileAuthorityStoreStatus WriteRecordAtomically(const std::filesystem::path&,
		std::string_view bytes) override
	{
		++writes;
		record = std::string(bytes);
		return profiles::ProfileAuthorityStoreStatus::Succeeded;
	}
};

class FakeStorageAuthority final : public storage::IStorageAuthority {
public:
	explicit FakeStorageAuthority(storage::StorageAuthorityOpenResult result) : m_result(std::move(result)) {}

	storage::StorageAuthorityOpenResult Open() override
	{
		++openCalls;
		m_open = m_result.Succeeded();
		return m_result;
	}

	void Close() noexcept override
	{
		++closeCalls;
		m_open = false;
	}

	bool IsOpen() const noexcept override { return m_open; }

	storage::StorageMutationResult Apply(const storage::StorageMutationRequest&) override
	{
		return { storage::EStorageMutationStatus::Failed, 0, false, "not used by this test" };
	}

	storage::StorageSnapshot Snapshot() const override { return {}; }

	std::unique_ptr<storage::IStorageChangeSubscription> Subscribe(storage::StorageChangeCallback) override
	{
		return nullptr;
	}

	int openCalls = 0;
	int closeCalls = 0;

private:
	storage::StorageAuthorityOpenResult m_result;
	bool m_open = false;
};

ControlPlatformRuntimeOptions Options()
{
	ControlPlatformRuntimeOptions options;
	options.profileDirectory = L"C:\\profile";
	options.storageDirectory = L"C:\\profile\\.sakura-platform";
	return options;
}

TEST(ControlPlatformRuntime, GenerationRollbackRejectsAuthorityCandidateBeforeCommit)
{
	auto backend = std::make_shared<FakeAuthorityBackend>();
	backend->record = AuthorityRecord(kProfileId, 6);
	auto storageAuthority = std::make_shared<FakeStorageAuthority>(storage::StorageAuthorityOpenResult{
		storage::EStorageAuthorityOpenStatus::GenerationRollback,
		"profile authority generation is behind durable storage", true });
	std::uint64_t candidateGeneration = 0;
	ControlPlatformRuntimeDependencies dependencies;
	dependencies.profileAuthorityBackend = backend;
	dependencies.storageFactory = [storageAuthority, &candidateGeneration](const std::filesystem::path&,
		std::uint64_t generation, std::size_t) {
		candidateGeneration = generation;
		return storageAuthority;
	};
	CControlPlatformRuntime runtime(Options(), std::move(dependencies));

	const auto result = runtime.Start();

	EXPECT_EQ(EControlPlatformRuntimeResultCode::StorageOpenFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.authorityResult.has_value());
	EXPECT_EQ(profiles::ProfileAuthorityStoreStatus::PreCommitRejected, result.authorityResult->status);
	ASSERT_TRUE(result.storageOpenResult.has_value());
	EXPECT_EQ(storage::EStorageAuthorityOpenStatus::GenerationRollback, result.storageOpenResult->status);
	EXPECT_EQ(7u, candidateGeneration);
	EXPECT_EQ(0u, backend->writes);
	EXPECT_EQ(1, storageAuthority->openCalls);
	EXPECT_EQ(1, storageAuthority->closeCalls);
}

TEST(ControlPlatformRuntime, SurvivingStorageCannotBeBoundToANewProfileIdentity)
{
	auto backend = std::make_shared<FakeAuthorityBackend>();
	auto storageAuthority = std::make_shared<FakeStorageAuthority>(storage::StorageAuthorityOpenResult{
		storage::EStorageAuthorityOpenStatus::Opened, {}, true });
	std::uint64_t candidateGeneration = 0;
	ControlPlatformRuntimeDependencies dependencies;
	dependencies.profileAuthorityBackend = backend;
	dependencies.storageFactory = [storageAuthority, &candidateGeneration](const std::filesystem::path&,
		std::uint64_t generation, std::size_t) {
		candidateGeneration = generation;
		return storageAuthority;
	};
	CControlPlatformRuntime runtime(Options(), std::move(dependencies));

	const auto result = runtime.Start();

	EXPECT_EQ(EControlPlatformRuntimeResultCode::StorageOpenFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.storageOpenResult.has_value());
	EXPECT_EQ(storage::EStorageAuthorityOpenStatus::OrphanedState, result.storageOpenResult->status);
	EXPECT_TRUE(result.storageOpenResult->persistedStateFound);
	EXPECT_EQ(1u, candidateGeneration);
	EXPECT_EQ(0u, backend->writes);
	EXPECT_FALSE(backend->record.has_value());
	EXPECT_EQ(1, storageAuthority->closeCalls);
}

} // namespace
} // namespace platform::controlipc
