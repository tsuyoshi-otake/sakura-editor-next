/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

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

} // namespace
} // namespace workbench::outline
