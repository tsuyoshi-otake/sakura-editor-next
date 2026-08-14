/*! @file
	@brief Presentation-neutral editor-process startup and shutdown lifecycle.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace editor::lifecycle {

//! Ordered composition checkpoints for one editor-process lifetime.  A phase
//! may depend only on an earlier phase; its finalizer runs in reverse order.
enum class EEditorAppLifecyclePhase : std::uint8_t {
	ProfileResolution,
	PlatformServices,
	StorageMigration,
	WorkspaceConfiguration,
	WorkbenchCreation,
	Restore,
	UiServices,
	Ready,
};

enum class EEditorAppLifecycleState : std::uint8_t {
	Stopped,
	Starting,
	Running,
	Stopping,
};

//! A phase callback always declares one observable completion.  No callback
//! may leave a phase in an implicit "maybe started" state.
enum class EEditorAppLifecyclePhaseOutcome : std::uint8_t {
	Succeeded,
	Cancelled,
	TimedOut,
	Failed,
};

//! A phase finalizer is best-effort, but is still required to publish whether
//! a forced shutdown was necessary.
enum class EEditorAppLifecycleFinalizationOutcome : std::uint8_t {
	Succeeded,
	TimedOut,
	Failed,
};

enum class EEditorAppLifecycleResultCode : std::uint8_t {
	Started,
	AlreadyRunning,
	Stopped,
	AlreadyStopped,
	StopDeferred,
	StopInProgress,
	Cancelled,
	TimedOut,
	PhaseFailed,
	ForcedShutdown,
	InvalidPlan,
	UnexpectedFailure,
};

class EditorAppLifecyclePhaseResult final {
public:
	constexpr explicit EditorAppLifecyclePhaseResult(EEditorAppLifecyclePhaseOutcome outcome) noexcept
		: m_outcome(outcome)
	{
	}

	[[nodiscard]] constexpr EEditorAppLifecyclePhaseOutcome Outcome() const noexcept { return m_outcome; }

private:
	const EEditorAppLifecyclePhaseOutcome m_outcome;
};

class EditorAppLifecycleFinalizationResult final {
public:
	constexpr explicit EditorAppLifecycleFinalizationResult(EEditorAppLifecycleFinalizationOutcome outcome) noexcept
		: m_outcome(outcome)
	{
	}

	[[nodiscard]] constexpr EEditorAppLifecycleFinalizationOutcome Outcome() const noexcept { return m_outcome; }

private:
	const EEditorAppLifecycleFinalizationOutcome m_outcome;
};

//! Every entered phase is finalized by its declared phase finalizer.  The
//! lifecycle invokes it even when Start returns Cancelled, TimedOut, or Failed,
//! so a phase owns all resources acquired before it reports its terminal result.
class EditorAppLifecyclePhaseDefinition final {
public:
	EditorAppLifecyclePhaseDefinition(
		EEditorAppLifecyclePhase phase,
		std::function<EditorAppLifecyclePhaseResult()> start,
		std::function<EditorAppLifecycleFinalizationResult()> finalize)
		: m_phase(phase)
		, m_start(std::move(start))
		, m_finalize(std::move(finalize))
	{
	}

	[[nodiscard]] EEditorAppLifecyclePhase Phase() const noexcept { return m_phase; }
	[[nodiscard]] bool HasStartCallback() const noexcept { return static_cast<bool>(m_start); }
	[[nodiscard]] bool HasFinalizeCallback() const noexcept { return static_cast<bool>(m_finalize); }
	[[nodiscard]] EditorAppLifecyclePhaseResult StartPhase() const { return m_start(); }
	[[nodiscard]] EditorAppLifecycleFinalizationResult FinalizePhase() const { return m_finalize(); }

private:
	const EEditorAppLifecyclePhase m_phase;
	const std::function<EditorAppLifecyclePhaseResult()> m_start;
	const std::function<EditorAppLifecycleFinalizationResult()> m_finalize;
};

class EditorAppLifecycleResult final {
public:
	EditorAppLifecycleResult(
		EEditorAppLifecycleResultCode code,
		EEditorAppLifecycleState state,
		std::optional<EEditorAppLifecyclePhase> phase = std::nullopt,
		bool forcedShutdown = false) noexcept
		: m_code(code)
		, m_state(state)
		, m_phase(phase)
		, m_forcedShutdown(forcedShutdown)
	{
	}

	[[nodiscard]] EEditorAppLifecycleResultCode Code() const noexcept { return m_code; }
	[[nodiscard]] EEditorAppLifecycleState State() const noexcept { return m_state; }
	[[nodiscard]] const std::optional<EEditorAppLifecyclePhase>& Phase() const noexcept { return m_phase; }
	[[nodiscard]] bool ForcedShutdown() const noexcept { return m_forcedShutdown; }

	[[nodiscard]] bool IsRunning() const noexcept
	{
		return m_code == EEditorAppLifecycleResultCode::Started
			|| m_code == EEditorAppLifecycleResultCode::AlreadyRunning;
	}

private:
	const EEditorAppLifecycleResultCode m_code;
	const EEditorAppLifecycleState m_state;
	const std::optional<EEditorAppLifecyclePhase> m_phase;
	const bool m_forcedShutdown;
};

//! Serializes one editor-process composition plan. Callbacks execute without
//! the lifecycle mutex held. A callback may safely call Stop(): that request is
//! deferred to the outer Start() call, which owns reverse finalization and
//! returns only after the lifecycle reaches Stopped.
class EditorAppLifecycle final {
public:
	explicit EditorAppLifecycle(std::vector<EditorAppLifecyclePhaseDefinition> phases);
	~EditorAppLifecycle() noexcept;

	EditorAppLifecycle(const EditorAppLifecycle&) = delete;
	EditorAppLifecycle& operator=(const EditorAppLifecycle&) = delete;

	[[nodiscard]] EditorAppLifecycleResult Start();
	[[nodiscard]] EditorAppLifecycleResult Stop() noexcept;
	[[nodiscard]] EEditorAppLifecycleState State() noexcept;
	[[nodiscard]] std::optional<EditorAppLifecycleResult> LastTerminalResult();
	[[nodiscard]] std::size_t ActivePhaseCount() noexcept;

private:
	[[nodiscard]] bool IsValidPlan() const noexcept;
	[[nodiscard]] EditorAppLifecycleResult FinishStop(
		EEditorAppLifecycleResultCode requestedCode,
		std::optional<EEditorAppLifecyclePhase> phase) noexcept;
	[[nodiscard]] EditorAppLifecycleResult MakeResult(EEditorAppLifecycleResultCode code,
		EEditorAppLifecycleState state, std::optional<EEditorAppLifecyclePhase> phase = std::nullopt,
		bool forcedShutdown = false) const noexcept;

	const std::vector<EditorAppLifecyclePhaseDefinition> m_phases;
	std::mutex m_mutex;
	std::vector<std::size_t> m_activePhaseIndices;
	EEditorAppLifecycleState m_state = EEditorAppLifecycleState::Stopped;
	bool m_stopRequested = false;
	std::optional<EditorAppLifecycleResult> m_lastTerminalResult;
};

} // namespace editor::lifecycle
