/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/WorkspaceContextTypes.h"

namespace config {

//! Pure in-memory workspace context and explicit trust contract.
class IWorkspaceContextService {
public:
	virtual ~IWorkspaceContextService() = default;

	virtual WorkspaceContextSnapshot Snapshot() const = 0;
	virtual WorkspaceContextResult SetEmpty(const WorkspaceContextOperation& operation) = 0;
	virtual WorkspaceContextResult SetFolder(const SetFolderRequest& request) = 0;
	virtual WorkspaceContextResult SetWorkspace(const SetWorkspaceRequest& request) = 0;
	virtual WorkspaceContextResult SetTrust(const SetTrustRequest& request) = 0;
	virtual WorkspaceContextSubscription Subscribe(WorkspaceContextListener listener) = 0;
};

} // namespace config
