/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpChildJob.h"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace {

constexpr DWORD kAllUiRestrictions = JOB_OBJECT_UILIMIT_DESKTOP
	| JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS
	| JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES
	| JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD
	| JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;

struct JobGuard final {
	HANDLE value = ::CreateJobObjectW(nullptr, nullptr);
	~JobGuard() { if (value) ::CloseHandle(value); }
};

void ExpectExtendedLimits(HANDLE job, SIZE_T memoryBytes)
{
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
	ASSERT_TRUE(::QueryInformationJobObject(job, JobObjectExtendedLimitInformation,
		&extended, sizeof(extended), nullptr));
	constexpr DWORD shared = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
		| JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
	EXPECT_EQ(extended.BasicLimitInformation.LimitFlags & shared, shared);
	EXPECT_EQ(extended.BasicLimitInformation.ActiveProcessLimit, 1u);
	if (memoryBytes != 0) {
		EXPECT_NE(extended.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY, 0u);
		EXPECT_EQ(extended.ProcessMemoryLimit, memoryBytes);
	} else {
		EXPECT_EQ(extended.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY, 0u);
	}
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
	ASSERT_TRUE(::QueryInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui), nullptr));
	EXPECT_EQ(ui.UIRestrictionsClass, kAllUiRestrictions);
}

TEST(SenpChildJob, ExtensionHostLimitsAreAppliedAndReadBack)
{
	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	ASSERT_TRUE(senp::ConfigureChildJob(job.value, senp::kExtensionHostJobLimits));
	ExpectExtendedLimits(job.value, 512u * 1024u * 1024u);
}

TEST(SenpChildJob, PackageToolLimitsAreAppliedAndReadBack)
{
	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	ASSERT_TRUE(senp::ConfigureChildJob(job.value, senp::kPackageToolJobLimits));
	ExpectExtendedLimits(job.value, 256u * 1024u * 1024u);
}

TEST(SenpChildJob, ZeroMemoryLimitRequestsNoProcessMemoryCap)
{
	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	ASSERT_TRUE(senp::ConfigureChildJob(job.value, {}));
	ExpectExtendedLimits(job.value, 0);
}

TEST(SenpChildJob, InvalidJobFailsClosed)
{
	EXPECT_FALSE(senp::ConfigureChildJob(nullptr, senp::kExtensionHostJobLimits));
	EXPECT_FALSE(senp::ConfigureChildJob(INVALID_HANDLE_VALUE, senp::kExtensionHostJobLimits));
	// A job object is a different kernel type than e.g. an event: an arbitrary
	// handle must fail the Set/query sequence rather than silently "succeed".
	HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(event, nullptr);
	EXPECT_FALSE(senp::ConfigureChildJob(event, senp::kExtensionHostJobLimits));
	::CloseHandle(event);
}

TEST(SenpChildJob, TamperedReadBackFailsClosed)
{
	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	ASSERT_TRUE(senp::ConfigureChildJob(job.value, senp::kExtensionHostJobLimits));

	// A query that fails outright is a launch failure.
	EXPECT_FALSE(senp::detail::VerifyChildJobLimits(job.value, senp::kExtensionHostJobLimits,
		[](HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD, LPDWORD) -> BOOL { ::SetLastError(ERROR_ACCESS_DENIED); return FALSE; }));

	// A read-back missing one memory byte is a launch failure.
	EXPECT_FALSE(senp::detail::VerifyChildJobLimits(job.value, senp::kExtensionHostJobLimits,
		[](HANDLE h, JOBOBJECTINFOCLASS cls, LPVOID out, DWORD size, LPDWORD ret) -> BOOL {
			const BOOL ok = ::QueryInformationJobObject(h, cls, out, size, ret);
			if (ok && cls == JobObjectExtendedLimitInformation)
				static_cast<JOBOBJECT_EXTENDED_LIMIT_INFORMATION*>(out)->ProcessMemoryLimit -= 1;
			return ok;
		}));

	// A read-back that drops kill-on-close is a launch failure.
	EXPECT_FALSE(senp::detail::VerifyChildJobLimits(job.value, senp::kExtensionHostJobLimits,
		[](HANDLE h, JOBOBJECTINFOCLASS cls, LPVOID out, DWORD size, LPDWORD ret) -> BOOL {
			const BOOL ok = ::QueryInformationJobObject(h, cls, out, size, ret);
			if (ok && cls == JobObjectExtendedLimitInformation)
				static_cast<JOBOBJECT_EXTENDED_LIMIT_INFORMATION*>(out)->BasicLimitInformation.LimitFlags
					&= ~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			return ok;
		}));

	// A read-back that drops one UI restriction class is a launch failure.
	EXPECT_FALSE(senp::detail::VerifyChildJobLimits(job.value, senp::kExtensionHostJobLimits,
		[](HANDLE h, JOBOBJECTINFOCLASS cls, LPVOID out, DWORD size, LPDWORD ret) -> BOOL {
			const BOOL ok = ::QueryInformationJobObject(h, cls, out, size, ret);
			if (ok && cls == JobObjectBasicUIRestrictions)
				static_cast<JOBOBJECT_BASIC_UI_RESTRICTIONS*>(out)->UIRestrictionsClass
					&= ~DWORD(JOB_OBJECT_UILIMIT_READCLIPBOARD);
			return ok;
		}));

	// The honest read-back still verifies.
	EXPECT_TRUE(senp::detail::VerifyChildJobLimits(job.value, senp::kExtensionHostJobLimits,
		&::QueryInformationJobObject));
}

TEST(SenpChildJob, KillOnCloseOnlyJobFailsTheNewVerification)
{
	// The pre-B4b v1/tool configuration: kill-on-close and nothing else. The
	// shared verification must reject it, proving the new limits are real.
	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	ASSERT_TRUE(::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
		&limits, sizeof(limits)));
	EXPECT_FALSE(senp::detail::VerifyChildJobLimits(job.value, senp::kExtensionHostJobLimits,
		&::QueryInformationJobObject));
	EXPECT_FALSE(senp::detail::VerifyChildJobLimits(job.value, senp::kPackageToolJobLimits,
		&::QueryInformationJobObject));
}

TEST(SenpChildJob, RealChildProcessRunsInsideTheConfiguredJob)
{
	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	ASSERT_TRUE(senp::ConfigureChildJob(job.value, senp::kExtensionHostJobLimits));

	std::wstring command = L"cmd.exe /c rem";
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	ASSERT_TRUE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
	if (!::AssignProcessToJobObject(job.value, process.hProcess)) {
		const DWORD error = ::GetLastError();
		// Never leave the suspended child behind on a failed assertion.
		(void)::TerminateProcess(process.hProcess, 1);
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
		FAIL() << "AssignProcessToJobObject failed: " << error;
	}

	BOOL inJob = FALSE;
	ASSERT_TRUE(::IsProcessInJob(process.hProcess, nullptr, &inJob));
	EXPECT_TRUE(inJob);
	ExpectExtendedLimits(job.value, 512u * 1024u * 1024u);

	::ResumeThread(process.hThread);
	EXPECT_EQ(::WaitForSingleObject(process.hProcess, 10000), WAIT_OBJECT_0);
	::CloseHandle(process.hThread);
	::CloseHandle(process.hProcess);
}

TEST(SenpChildJob, LaunchConfigUsesExecutableDirectoryAndAllowlistedUnicodeEnvironment)
{
	// The marker proves the child's environment is an explicit allowlist, not
	// the whole test process environment the launch used to inherit.
	struct ScopedEnvironmentVariable final {
		explicit ScopedEnvironmentVariable(const wchar_t* name) : name(name) {}
		~ScopedEnvironmentVariable() { ::SetEnvironmentVariableW(name.c_str(), nullptr); }
		std::wstring name;
	} marker{ L"B6D_MARKER" };
	ASSERT_TRUE(::SetEnvironmentVariableW(L"B6D_MARKER", L"marker-value"));

	const auto config = senp::detail::BuildSenpChildLaunchConfig(
		L"C:\\senp\\bin\\sakura-senp-host.exe");
	EXPECT_EQ(config.workingDirectory, L"C:\\senp\\bin");
	EXPECT_NE(config.environmentFlags & CREATE_UNICODE_ENVIRONMENT, 0u);
	ASSERT_FALSE(config.environment.empty());

	std::vector<std::wstring> entries;
	for (const wchar_t* cursor = config.environment.data(); *cursor != L'\0';) {
		entries.emplace_back(cursor);
		cursor += entries.back().size() + 1;
	}
	std::array<wchar_t, 32768> expected{};
	ASSERT_LT(::GetEnvironmentVariableW(L"SystemRoot", expected.data(),
		static_cast<DWORD>(expected.size())), expected.size());
	const std::wstring expectedSystemRoot{ expected.data() };
	expected.fill(L'\0');
	ASSERT_LT(::GetEnvironmentVariableW(L"windir", expected.data(),
		static_cast<DWORD>(expected.size())), expected.size());
	const std::wstring expectedWindir{ expected.data() };
	// The entry keeps the parent's own name casing, so the name is matched
	// case-insensitively and the value exactly.
	const auto entryHasValue = [](const std::wstring& e, std::wstring_view name,
		const std::wstring& value) {
		const auto equals = e.find(L'=');
		return equals != std::wstring::npos && value == e.substr(equals + 1)
			&& ::CompareStringOrdinal(e.data(), static_cast<int>(equals),
				name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
	};
	EXPECT_NE(std::find_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
		return entryHasValue(e, L"SystemRoot", expectedSystemRoot);
	}), entries.end());
	EXPECT_NE(std::find_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
		return entryHasValue(e, L"windir", expectedWindir);
	}), entries.end());
	EXPECT_EQ(entries.size(), 2u);
	EXPECT_EQ(std::find_if(entries.begin(), entries.end(), [](const std::wstring& e) {
		return e.starts_with(L"B6D_MARKER=");
	}), entries.end());
}

TEST(SenpChildJob, ChildHandlesReportNoInheritFlagUntilEnabledForLaunch)
{
	// Mirrors the launchers' creation pattern: the child-side standard-stream
	// handles exist non-inheritable for the whole window between creation and
	// SetChildHandlesInheritable immediately before CreateProcessW.
	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, FALSE };
	HANDLE read = nullptr;
	HANDLE write = nullptr;
	ASSERT_TRUE(::CreatePipe(&read, &write, &security, 0));
	DWORD flags = 0;
	ASSERT_TRUE(::GetHandleInformation(write, &flags));
	EXPECT_EQ(flags & HANDLE_FLAG_INHERIT, 0u);
	HANDLE nul = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(nul, nullptr);
	ASSERT_NE(nul, INVALID_HANDLE_VALUE);
	ASSERT_TRUE(::GetHandleInformation(nul, &flags));
	EXPECT_EQ(flags & HANDLE_FLAG_INHERIT, 0u);

	const std::array<HANDLE, 2> childSide{ write, nul };
	ASSERT_TRUE(senp::detail::SetChildHandlesInheritable(childSide));
	ASSERT_TRUE(::GetHandleInformation(write, &flags));
	EXPECT_NE(flags & HANDLE_FLAG_INHERIT, 0u);
	ASSERT_TRUE(::GetHandleInformation(nul, &flags));
	EXPECT_NE(flags & HANDLE_FLAG_INHERIT, 0u);

	// The launchers close the child-side handles right after CreateProcessW;
	// SetHandleInformation on a handle outside the list must fail closed.
	EXPECT_FALSE(senp::detail::SetChildHandlesInheritable(std::array<HANDLE, 1>{ nullptr }));
	::CloseHandle(read);
	::CloseHandle(write);
	::CloseHandle(nul);
}

} // namespace
