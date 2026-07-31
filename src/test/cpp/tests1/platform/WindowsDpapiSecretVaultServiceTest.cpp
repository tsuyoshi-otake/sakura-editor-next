/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/secrets/CWindowsDpapiSecretVaultService.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace platform::secrets {
namespace {

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kOtherProfileId = "fedcba9876543210fedcba9876543210";

class TemporaryDirectory final {
public:
	TemporaryDirectory()
	{
		const auto nonce = std::to_wstring(::GetCurrentProcessId()) + L"-"
			+ std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++s_sequence);
		m_path = std::filesystem::temp_directory_path() / (L"sakura-secret-vault-test-" + nonce);
	}
	~TemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}
	[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	inline static std::atomic_uint64_t s_sequence = 0;
	std::filesystem::path m_path;
};

SecretMutationRequest Set(std::string key, std::string value, std::string operationId,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .kind = ESecretMutationKind::Set, .extensionId = "publisher.extension",
		.key = std::move(key), .value = std::move(value), .operationId = std::move(operationId),
		.expectedRevision = expectedRevision };
}

SecretMutationRequest Delete(std::string key, std::string operationId,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .kind = ESecretMutationKind::Delete, .extensionId = "publisher.extension",
		.key = std::move(key), .operationId = std::move(operationId),
		.expectedRevision = expectedRevision };
}

LegacySecretVaultImportRequest Legacy(std::vector<LegacySecretVaultEntry> entries,
	std::string operationId, std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .extensionId = "publisher.extension", .entries = std::move(entries),
		.operationId = std::move(operationId), .expectedRevision = expectedRevision };
}

class FailingWriteOperations final : public IWindowsDpapiSecretVaultFileOperations {
public:
	explicit FailingWriteOperations(std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> inner)
		: m_inner(std::move(inner))
	{
	}

	bool PrepareDirectory(const std::filesystem::path& directory) override
	{
		return m_inner->PrepareDirectory(directory);
	}
	WindowsDpapiSecretVaultWriterLockResult AcquireWriterLock(
		const std::filesystem::path& lockPath) override
	{
		return m_inner->AcquireWriterLock(lockPath);
	}
	bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
		bool& found) override
	{
		return m_inner->ReadFile(path, bytes, found);
	}
	bool WriteFileAtomically(const std::filesystem::path& path,
		std::span<const std::uint8_t> bytes) override
	{
		return !failWrites && m_inner->WriteFileAtomically(path, bytes);
	}

	bool failWrites = false;

private:
	std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> m_inner;
};

TEST(WindowsDpapiSecretVaultService, RoundTripsReopensAndNeverLeavesPlaintextInCiphertextFile)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	ASSERT_EQ(ESecretMutationStatus::Succeeded,
		created.service->Apply(Set("access-token", "super-secret-value", "set-1", 0)).status);
	const auto statePath = created.service->StatePath();
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	created.service.reset();

	std::ifstream stream(statePath, std::ios::binary);
	const std::string ciphertext((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	EXPECT_NE(std::string::npos, ciphertext.find("SAKVLT01"));
	EXPECT_EQ(std::string::npos, ciphertext.find(std::string(kProfileId)));
	EXPECT_EQ(std::string::npos, ciphertext.find("publisher.extension"));
	EXPECT_EQ(std::string::npos, ciphertext.find("access-token"));
	EXPECT_EQ(std::string::npos, ciphertext.find("super-secret-value"));

	auto reopened = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(reopened.Succeeded());
	const auto found = reopened.service->Get("PUBLISHER.EXTENSION", "access-token");
	ASSERT_EQ(ESecretGetStatus::Found, found.status);
	ASSERT_TRUE(found.value);
	EXPECT_EQ("super-secret-value", *found.value);
}

TEST(WindowsDpapiSecretVaultService, WrongProfileAndTamperedStateFailClosed)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	ASSERT_EQ(ESecretMutationStatus::Succeeded,
		created.service->Apply(Set("token", "secret", "set-1", 0)).status);
	const auto statePath = created.service->StatePath();
	ASSERT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	created.service.reset();

	CWindowsDpapiSecretVaultService wrongProfile(temporary.Path(), std::string(kOtherProfileId));
	EXPECT_EQ(EWindowsDpapiSecretVaultOpenStatus::CryptoError, wrongProfile.Open().status);
	EXPECT_FALSE(wrongProfile.IsOpen());

	std::fstream stream(statePath, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(stream.good());
	stream.seekg(-1, std::ios::end);
	char byte = 0;
	stream.read(&byte, 1);
	stream.clear();
	stream.seekp(-1, std::ios::end);
	byte ^= 0x5a;
	stream.write(&byte, 1);
	stream.close();

	CWindowsDpapiSecretVaultService tampered(temporary.Path(), std::string(kProfileId));
	const auto open = tampered.Open();
	EXPECT_TRUE(open.status == EWindowsDpapiSecretVaultOpenStatus::CryptoError
		|| open.status == EWindowsDpapiSecretVaultOpenStatus::CorruptData);
	EXPECT_FALSE(tampered.IsOpen());
}

TEST(WindowsDpapiSecretVaultService, RetainsExclusiveWriterLockForTheControlOwnerLifetime)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	CWindowsDpapiSecretVaultService second(temporary.Path(), std::string(kProfileId));
	EXPECT_EQ(EWindowsDpapiSecretVaultOpenStatus::WriterBusy, second.Open().status);
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	EXPECT_EQ(EWindowsDpapiSecretVaultOpenStatus::Opened, second.Open().status);
}

TEST(WindowsDpapiSecretVaultService, PreservesCasExactReplayAndOperationIdCollisionAcrossReopen)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	const auto firstRequest = Set("token", "one", "operation-1", 0);
	EXPECT_EQ(ESecretMutationStatus::Succeeded, created.service->Apply(firstRequest).status);
	EXPECT_TRUE(created.service->Apply(firstRequest).replayed);
	EXPECT_EQ(ESecretMutationStatus::Conflict,
		created.service->Apply(Set("token", "two", "operation-2", 0)).status);
	EXPECT_EQ(ESecretMutationStatus::Invalid,
		created.service->Apply(Set("token", "two", "operation-1", 1)).status);
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	created.service.reset();

	auto reopened = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(reopened.Succeeded());
	EXPECT_TRUE(reopened.service->Apply(firstRequest).replayed);
	EXPECT_EQ("one", *reopened.service->Get("publisher.extension", "token").value);
}

TEST(WindowsDpapiSecretVaultService, IdempotentMutationDoesNotAdvanceRevisionOrNotify)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	std::vector<SecretChange> changes;
	auto subscription = created.service->Subscribe([&](const SecretChange& change) { changes.push_back(change); });
	ASSERT_TRUE(subscription);
	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		created.service->Apply(Set("token", "one", "set-1", 0)).status);
	const auto noChange = created.service->Apply(Set("token", "one", "set-2", 1));
	EXPECT_EQ(ESecretMutationStatus::NotApplicable, noChange.status);
	EXPECT_EQ(1u, noChange.revision);
	ASSERT_EQ(1u, changes.size());
	EXPECT_FALSE(changes[0].address.key.empty());
	EXPECT_EQ(std::string::npos, changes[0].address.key.find("one"));
}

TEST(WindowsDpapiSecretVaultService, FailedPersistLeavesPreviousCommittedStateAuthoritative)
{
	TemporaryDirectory temporary;
	auto operations = std::make_shared<FailingWriteOperations>(
		CreateWin32WindowsDpapiSecretVaultFileOperations());
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId),
		CWindowsDpapiSecretVaultService::kMaximumCompletedOperations,
		kMaximumSecretVaultSubscriptions, operations);
	ASSERT_TRUE(created.Succeeded());
	ASSERT_EQ(ESecretMutationStatus::Succeeded,
		created.service->Apply(Set("token", "one", "set-1", 0)).status);
	operations->failWrites = true;
	EXPECT_EQ(ESecretMutationStatus::Failed,
		created.service->Apply(Set("token", "two", "set-2", 1)).status);
	EXPECT_EQ("one", *created.service->Get("publisher.extension", "token").value);
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	created.service.reset();

	auto reopened = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(reopened.Succeeded());
	EXPECT_EQ("one", *reopened.service->Get("publisher.extension", "token").value);
}

TEST(WindowsDpapiSecretVaultService, StopIsTerminalAndWipesTheLiveAuthority)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	auto subscription = created.service->Subscribe([](const SecretChange&) {});
	ASSERT_TRUE(subscription);
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	EXPECT_EQ(ESecretVaultStopStatus::AlreadyStopped, created.service->Stop());
	EXPECT_FALSE(subscription->IsSubscribed());
	EXPECT_EQ(ESecretGetStatus::Stopped, created.service->Get("publisher.extension", "token").status);
	EXPECT_EQ(ESecretMutationStatus::Stopped,
		created.service->Apply(Set("token", "one", "set-1")).status);
	EXPECT_FALSE(created.service->Subscribe([](const SecretChange&) {}));
}

TEST(WindowsDpapiSecretVaultService, BoundsTheMigrationAndReplayLedgers)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId), 1);
	ASSERT_TRUE(created.Succeeded());
	EXPECT_EQ(ELegacySecretVaultImportStatus::Invalid,
		created.service->ImportLegacy(Legacy({ { "too-large", std::string(kMaximumSecretVaultValueBytes + 1, 'x') } },
			"legacy-invalid")).status);
	EXPECT_EQ(ELegacySecretVaultImportStatus::Succeeded,
		created.service->ImportLegacy(Legacy({}, "legacy-old", 0)).status);
	EXPECT_EQ(ELegacySecretVaultImportStatus::Succeeded,
		created.service->ImportLegacy({ .extensionId = "other.extension", .entries = {},
			.operationId = "legacy-new", .expectedRevision = 1 }).status);
	// Only the latest import operation remains replayable; the first extension remains completed.
	EXPECT_EQ(ELegacySecretVaultImportStatus::AlreadyImported,
		created.service->ImportLegacy(Legacy({}, "legacy-old-again", 2)).status);
	EXPECT_TRUE(created.service->IsLegacyMigrationComplete("publisher.extension"));
}

TEST(WindowsDpapiSecretVaultService, ImportsLegacyEntriesAndEmptyMarkerAtomicallyAndReopens)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	EXPECT_EQ(ELegacySecretVaultImportStatus::Succeeded,
		created.service->ImportLegacy({ .extensionId = "empty.extension", .entries = {},
			.operationId = "empty-import", .expectedRevision = 0 }).status);
	EXPECT_TRUE(created.service->IsLegacyMigrationComplete("EMPTY.EXTENSION"));
	EXPECT_EQ(ESecretGetStatus::NotFound,
		created.service->Get("empty.extension", "absent-key").status);
	const auto import = Legacy({ { "alpha", "one" }, { "beta", "two" } }, "import-1", 1);
	EXPECT_EQ(ELegacySecretVaultImportStatus::Succeeded, created.service->ImportLegacy(import).status);
	EXPECT_TRUE(created.service->ImportLegacy(import).replayed);
	EXPECT_TRUE(created.service->IsLegacyMigrationComplete("publisher.extension"));
	EXPECT_EQ("one", *created.service->Get("publisher.extension", "alpha").value);
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, created.service->Stop());
	created.service.reset();

	auto reopened = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(reopened.Succeeded());
	EXPECT_TRUE(reopened.service->IsLegacyMigrationComplete("publisher.extension"));
	EXPECT_TRUE(reopened.service->ImportLegacy(import).replayed);
	EXPECT_EQ("two", *reopened.service->Get("publisher.extension", "beta").value);
}

TEST(WindowsDpapiSecretVaultService, EmptyImportPreventsDeleteThenLegacyReimportResurrection)
{
	TemporaryDirectory temporary;
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	EXPECT_EQ(ELegacySecretVaultImportStatus::Succeeded,
		created.service->ImportLegacy(Legacy({ { "token", "old-value" } }, "import-1", 0)).status);
	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		created.service->Apply(Delete("token", "delete-1", 1)).status);
	const auto noResurrection = created.service->ImportLegacy(
		Legacy({ { "token", "old-value" } }, "import-2", 2));
	EXPECT_EQ(ELegacySecretVaultImportStatus::AlreadyImported, noResurrection.status);
	EXPECT_EQ(ESecretGetStatus::NotFound,
		created.service->Get("publisher.extension", "token").status);
}

TEST(WindowsDpapiSecretVaultService, FailedLegacyImportCommitDoesNotMarkOrPartiallyImport)
{
	TemporaryDirectory temporary;
	auto operations = std::make_shared<FailingWriteOperations>(
		CreateWin32WindowsDpapiSecretVaultFileOperations());
	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path(), std::string(kProfileId),
		CWindowsDpapiSecretVaultService::kMaximumCompletedOperations,
		kMaximumSecretVaultSubscriptions, operations);
	ASSERT_TRUE(created.Succeeded());
	operations->failWrites = true;
	EXPECT_EQ(ELegacySecretVaultImportStatus::Failed,
		created.service->ImportLegacy(Legacy({ { "token", "old-value" } }, "import-1", 0)).status);
	EXPECT_FALSE(created.service->IsLegacyMigrationComplete("publisher.extension"));
	EXPECT_EQ(ESecretGetStatus::NotFound,
		created.service->Get("publisher.extension", "token").status);
}

} // namespace
} // namespace platform::secrets
