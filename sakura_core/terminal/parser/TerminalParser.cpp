/*! @file */
#include "StdAfx.h"
#include "terminal/parser/TerminalParser.h"

#include "terminal/parser/SakuraTerminalStateMachineEngine.h"
#include "terminal/vendor/windows_terminal/src/terminal/parser/stateMachine.hpp"

#include <memory>
#include <utility>

namespace terminal {
namespace {

void AppendUtf16( std::wstring& output, char32_t codepoint )
{
	if( codepoint <= 0xffff ) {
		output.push_back(static_cast<wchar_t>(codepoint));
		return;
	}
	codepoint -= 0x10000;
	output.push_back(static_cast<wchar_t>(0xd800 + (codepoint >> 10)));
	output.push_back(static_cast<wchar_t>(0xdc00 + (codepoint & 0x3ff)));
}

} // namespace

struct TerminalParser::Impl {
	using StateMachine = Microsoft::Console::VirtualTerminal::StateMachine;

	explicit Impl( TerminalModel& model, SakuraTerminalInputAdapter* inputAdapter, TerminalParser::ResponseSink responseSink )
	{
		auto ownedEngine = std::make_unique<SakuraTerminalStateMachineEngine>(model, inputAdapter, std::move(responseSink));
		engine = ownedEngine.get();
		stateMachine = std::make_unique<StateMachine>(std::move(ownedEngine));
	}

	void AppendReplacement( std::wstring& output ) noexcept
	{
		utf8Codepoint = utf8Minimum = 0;
		utf8Remaining = 0;
		output.push_back(0xfffd);
	}

	void Decode( std::string_view bytes, std::wstring& output )
	{
		output.reserve(output.size() + bytes.size());
		for( std::size_t index = 0; index < bytes.size(); ++index ) {
			const auto byte = static_cast<std::uint8_t>(bytes[index]);
			if( utf8Remaining != 0 ) {
				if( (byte & 0xc0) == 0x80 ) {
					utf8Codepoint = (utf8Codepoint << 6) | (byte & 0x3f);
					if( --utf8Remaining == 0 ) {
						const auto codepoint = utf8Codepoint;
						utf8Codepoint = 0;
						if( codepoint < utf8Minimum || codepoint > 0x10ffff ||
							(codepoint >= 0xd800 && codepoint <= 0xdfff) ) output.push_back(0xfffd);
						else AppendUtf16(output, codepoint);
					}
					continue;
				}
				AppendReplacement(output);
				--index; // Reprocess the non-continuation byte as a new character.
				continue;
			}
			if( byte < 0x80 ) output.push_back(static_cast<wchar_t>(byte));
			else if( byte >= 0xc2 && byte <= 0xdf ) {
				utf8Codepoint = byte & 0x1f; utf8Minimum = 0x80; utf8Remaining = 1;
			} else if( byte >= 0xe0 && byte <= 0xef ) {
				utf8Codepoint = byte & 0x0f; utf8Minimum = 0x800; utf8Remaining = 2;
			} else if( byte >= 0xf0 && byte <= 0xf4 ) {
				utf8Codepoint = byte & 0x07; utf8Minimum = 0x10000; utf8Remaining = 3;
			} else {
				output.push_back(0xfffd);
			}
		}
	}

	SakuraTerminalStateMachineEngine* engine{};
	std::unique_ptr<StateMachine> stateMachine;
	char32_t utf8Codepoint{};
	char32_t utf8Minimum{};
	std::uint8_t utf8Remaining{};
};

TerminalParser::TerminalParser( TerminalModel& model, SakuraTerminalInputAdapter* inputAdapter, ResponseSink responseSink )
	: m_impl(std::make_unique<Impl>(model, inputAdapter, std::move(responseSink)))
{
}

TerminalParser::~TerminalParser() = default;

void TerminalParser::Feed( std::string_view utf8Bytes )
{
	std::wstring decoded;
	m_impl->Decode(utf8Bytes, decoded);
	if( !decoded.empty() ) m_impl->stateMachine->ProcessString(decoded);
}

void TerminalParser::Flush()
{
	if( m_impl->utf8Remaining == 0 ) return;
	m_impl->utf8Remaining = 0;
	m_impl->utf8Codepoint = m_impl->utf8Minimum = 0;
	m_impl->stateMachine->ProcessCharacter(0xfffd);
}

void TerminalParser::Reset() noexcept
{
	m_impl->utf8Remaining = 0;
	m_impl->utf8Codepoint = m_impl->utf8Minimum = 0;
	m_impl->engine->Reset();
	m_impl->stateMachine->ResetState();
}

} // namespace terminal
