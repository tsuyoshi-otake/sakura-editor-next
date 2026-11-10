/*! @file
 * @brief Profile-scoped owner of the agent-session restore record (#406 Phase 2):
 * which sessions were live when a window left each Project folder.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/projects/AgentSessionRestore.h"
#include "workbench/projects/ProjectCatalogService.h"

#include <functional>
#include <memory>

namespace workbench::projects {

//! UI-thread service. The record is shared by every window of the profile, so
//! a window re-reads it (Load) before planning a restore.
class IAgentSessionRestoreService {
public:
	virtual ~IAgentSessionRestoreService() = default;
	//! Reads the stored state. Nothing stored gives an empty record. A record
	//! that cannot be decoded is kept on disk and gives an empty record too.
	virtual void Load() = 0;
	//! The state last loaded or updated; empty before the first Load.
	[[nodiscard]] virtual AgentSessionRestoreState Snapshot() const = 0;
	//! The recorded session identities for `folder`; empty when none.
	[[nodiscard]] virtual std::vector<std::wstring> RecordedSessions(
		std::wstring_view folder) const = 0;
	//! Applies `change`, which returns whether it changed the state, and saves
	//! the result. Returns whether the state changed. When another window
	//! saved first, its state is loaded and `change` is applied to that one.
	//! A change that cannot be saved still holds in this window until the
	//! next Load.
	virtual bool Update(const std::function<bool(AgentSessionRestoreState&)>& change) = 0;
};

//! The store is one profile-scoped payload with the coherent-load-before-save
//! contract of IProjectCatalogStore; composition gives it its own storage key.
class CAgentSessionRestoreService final : public IAgentSessionRestoreService {
public:
	explicit CAgentSessionRestoreService(std::unique_ptr<IProjectCatalogStore> store) noexcept;

	void Load() override;
	[[nodiscard]] AgentSessionRestoreState Snapshot() const override { return m_state; }
	[[nodiscard]] std::vector<std::wstring> RecordedSessions(
		std::wstring_view folder) const override;
	bool Update(const std::function<bool(AgentSessionRestoreState&)>& change) override;

private:
	[[nodiscard]] EProjectCatalogStoreSaveStatus Save();

	std::unique_ptr<IProjectCatalogStore> m_store;
	AgentSessionRestoreState m_state;
	//! A coherent load found a usable record or none; only then may Save run.
	bool m_writable = false;
};

} // namespace workbench::projects
