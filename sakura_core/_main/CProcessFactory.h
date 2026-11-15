/*!	@file
	@brief プロセス生成クラスヘッダーファイル

	@author aroka
	@date	2002/01/08 作成
*/
/*
	Copyright (C) 2002, aroka 新規作成
	Copyright (C) 2006, ryoji
	Copyright (C) 2018-2026, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#ifndef SAKURA_CPROCESSFACTORY_5006562F_7795_40FF_AA4C_FFB94842F7C5_H_
#define SAKURA_CPROCESSFACTORY_5006562F_7795_40FF_AA4C_FFB94842F7C5_H_
#pragma once

#include "global.h"

#include <optional>

class CProcess;

enum class EControlProcessStartupWaitOutcome : unsigned char {
	Ready,
	ChildExited,
	TimedOut,
	WaitFailed,
};

/*-----------------------------------------------------------------------
クラスの宣言
-----------------------------------------------------------------------*/
/*!
	@brief プロセス生成クラス

	与えられたコマンドライン引数から生成すべきプロセスの種別を判定し，
	対応するオブジェクトを返すFactoryクラス．

	通常のエディタプロセスの起動が指定された場合には，必要に応じてコントロールプロセス
	起動の起動をエディタの起動に先立って行う．
*/
class CProcessFactory {
public:
	static bool	IsExistControlProcess();
	static bool	StartControlProcess();
	[[nodiscard]] static EControlProcessStartupWaitOutcome ClassifyStartupWaitResult(
		DWORD waitResult) noexcept;
	//! Total control-process start attempts, the first one included.
	static constexpr unsigned int kMaximumControlStartAttempts = 4;
	//! The wait before starting the control process again after attempt number
	//! `completedAttempts` (1-based) exited with `exitCode`, or nullopt when that
	//! exit is not transient or the attempt budget is spent. Only
	//! ControlSharedDataStillInUse is transient (#423). The delay doubles from
	//! 250 ms and adds `jitter` reduced to at most half of the base.
	[[nodiscard]] static std::optional<DWORD> ControlStartRetryDelayMs(
		DWORD exitCode, unsigned int completedAttempts, unsigned int jitter) noexcept;

	CProcess* Create( HINSTANCE hInstance, LPCWSTR lpCmdLine );

private:
	bool	IsValidVersion() const;
	bool	ProfileSelect(HINSTANCE hInstance, LPCWSTR lpCmdLine) const;
	void	MigrateVirtualStoreIniForSelectedProfile() const;
	bool	IsStartingControlProcess() const;
};

#endif /* SAKURA_CPROCESSFACTORY_5006562F_7795_40FF_AA4C_FFB94842F7C5_H_ */
