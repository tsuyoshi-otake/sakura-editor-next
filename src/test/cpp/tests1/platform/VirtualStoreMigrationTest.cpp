/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <map>
#include <utility>
#include <vector>

#include "platform/VirtualStoreMigration.h"

namespace {

class FakeFileSystem final : public platform::IVirtualStoreMigrationFileSystem {
public:
	std::map<std::wstring, bool> existing;
	bool copySucceeds = true;
	bool recordSucceeds = true;
	unsigned int copyCalls = 0;
	unsigned int recordCalls = 0;
	std::map<std::wstring, bool> copyOutcomeByDestination;
	std::vector<std::pair<std::wstring, std::wstring>> copies;
	std::wstring recordText;

	bool Exists(const std::wstring& path) override { return existing[path]; }
	bool CopyFileWithoutOverwrite(const std::wstring& sourcePath, const std::wstring& destinationPath) override
	{
		++copyCalls;
		copies.emplace_back(sourcePath, destinationPath);
		const auto configured = copyOutcomeByDestination.find(destinationPath);
		if (!copySucceeds || (configured != copyOutcomeByDestination.end() && !configured->second) || existing[destinationPath]) {
			return false;
		}
		existing[destinationPath] = true;
		return true;
	}
	bool WriteMigrationRecord(const std::wstring& recordPath, const std::wstring& text) override
	{
		++recordCalls;
		recordText = text;
		if (!recordSucceeds) {
			return false;
		}
		existing[recordPath] = true;
		return true;
	}
};

class FakePathProvider final : public platform::IVirtualStoreMigrationPathProvider {
public:
	std::wstring GetLocalAppDataPath() override { return L"C:\\Users\\Sakura\\AppData\\Local"; }
	std::wstring GetCurrentUserSakuraIniPath() override { return L"C:\\Users\\Sakura\\AppData\\Roaming\\Sakura\\sakura.ini"; }
	std::wstring GetMigrationRecordPath() override { return L"C:\\Users\\Sakura\\AppData\\Roaming\\Sakura\\VirtualStoreMigration.record"; }
};

platform::VirtualStoreMigrationRequest MakeRequest()
{
	return { L"legacy.ini", L"settings.ini", L"settings.ini.virtualstore.bak", L"migration.record" };
}

} // namespace

TEST(VirtualStoreMigration, BuildsVirtualStorePathForLegacyExecutable)
{
	EXPECT_EQ(
		L"C:\\Users\\Sakura\\AppData\\Local\\VirtualStore\\Program Files\\Sakura Editor\\sakura.ini",
		platform::BuildLegacyVirtualStoreIniPath(
			L"C:\\Users\\Sakura\\AppData\\Local",
			L"C:/Program Files/Sakura Editor/sakura.exe"
		)
	);
	EXPECT_TRUE(platform::BuildLegacyVirtualStoreIniPath(L"C:\\Users\\Sakura", L"\\\\server\\share\\sakura.exe").empty());
}

TEST(VirtualStoreMigration, BuildsRequestThroughInjectablePathProvider)
{
	FakePathProvider provider;
	const auto request = platform::BuildVirtualStoreMigrationRequest(
		L"C:\\Program Files\\Sakura Editor\\sakura.exe", provider);

	EXPECT_EQ(L"C:\\Users\\Sakura\\AppData\\Local\\VirtualStore\\Program Files\\Sakura Editor\\sakura.ini", request.legacyIniPath);
	EXPECT_EQ(L"C:\\Users\\Sakura\\AppData\\Roaming\\Sakura\\sakura.ini", request.destinationIniPath);
	EXPECT_EQ(L"C:\\Users\\Sakura\\AppData\\Roaming\\Sakura\\sakura.ini.virtualstore.bak", request.backupIniPath);
	EXPECT_EQ(L"C:\\Users\\Sakura\\AppData\\Roaming\\Sakura\\VirtualStoreMigration.record", request.migrationRecordPath);
}

TEST(VirtualStoreMigration, ReportsNoLegacyWithoutWriting)
{
	FakeFileSystem fileSystem;
	EXPECT_EQ(platform::VirtualStoreMigrationResult::NoLegacy, platform::MigrateVirtualStoreIni(MakeRequest(), fileSystem));
	EXPECT_EQ(0u, fileSystem.copyCalls);
	EXPECT_EQ(0u, fileSystem.recordCalls);
}

TEST(VirtualStoreMigration, ReportsFailedWhenDestinationIsUnavailable)
{
	FakeFileSystem fileSystem;
	auto request = MakeRequest();
	request.destinationIniPath.clear();
	request.backupIniPath.clear();

	EXPECT_EQ(platform::VirtualStoreMigrationResult::Failed, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_EQ(0u, fileSystem.copyCalls);
	EXPECT_EQ(0u, fileSystem.recordCalls);
}

TEST(VirtualStoreMigration, KeepsExistingDestinationUntouched)
{
	FakeFileSystem fileSystem;
	const auto request = MakeRequest();
	fileSystem.existing[request.legacyIniPath] = true;
	fileSystem.existing[request.destinationIniPath] = true;

	EXPECT_EQ(platform::VirtualStoreMigrationResult::DestinationExists, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_EQ(0u, fileSystem.copyCalls);
	EXPECT_EQ(0u, fileSystem.recordCalls);
}

TEST(VirtualStoreMigration, MigrationRecordMakesOperationIdempotent)
{
	FakeFileSystem fileSystem;
	const auto request = MakeRequest();
	fileSystem.existing[request.legacyIniPath] = true;
	fileSystem.existing[request.migrationRecordPath] = true;

	EXPECT_EQ(platform::VirtualStoreMigrationResult::AlreadyMigrated, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_EQ(0u, fileSystem.copyCalls);
	EXPECT_EQ(0u, fileSystem.recordCalls);
}

TEST(VirtualStoreMigration, CopiesWithoutDeletingLegacyAndWritesRecord)
{
	FakeFileSystem fileSystem;
	const auto request = MakeRequest();
	fileSystem.existing[request.legacyIniPath] = true;

	EXPECT_EQ(platform::VirtualStoreMigrationResult::Copied, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_TRUE(fileSystem.existing[request.legacyIniPath]);
	EXPECT_TRUE(fileSystem.existing[request.destinationIniPath]);
	EXPECT_TRUE(fileSystem.existing[request.backupIniPath]);
	EXPECT_TRUE(fileSystem.existing[request.migrationRecordPath]);
	ASSERT_EQ(2u, fileSystem.copies.size());
	EXPECT_EQ(std::make_pair(request.legacyIniPath, request.backupIniPath), fileSystem.copies[0]);
	EXPECT_EQ(std::make_pair(request.legacyIniPath, request.destinationIniPath), fileSystem.copies[1]);
	EXPECT_EQ(request.legacyIniPath, fileSystem.recordText);

	EXPECT_EQ(platform::VirtualStoreMigrationResult::AlreadyMigrated, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_EQ(2u, fileSystem.copyCalls);
}

TEST(VirtualStoreMigration, ReportsFailedBackupWithoutCreatingDestination)
{
	FakeFileSystem fileSystem;
	const auto request = MakeRequest();
	fileSystem.existing[request.legacyIniPath] = true;
	fileSystem.copyOutcomeByDestination[request.backupIniPath] = false;

	EXPECT_EQ(platform::VirtualStoreMigrationResult::Failed, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_FALSE(fileSystem.existing[request.destinationIniPath]);
	EXPECT_FALSE(fileSystem.existing[request.backupIniPath]);
	EXPECT_EQ(1u, fileSystem.copyCalls);
	EXPECT_EQ(0u, fileSystem.recordCalls);
}

TEST(VirtualStoreMigration, KeepsBackupWhenDestinationCopyFails)
{
	FakeFileSystem fileSystem;
	const auto request = MakeRequest();
	fileSystem.existing[request.legacyIniPath] = true;
	fileSystem.copyOutcomeByDestination[request.destinationIniPath] = false;

	EXPECT_EQ(platform::VirtualStoreMigrationResult::Failed, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_TRUE(fileSystem.existing[request.legacyIniPath]);
	EXPECT_TRUE(fileSystem.existing[request.backupIniPath]);
	EXPECT_FALSE(fileSystem.existing[request.destinationIniPath]);
	EXPECT_EQ(2u, fileSystem.copyCalls);
	EXPECT_EQ(0u, fileSystem.recordCalls);
}

TEST(VirtualStoreMigration, ReportsFailedRecordAfterNonOverwritingCopy)
{
	FakeFileSystem fileSystem;
	const auto request = MakeRequest();
	fileSystem.existing[request.legacyIniPath] = true;
	fileSystem.recordSucceeds = false;

	EXPECT_EQ(platform::VirtualStoreMigrationResult::Failed, platform::MigrateVirtualStoreIni(request, fileSystem));
	EXPECT_TRUE(fileSystem.existing[request.legacyIniPath]);
	EXPECT_TRUE(fileSystem.existing[request.destinationIniPath]);
	EXPECT_TRUE(fileSystem.existing[request.backupIniPath]);
	EXPECT_FALSE(fileSystem.existing[request.migrationRecordPath]);
	EXPECT_EQ(2u, fileSystem.copyCalls);
	EXPECT_EQ(1u, fileSystem.recordCalls);
}
