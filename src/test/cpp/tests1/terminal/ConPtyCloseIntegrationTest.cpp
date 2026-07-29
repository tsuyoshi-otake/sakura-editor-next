/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/Windows11Platform.h"
#include "terminal/session/TerminalSession.h"

#include <chrono>

namespace {

using namespace std::chrono_literals;

bool IsConPtyUnavailableError( std::uint32_t errorCode )
{
	return errorCode == ERROR_CALL_NOT_IMPLEMENTED ||
		errorCode == ERROR_NOT_SUPPORTED ||
		errorCode == ERROR_PROC_NOT_FOUND;
}

TEST(ConPtyCloseIntegration, ImmediatelyClosingCmdSessionIsBoundedTerminalAndIdempotent)
{
	if( !platform::SupportsWindows11Features(platform::QueryWindowsBuild()) ) {
		GTEST_SKIP() << "ConPTY close integration test requires Windows 11.";
	}

	wchar_t systemDirectory[MAX_PATH]{};
	const UINT directoryLength = ::GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
	if( directoryLength == 0 || directoryLength >= std::size(systemDirectory) ) {
		GTEST_SKIP() << "cmd.exe system directory is unavailable.";
	}

	terminal::TerminalLaunchOptions options;
	options.executablePath.assign(systemDirectory, directoryLength);
	options.executablePath += L"\\cmd.exe";
	options.arguments = { L"/d", L"/q" };
	options.initialSize = { 80, 25 };

	terminal::CTerminalSession session(terminal::CreateConPtyTerminalBackend());
	const auto start = session.Start(options);
	if( !start.succeeded && IsConPtyUnavailableError(start.errorCode) ) {
		GTEST_SKIP() << "ConPTY is unavailable (error " << start.errorCode << ").";
	}
	ASSERT_TRUE(start.succeeded) << "ConPTY cmd.exe launch failed with " << start.errorCode;

	const auto closeStarted = std::chrono::steady_clock::now();
	session.Close();
	const auto closeElapsed = std::chrono::steady_clock::now() - closeStarted;
	EXPECT_LT(closeElapsed, 5s);
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());

	const auto repeatedCloseStarted = std::chrono::steady_clock::now();
	session.Close();
	EXPECT_LT(std::chrono::steady_clock::now() - repeatedCloseStarted, 5s);
	EXPECT_EQ(terminal::TerminalSessionState::Exited, session.GetState());
}

} // namespace
