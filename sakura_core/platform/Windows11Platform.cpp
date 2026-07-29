/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "platform/Windows11Platform.h"

namespace {

struct RtlOsVersionInfoW {
	ULONG dwOSVersionInfoSize;
	ULONG dwMajorVersion;
	ULONG dwMinorVersion;
	ULONG dwBuildNumber;
	ULONG dwPlatformId;
	WCHAR szCSDVersion[128];
};

using RtlGetVersionProc = LONG (WINAPI*)(RtlOsVersionInfoW*);

constexpr std::size_t kDosHeaderSize = 64;
constexpr std::size_t kELfanewOffset = 0x3c;
constexpr std::size_t kNtSignatureSize = 4;
constexpr std::size_t kFileHeaderSize = 20;
constexpr std::size_t kNtHeaderPrefixSize = kNtSignatureSize + kFileHeaderSize;
constexpr std::uint16_t kImageFileMachineI386 = 0x014c;
constexpr std::uint16_t kImageFileMachineAmd64 = 0x8664;
constexpr std::uint16_t kImageFileMachineArm64 = 0xaa64;

std::uint16_t ReadLittleEndian16(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
	return std::uint16_t(std::to_integer<std::uint8_t>(bytes[offset]))
		| (std::uint16_t(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8);
}

std::uint32_t ReadLittleEndian32(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
	return std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset]))
		| (std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8)
		| (std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16)
		| (std::uint32_t(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24);
}

void AppendLiteral(platform::StartupPlatformDiagnostic& diagnostic, std::size_t& length, const wchar_t* text) noexcept
{
	while (*text != L'\0' && length + 1 < diagnostic.text.size()) {
		diagnostic.text[length++] = *text++;
	}
	diagnostic.text[length] = L'\0';
}

void AppendUnsigned(platform::StartupPlatformDiagnostic& diagnostic, std::size_t& length, std::uint32_t value) noexcept
{
	wchar_t digits[10];
	std::size_t count = 0;
	do {
		digits[count++] = wchar_t(L'0' + value % 10);
		value /= 10;
	} while (value != 0 && count < std::size(digits));
	while (count != 0 && length + 1 < diagnostic.text.size()) {
		diagnostic.text[length++] = digits[--count];
	}
	diagnostic.text[length] = L'\0';
}

const wchar_t* StatusText(platform::WindowsBuildStatus status) noexcept
{
	switch (status) {
	case platform::WindowsBuildStatus::NtdllUnavailable:
		return L"ntdll unavailable";
	case platform::WindowsBuildStatus::RtlGetVersionUnavailable:
		return L"RtlGetVersion unavailable";
	case platform::WindowsBuildStatus::RtlGetVersionFailed:
		return L"RtlGetVersion failed";
	case platform::WindowsBuildStatus::Success:
		break;
	}
	return L"unknown error";
}

enum class ReadResult {
	Complete,
	Truncated,
	IoError,
};

ReadResult ReadExactly(HANDLE file, void* buffer, DWORD size) noexcept
{
	DWORD bytesRead = 0;
	if (::ReadFile(file, buffer, size, &bytesRead, nullptr) == FALSE) {
		return ReadResult::IoError;
	}
	return bytesRead == size ? ReadResult::Complete : ReadResult::Truncated;
}

} // namespace

namespace platform {

WindowsBuildResult EvaluateWindowsBuildQuery(
	std::int32_t ntStatus,
	std::uint32_t major,
	std::uint32_t minor,
	std::uint32_t build
) noexcept
{
	if (ntStatus < 0) {
		return {};
	}
	return { WindowsBuildStatus::Success, major, minor, build };
}

WindowsBuildResult QueryWindowsBuild() noexcept
{
	const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr) {
		return { WindowsBuildStatus::NtdllUnavailable };
	}
	const auto rtlGetVersion = reinterpret_cast<RtlGetVersionProc>(::GetProcAddress(ntdll, "RtlGetVersion"));
	if (rtlGetVersion == nullptr) {
		return { WindowsBuildStatus::RtlGetVersionUnavailable };
	}

	RtlOsVersionInfoW version{};
	version.dwOSVersionInfoSize = sizeof(version);
	const LONG status = rtlGetVersion(&version);
	return EvaluateWindowsBuildQuery(
		status,
		version.dwMajorVersion,
		version.dwMinorVersion,
		version.dwBuildNumber
	);
}

bool IsBuildAtLeast(const WindowsBuildResult& result, std::uint32_t minimumBuild) noexcept
{
	return result.status == WindowsBuildStatus::Success && result.build >= minimumBuild;
}

bool SupportsWindows11Features(const WindowsBuildResult& result) noexcept
{
	return IsBuildAtLeast(result, 22000);
}

StartupPlatformDiagnostic FormatStartupPlatformDiagnostic(const WindowsBuildResult& result) noexcept
{
	StartupPlatformDiagnostic diagnostic{};
	std::size_t length = 0;
	AppendLiteral(diagnostic, length, L"Sakura Editor NEXT requires Windows 11 build 22000 or later. ");
	if (result.status != WindowsBuildStatus::Success) {
		AppendLiteral(diagnostic, length, L"Windows build detection failed: ");
		AppendLiteral(diagnostic, length, StatusText(result.status));
		AppendLiteral(diagnostic, length, L".");
		return diagnostic;
	}

	AppendLiteral(diagnostic, length, L"Detected Windows ");
	AppendUnsigned(diagnostic, length, result.major);
	AppendLiteral(diagnostic, length, L".");
	AppendUnsigned(diagnostic, length, result.minor);
	AppendLiteral(diagnostic, length, L" build ");
	AppendUnsigned(diagnostic, length, result.build);
	AppendLiteral(diagnostic, length, L".");
	return diagnostic;
}

PeMachine ParsePeMachine(std::span<const std::byte> bytes) noexcept
{
	if (bytes.size() < kDosHeaderSize
		|| ReadLittleEndian16(bytes, 0) != 0x5a4d) { // MZ
		return PeMachine::Invalid;
	}

	const std::size_t ntOffset = ReadLittleEndian32(bytes, kELfanewOffset);
	if (ntOffset < kDosHeaderSize
		|| ntOffset > bytes.size()
		|| bytes.size() - ntOffset < kNtHeaderPrefixSize) {
		return PeMachine::Invalid;
	}
	if (ReadLittleEndian32(bytes, ntOffset) != 0x00004550) { // PE\0\0
		return PeMachine::Invalid;
	}

	switch (ReadLittleEndian16(bytes, ntOffset + kNtSignatureSize)) {
	case kImageFileMachineAmd64:
		return PeMachine::Amd64;
	case kImageFileMachineI386:
		return PeMachine::I386;
	case kImageFileMachineArm64:
		return PeMachine::Arm64;
	default:
		return PeMachine::Unknown;
	}
}

PeMachine ReadPeMachineFromFile(const wchar_t* path) noexcept
{
	if (path == nullptr || *path == L'\0') {
		return PeMachine::IoError;
	}
	const HANDLE file = ::CreateFileW(
		path,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);
	if (file == INVALID_HANDLE_VALUE) {
		return PeMachine::IoError;
	}
	LARGE_INTEGER fileSize{};
	if (::GetFileSizeEx(file, &fileSize) == FALSE) {
		::CloseHandle(file);
		return PeMachine::IoError;
	}
	if (fileSize.QuadPart < static_cast<LONGLONG>(kDosHeaderSize)) {
		::CloseHandle(file);
		return PeMachine::Invalid;
	}

	std::array<std::byte, kDosHeaderSize> dosHeader{};
	const ReadResult readDosHeader = ReadExactly(file, dosHeader.data(), DWORD(dosHeader.size()));
	if (readDosHeader != ReadResult::Complete) {
		::CloseHandle(file);
		return readDosHeader == ReadResult::IoError ? PeMachine::IoError : PeMachine::Invalid;
	}
	const std::size_t ntOffset = ReadLittleEndian32(dosHeader, kELfanewOffset);
	if (ReadLittleEndian16(dosHeader, 0) != 0x5a4d || ntOffset < kDosHeaderSize) {
		::CloseHandle(file);
		return PeMachine::Invalid;
	}
	const auto fileSizeBytes = static_cast<std::uint64_t>(fileSize.QuadPart);
	if (fileSizeBytes < kNtHeaderPrefixSize
		|| static_cast<std::uint64_t>(ntOffset) > fileSizeBytes - kNtHeaderPrefixSize) {
		::CloseHandle(file);
		return PeMachine::Invalid;
	}

	LARGE_INTEGER position{};
	position.QuadPart = static_cast<LONGLONG>(ntOffset);
	if (::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) == FALSE) {
		::CloseHandle(file);
		return PeMachine::IoError;
	}
	std::array<std::byte, kNtHeaderPrefixSize> ntHeader{};
	const ReadResult readNtHeader = ReadExactly(file, ntHeader.data(), DWORD(ntHeader.size()));
	::CloseHandle(file);
	if (readNtHeader != ReadResult::Complete) {
		return readNtHeader == ReadResult::IoError ? PeMachine::IoError : PeMachine::Invalid;
	}
	if (ReadLittleEndian32(ntHeader, 0) != 0x00004550) {
		return PeMachine::Invalid;
	}

	switch (ReadLittleEndian16(ntHeader, kNtSignatureSize)) {
	case kImageFileMachineAmd64:
		return PeMachine::Amd64;
	case kImageFileMachineI386:
		return PeMachine::I386;
	case kImageFileMachineArm64:
		return PeMachine::Arm64;
	default:
		return PeMachine::Unknown;
	}
}

} // namespace platform
