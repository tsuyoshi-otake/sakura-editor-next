/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "env/DLLSHAREDATA.h"
#include <sakura/shareddata/SharedDataAbiContract.h>

#include <cstddef>

namespace legacy::shareddata::abi {

//! Complete observation of the DLL mapping's top-level physical layout.
//!
//! The mapping is consumed across processes, so every value here is an ABI
//! contract. Additions must be made only at the physical end after the
//! architecture fixtures are deliberately updated.
[[nodiscard]] constexpr SharedDataAbiLayout ObserveDllShareDataLayout() noexcept
{
	return {
		sizeof(DLLSHAREDATA), alignof(DLLSHAREDATA),
		offsetof(DLLSHAREDATA, m_vStructureVersion), offsetof(DLLSHAREDATA, m_nSize),
		offsetof(DLLSHAREDATA, m_sVersion), offsetof(DLLSHAREDATA, m_sWorkBuffer),
		offsetof(DLLSHAREDATA, m_sFlags), offsetof(DLLSHAREDATA, m_sNodes),
		offsetof(DLLSHAREDATA, m_sHandles), offsetof(DLLSHAREDATA, m_szIniFile),
		offsetof(DLLSHAREDATA, m_szPrivateIniFile), offsetof(DLLSHAREDATA, m_sCharWidth),
		offsetof(DLLSHAREDATA, m_dwCustColors), offsetof(DLLSHAREDATA, m_PlugCmdIcon),
		offsetof(DLLSHAREDATA, m_maxTBNum), offsetof(DLLSHAREDATA, m_Common),
		offsetof(DLLSHAREDATA, m_nTypesCount), offsetof(DLLSHAREDATA, m_TypeBasis),
		offsetof(DLLSHAREDATA, m_TypeMini), offsetof(DLLSHAREDATA, m_PrintSettingArr),
		offsetof(DLLSHAREDATA, m_nLockCount), offsetof(DLLSHAREDATA, m_sSearchKeywords),
		offsetof(DLLSHAREDATA, m_sTagJump), offsetof(DLLSHAREDATA, m_sHistory),
		offsetof(DLLSHAREDATA, m_nExecFlgOpt), offsetof(DLLSHAREDATA, m_nDiffFlgOpt),
		offsetof(DLLSHAREDATA, m_szTagsCmdLine), offsetof(DLLSHAREDATA, m_nTagsOpt),
		offsetof(DLLSHAREDATA, m_bLineNumIsCRLF_ForJump),
	};
}

[[nodiscard]] constexpr bool IsFrozenDllShareDataLayout() noexcept
{
	return MatchesFrozenSharedDataAbiLayout(ObserveDllShareDataLayout());
}

static_assert(IsFrozenDllShareDataLayout(), "DLLSHAREDATA physical ABI drifted from its x86/x64 fixture");

} // namespace legacy::shareddata::abi
