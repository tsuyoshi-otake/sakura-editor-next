/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalTabManager.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/parser/TerminalParser.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <utility>
#include <windows.h>

namespace terminal {
namespace {

constexpr wchar_t kDefaultTabLabel[] = L"PowerShell";

std::wstring InitialTabLabel( const TerminalLaunchOptions& options )
{
	const std::filesystem::path executable(options.executablePath);
	const auto stem = executable.stem().wstring();
	return stem.empty() ? std::wstring(kDefaultTabLabel) : stem;
}

TerminalSize NormalizeSize( TerminalSize size ) noexcept
{
	size.columns = std::max<std::uint16_t>(1, size.columns);
	size.rows = std::max<std::uint16_t>(1, size.rows);
	return size;
}

} // namespace

struct TerminalTabManager::Impl {
	struct Tab {
		Tab( std::uint64_t tabId, TerminalSize size )
			: id(tabId)
			, input(std::make_unique<SakuraTerminalInputAdapter>())
			, model(std::make_unique<TerminalModel>(size.columns, size.rows))
			, parser(std::make_unique<TerminalParser>(*model, input.get()))
		{
		}

		std::uint64_t id{};
		std::wstring label{ kDefaultTabLabel };
		std::unique_ptr<SakuraTerminalInputAdapter> input;
		std::unique_ptr<TerminalModel> model;
		std::unique_ptr<TerminalParser> parser;
		std::unique_ptr<CTerminalSession> session;
		TerminalSessionState state{ TerminalSessionState::Idle };
		std::uint32_t errorCode{};
	};

	TerminalTabManagerDependencies dependencies;
	TerminalTabEventCallback eventCallback;
	std::vector<std::unique_ptr<Tab>> tabs;
	std::optional<std::uint64_t> activeTabId;
	std::uint64_t nextTabId{ 1 };
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

	bool Start( Tab& tab, TerminalSize rawSize, std::wstring_view workingDirectory )
	{
		const auto size = NormalizeSize(rawSize);
		tab.input = std::make_unique<SakuraTerminalInputAdapter>();
		tab.model = std::make_unique<TerminalModel>(size.columns, size.rows);
		tab.parser = std::make_unique<TerminalParser>(*tab.model, tab.input.get());
		tab.state = TerminalSessionState::Starting;
		tab.errorCode = 0;
		if( !dependencies.resolveLaunch || !dependencies.createSession ) {
			tab.state = TerminalSessionState::Failed;
			tab.errorCode = ERROR_INVALID_FUNCTION;
			return false;
		}
		auto launch = dependencies.resolveLaunch(size, workingDirectory);
		if( !launch || launch->executablePath.empty() ) {
			tab.state = TerminalSessionState::Failed;
			tab.errorCode = ERROR_FILE_NOT_FOUND;
			if( eventCallback ) eventCallback({ TerminalTabEventKind::StateChanged, tab.id, tab.state, tab.errorCode });
			return false;
		}
		launch->initialSize = size;
		if( launch->workingDirectory.empty() ) launch->workingDirectory.assign(workingDirectory);
		tab.label = InitialTabLabel(*launch);
		const auto callback = eventCallback;
		const auto id = tab.id;
		TerminalSessionCallbacks callbacks;
		callbacks.outputAvailable = [callback, id] {
			if( callback ) callback({ TerminalTabEventKind::OutputAvailable, id, TerminalSessionState::Running, 0 });
		};
		callbacks.stateChanged = [callback, id](TerminalSessionState state, std::uint32_t errorCode) {
			if( callback ) callback({ TerminalTabEventKind::StateChanged, id, state, errorCode });
		};
		tab.session = dependencies.createSession(std::move(callbacks));
		if( !tab.session ) {
			tab.state = TerminalSessionState::Failed;
			tab.errorCode = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
		startedAnySession = true;
		const auto result = tab.session->Start(*launch);
		tab.state = tab.session->GetState();
		tab.errorCode = result.succeeded ? 0 : result.errorCode;
		return result.succeeded;
	}
};

TerminalTabManager::TerminalTabManager( TerminalTabManagerDependencies dependencies, TerminalTabEventCallback eventCallback )
	: m_impl(std::make_unique<Impl>())
{
	m_impl->dependencies = std::move(dependencies);
	m_impl->eventCallback = std::move(eventCallback);
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
		auto tab = std::make_unique<Impl::Tab>(id, NormalizeSize(size));
		m_impl->Start(*tab, size, workingDirectory);
		m_impl->tabs.emplace_back(std::move(tab));
		m_impl->activeTabId = id;
		return id;
	} catch( ... ) {
		return std::nullopt;
	}
}

bool TerminalTabManager::SelectTab( std::uint64_t tabId ) noexcept
{
	if( m_impl->closed || m_impl->Find(tabId) == nullptr ) return false;
	m_impl->activeTabId = tabId;
	return true;
}

bool TerminalTabManager::RestartTab( std::uint64_t tabId, TerminalSize size, std::wstring_view workingDirectory )
{
	if( m_impl->closed ) return false;
	auto* tab = m_impl->Find(tabId);
	if( tab == nullptr ) return false;
	if( tab->session ) tab->session->Close();
	tab->session.reset();
	tab->label = kDefaultTabLabel;
	return m_impl->Start(*tab, size, workingDirectory);
}

bool TerminalTabManager::DeleteTab( std::uint64_t tabId ) noexcept
{
	if( m_impl->closed ) return false;
	const auto found = std::find_if( m_impl->tabs.begin(), m_impl->tabs.end(), [tabId](const auto& tab) { return tab->id == tabId; } );
	if( found == m_impl->tabs.end() ) return false;
	if( (*found)->session ) (*found)->session->Close();
	const auto index = static_cast<std::size_t>(found - m_impl->tabs.begin());
	m_impl->tabs.erase(found);
	if( m_impl->activeTabId == tabId ) {
		if( m_impl->tabs.empty() ) m_impl->activeTabId.reset();
		else m_impl->activeTabId = m_impl->tabs[std::min(index, m_impl->tabs.size() - 1)]->id;
	}
	return true;
}

void TerminalTabManager::Resize( TerminalSize rawSize )
{
	if( m_impl->closed ) return;
	const auto size = NormalizeSize(rawSize);
	for( auto& tab : m_impl->tabs ) {
		tab->model->Resize(size.columns, size.rows);
		if( tab->session ) tab->session->RequestResize(size);
	}
}

bool TerminalTabManager::ResizeTab( std::uint64_t tabId, TerminalSize rawSize )
{
	if( m_impl->closed ) return false;
	auto* tab = m_impl->Find(tabId);
	if( tab == nullptr ) return false;
	const auto size = NormalizeSize(rawSize);
	tab->model->Resize(size.columns, size.rows);
	if( tab->session ) tab->session->RequestResize(size);
	return true;
}

void TerminalTabManager::Close() noexcept
{
	if( !m_impl || m_impl->closed ) return;
	m_impl->closed = true;
	for( auto& tab : m_impl->tabs ) if( tab->session ) tab->session->Close();
	m_impl->tabs.clear();
	m_impl->activeTabId.reset();
}

TerminalDrainResult TerminalTabManager::DrainOutput( std::uint64_t tabId )
{
	TerminalDrainResult result;
	if( m_impl->closed ) return result;
	auto* tab = m_impl->Find(tabId);
	if( tab == nullptr || !tab->session ) return result;
	result.found = true;
	result.active = m_impl->activeTabId == tabId;
	const auto beforeTitle = tab->model->Title();
	const auto bytes = tab->session->DrainOutput();
	result.bytesDrained = bytes.size();
	if( !bytes.empty() ) tab->parser->Feed(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
	if( tab->model->Title() != beforeTitle ) {
		tab->label = tab->model->Title().empty() ? std::wstring(kDefaultTabLabel) : tab->model->Title();
		result.titleChanged = true;
	}
	result.dirtyRows = tab->model->ConsumeDirtyRows();
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
	return tab && tab->session ? tab->session->QueueInput(bytes) : TerminalQueueInputResult::NotRunning;
}

const TerminalModel* TerminalTabManager::Model( std::uint64_t tabId ) const noexcept
{
	if( m_impl->closed ) return nullptr;
	const auto* tab = m_impl->Find(tabId);
	return tab ? tab->model.get() : nullptr;
}

TerminalModel* TerminalTabManager::Model( std::uint64_t tabId ) noexcept
{
	return const_cast<TerminalModel*>(std::as_const(*this).Model(tabId));
}

const TerminalModel* TerminalTabManager::ActiveModel() const noexcept
{
	if( m_impl->closed || !m_impl->activeTabId ) return nullptr;
	const auto* tab = m_impl->Find(*m_impl->activeTabId);
	return tab ? tab->model.get() : nullptr;
}

TerminalModel* TerminalTabManager::ActiveModel() noexcept
{
	return const_cast<TerminalModel*>(std::as_const(*this).ActiveModel());
}

const SakuraTerminalInputAdapter* TerminalTabManager::ActiveInputAdapter() const noexcept
{
	if( m_impl->closed || !m_impl->activeTabId ) return nullptr;
	const auto* tab = m_impl->Find(*m_impl->activeTabId);
	return tab ? tab->input.get() : nullptr;
}

SakuraTerminalInputAdapter* TerminalTabManager::ActiveInputAdapter() noexcept
{
	return const_cast<SakuraTerminalInputAdapter*>(std::as_const(*this).ActiveInputAdapter());
}

const SakuraTerminalInputAdapter* TerminalTabManager::InputAdapter( std::uint64_t tabId ) const noexcept
{
	if( m_impl->closed ) return nullptr;
	const auto* tab = m_impl->Find(tabId);
	return tab ? tab->input.get() : nullptr;
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
		const auto state = tab->session ? tab->session->GetState() : tab->state;
		const auto error = tab->session ? tab->session->GetLastError() : tab->errorCode;
		result.push_back({ tab->id, tab->label, state, error, m_impl->activeTabId == tab->id });
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
