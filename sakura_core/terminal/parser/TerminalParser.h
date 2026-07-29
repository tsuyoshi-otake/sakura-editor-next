/*! @file */
#pragma once

#include "terminal/parser/TerminalDispatch.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace terminal {

class SakuraTerminalInputAdapter;

class TerminalParser final {
public:
	explicit TerminalParser( TerminalModel& model, SakuraTerminalInputAdapter* inputAdapter = nullptr );
	~TerminalParser();
	TerminalParser( const TerminalParser& ) = delete;
	TerminalParser& operator=( const TerminalParser& ) = delete;
	void Feed( std::string_view utf8Bytes );
	void Flush();
	void Reset() noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal
