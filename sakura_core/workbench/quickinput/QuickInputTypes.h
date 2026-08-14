/*! @file
	@brief Native quick-pick and input-box value types
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class EQuickInputKind { QuickPick, InputBox };
enum class EQuickInputState { Accepted, Cancelled, HostLost, Overloaded };

struct SQuickPickItem {
	std::size_t sourceIndex = 0;
	std::wstring label;
	std::wstring description;
	std::wstring detail;
	bool picked = false;
};

struct SQuickInputRequest {
	EQuickInputKind kind = EQuickInputKind::QuickPick;
	std::wstring title;
	std::wstring placeholder;
	std::wstring value;
	bool canPickMany = false;
	bool password = false;
	std::vector<SQuickPickItem> items;
};

struct SQuickInputCompletion {
	EQuickInputState state = EQuickInputState::Cancelled;
	std::vector<std::size_t> selectedIndices;
	std::optional<std::wstring> value;
};
