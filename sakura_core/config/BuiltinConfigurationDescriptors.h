/*! @file
 * @brief Built-in settings recognized by the first workbench runtime slice.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/ConfigurationTypes.h"

#include <vector>

namespace config {

//! Keep this catalog deliberately small. A key belongs here only when its
//! semantic owner exists; unknown file entries remain latent for the later
//! a future descriptor registry instead of being rejected.
[[nodiscard]] std::vector<ConfigurationDescriptor> BuiltinConfigurationDescriptors();

} // namespace config
