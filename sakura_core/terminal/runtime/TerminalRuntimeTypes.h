/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"
#include "terminal/session/TerminalSession.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <compare>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terminal {

//! Numeric IDs are intentionally not interchangeable, even though their wire
//! representation is an unsigned 64-bit value. Zero is reserved as invalid.
template<typename Tag>
struct TerminalNumericId final {
	std::uint64_t value{};

	[[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }
	explicit constexpr operator bool() const noexcept { return IsValid(); }
	friend constexpr bool operator==(const TerminalNumericId&, const TerminalNumericId&) noexcept = default;
	friend constexpr auto operator<=>(const TerminalNumericId&, const TerminalNumericId&) noexcept = default;
};

struct ProfileAuthorityGenerationTag;
struct BridgeEpochTag;
struct TerminalRuntimeGenerationTag;
struct TerminalSessionIdTag;
struct TerminalWindowIdTag;
struct TerminalPaneIdTag;
struct TerminalInstanceIdTag;
struct TerminalTopologyRevisionTag;
struct TerminalContentRevisionTag;
struct HarnessRequestIdTag;

using ProfileAuthorityGeneration = TerminalNumericId<ProfileAuthorityGenerationTag>;
using BridgeEpoch = TerminalNumericId<BridgeEpochTag>;
using TerminalRuntimeGeneration = TerminalNumericId<TerminalRuntimeGenerationTag>;
using TerminalSessionId = TerminalNumericId<TerminalSessionIdTag>;
using TerminalWindowId = TerminalNumericId<TerminalWindowIdTag>;
using TerminalPaneId = TerminalNumericId<TerminalPaneIdTag>;
using TerminalInstanceId = TerminalNumericId<TerminalInstanceIdTag>;
using TerminalTopologyRevision = TerminalNumericId<TerminalTopologyRevisionTag>;
using TerminalContentRevision = TerminalNumericId<TerminalContentRevisionTag>;
using HarnessRequestId = TerminalNumericId<HarnessRequestIdTag>;

struct EditorInstanceId final {
	std::array<std::uint8_t, 16> value{};

	[[nodiscard]] bool IsValid() const noexcept
	{
		for (const auto byte : value) if (byte != 0) return true;
		return false;
	}
	friend constexpr bool operator==(const EditorInstanceId&, const EditorInstanceId&) noexcept = default;
};

struct BridgeId final {
	std::array<std::uint8_t, 16> value{};

	[[nodiscard]] bool IsValid() const noexcept
	{
		for (const auto byte : value) if (byte != 0) return true;
		return false;
	}
	friend constexpr bool operator==(const BridgeId&, const BridgeId&) noexcept = default;
};

struct HarnessOperationId final {
	std::array<std::uint8_t, 16> value{};

	[[nodiscard]] bool IsValid() const noexcept
	{
		for (const auto byte : value) if (byte != 0) return true;
		return false;
	}
	friend constexpr bool operator==(const HarnessOperationId&, const HarnessOperationId&) noexcept = default;
};

struct HarnessMessageId final {
	std::array<std::uint8_t, 16> value{};

	[[nodiscard]] bool IsValid() const noexcept
	{
		for (const auto byte : value) if (byte != 0) return true;
		return false;
	}
	friend constexpr bool operator==(const HarnessMessageId&, const HarnessMessageId&) noexcept = default;
};

struct HarnessRunId final {
	std::array<std::uint8_t, 16> value{};

	[[nodiscard]] bool IsValid() const noexcept
	{
		for (const auto byte : value) if (byte != 0) return true;
		return false;
	}
	friend constexpr bool operator==(const HarnessRunId&, const HarnessRunId&) noexcept = default;
};

struct TerminalTargetCoordinate final {
	std::string profileId;
	ProfileAuthorityGeneration profileGeneration;
	EditorInstanceId editorId;
	BridgeEpoch bridgeEpoch;
	TerminalRuntimeGeneration runtimeGeneration;
	std::uint64_t instanceGeneration{};
	TerminalSessionId sessionId;
	TerminalWindowId windowId;
	TerminalPaneId paneId;
	TerminalInstanceId instanceId;

	friend bool operator==(const TerminalTargetCoordinate&, const TerminalTargetCoordinate&) = default;
};

enum class TerminalInstanceOrigin : std::uint8_t {
	Interactive,
	Task,
};

enum class TerminalChildEnvironmentPolicy : std::uint8_t {
	InteractiveWithHarnessShim,
	TaskWithoutHarnessShim,
};

enum class TerminalInstanceCloseReason : std::uint8_t {
	None,
	Explicit,
	Cancel,
	HostLost,
	Shutdown,
};

enum class TerminalInstanceState : std::uint8_t {
	Reserved,
	Starting,
	Running,
	Closing,
	Terminalized,
	Retired,
};

enum class TerminalInstanceOutcomeKind : std::uint8_t {
	StartFailed,
	StartCancelled,
	Exited,
	Cancelled,
	Closed,
	ForcedClosed,
	HostLost,
	Failed,
	ShutdownDeadlineExceeded,
};

//! Durable instance outcome. It is published only after the session's backend
//! and I/O workers have reached their quiescence boundary.
struct TerminalInstanceOutcome final {
	TerminalInstanceOutcomeKind kind{ TerminalInstanceOutcomeKind::Failed };
	std::optional<std::uint32_t> processExitCode;
	std::optional<std::uint32_t> platformErrorCode;
	bool backendQuiesced{};
	bool readerQuiesced{};
	bool writerQuiesced{};
	TerminalContentRevision finalContentRevision;

	[[nodiscard]] bool IsQuiescent() const noexcept
	{
		return backendQuiesced && readerQuiesced && writerQuiesced;
	}
};

struct TerminalCreateRequest final {
	HarnessOperationId operationId;
	//! Reserved identity is supplied to launch decoration before the session is
	//! constructed. It fences bridge metadata from a late replacement.
	TerminalInstanceId instanceId;
	std::uint64_t instanceGeneration{};
	TerminalInstanceOrigin origin{ TerminalInstanceOrigin::Interactive };
	TerminalChildEnvironmentPolicy environmentPolicy{ TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim };
	TerminalSessionId sessionId;
	std::optional<TerminalWindowId> windowId;
	TerminalPaneId paneId;
	TerminalLaunchOptions launch;
	std::optional<std::string> taskRunId;
};

struct TerminalInstanceSnapshot final {
	TerminalTargetCoordinate coordinate;
	TerminalInstanceOrigin origin{ TerminalInstanceOrigin::Interactive };
	TerminalChildEnvironmentPolicy environmentPolicy{ TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim };
	std::optional<std::string> taskRunId;
	TerminalInstanceState state{ TerminalInstanceState::Reserved };
	TerminalSessionState sessionState{ TerminalSessionState::Idle };
	std::uint32_t errorCode{};
	TerminalContentRevision contentRevision;
	std::wstring processName;
	std::wstring profileLabel;
	std::wstring sequenceTitle;
	std::wstring initialWorkingDirectory;
	std::size_t columns{};
	std::size_t rows{};
	std::size_t scrollbackSize{};
	std::size_t scrollbackLimit{};
	bool alternateScreen{};
	std::optional<TerminalInstanceOutcome> outcome;
};

enum class TerminalInstanceStartStatus : std::uint8_t {
	Started,
	StartFailed,
	StartCancelled,
	AlreadyStarted,
	Unavailable,
};

struct TerminalInstanceStartResult final {
	TerminalInstanceStartStatus status{ TerminalInstanceStartStatus::Unavailable };
	std::uint32_t errorCode{};
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return status == TerminalInstanceStartStatus::Started; }
	[[nodiscard]] bool Cancelled() const noexcept { return status == TerminalInstanceStartStatus::StartCancelled; }
};

enum class TerminalInstanceCloseWaitStatus : std::uint8_t {
	Closed,
	DeadlineExceeded,
	InProgress,
	Unavailable,
};

struct TerminalInstanceCloseWaitResult final {
	TerminalInstanceCloseWaitStatus status{ TerminalInstanceCloseWaitStatus::Unavailable };
	std::optional<TerminalInstanceOutcome> outcome;

	[[nodiscard]] bool IsQuiescent() const noexcept
	{
		return status == TerminalInstanceCloseWaitStatus::Closed
			|| status == TerminalInstanceCloseWaitStatus::DeadlineExceeded;
	}
};

enum class TerminalInstanceEventKind : std::uint8_t {
	OutputAvailable,
	StateChanged,
	Completed,
};

struct TerminalInstanceEvent final {
	TerminalInstanceEventKind kind{ TerminalInstanceEventKind::StateChanged };
	TerminalTargetCoordinate coordinate;
	TerminalInstanceState state{ TerminalInstanceState::Reserved };
	TerminalSessionState sessionState{ TerminalSessionState::Idle };
	std::uint32_t errorCode{};
	std::optional<TerminalInstanceOutcome> outcome;
};

using TerminalInstanceEventCallback = std::function<void(const TerminalInstanceEvent&)>;
using TerminalRuntimeEventCallback = TerminalInstanceEventCallback;

//! Session construction is injected so unit tests can supply a fake backend
//! through CTerminalSession without making runtime code depend on ConPTY.
using TerminalRuntimeSessionFactory = std::function<std::unique_ptr<CTerminalSession>(TerminalSessionCallbacks)>;

struct TerminalInstanceDependencies final {
	TerminalRuntimeSessionFactory createSession;
	//! Immutable process authority coordinates copied into every instance before
	//! its reserved instance/session/window/pane coordinates are applied.
	TerminalTargetCoordinate coordinateBase;
};

struct TerminalInstanceDrainResult final {
	bool found{};
	bool sessionRunning{};
	bool sequenceChanged{};
	bool synchronizedOutputCommitted{};
	bool protocolInputPending{};
	bool protocolInputRejected{};
	std::size_t bytesDrained{};
	TerminalScrollbackChange scrollbackChange;
	std::vector<std::size_t> dirtyRows;
	TerminalContentRevision contentRevision;
};

struct TerminalInstanceResizeResult final {
	bool succeeded{};
	std::uint32_t errorCode{};
};

enum class TerminalInputActionKind : std::uint8_t {
	LiteralText,
	NamedKey,
	PasteText,
};

enum class TerminalNamedKey : std::uint8_t {
	Enter,
	Escape,
	Tab,
	BSpace,
	Space,
	Up,
	Down,
	Left,
	Right,
	Home,
	End,
	PageUp,
	PageDown,
	Insert,
	Delete,
	F1,
	F2,
	F3,
	F4,
	F5,
	F6,
	F7,
	F8,
	F9,
	F10,
	F11,
	F12,
};

struct TerminalInputAction final {
	TerminalInputActionKind kind{ TerminalInputActionKind::LiteralText };
	std::u16string text;
	TerminalNamedKey key{ TerminalNamedKey::Enter };
};

struct TerminalInputBatch final {
	HarnessOperationId operationId;
	TerminalTargetCoordinate target;
	std::vector<TerminalInputAction> actions;
	std::uint16_t repeatCount{ 1 };
	std::chrono::steady_clock::time_point deadline{};
};

enum class TerminalInputResultCode : std::uint8_t {
	Accepted,
	InvalidInput,
	UnsupportedKey,
	TargetMissing,
	StaleGeneration,
	NotRunning,
	QueueFull,
	Denied,
	DeadlineExceeded,
	BrokerStopping,
	Ambiguous,
};

struct TerminalInputResult final {
	TerminalInputResultCode code{ TerminalInputResultCode::NotRunning };
	std::uint32_t errorCode{};
	TerminalContentRevision contentRevision;
};

struct TerminalCaptureCoordinates final {
	TerminalRuntimeGeneration runtimeGeneration;
	TerminalInstanceId instanceId;
	std::uint64_t instanceGeneration{};
	std::uint64_t screenEpoch{};
	TerminalContentRevision revision;
	std::uint64_t scrollbackBaseOrdinal{};
};

struct TerminalRowRange final {
	std::int64_t first{};
	std::int64_t last{};

	friend constexpr bool operator==(const TerminalRowRange&, const TerminalRowRange&) noexcept = default;
};

struct TerminalCaptureCursor final {
	std::uint8_t version{ 1 };
	TerminalRuntimeGeneration runtimeGeneration;
	TerminalInstanceId instanceId;
	std::uint64_t instanceGeneration{};
	std::uint64_t screenEpoch{};
	TerminalContentRevision revision;
	std::uint64_t scrollbackBaseOrdinal{};

	friend constexpr bool operator==(const TerminalCaptureCursor&, const TerminalCaptureCursor&) noexcept = default;
};

struct TerminalCapturedLine final {
	std::int64_t firstRow{};
	std::int64_t lastRow{};
	bool wrapped{};
	bool joined{};
	std::u16string text;
};

struct TerminalCaptureLimits final {
	std::size_t maximumPhysicalRows{ 4096 };
	std::size_t maximumCodeUnits{ 262144 };
	std::size_t maximumUtf8Bytes{ 512u * 1024u };
	std::chrono::milliseconds uiBudget{ 8 };
};

enum class TerminalCaptureResultCode : std::uint8_t {
	Succeeded,
	TargetMissing,
	StaleCursor,
	InvalidRequest,
	NotRunning,
	DeadlineExceeded,
	ResourceExhausted,
	Denied,
};

enum class TerminalCaptureTruncationReason : std::uint8_t {
	None,
	Rows,
	CodeUnits,
	Utf8Bytes,
	UiBudget,
	Deadline,
};

struct TerminalCaptureRequest final {
	HarnessOperationId operationId;
	TerminalTargetCoordinate target;
	std::optional<std::int64_t> startLine;
	std::optional<std::int64_t> endLine;
	bool joinWrappedLines{};
	std::optional<TerminalCaptureCursor> since;
	TerminalCaptureLimits limits;
	std::chrono::steady_clock::time_point deadline{};
};

struct TerminalCaptureResult final {
	TerminalCaptureResultCode code{ TerminalCaptureResultCode::NotRunning };
	TerminalCaptureCoordinates coordinates;
	std::vector<TerminalCapturedLine> lines;
	TerminalCaptureCursor earliestCursor;
	TerminalCaptureCursor nextCursor;
	bool alternateScreen{};
	bool gap{};
	bool resyncSnapshot{};
	bool truncated{};
	TerminalCaptureTruncationReason truncationReason{ TerminalCaptureTruncationReason::None };
};

//! Common operation status used by topology, snapshot, resize, and runtime
//! service APIs. The richer input/capture results retain their own contracts.
enum class TerminalRuntimeOperationCode : std::uint8_t {
	Succeeded,
	InvalidRequest,
	TargetMissing,
	TopologyChanged,
	NotRunning,
	Denied,
	DeadlineExceeded,
	Cancelled,
	ServerStopping,
	ResourceExhausted,
	OperationUnknown,
	AlreadyTerminal,
	Ambiguous,
	Unsupported,
	InternalError,
};

struct TerminalCreateResult final {
	TerminalRuntimeOperationCode code{ TerminalRuntimeOperationCode::InternalError };
	TerminalInstanceId instanceId;
	std::uint64_t instanceGeneration{};
	std::optional<TerminalInstanceOutcome> outcome;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return code == TerminalRuntimeOperationCode::Succeeded && instanceId.IsValid();
	}
};

struct TerminalSnapshotRequest final {
	TerminalTargetCoordinate target;
};

struct TerminalSnapshotResult final {
	TerminalRuntimeOperationCode code;
	std::optional<TerminalInstanceSnapshot> snapshot;
};

struct TerminalResizeRequest final {
	HarnessOperationId operationId;
	TerminalTargetCoordinate target;
	TerminalSize size;
	std::chrono::steady_clock::time_point deadline{};
};

struct TerminalResizeResult final {
	TerminalRuntimeOperationCode code;
};

struct TerminalTopologyResult final {
	TerminalRuntimeOperationCode code{ TerminalRuntimeOperationCode::InternalError };
	TerminalTopologyRevision revision;
	std::optional<TerminalSessionId> sessionId;
	std::optional<TerminalWindowId> windowId;
	std::optional<TerminalPaneId> paneId;
	std::optional<TerminalInstanceId> instanceId;
};

struct TerminalSessionCreateRequest final {
	HarnessOperationId operationId;
	TerminalSessionId sessionId;
	std::string name;
	bool detached{ true };
	//! Optional per-instance launch requested by a UI projection. The runtime
	//! still resolves and decorates a private copy before session creation.
	std::optional<TerminalLaunchOptions> launch;
};

struct TerminalWindowCreateRequest final {
	HarnessOperationId operationId;
	TerminalSessionId sessionId;
	std::optional<TerminalWindowId> windowId;
	std::string name;
	bool detached{ true };
	std::optional<TerminalLaunchOptions> launch;
};

enum class TerminalPaneOrientation : std::uint8_t {
	Horizontal,
	Vertical,
};

struct TerminalPaneSplitRequest final {
	HarnessOperationId operationId;
	TerminalPaneId paneId;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	bool detached{};
	std::optional<TerminalLaunchOptions> launch;
};

struct TerminalWindowSelectRequest final {
	HarnessOperationId operationId;
	TerminalWindowId windowId;
};

struct TerminalPaneSelectRequest final {
	HarnessOperationId operationId;
	TerminalPaneId paneId;
};

struct TerminalPaneCloseRequest final {
	HarnessOperationId operationId;
	TerminalPaneId paneId;
};

struct TerminalWindowCloseRequest final {
	HarnessOperationId operationId;
	TerminalWindowId windowId;
};

struct TerminalSessionCloseRequest final {
	HarnessOperationId operationId;
	TerminalSessionId sessionId;
};

class TerminalSubscription final {
public:
	TerminalSubscription() noexcept = default;
	explicit TerminalSubscription(std::function<void()> release) noexcept
		: m_release(std::move(release))
	{
	}
	TerminalSubscription(const TerminalSubscription&) = delete;
	TerminalSubscription& operator=(const TerminalSubscription&) = delete;
	TerminalSubscription(TerminalSubscription&& other) noexcept
		: m_release(std::move(other.m_release))
	{
	}
	TerminalSubscription& operator=(TerminalSubscription&& other) noexcept
	{
		if (this != &other) {
			Reset();
			m_release = std::move(other.m_release);
		}
		return *this;
	}
	~TerminalSubscription() { Reset(); }

	explicit operator bool() const noexcept { return static_cast<bool>(m_release); }
	void Reset() noexcept
	{
		if (!m_release) return;
		try {
			m_release();
		} catch (...) {
		}
		m_release = {};
	}

private:
	std::function<void()> m_release;
};

enum class TerminalRuntimeCloseWaitStatus : std::uint8_t {
	Closed,
	DeadlineExceeded,
	InProgress,
	Unavailable,
};

struct TerminalRuntimeCloseResult final {
	TerminalRuntimeCloseWaitStatus status{ TerminalRuntimeCloseWaitStatus::Unavailable };

	[[nodiscard]] bool IsQuiescent() const noexcept
	{
		return status == TerminalRuntimeCloseWaitStatus::Closed
			|| status == TerminalRuntimeCloseWaitStatus::DeadlineExceeded;
	}
};

} // namespace terminal
