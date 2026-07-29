/*! @file */
#pragma once

#include "terminal/model/TerminalModel.h"

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace terminal {

// Sakura-owned adapter between the VT state machine and TerminalModel. No
// Windows Terminal types cross this boundary.
class TerminalDispatch final {
public:
	using ResponseSink = std::function<void(std::string_view)>;
	explicit TerminalDispatch( TerminalModel& model, ResponseSink responseSink = {} ) noexcept
		: m_model(model), m_responseSink(std::move(responseSink)) {}

	void Print( char32_t codepoint ) { m_model.Print(codepoint); }
	void Execute( wchar_t control ) { m_model.ExecuteControl(control); }
	void Escape( wchar_t final );
	void Csi( wchar_t final, bool privateMode, const std::vector<int>& parameters );
	void Osc( unsigned int command, std::wstring_view payload );

private:
	static int Parameter( const std::vector<int>& parameters, std::size_t index, int defaultValue ) noexcept;
	void SelectGraphicRendition( const std::vector<int>& parameters );
	void Respond( std::string response ) const;

	TerminalModel& m_model;
	ResponseSink m_responseSink;
};

} // namespace terminal
