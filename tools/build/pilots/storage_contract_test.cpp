/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/storage/IStorageAuthority.h>
#include <sakura/storage/IStorageService.h>
#include <sakura/storage/StorageAuthorityFactory.h>
#include <sakura/storage/StorageSnapshotCache.h>
#include <sakura/storage/StorageTypes.h>

#if __has_include("platform/storage/StorageTypes.h")
#error "sakura_storage_tests consumer can reach the provider private header"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace platform::storage;

static_assert(std::has_virtual_destructor_v<IStorageService>);
static_assert(std::has_virtual_destructor_v<IStorageChangeSubscription>);
static_assert(std::has_virtual_destructor_v<IStorageAuthority>);

class FakeStorageAuthority final : public IStorageAuthority {
public:
	[[nodiscard]] StorageAuthorityOpenResult Open() override
	{
		if (m_open) return { EStorageAuthorityOpenStatus::AlreadyOpen, {} };
		m_open = true;
		return { EStorageAuthorityOpenStatus::Opened, {} };
	}

	void Close() noexcept override { m_open = false; }
	[[nodiscard]] bool IsOpen() const noexcept override { return m_open; }

	[[nodiscard]] StorageMutationResult Apply(const StorageMutationRequest&) override
	{
		return { .status = EStorageMutationStatus::NotApplicable, .diagnostic = "fake authority" };
	}

	[[nodiscard]] StorageSnapshot Snapshot() const override { return {}; }

	[[nodiscard]] std::unique_ptr<IStorageChangeSubscription> Subscribe(StorageChangeCallback) override
	{
		return nullptr;
	}

private:
	bool m_open = false;
};

StorageAddress ProfileAddress(std::string profileId, std::string key)
{
	return { .scope = EStorageScope::Profile, .scopeId = std::move(profileId),
		.owner = "workbench.layout", .key = std::move(key) };
}

#define CHECK_TRUE(expression) do { if (!(expression)) return false; } while (false)

bool RequiresScopeIdentityOnlyForProfileAndWorkspace()
{
	const StorageAddress application{ EStorageScope::Application, {}, "owner", "key" };
	const StorageAddress invalidApplication{ EStorageScope::Application, "unexpected", "owner", "key" };
	const StorageAddress profile{ EStorageScope::Profile, "profile-a", "owner", "key" };
	const StorageAddress invalidWorkspace{ EStorageScope::Workspace, {}, "owner", "key" };
	const StorageAddress invalidOwner{ EStorageScope::Profile, "profile-a", {}, "key" };

	return application.IsValid() && !invalidApplication.IsValid() && profile.IsValid()
		&& !invalidWorkspace.IsValid() && !invalidOwner.IsValid();
}

bool ComposesThroughLifecyclePortAndReachesTerminalClose()
{
	FakeStorageAuthority authority;
	CHECK_TRUE(!authority.IsOpen());
	CHECK_TRUE(authority.Open().status == EStorageAuthorityOpenStatus::Opened);
	CHECK_TRUE(authority.IsOpen());
	CHECK_TRUE(authority.Open().status == EStorageAuthorityOpenStatus::AlreadyOpen);
	authority.Close();
	CHECK_TRUE(!authority.IsOpen());
	return true;
}

bool PersistsThroughPublicAuthorityFactoryAndReopens()
{
	const auto uniqueSuffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const auto directory = std::filesystem::temp_directory_path()
		/ ("sakura-storage-contract-" + std::to_string(uniqueSuffix));
	struct Cleanup final {
		std::filesystem::path directory;
		~Cleanup() noexcept
		{
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
	} cleanup{ directory };

	std::error_code error;
	std::filesystem::remove_all(directory, error);
	auto authority = CreateAtomicFileStorageAuthority(directory, 17, 8);
	CHECK_TRUE(authority);
	CHECK_TRUE(authority->Open().Succeeded());
	const auto address = ProfileAddress("profile-a", "factory-backed");
	const StorageMutationRequest request{
		.operationId = "factory-backed-operation",
		.mutations = { StorageMutation{ .address = address, .target = EStorageTarget::Machine, .value = "persisted" } },
	};
	const auto applied = authority->Apply(request);
	CHECK_TRUE(applied.status == EStorageMutationStatus::Succeeded);
	CHECK_TRUE(std::filesystem::is_regular_file(directory / L"storage-v1.bin", error));
	authority->Close();
	CHECK_TRUE(!authority->IsOpen());
	authority.reset();

	auto reopened = CreateAtomicFileStorageAuthority(directory, 18, 8);
	CHECK_TRUE(reopened);
	CHECK_TRUE(reopened->Open().Succeeded());
	const auto snapshot = reopened->Snapshot();
	bool found = false;
	for (const auto& entry : snapshot.entries) {
		if (entry.address == address && entry.value == "persisted" && entry.revision == 1) {
			found = true;
			break;
		}
	}
	CHECK_TRUE(found);
	reopened->Close();
	return true;
}

bool RejectsUnknownScopesOversizedPartsAndMalformedUtf8()
{
	StorageAddress unknownScope{ EStorageScope::Profile, "profile-a", "owner", "key" };
	unknownScope.scope = static_cast<EStorageScope>(0xff);
	const StorageAddress oversized{ EStorageScope::Profile, "profile-a", "owner",
		std::string(kMaximumStorageAddressPartBytes + 1, 'k') };
	const StorageAddress malformed{ EStorageScope::Profile, "profile-a", "owner", std::string("\xc0\xaf", 2) };

	return !unknownScope.IsValid() && !oversized.IsValid() && !malformed.IsValid()
		&& IsValidStorageUtf8("\xe8\xa8\xad\xe5\xae\x9a");
}

bool RequiresSnapshotOnGenerationOrRevisionGap()
{
	const auto address = ProfileAddress("profile-a", "panel.size");
	CStorageSnapshotCache cache;
	const StorageChangeBatch batch{ .generation = 11, .baseRevision = 0, .revision = 1,
		.changes = { StorageChange{ .address = address, .target = EStorageTarget::Machine,
			.entry = StorageEntry{ .address = address, .target = EStorageTarget::Machine,
				.value = "220", .revision = 1 } } } };
	CHECK_TRUE(cache.Apply(batch) == EStorageChangeApplyStatus::ResyncRequired);
	cache.Replace(StorageSnapshot{ .generation = 11, .revision = 0 });
	CHECK_TRUE(cache.Apply(batch) == EStorageChangeApplyStatus::Applied);
	const auto cached = cache.Find(address);
	CHECK_TRUE(cached && cached->value == "220");
	CHECK_TRUE(cache.Apply(batch) == EStorageChangeApplyStatus::IgnoredStale);
	CHECK_TRUE(cache.Apply(StorageChangeBatch{ .generation = 11, .baseRevision = 2, .revision = 3 })
		== EStorageChangeApplyStatus::ResyncRequired);
	CHECK_TRUE(cache.Apply(StorageChangeBatch{ .generation = 12, .baseRevision = 1, .revision = 2 })
		== EStorageChangeApplyStatus::ResyncRequired);
	cache.Replace(StorageSnapshot{ .generation = 12, .revision = 1 });
	CHECK_TRUE(cache.Apply(batch) == EStorageChangeApplyStatus::IgnoredStale);
	return true;
}

bool MatchesTestsGenerationAndRevisionAsOneCacheState()
{
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{ .generation = 11, .revision = 0 });
	CHECK_TRUE(cache.Matches(11, 0));
	CHECK_TRUE(!cache.Matches(11, 1));
	CHECK_TRUE(!cache.Matches(12, 0));

	const auto address = ProfileAddress("profile-a", "panel.position");
	const StorageChangeBatch batch{ .generation = 11, .baseRevision = 0, .revision = 1,
		.changes = { StorageChange{ .address = address, .target = EStorageTarget::Machine,
			.entry = StorageEntry{ .address = address, .target = EStorageTarget::Machine,
				.value = "bottom", .revision = 1 } } } };
	CHECK_TRUE(cache.Apply(batch) == EStorageChangeApplyStatus::Applied);
	CHECK_TRUE(!cache.Matches(11, 0));
	CHECK_TRUE(cache.Matches(11, 1));
	cache.Replace(StorageSnapshot{ .generation = 0, .revision = 1 });
	CHECK_TRUE(!cache.Matches(11, 1));
	CHECK_TRUE(cache.Matches(0, 0));
	return true;
}

bool MalformedSnapshotsAndBatchesFailClosedWithoutPartialApplication()
{
	const auto firstAddress = ProfileAddress("profile-a", "first");
	const auto secondAddress = ProfileAddress("profile-a", "second");
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{
		.generation = 4,
		.revision = 1,
		.entries = { StorageEntry{ .address = firstAddress, .target = EStorageTarget::Machine,
			.value = "stable", .revision = 1 } },
	});
	CHECK_TRUE(cache.GetGeneration() == 4);
	const StorageChangeBatch mismatched{
		.generation = 4,
		.baseRevision = 1,
		.revision = 2,
		.changes = { StorageChange{
			.address = secondAddress,
			.target = EStorageTarget::Machine,
			.entry = StorageEntry{ .address = firstAddress, .target = EStorageTarget::Machine,
				.value = "corrupt", .revision = 2 },
		} },
	};
	CHECK_TRUE(cache.Apply(mismatched) == EStorageChangeApplyStatus::ResyncRequired);
	CHECK_TRUE(cache.GetRevision() == 1);
	const auto stable = cache.Find(firstAddress);
	CHECK_TRUE(stable && stable->value == "stable");
	CHECK_TRUE(!cache.Find(secondAddress));
	cache.Replace(StorageSnapshot{
		.generation = 0,
		.revision = 1,
		.entries = { StorageEntry{ .address = firstAddress, .target = EStorageTarget::Machine,
			.value = "invalid", .revision = 1 } },
	});
	CHECK_TRUE(cache.GetGeneration() == 0 && cache.GetRevision() == 0 && !cache.Find(firstAddress));
	return true;
}

bool ConcurrentApplyAndReadsExposeOnlyCopiedCoherentEntries()
{
	constexpr std::uint64_t kGeneration = 9;
	constexpr std::uint64_t kLastRevision = 128;
	const auto address = ProfileAddress("profile-a", "retry-worker-value");
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{ .generation = kGeneration, .revision = 0 });
	std::atomic_bool writerStarted = false;
	std::atomic_bool readerStarted = false;
	std::atomic_bool writerFinished = false;
	std::atomic_bool writerObservedApplyFailure = false;
	std::atomic_bool readerObservedInvalidEntry = false;
	std::thread writer([&] {
		writerStarted.store(true, std::memory_order_release);
		while (!readerStarted.load(std::memory_order_acquire)) {}
		for (std::uint64_t revision = 1; revision <= kLastRevision; ++revision) {
			const StorageChangeBatch batch{
				.generation = kGeneration,
				.baseRevision = revision - 1,
				.revision = revision,
				.changes = { StorageChange{ .address = address, .target = EStorageTarget::Machine,
					.entry = StorageEntry{ .address = address, .target = EStorageTarget::Machine,
						.value = std::to_string(revision), .revision = revision } } },
			};
			if (cache.Apply(batch) != EStorageChangeApplyStatus::Applied) {
				writerObservedApplyFailure.store(true, std::memory_order_release);
			}
		}
		writerFinished.store(true, std::memory_order_release);
	});
	std::thread reader([&] {
		while (!writerStarted.load(std::memory_order_acquire)) {}
		readerStarted.store(true, std::memory_order_release);
		while (!writerFinished.load(std::memory_order_acquire)) {
			const auto entry = cache.Find(address);
			if (entry && (entry->value != std::to_string(entry->revision)
				|| entry->address != address || entry->target != EStorageTarget::Machine)) {
				readerObservedInvalidEntry.store(true, std::memory_order_release);
			}
			if (cache.GetGeneration() != kGeneration || cache.GetRevision() > kLastRevision) {
				readerObservedInvalidEntry.store(true, std::memory_order_release);
			}
		}
	});
	writer.join();
	reader.join();
	CHECK_TRUE(!writerObservedApplyFailure.load(std::memory_order_acquire));
	CHECK_TRUE(!readerObservedInvalidEntry.load(std::memory_order_acquire));
	CHECK_TRUE(cache.GetGeneration() == kGeneration && cache.GetRevision() == kLastRevision);
	const auto entry = cache.Find(address);
	return entry && entry->value == std::to_string(kLastRevision) && entry->revision == kLastRevision;
}

bool ConcurrentReplaceAndReadsExposeOnlyCompleteSnapshots()
{
	constexpr std::uint64_t kGeneration = 10;
	constexpr std::uint64_t kReplacementCount = 128;
	const auto address = ProfileAddress("profile-a", "resynchronized-value");
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{ .generation = kGeneration, .revision = 1,
		.entries = { StorageEntry{ .address = address, .target = EStorageTarget::Machine,
			.value = "1", .revision = 1 } } });
	std::atomic_bool writerStarted = false;
	std::atomic_bool readerStarted = false;
	std::atomic_bool writerFinished = false;
	std::atomic_bool readerObservedInvalidSnapshot = false;
	std::thread writer([&] {
		writerStarted.store(true, std::memory_order_release);
		while (!readerStarted.load(std::memory_order_acquire)) {}
		for (std::uint64_t revision = 2; revision <= kReplacementCount; ++revision) {
			cache.Replace(StorageSnapshot{ .generation = kGeneration, .revision = revision,
				.entries = { StorageEntry{ .address = address, .target = EStorageTarget::Machine,
					.value = std::to_string(revision), .revision = revision } } });
		}
		writerFinished.store(true, std::memory_order_release);
	});
	std::thread reader([&] {
		while (!writerStarted.load(std::memory_order_acquire)) {}
		readerStarted.store(true, std::memory_order_release);
		while (!writerFinished.load(std::memory_order_acquire)) {
			const auto entry = cache.Find(address);
			if (!entry || entry->value != std::to_string(entry->revision)
				|| entry->address != address || entry->target != EStorageTarget::Machine
				|| cache.GetGeneration() != kGeneration || cache.GetRevision() > kReplacementCount) {
				readerObservedInvalidSnapshot.store(true, std::memory_order_release);
			}
		}
	});
	writer.join();
	reader.join();
	CHECK_TRUE(!readerObservedInvalidSnapshot.load(std::memory_order_acquire));
	CHECK_TRUE(cache.GetGeneration() == kGeneration && cache.GetRevision() == kReplacementCount);
	const auto entry = cache.Find(address);
	return entry && entry->value == std::to_string(kReplacementCount)
		&& entry->revision == kReplacementCount;
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{ "RequiresScopeIdentityOnlyForProfileAndWorkspace", RequiresScopeIdentityOnlyForProfileAndWorkspace },
	TestCase{ "ComposesThroughLifecyclePortAndReachesTerminalClose", ComposesThroughLifecyclePortAndReachesTerminalClose },
	TestCase{ "PersistsThroughPublicAuthorityFactoryAndReopens", PersistsThroughPublicAuthorityFactoryAndReopens },
	TestCase{ "RejectsUnknownScopesOversizedPartsAndMalformedUtf8", RejectsUnknownScopesOversizedPartsAndMalformedUtf8 },
	TestCase{ "RequiresSnapshotOnGenerationOrRevisionGap", RequiresSnapshotOnGenerationOrRevisionGap },
	TestCase{ "MatchesTestsGenerationAndRevisionAsOneCacheState", MatchesTestsGenerationAndRevisionAsOneCacheState },
	TestCase{ "MalformedSnapshotsAndBatchesFailClosedWithoutPartialApplication", MalformedSnapshotsAndBatchesFailClosedWithoutPartialApplication },
	TestCase{ "ConcurrentApplyAndReadsExposeOnlyCopiedCoherentEntries", ConcurrentApplyAndReadsExposeOnlyCopiedCoherentEntries },
	TestCase{ "ConcurrentReplaceAndReadsExposeOnlyCompleteSnapshots", ConcurrentReplaceAndReadsExposeOnlyCompleteSnapshots },
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "StorageAddress.\n";
			for (const auto& test : kTests) {
				if (!test.name.starts_with("ComposesThroughLifecycle") && !test.name.starts_with("PersistsThroughPublicAuthorityFactory") && !test.name.starts_with("RequiresSnapshot") && !test.name.starts_with("MatchesTests")
					&& !test.name.starts_with("MalformedSnapshots") && !test.name.starts_with("Concurrent")) {
					std::cout << "  " << test.name << '\n';
				}
			}
			std::cout << "StorageAuthority.\n";
			for (const auto& test : kTests) {
				if (test.name.starts_with("ComposesThroughLifecycle") || test.name.starts_with("PersistsThroughPublicAuthorityFactory")) std::cout << "  " << test.name << '\n';
			}
			std::cout << "StorageSnapshotCache.\n";
			for (const auto& test : kTests) {
				if (test.name.starts_with("RequiresSnapshot") || test.name.starts_with("MatchesTests")
					|| test.name.starts_with("MalformedSnapshots") || test.name.starts_with("Concurrent")) {
					std::cout << "  " << test.name << '\n';
				}
			}
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string_view suite = test.name.starts_with("ComposesThroughLifecycle") || test.name.starts_with("PersistsThroughPublicAuthorityFactory") ? "StorageAuthority."
			: test.name.starts_with("RequiresSnapshot")
			|| test.name.starts_with("MatchesTests")
			|| test.name.starts_with("MalformedSnapshots")
			|| test.name.starts_with("Concurrent") ? "StorageSnapshotCache." : "StorageAddress.";
		const std::string fullName = std::string(suite) + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}

#undef CHECK_TRUE
