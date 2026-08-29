/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/runtime/ITerminalRuntimeService.h"
#include "terminal/runtime/TerminalCollectionModel.h"
#include "terminal/tmux/TmuxCommandTypes.h"
#include "terminal/tmux/TmuxWaitChannelService.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

namespace terminal::tmux {

struct TmuxRuntimeAdapterLimits final {
	std::size_t maximumInputBytes{ 64u * 1024u };
	TerminalCaptureLimits captureLimits;
	std::chrono::seconds captureTimeout{ 300 };
	std::chrono::seconds maximumWait{ 300 };
};

//! Thread-safe, process-local operation identity allocator.
//!
//! IDs are monotonically allocated from one atomic sequence shared by all
//! adapter instances. Zero is never returned; exhaustion is terminal rather
//! than wrapping into a reusable operation identity.
class TmuxOperationIdAllocator final {
public:
	[[nodiscard]] std::optional<HarnessOperationId> Allocate() noexcept;
};

using TmuxCollectionSnapshotProvider =
	std::function<std::optional<runtime::topology::TerminalCollectionSnapshot>()>;
using TmuxInstanceSnapshotProvider =
	std::function<std::optional<TerminalInstanceSnapshot>(TerminalInstanceId)>;

//! Bridges the tmux-shaped command port to the HWND-free runtime service.
//!
//! Topology and instance snapshots are injected because the stable service
//! interface intentionally exposes only operation DTOs. Production code can
//! use ForConcrete(CTerminalRuntimeService, ...); tests can supply immutable
//! snapshot callbacks without constructing a process, HWND, or ConPTY.
class TmuxRuntimeAdapter final : public ITmuxRuntimePort {
public:
	TmuxRuntimeAdapter(
		ITerminalRuntimeService& runtime,
		TmuxCollectionSnapshotProvider collectionSnapshot,
		TmuxInstanceSnapshotProvider instanceSnapshot,
		TmuxWaitChannelService& waitChannels,
		TmuxRuntimeAdapterLimits limits = {});
	~TmuxRuntimeAdapter() override;

	TmuxRuntimeAdapter(const TmuxRuntimeAdapter&) = delete;
	TmuxRuntimeAdapter& operator=(const TmuxRuntimeAdapter&) = delete;

	//! Convenience factory for CTerminalRuntimeService or a compatible
	//! concrete service exposing CollectionSnapshot() and Instance().
	template<typename ConcreteRuntime>
	[[nodiscard]] static TmuxRuntimeAdapter ForConcrete(
		ConcreteRuntime& runtime,
		TmuxWaitChannelService& waitChannels,
		TmuxRuntimeAdapterLimits limits = {})
	{
		return TmuxRuntimeAdapter(
			static_cast<ITerminalRuntimeService&>(runtime),
			[&runtime] { return runtime.CollectionSnapshot(); },
			[&runtime](const TerminalInstanceId id) -> std::optional<TerminalInstanceSnapshot> {
				const auto* instance = runtime.Instance(id);
				return instance ? std::optional<TerminalInstanceSnapshot>(instance->Snapshot()) : std::nullopt;
			},
			waitChannels, std::move(limits));
	}

	[[nodiscard]] TmuxRuntimeSnapshot Snapshot() const override;
	[[nodiscard]] TmuxRuntimeResult CreateSession(const TmuxCreateSessionRequest&) override;
	[[nodiscard]] TmuxRuntimeResult CreateTerminalWindow(const TmuxCreateWindowRequest&) override;
	[[nodiscard]] TmuxRuntimeResult SplitWindow(const TmuxSplitWindowRequest&) override;
	[[nodiscard]] TmuxRuntimeResult SelectWindow(const TmuxSelectRequest&) override;
	[[nodiscard]] TmuxRuntimeResult SelectPane(const TmuxSelectRequest&) override;
	[[nodiscard]] TmuxRuntimeResult ClosePane(const TmuxCloseRequest&) override;
	[[nodiscard]] TmuxRuntimeResult CloseWindow(const TmuxCloseRequest&) override;
	[[nodiscard]] TmuxRuntimeResult CloseSession(const TmuxCloseRequest&) override;
	[[nodiscard]] TmuxRuntimeResult SendKeys(const TmuxInputBatch&) override;
	[[nodiscard]] TmuxCaptureResult CapturePane(const TmuxCaptureRequest&) override;
	[[nodiscard]] TmuxRuntimeResult WaitFor(const TmuxWaitRequest&) override;

	//! Makes waiters terminalize before the owning runtime is destroyed.
	void BeginShutdown() noexcept;

private:
	[[nodiscard]] std::optional<runtime::topology::TerminalCollectionSnapshot> CollectionSnapshot() const;
	[[nodiscard]] std::optional<TerminalInstanceSnapshot> InstanceSnapshot(TerminalInstanceId) const;
	[[nodiscard]] TmuxRuntimeResult CheckRevision(TerminalTopologyRevision) const;
	[[nodiscard]] std::optional<HarnessOperationId> OperationId() noexcept;
	[[nodiscard]] static TerminalTargetCoordinate TargetCoordinate(const TmuxResolvedTarget&);
	[[nodiscard]] TmuxRuntimeResult MapTopology(const TerminalTopologyResult&);
	[[nodiscard]] TmuxRuntimeResult MapInput(const TerminalInputResult&);
	[[nodiscard]] static TmuxCaptureResult MapCapture(const TerminalCaptureResult&, const TmuxRuntimeAdapterLimits&);

	ITerminalRuntimeService& m_runtime;
	TmuxCollectionSnapshotProvider m_collectionSnapshot;
	TmuxInstanceSnapshotProvider m_instanceSnapshot;
	TmuxWaitChannelService& m_waitChannels;
	TmuxRuntimeAdapterLimits m_limits;
	TmuxOperationIdAllocator m_operationIds;
};

} // namespace terminal::tmux
