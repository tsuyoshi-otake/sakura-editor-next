/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/outline/COutlineWorkbenchTool.h"

#include "outline/CDlgFuncList.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"

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

void DrawCodiconGlyph( HDC dc, int size, std::wstring_view name, COLORREF color ) noexcept
{
	if( dc == nullptr || size <= 0 ) return;
	const auto glyph = workbench::icons::FindCodiconGlyph(name);
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if( !glyph.has_value() || faceName.empty() || faceName.size() >= LF_FACESIZE ) return;
	LOGFONTW logFont{};
	logFont.lfHeight = -size;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), logFont.lfFaceName);
	logFont.lfFaceName[faceName.size()] = L'\0';
	const HFONT font = ::CreateFontIndirectW(&logFont);
	if( font == nullptr ) return;
	const HGDIOBJ oldFont = ::SelectObject( dc, font );
	const int oldBkMode = ::SetBkMode( dc, TRANSPARENT );
	const COLORREF oldTextColor = ::SetTextColor( dc, color );
	const wchar_t text[] = { *glyph, L'\0' };
	RECT bounds{ 0, 0, size, size };
	::DrawTextW( dc, text, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX );
	::SetTextColor( dc, oldTextColor );
	::SetBkMode( dc, oldBkMode );
	::SelectObject( dc, oldFont );
	::DeleteObject( font );
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

COLORREF OutlineBackgroundColor( const theme::ThemePalette& palette ) noexcept
{
	return palette.sideBar.ToColorRef();
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
		m_appearanceDirty = true;
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
	if( m_dialog != nullptr ) {
		// Stop the snapshot worker while the dialog still owns its result gate.
		// Close joins before the HWND is destroyed, so no worker can post into a
		// reclaimed dialog or outlive this tool.
		m_dialog->StopWorkbenchOutlineWorker();
	}
	if( window != nullptr ) ::DestroyWindow( window );
	if( m_dialog != nullptr ) {
		m_dialog->SetWorkbenchMode( false );
		m_dialog->SetWorkbenchParent( nullptr );
	}
	m_parent = nullptr;
	m_appliedWindow = nullptr;
	m_appearanceWindow = nullptr;
	m_hasAppliedLayout = false;
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
	if( window != nullptr && (::IsWindowVisible(window) != FALSE) != visible ) {
		::ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
	}
}

void COutlineWorkbenchTool::SetPalette( const theme::ThemePalette& palette )
{
	if( m_palette == palette ) return;
	m_palette = palette;
	m_appearanceDirty = true;
	if( m_symbolImages != nullptr ) RecreateSymbolImages();
	ApplyAppearance();
}

bool COutlineWorkbenchTool::Reparent( HWND parent ) noexcept
{
	if( m_dialog == nullptr || parent == nullptr || m_lifecycle == OutlineToolLifecycle::Closed ) return false;
	if( m_parent == parent ) return true;
	m_parent = parent;
	m_dialog->SetWorkbenchParent( parent );
	// The dialog is created lazily by the existing Command_FUNCLIST path.  When it does
	// not exist yet, updating the recorded workbench parent is the whole operation.
	if( const HWND window = GetDialogWindow(); window != nullptr ){
		if( ::SetParent( window, parent ) == nullptr ) return false;
		m_hasAppliedLayout = false;
		ApplyLayout();
	}
	return true;
}

void COutlineWorkbenchTool::ApplyLayout() noexcept
{
	const HWND window = GetDialogWindow();
	if( window == nullptr ) return;
	const LONG width = m_layout.bounds.right - m_layout.bounds.left;
	const LONG height = m_layout.bounds.bottom - m_layout.bounds.top;
	if( !m_hasAppliedLayout || m_appliedWindow != window || !(m_appliedLayout == m_layout) ){
		::SetWindowPos( window, nullptr, m_layout.bounds.left, m_layout.bounds.top, width, height,
			SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER );
		m_appliedWindow = window;
		m_appliedLayout = m_layout;
		m_hasAppliedLayout = true;
	}
	if( m_appearanceDirty || m_appearanceWindow != window ) ApplyAppearance();
	// SHOW_RELOAD creates the child with SW_HIDE while the right panel is already
	// visible.  The outline must become visible on this layout pass without making
	// the panel active; Activate() remains the only focus-changing path.
	const bool shouldShow = m_visible && ShouldShowOutlineDialog(m_lifecycle);
	if( (::IsWindowVisible(window) != FALSE) != shouldShow ) {
		::ShowWindow( window, shouldShow ? SW_SHOWNA : SW_HIDE );
	}
}

void COutlineWorkbenchTool::ApplyAppearance() noexcept
{
	if( m_dialog == nullptr || GetDialogWindow() == nullptr ) return;
	const HWND window = GetDialogWindow();
	if( !m_appearanceDirty && m_appearanceWindow == window ) return;
	const unsigned int dpi = m_layout.dpi == 0 ? kDefaultDpi : m_layout.dpi;
	if( m_font.Get() == nullptr ) (void)m_font.Recreate( theme::ThemeFontKind::Chrome, dpi );
	if( m_symbolImages == nullptr ) RecreateSymbolImages();
	const COLORREF selection = BlendColor( m_palette.raised.ToColorRef(), m_palette.border.ToColorRef() );
	m_dialog->SetWorkbenchAppearance(
		m_palette.primaryText.ToColorRef(),
		OutlineBackgroundColor(m_palette),
		m_palette.raised.ToColorRef(),
		selection,
		m_palette.highlightText.ToColorRef(),
		m_font.Get(),
		ScaleDip(kOutlineRowHeightDip, dpi),
		m_symbolImages );
	m_appearanceWindow = window;
	m_appearanceDirty = false;
}

void COutlineWorkbenchTool::RecreateSymbolImages() noexcept
{
	m_appearanceDirty = true;
	if( m_symbolImages != nullptr ){
		::ImageList_Destroy( m_symbolImages );
		m_symbolImages = nullptr;
	}
	const int size = (std::max)(8, ScaleDip(kSymbolImageSizeDip, m_layout.dpi));
	const int imageCount = CDlgFuncList::WorkbenchSymbolImageCount();
	HIMAGELIST images = ::ImageList_Create( size, size, ILC_COLOR24, imageCount, imageCount );
	if( images == nullptr ) return;
	(void)::ImageList_SetBkColor( images, OutlineBackgroundColor(m_palette) );

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
		const HBRUSH background = ::CreateSolidBrush(OutlineBackgroundColor(m_palette));
		::FillRect( colorDc, &bounds, background );
		::DeleteObject( background );
	};

	for( int imageIndex = 0; imageIndex < imageCount; ++imageIndex ){
		clearBitmap();
		DrawCodiconGlyph(
			colorDc, size, CDlgFuncList::WorkbenchSymbolCodiconName(imageIndex),
			m_palette.secondaryText.ToColorRef() );
		(void)::ImageList_Add(images, colorBitmap, nullptr);
	}

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
