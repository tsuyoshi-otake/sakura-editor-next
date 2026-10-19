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
	const HANDLE value = ::CreateJobObjectW(nullptr, nullptr);
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
		const std::wstring name;
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

TEST(SenpChildJob, LaunchedChildInheritsOnlyTheAllowlistedEnvironment)
{
	// A real child proves the scrub: the parent carries a credential-shaped
	// variable and a unique marker; the allowlisted block must forward
	// neither. The guard restores the original values on every exit path.
	class ScopedEnvironmentVariable final {
	public:
		explicit ScopedEnvironmentVariable(const wchar_t* name) : m_name(name)
		{
			std::array<wchar_t, 32768> buffer{};
			const DWORD length = ::GetEnvironmentVariableW(name, buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (length != 0 && length < buffer.size()) m_original.assign(buffer.data(), length);
		}
		~ScopedEnvironmentVariable()
		{
			::SetEnvironmentVariableW(m_name.c_str(),
				m_original.empty() ? nullptr : m_original.c_str());
		}
	private:
		const std::wstring m_name;
		std::wstring m_original;
	};
	ScopedEnvironmentVariable token{ L"GH_TOKEN" };
	ScopedEnvironmentVariable marker{ L"SAKURA_B2_LAUNCH_MARKER" };
	ASSERT_TRUE(::SetEnvironmentVariableW(L"GH_TOKEN", L"b2-token-value"));
	ASSERT_TRUE(::SetEnvironmentVariableW(L"SAKURA_B2_LAUNCH_MARKER", L"b2-marker-value"));

	// Resolve %SystemRoot%\System32\cmd.exe; the real child just runs `set`.
	std::array<wchar_t, 32768> root{};
	const DWORD rootLength = ::GetEnvironmentVariableW(L"SystemRoot", root.data(),
		static_cast<DWORD>(root.size()));
	ASSERT_GT(rootLength, 0u);
	ASSERT_LT(rootLength, root.size());
	const std::wstring executable = std::wstring(root.data(), rootLength) + L"\\System32\\cmd.exe";

	struct HandleGuard final {
		explicit HandleGuard(HANDLE& handle) : m_ref(handle) {}
		~HandleGuard() { if (m_ref != nullptr && m_ref != INVALID_HANDLE_VALUE) ::CloseHandle(m_ref); }
	private:
		HANDLE& m_ref;
	};
	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, FALSE };
	HANDLE outputRead = nullptr;
	HANDLE outputWrite = nullptr;
	ASSERT_TRUE(::CreatePipe(&outputRead, &outputWrite, &security, 0));
	HandleGuard outputReadGuard{ outputRead };
	HandleGuard outputWriteGuard{ outputWrite };
	HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(nullInput, nullptr);
	ASSERT_NE(nullInput, INVALID_HANDLE_VALUE);
	HandleGuard nullInputGuard{ nullInput };
	HANDLE nullError = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	ASSERT_NE(nullError, nullptr);
	ASSERT_NE(nullError, INVALID_HANDLE_VALUE);
	HandleGuard nullErrorGuard{ nullError };

	JobGuard job;
	ASSERT_NE(job.value, nullptr);
	ASSERT_TRUE(senp::ConfigureChildJob(job.value, senp::kExtensionHostJobLimits));

	std::wstring command = L"\"" + executable + L"\" /d /c set";
	const auto process = senp::detail::LaunchSenpChildProcess(executable, command,
		{ nullInput, outputWrite, nullError }, CREATE_NO_WINDOW, job.value);
	// The launchers close the child-side write end right after the call, so
	// the parent read end observes EOF once the child exits.
	::CloseHandle(outputWrite);
	outputWrite = nullptr;
	ASSERT_TRUE(process.has_value());
	::CloseHandle(process->hThread);
	struct ProcessGuard final {
		explicit ProcessGuard(HANDLE handle) : m_value(handle) {}
		~ProcessGuard() { (void)::TerminateProcess(m_value, ERROR_TIMEOUT); ::CloseHandle(m_value); }
	private:
		const HANDLE m_value;
	} processGuard{ process->hProcess };

	// Drain while waiting: `set` output under an inherited parent environment
	// (the mutation this test guards against) exceeds the pipe buffer, so a
	// plain wait-then-read would deadlock the child on a full pipe. Peek and
	// drain between 100 ms wait slices instead; the total wait stays bounded
	// at 30 s with TerminateProcess on timeout.
	std::string output;
	const auto drain = [&]() {
		DWORD available = 0;
		while (::PeekNamedPipe(outputRead, nullptr, 0, nullptr, &available, nullptr)
			&& available != 0) {
			std::array<char, 4096> buffer{};
			DWORD bytes = 0;
			if (!::ReadFile(outputRead, buffer.data(),
					std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &bytes, nullptr)
				|| bytes == 0) break;
			output.append(buffer.data(), bytes);
		}
	};
	const ULONGLONG deadline = ::GetTickCount64() + 30'000;
	DWORD waited;
	for (;;) {
		drain();
		waited = ::WaitForSingleObject(process->hProcess, 100);
		if (waited != WAIT_TIMEOUT || ::GetTickCount64() >= deadline) break;
	}
	drain();
	if (waited != WAIT_OBJECT_0) {
		(void)::TerminateProcess(process->hProcess, ERROR_TIMEOUT);
		(void)::WaitForSingleObject(process->hProcess, 1000);
	}
	ASSERT_EQ(waited, WAIT_OBJECT_0);

	// `set` echoes the stored names; the allowlist block canonicalizes them
	// uppercase for the case-insensitive ordering Windows requires, so the
	// search text is uppercased too. GH_TOKEN and the marker are already
	// uppercase names.
	std::string upper = output;
	std::ranges::transform(upper, upper.begin(),
		[](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
	EXPECT_NE(upper.find("SYSTEMROOT="), std::string::npos);
	EXPECT_NE(upper.find("WINDIR="), std::string::npos);
	EXPECT_EQ(upper.find("GH_TOKEN"), std::string::npos);
	EXPECT_EQ(upper.find("SAKURA_B2_LAUNCH_MARKER"), std::string::npos);
}

} // namespace
