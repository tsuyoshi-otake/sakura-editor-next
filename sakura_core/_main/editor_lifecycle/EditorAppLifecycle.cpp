/*! @file
	@brief Presentation-neutral editor-process startup and shutdown lifecycle.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include <sakura/editor/lifecycle/EditorAppLifecycle.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <utility>

namespace editor::lifecycle {
namespace {

EEditorAppLifecycleResultCode ResultCodeFor(EEditorAppLifecyclePhaseOutcome outcome) noexcept
{
	switch (outcome) {
	case EEditorAppLifecyclePhaseOutcome::Succeeded:
		return EEditorAppLifecycleResultCode::Started;
	case EEditorAppLifecyclePhaseOutcome::Cancelled:
		return EEditorAppLifecycleResultCode::Cancelled;
	case EEditorAppLifecyclePhaseOutcome::TimedOut:
		return EEditorAppLifecycleResultCode::TimedOut;
	case EEditorAppLifecyclePhaseOutcome::Failed:
		return EEditorAppLifecycleResultCode::PhaseFailed;
	}
	return EEditorAppLifecycleResultCode::UnexpectedFailure;
}

} // namespace

EditorAppLifecycle::EditorAppLifecycle(std::vector<EditorAppLifecyclePhaseDefinition> phases)
	: m_phases(std::move(phases))
{
}

EditorAppLifecycle::~EditorAppLifecycle() noexcept
{
	(void)Stop();
}

bool EditorAppLifecycle::IsValidPlan() const noexcept
{
	if (m_phases.empty()) return false;
	for (std::size_t index = 0; index < m_phases.size(); ++index) {
		const auto& phase = m_phases[index];
		if (!phase.HasStartCallback() || !phase.HasFinalizeCallback()) return false;
		if (index != 0 && static_cast<std::uint8_t>(m_phases[index - 1].Phase()) >= static_cast<std::uint8_t>(phase.Phase())) {
			return false;
		}
	}
	return true;
}

EditorAppLifecycleResult EditorAppLifecycle::MakeResult(EEditorAppLifecycleResultCode code,
	EEditorAppLifecycleState state, std::optional<EEditorAppLifecyclePhase> phase, bool forcedShutdown) const noexcept
{
	return EditorAppLifecycleResult(code, state, phase, forcedShutdown);
}

EditorAppLifecycleResult EditorAppLifecycle::Start()
{
	{
		std::scoped_lock lock(m_mutex);
		if (m_state == EEditorAppLifecycleState::Running) {
			return MakeResult(EEditorAppLifecycleResultCode::AlreadyRunning, m_state);
		}
		if (m_state != EEditorAppLifecycleState::Stopped) {
			return MakeResult(EEditorAppLifecycleResultCode::StopInProgress, m_state);
		}
		if (!IsValidPlan()) {
			const auto result = MakeResult(EEditorAppLifecycleResultCode::InvalidPlan, m_state);
			m_lastTerminalResult.emplace(result);
			return result;
		}
		try {
			m_activePhaseIndices.clear();
			m_activePhaseIndices.reserve(m_phases.size());
		}
		catch (const std::exception&) {
			const auto result = MakeResult(EEditorAppLifecycleResultCode::UnexpectedFailure, m_state);
			m_lastTerminalResult.emplace(result);
			return result;
		}
		m_state = EEditorAppLifecycleState::Starting;
		m_stopRequested = false;
	}

	for (std::size_t index = 0; index < m_phases.size(); ++index) {
		std::optional<EditorAppLifecyclePhaseResult> phaseResult;
		try {
			phaseResult.emplace(m_phases[index].StartPhase());
		}
		catch (const std::exception&) {
			phaseResult.emplace(EEditorAppLifecyclePhaseOutcome::Failed);
		}

		bool stopRequested = false;
		{
			std::scoped_lock lock(m_mutex);
			m_activePhaseIndices.push_back(index);
			stopRequested = m_stopRequested;
			if (phaseResult->Outcome() != EEditorAppLifecyclePhaseOutcome::Succeeded || stopRequested) {
				m_state = EEditorAppLifecycleState::Stopping;
			}
		}

		if (phaseResult->Outcome() != EEditorAppLifecyclePhaseOutcome::Succeeded) {
			return FinishStop(ResultCodeFor(phaseResult->Outcome()), m_phases[index].Phase());
		}
		if (stopRequested) {
			return FinishStop(EEditorAppLifecycleResultCode::Cancelled, m_phases[index].Phase());
		}
	}

	{
		std::scoped_lock lock(m_mutex);
		if (!m_stopRequested) {
			m_state = EEditorAppLifecycleState::Running;
			return MakeResult(EEditorAppLifecycleResultCode::Started, m_state);
		}
		m_state = EEditorAppLifecycleState::Stopping;
	}
	return FinishStop(EEditorAppLifecycleResultCode::Cancelled, m_phases.back().Phase());
}

EditorAppLifecycleResult EditorAppLifecycle::FinishStop(
	EEditorAppLifecycleResultCode requestedCode, std::optional<EEditorAppLifecyclePhase> phase) noexcept
{
	std::vector<std::size_t> active;
	{
		std::scoped_lock lock(m_mutex);
		active.swap(m_activePhaseIndices);
	}

	bool forcedShutdown = false;
	for (auto iterator = active.rbegin(); iterator != active.rend(); ++iterator) {
		try {
			const auto finalization = m_phases[*iterator].FinalizePhase();
			if (finalization.Outcome() != EEditorAppLifecycleFinalizationOutcome::Succeeded) forcedShutdown = true;
		}
		catch (const std::exception&) {
			forcedShutdown = true;
		}
	}

	std::scoped_lock lock(m_mutex);
	m_activePhaseIndices.clear();
	m_stopRequested = false;
	m_state = EEditorAppLifecycleState::Stopped;
	const auto code = forcedShutdown && requestedCode == EEditorAppLifecycleResultCode::Stopped
		? EEditorAppLifecycleResultCode::ForcedShutdown : requestedCode;
	const auto result = MakeResult(code, m_state, phase, forcedShutdown);
	m_lastTerminalResult.emplace(result);
	return result;
}

EditorAppLifecycleResult EditorAppLifecycle::Stop() noexcept
{
	{
		std::scoped_lock lock(m_mutex);
		switch (m_state) {
		case EEditorAppLifecycleState::Stopped:
			return MakeResult(EEditorAppLifecycleResultCode::AlreadyStopped, m_state);
		case EEditorAppLifecycleState::Starting:
			m_stopRequested = true;
			return MakeResult(EEditorAppLifecycleResultCode::StopDeferred, m_state);
		case EEditorAppLifecycleState::Stopping:
			return MakeResult(EEditorAppLifecycleResultCode::StopInProgress, m_state);
		case EEditorAppLifecycleState::Running:
			m_state = EEditorAppLifecycleState::Stopping;
			break;
		}
	}
	return FinishStop(EEditorAppLifecycleResultCode::Stopped, std::nullopt);
}

EEditorAppLifecycleState EditorAppLifecycle::State() noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_state;
}

std::optional<EditorAppLifecycleResult> EditorAppLifecycle::LastTerminalResult()
{
	std::scoped_lock lock(m_mutex);
	return m_lastTerminalResult;
}

std::size_t EditorAppLifecycle::ActivePhaseCount() noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_activePhaseIndices.size();
}

} // namespace editor::lifecycle
