/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"

namespace terminal {

//! HWND-free authority boundary for interactive and task terminal instances.
//!
//! Implementations own instance/session/model/parser lifetime and serialize
//! model mutation on the editor UI executor. Window, bridge, and task layers
//! consume this interface; none of them owns a CTerminalSession directly.
class ITerminalRuntimeService {
public:
	virtual ~ITerminalRuntimeService() = default;

	virtual TerminalCreateResult CreateInstance(const TerminalCreateRequest&) = 0;
	virtual TerminalTopologyResult CreateSession(const TerminalSessionCreateRequest&) = 0;
	virtual TerminalTopologyResult CreateTerminalWindow(const TerminalWindowCreateRequest&) = 0;
	virtual TerminalTopologyResult SplitPane(const TerminalPaneSplitRequest&) = 0;
	virtual TerminalTopologyResult SelectWindow(const TerminalWindowSelectRequest&) = 0;
	virtual TerminalTopologyResult SelectPane(const TerminalPaneSelectRequest&) = 0;
	virtual TerminalTopologyResult ClosePane(const TerminalPaneCloseRequest&) = 0;
	virtual TerminalTopologyResult CloseWindow(const TerminalWindowCloseRequest&) = 0;
	virtual TerminalTopologyResult CloseSession(const TerminalSessionCloseRequest&) = 0;

	virtual TerminalInputResult QueueInputBatch(const TerminalInputBatch&) = 0;
	virtual TerminalCaptureResult Capture(const TerminalCaptureRequest&) = 0;
	virtual TerminalSnapshotResult Snapshot(const TerminalSnapshotRequest&) const = 0;
	virtual TerminalResizeResult Resize(const TerminalResizeRequest&) = 0;

	//! Returns the backend-owned root-process identity for a current target.
	//! Implementations must fail closed when the target is stale, the instance
	//! is terminalized, or the backend cannot prove identity.
	virtual std::optional<TerminalBackendProcessIdentity> GetProcessIdentity(
		const TerminalTargetCoordinate&) const noexcept
	{
		return std::nullopt;
	}
	//! Verifies PID plus creation time against the backend-owned process/job
	//! boundary for a current target. A false result is the only safe fallback.
	virtual bool OwnsProcess(
		const TerminalTargetCoordinate&, std::uint32_t, std::uint64_t) const noexcept
	{
		return false;
	}
	//! Instance-ID forms are for same-runtime callers that already hold the
	//! instance identity. They still fail closed for an unknown/terminalized
	//! instance and do not bypass the target-coordinate forms.
	virtual std::optional<TerminalBackendProcessIdentity> GetProcessIdentity(
		TerminalInstanceId) const noexcept
	{
		return std::nullopt;
	}
	virtual bool OwnsProcess(
		TerminalInstanceId, std::uint32_t, std::uint64_t) const noexcept
	{
		return false;
	}

	//! Drains the bounded session queue and applies parser/model mutation on the
	//! runtime executor. A renderer is not required for this operation.
	virtual TerminalInstanceDrainResult DrainOutput(TerminalInstanceId) = 0;

	virtual TerminalSubscription Subscribe(TerminalRuntimeEventCallback) = 0;
	virtual void BeginClose() noexcept = 0;
	virtual TerminalRuntimeCloseResult WaitForClose(
		std::chrono::steady_clock::time_point absoluteDeadline) noexcept = 0;
};

} // namespace terminal
