/*! @file
 * @brief Completes ownership of a failed editor successor process.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>

enum class EFailedEditorProcessShutdownResult : std::uint8_t {
	Stopped,
	OwnershipUnresolved,
};

//! Stops a successor whose bounded ready handshake failed and joins it.
//!
//! The caller retains ownership of processHandle and must close it after this
//! function returns. A returned success proves that the process object is
//! signalled, so managed workspace resources can then be deleted safely.
class CFailedEditorProcessShutdown final {
public:
	struct Operations final {
		DWORD (WINAPI* wait)(HANDLE, DWORD) = ::WaitForSingleObject;
		BOOL (WINAPI* terminate)(HANDLE, UINT) = ::TerminateProcess;
	};

	[[nodiscard]] static EFailedEditorProcessShutdownResult TerminateAndWait(
		HANDLE processHandle, DWORD exitCode, const Operations& operations = {}) noexcept
	{
		if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE
			|| operations.wait == nullptr || operations.terminate == nullptr) {
			return EFailedEditorProcessShutdownResult::OwnershipUnresolved;
		}
		if (operations.wait(processHandle, 0) == WAIT_OBJECT_0) {
			return EFailedEditorProcessShutdownResult::Stopped;
		}
		if (operations.terminate(processHandle, exitCode) == FALSE) {
			return operations.wait(processHandle, 0) == WAIT_OBJECT_0
				? EFailedEditorProcessShutdownResult::Stopped
				: EFailedEditorProcessShutdownResult::OwnershipUnresolved;
		}
		return operations.wait(processHandle, INFINITE) == WAIT_OBJECT_0
			? EFailedEditorProcessShutdownResult::Stopped
			: EFailedEditorProcessShutdownResult::OwnershipUnresolved;
	}
};

//! Process-lifetime owner for the exceptional case where Win32 cannot prove
//! that a failed successor stopped.  Keeping the handle prevents the caller
//! from losing its only observation capability.  Managed workspace files are
//! deliberately retained by the caller while a handle lives here.
class CFailedEditorProcessOwner final {
public:
	static void Retain(HANDLE processHandle) noexcept
	{
		if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE) return;
		for (;;) {
			::AcquireSRWLockExclusive(&s_lock);
			for (auto& retained : s_processes) {
				if (retained != nullptr && ::WaitForSingleObject(retained, 0) == WAIT_OBJECT_0) {
					::CloseHandle(retained);
					retained = nullptr;
				}
			}
			for (auto& retained : s_processes) {
				if (retained == nullptr) {
					retained = processHandle;
					::ReleaseSRWLockExclusive(&s_lock);
					return;
				}
			}
			::ReleaseSRWLockExclusive(&s_lock);

			// This path requires more simultaneous unresolved successor failures
			// than the editor supports windows.  Preserve ownership by joining the
			// incoming process instead of dropping its sole handle.
			if (::WaitForSingleObject(processHandle, INFINITE) == WAIT_OBJECT_0) {
				::CloseHandle(processHandle);
				return;
			}
			::Sleep(1);
		}
	}

private:
	static inline SRWLOCK s_lock = SRWLOCK_INIT;
	static inline std::array<HANDLE, 64> s_processes{};
};
