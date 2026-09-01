/*!	@file
	@brief プロセス基底クラス

	@author aroka
	@date 2002/01/07 作成
	@date 2002/01/17 修正
*/
/*
	Copyright (C) 2002, aroka 新規作成
	Copyright (C) 2004, Moca
	Copyright (C) 2009, ryoji
	Copyright (C) 2018-2022, Sakura Editor Organization

	This source code is designed for sakura editor.
	Please contact the copyright holder to use this code for other purpose.
*/

#include "StdAfx.h"
#include "_main/CProcess.h"

#include <algorithm>

#include "util/module.h"
#include "env/CShareData.h"
#include "env/DLLSHAREDATA.h"
#include "config/app_constants.h"
#include "CSelectLang.h"

/*!
	@brief プロセス基底クラス
	
	@author aroka
	@date 2002/01/07
*/
CProcess::CProcess(
	HINSTANCE	hInstance,		//!< handle to process instance
	[[maybe_unused]] LPCWSTR		lpCmdLine		//!< pointer to command line
)
: m_hInstance( hInstance )
{
}

/*!
	@brief iniファイルパスを取得する
 */
std::filesystem::path CProcess::GetIniFileName() const
{
	if (m_cShareData.IsPrivateSettings()) {
		const DLLSHAREDATA *pShareData = &GetDllShareData();
		return pShareData->m_szPrivateIniFile.c_str();
	}
	return GetExeFileName().replace_extension(L".ini");
}

std::optional<std::filesystem::path> CProcess::TryGetResolvedProfileDirectory() const
{
	const auto* const shareData = m_cShareData.GetDllShareDataPtr();
	if (!shareData) {
		return std::nullopt;
	}

	const auto& frozenIniFile = shareData->m_szPrivateIniFile;
	const auto* const begin = frozenIniFile.data();
	const auto* const end = begin + frozenIniFile.BUFFER_COUNT;
	const auto terminator = std::find(begin, end, L'\0');
	if (terminator == begin || terminator == end) {
		return std::nullopt;
	}

	// A full buffer cannot distinguish an intentional boundary-length string from
	// StaticString truncation, so reject it rather than using a partial anchor.
	if (terminator == end - 1) {
		return std::nullopt;
	}

	const std::filesystem::path frozenIniPath{ std::wstring_view{ begin, static_cast<size_t>(terminator - begin) } };
	if (!frozenIniPath.is_absolute() || frozenIniPath.filename().empty() ||
		frozenIniPath.filename() == L"." || frozenIniPath.filename() == L"..") {
		return std::nullopt;
	}
	for (const auto& component : frozenIniPath) {
		if (component == L"." || component == L"..") {
			return std::nullopt;
		}
	}

	const auto directory = frozenIniPath.parent_path().lexically_normal();
	if (directory.empty() || !directory.is_absolute()) {
		return std::nullopt;
	}
	return directory;
}

/*!
	@brief プロセスを初期化する

	共有メモリを初期化する
*/
bool CProcess::InitializeProcess()
{
	/* 共有データ構造体のアドレスを返す */
	if( !GetShareData().InitShareData() ){
		//	適切なデータを得られなかった
		::MYMESSAGEBOX( nullptr, MB_OK | MB_ICONERROR,
			GSTR_APPNAME, L"異なるバージョンのエディタを同時に起動することはできません。" );
		return false;
	}

	/* リソースから製品バージョンの取得 */
	//	2004.05.13 Moca 共有データのバージョン情報はコントロールプロセスだけが
	//	ShareDataで設定するように変更したのでここからは削除

	return true;
}

/*!
	@brief プロセス実行
	
	@author aroka
	@date 2002/01/16
*/
DWORD CProcess::Run()
{
	if( InitializeProcess() )
	{
			MainLoop() ;
			OnExitProcess();
		return ERROR_SUCCESS;
	}
	return StartupFailureExitCode();
}

DWORD CProcess::StartupFailureExitCode() const noexcept
{
	return ERROR_PROCESS_ABORTED;
}

/*!
	言語選択後に共有メモリ内の文字列を更新する
*/
void CProcess::RefreshString()
{
	m_cShareData.RefreshString();
}
