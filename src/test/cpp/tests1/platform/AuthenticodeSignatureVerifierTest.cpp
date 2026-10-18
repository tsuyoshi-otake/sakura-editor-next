/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include <sakura/security/AuthenticodeSignatureVerifier.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr wchar_t kOtherVendorBinary[] = L"C:\\Program Files\\Git\\mingw64\\bin\\git.exe";

std::wstring CurrentExecutable()
{
	std::wstring path(32768, L'\0');
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	return path;
}

//! A test-owned copy of a file, removed with its directory on every exit path.
struct ScopedCopy final {
	explicit ScopedCopy(const std::filesystem::path& source)
	{
		directory = std::filesystem::temp_directory_path()
			/ (L"sakura-authenticode-" + std::to_wstring(::GetCurrentProcessId()));
		std::filesystem::create_directories(directory);
		path = directory / source.filename();
		std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing);
	}
	~ScopedCopy()
	{
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}
	std::filesystem::path directory;
	std::filesystem::path path;
};

} // namespace

TEST(AuthenticodeSignatureVerifier, RejectsMissingAndUnsignedFiles)
{
	EXPECT_FALSE(platform::security::VerifiedSignerOrganization(
		L"C:\\definitely-not-present\\gh.exe"));
	EXPECT_FALSE(platform::security::VerifiedSignerOrganization(CurrentExecutable()))
		<< L"a self-built unsigned binary carries no signer organization";
	EXPECT_FALSE(platform::security::VerifiedSignerOrganization(L""));
}

TEST(AuthenticodeSignatureVerifier, ReportsTheGitHubOrganizationForTheVendorCli)
{
	const wchar_t* const cli = L"C:\\Program Files\\GitHub CLI\\gh.exe";
	if (::GetFileAttributesW(cli) == INVALID_FILE_ATTRIBUTES) {
		GTEST_SKIP() << "requires the vendor GitHub CLI install under Program Files";
	}
	const auto organization = platform::security::VerifiedSignerOrganization(cli);
	ASSERT_TRUE(organization);
	EXPECT_EQ(L"GitHub, Inc.", *organization);
}

TEST(AuthenticodeSignatureVerifier, ReportsADifferentOrganizationForAnotherSignedBinary)
{
	if (::GetFileAttributesW(kOtherVendorBinary) == INVALID_FILE_ATTRIBUTES) {
		GTEST_SKIP() << "requires another vendor's embedded-signed binary";
	}
	const auto organization = platform::security::VerifiedSignerOrganization(kOtherVendorBinary);
	ASSERT_TRUE(organization) << L"another vendor's embedded signature must verify, only the publisher differs";
	EXPECT_NE(L"GitHub, Inc.", *organization);
}

TEST(AuthenticodeSignatureVerifier, RejectsASignedFileWhoseContentNoLongerMatchesItsSignature)
{
	if (::GetFileAttributesW(kOtherVendorBinary) == INVALID_FILE_ATTRIBUTES) {
		GTEST_SKIP() << "requires another vendor's embedded-signed binary";
	}
	const ScopedCopy copy(kOtherVendorBinary);
	ASSERT_TRUE(platform::security::VerifiedSignerOrganization(copy.path.wstring()))
		<< L"the unmodified copy must verify, or the tampered case below proves nothing";

	// One byte in the middle of the image lies inside a hashed section, far
	// from the trailing certificate table, so the signature still parses and
	// names its signer while its digest no longer matches.
	const auto size = std::filesystem::file_size(copy.path);
	{
		std::fstream file(copy.path, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file);
		file.seekg(static_cast<std::streamoff>(size / 2));
		char value = 0;
		ASSERT_TRUE(file.get(value));
		file.seekp(static_cast<std::streamoff>(size / 2));
		ASSERT_TRUE(file.put(static_cast<char>(value ^ 0x5A)));
	}
	EXPECT_FALSE(platform::security::VerifiedSignerOrganization(copy.path.wstring()));
}
