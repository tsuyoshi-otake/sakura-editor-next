/*! @file
 * @brief Profile-scoped owner of the Sessions view state: pins, archive marks, display count (#408).
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/projects/AgentSessionManagement.h"
#include "workbench/projects/ProjectCatalogService.h"

#include <functional>
#include <memory>

namespace workbench::projects {

//! UI-thread service. The state is shared by every window of the profile, so
//! a window re-reads it (Load) when its Sessions view refreshes.
class IAgentSessionViewStateService {
public:
	virtual ~IAgentSessionViewStateService() = default;
	//! Reads the stored state. Nothing stored gives the defaults. A record
	//! that cannot be decoded gives the defaults too and is never overwritten.
	virtual void Load() = 0;
	//! The state last loaded or updated; the defaults before the first Load.
	[[nodiscard]] virtual AgentSessionViewState Snapshot() const = 0;
	//! Applies `change`, which returns whether it changed the state, and saves
	//! the result. Returns whether the state changed. When another window
	//! saved first, its state is loaded and `change` is applied to that one.
	//! A change that cannot be saved still holds in this window until the
	//! next Load.
	virtual bool Update(const std::function<bool(AgentSessionViewState&)>& change) = 0;
};

//! The store is one profile-scoped payload with the coherent-load-before-save
//! contract of IProjectCatalogStore; composition gives it its own storage key.
class CAgentSessionViewStateService final : public IAgentSessionViewStateService {
public:
	explicit CAgentSessionViewStateService(std::unique_ptr<IProjectCatalogStore> store) noexcept;

	void Load() override;
	[[nodiscard]] AgentSessionViewState Snapshot() const override { return m_state; }
	bool Update(const std::function<bool(AgentSessionViewState&)>& change) override;

private:
	[[nodiscard]] EProjectCatalogStoreSaveStatus Save();

	std::unique_ptr<IProjectCatalogStore> m_store;
	AgentSessionViewState m_state;
	//! A coherent load found a usable record or none; only then may Save run.
	bool m_writable = false;
};

} // namespace workbench::projects
