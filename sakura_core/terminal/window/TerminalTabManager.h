/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"
#include "terminal/runtime/TerminalRuntimeService.h"

#include <chrono>
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
class CDefaultTerminalLaunchProfileService;

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
	//! ${process} source: the launch executable stem. Never overwritten by the
	//! process, so it stays a stable name the presentation layer can fall back to.
	std::wstring processName;
	//! Stable executable/profile identity used as the presentation fallback. Kept
	//! separate from processName so the two can diverge once real terminal
	//! profiles exist; visible chrome may prefer a resolved OSC title.
	std::wstring profileLabel;
	//! ${sequence} source: the raw OSC 0/2 title. This is not a display title;
	//! TerminalTabPresentation decides whether any of it reaches the tab.
	std::wstring sequenceTitle;
	//! ${cwd} fallback while no shell-integration CWD detection exists.
	std::wstring initialWorkingDirectory;
	TerminalSessionState state{ TerminalSessionState::Idle };
	std::uint32_t errorCode{};
	bool active{};
};

struct TerminalDrainResult {
	bool found{};
	bool active{};
	//! The process changed its OSC title. The displayed title only changes with
	//! it when the resolved presentation template actually consumes ${sequence}.
	bool sequenceChanged{};
	bool synchronizedOutputCommitted{};
	bool protocolInputPending{};
	bool protocolInputRejected{};
	std::size_t bytesDrained{};
	TerminalScrollbackChange scrollbackChange;
	std::vector<std::size_t> dirtyRows;
};

struct TerminalTabScrollbackChange final {
	std::uint64_t tabId{};
	TerminalScrollbackChange change;
};

enum class TerminalTabClearStatus : std::uint8_t {
	Cleared,
	DeadlineExceeded,
	InProgress,
	Unavailable,
};

struct TerminalTabClearResult {
	TerminalTabClearStatus status{ TerminalTabClearStatus::Cleared };
	std::size_t clearedTabCount{};

	[[nodiscard]] constexpr bool IsQuiescent() const noexcept
	{
		return status == TerminalTabClearStatus::Cleared
			|| status == TerminalTabClearStatus::DeadlineExceeded;
	}
};

using TerminalSessionFactory = std::function<std::unique_ptr<CTerminalSession>(TerminalSessionCallbacks callbacks)>;
using TerminalLaunchResolver = std::function<std::optional<TerminalLaunchOptions>(TerminalSize size, std::wstring_view workingDirectory)>;
using TerminalTabEventCallback = std::function<void(const TerminalTabEvent& event)>;

struct TerminalTabManagerDependencies {
	TerminalSessionFactory createSession;
	TerminalLaunchResolver resolveLaunch;
	//! An existing runtime authority may be supplied by the workbench. When it
	//! is absent, the manager creates one and keeps it alive independently of
	//! the terminal projection.
	std::shared_ptr<CTerminalRuntimeService> runtimeService;
	//! Shared profile catalog owned by the editor process. UI profile commands
	//! and runtime launches must mutate/read this same policy object.
	std::shared_ptr<CDefaultTerminalLaunchProfileService> launchProfiles;
	TerminalRuntimeLaunchDecorator decorateLaunch;
};

//! UI-thread-owned terminal tab/session collection.
//!
//! Session callbacks route through shared internal state and only copy an event
//! to the supplied callback. This lets HWND owners marshal work safely while
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
	//! Removes every tab without closing the manager. Each live session receives
	//! BeginClose and is handed to the bounded retirement service; this method
	//! never waits for backend or worker quiescence. Tab IDs remain monotonic so
	//! late notifications cannot alias replacement tabs. The deadline is retained
	//! for source/API compatibility and is not a UI-thread wait budget.
	[[nodiscard]] TerminalTabClearResult ClearTabs( std::chrono::steady_clock::time_point deadline ) noexcept;
	void Resize( TerminalSize size );
	[[nodiscard]] bool ResizeTab( std::uint64_t tabId, TerminalSize size );
	//! Applies the stable terminal.integrated.scrollback policy to existing tabs
	//! and remembers it for future/restarted models. No PTY is restarted.
	[[nodiscard]] std::vector<TerminalTabScrollbackChange> SetScrollbackLimit( std::size_t lines );
	[[nodiscard]] std::size_t ScrollbackLimit() const noexcept;
	void Close() noexcept;

	[[nodiscard]] TerminalDrainResult DrainOutput( std::uint64_t tabId );
	[[nodiscard]] TerminalQueueInputResult QueueInput( std::uint64_t tabId, std::span<const std::uint8_t> bytes );
	[[nodiscard]] TerminalQueueInputResult QueueActiveInput( std::span<const std::uint8_t> bytes );
	//! Retries terminal protocol replies (DSR/DA/etc.) which were deferred only
	//! because the bounded session input queue was full.
	[[nodiscard]] TerminalQueueInputResult FlushPendingProtocolInput( std::uint64_t tabId );
	void RecordViewportDiagnostic( std::uint64_t tabId, const TerminalViewportDiagnosticSnapshot& snapshot ) noexcept;
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
	std::shared_ptr<Impl> m_impl;
};

} // namespace terminal
