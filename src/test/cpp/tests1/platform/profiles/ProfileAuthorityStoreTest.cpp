/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/profiles/ProfileAuthorityStore.h"

#include <limits>
#include <optional>

namespace platform::profiles {
namespace {

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";
constexpr wchar_t kUnicodeLegacyAlias[] = L"\x65e2\x5b9a \u043f\u0440\u043e\u0444\u0456\u043b\u044c";

std::string CanonicalPayload(std::string_view profileId, std::uint64_t generation)
{
	return "SakuraProfileAuthority/v1\nprofileId=" + std::string(profileId)
		+ "\ngeneration=" + std::to_string(generation) + "\n";
}

std::string PayloadChecksum(std::string_view payload)
{
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
	return checksum;
}

std::string Record(std::string_view profileId, std::uint64_t generation)
{
	const auto payload = CanonicalPayload(profileId, generation);
	return payload + "checksum=" + PayloadChecksum(payload) + "\n";
}

class FakeLock final : public IProfileAuthorityStoreLock {};

class FakeProfileAuthorityStoreBackend final : public IProfileAuthorityStoreBackend {
public:
	ProfileAuthorityStoreStatus ensureStatus = ProfileAuthorityStoreStatus::Succeeded;
	ProfileAuthorityStoreStatus lockStatus = ProfileAuthorityStoreStatus::Succeeded;
	ProfileAuthorityStoreStatus readStatus = ProfileAuthorityStoreStatus::Succeeded;
	ProfileAuthorityStoreStatus randomStatus = ProfileAuthorityStoreStatus::Succeeded;
	ProfileAuthorityStoreStatus writeStatus = ProfileAuthorityStoreStatus::Succeeded;
	ProfileAuthorityProfileId nextProfileId = kProfileId;
	std::optional<std::string> durableRecord;
	std::size_t writes = 0;

	ProfileAuthorityStoreStatus EnsureMetadataDirectory(const std::filesystem::path&) override
	{
		return ensureStatus;
	}

	ProfileAuthorityStoreStatus AcquireExclusiveLock(
		const std::filesystem::path&,
		std::unique_ptr<IProfileAuthorityStoreLock>& lock) override
	{
		if (lockStatus == ProfileAuthorityStoreStatus::Succeeded) lock = std::make_unique<FakeLock>();
		return lockStatus;
	}

	ProfileAuthorityStoreStatus ReadRecord(const std::filesystem::path&, std::string& bytes, bool& exists) override
	{
		if (readStatus != ProfileAuthorityStoreStatus::Succeeded) return readStatus;
		exists = durableRecord.has_value();
		bytes = durableRecord.value_or(std::string{});
		return ProfileAuthorityStoreStatus::Succeeded;
	}

	ProfileAuthorityStoreStatus GenerateOpaqueProfileId(ProfileAuthorityProfileId& profileId) override
	{
		if (randomStatus == ProfileAuthorityStoreStatus::Succeeded) profileId = nextProfileId;
		return randomStatus;
	}

	ProfileAuthorityStoreStatus WriteRecordAtomically(const std::filesystem::path&, std::string_view bytes) override
	{
		++writes;
		if (writeStatus == ProfileAuthorityStoreStatus::Succeeded) durableRecord = std::string(bytes);
		return writeStatus;
	}
};

} // namespace

TEST(ProfileAuthorityStore, CreatesOpaqueIdentityForDefaultAndUnicodeLegacyAlias)
{
	auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	ProfileAuthorityStore store(L"C:\\test-profile", backend);

	const auto result = store.Acquire(kUnicodeLegacyAlias);

	ASSERT_TRUE(result.Succeeded());
	EXPECT_EQ(kProfileId, result.profileId);
	EXPECT_EQ(1u, result.authorityGeneration);
	ASSERT_TRUE(backend->durableRecord.has_value());
	EXPECT_EQ(std::string::npos, backend->durableRecord->find("test-profile"));
	EXPECT_EQ(Record(kProfileId, 1), *backend->durableRecord);
}

TEST(ProfileAuthorityStore, SuccessiveStoreLifetimesRetainIdentityAndAdvanceGeneration)
{
	auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	ProfileAuthorityStore firstLifetime(L"C:\\legacy-profile", backend);
	const auto first = firstLifetime.Acquire(L"-PROF:BeforeRename");

	ProfileAuthorityStore secondLifetime(L"C:\\legacy-profile", backend);
	const auto second = secondLifetime.Acquire(L"-PROF:\x540d\x524d\x5909\x66f4\x5f8c");

	ASSERT_TRUE(first.Succeeded());
	ASSERT_TRUE(second.Succeeded());
	EXPECT_EQ(first.profileId, second.profileId);
	EXPECT_EQ(1u, first.authorityGeneration);
	EXPECT_EQ(2u, second.authorityGeneration);
	EXPECT_EQ(2u, backend->writes);
}

TEST(ProfileAuthorityStore, FailureInjectionDoesNotPublishAnUncommittedGeneration)
{
	auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	backend->durableRecord = Record(kProfileId, 7);
	backend->writeStatus = ProfileAuthorityStoreStatus::ReplaceFailed;
	const std::string before = *backend->durableRecord;
	ProfileAuthorityStore store(L"C:\\profile", backend);

	const auto result = store.Acquire();

	EXPECT_EQ(ProfileAuthorityStoreStatus::ReplaceFailed, result.status);
	EXPECT_FALSE(result.Succeeded());
	EXPECT_TRUE(result.profileId.empty());
	EXPECT_EQ(0u, result.authorityGeneration);
	EXPECT_EQ(before, *backend->durableRecord);
	EXPECT_EQ(1u, backend->writes);
}

TEST(ProfileAuthorityStore, PreCommitRejectionPreservesExistingAuthorityGeneration)
{
	auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	backend->durableRecord = Record(kProfileId, 7);
	const std::string before = *backend->durableRecord;
	ProfileAuthorityStore store(L"C:\\profile", backend);
	std::optional<ProfileAuthorityCandidate> candidate;

	const auto result = store.Acquire({}, [&candidate](const ProfileAuthorityCandidate& value) {
		candidate = value;
		return false;
	});

	EXPECT_EQ(ProfileAuthorityStoreStatus::PreCommitRejected, result.status);
	ASSERT_TRUE(candidate.has_value());
	EXPECT_EQ(kProfileId, candidate->profileId);
	EXPECT_EQ(8u, candidate->authorityGeneration);
	EXPECT_TRUE(candidate->existingIdentity);
	EXPECT_EQ(before, *backend->durableRecord);
	EXPECT_EQ(0u, backend->writes);
}

TEST(ProfileAuthorityStore, PreCommitRejectionDoesNotPublishANewIdentity)
{
	auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	ProfileAuthorityStore store(L"C:\\profile", backend);
	std::optional<ProfileAuthorityCandidate> candidate;

	const auto result = store.Acquire({}, [&candidate](const ProfileAuthorityCandidate& value) {
		candidate = value;
		return false;
	});

	EXPECT_EQ(ProfileAuthorityStoreStatus::PreCommitRejected, result.status);
	ASSERT_TRUE(candidate.has_value());
	EXPECT_EQ(kProfileId, candidate->profileId);
	EXPECT_EQ(1u, candidate->authorityGeneration);
	EXPECT_FALSE(candidate->existingIdentity);
	EXPECT_FALSE(backend->durableRecord.has_value());
	EXPECT_EQ(0u, backend->writes);
}

TEST(ProfileAuthorityStore, CreateLoadAndLockFailureInjectionAreExplicitTerminalResults)
{
	const struct Case {
		ProfileAuthorityStoreStatus FakeProfileAuthorityStoreBackend::* member;
		ProfileAuthorityStoreStatus status;
	} cases[] = {
		{ &FakeProfileAuthorityStoreBackend::ensureStatus, ProfileAuthorityStoreStatus::MetadataDirectoryFailed },
		{ &FakeProfileAuthorityStoreBackend::lockStatus, ProfileAuthorityStoreStatus::LockUnavailable },
		{ &FakeProfileAuthorityStoreBackend::readStatus, ProfileAuthorityStoreStatus::RecordReadFailed },
		{ &FakeProfileAuthorityStoreBackend::randomStatus, ProfileAuthorityStoreStatus::RandomFailed },
	};
	for (const auto& testCase : cases) {
		auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
		(backend.get()->*testCase.member) = testCase.status;
		ProfileAuthorityStore store(L"C:\\profile", backend);

		const auto result = store.Acquire();

		EXPECT_EQ(testCase.status, result.status);
		EXPECT_FALSE(result.Succeeded());
		EXPECT_TRUE(result.profileId.empty());
		EXPECT_EQ(0u, result.authorityGeneration);
	}
}

TEST(ProfileAuthorityStore, TornOldAndDuplicateRecordsAreFailClosedWithoutReset)
{
	const std::string valid = Record(kProfileId, 1);
	std::string validLookingBitFlip = valid;
	validLookingBitFlip[std::string_view("SakuraProfileAuthority/v1\nprofileId=").size()] = '1';
	const std::string records[] = {
		"SakuraProfileAuthority/v1\nprofileId=" + std::string(kProfileId) + "\ngeneration=1\n",
		valid.substr(0, valid.size() - 1),
		"SakuraProfileAuthority/v1\nprofileId=" + std::string(kProfileId) + "\nprofileId=" + kProfileId + "\ngeneration=1\nchecksum=0000000000000000\n",
		validLookingBitFlip,
		std::string("SakuraProfileAuthority/v1\nprofileId=") + kProfileId + "\ngeneration=1\nchecksum=0000000000000000\n\xff",
	};
	const ProfileAuthorityStoreStatus expected[] = {
		ProfileAuthorityStoreStatus::CorruptRecord,
		ProfileAuthorityStoreStatus::CorruptRecord,
		ProfileAuthorityStoreStatus::CorruptRecord,
		ProfileAuthorityStoreStatus::CorruptRecord,
		ProfileAuthorityStoreStatus::InvalidUtf8,
	};
	for (std::size_t index = 0; index < std::size(records); ++index) {
		auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
		backend->durableRecord = records[index];
		ProfileAuthorityStore store(L"C:\\profile", backend);

		const auto result = store.Acquire();

		EXPECT_EQ(expected[index], result.status);
		EXPECT_EQ(records[index], *backend->durableRecord);
		EXPECT_EQ(0u, backend->writes);
	}
}

TEST(ProfileAuthorityStore, RejectsUnsupportedSchemaAndGenerationOverflow)
{
	auto unsupportedBackend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	unsupportedBackend->durableRecord = "SakuraProfileAuthority/v2\nprofileId=" + std::string(kProfileId) + "\ngeneration=1\nchecksum=0000000000000000\n";
	ProfileAuthorityStore unsupportedStore(L"C:\\profile", unsupportedBackend);
	EXPECT_EQ(ProfileAuthorityStoreStatus::UnsupportedSchema, unsupportedStore.Acquire().status);

	auto overflowBackend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	overflowBackend->durableRecord = Record(kProfileId, (std::numeric_limits<std::uint64_t>::max)());
	ProfileAuthorityStore overflowStore(L"C:\\profile", overflowBackend);
	const auto result = overflowStore.Acquire();
	EXPECT_EQ(ProfileAuthorityStoreStatus::GenerationOverflow, result.status);
	EXPECT_EQ(0u, overflowBackend->writes);
}

TEST(ProfileAuthorityStore, RejectsInvalidAliasBeforeItCanReachPersistence)
{
	auto backend = std::make_shared<FakeProfileAuthorityStoreBackend>();
	ProfileAuthorityStore store(L"C:\\profile", backend);
	const std::wstring malformedAlias(1, static_cast<wchar_t>(0xd800));

	const auto result = store.Acquire(malformedAlias);

	EXPECT_EQ(ProfileAuthorityStoreStatus::InvalidArgument, result.status);
	EXPECT_EQ(0u, backend->writes);
}

} // namespace platform::profiles
