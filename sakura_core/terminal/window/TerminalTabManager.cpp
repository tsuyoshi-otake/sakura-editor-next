/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalTabManager.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
#include <windows.h>

namespace terminal {
namespace {

constexpr wchar_t kDefaultTabLabel[] = L"PowerShell";

TerminalSize NormalizeSize( TerminalSize size ) noexcept
{
	size.columns = std::max<std::uint16_t>(1, size.columns);
	size.rows = std::max<std::uint16_t>(1, size.rows);
	return size;
}

HarnessOperationId MakeOperationId( const std::uint64_t value ) noexcept
{
	HarnessOperationId result;
	for( std::size_t index = 0; index < sizeof(value) && index < result.value.size(); ++index ) {
		result.value[index] = static_cast<std::uint8_t>(value >> (index * 8));
	}
	return result;
}

using runtime::topology::TerminalCollectionSnapshot;
using runtime::topology::TerminalLayoutNode;
using runtime::topology::TerminalPaneSnapshot;

void CollectLeaves( const TerminalLayoutNode& node, std::vector<TerminalPaneId>& leaves )
{
	if( node.IsLeaf() ) {
		if( node.pane.paneId.IsValid() ) leaves.push_back(node.pane.paneId);
		return;
	}
	for( const auto& child : node.children ) CollectLeaves(child, leaves);
}

TerminalPaneId FirstLeaf( const TerminalLayoutNode& node ) noexcept
{
	const auto* current = &node;
	while( !current->IsLeaf() && !current->children.empty() ) current = &current->children.front();
	return current->IsLeaf() ? current->pane.paneId : TerminalPaneId{};
}

//! The pane `paneId` sits beside under its parent split, and that split's
//! axis. SplitPane inserts the new leaf directly after the pane it split, so
//! the preceding sibling is the split source; a first child falls back to the
//! following one.
bool FindSplitNeighbor( const TerminalLayoutNode& node, const TerminalPaneId paneId,
	TerminalPaneId& neighbor, TerminalPaneOrientation& orientation ) noexcept
{
	if( node.IsLeaf() ) return false;
	for( std::size_t index = 0; index < node.children.size(); ++index ) {
		const auto& child = node.children[index];
		if( child.IsLeaf() && child.pane.paneId == paneId ) {
			if( node.children.size() < 2 ) return false;
			neighbor = FirstLeaf(node.children[index > 0 ? index - 1 : 1]);
			orientation = node.orientation;
			return neighbor.IsValid();
		}
		if( FindSplitNeighbor(child, paneId, neighbor, orientation) ) return true;
	}
	return false;
}

const TerminalPaneSnapshot* FindPaneSnapshot( const TerminalCollectionSnapshot& snapshot,
	const TerminalPaneId paneId ) noexcept
{
	for( const auto& session : snapshot.sessions ) {
		for( const auto& window : session.windows ) {
			for( const auto& pane : window.panes ) if( pane.id == paneId ) return &pane;
		}
	}
	return nullptr;
}

//! The runtime's current pane: the active session's current window's active
//! pane. A tmux command run outside every pane acts on this one.
TerminalPaneId CurrentPane( const TerminalCollectionSnapshot& snapshot ) noexcept
{
	if( !snapshot.activeSession ) return {};
	for( const auto& session : snapshot.sessions ) {
		if( session.id != *snapshot.activeSession ) continue;
		for( const auto& window : session.windows ) {
			if( window.id == session.activeWindow ) return window.activePane;
		}
	}
	return {};
}

} // namespace

struct TerminalTabManager::Impl {
	struct Tab {
		std::uint64_t id{};
		TerminalInstanceId instanceId;
		TerminalSessionId sessionId;
		TerminalWindowId windowId;
		TerminalPaneId paneId;
	};

	TerminalTabEventCallback eventCallback;
	struct EventRoute {
		TerminalInstanceId instanceId;
		std::uint64_t tabId{};
	};
	std::mutex eventMutex;
	std::vector<EventRoute> eventRoutes;
	bool acceptingEvents{ true };
	std::shared_ptr<CTerminalRuntimeService> runtimeService;
	TerminalSubscription runtimeSubscription;
	bool ownsRuntimeService{};
	std::vector<std::unique_ptr<Tab>> tabs;
	std::optional<std::uint64_t> activeTabId;
	std::uint64_t nextTabId{ 1 };
	std::uint64_t nextReplacementId{ 1 };
	std::size_t scrollbackLimit{ TerminalModel::kDefaultScrollbackLines };
	bool closed{};
	bool startedAnySession{};
	//! Runtime current pane seen by the previous ReconcileTopology (#403), so a
	//! selection is reported once per change rather than on every reconcile.
	TerminalPaneId lastRuntimeCurrentPane;

	Tab* Find( std::uint64_t id ) noexcept
	{
		const auto found = std::find_if( tabs.begin(), tabs.end(), [id](const auto& tab) { return tab->id == id; } );
		return found == tabs.end() ? nullptr : found->get();
	}

	const Tab* Find( std::uint64_t id ) const noexcept
	{
		const auto found = std::find_if( tabs.begin(), tabs.end(), [id](const auto& tab) { return tab->id == id; } );
		return found == tabs.end() ? nullptr : found->get();
	}

	void RegisterEventRoute( const TerminalInstanceId instanceId, const std::uint64_t tabId )
	{
		{
			std::lock_guard lock(eventMutex);
			if( !acceptingEvents ) return;
			eventRoutes.push_back({ instanceId, tabId });
		}
		AnnounceUndrainedOutput(instanceId);
	}

	void ReplaceEventRoute( const TerminalInstanceId oldInstanceId,
		const TerminalInstanceId newInstanceId, const std::uint64_t tabId )
	{
		{
			std::lock_guard lock(eventMutex);
			const auto old = std::remove_if(eventRoutes.begin(), eventRoutes.end(),
				[oldInstanceId](const EventRoute& route) { return route.instanceId == oldInstanceId; });
			eventRoutes.erase(old, eventRoutes.end());
			if( !acceptingEvents ) return;
			eventRoutes.push_back({ newInstanceId, tabId });
		}
		AnnounceUndrainedOutput(newInstanceId);
	}

	void RemoveEventRoute( const TerminalInstanceId instanceId ) noexcept
	{
		std::lock_guard lock(eventMutex);
		const auto found = std::remove_if(eventRoutes.begin(), eventRoutes.end(),
			[instanceId](const EventRoute& route) { return route.instanceId == instanceId; });
		eventRoutes.erase(found, eventRoutes.end());
	}

	void StopEvents() noexcept
	{
		std::lock_guard lock(eventMutex);
		acceptingEvents = false;
		eventRoutes.clear();
	}

	void ClearEventRoutes() noexcept
	{
		std::lock_guard lock(eventMutex);
		eventRoutes.clear();
	}

	void RegisterProjectionRoutes()
	{
		std::vector<TerminalInstanceId> rebound;
		{
			std::lock_guard lock(eventMutex);
			eventRoutes.clear();
			if( !acceptingEvents ) return;
			rebound.reserve(tabs.size());
			for( const auto& tab : tabs ) {
				eventRoutes.push_back({ tab->instanceId, tab->id });
				rebound.push_back(tab->instanceId);
			}
		}
		for( const auto instanceId : rebound ) AnnounceUndrainedOutput(instanceId);
	}

	//! Announces one fact about an instance to the owner. An instance with no
	//! route is ignored on purpose: the runtime service can be process-wide, so
	//! this manager observes instances that belong to other projections.
	void PublishTabEvent( const TerminalInstanceId instanceId, const TerminalTabEventKind kind,
		const TerminalSessionState sessionState, const std::uint32_t errorCode,
		const std::optional<TerminalJobProcessEvent>& jobProcess = std::nullopt ) noexcept
	{
		TerminalTabEvent translated;
		TerminalTabEventCallback callback;
		try {
			{
				std::lock_guard lock(eventMutex);
				if( !acceptingEvents || !eventCallback ) return;
				const auto found = std::find_if(eventRoutes.begin(), eventRoutes.end(), [instanceId](const EventRoute& route) {
					return route.instanceId == instanceId;
				});
				if( found == eventRoutes.end() ) return;
				translated.kind = kind;
				translated.tabId = found->tabId;
				translated.state = sessionState;
				translated.errorCode = errorCode;
				translated.jobProcess = jobProcess;
				callback = eventCallback;
			}
			callback(translated);
		} catch( ... ) {
			// A projection callback is advisory and must not unwind a session worker.
		}
	}

	//! Output availability is an edge. A session raises it when its reader
	//! queues a batch and re-arms only once that batch is drained, so a route
	//! bound after the session started - AddTab binds after CreateSession has
	//! already started the reader, and an attach rebuilds every route beneath a
	//! live session - misses an edge that will never come again, leaving the
	//! pane unpainted until something else wakes it. Read the level that edge
	//! stood for and announce it once; the owner coalesces a repeat exactly as
	//! it coalesces two real ones. Call with eventMutex unheld.
	void AnnounceUndrainedOutput( const TerminalInstanceId instanceId ) noexcept
	{
		const auto* instance = runtimeService ? runtimeService->Instance(instanceId) : nullptr;
		if( instance == nullptr || !instance->HasUndrainedOutput() ) return;
		PublishTabEvent(instanceId, TerminalTabEventKind::OutputAvailable,
			instance->SessionState(), instance->LastError());
	}

	//! Closes exactly one tab's terminal. A session can hold panes of other tabs
	//! once tmux has split it or added windows to it (#403), so a tab closes
	//! its own pane; the topology drops the window and the session with their
	//! last pane. A tab without a pane (no topology) closes its instance.
	void ClosePaneOrInstance( const TerminalPaneId paneId, const TerminalSessionId sessionId,
		const TerminalInstanceId instanceId, const HarnessOperationId operationId ) noexcept
	{
		if( paneId.IsValid() ) {
			TerminalPaneCloseRequest request;
			request.operationId = operationId;
			request.paneId = paneId;
			if( runtimeService->ClosePane(request).code == TerminalRuntimeOperationCode::Succeeded ) return;
		} else if( sessionId.IsValid() ) {
			TerminalSessionCloseRequest request;
			request.operationId = operationId;
			request.sessionId = sessionId;
			if( runtimeService->CloseSession(request).code == TerminalRuntimeOperationCode::Succeeded ) return;
		}
		// The pane may already be gone (a tmux kill-pane raced the panel). The
		// instance close is idempotent and keeps the process from outliving the tab.
		runtimeService->BeginCloseInstance(instanceId, TerminalInstanceCloseReason::Explicit);
	}

	//! Topology is not an instance fact, so it has no route: every change goes
	//! to the owner, which reconciles against the snapshot (#403).
	void PublishTopologyEvent() noexcept
	{
		TerminalTabEvent translated;
		TerminalTabEventCallback callback;
		try {
			{
				std::lock_guard lock(eventMutex);
				if( !acceptingEvents || !eventCallback ) return;
				translated.kind = TerminalTabEventKind::TopologyChanged;
				callback = eventCallback;
			}
			callback(translated);
		} catch( ... ) {
			// Advisory, as in PublishTabEvent: never unwind the publishing thread.
		}
	}

	void OnRuntimeEvent( const TerminalInstanceEvent& event ) noexcept
	{
		if( event.kind == TerminalInstanceEventKind::TopologyChanged ) {
			PublishTopologyEvent();
			return;
		}
		if( event.kind == TerminalInstanceEventKind::JobProcess ) {
			// A hint is not a session state change; keep it out of the state wake.
			if( event.jobProcess ) {
				PublishTabEvent(event.coordinate.instanceId, TerminalTabEventKind::JobProcess,
					event.sessionState, event.errorCode, event.jobProcess);
			}
			return;
		}
		PublishTabEvent(event.coordinate.instanceId,
			event.kind == TerminalInstanceEventKind::OutputAvailable
				? TerminalTabEventKind::OutputAvailable : TerminalTabEventKind::StateChanged,
			event.sessionState, event.errorCode);
	}
};

TerminalTabManager::TerminalTabManager( TerminalTabManagerDependencies dependencies, TerminalTabEventCallback eventCallback )
	: m_impl(std::make_shared<Impl>())
{
	m_impl->eventCallback = std::move(eventCallback);
	if( dependencies.runtimeService ) {
		m_impl->runtimeService = std::move(dependencies.runtimeService);
		m_impl->ownsRuntimeService = false;
	} else {
		TerminalRuntimeServiceDependencies runtimeDependencies;
		runtimeDependencies.createSession = std::move(dependencies.createSession);
		runtimeDependencies.resolveLaunch = std::move(dependencies.resolveLaunch);
		runtimeDependencies.decorateLaunch = std::move(dependencies.decorateLaunch);
		m_impl->runtimeService = std::make_shared<CTerminalRuntimeService>(std::move(runtimeDependencies));
		m_impl->ownsRuntimeService = true;
	}
	if( m_impl->runtimeService ) {
		const std::weak_ptr<Impl> weak = m_impl;
		m_impl->runtimeSubscription = m_impl->runtimeService->Subscribe([weak](const TerminalInstanceEvent& event) {
			if( const auto impl = weak.lock() ) impl->OnRuntimeEvent(event);
		});
	}
}

TerminalTabManager::~TerminalTabManager()
{
	Close();
}

std::optional<std::uint64_t> TerminalTabManager::Activate( TerminalSize size, std::wstring_view workingDirectory )
{
	if( m_impl->closed ) return std::nullopt;
	if( m_impl->tabs.empty() ) return AddTab(size, workingDirectory);
	if( !m_impl->activeTabId ) m_impl->activeTabId = m_impl->tabs.front()->id;
	return m_impl->activeTabId;
}

std::optional<std::uint64_t> TerminalTabManager::AddTab( TerminalSize size, std::wstring_view workingDirectory )
{
	if( m_impl->closed || m_impl->nextTabId == std::numeric_limits<std::uint64_t>::max() ) return std::nullopt;
	try {
		const auto id = m_impl->nextTabId++;
		auto tab = std::make_unique<Impl::Tab>();
		tab->id = id;
		TerminalSessionCreateRequest request;
		request.operationId = MakeOperationId(id);
		request.name = std::to_string(id);
		TerminalLaunchOptions launch;
		launch.initialSize = NormalizeSize(size);
		launch.workingDirectory.assign(workingDirectory);
		request.launch = std::move(launch);
		const auto created = m_impl->runtimeService
			? m_impl->runtimeService->CreateSession(request) : TerminalTopologyResult{};
		if( !created.instanceId || !created.instanceId->IsValid() ) return std::nullopt;
		tab->instanceId = *created.instanceId;
		if( created.sessionId ) tab->sessionId = *created.sessionId;
		if( created.windowId ) tab->windowId = *created.windowId;
		if( created.paneId ) tab->paneId = *created.paneId;
		if( auto* model = m_impl->runtimeService->Model(tab->instanceId) ) {
			model->SetScrollbackLimit(m_impl->scrollbackLimit);
			static_cast<void>(model->ConsumeScrollbackChange());
		}
		m_impl->startedAnySession = true;
		m_impl->tabs.emplace_back(std::move(tab));
		m_impl->RegisterEventRoute(m_impl->tabs.back()->instanceId, id);
		m_impl->activeTabId = id;
		return id;
	} catch( ... ) {
		return std::nullopt;
	}
}

bool TerminalTabManager::SelectTab( std::uint64_t tabId ) noexcept
{
	if( m_impl->closed ) return false;
	const auto* tab = m_impl->Find(tabId);
	if( tab == nullptr ) return false;
	if( m_impl->runtimeService && tab->paneId.IsValid() ) {
		TerminalPaneSelectRequest request;
		request.operationId = MakeOperationId(tabId);
		request.paneId = tab->paneId;
		const auto selected = m_impl->runtimeService->SelectPane(request);
		if( selected.code != TerminalRuntimeOperationCode::Succeeded
			&& selected.code != TerminalRuntimeOperationCode::TargetMissing) return false;
	}
	m_impl->activeTabId = tabId;
	return true;
}

bool TerminalTabManager::RestartTab( std::uint64_t tabId, TerminalSize size, std::wstring_view workingDirectory )
{
	if( m_impl->closed ) return false;
	auto* tab = m_impl->Find(tabId);
	if( tab == nullptr ) return false;
	TerminalSessionCreateRequest request;
	request.operationId = MakeOperationId(tabId);
	request.name = std::to_string(tabId) + "-replacement-" + std::to_string(m_impl->nextReplacementId++);
	TerminalLaunchOptions launch;
	launch.initialSize = NormalizeSize(size);
	launch.workingDirectory.assign(workingDirectory);
	request.launch = std::move(launch);
	const auto created = m_impl->runtimeService
		? m_impl->runtimeService->CreateSession(request) : TerminalTopologyResult{};
	if( !created.instanceId || !created.instanceId->IsValid() ) return false;
	const auto oldInstance = tab->instanceId;
	const auto oldSession = tab->sessionId;
	const auto oldPane = tab->paneId;
	tab->instanceId = *created.instanceId;
	if( created.sessionId ) tab->sessionId = *created.sessionId;
	if( created.windowId ) tab->windowId = *created.windowId;
	if( created.paneId ) tab->paneId = *created.paneId;
	m_impl->ReplaceEventRoute(oldInstance, tab->instanceId, tabId);
	if( auto* model = m_impl->runtimeService->Model(tab->instanceId) ) {
		model->SetScrollbackLimit(m_impl->scrollbackLimit);
		static_cast<void>(model->ConsumeScrollbackChange());
	}
	m_impl->startedAnySession = true;
	if( m_impl->runtimeService ) {
		m_impl->ClosePaneOrInstance(oldPane, oldSession, oldInstance,
			MakeOperationId(tabId ^ 0x8000000000000000ULL));
	}
	return created.code == TerminalRuntimeOperationCode::Succeeded;
}

bool TerminalTabManager::DeleteTab( std::uint64_t tabId ) noexcept
{
	if( m_impl->closed ) return false;
	const auto found = std::find_if( m_impl->tabs.begin(), m_impl->tabs.end(), [tabId](const auto& tab) { return tab->id == tabId; } );
	if( found == m_impl->tabs.end() ) return false;
	m_impl->RemoveEventRoute((*found)->instanceId);
	if( m_impl->runtimeService ) {
		m_impl->ClosePaneOrInstance((*found)->paneId, (*found)->sessionId, (*found)->instanceId,
			MakeOperationId(tabId));
	}
	const auto index = static_cast<std::size_t>(found - m_impl->tabs.begin());
	m_impl->tabs.erase(found);
	if( m_impl->activeTabId == tabId ) {
		if( m_impl->tabs.empty() ) m_impl->activeTabId.reset();
		else m_impl->activeTabId = m_impl->tabs[std::min(index, m_impl->tabs.size() - 1)]->id;
	}
	return true;
}

TerminalTabClearResult TerminalTabManager::ClearTabs( std::chrono::steady_clock::time_point deadline ) noexcept
{
	TerminalTabClearResult result;
	if( !m_impl || m_impl->closed ) {
		result.status = TerminalTabClearStatus::Unavailable;
		return result;
	}
	// The old deadline was a synchronous join budget.  UI ownership now ends at
	// the nonblocking handoff, so no reporting deadline can make this method wait.
	static_cast<void>(deadline);
	result.clearedTabCount = m_impl->tabs.size();
	m_impl->ClearEventRoutes();
	if( m_impl->runtimeService ) {
		for( const auto& tab : m_impl->tabs ) {
			if( !tab->sessionId.IsValid() ) {
				m_impl->runtimeService->BeginCloseInstance(tab->instanceId);
				continue;
			}
			TerminalSessionCloseRequest request;
			request.operationId = MakeOperationId(tab->id);
			request.sessionId = tab->sessionId;
			static_cast<void>(m_impl->runtimeService->CloseSession(request));
		}
	}
	m_impl->tabs.clear();
	m_impl->activeTabId.reset();
	return result;
}

TerminalDetachedTabProjection TerminalTabManager::DetachTabs() noexcept
{
	TerminalDetachedTabProjection result;
	if( !m_impl || m_impl->closed ) return result;
	result.tabs.reserve(m_impl->tabs.size());
	for( const auto& tab : m_impl->tabs ) {
		result.tabs.push_back({ tab->id, tab->instanceId, tab->sessionId, tab->windowId, tab->paneId });
	}
	result.activeTabId = m_impl->activeTabId;
	m_impl->ClearEventRoutes();
	m_impl->tabs.clear();
	m_impl->activeTabId.reset();
	return result;
}

TerminalTabProjectionAttachResult TerminalTabManager::AttachTabs(
	TerminalDetachedTabProjection projection ) noexcept
{
	TerminalTabProjectionAttachResult result;
	if( !m_impl || m_impl->closed ) return result;
	if( !m_impl->tabs.empty() ) {
		result.code = TerminalTabProjectionAttachCode::Busy;
		result.errorCode = ERROR_BUSY;
		return result;
	}
	try {
		std::vector<std::uint64_t> ids;
		ids.reserve(projection.tabs.size());
		for( const auto& record : projection.tabs ) {
			if( record.tabId == 0 || !record.instanceId.IsValid()
				|| !m_impl->runtimeService || m_impl->runtimeService->Instance(record.instanceId) == nullptr ) {
				result.code = record.tabId == 0 || !record.instanceId.IsValid()
					? TerminalTabProjectionAttachCode::InvalidState
					: TerminalTabProjectionAttachCode::TargetMissing;
				result.errorCode = ERROR_NOT_FOUND;
				return result;
			}
			if( std::find(ids.begin(), ids.end(), record.tabId) != ids.end() ) {
				result.code = TerminalTabProjectionAttachCode::InvalidState;
				result.errorCode = ERROR_DUP_NAME;
				return result;
			}
			ids.push_back(record.tabId);
		}
		if( projection.activeTabId
			&& std::find(ids.begin(), ids.end(), *projection.activeTabId) == ids.end() ) {
			result.code = TerminalTabProjectionAttachCode::InvalidState;
			result.errorCode = ERROR_NOT_FOUND;
			return result;
		}

		for( const auto& record : projection.tabs ) {
			auto tab = std::make_unique<Impl::Tab>();
			tab->id = record.tabId;
			tab->instanceId = record.instanceId;
			tab->sessionId = record.sessionId;
			tab->windowId = record.windowId;
			tab->paneId = record.paneId;
			m_impl->tabs.push_back(std::move(tab));
			if( m_impl->nextTabId <= record.tabId ) {
				m_impl->nextTabId = record.tabId == std::numeric_limits<std::uint64_t>::max()
					? std::numeric_limits<std::uint64_t>::max() : record.tabId + 1;
			}
		}
		m_impl->activeTabId = projection.activeTabId;
		if( !m_impl->activeTabId && !m_impl->tabs.empty() ) m_impl->activeTabId = m_impl->tabs.front()->id;
		m_impl->RegisterProjectionRoutes();
		if( !m_impl->tabs.empty() ) m_impl->startedAnySession = true;
		result.code = TerminalTabProjectionAttachCode::Succeeded;
		result.attachedTabCount = m_impl->tabs.size();
		return result;
	} catch( ... ) {
		m_impl->ClearEventRoutes();
		m_impl->tabs.clear();
		m_impl->activeTabId.reset();
		result.code = TerminalTabProjectionAttachCode::InvalidState;
		result.errorCode = ERROR_NOT_ENOUGH_MEMORY;
		return result;
	}
}

TerminalTabTopologyReconcileResult TerminalTabManager::ReconcileTopology()
{
	TerminalTabTopologyReconcileResult result;
	if( !m_impl || m_impl->closed || !m_impl->runtimeService ) return result;
	try {
		const auto snapshot = m_impl->runtimeService->CollectionSnapshot();
		if( !snapshot ) return result;

		// A pane that left the topology was closed by whoever removed it, so the
		// tab goes without another close request.
		for( std::size_t index = 0; index < m_impl->tabs.size(); ) {
			const auto& tab = *m_impl->tabs[index];
			if( !tab.paneId.IsValid() || FindPaneSnapshot(*snapshot, tab.paneId) != nullptr ) {
				++index;
				continue;
			}
			const auto tabId = tab.id;
			m_impl->RemoveEventRoute(tab.instanceId);
			m_impl->tabs.erase(m_impl->tabs.begin() + static_cast<std::ptrdiff_t>(index));
			if( m_impl->activeTabId == tabId ) {
				if( m_impl->tabs.empty() ) m_impl->activeTabId.reset();
				else m_impl->activeTabId = m_impl->tabs[(std::min)(index, m_impl->tabs.size() - 1)]->id;
			}
			result.removed.push_back(tabId);
		}

		const auto tabForPane = [this]( const TerminalPaneId paneId ) -> std::optional<std::uint64_t> {
			for( const auto& tab : m_impl->tabs ) if( tab->paneId == paneId ) return tab->id;
			return std::nullopt;
		};
		const auto projectsSession = [this]( const TerminalSessionId sessionId ) {
			return std::any_of(m_impl->tabs.begin(), m_impl->tabs.end(),
				[sessionId]( const auto& tab ) { return tab->sessionId == sessionId; });
		};
		for( const auto& session : snapshot->sessions ) {
			if( !projectsSession(session.id) ) continue;
			for( const auto windowId : session.order ) {
				const auto window = std::find_if(session.windows.begin(), session.windows.end(),
					[windowId]( const auto& item ) { return item.id == windowId; });
				if( window == session.windows.end() ) continue;
				std::vector<TerminalPaneId> leaves;
				CollectLeaves(window->root, leaves);
				for( const auto paneId : leaves ) {
					if( tabForPane(paneId) ) continue;
					const auto* pane = FindPaneSnapshot(*snapshot, paneId);
					// A pane whose instance is still being created is adopted on the
					// event that follows its creation.
					if( pane == nullptr || m_impl->runtimeService->Instance(pane->instanceId) == nullptr ) continue;
					if( m_impl->nextTabId == std::numeric_limits<std::uint64_t>::max() ) return result;
					TerminalTabAdoption adoption;
					TerminalPaneId neighbor;
					TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
					if( FindSplitNeighbor(window->root, paneId, neighbor, orientation) ) {
						adoption.splitOf = tabForPane(neighbor);
						adoption.orientation = orientation;
					}
					if( !adoption.splitOf ) {
						for( const auto sibling : leaves ) {
							if( sibling == paneId ) continue;
							if( const auto siblingTab = tabForPane(sibling) ) {
								adoption.splitOf = siblingTab;
								break;
							}
						}
					}
					auto tab = std::make_unique<Impl::Tab>();
					tab->id = m_impl->nextTabId++;
					tab->instanceId = pane->instanceId;
					tab->sessionId = session.id;
					tab->windowId = window->id;
					tab->paneId = paneId;
					if( auto* model = m_impl->runtimeService->Model(tab->instanceId) ) {
						model->SetScrollbackLimit(m_impl->scrollbackLimit);
						static_cast<void>(model->ConsumeScrollbackChange());
					}
					adoption.tabId = tab->id;
					m_impl->tabs.emplace_back(std::move(tab));
					m_impl->startedAnySession = true;
					m_impl->RegisterEventRoute(pane->instanceId, adoption.tabId);
					if( !m_impl->activeTabId ) m_impl->activeTabId = adoption.tabId;
					result.adopted.push_back(adoption);
				}
			}
		}

		const auto current = CurrentPane(*snapshot);
		if( current.IsValid() && current != m_impl->lastRuntimeCurrentPane ) {
			m_impl->lastRuntimeCurrentPane = current;
			const auto currentTab = tabForPane(current);
			if( currentTab && currentTab != m_impl->activeTabId ) result.selected = currentTab;
		}
	} catch( ... ) {
		// Partial progress stays consistent: every adopted tab is fully routed
		// before the next one starts, and a removed tab never comes back.
	}
	return result;
}

void TerminalTabManager::Resize( TerminalSize rawSize )
{
	if( m_impl->closed ) return;
	const auto size = NormalizeSize(rawSize);
	for( auto& tab : m_impl->tabs ) {
		if( auto* instance = m_impl->runtimeService->Instance(tab->instanceId) ) {
			static_cast<void>(instance->Resize(size));
		}
	}
}

bool TerminalTabManager::ResizeTab( std::uint64_t tabId, TerminalSize rawSize )
{
	if( m_impl->closed ) return false;
	auto* tab = m_impl->Find(tabId);
	if( tab == nullptr ) return false;
	const auto size = NormalizeSize(rawSize);
	auto* instance = m_impl->runtimeService->Instance(tab->instanceId);
	return instance != nullptr && instance->Resize(size).succeeded;
}

std::vector<TerminalTabScrollbackChange> TerminalTabManager::SetScrollbackLimit( std::size_t lines )
{
	std::vector<TerminalTabScrollbackChange> changes;
	if( m_impl->closed ) return changes;
	m_impl->scrollbackLimit = std::min(lines, TerminalModel::kMaxScrollbackLines);
	changes.reserve(m_impl->tabs.size());
	for( auto& tab : m_impl->tabs ) {
		if( auto* model = m_impl->runtimeService->Model(tab->instanceId) ) {
			model->SetScrollbackLimit(m_impl->scrollbackLimit);
			auto change = model->ConsumeScrollbackChange();
			if( change.Changed() ) changes.push_back({ tab->id, change });
		}
	}
	return changes;
}

std::size_t TerminalTabManager::ScrollbackLimit() const noexcept
{
	return m_impl && !m_impl->closed ? m_impl->scrollbackLimit : 0;
}

void TerminalTabManager::Close() noexcept
{
	if( !m_impl || m_impl->closed ) return;
	m_impl->StopEvents();
	m_impl->runtimeSubscription.Reset();
	if( m_impl->ownsRuntimeService ) {
		const auto deadline = std::chrono::steady_clock::now()
			+ CTerminalSession::kGracefulCloseTimeout + CTerminalSession::kForcedCloseTimeout;
		static_cast<void>(ClearTabs(deadline));
		if( m_impl->runtimeService ) m_impl->runtimeService->BeginClose();
	} else {
		// The shared runtime outlives this projection. Destroy only the tab
		// projection and subscription; logical sessions/instances remain live.
		m_impl->tabs.clear();
		m_impl->activeTabId.reset();
	}
	m_impl->closed = true;
	m_impl->tabs.clear();
	m_impl->activeTabId.reset();
}

TerminalDrainResult TerminalTabManager::DrainOutput( std::uint64_t tabId )
{
	TerminalDrainResult result;
	if( m_impl->closed ) return result;
	auto* tab = m_impl->Find(tabId);
	if( tab == nullptr || !m_impl->runtimeService ) return result;
	result.found = true;
	result.active = m_impl->activeTabId == tabId;
	const auto drained = m_impl->runtimeService->DrainOutput(tab->instanceId);
	result.sequenceChanged = drained.sequenceChanged;
	result.synchronizedOutputCommitted = drained.synchronizedOutputCommitted;
	result.protocolInputPending = drained.protocolInputPending;
	result.protocolInputRejected = drained.protocolInputRejected;
	result.bytesDrained = drained.bytesDrained;
	result.scrollbackChange = drained.scrollbackChange;
	result.dirtyRows = drained.dirtyRows;
	result.clipboardWrite = drained.clipboardWrite;
	return result;
}

TerminalQueueInputResult TerminalTabManager::QueueActiveInput( std::span<const std::uint8_t> bytes )
{
	if( m_impl->closed || !m_impl->activeTabId ) return TerminalQueueInputResult::NotRunning;
	return QueueInput(*m_impl->activeTabId, bytes);
}

TerminalQueueInputResult TerminalTabManager::QueueInput( std::uint64_t tabId, std::span<const std::uint8_t> bytes )
{
	if( m_impl->closed ) return TerminalQueueInputResult::NotRunning;
	auto* tab = m_impl->Find(tabId);
	if( !tab || !m_impl->runtimeService ) return TerminalQueueInputResult::NotRunning;
	auto* instance = m_impl->runtimeService->Instance(tab->instanceId);
	return instance ? instance->QueueInput(bytes)
		: TerminalQueueInputResult::NotRunning;
}

TerminalQueueInputResult TerminalTabManager::FlushPendingProtocolInput( std::uint64_t tabId )
{
	if( m_impl->closed ) return TerminalQueueInputResult::NotRunning;
	auto* tab = m_impl->Find(tabId);
	if( !tab || !m_impl->runtimeService ) return TerminalQueueInputResult::NotRunning;
	auto* instance = m_impl->runtimeService->Instance(tab->instanceId);
	return instance ? instance->FlushPendingProtocolInput()
		: TerminalQueueInputResult::NotRunning;
}

void TerminalTabManager::RecordViewportDiagnostic(
	std::uint64_t tabId,
	const TerminalViewportDiagnosticSnapshot& snapshot ) noexcept
{
	if( m_impl->closed ) return;
	auto* tab = m_impl->Find(tabId);
	if( tab && m_impl->runtimeService ) {
		if( auto* instance = m_impl->runtimeService->Instance(tab->instanceId) ) {
			instance->RecordViewportDiagnostic(snapshot);
		}
	}
}

bool TerminalTabManager::HasPendingProtocolInput( std::uint64_t tabId ) const noexcept
{
	if( m_impl->closed ) return false;
	const auto* tab = m_impl->Find(tabId);
	const auto* instance = tab && m_impl->runtimeService
		? m_impl->runtimeService->Instance(tab->instanceId) : nullptr;
	return instance != nullptr && instance->HasPendingProtocolInput();
}

const TerminalModel* TerminalTabManager::Model( std::uint64_t tabId ) const noexcept
{
	if( m_impl->closed ) return nullptr;
	const auto* tab = m_impl->Find(tabId);
	return tab && m_impl->runtimeService ? m_impl->runtimeService->Model(tab->instanceId) : nullptr;
}

TerminalModel* TerminalTabManager::Model( std::uint64_t tabId ) noexcept
{
	return const_cast<TerminalModel*>(std::as_const(*this).Model(tabId));
}

const TerminalModel* TerminalTabManager::ActiveModel() const noexcept
{
	if( m_impl->closed || !m_impl->activeTabId ) return nullptr;
	const auto* tab = m_impl->Find(*m_impl->activeTabId);
	return tab && m_impl->runtimeService ? m_impl->runtimeService->Model(tab->instanceId) : nullptr;
}

TerminalModel* TerminalTabManager::ActiveModel() noexcept
{
	return const_cast<TerminalModel*>(std::as_const(*this).ActiveModel());
}

const SakuraTerminalInputAdapter* TerminalTabManager::ActiveInputAdapter() const noexcept
{
	if( m_impl->closed || !m_impl->activeTabId ) return nullptr;
	const auto* tab = m_impl->Find(*m_impl->activeTabId);
	return tab && m_impl->runtimeService ? m_impl->runtimeService->InputAdapter(tab->instanceId) : nullptr;
}

SakuraTerminalInputAdapter* TerminalTabManager::ActiveInputAdapter() noexcept
{
	return const_cast<SakuraTerminalInputAdapter*>(std::as_const(*this).ActiveInputAdapter());
}

const SakuraTerminalInputAdapter* TerminalTabManager::InputAdapter( std::uint64_t tabId ) const noexcept
{
	if( m_impl->closed ) return nullptr;
	const auto* tab = m_impl->Find(tabId);
	return tab && m_impl->runtimeService ? m_impl->runtimeService->InputAdapter(tab->instanceId) : nullptr;
}

SakuraTerminalInputAdapter* TerminalTabManager::InputAdapter( std::uint64_t tabId ) noexcept
{
	return const_cast<SakuraTerminalInputAdapter*>(std::as_const(*this).InputAdapter(tabId));
}

std::optional<std::uint64_t> TerminalTabManager::ActiveTabId() const noexcept
{
	return m_impl->closed ? std::nullopt : m_impl->activeTabId;
}

std::vector<TerminalTabSnapshot> TerminalTabManager::Snapshot() const
{
	std::vector<TerminalTabSnapshot> result;
	if( m_impl->closed ) return result;
	result.reserve(m_impl->tabs.size());
	for( const auto& tab : m_impl->tabs ) {
		const auto* instance = m_impl->runtimeService
			? m_impl->runtimeService->Instance(tab->instanceId) : nullptr;
		if( !instance ) continue;
		const auto snapshot = instance->Snapshot();
		result.push_back({ tab->id, snapshot.processName, snapshot.profileLabel, snapshot.sequenceTitle,
			snapshot.initialWorkingDirectory, snapshot.sessionState, snapshot.errorCode,
			m_impl->activeTabId == tab->id });
	}
	return result;
}

std::vector<TerminalTabProjectionRecord> TerminalTabManager::ProjectionSnapshot() const
{
	std::vector<TerminalTabProjectionRecord> result;
	if( m_impl->closed ) return result;
	result.reserve(m_impl->tabs.size());
	for( const auto& tab : m_impl->tabs ) {
		result.push_back({ tab->id, tab->instanceId, tab->sessionId, tab->windowId, tab->paneId });
	}
	return result;
}

std::optional<TerminalBackendJobProcesses> TerminalTabManager::ObserveJobProcesses( std::uint64_t tabId ) const noexcept
{
	if( m_impl->closed || !m_impl->runtimeService ) return std::nullopt;
	const auto found = std::find_if(m_impl->tabs.begin(), m_impl->tabs.end(), [tabId](const auto& tab) { return tab->id == tabId; });
	if( found == m_impl->tabs.end() ) return std::nullopt;
	const auto* instance = std::as_const(*m_impl->runtimeService).Instance((*found)->instanceId);
	return instance ? instance->ObserveJobProcesses() : std::nullopt;
}

std::size_t TerminalTabManager::TabCount() const noexcept
{
	return m_impl->closed ? 0 : m_impl->tabs.size();
}

bool TerminalTabManager::HasStartedAnySession() const noexcept
{
	return m_impl->startedAnySession;
}

} // namespace terminal
