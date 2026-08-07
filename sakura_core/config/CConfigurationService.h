/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/IConfigurationService.h"

#include <memory>

namespace config {

//! In-memory P0 semantic core. Persistence adapters own parsing and call
//! ReplaceSource only after they have produced a complete valid model.
class CConfigurationService final : public IConfigurationService {
public:
	explicit CConfigurationService(std::vector<ConfigurationDescriptor> descriptors);
	~CConfigurationService() override;

	CConfigurationService(const CConfigurationService&) = delete;
	CConfigurationService& operator=(const CConfigurationService&) = delete;

	ConfigurationLookupResult GetValue(const std::string& key, const ConfigurationTarget& target) const override;
	ConfigurationReadSnapshotResult ReadSnapshot(const std::vector<std::string>& keys, const ConfigurationTarget& target) const override;
	ConfigurationInspection Inspect(const std::string& key, const ConfigurationTarget& target) const override;
	ConfigurationResult Update(const ConfigurationUpdate& request) override;
	ConfigurationResult ReplaceSource(const ConfigurationReplaceSource& request) override;
	ConfigurationBatchResult ReplaceSources(const ConfigurationReplaceSources& request) override;
	ConfigurationSubscription Subscribe(ConfigurationListener listener) override;

	// Not part of IConfigurationService: Workspace Trust is a workbench-runtime
	// concern, not a general configuration-consumer capability. Commits the
	// joint (workspaceTrusted, restrictedKeys) fact that CollectProvenanceLocked
	// consults when deciding whether a Workspace/Folder-scope contribution to a
	// restricted key is withheld.
	ConfigurationResult ApplyRestrictedConfigurations(const RestrictedConfigurationPolicy& policy);

	// Implementation state is declared here so the translation unit can keep
	// helpers independent of the public service API.
	struct State;

private:
	std::shared_ptr<State> m_state;
};

} // namespace config
