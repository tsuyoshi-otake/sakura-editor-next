/*! @file */
#include "pch.h"
#include "platform/process/WindowsExecutableResolver.h"
#include "terminal/PowerShellLocator.h"

#include <map>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

class FakeProvider final : public terminal::IPowerShellLocatorProvider {
public:
	std::map<terminal::EPowerShellSource, std::vector<std::wstring>> candidates;
	std::map<std::wstring, std::wstring> productVersions;
	std::map<std::wstring, std::wstring> probeVersions;
	std::map<std::wstring, terminal::PowerShellFileStamp> stamps;
	std::map<std::wstring, std::wstring> canonicalPaths;
	std::vector<unsigned int> probeTimeouts;
	std::map<std::wstring, bool> amd64;
	unsigned int metadataCalls = 0;
	std::atomic<unsigned int> activeProbes = 0;
	std::atomic<unsigned int> maxActiveProbes = 0;
	unsigned int probeDelayMs = 0;
	std::mutex probeMutex;

	std::vector<std::wstring> GetCandidates( terminal::EPowerShellSource source ) override { return candidates[source]; }
	std::wstring CanonicalizePath( const std::wstring& path ) override {
		const auto it = canonicalPaths.find(path);
		return it == canonicalPaths.end() ? path : it->second;
	}
	std::optional<terminal::PowerShellFileStamp> GetFileStamp( const std::wstring& path ) override {
		const auto it = stamps.find(path);
		return it == stamps.end() ? std::optional<terminal::PowerShellFileStamp>{ terminal::PowerShellFileStamp{ static_cast<std::uint64_t>(path.size()), 1 } } : it->second;
	}
	bool IsAmd64Executable( const std::wstring& path ) override { const auto it = amd64.find(path); return it == amd64.end() || it->second; }
	std::optional<std::wstring> GetProductVersion( const std::wstring& path ) override { ++metadataCalls; const auto it = productVersions.find(path); return it == productVersions.end() ? std::nullopt : std::optional<std::wstring>(it->second); }
	std::optional<std::wstring> ProbeVersion( const std::wstring& path, unsigned int timeoutMs ) override {
		const auto active = activeProbes.fetch_add(1) + 1;
		auto observed = maxActiveProbes.load();
		while( observed < active && !maxActiveProbes.compare_exchange_weak(observed, active) ) {}
		std::optional<std::wstring> result;
		{
			std::lock_guard lock(probeMutex);
			probeTimeouts.push_back(timeoutMs);
			const auto it = probeVersions.find(path);
			if( it != probeVersions.end() ) result = it->second;
		}
		if( probeDelayMs != 0 ) std::this_thread::sleep_for(std::chrono::milliseconds(probeDelayMs));
		activeProbes.fetch_sub(1);
		return result;
	}
};

class ScopedTestFile final {
public:
	explicit ScopedTestFile( std::wstring path ) : m_path(std::move(path))
	{
		m_handle = ::CreateFileW(m_path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	~ScopedTestFile()
	{
		if( m_handle != INVALID_HANDLE_VALUE ) ::CloseHandle(m_handle);
		::DeleteFileW(m_path.c_str());
	}

	ScopedTestFile( const ScopedTestFile& ) = delete;
	ScopedTestFile& operator=( const ScopedTestFile& ) = delete;

	[[nodiscard]] bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }
	[[nodiscard]] const std::wstring& Path() const { return m_path; }

private:
	std::wstring m_path;
	HANDLE m_handle = INVALID_HANDLE_VALUE;
};

std::wstring TempDirectoryPath()
{
	wchar_t tempPath[MAX_PATH]{};
	const DWORD tempLength = ::GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
	if( tempLength == 0 || tempLength >= std::size(tempPath) ) return {};
	wchar_t uniquePath[MAX_PATH]{};
	if( ::GetTempFileNameW(tempPath, L"sen", 0, uniquePath) == 0 ) return {};
	::DeleteFileW(uniquePath);
	return ::CreateDirectoryW(uniquePath, nullptr) ? std::wstring(uniquePath) : std::wstring{};
}

class ScopedTestDirectory final {
public:
	ScopedTestDirectory() : m_path(TempDirectoryPath()) {}
	~ScopedTestDirectory()
	{
		if( !m_path.empty() ) ::RemoveDirectoryW(m_path.c_str());
	}

	ScopedTestDirectory( const ScopedTestDirectory& ) = delete;
	ScopedTestDirectory& operator=( const ScopedTestDirectory& ) = delete;

	[[nodiscard]] bool IsValid() const { return !m_path.empty(); }
	[[nodiscard]] const std::wstring& Path() const { return m_path; }

private:
	std::wstring m_path;
};

class FakeProfileProvider final : public terminal::ITerminalProfileProvider {
public:
	terminal::PowerShellDiscoveryResult discovery;
	int discoverCalls = 0;
	int invalidations = 0;

	terminal::PowerShellDiscoveryResult DiscoverProfiles() override { ++discoverCalls; return discovery; }
	void InvalidateProfileCache() noexcept override { ++invalidations; }
};

TEST(PowerShellLocator, ExplicitOlderProfileWinsOverNewerStable)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::UserProfile] = { L"C:\\Users\\A Name\\pwsh.exe" };
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { L"C:\\Program Files\\PowerShell\\7\\pwsh.exe", L"c:\\users\\a name\\PWSH.exe" };
	provider.productVersions[L"C:\\Users\\A Name\\pwsh.exe"] = L"7.4.0";
	provider.productVersions[L"C:\\Program Files\\PowerShell\\7\\pwsh.exe"] = L"7.5.0";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_EQ(2u, result.candidates.size());
	ASSERT_TRUE(result.defaultCandidate.has_value());
	EXPECT_EQ(L"C:\\Users\\A Name\\pwsh.exe", result.defaultCandidate->path);
	EXPECT_TRUE(result.defaultCandidate->explicitlyConfigured);
}

TEST(PowerShellLocator, CanonicalDuplicateRetainsExplicitProfilePriority)
{
	FakeProvider provider;
	const std::wstring explicitAlias = L"C:\\Tools\\pwsh.exe";
	const std::wstring installedPath = L"C:\\Program Files\\PowerShell\\7\\pwsh.exe";
	provider.candidates[terminal::EPowerShellSource::UserProfile] = { explicitAlias };
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { installedPath };
	provider.canonicalPaths[explicitAlias] = installedPath;
	provider.productVersions[installedPath] = L"7.6.3";

	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_EQ(1u, result.candidates.size());
	EXPECT_EQ(installedPath, result.candidates.front().path);
	EXPECT_TRUE(result.candidates.front().explicitlyConfigured);
	ASSERT_TRUE(result.defaultCandidate.has_value());
	EXPECT_EQ(installedPath, result.defaultCandidate->path);
}

TEST(PowerShellLocator, InvalidExplicitProfileFallsBackToHighestStable)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::UserProfile] = { L"x86-explicit.exe" };
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { L"stable.exe" };
	provider.amd64[L"x86-explicit.exe"] = false;
	provider.productVersions[L"stable.exe"] = L"7.6.3";

	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_TRUE(result.defaultCandidate.has_value());
	EXPECT_EQ(L"stable.exe", result.defaultCandidate->path);
	EXPECT_FALSE(result.defaultCandidate->explicitlyConfigured);
}

TEST(PowerShellLocator, PicksHighestStableWhileLeavingPreviewSelectable)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { L"stable.exe", L"preview.exe" };
	provider.productVersions[L"stable.exe"] = L"PowerShell 7.4.2";
	provider.productVersions[L"preview.exe"] = L"7.5.0-preview.3";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_TRUE(result.defaultCandidate.has_value());
	EXPECT_EQ(L"stable.exe", result.defaultCandidate->path);
	ASSERT_EQ(2u, result.candidates.size());
	EXPECT_EQ(terminal::EPowerShellChannel::Preview, result.candidates[1].channel);
}

TEST(PowerShellLocator, UsesLegacyOnlyWhenNoStableExists)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { L"preview.exe" };
	provider.candidates[terminal::EPowerShellSource::LegacySystem] = { L"legacy.exe" };
	provider.productVersions[L"preview.exe"] = L"7.5.0-preview.1";
	provider.productVersions[L"legacy.exe"] = L"5.1.19041.1";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_TRUE(result.defaultCandidate.has_value());
	EXPECT_EQ(L"legacy.exe", result.defaultCandidate->path);
	EXPECT_EQ(terminal::EPowerShellChannel::Legacy, result.defaultCandidate->channel);
}

TEST(PowerShellLocator, NeverTreatsWindowsPowerShellAsPowerShell7Stable)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::UserProfile] = { L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe" };
	provider.productVersions[provider.candidates[terminal::EPowerShellSource::UserProfile].front()] = L"10.0.26100.1";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_TRUE(result.defaultCandidate.has_value());
	EXPECT_EQ(terminal::EPowerShellChannel::Legacy, result.defaultCandidate->channel);
}

TEST(PowerShellLocator, RecognizesPreviewFromInstallPathWhenMetadataLacksSuffix)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { L"C:\\Program Files\\PowerShell\\7-preview\\pwsh.exe" };
	provider.productVersions[provider.candidates[terminal::EPowerShellSource::ProgramFiles].front()] = L"7.7.0.0";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_EQ(1u, result.candidates.size());
	EXPECT_EQ(terminal::EPowerShellChannel::Preview, result.candidates.front().channel);
	EXPECT_FALSE(result.defaultCandidate.has_value());
}

TEST(PowerShellLocator, RejectsNonAmd64Executables)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::ProgramFiles] = { L"arm64.exe", L"amd64.exe" };
	provider.amd64[L"arm64.exe"] = false;
	provider.productVersions[L"amd64.exe"] = L"7.4.0";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	ASSERT_EQ(1u, result.candidates.size());
	EXPECT_EQ(L"amd64.exe", result.candidates.front().path);
}

TEST(PowerShellLocator, CapsCandidatesAndBoundsUnknownVersionProbe)
{
	FakeProvider provider;
	for( int i = 0; i != 40; ++i ) provider.candidates[terminal::EPowerShellSource::ProgramFiles].push_back(L"candidate" + std::to_wstring(i) + L".exe");
	provider.probeVersions[L"candidate0.exe"] = L"7.4.0";
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	EXPECT_LE(provider.metadataCalls, terminal::PowerShellLocator::kCandidateLimit);
	ASSERT_FALSE(provider.probeTimeouts.empty());
	for( const auto timeout : provider.probeTimeouts ) EXPECT_EQ(2000u, timeout);
}

TEST(PowerShellLocator, RunsAtMostTwoUnknownVersionProbesConcurrently)
{
	FakeProvider provider;
	provider.probeDelayMs = 20;
	for( int i = 0; i != 6; ++i ) {
		const auto path = L"probe" + std::to_wstring(i) + L".exe";
		provider.candidates[terminal::EPowerShellSource::ProgramFiles].push_back(path);
		provider.probeVersions[path] = L"7.6." + std::to_wstring(i);
	}
	terminal::PowerShellLocator locator(provider);
	const auto result = locator.Discover();
	EXPECT_EQ(6u, result.candidates.size());
	EXPECT_GE(provider.maxActiveProbes.load(), 1u);
	EXPECT_LE(provider.maxActiveProbes.load(), terminal::PowerShellLocator::kMaxConcurrentVersionProbes);
}

TEST(PowerShellLocator, DoesNotProbeUntrustedPathCandidateWithoutMetadata)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::Path] = { L"untrusted-path.exe" };
	provider.probeVersions[L"untrusted-path.exe"] = L"7.6.3";
	terminal::PowerShellLocator locator(provider);

	const auto result = locator.Discover();
	EXPECT_TRUE(result.candidates.empty());
	EXPECT_TRUE(provider.probeTimeouts.empty());
}

TEST(WindowsExecutableResolver, RejectsEmptyPathEntryInsteadOfCurrentDirectory)
{
	std::vector<std::wstring> candidates;
	const auto candidateResolver = [&candidates]( std::wstring_view candidate ) -> std::optional<std::wstring> {
		candidates.emplace_back(candidate);
		return std::nullopt;
	};

	const auto result = platform::ResolveWindowsExecutableFromPath(
		L"pwsh.exe", L";C:\\path-that-does-not-exist", candidateResolver);
	EXPECT_FALSE(result.has_value());
	ASSERT_EQ(1u, candidates.size());
	EXPECT_EQ(L"C:\\path-that-does-not-exist\\pwsh.exe", candidates.front());
}

TEST(WindowsExecutableResolver, RejectsRelativePathEntry)
{
	std::vector<std::wstring> candidates;
	const auto candidateResolver = [&candidates]( std::wstring_view candidate ) -> std::optional<std::wstring> {
		candidates.emplace_back(candidate);
		return std::nullopt;
	};

	const auto result = platform::ResolveWindowsExecutableFromPath(
		L"pwsh.exe", L".;C:\\path-that-does-not-exist", candidateResolver);
	EXPECT_FALSE(result.has_value());
	ASSERT_EQ(1u, candidates.size());
	EXPECT_EQ(L"C:\\path-that-does-not-exist\\pwsh.exe", candidates.front());
}

TEST(WindowsExecutableResolver, AcceptsQuotedAbsolutePathEntryWithoutUsingCurrentDirectory)
{
	std::vector<std::wstring> candidates;
	const auto candidateResolver = [&candidates]( std::wstring_view candidate ) -> std::optional<std::wstring> {
		candidates.emplace_back(candidate);
		return std::wstring(candidate);
	};

	const auto result = platform::ResolveWindowsExecutableFromPath(
		L"pwsh.exe", L"\"C:\\Program Files\\PowerShell\\7\"", candidateResolver);
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe", *result);
	ASSERT_EQ(1u, candidates.size());
}

TEST(WindowsExecutableResolver, FindsExistingAbsolutePathCandidate)
{
	ScopedTestDirectory directory;
	ASSERT_TRUE(directory.IsValid());
	const auto executableName = L"pwsh.exe";
	ScopedTestFile executable(directory.Path() + L"\\" + executableName);
	ASSERT_TRUE(executable.IsOpen());

	const auto result = platform::ResolveWindowsExecutableFromPath(executableName, directory.Path());
	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(platform::IsAbsoluteWindowsPath(*result));
	wchar_t longPath[32768]{};
	const DWORD longPathLength = ::GetLongPathNameW(executable.Path().c_str(), longPath, static_cast<DWORD>(std::size(longPath)));
	ASSERT_GT(longPathLength, 0u);
	ASSERT_LT(longPathLength, std::size(longPath));
	EXPECT_EQ(0, _wcsicmp(result->c_str(), std::wstring(longPath, longPathLength).c_str()));
}

TEST(PowerShellLocator, CachesVersionByPathStampAndVersion)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::Path] = { L"cached.exe" };
	provider.productVersions[L"cached.exe"] = L"7.4.0";
	terminal::PowerShellLocator locator(provider);
	locator.Discover(); locator.Discover();
	EXPECT_EQ(1u, provider.metadataCalls);
}

TEST(PowerShellLocator, RefreshesCachedVersionWhenFileStampChanges)
{
	FakeProvider provider;
	provider.candidates[terminal::EPowerShellSource::Path] = { L"cached.exe" };
	provider.productVersions[L"cached.exe"] = L"7.4.0";
	provider.stamps[L"cached.exe"] = { 100, 1 };
	terminal::PowerShellLocator locator(provider);
	locator.Discover();
	provider.stamps[L"cached.exe"] = { 101, 2 };
	provider.productVersions[L"cached.exe"] = L"7.6.3";
	const auto refreshed = locator.Discover();
	EXPECT_EQ(2u, provider.metadataCalls);
	ASSERT_TRUE(refreshed.defaultCandidate.has_value());
	EXPECT_EQ(7u, refreshed.defaultCandidate->version.major);
	EXPECT_EQ(6u, refreshed.defaultCandidate->version.minor);
	EXPECT_EQ(3u, refreshed.defaultCandidate->version.patch);
}

TEST(TerminalProfileCatalog, UsesStableByDefaultAndAllowsExplicitPreviewSelection)
{
	FakeProfileProvider provider;
	const terminal::TerminalProfile stable{ L"stable.exe", { 7, 6, 3 }, terminal::EPowerShellSource::ProgramFiles,
		terminal::TerminalChannel::Stable };
	const terminal::TerminalProfile preview{ L"preview.exe", { 7, 7, 0, 0, true }, terminal::EPowerShellSource::ProgramFiles,
		terminal::TerminalChannel::Preview };
	provider.discovery.candidates = { stable, preview };
	provider.discovery.defaultCandidate = stable;
	terminal::TerminalProfileCatalog catalog(provider);

	ASSERT_EQ(L"stable.exe", catalog.ResolveProfile()->path);
	EXPECT_TRUE(catalog.SelectProfile(L"PREVIEW.EXE"));
	ASSERT_EQ(L"preview.exe", catalog.ResolveProfile()->path);
	EXPECT_EQ(1, provider.discoverCalls);
}

TEST(TerminalProfileCatalog, RedetectPreservesAvailableSelectionAndFallsBackWhenRemoved)
{
	FakeProfileProvider provider;
	const terminal::TerminalProfile stable{ L"stable.exe", { 7, 6, 3 }, terminal::EPowerShellSource::ProgramFiles,
		terminal::TerminalChannel::Stable };
	const terminal::TerminalProfile preview{ L"preview.exe", { 7, 7, 0, 0, true }, terminal::EPowerShellSource::ProgramFiles,
		terminal::TerminalChannel::Preview };
	provider.discovery.candidates = { stable, preview };
	provider.discovery.defaultCandidate = stable;
	terminal::TerminalProfileCatalog catalog(provider);
	ASSERT_TRUE(catalog.SelectProfile(preview.path));

	catalog.Redetect();
	ASSERT_EQ(preview.path, catalog.ResolveProfile()->path);
	provider.discovery.candidates = { stable };
	provider.discovery.defaultCandidate = stable;
	catalog.Redetect();
	ASSERT_EQ(stable.path, catalog.ResolveProfile()->path);
	EXPECT_EQ(2, provider.invalidations);
}

} // namespace
