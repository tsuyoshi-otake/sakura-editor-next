/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <gtest/gtest.h>

#include "platform/process/BoundedProcessRunner.h"

#include <array>
#include <charconv>
#include <string>

namespace platform::process {
namespace {

constexpr wchar_t kChildRoleName[] = L"SAKURA_BOUNDED_PROCESS_TEST_ROLE";

std::wstring CurrentExecutable()
{
	std::wstring path(32768, L'\0');
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	return path;
}

std::wstring CurrentDirectory()
{
	std::wstring path(32768, L'\0');
	const DWORD length = ::GetCurrentDirectoryW(static_cast<DWORD>(path.size()), path.data());
	path.resize(length);
	return path;
}

BoundedProcessRequest ChildRequest(std::wstring role)
{
	BoundedProcessRequest request(CurrentExecutable(), CurrentDirectory(), {
		L"--gtest_filter=BoundedProcessRunnerChild.ExercisesRequestedBoundary",
		L"--gtest_color=no",
	});
	request.SetEnvironmentOverrides({ { kChildRoleName, std::move(role) } });
	request.SetTimeoutMilliseconds(3000);
	return request;
}

std::string Bytes(const std::vector<std::uint8_t>& value)
{
	if (value.empty()) return {};
	return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void WriteAll(HANDLE handle, std::string_view value)
{
	DWORD written{};
	ASSERT_TRUE(::WriteFile(handle, value.data(), static_cast<DWORD>(value.size()), &written, nullptr));
	ASSERT_EQ(value.size(), written);
}

} // namespace

TEST(BoundedProcessRunnerChild, ExercisesRequestedBoundary)
{
	std::array<wchar_t, 32> role{};
	const DWORD length = ::GetEnvironmentVariableW(kChildRoleName, role.data(), static_cast<DWORD>(role.size()));
	if (length == 0) return;
	const std::wstring_view selected(role.data(), length);
	if (selected == L"echo") {
		std::array<char, 64> input{};
		DWORD read{};
		ASSERT_TRUE(::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), input.data(), static_cast<DWORD>(input.size()), &read, nullptr));
		WriteAll(::GetStdHandle(STD_OUTPUT_HANDLE), "stdout:" + std::string(input.data(), read));
		WriteAll(::GetStdHandle(STD_ERROR_HANDLE), "stderr:owned-environment");
		return;
	}
	if (selected == L"pump") {
		std::array<char, 16384> input{};
		for (;;) {
			DWORD read{};
			if (!::ReadFile(::GetStdHandle(STD_INPUT_HANDLE), input.data(),
				static_cast<DWORD>(input.size()), &read, nullptr) || read == 0) break;
			WriteAll(::GetStdHandle(STD_OUTPUT_HANDLE), std::string_view(input.data(), read));
		}
		return;
	}
	if (selected == L"stdout-limit") {
		WriteAll(::GetStdHandle(STD_OUTPUT_HANDLE), std::string(65536, 'o'));
		return;
	}
	if (selected == L"stderr-limit") {
		WriteAll(::GetStdHandle(STD_ERROR_HANDLE), std::string(65536, 'e'));
		return;
	}
	if (selected == L"failed") ::ExitProcess(7);
	if (selected == L"spawn") {
		ASSERT_TRUE(::SetEnvironmentVariableW(kChildRoleName, L"sleep"));
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION process{};
		auto executable = CurrentExecutable();
		auto commandLine = BuildWindowsCommandLine(executable, {
			L"--gtest_filter=BoundedProcessRunnerChild.ExercisesRequestedBoundary", L"--gtest_color=no" });
		ASSERT_TRUE(::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, CurrentDirectory().c_str(), &startup, &process));
		const std::string marker = "grandchild-pid=" + std::to_string(process.dwProcessId) + "\n";
		WriteAll(::GetStdHandle(STD_OUTPUT_HANDLE), marker);
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
	}
	::Sleep(10000);
}

TEST(BoundedProcessRunner, QuotesArgumentsForCommandLineToArgvW)
{
	EXPECT_EQ(L"plain", QuoteWindowsArgument(L"plain"));
	EXPECT_EQ(L"\"\"", QuoteWindowsArgument(L""));
	EXPECT_EQ(L"\"two words\"", QuoteWindowsArgument(L"two words"));
	EXPECT_EQ(L"\"a\\\"b\"", QuoteWindowsArgument(L"a\"b"));
	EXPECT_EQ(L"\"C:\\space here\\\\\"", QuoteWindowsArgument(L"C:\\space here\\"));
}

TEST(BoundedProcessRunner, CapturesSeparatedStreamsAndClosesStandardInput)
{
	auto request = ChildRequest(L"echo");
	request.SetStandardInput("payload");
	const auto result = RunBoundedProcess(request, nullptr);

	ASSERT_EQ(EBoundedProcessStatus::Succeeded, result.Status());
	EXPECT_EQ(0, result.ExitCode());
	EXPECT_NE(std::string::npos, Bytes(result.StandardOutput()).find("stdout:payload"));
	EXPECT_NE(std::string::npos, Bytes(result.StandardError()).find("stderr:owned-environment"));
}

TEST(BoundedProcessRunner, StreamsInputLargerThanThePipeWithoutDeadlock)
{
	auto request = ChildRequest(L"pump");
	const std::string payload(256u * 1024u, 'p');
	request.SetStandardInput(payload);
	const auto result = RunBoundedProcess(request, nullptr);

	ASSERT_EQ(EBoundedProcessStatus::Succeeded, result.Status());
	EXPECT_NE(std::string::npos, Bytes(result.StandardOutput()).find(payload));
}

TEST(BoundedProcessRunner, TerminatesOnEitherStreamBudget)
{
	for (const auto role : { L"stdout-limit", L"stderr-limit" }) {
		auto request = ChildRequest(role);
		request.SetMaximumStandardOutputBytes(1024);
		request.SetMaximumStandardErrorBytes(1024);
		const auto result = RunBoundedProcess(request, nullptr);
		EXPECT_EQ(EBoundedProcessStatus::OutputLimitExceeded, result.Status()) << Bytes(result.StandardError());
	}
}

TEST(BoundedProcessRunner, ReportsNonzeroExitCode)
{
	const auto result = RunBoundedProcess(ChildRequest(L"failed"), nullptr);
	EXPECT_EQ(EBoundedProcessStatus::Failed, result.Status());
	EXPECT_EQ(7, result.ExitCode());
}

TEST(BoundedProcessRunner, DistinguishesTimeoutAndCancellation)
{
	auto timeoutRequest = ChildRequest(L"sleep");
	timeoutRequest.SetTimeoutMilliseconds(100);
	EXPECT_EQ(EBoundedProcessStatus::TimedOut, RunBoundedProcess(timeoutRequest, nullptr).Status());

	HANDLE stop = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
	ASSERT_NE(nullptr, stop);
	auto cancelRequest = ChildRequest(L"sleep");
	const auto cancelled = RunBoundedProcess(cancelRequest, stop);
	::CloseHandle(stop);
	EXPECT_EQ(EBoundedProcessStatus::Cancelled, cancelled.Status());
}

TEST(BoundedProcessRunner, TimeoutOwnsAndTerminatesTheChildProcessTree)
{
	auto request = ChildRequest(L"spawn");
	request.SetTimeoutMilliseconds(1800);
	const auto result = RunBoundedProcess(request, nullptr);
	ASSERT_EQ(EBoundedProcessStatus::TimedOut, result.Status());

	const auto output = Bytes(result.StandardOutput());
	const auto marker = output.find("grandchild-pid=");
	ASSERT_NE(std::string::npos, marker) << output;
	const auto begin = marker + std::string("grandchild-pid=").size();
	DWORD processId{};
	const auto conversion = std::from_chars(output.data() + begin, output.data() + output.size(), processId);
	ASSERT_EQ(std::errc{}, conversion.ec);
	HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, processId);
	if (process != nullptr) {
		EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(process, 1000));
		::CloseHandle(process);
	}
}

TEST(BoundedProcessRunner, RejectsMalformedRequestsBeforeLaunch)
{
	auto request = ChildRequest(L"echo");
	EXPECT_TRUE(IsExecutableBoundedProcessRequest(request));
	request.SetEnvironmentOverrides({ { L"BAD=NAME", L"value" } });
	EXPECT_FALSE(IsExecutableBoundedProcessRequest(request));
	EXPECT_EQ(EBoundedProcessStatus::InvalidRequest, RunBoundedProcess(request, nullptr).Status());

	BoundedProcessRequest missing(L"C:\\missing\\not-a-tool.exe", CurrentDirectory(), { L"arg" });
	EXPECT_EQ(EBoundedProcessStatus::LaunchFailed, RunBoundedProcess(missing, nullptr).Status());
}

} // namespace platform::process
