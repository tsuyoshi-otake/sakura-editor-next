/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/runtime/ITerminalRuntimeService.h"
#include "terminal/runtime/TerminalCaptureIndex.h"
#include "terminal/runtime/TerminalCollectionModel.h"
#include "terminal/runtime/TerminalInputBatch.h"
#include "terminal/runtime/TerminalInstance.h"
#include "terminal/model/TerminalModel.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace terminal {

class SakuraTerminalInputAdapter;
class TerminalModel;

//! Resolves the launch profile without giving the runtime authority access to
//! workbench or window state. The returned options are copied before the
//! optional decorator is called.
using TerminalRuntimeLaunchResolver =
	std::function<std::optional<TerminalLaunchOptions>(TerminalSize, std::wstring_view)>;

//! Adds runtime-owned launch metadata (for example SAKURA_* bridge fields and
//! an installation-owned CLI directory). It runs on a private launch-options
//! copy and therefore cannot mutate a caller's or host process environment.
using TerminalRuntimeLaunchDecorator =
	std::function<void(const TerminalCreateRequest&, TerminalLaunchOptions&)>;

struct TerminalRuntimeServiceDependencies final {
	TerminalRuntimeSessionFactory createSession;
	TerminalRuntimeLaunchResolver resolveLaunch;
	TerminalRuntimeLaunchDecorator decorateLaunch;
	TerminalSize defaultSize{ 80, 24 };
	std::wstring defaultWorkingDirectory;
	std::size_t scrollbackLimit{ TerminalModel::kDefaultScrollbackLines };
	//! Exact profile/editor/bridge identity shared by every target published by
	//! this process runtime. Per-instance fields are overwritten by the runtime.
	TerminalTargetCoordinate coordinateBase;
};

//! Concrete HWND-free authority for terminal instances and their logical
//! session/window/pane topology.
//!
//! The service keeps terminal instances after a UI projection is hidden or
//! rebuilt. An instance is removed from the topology only by an explicit close
//! operation; the instance itself remains owned until its session has crossed
//! the quiescence boundary.
class CTerminalRuntimeService final : public ITerminalRuntimeService {
public:
	explicit CTerminalRuntimeService(
		TerminalRuntimeServiceDependencies dependencies = {},
		TerminalRuntimeGeneration runtimeGeneration = TerminalRuntimeGeneration{ 1 });
	explicit CTerminalRuntimeService(
		TerminalRuntimeSessionFactory createSession,
		TerminalRuntimeGeneration runtimeGeneration = TerminalRuntimeGeneration{ 1 });
	~CTerminalRuntimeService() override;

	CTerminalRuntimeService(const CTerminalRuntimeService&) = delete;
	CTerminalRuntimeService& operator=(const CTerminalRuntimeService&) = delete;

	TerminalCreateResult CreateInstance(const TerminalCreateRequest&) override;
	TerminalTopologyResult CreateSession(const TerminalSessionCreateRequest&) override;
	TerminalTopologyResult CreateTerminalWindow(const TerminalWindowCreateRequest&) override;
	TerminalTopologyResult SplitPane(const TerminalPaneSplitRequest&) override;
	TerminalTopologyResult SelectWindow(const TerminalWindowSelectRequest&) override;
	TerminalTopologyResult SelectPane(const TerminalPaneSelectRequest&) override;
	TerminalTopologyResult ClosePane(const TerminalPaneCloseRequest&) override;
	TerminalTopologyResult CloseWindow(const TerminalWindowCloseRequest&) override;
	TerminalTopologyResult CloseSession(const TerminalSessionCloseRequest&) override;

	TerminalInputResult QueueInputBatch(const TerminalInputBatch&) override;
	TerminalCaptureResult Capture(const TerminalCaptureRequest&) override;
	TerminalSnapshotResult Snapshot(const TerminalSnapshotRequest&) const override;
	TerminalResizeResult Resize(const TerminalResizeRequest&) override;
	[[nodiscard]] std::optional<TerminalBackendProcessIdentity> GetProcessIdentity(
		const TerminalTargetCoordinate&) const noexcept override;
	[[nodiscard]] bool OwnsProcess(
		const TerminalTargetCoordinate&, std::uint32_t, std::uint64_t) const noexcept override;
	[[nodiscard]] std::optional<TerminalBackendProcessIdentity> GetProcessIdentity(
		TerminalInstanceId) const noexcept override;
	[[nodiscard]] bool OwnsProcess(
		TerminalInstanceId, std::uint32_t, std::uint64_t) const noexcept override;
	TerminalInstanceDrainResult DrainOutput(TerminalInstanceId) override;

	TerminalSubscription Subscribe(TerminalRuntimeEventCallback) override;
	void BeginClose() noexcept override;
	TerminalRuntimeCloseResult WaitForClose(
		std::chrono::steady_clock::time_point absoluteDeadline) noexcept override;

	//! Projection adapters use these borrowed views only on the serialized
	//! runtime/UI executor. The service retains ownership for their entire
	//! lifetime, including while a projection is hidden.
	[[nodiscard]] TerminalInstance* Instance(TerminalInstanceId) noexcept;
	[[nodiscard]] const TerminalInstance* Instance(TerminalInstanceId) const noexcept;
	void BeginCloseInstance(
		TerminalInstanceId,
		TerminalInstanceCloseReason reason = TerminalInstanceCloseReason::Explicit) noexcept;
	[[nodiscard]] TerminalInstanceCloseWaitResult WaitForInstanceClose(
		TerminalInstanceId,
		std::chrono::steady_clock::time_point absoluteDeadline) noexcept;
	[[nodiscard]] TerminalModel* Model(TerminalInstanceId) noexcept;
	[[nodiscard]] const TerminalModel* Model(TerminalInstanceId) const noexcept;
	[[nodiscard]] SakuraTerminalInputAdapter* InputAdapter(TerminalInstanceId) noexcept;
	[[nodiscard]] const SakuraTerminalInputAdapter* InputAdapter(TerminalInstanceId) const noexcept;
	[[nodiscard]] TerminalRuntimeGeneration RuntimeGeneration() const noexcept;
	[[nodiscard]] std::optional<runtime::topology::TerminalCollectionSnapshot> CollectionSnapshot() const;

private:
	struct Impl;
	std::shared_ptr<Impl> m_impl;
};

using TerminalRuntimeService = CTerminalRuntimeService;

} // namespace terminal
