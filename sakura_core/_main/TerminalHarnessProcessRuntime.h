/*! @file @brief Editor-process owner for the integrated terminal control plane. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/runtime/TerminalRuntimeService.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace terminal {
class CDefaultTerminalLaunchProfileService;
}

//! Immutable inputs already verified by the control/profile bootstrap.
struct TerminalHarnessProcessRuntimeOptions final {
	std::string profileId;
	std::uint64_t profileGeneration{};
	std::filesystem::path defaultWorkingDirectory;
	std::filesystem::path terminalToolsDirectory;
};

//! Process-lifetime composition for terminal authority, Harness Bridge, and
//! tmux compatibility dispatch. It owns no HWND and outlives every projection.
class CTerminalHarnessProcessRuntime final {
public:
	explicit CTerminalHarnessProcessRuntime(TerminalHarnessProcessRuntimeOptions options);
	~CTerminalHarnessProcessRuntime();
	CTerminalHarnessProcessRuntime(const CTerminalHarnessProcessRuntime&) = delete;
	CTerminalHarnessProcessRuntime& operator=(const CTerminalHarnessProcessRuntime&) = delete;

	//! Starts the isolated bridge. A false result leaves the editor usable while
	//! interactive terminal creation fails closed through launch decoration.
	[[nodiscard]] bool Start(std::wstring& diagnostic) noexcept;
	void Stop() noexcept;

	[[nodiscard]] std::shared_ptr<terminal::CTerminalRuntimeService> RuntimeService() const noexcept;
	[[nodiscard]] std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> LaunchProfiles() const noexcept;
	[[nodiscard]] bool IsReady() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
