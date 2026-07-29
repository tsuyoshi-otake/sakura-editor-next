/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/outline/COutlineWorkbenchTool.h"

#include "outline/CDlgFuncList.h"

#include <algorithm>

namespace workbench::outline {

OutlineToolLifecycle AdvanceOutlineToolLifecycle(
	OutlineToolLifecycle current,
	OutlineToolEvent event ) noexcept
{
	if( current == OutlineToolLifecycle::Closed ) return current;
	if( event == OutlineToolEvent::Close ) return OutlineToolLifecycle::Closed;

	switch( current ) {
	case OutlineToolLifecycle::Idle:
		return event == OutlineToolEvent::Create ? OutlineToolLifecycle::Inactive : current;
	case OutlineToolLifecycle::Inactive:
		return event == OutlineToolEvent::Activate ? OutlineToolLifecycle::Active : current;
	case OutlineToolLifecycle::Active:
		return event == OutlineToolEvent::Deactivate ? OutlineToolLifecycle::Inactive : current;
	case OutlineToolLifecycle::Closed:
		break;
	}
	return current;
}

OutlineToolLayout NormalizeOutlineToolLayout( const RECT& contentRect, unsigned int dpi ) noexcept
{
	OutlineToolLayout layout;
	layout.bounds.left = std::max<LONG>( 0, contentRect.left );
	layout.bounds.top = std::max<LONG>( 0, contentRect.top );
	layout.bounds.right = std::max( layout.bounds.left, contentRect.right );
	layout.bounds.bottom = std::max( layout.bounds.top, contentRect.bottom );
	layout.dpi = dpi == 0 ? 96 : dpi;
	return layout;
}

bool ShouldShowOutlineDialog( OutlineToolLifecycle lifecycle ) noexcept
{
	return lifecycle == OutlineToolLifecycle::Inactive || lifecycle == OutlineToolLifecycle::Active;
}

bool ShouldRelayoutOutlineAfterReload(
	bool commandSucceeded,
	bool rightPanelVisible,
	bool dialogCreated ) noexcept
{
	return commandSucceeded && rightPanelVisible && dialogCreated;
}

COutlineWorkbenchTool::COutlineWorkbenchTool( CDlgFuncList& dialog ) noexcept
	: m_dialog(&dialog)
{
}

COutlineWorkbenchTool::~COutlineWorkbenchTool()
{
	Close();
}

bool COutlineWorkbenchTool::Create( HWND parent )
{
	if( m_dialog == nullptr || parent == nullptr || m_lifecycle == OutlineToolLifecycle::Closed ) return false;
	if( m_lifecycle != OutlineToolLifecycle::Idle ) return parent == m_parent;

	m_parent = parent;
	m_dialog->SetWorkbenchParent( parent );
	m_dialog->SetWorkbenchMode( true );
	m_lifecycle = AdvanceOutlineToolLifecycle( m_lifecycle, OutlineToolEvent::Create );
	return true;
}

void COutlineWorkbenchTool::Layout( const RECT& contentRect, unsigned int dpi )
{
	if( m_lifecycle == OutlineToolLifecycle::Closed ) return;
	m_layout = NormalizeOutlineToolLayout( contentRect, dpi );
	ApplyLayout();
}

void COutlineWorkbenchTool::Activate()
{
	if( m_lifecycle == OutlineToolLifecycle::Idle || m_lifecycle == OutlineToolLifecycle::Closed ) return;
	m_lifecycle = AdvanceOutlineToolLifecycle( m_lifecycle, OutlineToolEvent::Activate );
	ApplyLayout();
	const HWND window = GetDialogWindow();
	if( window == nullptr ) return;
	HWND focusTarget = ::GetNextDlgTabItem( window, nullptr, FALSE );
	if( focusTarget != nullptr
		&& (!::IsWindowVisible(focusTarget) || !::IsWindowEnabled(focusTarget)) ) {
		focusTarget = nullptr;
	}
	::SetFocus( focusTarget != nullptr ? focusTarget : window );
}

void COutlineWorkbenchTool::Deactivate()
{
	if( m_lifecycle == OutlineToolLifecycle::Closed ) return;
	m_lifecycle = AdvanceOutlineToolLifecycle( m_lifecycle, OutlineToolEvent::Deactivate );
}

bool COutlineWorkbenchTool::PreTranslateMessage( MSG& message )
{
	if( m_lifecycle != OutlineToolLifecycle::Active ) return false;
	const HWND window = GetDialogWindow();
	if( window == nullptr || (message.hwnd != window && !::IsChild(window, message.hwnd)) ) return false;
	return ::IsDialogMessageW( window, &message ) != FALSE;
}

void COutlineWorkbenchTool::Close()
{
	if( m_lifecycle == OutlineToolLifecycle::Closed ) return;
	const HWND window = GetDialogWindow();
	if( window != nullptr ) ::DestroyWindow( window );
	if( m_dialog != nullptr ) {
		m_dialog->SetWorkbenchMode( false );
		m_dialog->SetWorkbenchParent( nullptr );
	}
	m_parent = nullptr;
	m_lifecycle = AdvanceOutlineToolLifecycle( m_lifecycle, OutlineToolEvent::Close );
}

void COutlineWorkbenchTool::ApplyLayout() noexcept
{
	const HWND window = GetDialogWindow();
	if( window == nullptr ) return;
	const LONG width = m_layout.bounds.right - m_layout.bounds.left;
	const LONG height = m_layout.bounds.bottom - m_layout.bounds.top;
	::SetWindowPos( window, nullptr, m_layout.bounds.left, m_layout.bounds.top, width, height,
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER );
	// SHOW_RELOAD creates the child with SW_HIDE while the right panel is already
	// visible.  The outline must become visible on this layout pass without making
	// the panel active; Activate() remains the only focus-changing path.
	if( ShouldShowOutlineDialog(m_lifecycle) ) {
		::ShowWindow( window, SW_SHOWNA );
	}
}

HWND COutlineWorkbenchTool::GetDialogWindow() const noexcept
{
	if( m_dialog == nullptr ) return nullptr;
	const HWND window = m_dialog->GetHwnd();
	return window != nullptr && ::IsWindow(window) ? window : nullptr;
}

} // namespace workbench::outline
