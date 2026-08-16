#include <windows.h>

#include "bregexp.h"

#include <stdio.h>

int main()
{
	wchar_t msg[BREGEXP_MAX_ERROR_MESSAGE_LEN] = {};
	BREGEXP* rx = nullptr;
	wchar_t pattern[] = L"/([0-9]+)/";
	wchar_t text[] = L"ab12cd";
	const int matched = BMatch(pattern, text, text + 6, &rx, msg);
	if (matched <= 0 || rx == nullptr || rx->nparens < 1) {
		fwprintf(stderr, L"BMatchW failed: %s\n", msg);
		return 1;
	}
	if (rx->startp[0] != text + 2 || rx->endp[0] != text + 4) {
		fwprintf(stderr, L"capture offsets drifted\n");
		BRegfree(rx);
		return 1;
	}
	BRegfree(rx);
	const wchar_t* version = BRegexpVersion();
	if (version == nullptr || version[0] == 0) {
		fwprintf(stderr, L"missing BRegexpVersionW\n");
		return 1;
	}
	return 0;
}
