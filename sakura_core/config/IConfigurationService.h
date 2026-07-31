/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/ConfigurationTypes.h"

#include <vector>

namespace config {

//! UI- and persistence-independent semantic configuration service contract.
class IConfigurationService {
public:
	virtual ~IConfigurationService() = default;

	virtual ConfigurationLookupResult GetValue(const std::string& key, const ConfigurationTarget& target) const = 0;
	virtual ConfigurationReadSnapshotResult ReadSnapshot(const std::vector<std::string>& keys, const ConfigurationTarget& target) const = 0;
	virtual ConfigurationInspection Inspect(const std::string& key, const ConfigurationTarget& target) const = 0;
	virtual ConfigurationResult Update(const ConfigurationUpdate& request) = 0;
	virtual ConfigurationResult ReplaceSource(const ConfigurationReplaceSource& request) = 0;
	virtual ConfigurationBatchResult ReplaceSources(const ConfigurationReplaceSources& request) = 0;
	virtual ConfigurationSubscription Subscribe(ConfigurationListener listener) = 0;
};

} // namespace config
