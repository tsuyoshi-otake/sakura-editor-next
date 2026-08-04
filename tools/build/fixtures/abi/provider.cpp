#if defined(ABI_FIXTURE_PACK_VALUE)

struct SakuraAbiFixtureValue {
	char tag;
	long long payload;
};

SakuraAbiFixtureValue sakura_abi_fixture_provider() noexcept {
	return {'s', 42};
}

#elif defined(ABI_FIXTURE_STL_VALUE)

#include <vector>

std::vector<int> sakura_abi_fixture_provider() {
	return {42};
}

#else

extern "C" int sakura_abi_fixture_provider() noexcept {
	return 42;
}

#endif
