/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

namespace detail {

//! Test seam for the v1 hello reply decode: strict JSON carrying exactly the
//! members type=="hello", protocol==1, abi=="sakura:senp/extension@1.0.0";
//! any extra, missing or mistyped member fails.
[[nodiscard]] bool AcceptExtensionHello(std::string_view response);

//! Decodes a v1 "decorations" reply into `slots`. The reply is strict JSON,
//! not configuration JSONC. Every slot's `line` is a caller-assigned index
//! within the request's `lines` array and must be below `lineCount`.
[[nodiscard]] bool ParseDecorationSlots(std::string_view response,
	std::uint32_t lineCount,
	std::vector<std::pair<std::uint32_t, IndentDecoration>>& slots);

//! One line inside a batched `decorate` frame; `line` is the caller-assigned
//! index the reply slots map back to.
struct DecorateLine final {
	std::uint32_t line = 0;
	std::wstring text;
};

//! One extension host process. The production implementation is the v1 pipe
//! host; tests substitute scripted peers through `SenpRuntimeHooks`.
class ISenpExtensionHost {
public:
	virtual ~ISenpExtensionHost() = default;
	[[nodiscard]] virtual bool Open(std::wstring_view modulePath,
		std::wstring_view moduleSha256) = 0;
	//! Sends one `decorate` frame covering `lines` and appends the reply's
	//! slots, each keyed by the caller-assigned `DecorateLine::line` index.
	[[nodiscard]] virtual bool Decorate(std::uint32_t tabSize,
		const std::vector<DecorateLine>& lines,
		std::vector<std::pair<std::uint32_t, IndentDecoration>>& slots) = 0;
	virtual void Close() noexcept = 0;
};

//! Test seam for the two OS dependencies of the v1 runtime worker: spawning
//! extension hosts and repainting the requester. Hooks are
//! constructor-injected per instance, never global; an empty std::function
//! means the production path.
struct SenpRuntimeHooks final {
	std::function<std::unique_ptr<ISenpExtensionHost>()> hostFactory;
	//! Called from the worker thread after a result was cached. Empty means
	//! `InvalidateRect(hwnd, nullptr, FALSE)` on a live window.
	std::function<void(std::uintptr_t)> repaint;
	//! Empty means the `sakura-senp-host.exe` beside this executable.
	std::wstring hostExecutable;
};

} // namespace detail

[[nodiscard]] std::unique_ptr<ISenpRuntimeService> CreateWin32SenpRuntimeService(
	ISenpManagementService& management, detail::SenpRuntimeHooks hooks = {});

} // namespace senp
