/*!	@file
	@brief コントロールプロセスクラスヘッダーファイル

	@author aroka
	@date	2002/01/08 作成
*/
/*
	Copyright (C) 2002, aroka 新規作成, YAZAKI
	Copyright (C) 2006, ryoji
	Copyright (C) 2018-2026, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#ifndef SAKURA_CCONTROLPROCESS_AFB90808_4287_4A11_B7FB_9CD21CF8BFD6_H_
#define SAKURA_CCONTROLPROCESS_AFB90808_4287_4A11_B7FB_9CD21CF8BFD6_H_
#pragma once

#include <filesystem>
#include <memory>

#include "global.h"
#include "CProcess.h"

class CControlTray;

namespace platform::controlipc {
class CControlPlatformRuntime;
struct ControlPlatformRuntimeResult;
}

//! Stable launcher-observable terminal outcomes for control-process startup.
//! Values are kept in a private application range so they do not masquerade as
//! WaitForMultipleObjects or Win32 system errors.
enum class EControlProcessStartupExitCode : DWORD {
	InitializationFailed = ERROR_PROCESS_ABORTED,
	ControlPlatformFailed = 0x2000,
	ControlPlatformAuthorityFailed,
	ControlPlatformStorageCreateFailed,
	ControlPlatformStorageWriterBusy,
	ControlPlatformStorageIoError,
	ControlPlatformStorageCorruptData,
	ControlPlatformStorageUnsupportedFormat,
	ControlPlatformStorageGenerationRollback,
	ControlPlatformStorageOrphanedState,
};

/*-----------------------------------------------------------------------
クラスの宣言
-----------------------------------------------------------------------*/
/*!
	@brief コントロールプロセスクラス
	
	コントロールプロセスはCControlTrayクラスのインスタンスを作る。
	
	@date 2002.2.17 YAZAKI CShareDataのインスタンスは、CProcessにひとつあるのみ。
*/
class CControlProcess final : public CProcess {
public:
	CControlProcess(HINSTANCE hInstance, LPCWSTR lpCmdLine);

	~CControlProcess();

	std::filesystem::path GetIniFileName() const override;
	[[nodiscard]] static DWORD StartupExitCodeFor(
		const platform::controlipc::ControlPlatformRuntimeResult& result) noexcept;
	[[nodiscard]] static std::wstring_view StartupFailureMessage(DWORD exitCode) noexcept;

protected:
	CControlProcess();
	bool InitializeProcess() override;
	bool MainLoop() override;
	void OnExitProcess() override;
	[[nodiscard]] DWORD StartupFailureExitCode() const noexcept override;

private:
	std::filesystem::path GetPrivateIniFileName(const std::wstring& exeIniPath, const std::wstring& filename) const;
	bool StartControlPlatform();
	void StopControlPlatform() noexcept;
	//! Applies an update the user already asked for, on the way out. Never
	//! throws: process exit is not a place that can report a failure.
	void RunPendingUpdateInstaller() noexcept;

	HANDLE			m_hMutex = nullptr;					//!< アプリケーション実行検出用ミューテックス
	HANDLE			m_hMutexCP = nullptr;				//!< コントロールプロセスミューテックス
	CControlTray*	m_pcTray = nullptr;
	std::unique_ptr<platform::controlipc::CControlPlatformRuntime> m_controlPlatformRuntime;
	DWORD m_startupExitCode = static_cast<DWORD>(EControlProcessStartupExitCode::InitializationFailed);
	bool m_launcherOwnsStartupError = false;
};

#endif /* SAKURA_CCONTROLPROCESS_AFB90808_4287_4A11_B7FB_9CD21CF8BFD6_H_ */
