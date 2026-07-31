/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/IWorkspaceContextService.h"

#include <memory>

namespace config {

//! Thread-safe P0 semantic core. It has no UI, Win32, storage, or filesystem
//! dependency. The supplied empty-window identifier remains immutable for the
//! life of this service instance.
class CWorkspaceContextService final : public IWorkspaceContextService {
public:
	explicit CWorkspaceContextService(std::wstring emptyWindowIdentity, std::uint64_t generation = 1);
	~CWorkspaceContextService() override;

	CWorkspaceContextService(const CWorkspaceContextService&) = delete;
	CWorkspaceContextService& operator=(const CWorkspaceContextService&) = delete;

	WorkspaceContextSnapshot Snapshot() const override;
	WorkspaceContextResult SetEmpty(const WorkspaceContextOperation& operation) override;
	WorkspaceContextResult SetFolder(const SetFolderRequest& request) override;
	WorkspaceContextResult SetWorkspace(const SetWorkspaceRequest& request) override;
	WorkspaceContextResult SetTrust(const SetTrustRequest& request) override;
	WorkspaceContextSubscription Subscribe(WorkspaceContextListener listener) override;

	struct State;

private:
	std::shared_ptr<State> m_state;
};

} // namespace config
