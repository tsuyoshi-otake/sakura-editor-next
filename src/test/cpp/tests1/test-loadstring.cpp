/*! @file */
/*
	Copyright (C) 2018-2025, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "CSelectLang.h"
#include "func/CFuncLookup.h"
#include "window/CEditWnd.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kLanguageDllNtOffset = 0x80;

void WriteLanguageDll16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::byte>(value & 0xff);
	bytes[offset + 1] = static_cast<std::byte>(value >> 8);
}

void WriteLanguageDll32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
	bytes[offset] = static_cast<std::byte>(value & 0xff);
	bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xff);
	bytes[offset + 2] = static_cast<std::byte>((value >> 16) & 0xff);
	bytes[offset + 3] = static_cast<std::byte>((value >> 24) & 0xff);
}

class TemporaryLanguageDllFile final {
public:
	explicit TemporaryLanguageDllFile(std::uint16_t machine)
	{
		std::vector<std::byte> bytes(kLanguageDllNtOffset + 24);
		WriteLanguageDll16(bytes, 0, 0x5a4d); // MZ
		WriteLanguageDll32(bytes, 0x3c, kLanguageDllNtOffset);
		WriteLanguageDll32(bytes, kLanguageDllNtOffset, 0x00004550); // PE\0\0
		WriteLanguageDll16(bytes, kLanguageDllNtOffset + 4, machine);

		wchar_t directory[MAX_PATH]{};
		const DWORD directoryLength = ::GetTempPathW(std::size(directory), directory);
		if (directoryLength == 0 || directoryLength >= std::size(directory)) {
			throw std::runtime_error("GetTempPathW failed");
		}
		wchar_t fileName[MAX_PATH]{};
		if (::GetTempFileNameW(directory, L"skl", 0, fileName) == 0) {
			throw std::runtime_error("GetTempFileNameW failed");
		}
		m_path = fileName;

		std::ofstream file(m_path, std::ios::binary | std::ios::trunc);
		if (!file) {
			throw std::runtime_error("Could not create temporary language DLL");
		}
		file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!file) {
			throw std::runtime_error("Could not write temporary language DLL");
		}
	}

	~TemporaryLanguageDllFile()
	{
		std::error_code error;
		std::filesystem::remove(m_path, error);
	}

	const std::filesystem::path& path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

std::filesystem::path GetCurrentProcessDirectory()
{
	std::array<wchar_t, 32768> path{};
	const auto length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size()) {
		throw std::runtime_error("GetModuleFileNameW failed");
	}
	return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

//! Restores both the selected resource module and thread UI language on all
//! GoogleTest exit paths, including ASSERT_*/exception unwinding.
class ScopedLanguageSelection final {
public:
	ScopedLanguageSelection()
		: m_threadLanguage(::GetThreadUILanguage())
		, m_hadLanguageEnvironment(!CSelectLang::gm_Langs.empty())
	{
		if (m_hadLanguageEnvironment && CSelectLang::gm_Selected < CSelectLang::gm_Langs.size()) {
			m_previousDll = CSelectLang::GetLangInfo(CSelectLang::gm_Selected).GetDllName();
		}
	}

	~ScopedLanguageSelection()
	{
		if (m_hadLanguageEnvironment) {
			CSelectLang::ChangeLang(m_previousDll);
		} else {
			CSelectLang::ChangeLang(L"");
			CSelectLang::gm_Langs.clear();
			CSelectLang::gm_Selected = 0;
		}
		::SetThreadUILanguage(m_threadLanguage);
	}

	ScopedLanguageSelection(const ScopedLanguageSelection&) = delete;
	ScopedLanguageSelection& operator=(const ScopedLanguageSelection&) = delete;

private:
	LANGID m_threadLanguage;
	bool m_hadLanguageEnvironment;
	std::filesystem::path m_previousDll;
};

struct FileMenuLocaleExpectation {
	const wchar_t* dllName;
	WORD languageId;
	const wchar_t* openFolder;
	const wchar_t* openWorkspace;
	const wchar_t* recentWorkspace;
	const wchar_t* closeFolder;
	const wchar_t* closeWorkspace;
};

struct WorkbenchScmLocaleExpectation {
	const wchar_t* dllName;
	std::array<const wchar_t*, 5> strings;
};

constexpr std::array<UINT, 5> kWorkbenchScmStringResourceIds = {
	STR_WORKBENCH_GIT_EMPTY_WORKBENCH,
	STR_WORKBENCH_SCM_REPOSITORIES_TITLE,
	STR_WORKBENCH_SCM_CHANGES_TITLE,
	STR_WORKBENCH_SCM_GRAPH_TITLE,
	STR_WORKBENCH_SCM_GRAPH_UNAVAILABLE,
};

//! This is the complete function-code surface of the File menu's v8 default,
//! including its ordinary submenu labels and special recent projections.
constexpr std::array<int, 41> kV8FileMenuFunctionCodes = {
	F_FILE_TOPMENU,
	F_FILENEW,
	F_FILENEW_NEWWINDOW,
	F_FILEOPEN,
	F_OPEN_WORKSPACE_FOLDER,
	F_OPEN_WORKSPACE,
	F_FILE_OPENRECENT_SUBMENU,
	F_RECENT_WORKSPACE_LIST,
	F_ADD_FOLDER_TO_WORKSPACE,
	F_SAVE_WORKSPACE_AS,
	F_DUPLICATE_WORKSPACE,
	F_FILESAVE,
	F_FILESAVEAS_DIALOG,
	F_FILESAVEALL,
	F_CLOSE_ACTIVE_EDITOR,
	F_CLOSE_WORKSPACE,
	F_WINCLOSE,
	F_EXITALL,
	F_FILESAVECLOSE,
	F_FILECLOSE,
	F_FILECLOSE_OPEN,
	F_FILE_REOPEN_SUBMENU,
	F_FILE_REOPEN,
	F_FILE_REOPEN_SJIS,
	F_FILE_REOPEN_JIS,
	F_FILE_REOPEN_EUC,
	F_FILE_REOPEN_LATIN1,
	F_FILE_REOPEN_UNICODE,
	F_FILE_REOPEN_UNICODEBE,
	F_FILE_REOPEN_UTF8,
	F_FILE_REOPEN_CESU8,
	F_FILE_REOPEN_UTF7,
	F_PRINT,
	F_PRINT_PREVIEW,
	F_PRINT_PAGESETUP,
	F_PROPERTY_FILE,
	F_BROWSE,
	F_FILE_RCNTFLDR_SUBMENU,
	F_FOLDER_USED_RECENTLY,
	F_GROUPCLOSE,
	F_EXITALLEDITORS,
};

std::wstring ResolveFileMenuFunctionName(const CFuncLookup& lookup, int functionCode)
{
	std::array<wchar_t, 256> buffer{};
	(void)lookup.Funccode2Name(functionCode, buffer.data(), buffer.size());
	return buffer.data();
}

} // namespace

TEST(CSelectLang, test001)
{
	// 強制的に初期化前状態にする
	CSelectLang::gm_Langs.clear();

	EXPECT_THAT(CSelectLang::getLangRsrcInstance(), NotNull());
	EXPECT_THAT(CSelectLang::getDefaultLangId(), 0x0411);
	EXPECT_THAT(CSelectLang::getDefaultLangString(), StrEq(L"Japanese"));
	EXPECT_THAT(CSelectLang::GetLangInfo(), IsEmpty());
}

TEST(CSelectLang, test002)
{
	// 初期化する
	CSelectLang::InitializeLanguageEnvironment();

	EXPECT_THAT(CSelectLang::GetLangInfo().size(), Ge(size_t(1)));

	CSelectLang::ChangeLang(L"sakura_lang_en_US.dll");

	EXPECT_THAT(CSelectLang::getLangRsrcInstance(), NotNull());
	EXPECT_THAT(CSelectLang::getDefaultLangId(), 0x0409);
	EXPECT_THAT(CSelectLang::getDefaultLangString(), StrEq(L"English (United States)"));
	EXPECT_THAT(CSelectLang::GetLangInfo(1).GetDllName(), StrEq(L"sakura_lang_en_US.dll"));
	EXPECT_THAT(CSelectLang::GetLangInfo(1).GetLangName(), StrEq(L"English (United States)"));

	CSelectLang::ChangeLang(L"");

	EXPECT_THAT(CSelectLang::getLangRsrcInstance(), NotNull());
	EXPECT_THAT(CSelectLang::getDefaultLangId(), 0x0411);
	EXPECT_THAT(CSelectLang::getDefaultLangString(), StrEq(L"Japanese"));
	EXPECT_THAT(CSelectLang::GetLangInfo(0).GetDllName(), StrEq(L""));
	EXPECT_THAT(CSelectLang::GetLangInfo(0).GetLangName(), StrEq(L"Japanese"));

	// 強制的に初期化前状態にする
	CSelectLang::gm_Langs.clear();
}

TEST(CSelectLang, RejectsI386ResourceDllBeforeLoad)
{
	TemporaryLanguageDllFile file(IMAGE_FILE_MACHINE_I386);
	CSelectLang::SSelLangInfo language(file.path());

	::SetLastError(ERROR_SUCCESS);
	EXPECT_FALSE(language.Load());
	EXPECT_EQ(ERROR_EXE_MACHINE_TYPE_MISMATCH, ::GetLastError());
}

TEST(CSelectLang, MissingResourceDllRetainsLoadFailure)
{
	CSelectLang::SSelLangInfo language(L"sakura_lang_not_present.dll");

	::SetLastError(ERROR_SUCCESS);
	EXPECT_FALSE(language.Load());
	EXPECT_NE(ERROR_EXE_MACHINE_TYPE_MISMATCH, ::GetLastError());
}

/*!
 * @brief リソース文字列の読込テスト
 *
 * tests1.exeに埋め込んだsakura_rc.rcの値を読み込めるかチェックするテスト。
 */
TEST(LoadStringW, LoadStringResource001)
{
	// リソースから言語識別子のIDを読み取る
	// ここのリソース文字列値は、言語選択のキーなので変更してはならない。
	EXPECT_THAT(LS(STR_SELLANG_LANGID), StrEq(L"0x0411"));

	// リソースから選択中言語のラベル文字列を読み取る
	// これは共通設定の選択中言語のとこに表示するラベル文字列。
	// 古いWindows APIが言語表示名を提供してなかったことに起因するリソース。
	EXPECT_THAT(LS(STR_SELLANG_NAME), StrEq(L"Japanese"));
}

TEST(LoadStringW, LoadStringResource002)
{
	// ロケールを設定
	::SetThreadUILanguage(MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN));

	EXPECT_THAT(cxx::load_string_as_acp(STR_SELLANG_LANGID), StrEq("0x0411"));
	EXPECT_THAT(cxx::load_string_as_acp(STR_SELLANG_NAME), StrEq("Japanese"));
	EXPECT_THAT(cxx::load_string_as_acp(F_FILENEW), StrEq("新規作成"));
}

TEST(LoadStringW, LoadStringResource100)
{
	// ID範囲を越える値を指定した場合は例外が発生する
	EXPECT_ANY_THROW(cxx::load_string(std::numeric_limits<WORD>::max() + 1));
}

TEST(LoadStringW, LoadStringResource101)
{
	// 対応する文字列リソースが存在しない機能IDを指定
	EXPECT_THAT(LS(F_EXPANDPARAMETER), StrEq(L""));
}

TEST(CSelectLang, MissingStringReturnsEmpty)
{
	EXPECT_TRUE(CSelectLang::LoadStringW(F_EXPANDPARAMETER).empty());
}

TEST(FileMenuLocalization, AllV8FileFunctionNamesResolveFromEachSelectedRuntimeResource)
{
	ScopedLanguageSelection restoreLanguage;
	const auto resourceDirectory = GetCurrentProcessDirectory();
	ASSERT_TRUE(std::filesystem::is_regular_file(resourceDirectory / L"sakura_lang_en_US.dll"))
		<< "build-sln must stage sakura_lang_en_US.dll beside tests1.exe";
	ASSERT_TRUE(std::filesystem::is_regular_file(resourceDirectory / L"sakura_lang_zh_CN.dll"))
		<< "build-sln must stage sakura_lang_zh_CN.dll beside tests1.exe";

	CSelectLang::InitializeLanguageEnvironment();
	const std::array locales = {
		FileMenuLocaleExpectation{ L"", MAKELANGID(LANG_JAPANESE, SUBLANG_JAPANESE_JAPAN),
			L"フォルダーを開く...", L"ファイルからワークスペースを開く...", L"最近使用したフォルダーまたはワークスペース", L"フォルダーを閉じる", L"ワークスペースを閉じる" },
		FileMenuLocaleExpectation{ L"sakura_lang_en_US.dll", MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
			L"Open Folder...", L"Open Workspace from File...", L"Recently Opened Folder or Workspace", L"Close Folder", L"Close Workspace" },
		FileMenuLocaleExpectation{ L"sakura_lang_zh_CN.dll", MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
			L"打开文件夹...", L"从文件打开工作区...", L"最近打开的文件夹或工作区", L"关闭文件夹", L"关闭工作区" },
	};
	CFuncLookup lookup;
	for (const auto& locale : locales) {
		SCOPED_TRACE(locale.dllName);
		CSelectLang::ChangeLang(locale.dllName);
		ASSERT_EQ(locale.languageId, CSelectLang::getDefaultLangId());

		for (const int functionCode : kV8FileMenuFunctionCodes) {
			SCOPED_TRACE(functionCode);
			std::wstring directResource;
			try {
				directResource = CSelectLang::LoadStringW(static_cast<UINT>(functionCode));
			}
			catch (const std::out_of_range&) {
				ADD_FAILURE() << "selected language resource is missing File-menu function code " << functionCode;
				continue;
			}
			EXPECT_FALSE(directResource.empty());
			const auto menuName = ResolveFileMenuFunctionName(lookup, functionCode);
			EXPECT_EQ(directResource, menuName);
			EXPECT_NE(L"-- undefined name --", menuName);
		}

		EXPECT_EQ(locale.openFolder, ResolveFileMenuFunctionName(lookup, F_OPEN_WORKSPACE_FOLDER));
		EXPECT_EQ(locale.openWorkspace, ResolveFileMenuFunctionName(lookup, F_OPEN_WORKSPACE));
		EXPECT_EQ(locale.recentWorkspace, ResolveFileMenuFunctionName(lookup, F_RECENT_WORKSPACE_LIST));
		EXPECT_EQ(locale.closeFolder, CSelectLang::LoadStringW(STR_CLOSE_FOLDER));
		EXPECT_EQ(locale.closeWorkspace, CSelectLang::LoadStringW(
			CEditWnd::CloseWorkspaceMenuLabelResource(config::EWorkspaceKind::Workspace)));
		EXPECT_EQ(STR_CLOSE_FOLDER, CEditWnd::CloseWorkspaceMenuLabelResource(config::EWorkspaceKind::Folder));
		EXPECT_EQ(0U, CEditWnd::CloseWorkspaceMenuLabelResource(config::EWorkspaceKind::Empty));
	}
}

TEST(WorkbenchScmLocalization, AllSelectedRuntimeResourcesContainLocalizedScmViewStrings)
{
	ScopedLanguageSelection restoreLanguage;
	const auto resourceDirectory = GetCurrentProcessDirectory();
	ASSERT_TRUE(std::filesystem::is_regular_file(resourceDirectory / L"sakura_lang_en_US.dll"));
	ASSERT_TRUE(std::filesystem::is_regular_file(resourceDirectory / L"sakura_lang_zh_CN.dll"));

	CSelectLang::InitializeLanguageEnvironment();
	const std::array locales = {
		WorkbenchScmLocaleExpectation{ L"", {
			L"ソース管理の対象となるフォルダーを開いていません。", L"リポジトリ", L"変更", L"グラフ", L"グラフ ビューはまだ利用できません。" } },
		WorkbenchScmLocaleExpectation{ L"sakura_lang_en_US.dll", {
			L"Open a folder to use Source Control.", L"Repositories", L"Changes", L"Graph", L"The Graph view is not available yet." } },
		WorkbenchScmLocaleExpectation{ L"sakura_lang_zh_CN.dll", {
			L"打开一个文件夹以使用源代码管理。", L"存储库", L"更改", L"图形", L"图形视图尚不可用。" } },
	};

	for (const auto& locale : locales) {
		SCOPED_TRACE(locale.dllName);
		CSelectLang::ChangeLang(locale.dllName);
		for (size_t index = 0; index < kWorkbenchScmStringResourceIds.size(); ++index) {
			const UINT resourceId = kWorkbenchScmStringResourceIds[index];
			SCOPED_TRACE(resourceId);
			try {
				const auto resource = cxx::load_string(
					resourceId, std::optional<HMODULE>{ CSelectLang::getLangRsrcInstance() });
				EXPECT_EQ(locale.strings[index], resource);
			}
			catch (const std::out_of_range&) {
				ADD_FAILURE() << "selected language resource is missing Workbench SCM string " << resourceId;
			}
		}
	}
}

TEST(FileMenuLocalization, LegacyMissingFunctionUsesTheSelectedLocaleUndefinedResource)
{
	ScopedLanguageSelection restoreLanguage;
	const auto resourceDirectory = GetCurrentProcessDirectory();
	ASSERT_TRUE(std::filesystem::is_regular_file(resourceDirectory / L"sakura_lang_en_US.dll"));
	ASSERT_TRUE(std::filesystem::is_regular_file(resourceDirectory / L"sakura_lang_zh_CN.dll"));
	CSelectLang::InitializeLanguageEnvironment();
	CFuncLookup lookup;
	for (const auto* dllName : { L"", L"sakura_lang_en_US.dll", L"sakura_lang_zh_CN.dll" }) {
		SCOPED_TRACE(dllName);
		CSelectLang::ChangeLang(dllName);
		const auto expectedUndefinedName = CSelectLang::LoadStringW(static_cast<UINT>(F_DISABLE));
		std::array<wchar_t, 256> name{};
		EXPECT_FALSE(lookup.Funccode2Name(F_EXPANDPARAMETER, name.data(), name.size()));
		EXPECT_EQ(expectedUndefinedName, name.data());
		EXPECT_NE(L"-- undefined name --", name.data());
	}
}
