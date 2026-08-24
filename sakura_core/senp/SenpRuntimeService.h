/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace senp {

class ISenpManagementService;

struct IndentDecoration final {
	std::uint32_t visualStart = 0;
	std::uint32_t visualLength = 0;
	std::uint32_t depth = 0;
	[[nodiscard]] bool operator==(const IndentDecoration&) const = default;
};

enum class ERuntimeState : std::uint8_t {
	Created,
	Ready,
	ReadyWithDiagnostics,
	Stopped,
};

struct RuntimeSnapshot final {
	ERuntimeState state = ERuntimeState::Created;
	std::uint64_t revision = 0;
	std::size_t activeHosts = 0;
	std::wstring diagnostic;
};

//! Owns isolated extension host processes and an asynchronous decoration cache.
//! Paint callers can enqueue missing work but never wait for a Rust process.
class ISenpRuntimeService {
public:
	virtual ~ISenpRuntimeService() = default;
	[[nodiscard]] virtual bool Start() = 0;
	virtual void Stop() noexcept = 0;
	//! Invalidates paint-facing decoration cache state after a successful package
	//! or enablement change. This method never starts an extension host.
	virtual void NotifyExtensionsChanged() noexcept = 0;
	[[nodiscard]] virtual std::optional<std::vector<IndentDecoration>> RequestIndentDecorations(
		std::wstring_view line, std::uint32_t tabSize, std::uintptr_t repaintWindow) = 0;
	[[nodiscard]] virtual RuntimeSnapshot Snapshot() const = 0;
};

[[nodiscard]] std::unique_ptr<ISenpRuntimeService> CreateWin32SenpRuntimeService(
	ISenpManagementService& management);

} // namespace senp
