/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateTypes.h"

namespace update {

std::string_view UpdateStateTypeId(EUpdateStateType state) noexcept
{
	switch (state) {
	case EUpdateStateType::Uninitialized:        return "uninitialized";
	case EUpdateStateType::Disabled:             return "disabled";
	case EUpdateStateType::Idle:                 return "idle";
	case EUpdateStateType::CheckingForUpdates:   return "checking for updates";
	case EUpdateStateType::AvailableForDownload: return "available for download";
	case EUpdateStateType::Downloading:          return "downloading";
	case EUpdateStateType::Downloaded:           return "downloaded";
	case EUpdateStateType::Updating:             return "updating";
	case EUpdateStateType::Ready:                return "ready";
	case EUpdateStateType::Overwriting:          return "overwriting";
	case EUpdateStateType::Cancelling:           return "cancelling";
	case EUpdateStateType::Restarting:           return "restarting";
	}
	return "uninitialized";
}

std::optional<EUpdateStateType> ParseUpdateStateTypeId(std::string_view id) noexcept
{
	constexpr EUpdateStateType kStates[] = {
		EUpdateStateType::Uninitialized,
		EUpdateStateType::Disabled,
		EUpdateStateType::Idle,
		EUpdateStateType::CheckingForUpdates,
		EUpdateStateType::AvailableForDownload,
		EUpdateStateType::Downloading,
		EUpdateStateType::Downloaded,
		EUpdateStateType::Updating,
		EUpdateStateType::Ready,
		EUpdateStateType::Overwriting,
		EUpdateStateType::Cancelling,
		EUpdateStateType::Restarting,
	};
	for (const auto state : kStates) {
		if (UpdateStateTypeId(state) == id) return state;
	}
	return std::nullopt;
}

std::string_view UpdateModeId(EUpdateMode mode) noexcept
{
	switch (mode) {
	case EUpdateMode::None:    return "none";
	case EUpdateMode::Manual:  return "manual";
	case EUpdateMode::Start:   return "start";
	case EUpdateMode::Default: return "default";
	}
	return "default";
}

std::optional<EUpdateMode> ParseUpdateModeId(std::string_view id) noexcept
{
	if (id == "none")    return EUpdateMode::None;
	if (id == "manual")  return EUpdateMode::Manual;
	if (id == "start")   return EUpdateMode::Start;
	if (id == "default") return EUpdateMode::Default;
	return std::nullopt;
}

bool IsActionableUpdateState(EUpdateStateType state) noexcept
{
	return state == EUpdateStateType::AvailableForDownload
		|| state == EUpdateStateType::Downloaded
		|| state == EUpdateStateType::Ready;
}

std::string_view UpdateIndicatorCommandId(EUpdateStateType state) noexcept
{
	switch (state) {
	case EUpdateStateType::AvailableForDownload: return "update.downloadNow";
	case EUpdateStateType::Downloaded:           return "update.install";
	case EUpdateStateType::Ready:                return "update.restart";
	default:                                     return {};
	}
}

} // namespace update
