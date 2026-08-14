/*! @file
	@brief Native modal quick-pick and input-box UI
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/quickinput/QuickInputTypes.h"

#include <Windows.h>

class CQuickInputDialog final {
public:
	explicit CQuickInputDialog(const SQuickInputRequest& request);
	[[nodiscard]] SQuickInputCompletion DoModal(HWND parent) noexcept;
	static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

private:
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
	bool Create(HWND parent) noexcept;
	void Layout(int width, int height) noexcept;
	void Accept() noexcept;
	void Cancel() noexcept;

	const SQuickInputRequest& m_request;
	SQuickInputCompletion m_completion;
	HWND m_parent = nullptr;
	HWND m_window = nullptr;
	HWND m_prompt = nullptr;
	HWND m_input = nullptr;
	HWND m_ok = nullptr;
	HWND m_cancel = nullptr;
};
