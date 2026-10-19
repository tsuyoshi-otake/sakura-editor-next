/*! @file
 * @brief UI-independent STARTUPINFOEXW + PROC_THREAD_ATTRIBUTE_HANDLE_LIST owner.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include <cstdint>
#include <span>
#include <vector>

namespace platform::process {

//! Owns the STARTUPINFOEXW block and the PROC_THREAD_ATTRIBUTE_HANDLE_LIST a
//! legacy CreateProcessW launcher needs so the child receives exactly the
//! handles placed in hStdInput/hStdOutput/hStdError -- never every inheritable
//! handle in the process. Callers fill StartupInfo(), call Prepare() to
//! collect the distinct non-null inheritable std handles into the attribute
//! list, then pass InheritHandles(), AdditionalCreationFlags() and
//! LaunchStartupInfo() to CreateProcessW. When no handle qualifies the list is
//! absent: InheritHandles() reports FALSE, the caller must not set
//! EXTENDED_STARTUPINFO_PRESENT, and no ambient handle can leak in.
class InheritedHandleList final {
public:
	InheritedHandleList() noexcept { m_startup.StartupInfo.cb = sizeof(STARTUPINFOW); }
	~InheritedHandleList() { Reset(); }
	InheritedHandleList(const InheritedHandleList&) = delete;
	InheritedHandleList& operator=(const InheritedHandleList&) = delete;
	InheritedHandleList(InheritedHandleList&&) = delete;
	InheritedHandleList& operator=(InheritedHandleList&&) = delete;

	//! The startup block the caller fills before Prepare(). cb starts at
	//! sizeof(STARTUPINFOW); LaunchStartupInfo() widens it to
	//! sizeof(STARTUPINFOEXW) once an attribute list exists.
	[[nodiscard]] STARTUPINFOW& StartupInfo() noexcept { return m_startup.StartupInfo; }

	//! Collects the distinct, non-null, inheritable handles currently stored
	//! in the startup block's hStdInput/hStdOutput/hStdError fields into a
	//! PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Handles that are null,
	//! INVALID_HANDLE_VALUE, or lack HANDLE_FLAG_INHERIT are skipped -- the
	//! API rejects them and the launch must not carry them; duplicates
	//! collapse to one entry. Returns false when list construction itself
	//! fails; callers must treat false as a launch failure. Success with an
	//! empty result leaves InheritHandles() == FALSE.
	[[nodiscard]] bool Prepare() noexcept;

	//! TRUE only while a non-empty attribute list exists.
	[[nodiscard]] BOOL InheritHandles() const noexcept
	{
		return m_attributes != nullptr ? TRUE : FALSE;
	}

	//! EXTENDED_STARTUPINFO_PRESENT when the list exists, 0 otherwise. The
	//! caller ORs this into its CreateProcessW creation flags.
	[[nodiscard]] DWORD AdditionalCreationFlags() const noexcept
	{
		return m_attributes != nullptr ? EXTENDED_STARTUPINFO_PRESENT : 0;
	}

	//! The startup block to hand to CreateProcessW.
	[[nodiscard]] LPSTARTUPINFOW LaunchStartupInfo() noexcept
	{
		m_startup.StartupInfo.cb = static_cast<DWORD>(
			m_attributes != nullptr ? sizeof(m_startup) : sizeof(STARTUPINFOW));
		return &m_startup.StartupInfo;
	}

	//! The collected list contents; a test seam for dedupe/exclusion checks.
	[[nodiscard]] std::span<const HANDLE> Handles() const noexcept { return m_handles; }

private:
	void Reset() noexcept
	{
		if (m_attributes != nullptr) {
			::DeleteProcThreadAttributeList(m_attributes);
			m_attributes = nullptr;
		}
	}

	STARTUPINFOEXW m_startup{};
	std::vector<HANDLE> m_handles;
	std::vector<std::uint8_t> m_attributeStorage;
	LPPROC_THREAD_ATTRIBUTE_LIST m_attributes{};
};

} // namespace platform::process
