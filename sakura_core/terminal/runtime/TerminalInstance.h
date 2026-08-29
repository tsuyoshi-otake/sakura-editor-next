/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace terminal {

class SakuraTerminalInputAdapter;
class TerminalModel;

//! Runtime-owned terminal process, parser, model, and input authority.
//!
//! The object has no HWND or panel lifetime. Session callbacks capture only a
//! weak shared implementation, so an old session cannot notify a replacement
//! instance after its owning projection has gone away.
class TerminalInstance final {
public:
	TerminalInstance(
		TerminalInstanceId instanceId,
		TerminalRuntimeGeneration runtimeGeneration,
		std::uint64_t instanceGeneration,
		TerminalCreateRequest request,
		TerminalInstanceDependencies dependencies,
		TerminalInstanceEventCallback eventCallback = {});
	~TerminalInstance();

	TerminalInstance(const TerminalInstance&) = delete;
	TerminalInstance& operator=(const TerminalInstance&) = delete;

	//! Starts the injected CTerminalSession exactly once.
	[[nodiscard]] TerminalInstanceStartResult Start();
	//! Replaces the launch options only before the first start attempt, then
	//! starts the instance. This overload keeps composition code explicit while
	//! retaining the request metadata owned by the instance.
	[[nodiscard]] TerminalInstanceStartResult Start(const TerminalLaunchOptions& options);

	//! Requests close without waiting. The session owns all worker joins.
	void BeginClose(TerminalInstanceCloseReason reason = TerminalInstanceCloseReason::Explicit) noexcept;
	void RequestCancel() noexcept;
	void NotifyHostLost() noexcept;
	//! Waits for session quiescence using one absolute deadline. A quiescent
	//! result never means that a backend or I/O worker was detached.
	[[nodiscard]] TerminalInstanceCloseWaitResult WaitForClose(
		std::chrono::steady_clock::time_point absoluteDeadline) noexcept;
	//! External close convenience. Callback/worker callers receive InProgress
	//! through WaitForClose and leave finalization to an external owner.
	void Close() noexcept;

	[[nodiscard]] TerminalInstanceId Id() const noexcept;
	[[nodiscard]] TerminalRuntimeGeneration RuntimeGeneration() const noexcept;
	[[nodiscard]] std::uint64_t InstanceGeneration() const noexcept;
	[[nodiscard]] TerminalInstanceOrigin Origin() const noexcept;
	[[nodiscard]] TerminalChildEnvironmentPolicy EnvironmentPolicy() const noexcept;
	[[nodiscard]] TerminalInstanceState State() const noexcept;
	[[nodiscard]] TerminalSessionState SessionState() const noexcept;
	[[nodiscard]] std::uint32_t LastError() const noexcept;
	[[nodiscard]] std::optional<TerminalInstanceOutcome> Outcome() const;
	[[nodiscard]] TerminalInstanceSnapshot Snapshot() const;
	//! Returns the session/backend root-process identity while this instance is
	//! live. Terminalized instances fail closed.
	[[nodiscard]] std::optional<TerminalBackendProcessIdentity> GetProcessIdentity() const noexcept;
	//! Verifies PID and creation time through the injected backend/session seam.
	//! Invalid input, unavailable backends, and terminalized instances return
	//! false.
	[[nodiscard]] bool OwnsProcess(
		std::uint32_t processId, std::uint64_t creationTime) const noexcept;

	//! These borrowed model/input views are valid only on the runtime's serialized
	//! UI executor. A future projection lease will add generation-checked detach.
	[[nodiscard]] const TerminalModel* Model() const noexcept;
	[[nodiscard]] TerminalModel* Model() noexcept;
	[[nodiscard]] const SakuraTerminalInputAdapter* InputAdapter() const noexcept;
	[[nodiscard]] SakuraTerminalInputAdapter* InputAdapter() noexcept;

	[[nodiscard]] TerminalInstanceDrainResult DrainOutput();
	[[nodiscard]] TerminalQueueInputResult QueueInput(
		std::span<const std::uint8_t> bytes,
		TerminalInputSource source = TerminalInputSource::Interactive);
	[[nodiscard]] TerminalQueueInputResult FlushPendingProtocolInput();
	[[nodiscard]] bool HasPendingProtocolInput() const noexcept;
	[[nodiscard]] bool ProtocolInputRejected() const noexcept;
	[[nodiscard]] TerminalInstanceResizeResult Resize(TerminalSize size);
	void RecordViewportDiagnostic(const TerminalViewportDiagnosticSnapshot& snapshot) noexcept;

private:
	struct Impl;
	std::shared_ptr<Impl> m_impl;
};

} // namespace terminal
