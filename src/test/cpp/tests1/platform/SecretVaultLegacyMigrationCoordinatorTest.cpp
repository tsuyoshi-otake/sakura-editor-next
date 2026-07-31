/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionSecretStorage.h"
#include "platform/secrets/CSecretVaultLegacyMigrationCoordinator.h"

#include <atomic>
#include <filesystem>

namespace platform::secrets {
namespace {

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";

class TemporaryDirectory final {
public:
	TemporaryDirectory()
	{
		m_path = std::filesystem::temp_directory_path() / (L"sakura-secret-migration-test-"
			+ std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++s_sequence));
	}
	~TemporaryDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}
	[[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	inline static std::atomic_uint64_t s_sequence = 0;
	std::filesystem::path m_path;
};

TEST(SecretVaultLegacyMigrationCoordinator, ImportsOnceAndPersistsAbsentAndEmptyCompletionMarkers)
{
	TemporaryDirectory temporary;
	const auto legacyRoot = temporary.Path() / L"legacy";
	ASSERT_TRUE(std::filesystem::create_directories(legacyRoot));
	CExtensionSecretStorage legacy(legacyRoot);
	ASSERT_TRUE(legacy.Store(L"publisher.one", L"token", L"legacy-value").success);
	ASSERT_TRUE(legacy.Store(L"empty.extension", L"temporary", L"value").success);
	ASSERT_TRUE(legacy.Delete(L"empty.extension", L"temporary").success);

	auto created = CWindowsDpapiSecretVaultService::Create(temporary.Path() / L"vault", std::string(kProfileId));
	ASSERT_TRUE(created.Succeeded());
	CSecretVaultLegacyMigrationCoordinator coordinator(*created.service, legacyRoot);

	EXPECT_EQ(ESecretVaultLegacyMigrationStatus::Migrated, coordinator.EnsureMigrated("publisher.one").status);
	EXPECT_EQ("legacy-value", *created.service->Get("publisher.one", "token").value);
	ASSERT_TRUE(legacy.Store(L"publisher.one", L"later", L"must-not-import").success);
	EXPECT_EQ(ESecretVaultLegacyMigrationStatus::Migrated, coordinator.EnsureMigrated("publisher.one").status);
	EXPECT_EQ(ESecretGetStatus::NotFound, created.service->Get("publisher.one", "later").status);

	EXPECT_EQ(ESecretVaultLegacyMigrationStatus::Migrated, coordinator.EnsureMigrated("empty.extension").status);
	EXPECT_TRUE(created.service->IsLegacyMigrationComplete("empty.extension"));
	EXPECT_EQ(ESecretVaultLegacyMigrationStatus::Migrated, coordinator.EnsureMigrated("absent.extension").status);
	EXPECT_TRUE(created.service->IsLegacyMigrationComplete("absent.extension"));
	EXPECT_EQ(ESecretVaultLegacyMigrationStopStatus::Stopped, coordinator.Stop());
	EXPECT_EQ(ESecretVaultLegacyMigrationStatus::Stopped, coordinator.EnsureMigrated("publisher.one").status);
}

} // namespace
} // namespace platform::secrets
