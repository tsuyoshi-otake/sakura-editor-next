/*! @file
	@brief Compatibility include for the Control IPC security leaf.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcSecurity.h>
#include <sakura/security/CurrentUserSecurityAttributes.h>

namespace platform::controlipc {

//! Transitional alias for legacy Control IPC callers. New code includes the
//! owning security contract directly.
using CurrentUserSecurityAttributes = ::platform::security::CurrentUserSecurityAttributes;

} // namespace platform::controlipc
