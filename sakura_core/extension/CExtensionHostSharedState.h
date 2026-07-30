/*! @file
	@brief 制御プロセスが公開する拡張ホスト接続情報
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionHostBroker.h"

#include <filesystem>
#include <optional>
#include <string>

/*!
	@brief プロファイル単位の小さな read-mostly 共有メモリ

	制御プロセスだけが書き、各エディタは seqlock snapshot を読む。既存の
	DLLSHAREDATA とは分離し、設定共有領域の ABI と保存形式へ影響させない。
*/
class CExtensionHostSharedState final {
public:
	CExtensionHostSharedState() = default;
	~CExtensionHostSharedState();
	CExtensionHostSharedState(const CExtensionHostSharedState&) = delete;
	CExtensionHostSharedState& operator=(const CExtensionHostSharedState&) = delete;

	bool CreateForBroker(const std::filesystem::path& profileDirectory, std::wstring& diagnostic);
	bool OpenForEditor(const std::filesystem::path& profileDirectory, std::wstring& diagnostic);
	void Close() noexcept;

	void Publish(const SExtensionHostBrokerSnapshot& snapshot) noexcept;
	[[nodiscard]] std::optional<SExtensionHostBrokerSnapshot> Read() const noexcept;
	[[nodiscard]] const std::wstring& MappingName() const noexcept { return m_mappingName; }

private:
	struct SharedBlock;

	HANDLE m_mapping = nullptr;
	SharedBlock* m_block = nullptr;
	bool m_writer = false;
	std::wstring m_mappingName;
};
