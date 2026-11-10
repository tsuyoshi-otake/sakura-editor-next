/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionViewStateService.h"

#include <utility>

namespace workbench::projects {

CAgentSessionViewStateService::CAgentSessionViewStateService(
	std::unique_ptr<IProjectCatalogStore> store) noexcept :
	m_store(std::move(store))
{
}

void CAgentSessionViewStateService::Load()
{
	if (!m_store) return;
	const auto loaded = m_store->Load();
	switch (loaded.status) {
	case EProjectCatalogStoreLoadStatus::Succeeded: {
		auto decoded = loaded.payload ? DecodeAgentSessionViewState(*loaded.payload) : std::nullopt;
		m_writable = decoded.has_value();
		m_state = decoded ? std::move(*decoded) : AgentSessionViewState{};
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

EProjectCatalogStoreSaveStatus CAgentSessionViewStateService::Save()
{
	auto payload = EncodeAgentSessionViewState(m_state);
	if (!payload) return EProjectCatalogStoreSaveStatus::Failed;
	return m_store->Save(std::move(*payload)).status;
}

bool CAgentSessionViewStateService::Update(const std::function<bool(AgentSessionViewState&)>& change)
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
