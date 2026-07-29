/*! @file */
#include "StdAfx.h"
#include "terminal/parser/SakuraTerminalStateMachineEngine.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace terminal {
namespace {

std::string_view IdentifierText( Microsoft::Console::VirtualTerminal::VTID id ) noexcept
{
	const auto* const text = id.ToString();
	return { text, std::char_traits<char>::length(text) };
}

// The upstream parser keeps colon-delimited values attached to their owning
// parameter. TerminalDispatch deliberately has no Windows Terminal types, so
// normalize only the SGR extended-color forms it already understands before
// crossing that boundary. Do not partially dispatch a sequence containing an
// unsupported subparameter: before this conversion all subparameters caused
// the whole CSI sequence to be ignored.
bool NormalizeSgrParameters(
	const Microsoft::Console::VirtualTerminal::VTParameters& parameters,
	std::vector<int>& values )
{
	constexpr std::size_t kMaximumParameters = 32;
	values.clear();
	values.reserve(std::min<std::size_t>(parameters.size(), kMaximumParameters));

	for( std::size_t index = 0; index < parameters.size() && index < kMaximumParameters; ++index ) {
		const auto parameter = parameters.at(index);
		const auto value = parameter.has_value() ? parameter.value() : 0;
		if( !parameters.hasSubParamsFor(index) ) {
			values.push_back(value);
			continue;
		}

		if( value != 38 && value != 48 ) return false;
		const auto subParameters = parameters.subParamsFor(index);
		const auto mode = subParameters.at(0);
		if( !mode.has_value() ) return false;

		if( mode.value() == 5 ) {
			// CSI 38:5:<index> m and CSI 48:5:<index> m
			const auto color = subParameters.at(1);
			if( subParameters.size() != 2 || !color.has_value() ) return false;
			values.insert(values.end(), { value, 5, color.value() });
			continue;
		}

		if( mode.value() == 2 ) {
			// CSI 38:2:<color-space>:R:G:B m. The color-space field is
			// conventionally omitted (38:2::R:G:B) or zero for default RGB.
			const auto colorSpace = subParameters.at(1);
			const auto red = subParameters.at(2);
			const auto green = subParameters.at(3);
			const auto blue = subParameters.at(4);
			if( subParameters.size() != 5 ||
				(colorSpace.has_value() && colorSpace.value() != 0) ||
				!red.has_value() || !green.has_value() || !blue.has_value() ) return false;
			values.insert(values.end(), { value, 2, red.value(), green.value(), blue.value() });
			continue;
		}

		return false;
	}
	return true;
}

} // namespace

SakuraTerminalStateMachineEngine::SakuraTerminalStateMachineEngine(
	TerminalModel& model, SakuraTerminalInputAdapter* inputAdapter, TerminalDispatch::ResponseSink responseSink ) noexcept
	: m_dispatch(model, std::move(responseSink)), m_inputAdapter(inputAdapter)
{
}

void SakuraTerminalStateMachineEngine::Reset() noexcept
{
	m_pendingHighSurrogate = 0;
}

void SakuraTerminalStateMachineEngine::UnknownSequence() noexcept {}

bool SakuraTerminalStateMachineEngine::EncounteredWin32InputModeSequence() const noexcept
{
	return false;
}

void SakuraTerminalStateMachineEngine::FlushPendingSurrogate()
{
	if( m_pendingHighSurrogate ) {
		m_dispatch.Print(0xfffd);
		m_pendingHighSurrogate = 0;
	}
}

void SakuraTerminalStateMachineEngine::PrintCodeUnit( wchar_t value )
{
	if( value >= 0xd800 && value <= 0xdbff ) {
		FlushPendingSurrogate();
		m_pendingHighSurrogate = value;
		return;
	}
	if( value >= 0xdc00 && value <= 0xdfff ) {
		if( m_pendingHighSurrogate ) {
			const char32_t codepoint = 0x10000 +
				((static_cast<char32_t>(m_pendingHighSurrogate) - 0xd800) << 10) +
				(static_cast<char32_t>(value) - 0xdc00);
			m_pendingHighSurrogate = 0;
			m_dispatch.Print(codepoint);
		} else {
			m_dispatch.Print(0xfffd);
		}
		return;
	}
	FlushPendingSurrogate();
	m_dispatch.Print(static_cast<char32_t>(value));
}

bool SakuraTerminalStateMachineEngine::ActionExecute( wchar_t value )
{
	FlushPendingSurrogate();
	m_dispatch.Execute(value);
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionExecuteFromEscape( wchar_t value )
{
	return ActionExecute(value);
}

bool SakuraTerminalStateMachineEngine::ActionPrint( wchar_t value )
{
	PrintCodeUnit(value);
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionPrintString( std::wstring_view value )
{
	for( const auto codeUnit : value ) PrintCodeUnit(codeUnit);
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionPassThroughString( std::wstring_view )
{
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionEscDispatch( Microsoft::Console::VirtualTerminal::VTID id )
{
	FlushPendingSurrogate();
	const auto text = IdentifierText(id);
	if( text.size() == 1 ) {
		const auto final = static_cast<wchar_t>(static_cast<unsigned char>(text.front()));
		m_dispatch.Escape(final);
		if( final == L'c' && m_inputAdapter ) m_inputAdapter->Reset();
	}
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionVt52EscDispatch(
	Microsoft::Console::VirtualTerminal::VTID,
	Microsoft::Console::VirtualTerminal::VTParameters )
{
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionCsiDispatch(
	Microsoft::Console::VirtualTerminal::VTID id,
	Microsoft::Console::VirtualTerminal::VTParameters parameters )
{
	FlushPendingSurrogate();
	const auto text = IdentifierText(id);
	if( text.empty() ) return true;
	const bool privateMode = text.size() == 2 && text.front() == '?';
	if( text.size() != 1 && !privateMode ) return true;
	const auto final = static_cast<wchar_t>(static_cast<unsigned char>(text.back()));
	std::vector<int> values;
	if( parameters.hasSubParams() ) {
		if( final != L'm' || privateMode || !NormalizeSgrParameters(parameters, values) ) return true;
	} else {
		values.reserve(std::min<std::size_t>(parameters.size(), 32));
		for( std::size_t index = 0; index < parameters.size() && index < 32; ++index ) {
			const auto parameter = parameters.at(index);
			values.push_back(parameter.has_value() ? parameter.value() : 0);
		}
	}
	m_dispatch.Csi(final, privateMode, values);
	if( privateMode && (final == L'h' || final == L'l') && m_inputAdapter ) {
		const bool enabled = final == L'h';
		for( const auto mode : values ) {
			if( mode == 1047 || mode == 1049 ) m_inputAdapter->SetAlternateScreen(enabled);
			else m_inputAdapter->SetMode(mode, enabled);
		}
	}
	return true;
}

SakuraTerminalStateMachineEngine::StringHandler SakuraTerminalStateMachineEngine::ActionDcsDispatch(
	Microsoft::Console::VirtualTerminal::VTID,
	Microsoft::Console::VirtualTerminal::VTParameters )
{
	// An empty handler makes the upstream state machine enter DCS-ignore. SIXEL,
	// file transfer, and arbitrary host passthrough remain disabled.
	return {};
}

bool SakuraTerminalStateMachineEngine::ActionOscDispatch( std::size_t parameter, std::wstring_view value )
{
	FlushPendingSurrogate();
	m_dispatch.Osc(static_cast<unsigned int>(std::min<std::size_t>(parameter, UINT_MAX)), value);
	return true;
}

bool SakuraTerminalStateMachineEngine::ActionSs3Dispatch(
	wchar_t, Microsoft::Console::VirtualTerminal::VTParameters )
{
	return true;
}

} // namespace terminal
