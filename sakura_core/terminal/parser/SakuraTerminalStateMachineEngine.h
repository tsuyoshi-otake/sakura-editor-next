/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/parser/TerminalDispatch.h"
#include "terminal/vendor/windows_terminal/sakura_compat/WindowsTerminalCompat.h"
#include "terminal/vendor/windows_terminal/src/terminal/parser/IStateMachineEngine.hpp"

namespace terminal {

class SakuraTerminalInputAdapter;

class SakuraTerminalStateMachineEngine final : public Microsoft::Console::VirtualTerminal::IStateMachineEngine {
public:
	SakuraTerminalStateMachineEngine( TerminalModel& model, SakuraTerminalInputAdapter* inputAdapter,
		TerminalDispatch::ResponseSink responseSink = {} ) noexcept;
	void Reset() noexcept;

	void UnknownSequence() noexcept override;
	bool EncounteredWin32InputModeSequence() const noexcept override;
	bool ActionExecute( wchar_t value ) override;
	bool ActionExecuteFromEscape( wchar_t value ) override;
	bool ActionPrint( wchar_t value ) override;
	bool ActionPrintString( std::wstring_view value ) override;
	bool ActionPassThroughString( std::wstring_view value ) override;
	bool ActionEscDispatch( Microsoft::Console::VirtualTerminal::VTID id ) override;
	bool ActionVt52EscDispatch( Microsoft::Console::VirtualTerminal::VTID id,
		Microsoft::Console::VirtualTerminal::VTParameters parameters ) override;
	bool ActionCsiDispatch( Microsoft::Console::VirtualTerminal::VTID id,
		Microsoft::Console::VirtualTerminal::VTParameters parameters ) override;
	StringHandler ActionDcsDispatch( Microsoft::Console::VirtualTerminal::VTID id,
		Microsoft::Console::VirtualTerminal::VTParameters parameters ) override;
	bool ActionOscDispatch( std::size_t parameter, std::wstring_view value ) override;
	bool ActionSs3Dispatch( wchar_t value, Microsoft::Console::VirtualTerminal::VTParameters parameters ) override;

private:
	void FlushPendingSurrogate();
	void PrintCodeUnit( wchar_t value );

	TerminalDispatch m_dispatch;
	SakuraTerminalInputAdapter* m_inputAdapter{};
	wchar_t m_pendingHighSurrogate{};
};

} // namespace terminal
