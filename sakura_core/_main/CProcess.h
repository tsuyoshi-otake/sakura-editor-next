/*!	@file
	@brief プロセス基底クラスヘッダーファイル

	@author aroka
	@date	2002/01/08 作成
*/
/*
	Copyright (C) 2002, aroka 新規作成
	Copyright (C) 2009, ryoji
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#ifndef SAKURA_CPROCESS_FECC5450_9096_4EAD_A6DA_C8B12C3A31B5_H_
#define SAKURA_CPROCESS_FECC5450_9096_4EAD_A6DA_C8B12C3A31B5_H_
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "global.h"
#include "util/design_template.h"
#include "env/CShareData.h"

namespace terminal {
class CDefaultTerminalLaunchProfileService;
class CTerminalRuntimeService;
}

/*-----------------------------------------------------------------------
クラスの宣言
-----------------------------------------------------------------------*/
/*!
	@brief プロセス基底クラス
*/
class CProcess : public TSingleInstance<CProcess> {
public:
	CProcess( HINSTANCE hInstance, LPCWSTR lpCmdLine );
	DWORD Run();
	virtual ~CProcess(){}
	virtual void RefreshString();

	virtual std::filesystem::path GetIniFileName() const;
	// Returns the directory frozen in legacy shared data after the shared-data
	// mapping is attached.  This deliberately does not resolve profile options.
	[[nodiscard]] std::optional<std::filesystem::path> TryGetResolvedProfileDirectory() const;

protected:
	CProcess();
	virtual bool InitializeProcess();
	virtual bool MainLoop() = 0;
	virtual void OnExitProcess() = 0;
	//! Returned only when InitializeProcess() reaches a terminal failure. Derived
	//! processes may expose a more specific launcher-observable startup outcome.
	[[nodiscard]] virtual DWORD StartupFailureExitCode() const noexcept;

protected:
	void			SetMainWindow(HWND hwnd){ m_hWnd = hwnd; }

public:
	HINSTANCE		GetProcessInstance() const{ return m_hInstance; }
	CShareData&		GetShareData()   { return m_cShareData; }
	HWND			GetMainWindow() const{ return m_hWnd; }

	[[nodiscard]] const CShareData* GetShareDataPtr() const { return &m_cShareData; }
	[[nodiscard]] virtual std::shared_ptr<terminal::CTerminalRuntimeService>
		GetTerminalRuntimeService() const noexcept { return {}; }
	[[nodiscard]] virtual std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService>
		GetTerminalLaunchProfiles() const noexcept { return {}; }

private:
	HINSTANCE	m_hInstance;
	HWND		m_hWnd = nullptr;
	CShareData		m_cShareData;
};

#endif /* SAKURA_CPROCESS_FECC5450_9096_4EAD_A6DA_C8B12C3A31B5_H_ */
