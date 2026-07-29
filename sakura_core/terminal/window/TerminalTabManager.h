/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"
#include "terminal/session/TerminalSession.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class SakuraTerminalInputAdapter;

enum class TerminalTabEventKind : std::uint8_t {
	OutputAvailable,
	StateChanged,
};

struct TerminalTabEvent {
	TerminalTabEventKind kind{ TerminalTabEventKind::OutputAvailable };
	std::uint64_t tabId{};
	TerminalSessionState state{ TerminalSessionState::Idle };
	std::uint32_t errorCode{};
};

struct TerminalTabSnapshot {
	std::uint64_t id{};
	std::wstring label;
	TerminalSessionState state{ TerminalSessionState::Idle };
	std::uint32_t errorCode{};
	bool active{};
};

struct TerminalDrainResult {
	bool found{};
	bool active{};
	bool titleChanged{};
	bool synchronizedOutputCommitted{};
	bool protocolInputPending{};
	bool protocolInputRejected{};
	std::size_t bytesDrained{};
	std::vector<std::size_t> dirtyRows;
};

using TerminalSessionFactory = std::function<std::unique_ptr<CTerminalSession>(TerminalSessionCallbacks callbacks)>;
using TerminalLaunchResolver = std::function<std::optional<TerminalLaunchOptions>(TerminalSize size, std::wstring_view workingDirectory)>;
using TerminalTabEventCallback = std::function<void(const TerminalTabEvent& event)>;

struct TerminalTabManagerDependencies {
	TerminalSessionFactory createSession;
	TerminalLaunchResolver resolveLaunch;
};

//! UI-thread-owned terminal tab/session collection.
//!
//! Session callbacks only copy an event to the supplied callback; they never
//! dereference this manager.  This lets HWND owners marshal work safely while
//! fake backends can exercise the lifecycle without a window.
class TerminalTabManager final {
public:
	explicit TerminalTabManager( TerminalTabManagerDependencies dependencies, TerminalTabEventCallback eventCallback = {} );
	~TerminalTabManager();

	TerminalTabManager( const TerminalTabManager& ) = delete;
	TerminalTabManager& operator=( const TerminalTabManager& ) = delete;

	[[nodiscard]] std::optional<std::uint64_t> Activate( TerminalSize size, std::wstring_view workingDirectory );
	[[nodiscard]] std::optional<std::uint64_t> AddTab( TerminalSize size, std::wstring_view workingDirectory );
	[[nodiscard]] bool SelectTab( std::uint64_t tabId ) noexcept;
	[[nodiscard]] bool RestartTab( std::uint64_t tabId, TerminalSize size, std::wstring_view workingDirectory );
	[[nodiscard]] bool DeleteTab( std::uint64_t tabId ) noexcept;
	void Resize( TerminalSize size );
	[[nodiscard]] bool ResizeTab( std::uint64_t tabId, TerminalSize size );
	void Close() noexcept;

	[[nodiscard]] TerminalDrainResult DrainOutput( std::uint64_t tabId );
	[[nodiscard]] TerminalQueueInputResult QueueInput( std::uint64_t tabId, std::span<const std::uint8_t> bytes );
	[[nodiscard]] TerminalQueueInputResult QueueActiveInput( std::span<const std::uint8_t> bytes );
	//! Retries terminal protocol replies (DSR/DA/etc.) which were deferred only
	//! because the bounded session input queue was full.
	[[nodiscard]] TerminalQueueInputResult FlushPendingProtocolInput( std::uint64_t tabId );
	[[nodiscard]] bool HasPendingProtocolInput( std::uint64_t tabId ) const noexcept;
	[[nodiscard]] const TerminalModel* Model( std::uint64_t tabId ) const noexcept;
	[[nodiscard]] TerminalModel* Model( std::uint64_t tabId ) noexcept;
	[[nodiscard]] const TerminalModel* ActiveModel() const noexcept;
	[[nodiscard]] TerminalModel* ActiveModel() noexcept;
	[[nodiscard]] const SakuraTerminalInputAdapter* InputAdapter( std::uint64_t tabId ) const noexcept;
	[[nodiscard]] SakuraTerminalInputAdapter* InputAdapter( std::uint64_t tabId ) noexcept;
	[[nodiscard]] const SakuraTerminalInputAdapter* ActiveInputAdapter() const noexcept;
	[[nodiscard]] SakuraTerminalInputAdapter* ActiveInputAdapter() noexcept;
	[[nodiscard]] std::optional<std::uint64_t> ActiveTabId() const noexcept;
	[[nodiscard]] std::vector<TerminalTabSnapshot> Snapshot() const;
	[[nodiscard]] std::size_t TabCount() const noexcept;
	[[nodiscard]] bool HasStartedAnySession() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal
