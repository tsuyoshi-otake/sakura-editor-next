/*! @file
    @brief Shared default terminal launch-profile policy.
*/
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/PowerShellLocator.h"
#include "terminal/session/TerminalSession.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace terminal {

//! Process-shared launch policy used by the terminal runtime and its UI
//! projection. Sharing this object keeps profile selection and Bridge-created
//! terminal launches on one authoritative catalog.
class CDefaultTerminalLaunchProfileService final {
public:
	CDefaultTerminalLaunchProfileService();
	~CDefaultTerminalLaunchProfileService();
	CDefaultTerminalLaunchProfileService(const CDefaultTerminalLaunchProfileService&) = delete;
	CDefaultTerminalLaunchProfileService& operator=(const CDefaultTerminalLaunchProfileService&) = delete;

	[[nodiscard]] std::optional<TerminalLaunchOptions> Resolve(
		TerminalSize size, std::wstring_view workingDirectory);
	void Redetect() noexcept;
	[[nodiscard]] std::vector<TerminalProfile> Profiles();
	[[nodiscard]] std::optional<TerminalProfile> SelectedProfile();
	[[nodiscard]] bool SelectProfile(std::wstring_view path);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal
