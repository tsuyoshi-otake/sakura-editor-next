/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "extmodule/CDllHandler.h"
#include "platform/Windows11Platform.h"

namespace {

constexpr std::size_t kNtOffset = 0x80;

void Write16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::byte>(value & 0xff);
	bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void Write32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
	bytes[offset] = static_cast<std::byte>(value & 0xff);
	bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
	bytes[offset + 2] = static_cast<std::byte>((value >> 16) & 0xff);
	bytes[offset + 3] = static_cast<std::byte>((value >> 24) & 0xff);
}

std::vector<std::byte> MakePe(std::uint16_t machine)
{
	std::vector<std::byte> bytes(kNtOffset + 24);
	Write16(bytes, 0, 0x5a4d); // MZ
	Write32(bytes, 0x3c, kNtOffset);
	Write32(bytes, kNtOffset, 0x00004550); // PE\0\0
	Write16(bytes, kNtOffset + 4, machine);
	return bytes;
}

class TemporaryPeFile final {
public:
	explicit TemporaryPeFile(std::span<const std::byte> bytes)
	{
		wchar_t temporaryDirectory[MAX_PATH]{};
		const DWORD directoryLength = ::GetTempPathW(std::size(temporaryDirectory), temporaryDirectory);
		if (directoryLength == 0 || directoryLength >= std::size(temporaryDirectory)) {
			throw std::runtime_error("GetTempPathW failed");
		}

		wchar_t temporaryFile[MAX_PATH]{};
		if (::GetTempFileNameW(temporaryDirectory, L"skp", 0, temporaryFile) == 0) {
			throw std::runtime_error("GetTempFileNameW failed");
		}
		m_path = temporaryFile;

		std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
		if (!file) {
			throw std::runtime_error("Could not open temporary PE file");
		}
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!file) {
			throw std::runtime_error("Could not write temporary PE file");
		}
	}

	~TemporaryPeFile()
	{
		std::error_code error;
		std::filesystem::remove(m_path, error);
	}

	const wchar_t* path() const noexcept
	{
		return m_path.c_str();
	}

private:
	std::filesystem::path m_path;
};

class PeProbeDll final : public CDllImp {
protected:
	bool InitDllImp() override
	{
		return true;
	}

	LPCWSTR GetDllNameImp(int) override
	{
		return nullptr;
	}
};

} // namespace

TEST(Windows11Platform, EvaluatesSuccessfulBuildQuery)
{
	const auto result = platform::EvaluateWindowsBuildQuery(0, 10, 0, 22631);

	EXPECT_EQ(platform::WindowsBuildStatus::Success, result.status);
	EXPECT_EQ(22631u, result.build);
	EXPECT_TRUE(platform::IsBuildAtLeast(result, 22000));
	EXPECT_TRUE(platform::SupportsWindows11Features(result));
}

TEST(Windows11Platform, RejectsFailedBuildQuery)
{
	const auto result = platform::EvaluateWindowsBuildQuery(-1, 10, 0, 22631);

	EXPECT_EQ(platform::WindowsBuildStatus::RtlGetVersionFailed, result.status);
	EXPECT_FALSE(platform::IsBuildAtLeast(result, 1));
	EXPECT_FALSE(platform::SupportsWindows11Features(result));
}

TEST(Windows11Platform, Windows11FeatureBoundaryIsBuild22000)
{
	const auto oldBuild = platform::EvaluateWindowsBuildQuery(0, 10, 0, 21999);
	const auto windows11Build = platform::EvaluateWindowsBuildQuery(0, 10, 0, 22000);

	EXPECT_FALSE(platform::SupportsWindows11Features(oldBuild));
	EXPECT_TRUE(platform::SupportsWindows11Features(windows11Build));
}

TEST(Windows11Platform, FormatsNeutralStartupDiagnostic)
{
	const auto result = platform::EvaluateWindowsBuildQuery(0, 10, 0, 22631);
	const auto diagnostic = platform::FormatStartupPlatformDiagnostic(result);

	EXPECT_STREQ(L"Sakura Editor NEXT requires Windows 11 build 22000 or later. Detected Windows 10.0 build 22631.", diagnostic.text.data());
}

TEST(Windows11Platform, ParsesSupportedPeMachineTypes)
{
	EXPECT_EQ(platform::PeMachine::Amd64, platform::ParsePeMachine(MakePe(0x8664)));
	EXPECT_EQ(platform::PeMachine::I386, platform::ParsePeMachine(MakePe(0x014c)));
	EXPECT_EQ(platform::PeMachine::Arm64, platform::ParsePeMachine(MakePe(0xaa64)));
	EXPECT_EQ(platform::PeMachine::Unknown, platform::ParsePeMachine(MakePe(0x0200)));
}

TEST(Windows11Platform, RejectsTruncatedAndBadPeHeaders)
{
	EXPECT_EQ(platform::PeMachine::Invalid, platform::ParsePeMachine({}));

	auto truncatedDos = MakePe(0x8664);
	truncatedDos.resize(63);
	EXPECT_EQ(platform::PeMachine::Invalid, platform::ParsePeMachine(truncatedDos));

	auto truncatedNt = MakePe(0x8664);
	truncatedNt.resize(kNtOffset + 23);
	EXPECT_EQ(platform::PeMachine::Invalid, platform::ParsePeMachine(truncatedNt));

	auto badDosSignature = MakePe(0x8664);
	badDosSignature[0] = std::byte{ 0 };
	EXPECT_EQ(platform::PeMachine::Invalid, platform::ParsePeMachine(badDosSignature));

	auto badNtSignature = MakePe(0x8664);
	badNtSignature[kNtOffset] = std::byte{ 0 };
	EXPECT_EQ(platform::PeMachine::Invalid, platform::ParsePeMachine(badNtSignature));
}

TEST(Windows11Platform, RejectsOverflowingNtHeaderOffset)
{
	auto bytes = MakePe(0x8664);
	Write32(bytes, 0x3c, 0xfffffff0);

	EXPECT_EQ(platform::PeMachine::Invalid, platform::ParsePeMachine(bytes));
}

TEST(Windows11Platform, ReadsMachineTypeFromBoundedFileHeaders)
{
	TemporaryPeFile amd64File(MakePe(0x8664));
	TemporaryPeFile i386File(MakePe(0x014c));

	EXPECT_EQ(platform::PeMachine::Amd64, platform::ReadPeMachineFromFile(amd64File.path()));
	EXPECT_EQ(platform::PeMachine::I386, platform::ReadPeMachineFromFile(i386File.path()));
}

TEST(Windows11Platform, RejectsNtHeaderOffsetBeyondFileSize)
{
	auto bytes = MakePe(0x8664);
	bytes.resize(64);
	TemporaryPeFile file(bytes);

	EXPECT_EQ(platform::PeMachine::Invalid, platform::ReadPeMachineFromFile(file.path()));
}

TEST(Windows11Platform, RejectsI386BeforeDynamicLibraryLoading)
{
	TemporaryPeFile i386File(MakePe(0x014c));
	PeProbeDll dll;

	EXPECT_EQ(DLL_MACHINE_MISMATCH, dll.InitDll(i386File.path()));
	EXPECT_FALSE(dll.IsAvailable());
}
