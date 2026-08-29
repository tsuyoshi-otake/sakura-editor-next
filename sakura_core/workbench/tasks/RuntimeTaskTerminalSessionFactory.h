/*! @file @brief Task execution adapter for the process-owned terminal runtime. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/tasks/TaskExecutionService.h"

#include <memory>

namespace terminal {
class CDefaultTerminalLaunchProfileService;
class CTerminalRuntimeService;
}

namespace workbench::tasks {

//! Creates Task sessions that borrow the same process-owned runtime as the
//! interactive terminal projection. Task launches never receive Harness
//! credentials or the Sakura-scoped tmux shim directory.
[[nodiscard]] std::shared_ptr<ITaskExecutionSessionFactory>
CreateRuntimeTaskTerminalSessionFactory(
	std::shared_ptr<terminal::CTerminalRuntimeService> runtime,
	std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profiles);

} // namespace workbench::tasks
