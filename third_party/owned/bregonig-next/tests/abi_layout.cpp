#include <windows.h>

#include "bregexp.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

template <int PtrBytes>
struct BregexpLayoutModel {
	using Ptr = std::conditional_t<PtrBytes == 8, std::uint64_t, std::uint32_t>;
	Ptr outp;
	Ptr outendp;
	int splitctr;
	Ptr splitp;
	Ptr rsv1;
	Ptr parap;
	Ptr paraendp;
	Ptr transtblp;
	Ptr startp;
	Ptr endp;
	int nparens;
};

static_assert(std::is_same_v<decltype(BREGEXP::rsv1), INT_PTR>, "rsv1 must be pointer-sized");
static_assert(std::is_same_v<std::remove_const_t<decltype(BREGEXP::splitctr)>, int>, "splitctr must be int");
static_assert(std::is_same_v<decltype(BREGEXP::nparens), int>, "nparens must be int");
static_assert(std::is_pointer_v<decltype(BREGEXP::outp)>, "outp must be a pointer");
static_assert(std::is_const_v<std::remove_pointer_t<decltype(BREGEXP::outp)>>, "outp points at const TCHAR");

static_assert(sizeof(BregexpLayoutModel<4>) == 44, "x86 sizeof");
static_assert(alignof(BregexpLayoutModel<4>) == 4, "x86 align");
static_assert(offsetof(BregexpLayoutModel<4>, rsv1) == 16, "x86 rsv1");
static_assert(offsetof(BregexpLayoutModel<4>, nparens) == 40, "x86 nparens");

static_assert(sizeof(BregexpLayoutModel<8>) == 88, "x64 sizeof");
static_assert(alignof(BregexpLayoutModel<8>) == 8, "x64 align");
static_assert(offsetof(BregexpLayoutModel<8>, rsv1) == 32, "x64 rsv1");
static_assert(offsetof(BregexpLayoutModel<8>, nparens) == 80, "x64 nparens");

static_assert(sizeof(BREGEXP) == sizeof(BregexpLayoutModel<sizeof(void*)>), "live sizeof");
static_assert(alignof(BREGEXP) == alignof(BregexpLayoutModel<sizeof(void*)>), "live align");
static_assert(offsetof(BREGEXP, outp) == offsetof(BregexpLayoutModel<sizeof(void*)>, outp), "outp");
static_assert(offsetof(BREGEXP, outendp) == offsetof(BregexpLayoutModel<sizeof(void*)>, outendp), "outendp");
static_assert(offsetof(BREGEXP, splitctr) == offsetof(BregexpLayoutModel<sizeof(void*)>, splitctr), "splitctr");
static_assert(offsetof(BREGEXP, splitp) == offsetof(BregexpLayoutModel<sizeof(void*)>, splitp), "splitp");
static_assert(offsetof(BREGEXP, rsv1) == offsetof(BregexpLayoutModel<sizeof(void*)>, rsv1), "rsv1");
static_assert(offsetof(BREGEXP, parap) == offsetof(BregexpLayoutModel<sizeof(void*)>, parap), "parap");
static_assert(offsetof(BREGEXP, paraendp) == offsetof(BregexpLayoutModel<sizeof(void*)>, paraendp), "paraendp");
static_assert(offsetof(BREGEXP, transtblp) == offsetof(BregexpLayoutModel<sizeof(void*)>, transtblp), "transtblp");
static_assert(offsetof(BREGEXP, startp) == offsetof(BregexpLayoutModel<sizeof(void*)>, startp), "startp");
static_assert(offsetof(BREGEXP, endp) == offsetof(BregexpLayoutModel<sizeof(void*)>, endp), "endp");
static_assert(offsetof(BREGEXP, nparens) == offsetof(BregexpLayoutModel<sizeof(void*)>, nparens), "nparens");

int main()
{
	return 0;
}
