/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/process/InheritedHandleList.h"

#include <algorithm>
#include <array>

namespace platform::process {

bool InheritedHandleList::Prepare() noexcept
{
	try {
		if (m_attributes != nullptr) {
			::DeleteProcThreadAttributeList(m_attributes);
			m_attributes = nullptr;
		}
		m_startup.lpAttributeList = nullptr;
		m_handles.clear();
		const std::array<HANDLE, 3> candidates{
			m_startup.StartupInfo.hStdInput,
			m_startup.StartupInfo.hStdOutput,
			m_startup.StartupInfo.hStdError,
		};
		for (const HANDLE handle : candidates) {
			if (handle == nullptr || handle == INVALID_HANDLE_VALUE) continue;
			DWORD flags{};
			if (!::GetHandleInformation(handle, &flags)
				|| (flags & HANDLE_FLAG_INHERIT) == 0) continue;
			if (std::find(m_handles.begin(), m_handles.end(), handle) == m_handles.end()) {
				m_handles.push_back(handle);
			}
		}
		if (m_handles.empty()) return true;

		SIZE_T attributeBytes{};
		(void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
		if (attributeBytes == 0) return false;
		m_attributeStorage.assign(attributeBytes, std::uint8_t{});
		auto* const attributes =
			reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attributeStorage.data());
		if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
			m_attributeStorage.clear();
			return false;
		}
		m_attributes = attributes;
		if (!::UpdateProcThreadAttribute(m_attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				m_handles.data(),
				static_cast<SIZE_T>(m_handles.size() * sizeof(HANDLE)), nullptr, nullptr)) {
			::DeleteProcThreadAttributeList(m_attributes);
			m_attributes = nullptr;
			return false;
		}
		m_startup.lpAttributeList = m_attributes;
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace platform::process
