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

} // namespace
