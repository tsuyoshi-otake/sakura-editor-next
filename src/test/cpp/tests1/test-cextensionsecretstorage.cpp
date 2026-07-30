/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionSecretStorage.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <bcrypt.h>
#include <windows.h>

namespace {

class CExtensionSecretStorageTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		std::array<std::uint8_t, 8> random{};
		ASSERT_GE(::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
			BCRYPT_USE_SYSTEM_PREFERRED_RNG), 0);
		std::wstring suffix;
		static constexpr wchar_t digits[] = L"0123456789abcdef";
		for (const auto byte : random) {
			suffix.push_back(digits[byte >> 4]);
			suffix.push_back(digits[byte & 0x0f]);
		}
		m_root = std::filesystem::temp_directory_path() /
			(L"sakura-secret-storage-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + suffix);
	}

	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}

	std::vector<std::uint8_t> ReadAllStorageBytes() const
	{
		std::vector<std::uint8_t> result;
		std::error_code error;
		if (!std::filesystem::exists(m_root, error)) return result;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(m_root)) {
			if (!entry.is_regular_file()) continue;
			std::ifstream input(entry.path(), std::ios::binary);
			result.insert(result.end(), std::istreambuf_iterator<char>(input), {});
		}
		return result;
	}

	std::filesystem::path FirstSecretFile() const
	{
		for (const auto& entry : std::filesystem::recursive_directory_iterator(m_root)) {
			if (entry.is_regular_file() && entry.path().extension() == L".bin") return entry.path();
		}
		return {};
	}

	std::filesystem::path m_root;
};

TEST_F(CExtensionSecretStorageTest, RoundTripsUnicodeWithoutPersistingPlaintext)
{
	CExtensionSecretStorage storage(m_root);
	ASSERT_TRUE(storage.Store(L"sample.publisher", L"api-token", L"秘密-very-secret-value").success);
	const auto read = storage.Get(L"sample.publisher", L"api-token");
	ASSERT_TRUE(read.success);
	ASSERT_TRUE(read.value.has_value());
	EXPECT_EQ(L"秘密-very-secret-value", *read.value);

	const auto bytes = ReadAllStorageBytes();
	const std::string persisted(bytes.begin(), bytes.end());
	EXPECT_EQ(std::string::npos, persisted.find("very-secret-value"));
	EXPECT_EQ(std::string::npos, persisted.find("api-token"));
	EXPECT_EQ(std::string::npos, persisted.find("sample.publisher"));
}

TEST_F(CExtensionSecretStorageTest, NamespacesSecretsByExtensionId)
{
	CExtensionSecretStorage storage(m_root);
	ASSERT_TRUE(storage.Store(L"first.extension", L"token", L"first-value").success);
	ASSERT_TRUE(storage.Store(L"second.extension", L"token", L"second-value").success);
	const auto first = storage.Get(L"first.extension", L"token");
	const auto second = storage.Get(L"second.extension", L"token");
	ASSERT_TRUE(first.success && first.value);
	ASSERT_TRUE(second.success && second.value);
	EXPECT_EQ(L"first-value", *first.value);
	EXPECT_EQ(L"second-value", *second.value);
}

TEST_F(CExtensionSecretStorageTest, ListsKeysAndDeletesIdempotently)
{
	CExtensionSecretStorage storage(m_root);
	ASSERT_TRUE(storage.Store(L"sample.publisher", L"z-key", L"z").success);
	ASSERT_TRUE(storage.Store(L"sample.publisher", L"a-key", L"a").success);
	const auto keys = storage.Keys(L"sample.publisher");
	ASSERT_TRUE(keys.success);
	EXPECT_EQ((std::vector<std::wstring>{ L"a-key", L"z-key" }), keys.keys);
	EXPECT_TRUE(storage.Delete(L"sample.publisher", L"a-key").success);
	EXPECT_TRUE(storage.Delete(L"sample.publisher", L"a-key").success);
	const auto missing = storage.Get(L"sample.publisher", L"a-key");
	EXPECT_TRUE(missing.success);
	EXPECT_FALSE(missing.value.has_value());
}

TEST_F(CExtensionSecretStorageTest, ReportsCorruptCiphertextWithoutReturningAValue)
{
	CExtensionSecretStorage storage(m_root);
	ASSERT_TRUE(storage.Store(L"sample.publisher", L"token", L"value").success);
	const auto path = FirstSecretFile();
	ASSERT_FALSE(path.empty());
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(output.is_open());
		output << "corrupt";
		ASSERT_TRUE(output.good());
	}
	const auto read = storage.Get(L"sample.publisher", L"token");
	EXPECT_FALSE(read.success);
	EXPECT_EQ(EExtensionSecretStorageStatus::CorruptData, read.status);
	EXPECT_FALSE(read.value.has_value());
	EXPECT_EQ(std::wstring::npos, read.diagnostic.find(L"token"));
	EXPECT_EQ(std::wstring::npos, read.diagnostic.find(L"value"));
}

TEST_F(CExtensionSecretStorageTest, RejectsInvalidNamespaceAndOversizedValues)
{
	CExtensionSecretStorage storage(m_root);
	EXPECT_FALSE(storage.Store(L"missing-separator", L"token", L"value").success);
	EXPECT_FALSE(storage.Store(L"sample.publisher", L"", L"value").success);
	EXPECT_FALSE(storage.Store(L"sample.publisher", L"token", std::wstring(1024 * 1024 + 1, L'x')).success);
}

} // namespace
