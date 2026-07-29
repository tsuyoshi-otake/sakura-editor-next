/*! @file */
/*
	Copyright (C) 2018-2025, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "CSelectLang.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
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
