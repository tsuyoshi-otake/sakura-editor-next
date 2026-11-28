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
#include "workbench/projects/AgentSessionLiveRecord.h"
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
	// empty Terminal does not start a second, default terminal. A terminal
	// still at its first prompt in this folder -- normally the default shell
	// the Project opened with -- runs the command instead of being left
	// unused beside a new one (#415).
	const auto target = m_terminalTool->RunInFirstPromptTerminalOrAdd(folder, input);
	if (!target) return EProjectsActivationStatus::Failed;
	const auto terminalId = std::optional<std::uint64_t>(target->tabId);
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
	AgentSessionDiscoveryRoots roots;
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
			const auto present = m_terminalTool->AgentPresence(true);
			if (!present.empty()) roots = ResolveAgentSessionDiscoveryRoots();
			const auto liveRoots = ResolveAgentLiveSessionRoots(roots);
			for (const auto& record : present) {
				const std::wstring name(terminal::TerminalAgentDisplayName(record.kind));
				std::optional<std::wstring> known;
				if (const auto found = m_resumedAgentSessions.find(record.tabId);
					found != m_resumedAgentSessions.end()) {
					known = found->second;
				}
				// The running CLI's own record outranks the identity it was
				// resumed with: it may have switched sessions since (#448).
				std::vector<AgentProcessCoordinates> processes;
				for (const auto& process : m_terminalTool->AgentProcesses(record.tabId)) {
					processes.push_back({ .processId = process.processId,
						.creationTime = process.creationTime });
				}
				std::optional<std::wstring> live;
				for (const auto provider : { EAgentSessionProvider::ClaudeCode,
					EAgentSessionProvider::Codex, EAgentSessionProvider::Devin,
					EAgentSessionProvider::Pi }) {
					if (AgentSessionProviderDisplayName(provider) == name) {
						live = ReadLiveAgentSessionIdentity(provider, processes, liveRoots);
					}
				}
				agents.push_back({ .name = name, .knownSessionIdentity = std::move(known),
					.liveSessionIdentity = std::move(live),
					.startedUnixSeconds = record.startedUnixSeconds, .tabId = record.tabId });
			}
		} catch (...) {
			agents.clear();
		}
	}
	// Discovery only checks live identities and infers the unidentified, and
	// the close or switch must stay bounded: the providers that can change the
	// result run together under one window, and one that misses it loses only
	// its own answer.
	std::optional<AgentSessionProbeAnswers> discovered;
	try {
		const auto plan = PlanAgentSessionRecordDiscovery(agents);
		AgentSessionDiscoveryLimits limits;
		limits.requiredIdentities = plan.requiredIdentities;
		discovered.emplace(DiscoverAgentSessionsWithin(folder, roots, limits,
			plan.providers, std::chrono::milliseconds(1500)));
	} catch (const std::exception&) {
	}
	try {
		// Each claim names the tab it landed on, so the layout can put the
		// identity on that tab's leaf; a live agent that ends up unclaimed is
		// recorded as a plain shell leaf, never guessed (#415 Phase 3).
		const auto claims = discovered
			? RecordLiveAgentSessionsByTab(agents, discovered->sessions,
				kMaximumAgentSessionRestoreSessions, discovered->answered)
			: RecordLiveAgentSessionsByTab(agents, {});
		std::unordered_map<std::uint64_t, std::wstring> claimsByTab;
		for (const auto& claim : claims) claimsByTab[claim.tabId] = claim.identity;
		std::unordered_map<std::uint64_t, std::wstring> tabDirectories;
		for (const auto& tab : m_terminalTool->Tabs()) {
			tabDirectories[tab.id] = tab.initialWorkingDirectory;
		}
		const auto snapshot = m_terminalTool->GroupLayoutSnapshot();
		AgentSessionRestoreLayout layout;
		std::unordered_map<std::uint64_t, std::size_t> leafIndexByTab;
		std::vector<std::wstring> identities;
		for (const auto& group : snapshot.groups) {
			// A group without a recorded session is never restored (open
			// decision 1), so its shells are not recorded either; they would
			// only spend the shell-leaf bound.
			if (std::none_of(group.tabIds.begin(), group.tabIds.end(),
				[&claimsByTab](const std::uint64_t tabId) { return claimsByTab.contains(tabId); })) {
				continue;
			}
			AgentSessionRestoreLayoutGroup recorded;
			// The reachable tree is emitted in depth-first order, which is
			// also group.tabIds' order; dead nodes left by past collapses are
			// never recorded.
			const std::function<std::optional<std::size_t>(std::size_t)> emit =
				[&](const std::size_t nodeIndex) -> std::optional<std::size_t> {
				if (nodeIndex >= group.nodes.size()) return std::nullopt;
				const auto& node = group.nodes[nodeIndex];
				if (node.children.empty()) {
					AgentSessionRestoreLeaf leaf;
					const auto claim = claimsByTab.find(node.leafId);
					if (claim != claimsByTab.end()) {
						leaf.session = claim->second;
						identities.push_back(claim->second);
					} else {
						const auto directory = tabDirectories.find(node.leafId);
						leaf.shell = directory != tabDirectories.end()
							&& !directory->second.empty()
							? directory->second : std::wstring(folder);
					}
					const std::size_t newLeaf = layout.leaves.size();
					layout.leaves.push_back(std::move(leaf));
					leafIndexByTab[node.leafId] = newLeaf;
					recorded.nodes.push_back({ .leaf = newLeaf });
					return recorded.nodes.size() - 1;
				}
				std::vector<std::size_t> children;
				for (const auto child : node.children) {
					if (const auto emitted = emit(child)) children.push_back(*emitted);
				}
				if (children.empty()) return std::nullopt;
				recorded.nodes.push_back({
					.leaf = std::nullopt,
					.orientation = node.orientation == terminal::TerminalPaneOrientation::Vertical
						? AgentSessionRestoreOrientation::Vertical
						: AgentSessionRestoreOrientation::Horizontal,
					.children = std::move(children),
					.weights = node.weights,
				});
				return recorded.nodes.size() - 1;
			};
			const auto root = group.root < group.nodes.size() ? emit(group.root) : std::nullopt;
			if (!root) continue;
			recorded.root = *root;
			layout.groups.push_back(std::move(recorded));
		}
		if (const auto active = snapshot.activeTabId) {
			if (const auto leaf = leafIndexByTab.find(*active); leaf != leafIndexByTab.end()) {
				layout.active = leaf->second;
			}
		}
		// A live layout past a bound (more shells or nodes than the record
		// holds) still records its sessions, without the layout, so they are
		// resumed as separate terminals rather than lost.
		std::optional<AgentSessionRestoreLayout> recordedLayout;
		if (!identities.empty() && IsValidAgentSessionRestoreLayout(layout, identities)) {
			recordedLayout = std::move(layout);
		}
		(void)service->Update([folder = std::wstring(folder), identities = std::move(identities),
			recordedLayout = std::move(recordedLayout)](AgentSessionRestoreState& state) {
			return SetAgentSessionRestoreEntry(state, folder, identities, recordedLayout);
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
	auto recorded = service->RecordedEntry(folder);
	if (!recorded || recorded->sessions.empty()) return;
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
	m_agentSessionRestoreRecorded = std::move(*recorded);
	try {
		m_agentSessionRestoreStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (m_agentSessionRestoreStop == nullptr) throw std::bad_alloc();
		m_agentSessionRestore = std::async(std::launch::async,
			[folder = m_agentSessionRestoreFolder,
				recorded = m_agentSessionRestoreRecorded.sessions,
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
	m_agentSessionRestoreRecorded = {};
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
		[this, folder](const AgentSessionRestorePlan& toInstall) {
			return InstallAgentSessionRestorePlan(toInstall, folder);
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

//! Installs one restore plan's groups as a single terminal transaction (#415
//! Phase 3). Each session leaf gets a terminal in `folder` with the same
//! resume command ResumeAgentSessionInTerminal builds; the first session
//! leaf may take the untouched default shell. Each shell leaf opens a new
//! terminal in its recorded directory (or `folder` when it no longer
//! exists). The recorded active leaf -- or the plan's fallback -- is
//! selected, and every resumed tab is marked so the next record knows its
//! identity.
workbench::projects::AgentSessionRestoreInstallResult CEditWnd::InstallAgentSessionRestorePlan(
	const workbench::projects::AgentSessionRestorePlan& plan, const std::wstring_view folder)
{
	using namespace workbench::projects;
	AgentSessionRestoreInstallResult result;
	if (m_terminalTool == nullptr) {
		for (const auto& group : plan.groups) result.failed += group.leaves.size();
		return result;
	}
	std::vector<terminal::TerminalRestoreGroup> spec;
	spec.reserve(plan.groups.size());
	bool reuseOffered = false;
	for (const auto& group : plan.groups) {
		terminal::TerminalRestoreGroup specGroup;
		specGroup.leaves.reserve(group.leaves.size());
		specGroup.nodes.reserve(group.nodes.size());
		specGroup.root = group.root;
		for (const auto& leaf : group.leaves) {
			terminal::TerminalRestoreLeaf specLeaf;
			if (leaf.session) {
				const auto command = BuildAgentSessionResumeCommand(
					leaf.session->provider, leaf.session->id);
				if (!command) {
					// A discovered id is valid by contract; a defensive drop
					// collapses the leaf like a missing session.
					specLeaf.drop = true;
					specGroup.leaves.push_back(std::move(specLeaf));
					continue;
				}
				specLeaf.workingDirectory = std::wstring(folder);
				specLeaf.input.reserve(command->size() + 1U);
				for (const wchar_t character : *command) {
					specLeaf.input.push_back(static_cast<char>(character));
				}
				specLeaf.input.push_back('\r');
				// Phase 1's first-prompt reuse applies to the first session
				// leaf only; the tool checks eligibility itself.
				if (!reuseOffered) {
					specLeaf.reuseFirstPrompt = true;
					reuseOffered = true;
				}
			} else {
				// A plain shell restarts in its recorded directory when that
				// directory still exists, else in the Project folder.
				const std::wstring recorded = leaf.shellDirectory
					? *leaf.shellDirectory : std::wstring(folder);
				const auto attributes = ::GetFileAttributesW(recorded.c_str());
				specLeaf.workingDirectory = attributes != INVALID_FILE_ATTRIBUTES
					&& (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
					? recorded : std::wstring(folder);
			}
			specGroup.leaves.push_back(std::move(specLeaf));
		}
		for (const auto& node : group.nodes) {
			specGroup.nodes.push_back({
				.leaf = node.leaf,
				.orientation = node.orientation == AgentSessionRestoreOrientation::Vertical
					? terminal::TerminalPaneOrientation::Vertical
					: terminal::TerminalPaneOrientation::Horizontal,
				.children = node.children,
				.weights = node.weights,
			});
		}
		spec.push_back(std::move(specGroup));
	}
	std::optional<std::pair<std::size_t, std::size_t>> active;
	if (plan.activeGroup && plan.activeLeaf) {
		active = std::pair{ *plan.activeGroup, *plan.activeLeaf };
	}
	const auto installed = m_terminalTool->RestoreTerminalGroupLayout(std::move(spec), active);
	for (std::size_t groupIndex = 0; groupIndex < plan.groups.size(); ++groupIndex) {
		const auto& group = plan.groups[groupIndex];
		for (std::size_t leafIndex = 0; leafIndex < group.leaves.size(); ++leafIndex) {
			const auto& leaf = group.leaves[leafIndex];
			const auto tabId = groupIndex < installed.leafTabIds.size()
				&& leafIndex < installed.leafTabIds[groupIndex].size()
				? installed.leafTabIds[groupIndex][leafIndex] : std::nullopt;
			if (tabId) {
				if (leaf.session) {
					// The command was accepted: this terminal carries a
					// session whose identity is known rather than inferred.
					m_resumedAgentSessions[*tabId] = AgentSessionIdentity(*leaf.session);
					++result.resumed;
				}
			} else {
				++result.failed;
			}
		}
	}
	if (result.resumed != 0) {
		(void)ActivateBuiltinWorkbenchView(workbench::layout::ids::view::Terminal, true);
	}
	return result;
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
	m_agentSessionRestoreRecorded = {};
}
