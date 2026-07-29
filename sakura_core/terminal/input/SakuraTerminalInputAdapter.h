/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/window/TerminalInput.h"

#include <memory>
#include <optional>
#include <string>

namespace terminal {

//! Private Sakura boundary around the pinned Windows Terminal input encoder.
//! No Microsoft::Console or TIL type crosses this header.
class SakuraTerminalInputAdapter final {
public:
	SakuraTerminalInputAdapter();
	~SakuraTerminalInputAdapter();
	SakuraTerminalInputAdapter( const SakuraTerminalInputAdapter& ) = delete;
	SakuraTerminalInputAdapter& operator=( const SakuraTerminalInputAdapter& ) = delete;

	[[nodiscard]] std::optional<std::string> EncodeKey( const TerminalKeyEvent& event );
	[[nodiscard]] std::optional<std::string> EncodeMouse( const TerminalMouseEvent& event );
	[[nodiscard]] std::optional<std::string> EncodeFocus( bool focused ) const;
	void SetMode( int mode, bool enabled ) noexcept;
	void SetAlternateScreen( bool enabled ) noexcept;
	void Reset() noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal
