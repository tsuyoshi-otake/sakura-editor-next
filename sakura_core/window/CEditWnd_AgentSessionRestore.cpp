/*! @file
 * @brief CEditWnd's #406 Phase-2 agent session restore wiring: the shared
 * resume path, the live-session record on close/switch, and the one bounded
 * restore attempt per Project open. Split out of CEditWnd.cpp, which is at
 * the COFF section limit.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "window/CEditWnd.h"

#include "terminal/window/CTerminalTool.h"
#include "workbench/IWorkbenchRuntime.h"
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/projects/AgentSessionDiscovery.h"
#include "workbench/projects/AgentSessionRestore.h"
#include "workbench/projects/AgentSessionRestoreService.h"
#include "workbench/projects/CProjectsPage.h"

workbench::projects::EProjectsActivationStatus CEditWnd::ResumeAgentSessionInTerminal(
	const workbench::projects::AgentSessionRecord& session, const std::wstring_view folder)
{
	using workbench::projects::EProjectsActivationStatus;
	if (m_terminalTool == nullptr || GetHwnd() == nullptr || folder.empty()) {
		return EProjectsActivationStatus::Failed;
	}
	const auto command = workbench::projects::BuildAgentSessionResumeCommand(
		session.provider, session.id);
	if (!command) return EProjectsActivationStatus::Failed;
	// The command is ASCII by construction (validated id); like VS
	// Code's sendText(text, true), the line ends with CR.
	std::string input;
	input.reserve(command->size() + 1U);
	for (const wchar_t character : *command) input.push_back(static_cast<char>(character));
	input.push_back('\r');
	// Create the terminal before revealing the view, so revealing an
	// empty Terminal does not start a second, default terminal.
	const auto terminalId = m_terminalTool->AddTerminalWithInput(folder, input);
	if (!terminalId) return EProjectsActivationStatus::Failed;
	// The command was accepted: even if the reveal below fails, this terminal
	// carries a session whose identity is known rather than inferred.
	m_resumedAgentSessions[*terminalId] = workbench::projects::AgentSessionIdentity(session);
	if (!ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Terminal, true)
		|| !m_terminalTool->SelectTerminal(*terminalId)) {
		return EProjectsActivationStatus::Failed;
	}
	return EProjectsActivationStatus::FocusedCurrentWindow;
}

bool CEditWnd::AgentSessionRestoreEnabled() const
{
	if (m_workbenchRuntime == nullptr) return false;
	bool enabled = true;	// mirrors the descriptor default
	try {
		const auto lookup = m_workbenchRuntime->Configuration().GetValue(
			"sakura.projects.restoreAgentSessions", BuildWorkbenchConfigurationTarget());
		if (lookup.value) {
			if (const auto* value = std::get_if<bool>(&lookup.value->Value()); value != nullptr) {
				enabled = *value;
			}
		}
	} catch (...) {
		// An unreadable setting takes the registered default rather than
		// whatever this window happened to hold.
		enabled = true;
	}
	return enabled;
}

void CEditWnd::PersistLiveAgentSessions(const std::wstring_view folder) noexcept
{
	using namespace workbench::projects;
	// No terminal tool means this window never owned a terminal projection --
	// recording "nothing live" would erase a record another window wrote.
	if (folder.empty() || m_workbenchRuntime == nullptr || m_terminalTool == nullptr
		|| !AgentSessionRestoreEnabled()) return;
	auto* const service = m_workbenchRuntime->AgentSessionRestore();
	if (service == nullptr) return;
	std::vector<LiveAgentSessionSource> agents;
	bool inferenceNeeded = false;
	{
		try {
			// A tab that is gone can no longer host the session it was resumed
			// with, so the known-identity map is pruned against every live tab
			// -- not just the ones presence resolved, which would drop a
			// resumed terminal whose agent has not announced itself yet.
			std::vector<std::uint64_t> liveTabs;
			for (const auto& tab : m_terminalTool->Tabs()) liveTabs.push_back(tab.id);
			for (auto it = m_resumedAgentSessions.begin(); it != m_resumedAgentSessions.end();) {
				it = std::find(liveTabs.begin(), liveTabs.end(), it->first) == liveTabs.end()
					? m_resumedAgentSessions.erase(it) : std::next(it);
			}
			for (const auto& record : m_terminalTool->AgentPresence(true)) {
				LiveAgentSessionSource agent{
					.name = std::wstring(terminal::TerminalAgentDisplayName(record.kind)),
					.startedUnixSeconds = record.startedUnixSeconds,
				};
				const auto known = m_resumedAgentSessions.find(record.tabId);
				if (known != m_resumedAgentSessions.end()) {
					agent.knownSessionIdentity = known->second;
				}
				// Only a session-capable provider whose known identity is
				// missing or unusable can need session data for inference;
				// discovery is skipped entirely otherwise.
				bool covered = false;
				if (agent.knownSessionIdentity && IsValidAgentSessionIdentity(*agent.knownSessionIdentity)) {
					const auto provider = AgentSessionIdentityProvider(*agent.knownSessionIdentity);
					covered = provider
						&& AgentSessionProviderDisplayName(*provider) == agent.name;
				}
				if (!covered) {
					for (const auto provider : { EAgentSessionProvider::ClaudeCode,
						EAgentSessionProvider::Codex, EAgentSessionProvider::Devin,
						EAgentSessionProvider::Pi }) {
						if (AgentSessionProviderDisplayName(provider) == agent.name) {
							inferenceNeeded = true;
							break;
						}
					}
				}
				agents.push_back(std::move(agent));
			}
		} catch (...) {
			agents.clear();
			inferenceNeeded = false;
		}
	}
	std::vector<AgentSessionRecord> sessions;
	if (inferenceNeeded) {
		// "Newest session activity" wants fresh data even at close, but the
		// transition must stay bounded and discovery belongs off the UI thread:
		// it runs on a worker with a tight Devin cap and is abandoned at a
		// fixed deadline. Whatever it did not finish simply records nothing --
		// never a guess, never an unbounded stall.
		const HANDLE stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (stop != nullptr) {
			try {
				auto discovery = std::async(std::launch::async,
					[folder = std::wstring(folder), stop] {
						AgentSessionDiscoveryLimits limits;
						limits.devinTimeoutMilliseconds = 1000;
						return DiscoverAgentSessions(folder,
							ResolveAgentSessionDiscoveryRoots(), limits, stop);
					});
				if (discovery.wait_for(std::chrono::milliseconds(1500)) == std::future_status::ready) {
					if (auto result = discovery.get()) sessions = std::move(*result);
				} else {
					::SetEvent(stop);
					// The discovery contract joins promptly once stop is set;
					// the future's destructor would wait either way.
					try { (void)discovery.get(); } catch (...) {}
				}
			} catch (...) {
			}
			::CloseHandle(stop);
		}
	}
	try {
		const auto identities = RecordLiveAgentSessions(agents, sessions);
		(void)service->Update([folder = std::wstring(folder), &identities](AgentSessionRestoreState& state) {
			return SetAgentSessionRestoreEntry(state, folder, identities);
		});
	} catch (...) {
	}
}

void CEditWnd::BeginAgentSessionRestore() noexcept
{
	using namespace workbench::projects;
	if (m_workbenchRuntime == nullptr || !AgentSessionRestoreEnabled()) return;
	std::wstring identityKey;
	try {
		identityKey = m_workbenchRuntime->WorkspaceContext().Snapshot().workspaceIdentityKey;
	} catch (...) {
		return;
	}
	// One attempt per open; the marker moves with each accepted switch so
	// leaving a Project and returning restores it again. An empty context
	// still consumes the marker: A -> EMPTY -> A restores A again.
	if (identityKey == m_agentSessionRestoreIdentity) return;
	m_agentSessionRestoreIdentity = identityKey;
	// A newer open supersedes an in-flight attempt, whatever it can restore.
	CancelAgentSessionRestore();
	if (identityKey.empty()) return;
	const auto folder = GetSemanticWorkspaceRoot();
	// Like the Sessions view: only a single-root workspace carries session
	// history; empty and multi-root workspaces restore nothing.
	if (folder.empty()) return;
	auto* const service = m_workbenchRuntime->AgentSessionRestore();
	if (service == nullptr) return;
	service->Load();
	auto recorded = service->RecordedSessions(folder);
	if (recorded.empty()) return;
	// A workspace switch keeps the outgoing terminals alive in the terminal
	// registry, so returning to a Project reattaches agents that never
	// stopped. Resuming their sessions again would run two agents on one
	// session: a projection that already hosts an agent restores nothing.
	try {
		if (m_terminalTool != nullptr && !m_terminalTool->AgentPresence(true).empty()) return;
	} catch (...) {
		return;
	}
	m_agentSessionRestoreFolder = folder;
	m_agentSessionRestoreRecorded = std::move(recorded);
	try {
		m_agentSessionRestoreStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (m_agentSessionRestoreStop == nullptr) throw std::bad_alloc();
		m_agentSessionRestore = std::async(std::launch::async,
			[folder = m_agentSessionRestoreFolder,
				recorded = m_agentSessionRestoreRecorded,
				stop = m_agentSessionRestoreStop]() mutable {
				// Every recorded identity must surface when it still exists,
				// however many newer sessions the folder gained meanwhile.
				AgentSessionDiscoveryLimits limits;
				limits.requiredIdentities = std::move(recorded);
				return DiscoverAgentSessions(folder, ResolveAgentSessionDiscoveryRoots(),
					limits, stop);
			});
	} catch (...) {
		CancelAgentSessionRestore();
		return;
	}
	if (GetHwnd() == nullptr
		|| ::SetTimer(GetHwnd(), IDT_AGENT_SESSION_RESTORE, 50, nullptr) == 0) {
		// Nothing could observe the completion: cancel and fail closed.
		CancelAgentSessionRestore();
	}
}

void CEditWnd::PollAgentSessionRestore() noexcept
{
	using namespace workbench::projects;
	const HWND hwnd = GetHwnd();
	if (!m_agentSessionRestore.valid()) {
		if (hwnd != nullptr) (void)::KillTimer(hwnd, IDT_AGENT_SESSION_RESTORE);
		return;
	}
	if (m_agentSessionRestore.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
	std::optional<std::vector<AgentSessionRecord>> sessions;
	try { sessions = m_agentSessionRestore.get(); }
	catch (...) { sessions.reset(); }
	const auto folder = m_agentSessionRestoreFolder;
	const auto recorded = m_agentSessionRestoreRecorded;
	m_agentSessionRestore = {};
	if (m_agentSessionRestoreStop != nullptr) {
		::CloseHandle(m_agentSessionRestoreStop);
		m_agentSessionRestoreStop = nullptr;
	}
	m_agentSessionRestoreFolder.clear();
	m_agentSessionRestoreRecorded.clear();
	if (hwnd != nullptr) (void)::KillTimer(hwnd, IDT_AGENT_SESSION_RESTORE);
	if (!sessions) {
		::OutputDebugStringW(L"Sakura Editor NEXT: agent session restore discovery failed or was cancelled.\n");
		return;
	}
	// A switch while discovery ran means a newer open owns the restore; this
	// attempt's folder is stale and ends silently (never retried).
	if (!IsSameAgentSessionFolder(folder, GetSemanticWorkspaceRoot())) return;
	const auto plan = PlanAgentSessionRestore(AgentSessionRestoreEnabled(), recorded, sessions);
	for (const auto& missing : plan.skipped) {
		const auto line = L"Sakura Editor NEXT: agent session restore skipped a session that no longer exists: " + missing + L"\n";
		::OutputDebugStringW(line.c_str());
	}
	const auto outcome = DispatchAgentSessionRestore(plan,
		[this, folder](const AgentSessionRecord& session) {
			return ResumeAgentSessionInTerminal(session, folder)
				== EProjectsActivationStatus::FocusedCurrentWindow;
		});
	// The open that used a record consumes it: another window opened on the
	// same folder must not resume sessions this window is now running. This
	// window records them again when it leaves the Project or closes.
	if (plan.status == EAgentSessionRestoreStatus::Ready && m_workbenchRuntime != nullptr) {
		try {
			if (auto* const service = m_workbenchRuntime->AgentSessionRestore(); service != nullptr) {
				(void)service->Update([&folder](AgentSessionRestoreState& state) {
					return SetAgentSessionRestoreEntry(state, folder, {});
				});
			}
		} catch (...) {
		}
	}
	wchar_t message[256];
	(void)_snwprintf_s(message, _TRUNCATE,
		L"Sakura Editor NEXT: agent session restore status=%d resumed=%zu skipped=%zu dropped=%zu failed=%zu.\n",
		static_cast<int>(outcome.status), outcome.resumed, outcome.skipped,
		outcome.dropped, outcome.failed);
	::OutputDebugStringW(message);
}

void CEditWnd::CancelAgentSessionRestore() noexcept
{
	if (GetHwnd() != nullptr) (void)::KillTimer(GetHwnd(), IDT_AGENT_SESSION_RESTORE);
	if (m_agentSessionRestoreStop != nullptr) (void)::SetEvent(m_agentSessionRestoreStop);
	if (m_agentSessionRestore.valid()) {
		// The discovery contract bounds this wait: it returns once stop is set.
		try { (void)m_agentSessionRestore.get(); } catch (...) {}
		m_agentSessionRestore = {};
	}
	if (m_agentSessionRestoreStop != nullptr) {
		::CloseHandle(m_agentSessionRestoreStop);
		m_agentSessionRestoreStop = nullptr;
	}
	m_agentSessionRestoreFolder.clear();
	m_agentSessionRestoreRecorded.clear();
}
