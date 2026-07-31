/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "debug/launch/LaunchConfigurationCatalog.h"

#include "platform/uri/UriIdentity.h"

#include <algorithm>
#include <set>
#include <utility>

namespace debug::launch {
namespace {

using JsoncValue = platform::serialization::JsoncValue;
using EArtifactKind = workbench::workspace::EWorkspaceArtifactDocumentKind;

LaunchConfigurationCatalogResult Failure(ELaunchConfigurationCatalogStatus status, const char* diagnostic)
{
	return { status, diagnostic };
}

bool IsValidIdentifier(std::wstring_view value) noexcept
{
	return !value.empty() && value.size() <= LaunchConfigurationCatalogLimits::kMaximumIdentifierLength
		&& std::none_of(value.begin(), value.end(), [](wchar_t character) {
			return character <= 0x1f || character == 0x7f;
		});
}

const std::wstring* RequiredIdentifier(const JsoncValue::Object& object, const wchar_t* name) noexcept
{
	const auto found = object.find(name);
	if (found == object.end()) return nullptr;
	const auto* value = std::get_if<std::wstring>(&found->second.Value());
	return value && IsValidIdentifier(*value) ? value : nullptr;
}

bool IsBoundedValue(const JsoncValue& value, std::size_t depth, std::size_t& nodes) noexcept;

bool IsBoundedObject(const JsoncValue::Object& object, std::size_t depth, std::size_t& nodes) noexcept
{
	if (depth > platform::serialization::CJsoncDocument::kMaximumDepth || ++nodes > platform::serialization::CJsoncDocument::kMaximumNodes) return false;
	for (const auto& [key, member] : object) {
		if (key.size() > platform::serialization::CJsoncDocument::kMaximumObjectKeyLength
			|| !IsBoundedValue(member, depth + 1U, nodes)) return false;
	}
	return true;
}

bool IsBoundedValue(const JsoncValue& value, std::size_t depth, std::size_t& nodes) noexcept
{
	if (depth > platform::serialization::CJsoncDocument::kMaximumDepth || ++nodes > platform::serialization::CJsoncDocument::kMaximumNodes) return false;
	if (const auto* string = std::get_if<std::wstring>(&value.Value())) {
		return string->size() <= platform::serialization::CJsoncDocument::kMaximumStringLength;
	}
	if (const auto* array = std::get_if<JsoncValue::Array>(&value.Value())) {
		for (const auto& member : *array) if (!IsBoundedValue(member, depth + 1U, nodes)) return false;
	} else if (const auto* object = std::get_if<JsoncValue::Object>(&value.Value())) {
		for (const auto& [key, member] : *object) {
			if (key.size() > platform::serialization::CJsoncDocument::kMaximumObjectKeyLength
				|| !IsBoundedValue(member, depth + 1U, nodes)) return false;
		}
	}
	return true;
}

bool IsBoundedObject(const JsoncValue::Object& object) noexcept
{
	std::size_t nodes = 0;
	return IsBoundedObject(object, 0U, nodes);
}

} // namespace

bool CLaunchConfigurationCatalog::IsValidResource(const platform::uri::Uri& resource) noexcept
{
	return !resource.Scheme().empty() && !resource.Path().empty()
		&& !resource.Query() && !resource.Fragment()
		&& !platform::uri::UriIdentityService::MakeComparisonKey(resource).empty();
}

LaunchConfigurationCatalogResult CLaunchConfigurationCatalog::BuildCatalog(
	const workbench::workspace::WorkspaceArtifactDocument& document, LaunchConfigurationCatalog& catalog)
{
	if (document.kind != EArtifactKind::Launch) return Failure(ELaunchConfigurationCatalogStatus::InvalidDocument, "workspace artifact is not a launch document");
	if (document.generation == 0 || document.revision == 0) return Failure(ELaunchConfigurationCatalogStatus::InvalidDocument, "launch document generation and revision are required");
	if (!IsValidResource(document.resource)) return Failure(ELaunchConfigurationCatalogStatus::InvalidResource, "launch document resource is invalid");
	if (!IsBoundedObject(document.artifact)) return Failure(ELaunchConfigurationCatalogStatus::InvalidSchema, "launch document exceeds the JSONC bounds");

	const auto configurations = document.artifact.find(L"configurations");
	if (configurations != document.artifact.end()) {
		const auto* entries = std::get_if<JsoncValue::Array>(&configurations->second.Value());
		if (!entries) return Failure(ELaunchConfigurationCatalogStatus::ConfigurationsMustBeArray, "launch configurations must be an array");
		if (entries->size() > LaunchConfigurationCatalogLimits::kMaximumConfigurations) return Failure(ELaunchConfigurationCatalogStatus::MaximumConfigurationsExceeded, "launch configuration limit exceeded");
		std::set<std::wstring, std::less<>> names;
		for (const auto& entry : *entries) {
			const auto* object = std::get_if<JsoncValue::Object>(&entry.Value());
			if (!object) return Failure(ELaunchConfigurationCatalogStatus::InvalidConfiguration, "launch configuration must be an object");
			const auto* name = RequiredIdentifier(*object, L"name");
			const auto* type = RequiredIdentifier(*object, L"type");
			const auto* request = RequiredIdentifier(*object, L"request");
			if (!name || !type || !request) return Failure(ELaunchConfigurationCatalogStatus::InvalidConfiguration, "launch configuration requires bounded name, type, and request strings");
			if (!names.emplace(*name).second) return Failure(ELaunchConfigurationCatalogStatus::DuplicateName, "duplicate launch configuration name");
			catalog.configurations.push_back({ *name, *type, *request, *object });
		}
	}

	const auto compounds = document.artifact.find(L"compounds");
	if (compounds != document.artifact.end()) {
		const auto* compoundEntries = std::get_if<JsoncValue::Array>(&compounds->second.Value());
		if (!compoundEntries) return Failure(ELaunchConfigurationCatalogStatus::CompoundsMustBeArray, "launch compounds must be an array");
		if (compoundEntries->size() > LaunchConfigurationCatalogLimits::kMaximumCompounds) return Failure(ELaunchConfigurationCatalogStatus::MaximumCompoundsExceeded, "launch compound limit exceeded");
		std::set<std::wstring, std::less<>> catalogNames;
		std::set<std::wstring, std::less<>> configurationNames;
		for (const auto& configuration : catalog.configurations) {
			catalogNames.emplace(configuration.name);
			configurationNames.emplace(configuration.name);
		}
		for (const auto& entry : *compoundEntries) {
			const auto* object = std::get_if<JsoncValue::Object>(&entry.Value());
			if (!object) return Failure(ELaunchConfigurationCatalogStatus::InvalidCompound, "launch compound must be an object");
			const auto* name = RequiredIdentifier(*object, L"name");
			const auto references = object->find(L"configurations");
			if (!name || references == object->end()) return Failure(ELaunchConfigurationCatalogStatus::InvalidCompound, "launch compound requires a bounded name and configurations array");
			const auto* referenceEntries = std::get_if<JsoncValue::Array>(&references->second.Value());
			if (!referenceEntries) return Failure(ELaunchConfigurationCatalogStatus::InvalidCompound, "launch compound configurations must be an array");
			if (referenceEntries->size() > LaunchConfigurationCatalogLimits::kMaximumCompoundReferences) return Failure(ELaunchConfigurationCatalogStatus::MaximumCompoundReferencesExceeded, "launch compound reference limit exceeded");
			if (!catalogNames.emplace(*name).second) return Failure(ELaunchConfigurationCatalogStatus::DuplicateName, "duplicate launch configuration or compound name");
			LaunchCompound compound { .name = *name, .raw = *object };
			std::set<std::wstring, std::less<>> referencesSeen;
			for (const auto& reference : *referenceEntries) {
				const auto* target = std::get_if<std::wstring>(&reference.Value());
				if (!target || !IsValidIdentifier(*target)) return Failure(ELaunchConfigurationCatalogStatus::InvalidCompound, "launch compound reference must be a bounded string");
				if (!referencesSeen.emplace(*target).second) return Failure(ELaunchConfigurationCatalogStatus::DuplicateCompoundReference, "duplicate launch compound reference");
				if (!configurationNames.contains(*target)) {
					return Failure(ELaunchConfigurationCatalogStatus::UnknownCompoundReference, "launch compound references an unknown configuration");
				}
				compound.configurations.push_back(*target);
			}
			catalog.compounds.push_back(std::move(compound));
		}
	}

	return { ELaunchConfigurationCatalogStatus::Applied, "launch catalog applied" };
}

LaunchConfigurationCatalogResult CLaunchConfigurationCatalog::Apply(
	const workbench::workspace::LaunchDocumentSnapshot& snapshot)
{
	if (!snapshot.document) return Failure(ELaunchConfigurationCatalogStatus::InvalidSnapshot, "launch document snapshot is empty; clear must be explicit");
	const auto& document = *snapshot.document;
	{
		std::lock_guard lock(m_mutex);
		if (m_state == ELaunchConfigurationCatalogState::Stopped) return Failure(ELaunchConfigurationCatalogStatus::Stopped, "launch catalog is stopped");
		if (document.generation < m_generation) return Failure(ELaunchConfigurationCatalogStatus::StaleGeneration, "launch document belongs to an older generation");
		if (document.generation == m_generation && document.revision <= m_revision) return Failure(ELaunchConfigurationCatalogStatus::StaleRevision, "launch document revision is not newer");
	}

	LaunchConfigurationCatalog candidate { document.resource, document.generation, document.revision };
	const auto result = BuildCatalog(document, candidate);
	if (!result.Succeeded()) return result;

	std::lock_guard lock(m_mutex);
	if (m_state == ELaunchConfigurationCatalogState::Stopped) return Failure(ELaunchConfigurationCatalogStatus::Stopped, "launch catalog is stopped");
	if (document.generation < m_generation) return Failure(ELaunchConfigurationCatalogStatus::StaleGeneration, "launch document belongs to an older generation");
	if (document.generation == m_generation && document.revision <= m_revision) return Failure(ELaunchConfigurationCatalogStatus::StaleRevision, "launch document revision is not newer");
	m_generation = document.generation;
	m_revision = document.revision;
	m_catalog = std::move(candidate);
	m_state = ELaunchConfigurationCatalogState::Ready;
	return result;
}

LaunchConfigurationCatalogResult CLaunchConfigurationCatalog::Clear(std::uint64_t generation, std::uint64_t revision) noexcept
{
	if (generation == 0 || revision == 0) return Failure(ELaunchConfigurationCatalogStatus::InvalidDocument, "launch clear generation and revision are required");
	std::lock_guard lock(m_mutex);
	if (m_state == ELaunchConfigurationCatalogState::Stopped) return Failure(ELaunchConfigurationCatalogStatus::Stopped, "launch catalog is stopped");
	if (generation < m_generation) return Failure(ELaunchConfigurationCatalogStatus::StaleGeneration, "launch clear belongs to an older generation");
	if (generation == m_generation && revision <= m_revision) return Failure(ELaunchConfigurationCatalogStatus::StaleRevision, "launch clear revision is not newer");
	m_generation = generation;
	m_revision = revision;
	m_catalog.reset();
	m_state = ELaunchConfigurationCatalogState::Empty;
	return { ELaunchConfigurationCatalogStatus::Cleared, "launch catalog cleared" };
}

LaunchConfigurationCatalogResult CLaunchConfigurationCatalog::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_state == ELaunchConfigurationCatalogState::Stopped) return Failure(ELaunchConfigurationCatalogStatus::Stopped, "launch catalog is stopped");
	m_catalog.reset();
	m_state = ELaunchConfigurationCatalogState::Stopped;
	return { ELaunchConfigurationCatalogStatus::Cleared, "launch catalog stopped" };
}

LaunchConfigurationCatalogSnapshot CLaunchConfigurationCatalog::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return { m_state, m_catalog };
}

std::optional<LaunchConfiguration> CLaunchConfigurationCatalog::FindConfiguration(std::wstring_view name) const
{
	std::lock_guard lock(m_mutex);
	if (!m_catalog) return std::nullopt;
	const auto found = std::find_if(m_catalog->configurations.begin(), m_catalog->configurations.end(), [name](const LaunchConfiguration& configuration) {
		return configuration.name == name;
	});
	return found == m_catalog->configurations.end() ? std::nullopt : std::optional<LaunchConfiguration>(*found);
}

std::optional<LaunchCompound> CLaunchConfigurationCatalog::FindCompound(std::wstring_view name) const
{
	std::lock_guard lock(m_mutex);
	if (!m_catalog) return std::nullopt;
	const auto found = std::find_if(m_catalog->compounds.begin(), m_catalog->compounds.end(), [name](const LaunchCompound& compound) {
		return compound.name == name;
	});
	return found == m_catalog->compounds.end() ? std::nullopt : std::optional<LaunchCompound>(*found);
}

} // namespace debug::launch
