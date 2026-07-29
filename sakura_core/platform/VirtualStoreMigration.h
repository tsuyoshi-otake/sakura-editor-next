/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <string>

namespace platform {

//! VirtualStore の旧 sakura.ini を移す処理の明示的な終端状態。
enum class VirtualStoreMigrationResult {
	NoLegacy,
	DestinationExists,
	AlreadyMigrated,
	Copied,
	Failed,
};

//! 移行で扱う三つのパス。いずれも null 終端の Windows path とする。
struct VirtualStoreMigrationRequest {
	std::wstring legacyIniPath;
	std::wstring destinationIniPath;
	std::wstring backupIniPath;
	std::wstring migrationRecordPath;
};

//! legacy executable のインストール先に対応する VirtualStore\sakura.ini を組み立てる。
//!
//! VirtualStore はローカル固定ドライブのドライブ文字を保持しない。入力がその形式で
//! ない場合は空文字列を返す。ファイルシステムへはアクセスしないため単体テスト可能。
std::wstring BuildLegacyVirtualStoreIniPath(
	const std::wstring& localAppDataPath,
	const std::wstring& legacyExecutablePath
);

//! パス解決を OS 実装から分離する。profile 管理層は destination と record の保存先を決める。
class IVirtualStoreMigrationPathProvider {
public:
	virtual ~IVirtualStoreMigrationPathProvider() = default;

	virtual std::wstring GetLocalAppDataPath() = 0;
	virtual std::wstring GetCurrentUserSakuraIniPath() = 0;
	virtual std::wstring GetMigrationRecordPath() = 0;
};

//! executable の旧インストール先と path provider から移行要求を組み立てる。
VirtualStoreMigrationRequest BuildVirtualStoreMigrationRequest(
	const std::wstring& legacyExecutablePath,
	IVirtualStoreMigrationPathProvider& pathProvider
);

//! 移行の I/O 境界。CopyFileWithoutOverwrite は既存 destination を絶対に上書きしない。
	//! source の保全コピーと destination の双方に使い、既存ファイルは上書きしない。
class IVirtualStoreMigrationFileSystem {
public:
	virtual ~IVirtualStoreMigrationFileSystem() = default;

	virtual bool Exists(const std::wstring& path) = 0;
	virtual bool CopyFileWithoutOverwrite(
		const std::wstring& sourcePath,
		const std::wstring& destinationPath
	) = 0;
	//! recordText は診断用の source path。実装は原子的に永続化する。
	virtual bool WriteMigrationRecord(
		const std::wstring& recordPath,
		const std::wstring& recordText
	) = 0;
};

//! 外部 manifest 適用前に一度だけ行う移行判断。
//!
//! 既存設定を上書きせず、source を削除しない。例外を呼出元へ送出せず、全て Failed に収束する。
VirtualStoreMigrationResult MigrateVirtualStoreIni(
	const VirtualStoreMigrationRequest& request,
	IVirtualStoreMigrationFileSystem& fileSystem
) noexcept;

//! 現在の executable/profile に対応する Windows の実パスで移行を実行する。
//!
//! sakura.exe.ini が MultiUser=1 の場合だけ、CControlProcess と同じ規則で
//! ユーザー設定領域を解決する。解決できない destination は Failed として
//! 明示し、元の VirtualStore ファイルは削除しない。
VirtualStoreMigrationResult MigrateVirtualStoreIniForCurrentUser(
	const std::wstring& executablePath,
	const std::wstring& profileName
) noexcept;

} // namespace platform
