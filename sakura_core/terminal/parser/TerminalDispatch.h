/*! @file */
#pragma once

#include "terminal/model/TerminalModel.h"

#include <string_view>
#include <vector>

namespace terminal {

// Sakura-owned adapter between the VT state machine and TerminalModel. No
// Windows Terminal types cross this boundary.
class TerminalDispatch final {
public:
	explicit TerminalDispatch( TerminalModel& model ) noexcept : m_model(model) {}

	void Print( char32_t codepoint ) { m_model.Print(codepoint); }
	void Execute( wchar_t control ) { m_model.ExecuteControl(control); }
	void Escape( wchar_t final );
	void Csi( wchar_t final, bool privateMode, const std::vector<int>& parameters );
	void Osc( unsigned int command, std::wstring_view payload );

private:
	static int Parameter( const std::vector<int>& parameters, std::size_t index, int defaultValue ) noexcept;
	void SelectGraphicRendition( const std::vector<int>& parameters );

	TerminalModel& m_model;
};

} // namespace terminal
