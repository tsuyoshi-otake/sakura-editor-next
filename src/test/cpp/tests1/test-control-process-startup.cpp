/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "_main/CControlProcess.h"
#include "_main/CProcessFactory.h"
#include "platform/controlipc/ControlPlatformRuntime.h"

namespace {

TEST(ControlProcessStartup, WaitResultDistinguishesReadyExitTimeoutAndWaitFailure)
{
	EXPECT_EQ(EControlProcessStartupWaitOutcome::Ready,
		CProcessFactory::ClassifyStartupWaitResult(WAIT_OBJECT_0));
	EXPECT_EQ(EControlProcessStartupWaitOutcome::ChildExited,
		CProcessFactory::ClassifyStartupWaitResult(WAIT_OBJECT_0 + 1));
	EXPECT_EQ(EControlProcessStartupWaitOutcome::TimedOut,
		CProcessFactory::ClassifyStartupWaitResult(WAIT_TIMEOUT));
	EXPECT_EQ(EControlProcessStartupWaitOutcome::WaitFailed,
		CProcessFactory::ClassifyStartupWaitResult(WAIT_FAILED));
}

TEST(ControlProcessStartup, MapsGenerationRollbackToAStableLauncherExitCode)
{
	platform::controlipc::ControlPlatformRuntimeResult result;
	result.code = platform::controlipc::EControlPlatformRuntimeResultCode::StorageOpenFailed;
	result.storageOpenResult = platform::storage::StorageAuthorityOpenResult{
		platform::storage::EStorageAuthorityOpenStatus::GenerationRollback,
		"profile authority generation is behind durable storage", true };

	EXPECT_EQ(static_cast<DWORD>(EControlProcessStartupExitCode::ControlPlatformStorageGenerationRollback),
		CControlProcess::StartupExitCodeFor(result));
}

TEST(ControlProcessStartup, GenerationRollbackMessageExplainsNonDestructiveRecovery)
{
	const auto message = CControlProcess::StartupFailureMessage(
		static_cast<DWORD>(EControlProcessStartupExitCode::ControlPlatformStorageGenerationRollback));

	EXPECT_NE(std::wstring_view::npos, message.find(L".sakura-platform"));
	EXPECT_NE(std::wstring_view::npos, message.find(L"storage-v1.bin"));
	EXPECT_NE(std::wstring_view::npos, message.find(L"sakura.ini"));
}

constexpr auto kStillInUse = static_cast<DWORD>(EControlProcessStartupExitCode::ControlSharedDataStillInUse);

TEST(ControlProcessStartup, OnlyTheSharedDataStillInUseExitIsRetried)
{
	EXPECT_TRUE(CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 1, 0).has_value());
	for (const DWORD exitCode : {
		static_cast<DWORD>(EControlProcessStartupExitCode::InitializationFailed),
		static_cast<DWORD>(EControlProcessStartupExitCode::ControlPlatformFailed),
		static_cast<DWORD>(EControlProcessStartupExitCode::ControlPlatformStorageWriterBusy),
		static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION),
		DWORD{ 0 } }) {
		EXPECT_FALSE(CProcessFactory::ControlStartRetryDelayMs(exitCode, 1, 0).has_value()) << exitCode;
	}
}

TEST(ControlProcessStartup, SharedDataRetryDoublesItsDelayAndStopsAtTheAttemptBudget)
{
	static_assert(CProcessFactory::kMaximumControlStartAttempts == 4);
	EXPECT_EQ(DWORD{ 250 }, CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 1, 0));
	EXPECT_EQ(DWORD{ 500 }, CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 2, 0));
	EXPECT_EQ(DWORD{ 1000 }, CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 3, 0));
	EXPECT_FALSE(CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 4, 0).has_value());
	EXPECT_FALSE(CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 0, 0).has_value());
}

TEST(ControlProcessStartup, SharedDataRetryJitterStaysWithinHalfTheBase)
{
	for (const unsigned int jitter : { 0u, 1u, 125u, 126u, 0xFFFFFFFFu }) {
		const auto delay = CProcessFactory::ControlStartRetryDelayMs(kStillInUse, 1, jitter);
		ASSERT_TRUE(delay.has_value());
		EXPECT_GE(*delay, DWORD{ 250 });
		EXPECT_LE(*delay, DWORD{ 375 });
	}
}

TEST(ControlProcessStartup, SharedDataStillInUseMessageAsksToWaitNotToDeleteData)
{
	const auto message = CControlProcess::StartupFailureMessage(kStillInUse);
	// "still shutting down" (shuuryou shori-chuu)
	EXPECT_NE(std::wstring_view::npos, message.find(L"\u7D42\u4E86\u51E6\u7406\u4E2D"));
	EXPECT_NE(std::wstring_view::npos, message.find(L"sakura.exe"));
}

} // namespace
