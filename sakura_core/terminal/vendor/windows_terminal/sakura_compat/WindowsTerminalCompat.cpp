/*! @file */
#include "WindowsTerminalCompat.h"

#include "../src/terminal/parser/tracing.hpp"
#include "../src/terminal/types/inc/utils.hpp"

namespace Microsoft::Console::Utils {

const wchar_t* FindActionableControlCharacter( const wchar_t* begin, const std::size_t length ) noexcept
{
	const auto* current = begin;
	const auto* const end = begin + length;
	for( ; current < end; ++current ) {
		const auto value = static_cast<unsigned int>(*current);
		if( value <= 0x1f || (value >= 0x7f && value <= 0x9f) ) break;
	}
	return current;
}

} // namespace Microsoft::Console::Utils

namespace Microsoft::Console::VirtualTerminal {

// ETW tracing is intentionally omitted. TraceLogging's execution-character-set
// pragmas conflict with Sakura's Shift-JIS execution charset, while none of
// these callbacks participate in parser state or terminal output.
void ParserTracing::TraceStateChange( const wchar_t* ) const noexcept {}
void ParserTracing::TraceOnAction( const wchar_t* ) const noexcept {}
void ParserTracing::TraceOnExecute( wchar_t ) const noexcept {}
void ParserTracing::TraceOnExecuteFromEscape( wchar_t ) const noexcept {}
void ParserTracing::TraceOnEvent( const wchar_t* ) const noexcept {}
void ParserTracing::TraceCharInput( wchar_t ) {}
void ParserTracing::AddSequenceTrace( wchar_t ) {}
void ParserTracing::DispatchSequenceTrace( bool ) noexcept {}
void ParserTracing::ClearSequenceTrace() noexcept { _sequenceTrace.clear(); }
void ParserTracing::DispatchPrintRunTrace( const std::wstring_view& ) const {}

} // namespace Microsoft::Console::VirtualTerminal
