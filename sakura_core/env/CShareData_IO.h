/*! @file */
/*
	Copyright (C) 2008, kobake
	Copyright (C) 2018-2022, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CSHAREDATA_IO_B154E0E0_C606_468E_A3B7_767DDA1DE6EE_H_
#define SAKURA_CSHAREDATA_IO_B154E0E0_C606_468E_A3B7_767DDA1DE6EE_H_
#pragma once
#include <cstdint>

class CDataProfile;
class CMenuDrawer;
struct CommonSetting_CustomMenu; // defined CommonSetting.h
struct CommonSetting_MainMenu; // defined CommonSetting.h
struct CommonSetting_KeyBind;	// defined CommonSetting.h
struct ColorInfo; // defined doc/CDocTypeSetting.h
struct SFileTree;
struct SFileTreeItem;
struct STypeConfig;

// 2008.XX.XX kobake CShareDataから分離
// 2008.05.24 Uchi   ShareData_IO_CustMenu, ShareData_IO_KeyBind  move Export、Importに使用
// 2010.08.21 Moca アクセス権、関数名の整理
class CShareData_IO{
public:
	//セーブ・ロード
	static bool LoadShareData();	/* 共有データのロード */
	static void SaveShareData();	/* 共有データの保存 */

	// Pure policy seam for the main-menu migration. A missing persisted version
	// is historically version zero: the current resource is the base, then the
	// duplicate-guarded additive migrations fill resource omissions.
	[[nodiscard]] static int ResolveMainMenuReadVersion(
		bool isReadingMode, bool hasStoredVersion, int storedVersion, int currentVersion ) noexcept;

	// Version 6 migration.  VS Code's File menu exposes a single "Open Recent"
	// submenu holding recent folders, a separator, then recent files, so the two
	// legacy sibling submenus are merged in place.  Only the exact legacy four
	// entry run is rewritten; a customized menu is left untouched.  Returns true
	// when the run was found and rewritten.
	[[nodiscard]] static bool MergeMainMenuOpenRecent( CommonSetting_MainMenu& mainmenu ) noexcept;

	//! Version 8 migration. Replaces the File run only when the complete
	//! persisted model is the known unmodified version-7 default.
	[[nodiscard]] static bool MigrateMainMenuV7DefaultToV8( CommonSetting_MainMenu& mainmenu ) noexcept;

	//! Replays historical migrations on a copy and commits the v8 replacement
	//! only when that copy is the complete known default for storedVersion.
	//! Customized persisted menus therefore remain byte-for-byte unchanged.
	[[nodiscard]] static bool MigrateKnownMainMenuDefaultToV8(
		CommonSetting_MainMenu& mainmenu, int storedVersion ) noexcept;

	//! Canonical fingerprint for the persisted menu model. This intentionally
	//! includes the complete model rather than only the File-menu run so a
	//! customization anywhere prevents a destructive schema migration.
	[[nodiscard]] static std::uint64_t MainMenuModelFingerprint( const CommonSetting_MainMenu& mainmenu ) noexcept;

	//! Adds one versioned leaf after an existing menu item, without duplicating
	//! an already-present command.  This pure migration seam keeps additive
	//! menu upgrades idempotent and is used by ShareData_IO_MainMenu.
	[[nodiscard]] static bool AddMainMenuItemIfMissing(
		CommonSetting_MainMenu& mainmenu,
		int addFuncCode,
		int prevFuncCode,
		wchar_t accessKey,
		bool addPreviousSeparator,
		bool addNextSeparator) noexcept;

	//! Applies the pre-v8 duplicate-guarded main-menu migrations for a persisted
	//! schema version. This is intentionally separate from the destructive v8
	//! replacement so current resources can still receive historical additions.
	static void ApplyMainMenuHistoricalAdditions(
		CommonSetting_MainMenu& mainmenu, int storedVersion) noexcept;

protected:
	static bool ShareData_IO_2( bool bRead );	/* 共有データの保存 */

	// Feb. 12, 2006 D.S.Koba
	static void ShareData_IO_Mru( CDataProfile& cProfile );
	static void ShareData_IO_Keys( CDataProfile& cProfile );
	static void ShareData_IO_Grep( CDataProfile& cProfile );
	static void ShareData_IO_Folders( CDataProfile& cProfile );
	static void ShareData_IO_Cmd( CDataProfile& cProfile );
	static void ShareData_IO_Nickname( CDataProfile& cProfile );
	static void ShareData_IO_Common( CDataProfile& cProfile );
	static void ShareData_IO_Toolbar( CDataProfile& cProfile, CMenuDrawer* pcMenuDrawer );
	static void ShareData_IO_CustMenu( CDataProfile& cProfile );
	static void ShareData_IO_Font( CDataProfile& cProfile );
	static void ShareData_IO_KeyBind( CDataProfile& cProfile );
	static void ShareData_IO_Print( CDataProfile& cProfile );
	static void ShareData_IO_Types( CDataProfile& cProfile );
	static void ShareData_IO_KeyWords( CDataProfile& cProfile );
	static void ShareData_IO_Macro( CDataProfile& cProfile );
	static void ShareData_IO_Statusbar( CDataProfile& cProfile );	// 2008/6/21 Uchi
	static void ShareData_IO_Plugin( CDataProfile& cProfile, CMenuDrawer* pcMenuDrawer );	// 2009/11/30 syat
	static void ShareData_IO_MainMenu( CDataProfile& cProfile );		// 2010/5/15 Uchi
	static void ShareData_IO_Other( CDataProfile& cProfile );

public:
	static void ShareData_IO_FileTree( CDataProfile& cProfile, SFileTree& fileTree, const WCHAR* pszSecName );
	static void ShareData_IO_FileTreeItem( CDataProfile& cProfile, SFileTreeItem& item,
										   const WCHAR* pszSecName, int i );
	static void ShareData_IO_Type_One( CDataProfile& cProfile, STypeConfig& types, const WCHAR* pszSecName);	// 2010/04/12 Uchi 分離

public:
	static void IO_CustMenu( CDataProfile& cProfile, CommonSetting_CustomMenu& menu, bool bOutCmdName);
	static void IO_KeyBind( CDataProfile& cProfile, CommonSetting_KeyBind& sKeyBind, bool bOutCmdName);		// 2012.11.22 aroka
	static void IO_MainMenu( CDataProfile& c, CommonSetting_MainMenu& s, bool b ){		// 2010/5/15 Uchi
		IO_MainMenu(c, nullptr, s, b);
	}
	static void IO_MainMenu( CDataProfile& cProfile, std::vector<std::wstring>* pData,
		CommonSetting_MainMenu& mainmenu, bool bOutCmdName);
	static void IO_ColorSet( CDataProfile* pcProfile, const WCHAR* pszSecName, ColorInfo* pColorInfoArr );	/* 色設定 I/O */ // Feb. 12, 2006 D.S.Koba
};
#endif /* SAKURA_CSHAREDATA_IO_B154E0E0_C606_468E_A3B7_767DDA1DE6EE_H_ */
