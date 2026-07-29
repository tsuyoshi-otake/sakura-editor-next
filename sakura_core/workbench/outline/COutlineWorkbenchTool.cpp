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
namespace {

constexpr unsigned int kDefaultDpi = 96;
constexpr int kOutlineRowHeightDip = 22;
constexpr int kSymbolImageSizeDip = 18;

int ScaleDip( int dip, unsigned int dpi ) noexcept
{
	return ::MulDiv( dip, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), kDefaultDpi );
}

COLORREF BlendColor( COLORREF first, COLORREF second ) noexcept
{
	return RGB(
		(GetRValue(first) + GetRValue(second)) / 2,
		(GetGValue(first) + GetGValue(second)) / 2,
		(GetBValue(first) + GetBValue(second)) / 2 );
}

void DrawSymbolGlyph( HDC dc, int size, COLORREF color ) noexcept
{
	const int stroke = (std::max)(1, size / 16);
	const int left = size / 4;
	const int top = size / 5;
	const int right = size - left;
	const int bottom = size - top;
	const HPEN pen = ::CreatePen( PS_SOLID, stroke, color );
	const HGDIOBJ oldPen = ::SelectObject( dc, pen );
	const HGDIOBJ oldBrush = ::SelectObject( dc, ::GetStockObject(NULL_BRUSH) );
	::Rectangle( dc, left, top, right, bottom );
	for( int row = 0; row < 2; ++row ){
		const int y = top + (row + 1) * (bottom - top) / 3;
		::MoveToEx( dc, left + stroke * 2, y, nullptr );
		::LineTo( dc, right - stroke * 2, y );
	}
	::SelectObject( dc, oldBrush );
	::SelectObject( dc, oldPen );
	::DeleteObject( pen );
}

void DrawRootGlyph( HDC dc, int size, unsigned int dpi, COLORREF color ) noexcept
{
	const int fontHeight = -::MulDiv( 7, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), 72 );
	const HFONT font = ::CreateFontW( fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
		DEFAULT_PITCH | FF_SWISS, L"Segoe UI" );
	const HGDIOBJ oldFont = font != nullptr ? ::SelectObject(dc, font) : nullptr;
	const int oldBkMode = ::SetBkMode( dc, TRANSPARENT );
	const COLORREF oldTextColor = ::SetTextColor( dc, color );
	RECT bounds{ 0, 0, size, size };
	::DrawTextW( dc, L"abc", 3, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
	::SetTextColor( dc, oldTextColor );
	::SetBkMode( dc, oldBkMode );
	if( oldFont != nullptr ) ::SelectObject( dc, oldFont );
	if( font != nullptr ) ::DeleteObject( font );
}

} // namespace

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
	if( m_font.Dpi() != m_layout.dpi ){
		(void)m_font.Recreate( theme::ThemeFontKind::Chrome, m_layout.dpi );
		RecreateSymbolImages();
	}
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
	if( m_symbolImages != nullptr ){
		::ImageList_Destroy( m_symbolImages );
		m_symbolImages = nullptr;
	}
	m_lifecycle = AdvanceOutlineToolLifecycle( m_lifecycle, OutlineToolEvent::Close );
}

void COutlineWorkbenchTool::SetVisible( bool visible ) noexcept
{
	m_visible = visible;
	const HWND window = GetDialogWindow();
	if( window != nullptr ) ::ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
}

void COutlineWorkbenchTool::SetPalette( const theme::ThemePalette& palette )
{
	m_palette = palette;
	if( m_symbolImages != nullptr ) RecreateSymbolImages();
	ApplyAppearance();
}

void COutlineWorkbenchTool::ApplyLayout() noexcept
{
	const HWND window = GetDialogWindow();
	if( window == nullptr ) return;
	const LONG width = m_layout.bounds.right - m_layout.bounds.left;
	const LONG height = m_layout.bounds.bottom - m_layout.bounds.top;
	::SetWindowPos( window, nullptr, m_layout.bounds.left, m_layout.bounds.top, width, height,
		SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER );
	ApplyAppearance();
	// SHOW_RELOAD creates the child with SW_HIDE while the right panel is already
	// visible.  The outline must become visible on this layout pass without making
	// the panel active; Activate() remains the only focus-changing path.
	if( m_visible && ShouldShowOutlineDialog(m_lifecycle) ) {
		::ShowWindow( window, SW_SHOWNA );
	} else if( !m_visible ) {
		::ShowWindow( window, SW_HIDE );
	}
}

void COutlineWorkbenchTool::ApplyAppearance() noexcept
{
	if( m_dialog == nullptr || GetDialogWindow() == nullptr ) return;
	const unsigned int dpi = m_layout.dpi == 0 ? kDefaultDpi : m_layout.dpi;
	if( m_font.Get() == nullptr ) (void)m_font.Recreate( theme::ThemeFontKind::Chrome, dpi );
	if( m_symbolImages == nullptr ) RecreateSymbolImages();
	const COLORREF selection = BlendColor( m_palette.raised.ToColorRef(), m_palette.border.ToColorRef() );
	m_dialog->SetWorkbenchAppearance(
		m_palette.primaryText.ToColorRef(),
		m_palette.panel.ToColorRef(),
		m_palette.raised.ToColorRef(),
		selection,
		m_palette.highlightText.ToColorRef(),
		m_font.Get(),
		ScaleDip(kOutlineRowHeightDip, dpi),
		m_symbolImages );
}

void COutlineWorkbenchTool::RecreateSymbolImages() noexcept
{
	if( m_symbolImages != nullptr ){
		::ImageList_Destroy( m_symbolImages );
		m_symbolImages = nullptr;
	}
	const int size = (std::max)(8, ScaleDip(kSymbolImageSizeDip, m_layout.dpi));
	HIMAGELIST images = ::ImageList_Create( size, size, ILC_COLOR24, 2, 2 );
	if( images == nullptr ) return;
	(void)::ImageList_SetBkColor( images, m_palette.panel.ToColorRef() );

	const HDC screen = ::GetDC(nullptr);
	const HDC colorDc = ::CreateCompatibleDC(screen);
	const HBITMAP colorBitmap = ::CreateCompatibleBitmap(screen, size, size);
	if( colorDc == nullptr || colorBitmap == nullptr ){
		if( colorBitmap != nullptr ) ::DeleteObject(colorBitmap);
		if( colorDc != nullptr ) ::DeleteDC(colorDc);
		if( screen != nullptr ) ::ReleaseDC(nullptr, screen);
		::ImageList_Destroy(images);
		return;
	}

	const HGDIOBJ oldColorBitmap = ::SelectObject(colorDc, colorBitmap);
	RECT bounds{ 0, 0, size, size };
	const auto clearBitmap = [&]() noexcept {
		const HBRUSH background = ::CreateSolidBrush(m_palette.panel.ToColorRef());
		::FillRect( colorDc, &bounds, background );
		::DeleteObject( background );
	};

	clearBitmap();
	DrawRootGlyph(colorDc, size, m_layout.dpi, m_palette.secondaryText.ToColorRef());
	(void)::ImageList_Add(images, colorBitmap, nullptr);

	clearBitmap();
	DrawSymbolGlyph(colorDc, size, m_palette.secondaryText.ToColorRef());
	(void)::ImageList_Add(images, colorBitmap, nullptr);

	::SelectObject(colorDc, oldColorBitmap);
	::DeleteObject(colorBitmap);
	::DeleteDC(colorDc);
	if( screen != nullptr ) ::ReleaseDC(nullptr, screen);
	m_symbolImages = images;
}

HWND COutlineWorkbenchTool::GetDialogWindow() const noexcept
{
	if( m_dialog == nullptr ) return nullptr;
	const HWND window = m_dialog->GetHwnd();
	return window != nullptr && ::IsWindow(window) ? window : nullptr;
}

} // namespace workbench::outline
