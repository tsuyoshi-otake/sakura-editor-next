/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionTrustStore.h"

#include <filesystem>
#include <fstream>

namespace {

class CExtensionTrustStoreTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		m_root = std::filesystem::temp_directory_path() /
			(L"sakura-extension-trust-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(::GetTickCount64()));
		std::filesystem::create_directories(m_root / L"extension");
		m_file = m_root / L"trust.json";
	}
	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_root, ignored);
	}
	std::filesystem::path m_root;
	std::filesystem::path m_file;
};

TEST_F(CExtensionTrustStoreTest, PersistsOnlyTheExactExtensionVersionAndPath)
{
	const auto extensionPath = m_root / L"extension";
	CExtensionTrustStore first(m_file);
	EXPECT_FALSE(first.IsTrusted(L"sample.extension", L"1.0.0", extensionPath.wstring()));
	ASSERT_TRUE(first.Grant(L"sample.extension", L"1.0.0", extensionPath.wstring()));
	CExtensionTrustStore reopened(m_file);
	EXPECT_TRUE(reopened.IsTrusted(L"sample.extension", L"1.0.0", extensionPath.wstring()));
	EXPECT_FALSE(reopened.IsTrusted(L"sample.extension", L"1.0.1", extensionPath.wstring()));
	EXPECT_FALSE(reopened.IsTrusted(L"other.extension", L"1.0.0", extensionPath.wstring()));
	EXPECT_FALSE(reopened.IsTrusted(L"sample.extension", L"1.0.0", (m_root / L"other").wstring()));
}

TEST_F(CExtensionTrustStoreTest, CorruptStorageFailsClosedAndCanBeRepairedByExplicitGrant)
{
	{
		std::ofstream output(m_file, std::ios::binary);
		output << "not-json";
	}
	CExtensionTrustStore store(m_file);
	EXPECT_FALSE(store.IsTrusted(L"sample.extension", L"1.0.0", (m_root / L"extension").wstring()));
	ASSERT_TRUE(store.Grant(L"sample.extension", L"1.0.0", (m_root / L"extension").wstring()));
	EXPECT_TRUE(store.IsTrusted(L"sample.extension", L"1.0.0", (m_root / L"extension").wstring()));
	EXPECT_TRUE(store.RevokeAll());
	EXPECT_FALSE(store.IsTrusted(L"sample.extension", L"1.0.0", (m_root / L"extension").wstring()));
}

TEST_F(CExtensionTrustStoreTest, RejectsIncompleteIdentities)
{
	CExtensionTrustStore store(m_file);
	EXPECT_FALSE(store.Grant(L"", L"1.0.0", (m_root / L"extension").wstring()));
	EXPECT_FALSE(store.Grant(L"sample.extension", L"", (m_root / L"extension").wstring()));
	EXPECT_FALSE(store.Grant(L"sample.extension", L"1.0.0", L""));
}

} // namespace
