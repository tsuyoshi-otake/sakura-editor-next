/*! @file
 * @brief Workbench-facing persistence contract for the bounded layout memento.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <cstdint>
#include <optional>
#include <string>

namespace workbench::layout {

//! A load never exposes a partially decoded or incoherent memento.
enum class EWorkbenchLayoutMementoLoadStatus : std::uint8_t {
	Loaded,
	NotFound,
	InvalidStoredMemento,
	Unavailable,
	Failed,
};

struct WorkbenchLayoutMementoLoadResult final {
	EWorkbenchLayoutMementoLoadStatus status = EWorkbenchLayoutMementoLoadStatus::Failed;
	std::optional<WorkbenchLayoutStateSnapshot> snapshot;
	//! Diagnostics deliberately exclude profile paths, storage addresses, and durable payload values.
	std::wstring diagnostic;

	[[nodiscard]] bool Loaded() const noexcept
	{
		return status == EWorkbenchLayoutMementoLoadStatus::Loaded && snapshot.has_value();
	}
};

//! A save has one terminal result; conflicts are not retried with a new snapshot.
enum class EWorkbenchLayoutMementoSaveStatus : std::uint8_t {
	Persisted,
	NotDirty,
	Conflict,
	RetryExhausted,
	Unavailable,
	Stopped,
	Failed,
};

struct WorkbenchLayoutMementoSaveResult final {
	EWorkbenchLayoutMementoSaveStatus status = EWorkbenchLayoutMementoSaveStatus::Failed;
	std::wstring diagnostic;
};

/*! 
	@brief Persistence boundary owned by the workbench composition edge.

	A caller loads once before saving. A successful NotFound or Loaded result captures
	the storage revision used for the next compare-and-swap save. Implementations
	must never turn invalid durable memento data into a replacement write.
*/
class IWorkbenchLayoutMementoStore {
public:
	virtual ~IWorkbenchLayoutMementoStore() = default;
	[[nodiscard]] virtual WorkbenchLayoutMementoLoadResult Load() = 0;
	[[nodiscard]] virtual WorkbenchLayoutMementoSaveResult Save(const WorkbenchLayoutStateSnapshot& snapshot) = 0;
};

} // namespace workbench::layout
