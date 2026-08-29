/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terminal::tmux {

inline constexpr std::string_view kTmuxCompatibilityVersion =
	"sakura-tmux 0.1 (tmux 3.7c command subset; not upstream tmux)";

enum class TmuxCommandKind : std::uint8_t {
	Version,
	ListPanes,
	SendKeys,
	CapturePane,
	DisplayMessage,
	ListSessions,
	ListWindows,
	NewSession,
	NewWindow,
	SplitWindow,
	SelectWindow,
	SelectPane,
	HasSession,
	KillPane,
	KillWindow,
	KillSession,
	WaitFor,
};

enum class TmuxParseCode : std::uint8_t {
	Succeeded,
	InvalidUsage,
	UnsupportedSurface,
};

struct TmuxParseResult final {
	TmuxParseCode code{ TmuxParseCode::InvalidUsage };
	std::string diagnosticCode;
	};

struct TmuxCommand final {
	TmuxCommandKind kind{ TmuxCommandKind::Version };
	std::string canonicalName;
	std::optional<std::string> target;
	std::optional<std::string> format;
	std::optional<std::string> filter;
	std::optional<std::string> sessionName;
	std::optional<std::string> windowName;
	std::optional<std::string> directory;
	std::optional<std::string> startLine;
	std::optional<std::string> endLine;
	std::optional<std::string> length;
	std::optional<std::string> percentage;
	std::optional<std::string> channel;
	std::uint16_t repeatCount{ 1 };
	bool print{};
	bool literal{};
	bool joinWrapped{};
	bool horizontal{};
	bool vertical{};
	bool detached{};
	bool printResult{};
	bool all{};
	bool sessionsOnly{};
	bool signal{};
	bool lock{};
	bool unlock{};
	std::vector<std::string> operands;
};

[[nodiscard]] TmuxParseResult ParseTmuxArguments(
	const std::vector<std::string>& argv,
	TmuxCommand& command) noexcept;

enum class TmuxTargetKind : std::uint8_t {
	Session,
	Window,
	Pane,
};

struct TmuxPaneView final {
	TerminalPaneId id;
	TerminalInstanceId instanceId;
	TerminalTargetCoordinate coordinate;
	std::string title;
	std::string currentCommand;
	std::optional<std::uint32_t> deadStatus;
	std::size_t index{};
	std::size_t width{};
	std::size_t height{};
	std::size_t historySize{};
	std::size_t historyLimit{};
	bool active{};
	bool dead{};
};

struct TmuxWindowView final {
	TerminalWindowId id;
	std::string name;
	std::size_t index{};
	std::size_t width{};
	std::size_t height{};
	std::string layout;
	bool active{};
	std::vector<TmuxPaneView> panes;
};

struct TmuxSessionView final {
	TerminalSessionId id;
	std::string name;
	std::size_t index{};
	std::size_t createdSeconds{};
	std::size_t activitySeconds{};
	bool active{};
	bool attached{};
	std::vector<TmuxWindowView> windows;
};

struct TmuxRuntimeSnapshot final {
	TerminalTopologyRevision revision;
	std::optional<TerminalSessionId> activeSession;
	std::vector<TmuxSessionView> sessions;
};

struct TmuxResolvedTarget final {
	TmuxTargetKind kind{ TmuxTargetKind::Pane };
	TerminalTopologyRevision revision;
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
	TerminalPaneId paneId;
	TerminalTargetCoordinate coordinate;
	bool usedStableIdentity{};
};

enum class TmuxRuntimeCode : std::uint8_t {
	Succeeded,
	InvalidRequest,
	TargetMissing,
	TopologyChanged,
	NotRunning,
	Denied,
	DeadlineExceeded,
	ResourceExhausted,
	Ambiguous,
	Unsupported,
	Unavailable,
	Stopped,
	IdentityExhausted,
};

struct TmuxRuntimeResult final {
	TmuxRuntimeCode code{ TmuxRuntimeCode::Unavailable };
	TerminalTopologyRevision revision;
	std::optional<TerminalSessionId> sessionId;
	std::optional<TerminalWindowId> windowId;
	std::optional<TerminalPaneId> paneId;

	[[nodiscard]] bool Succeeded() const noexcept { return code == TmuxRuntimeCode::Succeeded; }
};

struct TmuxCreateSessionRequest final {
	std::string name;
	std::string directory;
	bool detached{ true };
	TerminalTopologyRevision expectedRevision;
};

struct TmuxCreateWindowRequest final {
	TerminalSessionId sessionId;
	std::string name;
	std::string directory;
	bool detached{};
	TerminalTopologyRevision expectedRevision;
};

struct TmuxSplitWindowRequest final {
	TmuxResolvedTarget target;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	std::optional<std::string> directory;
	std::optional<std::string> length;
	std::optional<std::string> percentage;
	bool detached{};
	TerminalTopologyRevision expectedRevision;
};

struct TmuxSelectRequest final {
	TmuxResolvedTarget target;
	TerminalTopologyRevision expectedRevision;
};

struct TmuxCloseRequest final {
	TmuxResolvedTarget target;
	TerminalTopologyRevision expectedRevision;
};

enum class TmuxInputTokenKind : std::uint8_t {
	LiteralText,
	NamedKey,
};

struct TmuxInputToken final {
	TmuxInputTokenKind kind{ TmuxInputTokenKind::LiteralText };
	std::string text;
};

struct TmuxInputBatch final {
	TmuxResolvedTarget target;
	std::vector<TmuxInputToken> tokens;
	std::uint16_t repeatCount{ 1 };
	TerminalTopologyRevision expectedRevision;
};

struct TmuxCaptureRequest final {
	TmuxResolvedTarget target;
	std::optional<std::int64_t> startLine;
	std::optional<std::int64_t> endLine;
	bool startAtHistoryBeginning{};
	bool endAtScreenEnd{};
	bool joinWrapped{};
	TerminalTopologyRevision expectedRevision;
};

struct TmuxCapturedLine final {
	std::string text;
	bool wrapped{};
	bool joined{};
};

struct TmuxCaptureResult final {
	TmuxRuntimeCode code{ TmuxRuntimeCode::Unavailable };
	std::vector<TmuxCapturedLine> lines;
	bool complete{};
	bool gap{};
	bool truncated{};
};

enum class TmuxWaitOperation : std::uint8_t {
	Wait,
	Signal,
	Lock,
	Unlock,
};

struct TmuxWaitRequest final {
	TmuxWaitOperation operation{ TmuxWaitOperation::Wait };
	std::string channel;
	std::chrono::steady_clock::time_point deadline{};
	TerminalTopologyRevision expectedRevision;
};

//! Runtime port injected by the CLI dispatcher. A production implementation
//! adapts these calls to ITerminalRuntimeService; tests can provide a fake
//! without creating an HWND, process, pipe, or terminal session.
class ITmuxRuntimePort {
public:
	virtual ~ITmuxRuntimePort() = default;

	[[nodiscard]] virtual TmuxRuntimeSnapshot Snapshot() const = 0;
	[[nodiscard]] virtual TmuxRuntimeResult CreateSession(const TmuxCreateSessionRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult CreateTerminalWindow(const TmuxCreateWindowRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult SplitWindow(const TmuxSplitWindowRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult SelectWindow(const TmuxSelectRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult SelectPane(const TmuxSelectRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult ClosePane(const TmuxCloseRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult CloseWindow(const TmuxCloseRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult CloseSession(const TmuxCloseRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult SendKeys(const TmuxInputBatch&) = 0;
	[[nodiscard]] virtual TmuxCaptureResult CapturePane(const TmuxCaptureRequest&) = 0;
	[[nodiscard]] virtual TmuxRuntimeResult WaitFor(const TmuxWaitRequest&) = 0;
};

struct TmuxFormatContext final {
	const TmuxSessionView* session{};
	const TmuxWindowView* window{};
	const TmuxPaneView* pane{};
};

enum class TmuxFormatCode : std::uint8_t {
	Succeeded,
	InvalidFormat,
	UnknownVariable,
	ResourceExhausted,
};

struct TmuxFormatResult final {
	TmuxFormatCode code{ TmuxFormatCode::InvalidFormat };
	std::string value;

	[[nodiscard]] bool Succeeded() const noexcept { return code == TmuxFormatCode::Succeeded; }
};

struct TmuxFilterResult final {
	TmuxFormatCode code{ TmuxFormatCode::InvalidFormat };
	bool value{};

	[[nodiscard]] bool Succeeded() const noexcept { return code == TmuxFormatCode::Succeeded; }
};

struct TmuxCompatibilityProfile final {
	std::string listSessionsFormat{ "#{session_name}: #{session_windows} windows" };
	std::string listWindowsFormat{ "#{window_index}: #{window_name} (#{window_panes} panes)" };
	std::string listPanesFormat{ "#{pane_index}: #{pane_id} #{pane_current_command}" };
	std::string displayFormat{ "#{pane_id}" };
};

enum class TmuxCommandResultCode : std::uint8_t {
	Succeeded,
	InvalidUsage,
	UnsupportedSurface,
	TargetMissing,
	TopologyChanged,
	Denied,
	Unavailable,
	ResourceExhausted,
	DeadlineExceeded,
	Ambiguous,
	InternalError,
};

struct TmuxCommandResult final {
	TmuxCommandResultCode code{ TmuxCommandResultCode::InternalError };
	std::string stdoutText;
	std::string diagnosticCode;
	std::optional<TerminalSessionId> sessionId;
	std::optional<TerminalWindowId> windowId;
	std::optional<TerminalPaneId> paneId;

	[[nodiscard]] bool Succeeded() const noexcept { return code == TmuxCommandResultCode::Succeeded; }
};

} // namespace terminal::tmux
