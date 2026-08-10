/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/shareddata/SharedDataAbiContract.h>

#include <array>
#include <iostream>
#include <string_view>

namespace {

using legacy::shareddata::abi::FrozenSharedDataAbiLayout;

constexpr auto kObserved = FrozenSharedDataAbiLayout();

static_assert(kObserved.StructureVersion() == 0);
static_assert(kObserved.MappingSize() == sizeof(unsigned int));
static_assert(kObserved.Version() < kObserved.WorkBuffer());
static_assert(kObserved.WorkBuffer() < kObserved.Flags());
static_assert(kObserved.Flags() < kObserved.Nodes());
static_assert(kObserved.Nodes() < kObserved.Handles());
static_assert(kObserved.Handles() < kObserved.IniFile());
static_assert(kObserved.IniFile() < kObserved.PrivateIniFile());
static_assert(kObserved.PrivateIniFile() < kObserved.CharWidth());
static_assert(kObserved.CharWidth() < kObserved.CustomColors());
static_assert(kObserved.CustomColors() < kObserved.PluginCommandIcons());
static_assert(kObserved.PluginCommandIcons() < kObserved.MaximumToolbarNumber());
static_assert(kObserved.MaximumToolbarNumber() < kObserved.CommonSettings());
static_assert(kObserved.CommonSettings() < kObserved.TypeCount());
static_assert(kObserved.TypeCount() < kObserved.TypeBasis());
static_assert(kObserved.TypeBasis() < kObserved.TypeMini());
static_assert(kObserved.TypeMini() < kObserved.PrintSettings());
static_assert(kObserved.PrintSettings() < kObserved.LockCount());
static_assert(kObserved.LockCount() < kObserved.SearchKeywords());
static_assert(kObserved.SearchKeywords() < kObserved.TagJump());
static_assert(kObserved.TagJump() < kObserved.History());
static_assert(kObserved.History() < kObserved.ExecuteFlags());
static_assert(kObserved.ExecuteFlags() < kObserved.DiffFlags());
static_assert(kObserved.DiffFlags() < kObserved.TagsCommandLine());
static_assert(kObserved.TagsCommandLine() < kObserved.TagsOptions());
static_assert(kObserved.TagsOptions() < kObserved.LineNumberUsesCrLf());
static_assert(kObserved.LineNumberUsesCrLf() < kObserved.Size());

class NamedOffset final {
public:
	constexpr NamedOffset(std::string_view name, std::size_t value) noexcept : m_name(name), m_value(value) {}
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] constexpr std::size_t Value() const noexcept { return m_value; }
private:
	const std::string_view m_name;
	const std::size_t m_value;
};

constexpr std::array kOffsets{
	NamedOffset{"m_vStructureVersion", kObserved.StructureVersion()}, NamedOffset{"m_nSize", kObserved.MappingSize()},
	NamedOffset{"m_sVersion", kObserved.Version()}, NamedOffset{"m_sWorkBuffer", kObserved.WorkBuffer()},
	NamedOffset{"m_sFlags", kObserved.Flags()}, NamedOffset{"m_sNodes", kObserved.Nodes()},
	NamedOffset{"m_sHandles", kObserved.Handles()}, NamedOffset{"m_szIniFile", kObserved.IniFile()},
	NamedOffset{"m_szPrivateIniFile", kObserved.PrivateIniFile()}, NamedOffset{"m_sCharWidth", kObserved.CharWidth()},
	NamedOffset{"m_dwCustColors", kObserved.CustomColors()}, NamedOffset{"m_PlugCmdIcon", kObserved.PluginCommandIcons()},
	NamedOffset{"m_maxTBNum", kObserved.MaximumToolbarNumber()}, NamedOffset{"m_Common", kObserved.CommonSettings()},
	NamedOffset{"m_nTypesCount", kObserved.TypeCount()}, NamedOffset{"m_TypeBasis", kObserved.TypeBasis()},
	NamedOffset{"m_TypeMini", kObserved.TypeMini()}, NamedOffset{"m_PrintSettingArr", kObserved.PrintSettings()},
	NamedOffset{"m_nLockCount", kObserved.LockCount()}, NamedOffset{"m_sSearchKeywords", kObserved.SearchKeywords()},
	NamedOffset{"m_sTagJump", kObserved.TagJump()}, NamedOffset{"m_sHistory", kObserved.History()},
	NamedOffset{"m_nExecFlgOpt", kObserved.ExecuteFlags()}, NamedOffset{"m_nDiffFlgOpt", kObserved.DiffFlags()},
	NamedOffset{"m_szTagsCmdLine", kObserved.TagsCommandLine()}, NamedOffset{"m_nTagsOpt", kObserved.TagsOptions()},
	NamedOffset{"m_bLineNumIsCRLF_ForJump", kObserved.LineNumberUsesCrLf()},
};

void PrintObservation()
{
	std::cout << "pointer_bits=" << (sizeof(void*) * 8) << '\n';
	std::cout << "sizeof=" << kObserved.Size() << '\n';
	std::cout << "alignof=" << kObserved.Alignment() << '\n';
	for (const auto& offset : kOffsets) std::cout << offset.Name() << '=' << offset.Value() << '\n';
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::string_view(argv[1]) == "--print-observation") {
		PrintObservation();
		return 0;
	}
	std::cout << "[       OK ] DLLSHAREDATA.AbiObservation\n";
	return 0;
}
