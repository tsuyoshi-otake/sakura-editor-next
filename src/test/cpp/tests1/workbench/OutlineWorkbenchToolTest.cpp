/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "outline/CDlgFuncList.h"
#include "outline/CFuncInfo.h"
#include "outline/CFuncInfoArr.h"
#include "workbench/outline/COutlineWorkbenchTool.h"

namespace workbench::outline {
namespace {

TEST(OutlineWorkbenchTool, LifecycleHasExplicitTerminalState)
{
	auto state = OutlineToolLifecycle::Idle;
	EXPECT_EQ( OutlineToolLifecycle::Idle,
		AdvanceOutlineToolLifecycle(state, OutlineToolEvent::Activate) );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Create );
	EXPECT_EQ( OutlineToolLifecycle::Inactive, state );
	EXPECT_EQ( OutlineToolLifecycle::Inactive,
		AdvanceOutlineToolLifecycle(state, OutlineToolEvent::Create) );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Activate );
	EXPECT_EQ( OutlineToolLifecycle::Active, state );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Deactivate );
	EXPECT_EQ( OutlineToolLifecycle::Inactive, state );
	state = AdvanceOutlineToolLifecycle( state, OutlineToolEvent::Close );
	EXPECT_EQ( OutlineToolLifecycle::Closed, state );
	for( const auto event : { OutlineToolEvent::Create, OutlineToolEvent::Activate,
		OutlineToolEvent::Deactivate, OutlineToolEvent::Close } ) {
		EXPECT_EQ( OutlineToolLifecycle::Closed, AdvanceOutlineToolLifecycle(state, event) );
	}
}

TEST(OutlineWorkbenchTool, GeometryClampsInvalidBoundsAndDefaultsDpi)
{
	const auto layout = NormalizeOutlineToolLayout( RECT{ -20, -10, -40, -30 }, 0 );
	EXPECT_EQ( (OutlineToolLayout{ RECT{ 0, 0, 0, 0 }, 96 }), layout );
}

TEST(OutlineWorkbenchTool, GeometryPreservesHostCoordinatesAtPerMonitorDpi)
{
	const RECT expected{ 3, 31, 403, 731 };
	const auto layout = NormalizeOutlineToolLayout( expected, 192 );
	EXPECT_EQ( (OutlineToolLayout{ expected, 192 }), layout );
}

TEST(OutlineWorkbenchTool, CreatedInactiveDialogIsVisibleWithoutActivation)
{
	// A SHOW_RELOAD can create the child after the visible host's previous layout.
	// Visibility is independent from keyboard activation so editor focus is retained.
	EXPECT_FALSE( ShouldShowOutlineDialog(OutlineToolLifecycle::Idle) );
	EXPECT_TRUE( ShouldShowOutlineDialog(OutlineToolLifecycle::Inactive) );
	EXPECT_TRUE( ShouldShowOutlineDialog(OutlineToolLifecycle::Active) );
	EXPECT_FALSE( ShouldShowOutlineDialog(OutlineToolLifecycle::Closed) );
}

TEST(OutlineWorkbenchTool, ReloadRelayoutRequiresSuccessfulVisibleChildCreation)
{
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(false, true, true) );
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(true, false, true) );
	EXPECT_FALSE( ShouldRelayoutOutlineAfterReload(true, true, false) );
	EXPECT_TRUE( ShouldRelayoutOutlineAfterReload(true, true, true) );
}

TEST(OutlineWorkbenchTool, NestedExplorerViewUsesSideBarBackgroundForEverySurface)
{
	auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	palette.panel = { 0x10, 0x20, 0x30 };
	palette.sideBar = { 0x40, 0x50, 0x60 };
	EXPECT_EQ(palette.sideBar.ToColorRef(), OutlineBackgroundColor(palette));
}

TEST(OutlineWorkbenchTool, SymbolKindsUseCanonicalCodiconImageSlots)
{
	EXPECT_EQ(10, CDlgFuncList::WorkbenchSymbolImageCount());
	EXPECT_EQ(0, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_DEFINITION));
	EXPECT_EQ(1, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_DECLARE));
	EXPECT_EQ(2, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_FUNCTION));
	EXPECT_EQ(3, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_CLASS));
	EXPECT_EQ(4, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_STRUCT));
	EXPECT_EQ(4, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_UNION));
	EXPECT_EQ(5, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_ENUM));
	EXPECT_EQ(6, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_NAMESPACE));
	EXPECT_EQ(7, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_INTERFACE));
	EXPECT_EQ(8, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_GLOBAL));
	EXPECT_EQ(9, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_ELEMENT_MAX));
	EXPECT_EQ(2, CDlgFuncList::WorkbenchSymbolImageIndex(FL_OBJ_FUNCTION | FUNCINFO_NOCLIPTEXT));
}

TEST(OutlineWorkbenchTool, SymbolImageSlotsResolveToCodiconNames)
{
	EXPECT_EQ(L"symbol-misc", CDlgFuncList::WorkbenchSymbolCodiconName(0));
	EXPECT_EQ(L"symbol-method", CDlgFuncList::WorkbenchSymbolCodiconName(1));
	EXPECT_EQ(L"symbol-function", CDlgFuncList::WorkbenchSymbolCodiconName(2));
	EXPECT_EQ(L"symbol-class", CDlgFuncList::WorkbenchSymbolCodiconName(3));
	EXPECT_EQ(L"symbol-structure", CDlgFuncList::WorkbenchSymbolCodiconName(4));
	EXPECT_EQ(L"symbol-enum", CDlgFuncList::WorkbenchSymbolCodiconName(5));
	EXPECT_EQ(L"symbol-namespace", CDlgFuncList::WorkbenchSymbolCodiconName(6));
	EXPECT_EQ(L"symbol-interface", CDlgFuncList::WorkbenchSymbolCodiconName(7));
	EXPECT_EQ(L"symbol-variable", CDlgFuncList::WorkbenchSymbolCodiconName(8));
	EXPECT_EQ(L"symbol-property", CDlgFuncList::WorkbenchSymbolCodiconName(9));
	EXPECT_EQ(L"symbol-misc", CDlgFuncList::WorkbenchSymbolCodiconName(99));
}

} // namespace
} // namespace workbench::outline
