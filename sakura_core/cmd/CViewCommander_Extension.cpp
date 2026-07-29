/*!	@file
	@brief CViewCommanderクラスのコマンド(拡張系)関数群

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "CViewCommander.h"
#include "CViewCommander_inline.h"

#include "window/CEditWnd.h"

/*!	拡張（Open VSX）サイドバーの表示切り替え

	@note 導入した拡張を「実行」する仕組みはまだ無い。
		このコマンドでできるのは取得と配置までである。
*/
void CViewCommander::Command_EXTENSION_LIST( void )
{
	GetEditWindow()->ToggleExtensionPane();
}
