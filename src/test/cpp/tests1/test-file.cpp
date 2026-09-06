/*! @file */
/*
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <tchar.h>
#include <Windows.h>
#include <Shlwapi.h>

#include <cstdlib>
#include <fstream>
#include <future>

#include <algorithm>

#include "config/maxdata.h"
#include "basis/primitive.h"
#include "debug/Debug2.h"
#include "basis/CMyString.h"
#include "mem/CNativeW.h"
#include "env/DLLSHAREDATA.h"
#include "_main/CCommandLine.h"
#include "_main/CControlProcess.h"
#include "_main/CProcess.h"
#include "env/CDataProfile.h"
#include "util/file.h"

std::filesystem::path GetIniFileNameForIO(bool bWrite);

WCHAR* CopyDirDir( std::span<WCHAR> destination, const WCHAR* target, const WCHAR* base );

namespace cxx {

bool WritePrivateProfileStringW(
	std::wstring_view appName,
	std::wstring_view keyName,
	std::wstring_view varValue,
	const std::optional<std::filesystem::path>& iniPath = std::nullopt
) noexcept
{
	return ::WritePrivateProfileStringW(std::data(appName), std::data(keyName), std::data(varValue), iniPath.has_value() ? iniPath.value().c_str() : nullptr);
}

std::wstring ExpandEnvironmentStringsW(const std::wstring& src)
{
	std::wstring expected(2048, L'\0');
	if (const auto ret = ::ExpandEnvironmentStringsW(std::data(src), std::data(expected), DWORD(std::size(expected)))) {
		expected.resize(ret - 1);
	}
	return expected;
}

#if defined(_MSC_VER) && defined(_DEBUG)

/*!
 * @brief MSVCの無効なパラメーターハンドラーを無効化するためのクラス
 */
struct MsvcInvalidParameterHandlerDisabler
{
	using Holder = cxx::ResourceHolder<&::_set_invalid_parameter_handler>;
	using Me = MsvcInvalidParameterHandlerDisabler;

	static void __cdecl empty_handler(
		wchar_t const*,
		wchar_t const*,
		wchar_t const*,
		unsigned int,
		uintptr_t
	)
	{
		return;	// 何もしない
	}

	MsvcInvalidParameterHandlerDisabler() = default;

	Holder prev{ ::_set_invalid_parameter_handler(&empty_handler) };
};

/*!
 * @brief MSVCのアサーションダイアログを抑制するためのクラス
 */
struct MsvcReportMode
{
	using Me = MsvcReportMode;

	MsvcReportMode() = default;

	MsvcReportMode(const Me&) = delete;
	Me& operator=(const Me&) = delete;

	MsvcReportMode(Me&& other) noexcept = default;
	Me& operator=(Me&& rhs) noexcept = default;

	~MsvcReportMode()
	{
		::_CrtSetReportMode(_CRT_ASSERT, m_old);
	}

private:
	int m_old = ::_CrtSetReportMode(_CRT_ASSERT, 0);
};

#endif // defined(_MSC_VER) && defined(_DEBUG)

TEST(CopyDirDir, test001)
{
	WCHAR szTemp[_MAX_PATH];
	EXPECT_THAT(CopyDirDir(szTemp, L"Windows", LR"(C:)"), StrEq(LR"(C:\Windows\)"));
}

TEST(CopyDirDir, test002)
{
	WCHAR szTemp[_MAX_PATH];
	EXPECT_THAT(CopyDirDir(szTemp, LR"(C:\Windows)", LR"(A:)"), StrEq(LR"(C:\Windows\)"));
}

TEST(CopyDirDir, test101)
{
#if defined(_MSC_VER) && defined(_DEBUG)

	MsvcInvalidParameterHandlerDisabler disabler{};	// 無効なパラメーターハンドラーを無効化

	MsvcReportMode reportMode{}; // アサーションダイアログを抑制

	WCHAR szTemp5[5];	// バッファ不足を発生させる
	EXPECT_THAT(CopyDirDir(szTemp5, L"Windows", LR"(C:)"), StrEq(L""));	// 入りきらない場合、バッファは空になる

#endif // defined(_MSC_VER) && defined(_DEBUG)
}

TEST(CopyDirDir, test102)
{
#if defined(_MSC_VER) && defined(_DEBUG)

	MsvcInvalidParameterHandlerDisabler disabler{};	// 無効なパラメーターハンドラーを無効化

	MsvcReportMode reportMode{}; // アサーションダイアログを抑制

	WCHAR szTemp3[3];	// バッファ不足を発生させる
	EXPECT_THAT(CopyDirDir(szTemp3, L"Windows", LR"(C:)"), StrEq(L""));	// 入りきらない場合、バッファは空になる

#endif // defined(_MSC_VER) && defined(_DEBUG)
}

TEST(CopyDirDir, test103)
{
#if defined(_MSC_VER) && defined(_DEBUG)

	MsvcInvalidParameterHandlerDisabler disabler{};	// 無効なパラメーターハンドラーを無効化

	MsvcReportMode reportMode{}; // アサーションダイアログを抑制

	WCHAR szTemp11[11];	// バッファ不足を発生させる
	EXPECT_THAT(CopyDirDir(szTemp11, L"Windows", LR"(C:)"), StrEq(L""));	// 入りきらない場合、バッファは空になる

#endif // defined(_MSC_VER) && defined(_DEBUG)
}

} // namespace cxx

namespace path_util {

using cxx::WritePrivateProfileStringW;
using cxx::ExpandEnvironmentStringsW;

class CResolvedProfileProcessForTest final : public CProcess {
public:
	CResolvedProfileProcessForTest() : CProcess(nullptr, L"") {}

	bool AttachSharedDataForTest()
	{
		return InitializeProcess();
	}

private:
	bool MainLoop() override { return true; }
	void OnExitProcess() override {}
};

TEST(file, TryGetResolvedProfileDirectory_UsesFrozenSharedDataAnchor)
{
	CResolvedProfileProcessForTest process;
	EXPECT_FALSE(process.TryGetResolvedProfileDirectory().has_value());
	ASSERT_TRUE(process.AttachSharedDataForTest());

	auto* const shareData = process.GetShareData().GetDllShareDataPtr();
	ASSERT_NE(nullptr, shareData);
	const auto originalFrozenIniFile = shareData->m_szPrivateIniFile;

	const auto verifyFrozenDirectory = [&](const std::filesystem::path& frozenIniFile,
		const std::filesystem::path& expectedDirectory) {
		auto& buffer = shareData->m_szPrivateIniFile;
		std::fill(buffer.data(), buffer.data() + buffer.BUFFER_COUNT, L'\0');
		buffer = frozenIniFile;

		const auto resolvedDirectory = process.TryGetResolvedProfileDirectory();
		ASSERT_TRUE(resolvedDirectory.has_value());
		EXPECT_EQ(*resolvedDirectory, expectedDirectory.lexically_normal());
	};

	verifyFrozenDirectory(LR"(C:\Sakura\sakura.ini)", LR"(C:\Sakura)");
	verifyFrozenDirectory(LR"(C:\Sakura\profiles\named\sakura.ini)", LR"(C:\Sakura\profiles\named)");
	verifyFrozenDirectory(LR"(C:\Users\Test\AppData\Roaming\sakura\sakura.ini)",
		LR"(C:\Users\Test\AppData\Roaming\sakura)");
	shareData->m_szPrivateIniFile = originalFrozenIniFile;
}

/*!
 * @brief パスがファイル名に使えない文字を含んでいるかチェックする
 */
TEST( file, IsInvalidFilenameChars )
{
	// ファイル名に使えない文字 = "\\/:*?\"<>|"
	// このうち、\\と/はパス区切りのため実質対象外になる。
	EXPECT_FALSE(IsInvalidFilenameChars(L"test.txt"));
	EXPECT_FALSE(IsInvalidFilenameChars(L".\\test.txt"));
	EXPECT_FALSE(IsInvalidFilenameChars(L"./test.txt"));
	EXPECT_FALSE(IsInvalidFilenameChars(L"C:\\test.txt"));
	EXPECT_FALSE(IsInvalidFilenameChars(L"C:/test.txt"));
	EXPECT_FALSE(IsInvalidFilenameChars(L"C:\\"));
	EXPECT_FALSE(IsInvalidFilenameChars(L"C:/"));

	EXPECT_FALSE(IsInvalidFilenameChars(L"test:001.txt"));

	EXPECT_TRUE(IsInvalidFilenameChars(L"test*.txt"));
	EXPECT_TRUE(IsInvalidFilenameChars(L"test?.txt"));
	EXPECT_TRUE(IsInvalidFilenameChars(L"test\".txt"));
	EXPECT_TRUE(IsInvalidFilenameChars(L"test<.txt"));
	EXPECT_TRUE(IsInvalidFilenameChars(L"test>.txt"));
	EXPECT_TRUE(IsInvalidFilenameChars(L"test|.txt"));
}

TEST(file, IsValidPathAvailableChar)
{
	EXPECT_TRUE(IsValidPathAvailableChar(L"test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L".\\test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"./test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:\\test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:/test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:\\"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:/"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:\\dir\\test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:\\dir\\dir2\\test.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"C:dir\\dir2\\test.txt"));

	EXPECT_TRUE(IsValidPathAvailableChar(L"test:001.txt"));
	EXPECT_TRUE(IsValidPathAvailableChar(L"\\dir\\dir2\\test:001.txt"));

	// 特別考慮：DOSデバイスパスの?はtrue
	EXPECT_TRUE(IsValidPathAvailableChar(L"\\\\?\\C:\\test.txt"));

	EXPECT_FALSE(IsValidPathAvailableChar(L"test*.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"test?.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"test\".txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"test<.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"test>.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"test|.txt"));

	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir\\test*.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir\\test?.txt"));

	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir*\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir?\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir\"\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir<\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir>\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\dir|\\text.txt"));

	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\*dir\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\?dir\\text.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"C:\\di*r\\text.txt"));


	EXPECT_FALSE(IsValidPathAvailableChar(L"\\\\?\\C:\\test?.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"\\\\?\\C:\\test*.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"\\\\?\\C:\\d*ir\\test.txt"));
	EXPECT_FALSE(IsValidPathAvailableChar(L"\\\\?\\C:\\dir*\\test.txt"));
}

/*!
 * @brief exeファイルパスの取得
 */
TEST(file, GetExeFileName)
{
	// 標準的なコードでexeファイルのパスを取得
	std::wstring expected(_MAX_PATH, L'\0');
	::GetModuleFileNameW(nullptr, std::data(expected), DWORD(std::size(expected)));
	expected.resize(::wcsnlen(std::data(expected), std::size(expected)));

	// 関数戻り値が、標準的なコードで取得した結果と一致すること
	auto exePath = GetExeFileName();
	EXPECT_THAT(exePath, StrEq(expected));
}

/*!
 * @brief 既存コード互換用に残しておく関数のリグレッション
 */
TEST(file, Deprecated_GetExedir)
{
	// テストに使うファイル名(空でなければなんでもいい)
	constexpr const auto filename = L"README.txt";

	// 比較用関数呼び出し
	auto exeBasePath = GetExeFileName().parent_path().append(filename);

	// 戻り値取得用のバッファを指定しない場合、何も起きない
	GetExedir(nullptr);

	// 戻り値取得用のバッファ
	std::wstring buffer(_MAX_PATH, L'\0');

	// exeフォルダーの取得
	GetExedir(std::data(buffer));
	buffer.resize(::wcsnlen(std::data(buffer), std::size(buffer)));
	buffer += filename;
	EXPECT_THAT(buffer, StrEq(exeBasePath.c_str()));

	// バッファを再確保する
	buffer = std::wstring(_MAX_PATH, L'\0');

	// exe基準ファイルパスの取得
	GetExedir(std::data(buffer), filename);
	EXPECT_THAT(buffer, StartsWith(exeBasePath.c_str()));
}

/*!
 * @brief iniファイルパスの取得(プロセス未作成時)
 */
TEST(file, GetIniFileName_OutOfProcess)
{
	// exeファイルの拡張子をiniに変えたパスが返る
	auto iniPath = GetExeFileName().replace_extension(L".ini");
	EXPECT_THAT(GetIniFileName(), StrEq(iniPath.c_str()));
}

/*!
 * @brief iniファイルパスの取得(コントロールプロセス未初期化、かつ、プロファイル指定なし時)
 */
TEST(file, GetIniFileName_InProcessDefaultProfileUnInitialized)
{
	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="")");

	// サイドカー設定がない既定プロファイルは、exeではなくユーザー
	// データ領域に固定される。
	auto path = std::filesystem::path(ExpandEnvironmentStringsW(L"%APPDATA%\\sakura"));
	path.append(GetExeFileName().replace_extension(L".ini").filename().wstring());
	EXPECT_THAT(GetIniFileName(), StrEq(path.c_str()));
}

/*!
 * @brief iniファイルパスの取得(コントロールプロセス未初期化、かつ、プロファイル指定あり時)
 */
TEST(file, GetIniFileName_InProcessNamedProfileUnInitialized)
{
	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="profile1")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="profile1")");

	// 既定ユーザーデータ領域の下に、選択したプロファイル名が付く。
	auto path = std::filesystem::path(ExpandEnvironmentStringsW(L"%APPDATA%\\sakura"));
	path.append(L"profile1").append(GetExeFileName().replace_extension(L".ini").filename().wstring());
	EXPECT_THAT(GetIniFileName(), StrEq(path.c_str()));
}

/*!
 * マルチユーザー設定ファイルを使うテストのためのフィクスチャクラス
 *
 * 設定ファイルを使うテストは「設定ファイルがない状態」からの始動を想定しているので
 * 始動前に設定ファイルを削除するようにしている。
 * テスト実行後に設定ファイルを残しておく意味はないので終了後も削除している。
 */
class CExeIniTest : public ::testing::Test {
protected:
	/*!
	 * マルチユーザー構成設定ファイルのパス
	 */
	std::filesystem::path exeIniPath;

	/*
	 * profile1 配下のフォルダーのパス
	 */
	std::filesystem::path profileDirPath;

	/*!
	 * テストが起動される直前に毎回呼ばれる関数
	 */
	void SetUp() override {
		// マルチユーザー構成設定ファイルのパス
		exeIniPath = GetExeFileName().concat(L".ini");
		profileDirPath.clear();
	}

	/*!
	 * テストが実行された直後に毎回呼ばれる関数
	 */
	void TearDown() override {
		std::error_code ec;

		// 存在チェック
		if (std::filesystem::exists(exeIniPath)) {
			// マルチユーザー構成設定ファイルを削除する
			std::filesystem::remove(exeIniPath, ec);
		}

		// profile1 配下のフォルダーも削除する
		if (!profileDirPath.empty()) {
			std::filesystem::remove_all(profileDirPath, ec);
		}

		// 削除チェック
		EXPECT_FALSE(fexist(exeIniPath));
		if (!profileDirPath.empty()) {
			EXPECT_FALSE(std::filesystem::exists(profileDirPath));
		}
	}
};

TEST_F(CExeIniTest, GetIniFileName_ExistingSidecarWithMultiUserDisabledUsesPortablePath)
{
	WritePrivateProfileStringW(L"Settings", L"MultiUser", L"0", exeIniPath);

	ASSERT_TRUE(fexist(exeIniPath));

	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="")");

	// MultiUser=0 を明示した場合だけ exe 隣接プロファイルを使う。
	const auto expectedPath = GetExeFileName().replace_extension(L".ini");
	EXPECT_THAT(GetIniFileName(), StrEq(expectedPath.c_str()));
}

/*!
 * @brief iniファイルパスの取得
 */
TEST_F(CExeIniTest, GetIniFileName_PrivateRoamingAppData)
{
	// 設定を書き込む
	WritePrivateProfileStringW(L"Settings", L"MultiUser", L"1", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserRootFolder", L"0", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserSubFolder", L"", exeIniPath);

	// 実在チェック
	EXPECT_TRUE(fexist(exeIniPath));

	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="profile1")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="profile1")");

	const auto iniPath = GetIniFileName();
	profileDirPath = iniPath.parent_path();

	// 期待値を取得する
	auto expected = ExpandEnvironmentStringsW(LR"(%USERPROFILE%\AppData\Roaming\sakura\profile1\)");
	expected += iniPath.filename();

	// テスト実施
	EXPECT_THAT(GetIniFileName(), StrEq(expected));
}

/*!
 * @brief iniファイルパスの取得
 */
TEST_F(CExeIniTest, GetIniFileName_PrivateDesktop)
{
	// 設定を書き込む
	WritePrivateProfileStringW(L"Settings", L"MultiUser", L"1", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserRootFolder", L"3", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserSubFolder", L"sakura", exeIniPath);

	// 実在チェック
	EXPECT_TRUE(fexist(exeIniPath));

	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="")");

	// 期待値を取得する
	auto expected = ExpandEnvironmentStringsW(LR"(%USERPROFILE%\Desktop\sakura\)");
	expected += GetIniFileName().filename();

	// テスト実施
	EXPECT_THAT(GetIniFileName(), StrEq(expected));
}

/*!
 * @brief iniファイルパスの取得
 */
TEST_F(CExeIniTest, GetIniFileName_PrivateProfile)
{
	// 設定を書き込む
	WritePrivateProfileStringW(L"Settings", L"MultiUser", L"1", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserRootFolder", L"1", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserSubFolder", L"sakura", exeIniPath);

	// 実在チェック
	EXPECT_TRUE(fexist(exeIniPath));

	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="")");

	// 期待値を取得する
	auto expected = ExpandEnvironmentStringsW(LR"(%USERPROFILE%\sakura\)");
	expected += GetIniFileName().filename();

	// テスト実施
	EXPECT_THAT(GetIniFileName(), StrEq(expected));
}

/*!
 * @brief iniファイルパスの取得
 */
TEST_F(CExeIniTest, GetIniFileName_PrivateDocument)
{
	// 設定を書き込む
	WritePrivateProfileStringW(L"Settings", L"MultiUser", L"1", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserRootFolder", L"2", exeIniPath);
	WritePrivateProfileStringW(L"Settings", L"UserSubFolder", L"sakura", exeIniPath);

	// 実在チェック
	EXPECT_TRUE(fexist(exeIniPath));

	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="")");

	// 期待値を取得する
	auto expected = ExpandEnvironmentStringsW(LR"(%USERPROFILE%\Documents\sakura\)");
	expected += GetIniFileName().filename();

	// テスト実施
	EXPECT_THAT(GetIniFileName(), StrEq(expected));
}

/*!
 * @brief 既存コード互換用に残しておく関数のリグレッション
 */
TEST(file, Deprecated_GetInidir)
{
	// テストに使うファイル名(空でなければなんでもいい)
	constexpr const auto filename = L"README.txt";

	// 比較用関数呼び出し
	auto iniBasePath = GetIniFileName().parent_path().append(filename);

	// 戻り値取得用のバッファを指定しない場合、何も起きない
	GetInidir(nullptr);

	// 戻り値取得用のバッファ
	std::wstring buffer(_MAX_PATH, L'\0');

	// iniフォルダーの取得
	GetInidir(std::data(buffer));
	buffer.resize(::wcsnlen(std::data(buffer), std::size(buffer)));
	buffer += filename;
	EXPECT_THAT(buffer, StrEq(iniBasePath.c_str()));

	// バッファを再確保する
	buffer = std::wstring(_MAX_PATH, L'\0');

	// ini基準ファイルパスの取得
	GetInidir(std::data(buffer), filename);
	EXPECT_THAT(buffer, StartsWith(iniBasePath.c_str()));
}

/*!
 * @brief INIファイルまたはEXEファイルのあるディレクトリ，または指定されたファイル名のフルパスを返す（INIを優先）
 */
TEST(file, GetInidirOrExedir)
{
	// コマンドラインのインスタンスを用意する
	auto pCommandLine = std::make_unique<CCommandLine>();
	pCommandLine->ParseCommandLine(LR"(-PROF="profile1")", false);

	// プロセスのインスタンスを用意する
	CControlProcess dummy(nullptr, LR"(-PROF="profile1")");

	std::wstring buf(_MAX_PATH, L'\0');

	GetInidirOrExedir(buf.data(), L"", true);
	EXPECT_THAT(buf, StartsWith(GetExeFileName().replace_filename(L"").c_str()));

	constexpr auto filename = L"test.txt";
	auto exeBasePath = GetExeFileName().parent_path().append(filename);
	auto iniBasePath = GetIniFileName().parent_path().append(filename);

	// EXE基準のファイルを作る
	CProfile().WriteProfile(exeBasePath.c_str(), L"file, GetInidirOrExedirのテスト");

	// INI基準のファイルを作る
	CProfile().WriteProfile(iniBasePath.c_str(), L"file, GetInidirOrExedirのテスト");

	// 両方あるときはINI基準のパスが変える
	GetInidirOrExedir(buf.data(), filename, true);
	EXPECT_THAT(buf, StartsWith(iniBasePath.c_str()));

	std::error_code ec;

	// INI基準パスのファイルを削除する
	std::filesystem::remove(iniBasePath, ec);
	EXPECT_FALSE(fexist(iniBasePath));

	// EXE基準のみ存在するときはEXE基準のパスが変える
	GetInidirOrExedir(buf.data(), filename, true);
	EXPECT_THAT(buf, StartsWith(exeBasePath.c_str()));

	// EXE基準パスのファイルを削除する
	std::filesystem::remove(exeBasePath, ec);
	EXPECT_FALSE(fexist(exeBasePath));

	// 両方ないときはINI基準のパスが変える
	GetInidirOrExedir(buf.data(), filename, true);
	EXPECT_THAT(buf, StartsWith(iniBasePath.c_str()));

	std::filesystem::remove(iniBasePath, ec);
	std::filesystem::remove(exeBasePath, ec);
	std::filesystem::remove_all(iniBasePath.parent_path(), ec);
}

/*!
 * @brief 入出力に使うiniファイルの判定
 */
TEST(file, GetIniFileNameForIO)
{
	auto iniPath = GetExeFileName().replace_extension(L".ini");

	// 書き込みモードのとき
	EXPECT_THAT(GetIniFileNameForIO(true), StrEq(iniPath.c_str()));

	// 書き込みモードでないとき
	EXPECT_THAT(GetIniFileNameForIO(false), StrEq(iniPath.c_str()));

	// 書き込みモードでないがiniファイルが実在するとき
	CProfile().WriteProfile(iniPath.c_str(), L"file, GetIniFileNameForIOのテスト");
	EXPECT_TRUE(fexist(iniPath));

	// テスト実施
	EXPECT_THAT(GetIniFileNameForIO(false), StrEq(iniPath.c_str()));

	// INIファイルを削除する
	std::error_code ec;
	std::filesystem::remove(iniPath, ec);
	EXPECT_FALSE(fexist(iniPath));
}

/*!
 * @brief フルパスからファイル名を取り出す
 */
TEST(file, GetFileTitlePointer)
{
	// フルパスからファイル名を取得する
	EXPECT_STREQ(L"test.txt", GetFileTitlePointer(LR"(C:\Temp\test.txt)"));

	// フルパスにファイル名が含まれていない場合
	EXPECT_STREQ(L"", GetFileTitlePointer(LR"(C:\Temp\)"));

	// フルパスに\\が含まれていない場合
	EXPECT_STREQ(L"test.txt", GetFileTitlePointer(L"test.txt"));

	// 渡したパスが無効な場合は落ちます。
	EXPECT_DEATH({ GetFileTitlePointer(nullptr); }, ".*");
}

/*!
 * @brief ディレクトリの深さを計算する
 */
TEST(file, CalcDirectoryDepth)
{
	// ドライブ文字を含むフルパス
	EXPECT_EQ(1, CalcDirectoryDepth(LR"(C:\Temp\test.txt)"));

	// 共有フォルダーを含むフルパス
	EXPECT_EQ(1, CalcDirectoryDepth(LR"(\\host\Temp\test.txt)"));

	// ドライブなしのフルパス
	EXPECT_EQ(1, CalcDirectoryDepth(LR"(\Temp\test.txt)"));

	// 相対パス（？）
	EXPECT_EQ(1, CalcDirectoryDepth(LR"(C:\Temp\.\test.txt)"));

	// 渡したパスが無効な場合は落ちます。
	EXPECT_DEATH({ CalcDirectoryDepth(nullptr); }, ".*");
}

/*!
	FileMatchScoreSepExtのテスト
 */
TEST(file, FileMatchScoreSepExt)
{
	int result = 0;

	// FileNameSepExtのテストパターン
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\test.txt)",
		LR"(C:\TEMP\TEST.TXT)");
	EXPECT_THAT(result, std::size(LR"(test.txt)") - 1);

	// FileNameSepExtのテストパターン（パスにフォルダーが含まれない）
	result = FileMatchScoreSepExt(
		LR"(TEST.TXT)",
		LR"(test.txt)");
	EXPECT_THAT(result, std::size(LR"(test.txt)") - 1);

	// FileNameSepExtのテストパターン（ファイル名がない）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\.txt)",
		LR"(C:\TEMP\.txt)");
	EXPECT_THAT(result, std::size(LR"(.txt)") - 1);

	// FileNameSepExtのテストパターン（拡張子がない）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\test)",
		LR"(C:\TEMP\test)");
	EXPECT_THAT(result, std::size(LR"(test)") - 1);

	// 全く同じパス同士の比較（ファイル名＋拡張子が完全一致）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\test.txt)",
		LR"(C:\TEMP\TEST.TXT)");
	EXPECT_THAT(result, std::size(LR"(test.txt)") - 1);

	// 異なるパスでファイル名＋拡張子が同じ（ファイル名＋拡張子が完全一致）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP1\TEST.TXT)",
		LR"(C:\TEMP2\test.txt)");
	EXPECT_THAT(result, std::size(LR"(test.txt)") - 1);

	// ファイル名が異なる1（最長一致を取得）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\test.txt)",
		LR"(C:\TEMP\TEST1.TST)");
	EXPECT_THAT(result, std::size(LR"(test)") - 1 + std::size(LR"(.t)") - 1);

	// ファイル名が異なる2（最長一致を取得）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\test1.tst)",
		LR"(C:\TEMP\TEST.TXT)");
	EXPECT_THAT(result, std::size(LR"(test)") - 1 + std::size(LR"(.t)") - 1);

	// 拡張子が異なる1（最長一致を取得）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\test.txt)",
		LR"(C:\TEMP\TEXT.TXTX)");
	EXPECT_THAT(result, std::size(LR"(te)") - 1 + std::size(LR"(.txt)") - 1);

	// 拡張子が異なる2（最長一致を取得）
	result = FileMatchScoreSepExt(
		LR"(C:\TEMP\text.txtx)",
		LR"(C:\TEMP\TEST.TXT)");
	EXPECT_THAT(result, std::size(LR"(te)") - 1 + std::size(LR"(.txt)") - 1);

	// サロゲート文字を含む1
	result = FileMatchScoreSepExt(
		L"C:\\TEMP\\test\xD83D\xDC49\xD83D\xDC46.TST",
		L"C:\\TEMP\\TEST\xD83D\xDC49\xD83D\xDC47.txt");
	EXPECT_THAT(result, std::size(LR"(testXX)") - 1 + std::size(LR"(.t)") - 1);

	// サロゲート文字を含む2
	result = FileMatchScoreSepExt(
		L"C:\\TEMP\\TEST\xD83D\xDC49\xD83D\xDC47.txt",
		L"C:\\TEMP\\test\xD83D\xDC49\xD83D\xDC46.TST");
	EXPECT_THAT(result, std::size(LR"(testXX)") - 1 + std::size(LR"(.t)") - 1);
}

/*!
	GetExtのテスト
 */
TEST(CFilePath, GetExt)
{
	CFilePath path;

	// 最も単純なパターン
	path = L"test.txt";
	EXPECT_THAT(path.GetExt(true), StrEq(L"txt"));

	// ファイルに拡張子がないパターン
	path = L"lib\\.NET Core\\README";
	EXPECT_THAT(path.GetExt(true), StrEq(L""));

	// 拡張子がない場合に返却されるポインタ値の確認
	EXPECT_EQ(path.GetExt(), path.c_str() + path.Length());

	// ファイルに拡張子がないパターン
	path = L"lib/.NET Core/README";
	EXPECT_THAT(path.GetExt(true), StrEq(L""));

	// 拡張子がない場合に返却されるポインタ値の確認
	EXPECT_EQ(path.GetExt(), path.c_str() + path.Length());
}

/*!
	CFileNameManager::GetFilePathFormatのテスト
 */
TEST(CFileNameManager, GetFilePathFormat)
{
	// バッファ
	std::wstring strBuf;

	// 十分な大きさのバッファを指定
	strBuf = std::wstring(50, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(LR"(C:\%Temp%\test.txt)", strBuf.data(), strBuf.size() + 1, L"%Temp%", L"テンポラリ"), StrEq(LR"(C:\テンポラリ\test.txt)"));

	// バッファ不足（パターンに一致した部分が切り捨てられる）
	strBuf = std::wstring(6, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(LR"(C:\%Temp%\test.txt)", strBuf.data(), strBuf.size() + 1, L"%Temp%", L"テンポラリ"), StrEq(LR"(C:\テンポ)"));

	// バッファ不足（パターンに一致しない部分が切り捨てられる）
	strBuf = std::wstring(15, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(LR"(C:\%Temp%\test.txt)", strBuf.data(), strBuf.size() + 1, L"%Temp%", L"テンポラリ"), StrEq(LR"(C:\テンポラリ\test.t)"));

	// ソースが部分文字列（十分な大きさのバッファを指定）
	strBuf = std::wstring(50, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(std::wstring_view(LR"(C:\%Temp%\test.txt.bak)", 18), strBuf.data(), strBuf.size() + 1, L"%Temp%", L"テンポラリ"), StrEq(LR"(C:\テンポラリ\test.txt)"));

	// ソースが部分文字列（十分な大きさのバッファを指定）
	strBuf = std::wstring(50, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(std::wstring_view(LR"(C:\test.txt\%Temp%.bak)", 18), strBuf.data(), strBuf.size() + 1, L"%Temp%", L"テンポラリ"), StrEq(LR"(C:\test.txt\テンポラリ)"));

	// ソースが部分文字列（置換文字が1文字アウト、十分な大きさのバッファを指定）
	strBuf = std::wstring(50, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(std::wstring_view(LR"(C:\test.txt\%Temp%.bak)", 17), strBuf.data(), strBuf.size() + 1, L"%Temp%", L"テンポラリ"), StrEq(LR"(C:\test.txt\%Temp)"));

	// 置換対象が部分文字列（十分な大きさのバッファを指定）
	strBuf = std::wstring(50, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(LR"(C:\%Temp%\test.txt)", strBuf.data(), strBuf.size() + 1, std::wstring_view(LR"(%Temp%\)", 6), L"テンポラリ"), StrEq(LR"(C:\テンポラリ\test.txt)"));

	// 置換先が部分文字列（十分な大きさのバッファを指定）
	strBuf = std::wstring(50, L'x');
	EXPECT_THAT(CFileNameManager::GetFilePathFormat(LR"(C:\%Temp%\test.txt)", strBuf.data(), strBuf.size() + 1, L"%Temp%", std::wstring_view(L"テンポラリってる", 5)), StrEq(LR"(C:\テンポラリ\test.txt)"));
}

TEST(CFilePath, GetDirPath001)
{
	CFilePath path(LR"(C:\Temp\test.txt)");
	EXPECT_THAT(path.GetDirPath(), StrEq(LR"(C:\Temp\)"));
}

TEST(CFilePath, GetDirPath002)
{
	CFilePath path(LR"(C:\Temp\)");	//ファイル名がない
	EXPECT_THAT(path.GetDirPath(), StrEq(LR"(C:\Temp\)"));
}

TEST(CFilePath, GetDirPath003)
{
	CFilePath path(LR"(C:\Temp)");	//末尾 \ がない
	EXPECT_THAT(path.GetDirPath(), StrEq(LR"(C:\)"));
}

TEST(CFilePath, GetDirPath101)
{
	CFilePath path(L"");	//パスが空
	EXPECT_THAT(path.GetDirPath(), StrEq(L""));
}

TEST(CFilePath, GetDirPath102)
{
	CFilePath path(L"test.txt");	//ディレクトリがない
	EXPECT_THAT(path.GetDirPath(), StrEq(L""));
}

} // namespace path_util

// FileOpen owns conversion options; callers may release or change their settings.
#include "io/CFileLoad.h"
#include "env/ShareDataTestSuite.hpp"

class FileLoadOptionsTest : public ::testing::Test, public env::ShareDataTestSuite {
protected:
    static void SetUpTestSuite() { SetUpShareData(); }
    static void TearDownTestSuite() { TearDownShareData(); }
    void SetUp() override {
        wchar_t directory[MAX_PATH]{};
        wchar_t name[MAX_PATH]{};
        ASSERT_NE(0u, ::GetTempPathW(MAX_PATH, directory));
        ASSERT_NE(0u, ::GetTempFileNameW(directory, L"sfl", 0, name));
        path = name;
    }
    void TearDown() override {
        pcShareData->GetDllShareDataPtr()->m_Common.m_sEdit.m_bEnableExtEol = originalExtendedEol;
        EXPECT_TRUE(::DeleteFileW(path.c_str()));
    }
    void Write(std::string_view bytes) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        ASSERT_FALSE(stream.fail());
    }
    const bool originalExtendedEol = pcShareData->GetDllShareDataPtr()->m_Common.m_sEdit.m_bEnableExtEol;
    std::filesystem::path path;
};

TEST_F(FileLoadOptionsTest, MimeOptionIsAppliedOnEachOpenAndReopen)
{
    Write("=?ISO-2022-JP?B?YWJj?=\r\n");
    CFileLoad loader;
    for (int option : {1, 0, 1, 0}) {
        ASSERT_EQ(CODE_JIS, loader.FileOpen(path.c_str(), false, CODE_JIS, option));
        CNativeW line;
        CEol eol;
        EXPECT_EQ(RESULT_COMPLETE, loader.ReadLine(&line, &eol));
        EXPECT_EQ(option ? L"abc\r\n" : L"=?ISO-2022-JP?B?YWJj?=\r\n",
            std::wstring(line.GetStringPtr(), line.GetStringLength()));
        EXPECT_EQ(EEolType::cr_and_lf, eol.GetType());
        loader.FileClose();
    }
}

TEST_F(FileLoadOptionsTest, AutoDetectionUsesConstructionSnapshot)
{
    Write("");
    SEncodingConfig options{};
    options.m_eDefaultCodetype = CODE_UTF8;
    CFileLoad loader(options);
    options.m_eDefaultCodetype = CODE_SJIS;
    EXPECT_EQ(CODE_UTF8, loader.FileOpen(path.c_str(), false, CODE_AUTODETECT, 0));
    loader.FileClose();
}
TEST_F(FileLoadOptionsTest, PreparedReaderPreservesExtendedEolBoundaries)
{
    pcShareData->GetDllShareDataPtr()->m_Common.m_sEdit.m_bEnableExtEol = true;
    Write("first\xc2\x85second\xe2\x80\xa8third\xe2\x80\xa9last\r\n");
    CFileLoad parent;
    ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    CFileLoad reader;
    reader.Prepare(parent, 0, static_cast<std::size_t>(parent.GetFileSize()));
    for (const auto expected : {L"first\u0085", L"second\u2028", L"third\u2029", L"last\r\n"}) {
        CNativeW parentLine, readerLine;
        CEol parentEol, readerEol;
        ASSERT_EQ(RESULT_COMPLETE, parent.ReadLine(&parentLine, &parentEol));
        EXPECT_EQ(expected, std::wstring(parentLine.GetStringPtr(), parentLine.GetStringLength()));
        EXPECT_EQ(RESULT_COMPLETE, reader.ReadLine(&readerLine, &readerEol));
        EXPECT_EQ(expected, std::wstring(readerLine.GetStringPtr(), readerLine.GetStringLength()));
        EXPECT_EQ(parentEol.GetType(), readerEol.GetType());
    }
}

TEST_F(FileLoadOptionsTest, PreparedUtf7ReaderResetsPreviousDecodedLineOffset)
{
    pcShareData->GetDllShareDataPtr()->m_Common.m_sEdit.m_bEnableExtEol = true;
    Write("first+AIUAcwBlAGMAbwBuAGQ-\r\n");
    CFileLoad parent, reader;
    ASSERT_EQ(CODE_UTF7, parent.FileOpen(path.c_str(), false, CODE_UTF7, 0));
    ASSERT_EQ(CODE_UTF7, reader.FileOpen(path.c_str(), false, CODE_UTF7, 0));
    CNativeW line;
    CEol eol;
    ASSERT_EQ(RESULT_COMPLETE, reader.ReadLine(&line, &eol));
    ASSERT_EQ(L"first\u0085", std::wstring(line.GetStringPtr(), line.GetStringLength()));
    reader.FileClose();
    reader.Prepare(parent, 0, static_cast<std::size_t>(parent.GetFileSize()));
    EXPECT_EQ(RESULT_COMPLETE, reader.ReadLine(&line, &eol));
    EXPECT_EQ(L"first\u0085", std::wstring(line.GetStringPtr(), line.GetStringLength()));
    EXPECT_EQ(EEolType::next_line, eol.GetType());
}

TEST_F(FileLoadOptionsTest, ExtendedEolPolicyIsStableUntilReopen)
{
    struct Input { ECodeType code; std::string_view bytes; };
    const char utf16[] = "f\0i\0r\0s\0t\0\x85\0s\0e\0c\0o\0n\0d\0\r\0\n\0";
    for (const auto input : {
        Input{CODE_UTF8, "first\xc2\x85second\r\n"},
        Input{CODE_UTF7, "first+AIUAcwBlAGMAbwBuAGQ-\r\n"},
        Input{CODE_UNICODE, std::string_view(utf16, sizeof(utf16) - 1)}}) {
        for (bool enabled : {false, true}) {
            SCOPED_TRACE(static_cast<int>(input.code));
            SCOPED_TRACE(enabled);
            Write(input.bytes);
            auto& global = pcShareData->GetDllShareDataPtr()->m_Common.m_sEdit.m_bEnableExtEol;
            global = enabled;
            CFileLoad parent;
            ASSERT_EQ(input.code, parent.FileOpen(path.c_str(), false, input.code, 0));
            global = !enabled;
            CFileLoad reader;
            reader.Prepare(parent, 0, static_cast<std::size_t>(parent.GetFileSize()));
            const auto check = [](CFileLoad& loader, bool split) {
                CNativeW line;
                CEol eol;
                EXPECT_EQ(RESULT_COMPLETE, loader.ReadLine(&line, &eol));
                EXPECT_EQ(split ? L"first\u0085" : L"first\u0085second\r\n",
                    std::wstring(line.GetStringPtr(), line.GetStringLength()));
                EXPECT_EQ(split ? EEolType::next_line : EEolType::cr_and_lf, eol.GetType());
            };
            check(parent, enabled);
            check(reader, enabled);
            reader.FileClose();
            parent.FileClose();
            ASSERT_EQ(input.code, parent.FileOpen(path.c_str(), false, input.code, 0));
            check(parent, !enabled);
        }
    }
}
TEST_F(FileLoadOptionsTest, PreparedReaderRetainsMappingAfterParentDestruction)
{
    class ObservedReader : public CFileLoad {
    public:
        const void* View() const { return m_pReadBufTop; }
    };
    Write("first\r\nsecond\r\n");
    ObservedReader reader;
    const void* view = nullptr;
    {
        CFileLoad parent;
        ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
        reader.Prepare(parent, 0, static_cast<std::size_t>(parent.GetFileSize()));
        view = reader.View();
        ASSERT_NE(nullptr, view);
    }
    MEMORY_BASIC_INFORMATION region{};
    ASSERT_EQ(sizeof(region), ::VirtualQuery(view, &region, sizeof(region)));
    // Assert the lease before dereferencing a potentially unmapped address.
    ASSERT_EQ(static_cast<DWORD>(MEM_COMMIT), region.State);
    ASSERT_EQ(static_cast<DWORD>(MEM_MAPPED), region.Type);
    CNativeW line;
    CEol eol;
    EXPECT_EQ(RESULT_COMPLETE, reader.ReadLine(&line, &eol));
    EXPECT_EQ(L"first\r\n", std::wstring(line.GetStringPtr(), line.GetStringLength()));
    reader.FileClose();
    ASSERT_EQ(sizeof(region), ::VirtualQuery(view, &region, sizeof(region)));
    EXPECT_EQ(static_cast<DWORD>(MEM_FREE), region.State);
}
TEST_F(FileLoadOptionsTest, PrepareReplacesActiveMappingAndRejectsInvalidSlices)
{
    class ObservedReader : public CFileLoad {
    public:
        const void* View() const { return m_pReadBufTop; }
    };
    Write("first\r\nsecond\r\n");
    CFileLoad parent;
    ObservedReader reader;
    ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    ASSERT_EQ(CODE_UTF8, reader.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    const void* previousView = reader.View();
    reader.Prepare(parent, 7, static_cast<std::size_t>(parent.GetFileSize()));
    MEMORY_BASIC_INFORMATION region{};
    ASSERT_EQ(sizeof(region), ::VirtualQuery(previousView, &region, sizeof(region)));
    EXPECT_EQ(static_cast<DWORD>(MEM_FREE), region.State);
    EXPECT_THROW(reader.Prepare(parent, 9, 7), CError_FileOpen);
    EXPECT_THROW(reader.Prepare(parent, 0, static_cast<std::size_t>(parent.GetFileSize()) + 1), CError_FileOpen);
    EXPECT_THROW(reader.Prepare(reader, 0, 0), CError_FileOpen);
    FILETIME stamp{};
    EXPECT_TRUE(reader.GetFileTime(nullptr, nullptr, &stamp));
    parent.FileClose();
    ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    CNativeW line;
    CEol eol;
    EXPECT_EQ(RESULT_COMPLETE, reader.ReadLine(&line, &eol));
    EXPECT_EQ(L"second\r\n", std::wstring(line.GetStringPtr(), line.GetStringLength()));
    parent.FileClose();
    EXPECT_THROW(reader.Prepare(parent, 0, 0), CError_FileOpen);
    reader.FileClose();
    EXPECT_FALSE(reader.GetFileTime(nullptr, nullptr, &stamp));
}

TEST_F(FileLoadOptionsTest, FailedOpenAndEmptyMappingCanBeReused)
{
    Write("");
    CFileLoad parent, reader;
    const auto missing = path.wstring() + L".missing";
    EXPECT_THROW(parent.FileOpen(missing.c_str(), false, CODE_UTF8, 0), CError_FileOpen);
    ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    reader.Prepare(parent, 0, 0);
    parent.FileClose();
    CNativeW line;
    CEol eol;
    EXPECT_EQ(RESULT_FAILURE, reader.ReadLine(&line, &eol));
    reader.FileClose();
    Write("reopened\r\n");
    ASSERT_EQ(CODE_UTF8, parent.FileOpen(path.c_str(), false, CODE_UTF8, 0));
    EXPECT_EQ(RESULT_COMPLETE, parent.ReadLine(&line, &eol));
    EXPECT_EQ(L"reopened\r\n", std::wstring(line.GetStringPtr(), line.GetStringLength()));
}
TEST_F(FileLoadOptionsTest, PreparedReadersOwnConvertersWithInheritedMimeOptions)
{
    class ObservedReader : public CFileLoad {
    public:
        const CCodeBase* Converter() const { return m_pCodeBase.get(); }
    };
    for (int mime : {0, 1}) {
        SCOPED_TRACE(mime);
        std::string bytes;
        for (int i = 0; i < 128; ++i) bytes += "=?ISO-2022-JP?B?YWJj?=\r\n";
        Write(bytes);
        ObservedReader parent, first, second;
        ASSERT_EQ(CODE_JIS, parent.FileOpen(path.c_str(), false, CODE_JIS, mime));
        first.Prepare(parent, 0, bytes.size());
        second.Prepare(parent, 0, bytes.size());
        ASSERT_NE(nullptr, first.Converter());
        ASSERT_NE(nullptr, second.Converter());
        ASSERT_NE(parent.Converter(), first.Converter());
        ASSERT_NE(parent.Converter(), second.Converter());
        ASSERT_NE(first.Converter(), second.Converter());
        parent.FileClose();
        const auto read = [mime](CFileLoad& reader) {
            for (int i = 0; i < 128; ++i) {
                CNativeW line;
                CEol eol;
                if (reader.ReadLine(&line, &eol) != RESULT_COMPLETE
                    || std::wstring(line.GetStringPtr(), line.GetStringLength()) !=
                        (mime ? L"abc\r\n" : L"=?ISO-2022-JP?B?YWJj?=\r\n")
                    || eol.GetType() != EEolType::cr_and_lf) return false;
            }
            return true;
        };
        // Exactly two workers; futures are collected before either reader dies.
        auto a = std::async(std::launch::async, read, std::ref(first));
        auto b = std::async(std::launch::async, read, std::ref(second));
        EXPECT_TRUE(a.get());
        EXPECT_TRUE(b.get());
    }
}