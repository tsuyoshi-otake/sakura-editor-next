/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpChildJob.h"
#include <array>
#include <string>

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

} // namespace
