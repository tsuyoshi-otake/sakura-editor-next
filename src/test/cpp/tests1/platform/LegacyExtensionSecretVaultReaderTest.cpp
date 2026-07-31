/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionSecretStorage.h"
#include "platform/secrets/LegacyExtensionSecretVaultReader.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <bcrypt.h>
#include <windows.h>

namespace platform::secrets {
namespace {

class CLegacyExtensionSecretVaultReaderTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		std::array<std::uint8_t, 8> random{};
		ASSERT_GE(::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG), 0);
		static constexpr wchar_t digits[] = L"0123456789abcdef";
		std::wstring suffix;
		for (const auto byte : random) {
			suffix.push_back(digits[byte >> 4]);
			suffix.push_back(digits[byte & 0x0f]);
		}
		m_root = std::filesystem::temp_directory_path() /
			(L"sakura-legacy-secret-vault-reader-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + suffix);
	}

	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}

	std::filesystem::path FirstSecretFile() const
	{
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator iterator(m_root, error), end;
			!error && iterator != end; iterator.increment(error)) {
			if (iterator->is_regular_file() && iterator->path().extension() == L".bin") return iterator->path();
		}
		return {};
	}

	std::filesystem::path FirstExtensionDirectory() const
	{
		std::error_code error;
		for (std::filesystem::directory_iterator iterator(m_root, error), end;
			!error && iterator != end; iterator.increment(error)) {
			if (iterator->is_directory()) return iterator->path();
		}
		return {};
	}

	std::vector<std::uint8_t> SnapshotFiles() const
	{
		std::vector<std::uint8_t> bytes;
		std::vector<std::filesystem::path> files;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator iterator(m_root, error), end;
			!error && iterator != end; iterator.increment(error)) {
			if (iterator->is_regular_file()) files.emplace_back(iterator->path());
		}
		std::sort(files.begin(), files.end());
		for (const auto& path : files) {
			std::ifstream input(path, std::ios::binary);
			bytes.insert(bytes.end(), std::istreambuf_iterator<char>(input), {});
		}
		return bytes;
	}

	std::filesystem::path m_root;
};

TEST_F(CLegacyExtensionSecretVaultReaderTest, DistinguishesAbsentAndEmptyNamespaces)
{
	CLegacyExtensionSecretVaultReader missing(m_root);
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::Absent, missing.ReadAll("sample.publisher").status);

	ASSERT_TRUE(std::filesystem::create_directories(m_root));
	CExtensionSecretStorage legacy(m_root);
	ASSERT_TRUE(legacy.Store(L"other.publisher", L"token", L"value").success);
	CLegacyExtensionSecretVaultReader reader(m_root);
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::Absent, reader.ReadAll("sample.publisher").status);

	// The legacy writer's extension directory is opaque; create it with a disposable fixture,
	// then remove that fixture through the legacy API during test setup.
	ASSERT_TRUE(legacy.Store(L"sample.publisher", L"temporary", L"value").success);
	ASSERT_TRUE(legacy.Delete(L"sample.publisher", L"temporary").success);
	const auto empty = reader.ReadAll("sample.publisher");
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::Empty, empty.status);
	EXPECT_TRUE(empty.entries.empty());
}

TEST_F(CLegacyExtensionSecretVaultReaderTest, ImportsMultipleLegacyEntriesWithoutMutatingFiles)
{
	CExtensionSecretStorage legacy(m_root);
	ASSERT_TRUE(legacy.Store(L"sample.publisher", L"z-key", L"z-value").success);
	ASSERT_TRUE(legacy.Store(L"sample.publisher", L"a-key", L"a-value").success);
	const auto before = SnapshotFiles();

	CLegacyExtensionSecretVaultReader reader(m_root);
	const auto result = reader.ReadAll("sample.publisher");
	const auto after = SnapshotFiles();

	ASSERT_EQ(ELegacyExtensionSecretVaultReadStatus::Success, result.status);
	ASSERT_EQ(2u, result.entries.size());
	EXPECT_EQ("a-key", result.entries[0].key);
	EXPECT_EQ("a-value", result.entries[0].value);
	EXPECT_EQ("z-key", result.entries[1].key);
	EXPECT_EQ("z-value", result.entries[1].value);
	EXPECT_EQ(before, after);
}

TEST_F(CLegacyExtensionSecretVaultReaderTest, FailsClosedForMalformedAndTamperedFilesWithoutWriting)
{
	CExtensionSecretStorage legacy(m_root);
	ASSERT_TRUE(legacy.Store(L"sample.publisher", L"token", L"value").success);
	const auto secret = FirstSecretFile();
	ASSERT_FALSE(secret.empty());

	{
		std::ofstream output(secret, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(output.is_open());
		output << "malformed";
	}
	const auto malformedBefore = SnapshotFiles();
	CLegacyExtensionSecretVaultReader reader(m_root);
	const auto malformed = reader.ReadAll("sample.publisher");
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::CorruptData, malformed.status);
	EXPECT_TRUE(malformed.entries.empty());
	EXPECT_EQ(malformedBefore, SnapshotFiles());

	ASSERT_TRUE(legacy.Store(L"sample.publisher", L"token", L"value").success);
	std::fstream tamper(secret, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(tamper.is_open());
	tamper.seekg(12);
	char byte = 0;
	tamper.read(&byte, 1);
	tamper.clear();
	tamper.seekp(12);
	byte ^= 0x5a;
	tamper.write(&byte, 1);
	tamper.close();
	const auto tamperedBefore = SnapshotFiles();
	const auto tampered = reader.ReadAll("sample.publisher");
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::CryptoError, tampered.status);
	EXPECT_TRUE(tampered.entries.empty());
	EXPECT_EQ(tamperedBefore, SnapshotFiles());
}

TEST_F(CLegacyExtensionSecretVaultReaderTest, RequiresCanonicalIdentityAndNeverUsesItAsAPath)
{
	CLegacyExtensionSecretVaultReader reader(m_root);
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::InvalidArgument, reader.ReadAll("Sample.Publisher").status);
	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::InvalidArgument, reader.ReadAll("sample.publisher/..").status);
	EXPECT_FALSE(std::filesystem::exists(m_root));
}

TEST_F(CLegacyExtensionSecretVaultReaderTest, RejectsAggregateEncryptedNamespaceBeforeDecrypting)
{
	CExtensionSecretStorage legacy(m_root);
	ASSERT_TRUE(legacy.Store(L"sample.publisher", L"temporary", L"value").success);
	ASSERT_TRUE(legacy.Delete(L"sample.publisher", L"temporary").success);
	const auto extensionDirectory = FirstExtensionDirectory();
	ASSERT_FALSE(extensionDirectory.empty());

	for (int index = 0; index < 3; ++index) {
		const auto path = extensionDirectory / (L"oversized-" + std::to_wstring(index) + L".bin");
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(output.is_open());
		output.close();
		std::filesystem::resize_file(path, 6 * 1024 * 1024);
	}
	const auto before = SnapshotFiles();

	CLegacyExtensionSecretVaultReader reader(m_root);
	const auto result = reader.ReadAll("sample.publisher");

	EXPECT_EQ(ELegacyExtensionSecretVaultReadStatus::CorruptData, result.status);
	EXPECT_EQ(ERROR_FILE_TOO_LARGE, result.errorCode);
	EXPECT_TRUE(result.entries.empty());
	EXPECT_EQ(before, SnapshotFiles());
}

} // namespace
} // namespace platform::secrets
