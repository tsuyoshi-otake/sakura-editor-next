/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::statusbar {

enum class EStatusbarMementoLoadStatus : std::uint8_t { Loaded, NotFound, Invalid, Unavailable, Failed };
enum class EStatusbarMementoSaveStatus : std::uint8_t { Persisted, NotDirty, Conflict, Unavailable, Failed };

struct StatusbarMementoLoadResult final {
	EStatusbarMementoLoadStatus status = EStatusbarMementoLoadStatus::Failed;
	std::optional<std::vector<std::string>> hiddenIds;
	std::wstring diagnostic;
};
struct StatusbarMementoSaveResult final {
	EStatusbarMementoSaveStatus status = EStatusbarMementoSaveStatus::Failed;
	std::wstring diagnostic;
	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EStatusbarMementoSaveStatus::Persisted || status == EStatusbarMementoSaveStatus::NotDirty;
	}
};

class IStatusbarVisibilityMementoStore {
public:
	virtual ~IStatusbarVisibilityMementoStore() = default;
	[[nodiscard]] virtual StatusbarMementoLoadResult Load() = 0;
	[[nodiscard]] virtual StatusbarMementoSaveResult Save(const std::vector<std::string>& hiddenIds) = 0;
};

} // namespace workbench::statusbar
