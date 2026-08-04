#if defined(ABI_FIXTURE_PACK_VALUE)

struct SakuraAbiFixtureValue {
	char tag;
	long long payload;
};

SakuraAbiFixtureValue sakura_abi_fixture_provider() noexcept;

int main() {
	const auto value = sakura_abi_fixture_provider();
	return value.tag == 's' && value.payload == 42 ? 0 : 1;
}

#elif defined(ABI_FIXTURE_STL_VALUE)

#include <vector>

std::vector<int> sakura_abi_fixture_provider();

int main() {
	const auto value = sakura_abi_fixture_provider();
	return value.size() == 1 && value[0] == 42 ? 0 : 1;
}

#else

extern "C" int sakura_abi_fixture_provider() noexcept;

int main() {
	return sakura_abi_fixture_provider() == 42 ? 0 : 1;
}

#endif
