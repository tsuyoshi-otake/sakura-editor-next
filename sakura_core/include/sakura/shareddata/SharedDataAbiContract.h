/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace legacy::shareddata::abi {

//! Reviewable, architecture-specific physical contract for DLLSHAREDATA's top-level mapping layout.
class SharedDataAbiLayout final {
public:
	constexpr SharedDataAbiLayout(std::size_t size, std::size_t alignment, std::size_t structureVersion,
		std::size_t mappingSize, std::size_t version, std::size_t workBuffer, std::size_t flags,
		std::size_t nodes, std::size_t handles, std::size_t iniFile, std::size_t privateIniFile,
		std::size_t charWidth, std::size_t customColors, std::size_t pluginCommandIcons,
		std::size_t maximumToolbarNumber, std::size_t commonSettings, std::size_t typeCount,
		std::size_t typeBasis, std::size_t typeMini, std::size_t printSettings, std::size_t lockCount,
		std::size_t searchKeywords, std::size_t tagJump, std::size_t history, std::size_t executeFlags,
		std::size_t diffFlags, std::size_t tagsCommandLine, std::size_t tagsOptions,
		std::size_t lineNumberUsesCrLf) noexcept
		: m_size(size), m_alignment(alignment), m_structureVersion(structureVersion), m_mappingSize(mappingSize),
		m_version(version), m_workBuffer(workBuffer), m_flags(flags), m_nodes(nodes), m_handles(handles),
		m_iniFile(iniFile), m_privateIniFile(privateIniFile), m_charWidth(charWidth), m_customColors(customColors),
		m_pluginCommandIcons(pluginCommandIcons), m_maximumToolbarNumber(maximumToolbarNumber),
		m_commonSettings(commonSettings), m_typeCount(typeCount), m_typeBasis(typeBasis), m_typeMini(typeMini),
		m_printSettings(printSettings), m_lockCount(lockCount), m_searchKeywords(searchKeywords),
		m_tagJump(tagJump), m_history(history), m_executeFlags(executeFlags), m_diffFlags(diffFlags),
		m_tagsCommandLine(tagsCommandLine), m_tagsOptions(tagsOptions), m_lineNumberUsesCrLf(lineNumberUsesCrLf) {}

	[[nodiscard]] constexpr std::size_t Size() const noexcept { return m_size; }
	[[nodiscard]] constexpr std::size_t Alignment() const noexcept { return m_alignment; }
	[[nodiscard]] constexpr std::size_t StructureVersion() const noexcept { return m_structureVersion; }
	[[nodiscard]] constexpr std::size_t MappingSize() const noexcept { return m_mappingSize; }
	[[nodiscard]] constexpr std::size_t Version() const noexcept { return m_version; }
	[[nodiscard]] constexpr std::size_t WorkBuffer() const noexcept { return m_workBuffer; }
	[[nodiscard]] constexpr std::size_t Flags() const noexcept { return m_flags; }
	[[nodiscard]] constexpr std::size_t Nodes() const noexcept { return m_nodes; }
	[[nodiscard]] constexpr std::size_t Handles() const noexcept { return m_handles; }
	[[nodiscard]] constexpr std::size_t IniFile() const noexcept { return m_iniFile; }
	[[nodiscard]] constexpr std::size_t PrivateIniFile() const noexcept { return m_privateIniFile; }
	[[nodiscard]] constexpr std::size_t CharWidth() const noexcept { return m_charWidth; }
	[[nodiscard]] constexpr std::size_t CustomColors() const noexcept { return m_customColors; }
	[[nodiscard]] constexpr std::size_t PluginCommandIcons() const noexcept { return m_pluginCommandIcons; }
	[[nodiscard]] constexpr std::size_t MaximumToolbarNumber() const noexcept { return m_maximumToolbarNumber; }
	[[nodiscard]] constexpr std::size_t CommonSettings() const noexcept { return m_commonSettings; }
	[[nodiscard]] constexpr std::size_t TypeCount() const noexcept { return m_typeCount; }
	[[nodiscard]] constexpr std::size_t TypeBasis() const noexcept { return m_typeBasis; }
	[[nodiscard]] constexpr std::size_t TypeMini() const noexcept { return m_typeMini; }
	[[nodiscard]] constexpr std::size_t PrintSettings() const noexcept { return m_printSettings; }
	[[nodiscard]] constexpr std::size_t LockCount() const noexcept { return m_lockCount; }
	[[nodiscard]] constexpr std::size_t SearchKeywords() const noexcept { return m_searchKeywords; }
	[[nodiscard]] constexpr std::size_t TagJump() const noexcept { return m_tagJump; }
	[[nodiscard]] constexpr std::size_t History() const noexcept { return m_history; }
	[[nodiscard]] constexpr std::size_t ExecuteFlags() const noexcept { return m_executeFlags; }
	[[nodiscard]] constexpr std::size_t DiffFlags() const noexcept { return m_diffFlags; }
	[[nodiscard]] constexpr std::size_t TagsCommandLine() const noexcept { return m_tagsCommandLine; }
	[[nodiscard]] constexpr std::size_t TagsOptions() const noexcept { return m_tagsOptions; }
	[[nodiscard]] constexpr std::size_t LineNumberUsesCrLf() const noexcept { return m_lineNumberUsesCrLf; }

private:
	const std::size_t m_size, m_alignment, m_structureVersion, m_mappingSize, m_version, m_workBuffer, m_flags;
	const std::size_t m_nodes, m_handles, m_iniFile, m_privateIniFile, m_charWidth, m_customColors, m_pluginCommandIcons;
	const std::size_t m_maximumToolbarNumber, m_commonSettings, m_typeCount, m_typeBasis, m_typeMini, m_printSettings, m_lockCount;
	const std::size_t m_searchKeywords, m_tagJump, m_history, m_executeFlags, m_diffFlags, m_tagsCommandLine, m_tagsOptions, m_lineNumberUsesCrLf;
};

//! Frozen observations captured with the supported MSVC x86 and x64 targets.
//! Do not update these values to hide an accidental mapping-layout drift.
[[nodiscard]] constexpr SharedDataAbiLayout FrozenSharedDataAbiLayout() noexcept
{
#if INTPTR_MAX == INT64_MAX
	return { 3241904, 8, 0, 4, 8, 16, 148352, 148368, 422824, 422840, 423360,
		423880, 555084, 555148, 559148, 559152, 2659404, 2659408, 2739052,
		2755372, 2781324, 2781328, 2935432, 2952656, 3241364, 3241368,
		3241372, 3241892, 3241896 };
#elif INTPTR_MAX == INT32_MAX
	return { 3240448, 4, 0, 4, 8, 16, 148348, 148360, 421784, 421792, 422312,
		422832, 554036, 554100, 558100, 558104, 2658356, 2658360, 2738004,
		2754324, 2780276, 2780280, 2934384, 2951204, 3239912, 3239916,
		3239920, 3240440, 3240444 };
#else
#error DLLSHAREDATA ABI is only frozen for 32-bit and 64-bit Windows targets.
#endif
}

[[nodiscard]] constexpr bool MatchesFrozenSharedDataAbiLayout(const SharedDataAbiLayout& actual) noexcept
{
	const auto frozen = FrozenSharedDataAbiLayout();
	return actual.Size() == frozen.Size() && actual.Alignment() == frozen.Alignment()
		&& actual.StructureVersion() == frozen.StructureVersion() && actual.MappingSize() == frozen.MappingSize()
		&& actual.Version() == frozen.Version() && actual.WorkBuffer() == frozen.WorkBuffer()
		&& actual.Flags() == frozen.Flags() && actual.Nodes() == frozen.Nodes() && actual.Handles() == frozen.Handles()
		&& actual.IniFile() == frozen.IniFile() && actual.PrivateIniFile() == frozen.PrivateIniFile()
		&& actual.CharWidth() == frozen.CharWidth() && actual.CustomColors() == frozen.CustomColors()
		&& actual.PluginCommandIcons() == frozen.PluginCommandIcons()
		&& actual.MaximumToolbarNumber() == frozen.MaximumToolbarNumber()
		&& actual.CommonSettings() == frozen.CommonSettings() && actual.TypeCount() == frozen.TypeCount()
		&& actual.TypeBasis() == frozen.TypeBasis() && actual.TypeMini() == frozen.TypeMini()
		&& actual.PrintSettings() == frozen.PrintSettings() && actual.LockCount() == frozen.LockCount()
		&& actual.SearchKeywords() == frozen.SearchKeywords() && actual.TagJump() == frozen.TagJump()
		&& actual.History() == frozen.History() && actual.ExecuteFlags() == frozen.ExecuteFlags()
		&& actual.DiffFlags() == frozen.DiffFlags() && actual.TagsCommandLine() == frozen.TagsCommandLine()
		&& actual.TagsOptions() == frozen.TagsOptions() && actual.LineNumberUsesCrLf() == frozen.LineNumberUsesCrLf();
}

} // namespace legacy::shareddata::abi
