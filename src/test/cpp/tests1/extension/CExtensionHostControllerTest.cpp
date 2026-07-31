/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionHostController.h"

#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace {

class ScopedTemporaryDirectory final {
public:
	ScopedTemporaryDirectory()
	{
		static std::atomic_uint64_t sequence = 0;
		m_path = std::filesystem::temp_directory_path() /
			(L"sakura-extension-controller-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
				std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(++sequence));
		std::filesystem::create_directories(m_path);
	}
	~ScopedTemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}
	const std::filesystem::path& Get() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

bool InitializeController(CExtensionHostController& controller, const std::filesystem::path& profileDirectory,
	std::wstring& diagnostic)
{
	return controller.Initialize(profileDirectory, diagnostic);
}

bool StartChild(PROCESS_INFORMATION& child)
{
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	std::wstring command = L"cmd.exe /d /c \"timeout /t 30 /nobreak >nul\"";
	return ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
		nullptr, nullptr, &startup, &child) != FALSE;
}

struct ChildHandleCloser final {
	void operator()(PROCESS_INFORMATION* process) const noexcept
	{
		if (process->hThread) ::CloseHandle(process->hThread);
		if (process->hProcess) ::CloseHandle(process->hProcess);
	}
};

} // namespace

TEST(CExtensionHostController, RejectsInvalidPidAndIgnoresUnknownRelease)
{
	ScopedTemporaryDirectory profileDirectory;
	CExtensionHostController controller;
	std::wstring diagnostic;
	if (!InitializeController(controller, profileDirectory.Get(), diagnostic)) GTEST_SKIP() << diagnostic;

	EXPECT_FALSE(controller.AcquireLease(0));
	EXPECT_FALSE(controller.AcquireLease((std::numeric_limits<std::uint32_t>::max)()));
	controller.ReleaseLease(::GetCurrentProcessId());
	EXPECT_EQ(0u, controller.Snapshot().leaseOwnerCount);
}

TEST(CExtensionHostController, CountsCurrentProcessLeasesUntilTheFinalRelease)
{
	ScopedTemporaryDirectory profileDirectory;
	CExtensionHostController controller;
	std::wstring diagnostic;
	if (!InitializeController(controller, profileDirectory.Get(), diagnostic)) GTEST_SKIP() << diagnostic;
	const auto processId = ::GetCurrentProcessId();

	ASSERT_TRUE(controller.AcquireLease(processId));
	ASSERT_TRUE(controller.AcquireLease(processId));
	EXPECT_EQ(1u, controller.Snapshot().leaseOwnerCount);

	controller.ReleaseLease(processId);
	EXPECT_EQ(1u, controller.Snapshot().leaseOwnerCount);
	controller.ReleaseLease(processId);
	EXPECT_EQ(0u, controller.Snapshot().leaseOwnerCount);

	controller.ReleaseLease(processId);
	EXPECT_EQ(0u, controller.Snapshot().leaseOwnerCount);
}

TEST(CExtensionHostController, ReclaimsEveryLeaseForATerminatedPinnedProcess)
{
	PROCESS_INFORMATION child{};
	ASSERT_TRUE(StartChild(child));
	const std::unique_ptr<PROCESS_INFORMATION, ChildHandleCloser> closeChildHandles(&child);
	ScopedTemporaryDirectory profileDirectory;
	CExtensionHostController controller;
	std::wstring diagnostic;
	if (!InitializeController(controller, profileDirectory.Get(), diagnostic)) GTEST_SKIP() << diagnostic;

	ASSERT_TRUE(controller.AcquireLease(child.dwProcessId));
	ASSERT_TRUE(controller.AcquireLease(child.dwProcessId));
	ASSERT_EQ(1u, controller.Snapshot().leaseOwnerCount);
	ASSERT_TRUE(::TerminateProcess(child.hProcess, ERROR_PROCESS_ABORTED));
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(child.hProcess, 5000));

	controller.Tick();
	EXPECT_EQ(0u, controller.Snapshot().leaseOwnerCount);
}
