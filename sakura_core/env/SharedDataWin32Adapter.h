/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>

namespace legacy::shareddata::win32 {

//! Brings the required tray endpoint to the foreground without exposing its native handle.
void ActivateRequiredTrayWindow();

//! Sends the full settings-change notification to the required tray endpoint.
void NotifyRequiredTraySettingsChanged();

//! Tests macro ownership through the shared-data adapter. An unavailable required mapping is an explicit failure.
[[nodiscard]] bool IsMacroRecordingOwnedBy(std::uintptr_t window);

} // namespace legacy::shareddata::win32
