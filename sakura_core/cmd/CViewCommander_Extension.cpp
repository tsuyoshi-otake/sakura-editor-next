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

/*!	拡張（Open VSX）ViewContainerの表示

	workbench.view.extensions ViewContainerを表示する。VS Codeにおける
	workbench.view.extensionsそのものであり、Open VSX Marketplaceはここに存在する。

	@note このコマンドは表示のみを行い、非表示にはしない。VS Codeでも非表示は
		workbench.action.toggleSidebarVisibility（Activity Barクリック相当）が
		担う操作であり、workbench.view.*系のコマンドではない。
	@note 導入した拡張を「実行」する仕組みはまだ無い。
		このコマンドでできるのは取得と配置までである。
*/
void CViewCommander::Command_EXTENSION_LIST( void )
{
	GetEditWindow()->ShowExtensionsViewContainer();
}
