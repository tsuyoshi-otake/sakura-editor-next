/*! @file */
#include "StdAfx.h"
#include "terminal/parser/TerminalDispatch.h"

#include <algorithm>

namespace terminal {

int TerminalDispatch::Parameter( const std::vector<int>& parameters, std::size_t index, int defaultValue ) noexcept
{
	return index < parameters.size() && parameters[index] != 0 ? parameters[index] : defaultValue;
}

void TerminalDispatch::Escape( wchar_t final )
{
	switch( final ) {
	case L'7': m_model.SaveCursor(); break;
	case L'8': m_model.RestoreCursor(); break;
	case L'D': m_model.ExecuteControl(L'\n'); break;
	case L'E': m_model.ExecuteControl(L'\r'); m_model.ExecuteControl(L'\n'); break;
	case L'M': m_model.ReverseIndex(); break;
	case L'c': m_model.Reset(); break;
	default: break;
	}
}

void TerminalDispatch::Csi( wchar_t final, bool privateMode, const std::vector<int>& parameters )
{
	const auto count = std::max(1, Parameter(parameters, 0, 1));
	switch( final ) {
	case L'A': m_model.MoveCursorRelative(0, -count); break;
	case L'B': m_model.MoveCursorRelative(0, count); break;
	case L'C': m_model.MoveCursorRelative(count, 0); break;
	case L'D': m_model.MoveCursorRelative(-count, 0); break;
	case L'E': m_model.MoveCursorRelative(0, count); m_model.SetCursorColumn(0); break;
	case L'F': m_model.MoveCursorRelative(0, -count); m_model.SetCursorColumn(0); break;
	case L'G': m_model.SetCursorColumn(static_cast<std::size_t>(count - 1)); break;
	case L'H': case L'f':
		m_model.SetCursorPosition(static_cast<std::size_t>(Parameter(parameters, 1, 1) - 1), static_cast<std::size_t>(Parameter(parameters, 0, 1) - 1));
		break;
	case L'J': m_model.EraseDisplay(parameters.empty() ? 0 : parameters[0]); break;
	case L'K': m_model.EraseLine(parameters.empty() ? 0 : parameters[0]); break;
	case L'S': m_model.ScrollUp(static_cast<std::size_t>(count)); break;
	case L'T': m_model.ScrollDown(static_cast<std::size_t>(count)); break;
	case L'm': SelectGraphicRendition(parameters); break;
	case L'r': {
		const auto top = Parameter(parameters, 0, 1);
		const auto bottom = Parameter(parameters, 1, static_cast<int>(m_model.RowCount()));
		m_model.SetScrollRegion(static_cast<std::size_t>(std::max(1, top) - 1), static_cast<std::size_t>(std::max(1, bottom) - 1));
		break;
	}
	case L's': m_model.SaveCursor(); break;
	case L'u': m_model.RestoreCursor(); break;
	case L'h': case L'l':
		if( privateMode ) {
			const bool enabled = final == L'h';
			for( const auto mode : parameters ) {
				if( mode == 1047 || mode == 1049 ) m_model.SetAlternateScreen(enabled);
				else m_model.SetMode(mode, enabled);
			}
		}
		break;
	default: break;
	}
}

void TerminalDispatch::SelectGraphicRendition( const std::vector<int>& parameters )
{
	const std::vector<int> reset{ 0 };
	const auto& values = parameters.empty() ? reset : parameters;
	for( std::size_t i = 0; i < values.size(); ++i ) {
		const auto value = values[i];
		switch( value ) {
		case 0: m_model.ResetAttributes(); break;
		case 1: m_model.SetBold(true); break;
		case 4: m_model.SetUnderline(true); break;
		case 7: m_model.SetInverse(true); break;
		case 22: m_model.SetBold(false); break;
		case 24: m_model.SetUnderline(false); break;
		case 27: m_model.SetInverse(false); break;
		case 39: m_model.SetForeground({}); break;
		case 49: m_model.SetBackground({}); break;
		default:
			if( value >= 30 && value <= 37 ) m_model.SetForeground(TerminalColor::Indexed(static_cast<std::uint8_t>(value - 30)));
			else if( value >= 40 && value <= 47 ) m_model.SetBackground(TerminalColor::Indexed(static_cast<std::uint8_t>(value - 40)));
			else if( value >= 90 && value <= 97 ) m_model.SetForeground(TerminalColor::Indexed(static_cast<std::uint8_t>(value - 90 + 8)));
			else if( value >= 100 && value <= 107 ) m_model.SetBackground(TerminalColor::Indexed(static_cast<std::uint8_t>(value - 100 + 8)));
			else if( value == 38 || value == 48 ) {
				const bool foreground = value == 38;
				if( i + 2 < values.size() && values[i + 1] == 5 ) {
					const auto color = TerminalColor::Indexed(static_cast<std::uint8_t>(std::clamp(values[i + 2], 0, 255)));
					foreground ? m_model.SetForeground(color) : m_model.SetBackground(color);
					i += 2;
				} else if( i + 4 < values.size() && values[i + 1] == 2 ) {
					const auto color = TerminalColor::Rgb(
						static_cast<std::uint8_t>(std::clamp(values[i + 2], 0, 255)),
						static_cast<std::uint8_t>(std::clamp(values[i + 3], 0, 255)),
						static_cast<std::uint8_t>(std::clamp(values[i + 4], 0, 255)));
					foreground ? m_model.SetForeground(color) : m_model.SetBackground(color);
					i += 4;
				}
			}
			break;
		}
	}
}

void TerminalDispatch::Osc( unsigned int command, std::wstring_view payload )
{
	// OSC 52 clipboard and every non-title OSC are deliberately disabled. This
	// also blocks arbitrary command/file-transfer extensions by default.
	if( command != 0 && command != 2 ) return;
	std::wstring title;
	title.reserve(std::min<std::size_t>(payload.size(), 256));
	for( const auto ch : payload ) {
		if( title.size() >= 256 ) break;
		if( ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) ) continue;
		title.push_back(ch);
	}
	m_model.SetTitle(std::move(title));
}

} // namespace terminal
