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

} // namespace

struct TerminalTabManager::Impl {
	struct Tab {
		std::uint64_t id{};
		TerminalInstanceId instanceId;
		TerminalSessionId sessionId;
		TerminalWindowId windowId;
		TerminalPaneId paneId;
	};

	TerminalTabManagerDependencies dependencies;
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
		std::lock_guard lock(eventMutex);
		if( !acceptingEvents ) return;
		eventRoutes.push_back({ instanceId, tabId });
	}

	void ReplaceEventRoute( const TerminalInstanceId oldInstanceId,
		const TerminalInstanceId newInstanceId, const std::uint64_t tabId )
	{
		std::lock_guard lock(eventMutex);
		const auto old = std::remove_if(eventRoutes.begin(), eventRoutes.end(),
			[oldInstanceId](const EventRoute& route) { return route.instanceId == oldInstanceId; });
		eventRoutes.erase(old, eventRoutes.end());
		if( acceptingEvents ) eventRoutes.push_back({ newInstanceId, tabId });
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
		ClearEventRoutes();
		for( const auto& tab : tabs ) RegisterEventRoute(tab->instanceId, tab->id);
	}

	void OnRuntimeEvent( const TerminalInstanceEvent& event ) noexcept
	{
		TerminalTabEvent translated;
		TerminalTabEventCallback callback;
		try {
			{
				std::lock_guard lock(eventMutex);
				if( !acceptingEvents || !eventCallback ) return;
				const auto found = std::find_if(eventRoutes.begin(), eventRoutes.end(), [&](const EventRoute& route) {
					return route.instanceId == event.coordinate.instanceId;
				});
				if( found == eventRoutes.end() ) return;
				translated.kind = event.kind == TerminalInstanceEventKind::OutputAvailable
					? TerminalTabEventKind::OutputAvailable : TerminalTabEventKind::StateChanged;
				translated.tabId = found->tabId;
				translated.state = event.sessionState;
				translated.errorCode = event.errorCode;
				callback = eventCallback;
			}
			callback(translated);
		} catch( ... ) {
			// A projection callback is advisory and must not unwind a session worker.
		}
	}
};

TerminalTabManager::TerminalTabManager( TerminalTabManagerDependencies dependencies, TerminalTabEventCallback eventCallback )
	: m_impl(std::make_shared<Impl>())
{
	m_impl->dependencies = std::move(dependencies);
	m_impl->eventCallback = std::move(eventCallback);
	if( m_impl->dependencies.runtimeService ) {
		m_impl->runtimeService = std::move(m_impl->dependencies.runtimeService);
		m_impl->ownsRuntimeService = false;
	} else {
		TerminalRuntimeServiceDependencies runtimeDependencies;
		runtimeDependencies.createSession = m_impl->dependencies.createSession;
		runtimeDependencies.resolveLaunch = m_impl->dependencies.resolveLaunch;
		runtimeDependencies.decorateLaunch = m_impl->dependencies.decorateLaunch;
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
	if( m_impl->runtimeService && oldSession.IsValid() ) {
		TerminalSessionCloseRequest closeRequest;
		closeRequest.operationId = MakeOperationId(tabId ^ 0x8000000000000000ULL);
		closeRequest.sessionId = oldSession;
		static_cast<void>(m_impl->runtimeService->CloseSession(closeRequest));
	} else if( m_impl->runtimeService ) {
		m_impl->runtimeService->BeginCloseInstance(oldInstance, TerminalInstanceCloseReason::Explicit);
	}
	return created.code == TerminalRuntimeOperationCode::Succeeded;
}

bool TerminalTabManager::DeleteTab( std::uint64_t tabId ) noexcept
{
	if( m_impl->closed ) return false;
	const auto found = std::find_if( m_impl->tabs.begin(), m_impl->tabs.end(), [tabId](const auto& tab) { return tab->id == tabId; } );
	if( found == m_impl->tabs.end() ) return false;
	m_impl->RemoveEventRoute((*found)->instanceId);
	if( m_impl->runtimeService && (*found)->sessionId.IsValid() ) {
		TerminalSessionCloseRequest request;
		request.operationId = MakeOperationId(tabId);
		request.sessionId = (*found)->sessionId;
		static_cast<void>(m_impl->runtimeService->CloseSession(request));
	} else if( m_impl->runtimeService ) {
		m_impl->runtimeService->BeginCloseInstance((*found)->instanceId);
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

std::size_t TerminalTabManager::TabCount() const noexcept
{
	return m_impl->closed ? 0 : m_impl->tabs.size();
}

bool TerminalTabManager::HasStartedAnySession() const noexcept
{
	return m_impl->startedAnySession;
}

} // namespace terminal
