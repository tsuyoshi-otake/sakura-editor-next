/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <gtest/gtest.h>

#include "platform/process/InheritedHandleList.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct ScopedHandle final {
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE handle) noexcept : handle(handle) {}
	~ScopedHandle() { if (IsValid()) ::CloseHandle(handle); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	[[nodiscard]] bool IsValid() const noexcept
	{
		return handle != nullptr && handle != INVALID_HANDLE_VALUE;
	}
	HANDLE handle{};
};

struct ScopedEnvironmentVariable final {
	explicit ScopedEnvironmentVariable(const wchar_t* name) : name(name)
	{
		std::array<wchar_t, 32768> buffer{};
		const DWORD length = ::GetEnvironmentVariableW(name, buffer.data(),
			static_cast<DWORD>(buffer.size()));
		if (length != 0 && length < buffer.size()) original.assign(buffer.data(), length);
	}
	~ScopedEnvironmentVariable()
	{
		::SetEnvironmentVariableW(name.c_str(), original.empty() ? nullptr : original.c_str());
	}
	std::wstring name;
	std::wstring original;
};

constexpr wchar_t kUnrelatedHandleVariable[] = L"SAKURA_IHL_UNRELATED_HANDLE";

std::wstring CurrentExecutable()
{
	std::wstring path(32768, L'\0');
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	return path;
}

bool IsInheritable(HANDLE handle)
{
	DWORD flags{};
	return ::GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT) != 0;
}

TEST(InheritedHandleList, EmptyStdHandlesProduceNoListAndNoInheritance)
{
	platform::process::InheritedHandleList list;
	ASSERT_TRUE(list.Prepare());
	EXPECT_TRUE(list.Handles().empty());
	EXPECT_EQ(list.InheritHandles(), FALSE);
	EXPECT_EQ(list.AdditionalCreationFlags(), 0u);
	EXPECT_EQ(list.LaunchStartupInfo()->cb, static_cast<DWORD>(sizeof(STARTUPINFOW)));
	EXPECT_EQ(list.LaunchStartupInfo()->dwFlags & STARTF_USESTDHANDLES, 0u);
}

TEST(InheritedHandleList, DuplicateNullInvalidAndNonInheritableHandlesAreExcluded)
{
	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	ScopedHandle read, write;
	ASSERT_TRUE(::CreatePipe(&read.handle, &write.handle, &security, 0));
	ScopedHandle nonInheritable(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	ASSERT_TRUE(nonInheritable.IsValid());
	ASSERT_FALSE(IsInheritable(nonInheritable.handle));

	platform::process::InheritedHandleList list;
	auto& startup = list.StartupInfo();
	startup.hStdInput = INVALID_HANDLE_VALUE;       // invalid: excluded
	startup.hStdOutput = write.handle;              // inheritable
	startup.hStdError = nonInheritable.handle;      // lacks HANDLE_FLAG_INHERIT: excluded
	ASSERT_TRUE(list.Prepare());
	EXPECT_EQ(list.InheritHandles(), TRUE);
	EXPECT_EQ(list.AdditionalCreationFlags(), DWORD(EXTENDED_STARTUPINFO_PRESENT));
	EXPECT_EQ(list.LaunchStartupInfo()->cb, static_cast<DWORD>(sizeof(STARTUPINFOEXW)));

	// The same inheritable handle in two std fields collapses to one entry.
	platform::process::InheritedHandleList duplicate;
	auto& duplicateStartup = duplicate.StartupInfo();
	duplicateStartup.hStdInput = nullptr;
	duplicateStartup.hStdOutput = write.handle;
	duplicateStartup.hStdError = write.handle;
	ASSERT_TRUE(duplicate.Prepare());
	ASSERT_EQ(duplicate.Handles().size(), 1u);
	EXPECT_EQ(duplicate.Handles()[0], write.handle);

	EXPECT_EQ(list.Handles().size(), 1u);
	EXPECT_EQ(list.Handles()[0], write.handle);
}

TEST(InheritedHandleList, OnlyNonInheritableStdHandlesDisableInheritance)
{
	ScopedHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	ASSERT_TRUE(event.IsValid());
	platform::process::InheritedHandleList list;
	auto& startup = list.StartupInfo();
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = nullptr;
	startup.hStdOutput = event.handle;
	startup.hStdError = event.handle;
	ASSERT_TRUE(list.Prepare());
	EXPECT_TRUE(list.Handles().empty());
	EXPECT_EQ(list.InheritHandles(), FALSE);
	EXPECT_EQ(list.AdditionalCreationFlags(), 0u);
	EXPECT_EQ(list.LaunchStartupInfo()->cb, static_cast<DWORD>(sizeof(STARTUPINFOW)));
}

TEST(InheritedHandleList, RealChildInheritsOnlyTheListedHandles)
{
	// An unrelated inheritable handle must not leak into the child. Occupy the
	// low handle range first so the value probed in the child cannot
	// coincidentally collide with one of the child's own startup handles.
	std::vector<ScopedHandle> padding(64);
	for (auto& holder : padding) {
		holder.handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		ASSERT_TRUE(holder.IsValid());
	}
	ScopedHandle unrelated(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
	ASSERT_TRUE(unrelated.IsValid());
	padding.clear();
	ASSERT_TRUE(::SetHandleInformation(unrelated.handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	ScopedHandle read, write;
	ASSERT_TRUE(::CreatePipe(&read.handle, &write.handle, &security, 0));
	// The parent side stays non-inheritable, like the launchers do.
	ASSERT_TRUE(::SetHandleInformation(read.handle, HANDLE_FLAG_INHERIT, 0));
	ScopedHandle input(::CreateFileW(L"NUL", GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr));
	ASSERT_TRUE(input.IsValid());

	ScopedEnvironmentVariable variable(kUnrelatedHandleVariable);
	const std::wstring encoded = std::to_wstring(
		reinterpret_cast<ULONG_PTR>(unrelated.handle));
	ASSERT_TRUE(::SetEnvironmentVariableW(kUnrelatedHandleVariable, encoded.c_str()));

	platform::process::InheritedHandleList list;
	auto& startup = list.StartupInfo();
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input.handle;
	startup.hStdOutput = write.handle;
	startup.hStdError = write.handle;
	ASSERT_TRUE(list.Prepare());
	ASSERT_TRUE(list.InheritHandles());
	EXPECT_EQ(list.Handles().size(), 2u);

	const std::wstring executable = CurrentExecutable();
	std::wstring commandLine = L"\"" + executable
		+ L"\" --gtest_filter=InheritedHandleListChild.UnrelatedHandleIsNotInherited"
		  L" --gtest_color=no --gtest_brief=1";
	PROCESS_INFORMATION process{};
	ASSERT_TRUE(::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
		list.InheritHandles(), CREATE_NO_WINDOW | list.AdditionalCreationFlags(),
		nullptr, nullptr, list.LaunchStartupInfo(), &process));
	ScopedHandle processHandle(process.hProcess);
	ScopedHandle threadHandle(process.hThread);
	// The child-side handles are inherited now; close the parent's copies like
	// the launchers do.
	::CloseHandle(write.handle);
	write.handle = nullptr;
	::CloseHandle(input.handle);
	input.handle = nullptr;

	// Drain while waiting so the child's gtest output never fills the pipe.
	// A hung child is terminated at the deadline instead of hanging tests1.
	std::string output;
	std::array<char, 4096> buffer{};
	const auto drain = [&] {
		DWORD available{};
		while (::PeekNamedPipe(read.handle, nullptr, 0, nullptr, &available, nullptr)
			&& available != 0) {
			DWORD chunk{};
			const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
			if (!::ReadFile(read.handle, buffer.data(), wanted, &chunk, nullptr) || chunk == 0) break;
			output.append(buffer.data(), chunk);
		}
	};
	const ULONGLONG deadline = ::GetTickCount64() + 30000;
	bool exited = false;
	for (;;) {
		const DWORD wait = ::WaitForSingleObject(process.hProcess, 50);
		drain();
		if (wait == WAIT_OBJECT_0) {
			exited = true;
			drain();
			break;
		}
		if (::GetTickCount64() >= deadline) break;
	}
	if (!exited) {
		(void)::TerminateProcess(process.hProcess, 1);
		(void)::WaitForSingleObject(process.hProcess, 5000);
		FAIL() << "child did not exit within 30 s; output so far:\n" << output;
	}
	DWORD exitCode = 1;
	EXPECT_TRUE(::GetExitCodeProcess(process.hProcess, &exitCode));
	EXPECT_EQ(exitCode, 0u) << output;
}

} // namespace

TEST(InheritedHandleListChild, UnrelatedHandleIsNotInherited)
{
	std::array<wchar_t, 32> encoded{};
	const DWORD length = ::GetEnvironmentVariableW(kUnrelatedHandleVariable, encoded.data(),
		static_cast<DWORD>(encoded.size()));
	if (length == 0 || length >= encoded.size()) {
		GTEST_SKIP() << "only meaningful as the child of "
			"InheritedHandleList.RealChildInheritsOnlyTheListedHandles";
	}
	const HANDLE value = reinterpret_cast<HANDLE>(
		static_cast<ULONG_PTR>(std::wcstoull(encoded.data(), nullptr, 10)));

	// The unrelated handle was inheritable in the parent, so it would keep
	// the same value here if the launch had leaked it.
	DWORD flags{};
	const BOOL valid = ::GetHandleInformation(value, &flags);
	const DWORD error = ::GetLastError();
	EXPECT_FALSE(valid) << "unrelated inheritable handle leaked into the child";
	EXPECT_EQ(error, DWORD(ERROR_INVALID_HANDLE));

	// The listed std handles did arrive, valid and inheritable.
	const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
	ASSERT_NE(out, nullptr);
	ASSERT_NE(out, INVALID_HANDLE_VALUE);
	EXPECT_TRUE(IsInheritable(out));
}
