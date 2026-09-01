/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <windows.h>

#include "extmodule/CBregexpDll2.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

#if defined(_WIN64)
constexpr std::size_t kExpectedSize = 88;
constexpr std::size_t kExpectedAlign = 8;
constexpr std::size_t kOffOutp = 0;
constexpr std::size_t kOffOutendp = 8;
constexpr std::size_t kOffSplitctr = 16;
constexpr std::size_t kOffSplitp = 24;
constexpr std::size_t kOffRsv1 = 32;
constexpr std::size_t kOffParap = 40;
constexpr std::size_t kOffParaendp = 48;
constexpr std::size_t kOffTranstblp = 56;
constexpr std::size_t kOffStartp = 64;
constexpr std::size_t kOffEndp = 72;
constexpr std::size_t kOffNparens = 80;
#else
constexpr std::size_t kExpectedSize = 44;
constexpr std::size_t kExpectedAlign = 4;
constexpr std::size_t kOffOutp = 0;
constexpr std::size_t kOffOutendp = 4;
constexpr std::size_t kOffSplitctr = 8;
constexpr std::size_t kOffSplitp = 12;
constexpr std::size_t kOffRsv1 = 16;
constexpr std::size_t kOffParap = 20;
constexpr std::size_t kOffParaendp = 24;
constexpr std::size_t kOffTranstblp = 28;
constexpr std::size_t kOffStartp = 32;
constexpr std::size_t kOffEndp = 36;
constexpr std::size_t kOffNparens = 40;
#endif

constexpr DWORD kRequiredDllCharacteristics =
	IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT;

constexpr const char* kRequiredExports[] = {
	"BMatch",
	"BMatchEx",
	"BMatchExW",
	"BMatchW",
	"BRegexpVersion",
	"BRegexpVersionW",
	"BRegfree",
	"BRegfreeW",
	"BSplit",
	"BSplitW",
	"BSubst",
	"BSubstEx",
	"BSubstExW",
	"BSubstW",
	"BTrans",
	"BTransW",
	"BoMatch",
	"BoMatchW",
	"BoSubst",
	"BoSubstW",
};

bool ImportIsAllowed(const std::string& name)
{
	if (_stricmp(name.c_str(), "KERNEL32.dll") == 0) {
		return true;
	}
	// Debug builds of the owned CMake tree keep historical dbgtrace.h
	// helpers that call wsprintf*/OutputDebugString via USER32.
	if (_stricmp(name.c_str(), "USER32.dll") == 0) {
		return true;
	}
	if (_strnicmp(name.c_str(), "VCRUNTIME", 9) == 0) {
		return true;
	}
	if (_strnicmp(name.c_str(), "ucrtbase", 8) == 0) {
		return true;
	}
	if (_strnicmp(name.c_str(), "api-ms-win-crt-", 15) == 0) {
		return true;
	}
#if defined(__MINGW32__)
	// The MinGW-w64 CRT is supplied by Windows. GCC and libstdc++ remain
	// statically linked so the installed DLL never depends on MSYS2.
	if (_stricmp(name.c_str(), "msvcrt.dll") == 0) {
		return true;
	}
#endif
	return false;
}

const IMAGE_NT_HEADERS* NtHeaders(HMODULE module)
{
	auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return nullptr;
	}
	auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return nullptr;
	}
	return nt;
}

std::vector<std::string> PeNamesFromDirectory(HMODULE module, DWORD directoryIndex)
{
	std::vector<std::string> names;
	const auto* nt = NtHeaders(module);
	if (nt == nullptr) {
		return names;
	}
	const auto& dir = nt->OptionalHeader.DataDirectory[directoryIndex];
	if (dir.VirtualAddress == 0 || dir.Size == 0) {
		return names;
	}
	auto* base = reinterpret_cast<const std::uint8_t*>(module);
	if (directoryIndex == IMAGE_DIRECTORY_ENTRY_EXPORT) {
		const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
		const auto* nameRvas = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
		for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
			names.emplace_back(reinterpret_cast<const char*>(base + nameRvas[i]));
		}
	}
	else if (directoryIndex == IMAGE_DIRECTORY_ENTRY_IMPORT) {
		const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
		for (; desc->Name != 0; ++desc) {
			names.emplace_back(reinterpret_cast<const char*>(base + desc->Name));
		}
	}
	return names;
}

enum class Api {
	Match,
	MatchEx,
	Subst,
	SubstEx,
};

class CorpusCase {
public:
	CorpusCase(
		const char* id,
		Api api,
		const wchar_t* pattern,
		const wchar_t* target,
		int targetRepeat,
		const wchar_t* targetSuffix,
		int beg,
		int rc,
		const wchar_t* msg,
		std::optional<int> nparens,
		std::optional<int> index,
		std::optional<int> lastIndex,
		const wchar_t* out)
		: m_id(id)
		, m_api(api)
		, m_pattern(pattern)
		, m_target(target)
		, m_targetRepeat(targetRepeat)
		, m_targetSuffix(targetSuffix)
		, m_beg(beg)
		, m_rc(rc)
		, m_msg(msg)
		, m_nparens(nparens)
		, m_index(index)
		, m_lastIndex(lastIndex)
		, m_out(out)
	{
	}

	const char* Id() const { return m_id; }
	Api GetApi() const { return m_api; }
	const wchar_t* Pattern() const { return m_pattern; }
	int Beg() const { return m_beg; }
	int Rc() const { return m_rc; }
	const wchar_t* Msg() const { return m_msg; }
	const std::optional<int>& Nparens() const { return m_nparens; }
	const std::optional<int>& Index() const { return m_index; }
	const std::optional<int>& LastIndex() const { return m_lastIndex; }
	const wchar_t* Out() const { return m_out; }

	std::wstring MakeTarget() const
	{
		if (m_targetRepeat > 0) {
			std::wstring text(static_cast<std::size_t>(m_targetRepeat), L'a');
			text += m_targetSuffix;
			return text;
		}
		return m_target;
	}

private:
	const char* m_id;
	Api m_api;
	const wchar_t* m_pattern;
	const wchar_t* m_target;
	int m_targetRepeat;
	const wchar_t* m_targetSuffix;
	int m_beg;
	int m_rc;
	const wchar_t* m_msg;
	std::optional<int> m_nparens;
	std::optional<int> m_index;
	std::optional<int> m_lastIndex;
	const wchar_t* m_out;
};

const CorpusCase kCorpus[] = {
	{"match-digits", Api::Match, L"/([0-9]+)/", L"ab12cd", 0, nullptr, 0, 1, L"", 1, 2, 4, nullptr},
	{"match-miss", Api::Match, L"/xyz/", L"ab12cd", 0, nullptr, 0, 0, L"", 0, std::nullopt, std::nullopt, nullptr},
	{"match-empty-pattern", Api::Match, L"//", L"abc", 0, nullptr, 0, 1, L"", 0, 0, 0, nullptr},
	{"match-invalid", Api::Match, L"/[0-9/", L"abc", 0, nullptr, 0, -1, L"premature end of char-class", std::nullopt, std::nullopt, std::nullopt, nullptr},
	{"match-zero-width", Api::Match, L"/(?=a)/", L"xa", 0, nullptr, 0, 1, L"", 0, 1, 1, nullptr},
	{"match-crlf", Api::Match, L"/a$/m", L"a\r\nb", 0, nullptr, 0, 0, L"", 0, std::nullopt, std::nullopt, nullptr},
	{"match-cr", Api::Match, L"/a\\r/", L"a\rb", 0, nullptr, 0, 1, L"", 0, 0, 2, nullptr},
	{"match-lf", Api::Match, L"/a\\n/", L"a\nb", 0, nullptr, 0, 1, L"", 0, 0, 2, nullptr},
	{"match-global-digit", Api::Match, L"/\\d+/g", L"a1b22c", 0, nullptr, 0, 1, L"", 0, 1, 2, nullptr},
	{"match-case-fold", Api::Match, L"/AbC/i", L"xxabcYY", 0, nullptr, 0, 1, L"", 0, 2, 5, nullptr},
	{"match-unicode-hiragana", Api::Match, L"/[\\u3042-\\u3093]+/", L"xx\u3042\u3044yy", 0, nullptr, 0, 0, L"", 0, std::nullopt, std::nullopt, nullptr},
	{"match-surrogate-pair", Api::Match, L"/./", L"\U0001F600!", 0, nullptr, 0, 1, L"", 0, 0, 2, nullptr},
	{"match-ex-mid", Api::MatchEx, L"/\\d+/", L"ab12cd34", 0, nullptr, 4, 1, L"", 0, 6, 8, nullptr},
	{"subst-simple", Api::Subst, L"s/[0-9]+/{N}/", L"ab12cd", 0, nullptr, 0, 1, L"", 0, 2, 4, L"ab{N}cd"},
	{"subst-global", Api::Subst, L"s/\\d+/#/g", L"a1b22c3", 0, nullptr, 0, 3, L"", 0, 6, 7, L"a#b#c#"},
	{"subst-backref", Api::Subst, L"s/([0-9]+)/[$1]/", L"n99x", 0, nullptr, 0, 1, L"", 1, 1, 3, L"n[99]x"},
	{"subst-ex-mid", Api::SubstEx, L"s/\\d+/N/", L"ab12cd34", 0, nullptr, 4, 1, L"", 0, 6, 8, L"cdN"},
	{"subst-empty-result", Api::Subst, L"s/abc//", L"abc", 0, nullptr, 0, 1, L"", 0, 0, 3, nullptr},
	{"match-lookahead-capture", Api::Match, L"/(a)(?=b)/", L"zabq", 0, nullptr, 0, 1, L"", 1, 1, 2, nullptr},
	{"match-large-class", Api::Match, L"/[a-zA-Z0-9_]{3}/", L"._ok_!", 0, nullptr, 0, 1, L"", 0, 1, 4, nullptr},
	{"match-pathological-small", Api::Match, L"/(a+)+$/", L"aaaaaaaaaaX", 0, nullptr, 0, 0, L"", 1, std::nullopt, std::nullopt, nullptr},
	{"match-long-target", Api::Match, L"/xyz$/", L"a", 4000, L"xyz", 0, 1, L"", 0, 4000, 4003, nullptr},
	{"match-cp932-kana", Api::Match, L"/\u30d7\u30ed/", L"xx\u30d7\u30edyy", 0, nullptr, 0, 1, L"", 0, 2, 4, nullptr},
};

} // namespace

static_assert(std::is_same_v<BREGEXP_W, BREGEXP>, "Sakura must alias the provider struct");
static_assert(std::is_same_v<decltype(BREGEXP::rsv1), INT_PTR>, "rsv1 must be pointer-sized");
static_assert(std::is_same_v<std::remove_const_t<decltype(BREGEXP::splitctr)>, int>);
static_assert(std::is_same_v<decltype(BREGEXP::nparens), int>);
static_assert(std::is_pointer_v<decltype(BREGEXP::outp)>);
static_assert(std::is_const_v<std::remove_pointer_t<decltype(BREGEXP::outp)>>);

static_assert(sizeof(BREGEXP) == kExpectedSize);
static_assert(alignof(BREGEXP) == kExpectedAlign);
static_assert(offsetof(BREGEXP, outp) == kOffOutp);
static_assert(offsetof(BREGEXP, outendp) == kOffOutendp);
static_assert(offsetof(BREGEXP, splitctr) == kOffSplitctr);
static_assert(offsetof(BREGEXP, splitp) == kOffSplitp);
static_assert(offsetof(BREGEXP, rsv1) == kOffRsv1);
static_assert(offsetof(BREGEXP, parap) == kOffParap);
static_assert(offsetof(BREGEXP, paraendp) == kOffParaendp);
static_assert(offsetof(BREGEXP, transtblp) == kOffTranstblp);
static_assert(offsetof(BREGEXP, startp) == kOffStartp);
static_assert(offsetof(BREGEXP, endp) == kOffEndp);
static_assert(offsetof(BREGEXP, nparens) == kOffNparens);

TEST(BregexpProviderContract, LoadLibraryBoundaryStillUsed)
{
	CBregexpDll2 dll;
	ASSERT_EQ(DLL_SUCCESS, dll.InitDll());
	ASSERT_TRUE(dll.IsAvailable());
	ASSERT_NE(nullptr, dll.GetLoadedDllName());
	EXPECT_NE(nullptr, wcsstr(dll.GetLoadedDllName(), L"bregonig.dll"));
}

TEST(BregexpProviderContract, ExportSetAndPeHardeningMatchOwnedContract)
{
	CBregexpDll2 dll;
	ASSERT_EQ(DLL_SUCCESS, dll.InitDll());
	HMODULE module = dll.GetInstance();
	ASSERT_NE(nullptr, module);

	const auto* nt = NtHeaders(module);
	ASSERT_NE(nullptr, nt);
	EXPECT_EQ(0, nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress);
	EXPECT_EQ(kRequiredDllCharacteristics,
		nt->OptionalHeader.DllCharacteristics & kRequiredDllCharacteristics);

	auto exports = PeNamesFromDirectory(module, IMAGE_DIRECTORY_ENTRY_EXPORT);
	std::sort(exports.begin(), exports.end());
	std::vector<std::string> expected(std::begin(kRequiredExports), std::end(kRequiredExports));
	EXPECT_EQ(expected, exports);

	for (const auto& importName : PeNamesFromDirectory(module, IMAGE_DIRECTORY_ENTRY_IMPORT)) {
		EXPECT_TRUE(ImportIsAllowed(importName)) << importName;
		EXPECT_STRNE("onigmo.dll", importName.c_str());
	}
}

TEST(BregexpProviderContract, DifferentialCorpusMatchesBron420Oracle)
{
	CBregexpDll2 dll;
	ASSERT_EQ(DLL_SUCCESS, dll.InitDll());
	ASSERT_TRUE(dll.ExistBMatchEx());
	ASSERT_TRUE(dll.ExistBSubstEx());

	for (const auto& c : kCorpus) {
		SCOPED_TRACE(c.Id());
		std::wstring target = c.MakeTarget();
		wchar_t msg[BREGEXP_MAX_ERROR_MESSAGE_LEN] = {};
		BREGEXP_W* rx = nullptr;
		wchar_t* begin = target.data();
		wchar_t* endp = begin + target.size();
		wchar_t* mid = begin + c.Beg();
		int rc = 0;
		switch (c.GetApi()) {
		case Api::Match:
			rc = dll.BMatch(c.Pattern(), begin, endp, &rx, msg);
			break;
		case Api::MatchEx:
			rc = dll.BMatchEx(c.Pattern(), begin, mid, endp, &rx, msg);
			break;
		case Api::Subst:
			rc = dll.BSubst(c.Pattern(), begin, endp, &rx, msg);
			break;
		case Api::SubstEx:
			rc = dll.BSubstEx(c.Pattern(), begin, mid, endp, &rx, msg);
			break;
		}
		EXPECT_EQ(c.Rc(), rc);
		EXPECT_STREQ(c.Msg(), msg);
		if (rx == nullptr) {
			EXPECT_FALSE(c.Nparens().has_value());
			EXPECT_FALSE(c.Index().has_value());
			continue;
		}
		if (c.Nparens().has_value()) {
			EXPECT_EQ(*c.Nparens(), rx->nparens);
		}
		if (c.Index().has_value()) {
			ASSERT_NE(nullptr, rx->startp);
			ASSERT_NE(nullptr, rx->endp);
			EXPECT_EQ(*c.Index(), static_cast<int>(rx->startp[0] - begin));
			EXPECT_EQ(*c.LastIndex(), static_cast<int>(rx->endp[0] - begin));
		}
		if (c.Out() != nullptr) {
			ASSERT_NE(nullptr, rx->outp);
			ASSERT_NE(nullptr, rx->outendp);
			const auto nchars = static_cast<std::size_t>(rx->outendp - rx->outp);
			EXPECT_EQ(std::wstring(c.Out()), std::wstring(rx->outp, nchars));
		}
		else if (c.GetApi() == Api::Subst || c.GetApi() == Api::SubstEx) {
			EXPECT_TRUE(rx->outp == nullptr || rx->outendp == rx->outp);
		}
		dll.BRegfree(rx);
	}
}
