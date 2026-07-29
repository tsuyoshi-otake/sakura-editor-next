/*! @file */
#include "StdAfx.h"
#include "terminal/input/SakuraTerminalInputAdapter.h"

#include "terminal/vendor/windows_terminal/sakura_compat/WindowsTerminalCompat.h"
#include "terminal/vendor/windows_terminal/src/terminal/input/terminalInput.hpp"

#include <windows.h>

namespace terminal {
namespace {

using UpstreamInput = Microsoft::Console::VirtualTerminal::TerminalInput;

DWORD ControlState( const TerminalKeyEvent& event ) noexcept
{
	DWORD state{};
	if( event.shift ) state |= SHIFT_PRESSED;
	if( event.control ) state |= event.rightControl ? RIGHT_CTRL_PRESSED : LEFT_CTRL_PRESSED;
	if( event.alt ) state |= event.rightAlt ? RIGHT_ALT_PRESSED : LEFT_ALT_PRESSED;
	if( event.enhanced ) state |= ENHANCED_KEY;
	if( event.capsLock ) state |= CAPSLOCK_ON;
	if( event.numLock ) state |= NUMLOCK_ON;
	return state;
}

short MouseControlState( const TerminalMouseEvent& event ) noexcept
{
	short state{};
	if( event.shift ) state |= SHIFT_PRESSED;
	if( event.control ) state |= LEFT_CTRL_PRESSED;
	if( event.alt ) state |= LEFT_ALT_PRESSED;
	return state;
}

unsigned int MouseMessage( const TerminalMouseEvent& event ) noexcept
{
	if( event.action == TerminalMouseAction::Move ) return WM_MOUSEMOVE;
	if( event.action == TerminalMouseAction::WheelUp || event.action == TerminalMouseAction::WheelDown ) return WM_MOUSEWHEEL;
	if( event.action == TerminalMouseAction::Release ) {
		return event.button == 1 ? WM_MBUTTONUP : event.button == 2 ? WM_RBUTTONUP : WM_LBUTTONUP;
	}
	return event.button == 1 ? WM_MBUTTONDOWN : event.button == 2 ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
}

std::optional<std::string> ConvertOutput( const UpstreamInput::OutputType& output )
{
	if( !output ) return std::nullopt;
	return EncodeTerminalText(*output);
}

} // namespace

struct SakuraTerminalInputAdapter::Impl {
	UpstreamInput input;

	Impl()
	{
		// OSC/CSI output must never opt the editor into lossless Win32 input or
		// the kitty protocol without an explicit Sakura policy decision.
		input.ForceDisableWin32InputMode(true);
		input.ForceDisableKittyKeyboardProtocol(true);
	}
};

SakuraTerminalInputAdapter::SakuraTerminalInputAdapter() : m_impl(std::make_unique<Impl>()) {}
SakuraTerminalInputAdapter::~SakuraTerminalInputAdapter() = default;

std::optional<std::string> SakuraTerminalInputAdapter::EncodeKey( const TerminalKeyEvent& event )
{
	INPUT_RECORD record{};
	record.EventType = KEY_EVENT;
	auto& key = record.Event.KeyEvent;
	key.bKeyDown = event.keyDown;
	key.wRepeatCount = event.repeatCount == 0 ? 1 : event.repeatCount;
	key.wVirtualKeyCode = static_cast<WORD>(event.virtualKey);
	key.wVirtualScanCode = event.scanCode;
	key.uChar.UnicodeChar = event.character;
	key.dwControlKeyState = ControlState(event);
	return ConvertOutput(m_impl->input.HandleKey(record));
}

std::optional<std::string> SakuraTerminalInputAdapter::EncodeMouse( const TerminalMouseEvent& event )
{
	const auto message = MouseMessage(event);
	const short delta = event.action == TerminalMouseAction::WheelUp ? WHEEL_DELTA :
		event.action == TerminalMouseAction::WheelDown ? -WHEEL_DELTA : 0;
	UpstreamInput::MouseButtonState state{};
	if( event.action != TerminalMouseAction::Release && event.action != TerminalMouseAction::WheelUp && event.action != TerminalMouseAction::WheelDown ) {
		state.isLeftButtonDown = event.button == 0;
		state.isMiddleButtonDown = event.button == 1;
		state.isRightButtonDown = event.button == 2;
	}
	return ConvertOutput(m_impl->input.HandleMouse(
		{ base::saturated_cast<til::CoordType>(event.column), base::saturated_cast<til::CoordType>(event.row) },
		message, MouseControlState(event), delta, state));
}

std::optional<std::string> SakuraTerminalInputAdapter::EncodeFocus( bool focused ) const
{
	return ConvertOutput(m_impl->input.HandleFocus(focused));
}

void SakuraTerminalInputAdapter::SetMode( int mode, bool enabled ) noexcept
{
	using Mode = UpstreamInput::Mode;
	switch( mode ) {
	case 1: m_impl->input.SetInputMode(Mode::CursorKey, enabled); break;
	case 20: m_impl->input.SetInputMode(Mode::LineFeed, enabled); break;
	case 66: m_impl->input.SetInputMode(Mode::Keypad, enabled); break;
	case 67: m_impl->input.SetInputMode(Mode::BackarrowKey, enabled); break;
	case 1000: m_impl->input.SetInputMode(Mode::DefaultMouseTracking, enabled); break;
	case 1002: m_impl->input.SetInputMode(Mode::ButtonEventMouseTracking, enabled); break;
	case 1003: m_impl->input.SetInputMode(Mode::AnyEventMouseTracking, enabled); break;
	case 1004: m_impl->input.SetInputMode(Mode::FocusEvent, enabled); break;
	case 1005: m_impl->input.SetInputMode(Mode::Utf8MouseEncoding, enabled); break;
	case 1006: m_impl->input.SetInputMode(Mode::SgrMouseEncoding, enabled); break;
	case 1007: m_impl->input.SetInputMode(Mode::AlternateScroll, enabled); break;
	case 1034: m_impl->input.SetInputMode(Mode::SendC1, enabled); break;
	case 9001: break; // Explicitly disabled in Impl regardless of remote output.
	default: break;
	}
}

void SakuraTerminalInputAdapter::SetAlternateScreen( bool enabled ) noexcept
{
	if( enabled ) m_impl->input.UseAlternateScreenBuffer();
	else m_impl->input.UseMainScreenBuffer();
}

void SakuraTerminalInputAdapter::Reset() noexcept
{
	m_impl->input.ResetInputModes();
	m_impl->input.ForceDisableWin32InputMode(true);
	m_impl->input.ForceDisableKittyKeyboardProtocol(true);
	m_impl->input.UseMainScreenBuffer();
}

} // namespace terminal
