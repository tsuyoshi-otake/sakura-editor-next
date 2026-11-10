/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionRestoreService.h"

#include <utility>

namespace workbench::projects {

CAgentSessionRestoreService::CAgentSessionRestoreService(
	std::unique_ptr<IProjectCatalogStore> store) noexcept :
	m_store(std::move(store))
{
}

void CAgentSessionRestoreService::Load()
{
	if (!m_store) return;
	const auto loaded = m_store->Load();
	switch (loaded.status) {
	case EProjectCatalogStoreLoadStatus::Succeeded: {
		auto decoded = loaded.payload ? DecodeAgentSessionRestoreState(*loaded.payload)
			: std::nullopt;
		m_writable = decoded.has_value();
		m_state = decoded ? std::move(*decoded) : AgentSessionRestoreState{};
		return;
	}
	case EProjectCatalogStoreLoadStatus::NotFound:
		m_state = {};
		m_writable = true;
		return;
	case EProjectCatalogStoreLoadStatus::Unavailable:
	case EProjectCatalogStoreLoadStatus::Failed:
		// The state this window already has stays in use.
		m_writable = false;
		return;
	}
}

std::vector<std::wstring> CAgentSessionRestoreService::RecordedSessions(
	const std::wstring_view folder) const
{
	const auto recorded = FindAgentSessionRestoreEntry(m_state, folder);
	return { recorded.begin(), recorded.end() };
}

EProjectCatalogStoreSaveStatus CAgentSessionRestoreService::Save()
{
	auto payload = EncodeAgentSessionRestoreState(m_state);
	if (!payload) return EProjectCatalogStoreSaveStatus::Failed;
	return m_store->Save(std::move(*payload)).status;
}

bool CAgentSessionRestoreService::Update(const std::function<bool(AgentSessionRestoreState&)>& change)
{
	if (!change) return false;
	// A window that started before storage was ready gets its first load here.
	if (!m_writable) Load();
	auto next = m_state;
	if (!change(next)) return false;
	m_state = std::move(next);
	if (!m_store || !m_writable) return true;
	if (Save() == EProjectCatalogStoreSaveStatus::Conflict) {
		Load();
		auto merged = m_state;
		if (m_writable && change(merged)) {
			m_state = std::move(merged);
			(void)Save();
		}
	}
	return true;
}

} // namespace workbench::projects
