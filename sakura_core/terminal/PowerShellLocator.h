/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace terminal {

enum class EPowerShellSource {
	UserProfile,
	ProgramFiles,
	Path,
	AppPaths,
	WindowsApps,
	LegacySystem,
};

enum class TerminalChannel {
	Stable,
	Preview,
	Legacy,
};

using EPowerShellChannel = TerminalChannel;

struct PowerShellVersion {
	unsigned int major = 0;
	unsigned int minor = 0;
	unsigned int patch = 0;
	unsigned int revision = 0;
	bool preview = false;

	static std::optional<PowerShellVersion> Parse( const std::wstring& text );
	bool operator<( const PowerShellVersion& other ) const;
};

struct PowerShellFileStamp {
	std::uint64_t size = 0;
	std::uint64_t lastWriteTime = 0;
	bool operator==( const PowerShellFileStamp& other ) const {
		return size == other.size && lastWriteTime == other.lastWriteTime;
	}
};

struct TerminalProfile {
	std::wstring path;
	PowerShellVersion version;
	EPowerShellSource source = EPowerShellSource::Path;
	TerminalChannel channel = TerminalChannel::Stable;
	// True only for a verified user-configured PowerShell (currently
	// POWERSHELL_EXE).  Keep this separate from ordering so canonical-path
	// de-duplication cannot turn an explicit selection into an automatic one.
	bool explicitlyConfigured = false;
};

using PowerShellCandidate = TerminalProfile;

struct PowerShellDiscoveryResult {
	std::vector<TerminalProfile> candidates; // Stable, preview, then legacy; each group is newest first.
	// A verified explicit profile wins. Otherwise this is the newest Stable,
	// falling back to Legacy when no Stable profile is available.
	std::optional<TerminalProfile> defaultCandidate;
};

class ITerminalProfileProvider {
public:
	virtual ~ITerminalProfileProvider() = default;
	virtual PowerShellDiscoveryResult DiscoverProfiles() = 0;
	virtual void InvalidateProfileCache() noexcept = 0;
};

// All operating-system interaction is behind this interface so discovery can be
// tested without depending on the PowerShell installations of the test machine.
class IPowerShellLocatorProvider {
public:
	virtual ~IPowerShellLocatorProvider() = default;
	virtual std::vector<std::wstring> GetCandidates( EPowerShellSource source ) = 0;
	virtual std::wstring CanonicalizePath( const std::wstring& path ) = 0;
	virtual std::optional<PowerShellFileStamp> GetFileStamp( const std::wstring& canonicalPath ) = 0;
	virtual bool IsAmd64Executable( const std::wstring& canonicalPath ) = 0;
	virtual std::optional<std::wstring> GetProductVersion( const std::wstring& canonicalPath ) = 0;
	// timeoutMs is always 2000. Implementations must terminate a timed-out probe
	// and accept up to two concurrent calls for different canonical paths.
	virtual std::optional<std::wstring> ProbeVersion( const std::wstring& canonicalPath, unsigned int timeoutMs ) = 0;
};

class PowerShellLocator final : public ITerminalProfileProvider {
public:
	explicit PowerShellLocator( IPowerShellLocatorProvider& provider );
	PowerShellDiscoveryResult Discover();
	void InvalidateCache() noexcept { m_cache.clear(); }
	PowerShellDiscoveryResult DiscoverProfiles() override { return Discover(); }
	void InvalidateProfileCache() noexcept override { InvalidateCache(); }

	static constexpr unsigned int kCandidateLimit = 32;
	static constexpr unsigned int kVersionProbeTimeoutMs = 2000;
	static constexpr unsigned int kMaxConcurrentVersionProbes = 2;

private:
	struct CacheValue {
		PowerShellFileStamp stamp;
		std::wstring versionText;
	};

	IPowerShellLocatorProvider& m_provider;
	std::unordered_map<std::wstring, CacheValue> m_cache;
};

//! Keeps explicit Preview/Legacy selections local to one editor process while
//! retaining the highest Stable profile as the automatic default.
class TerminalProfileCatalog {
public:
	explicit TerminalProfileCatalog( ITerminalProfileProvider& provider );
	[[nodiscard]] std::vector<TerminalProfile> Profiles();
	[[nodiscard]] std::optional<TerminalProfile> ResolveProfile();
	[[nodiscard]] bool SelectProfile( std::wstring_view canonicalPath );
	void Redetect();

private:
	void EnsureDiscovered();
	ITerminalProfileProvider& m_provider;
	std::optional<PowerShellDiscoveryResult> m_discovery;
	std::wstring m_selectedPath;
};

// Creates the Windows implementation. Callers own the returned provider.
class NativePowerShellLocatorProvider final : public IPowerShellLocatorProvider {
public:
	std::vector<std::wstring> GetCandidates( EPowerShellSource source ) override;
	std::wstring CanonicalizePath( const std::wstring& path ) override;
	std::optional<PowerShellFileStamp> GetFileStamp( const std::wstring& canonicalPath ) override;
	bool IsAmd64Executable( const std::wstring& canonicalPath ) override;
	std::optional<std::wstring> GetProductVersion( const std::wstring& canonicalPath ) override;
	std::optional<std::wstring> ProbeVersion( const std::wstring& canonicalPath, unsigned int timeoutMs ) override;
};

} // namespace terminal
