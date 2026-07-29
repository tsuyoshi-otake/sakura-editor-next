/*! @file */
#include "StdAfx.h"
#include "terminal/PowerShellLocator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <future>
#include <tuple>
#include <windows.h>
#include <winver.h>

#include "platform/Windows11Platform.h"

#pragma comment(lib, "Version.lib")

namespace terminal {
namespace {

constexpr std::array<EPowerShellSource, 6> kSourceOrder = {
	EPowerShellSource::UserProfile, EPowerShellSource::ProgramFiles,
	EPowerShellSource::Path, EPowerShellSource::AppPaths,
	EPowerShellSource::WindowsApps, EPowerShellSource::LegacySystem,
};

std::wstring Lower( std::wstring value )
{
	std::transform( value.begin(), value.end(), value.begin(), []( wchar_t c ) { return static_cast<wchar_t>(std::towlower(c)); } );
	return value;
}

bool IsLegacyPath( EPowerShellSource source, const std::wstring& path, const PowerShellVersion& version )
{
	if( source == EPowerShellSource::LegacySystem || version.major <= 5 ) return true;
	const auto lower = Lower(path);
	const auto slash = lower.find_last_of(L"\\/");
	return lower.substr(slash == std::wstring::npos ? 0 : slash + 1) == L"powershell.exe";
}

bool IsPreviewText( const std::wstring& value )
{
	const auto lower = Lower(value);
	return lower.find(L"preview") != std::wstring::npos || lower.find(L"-rc") != std::wstring::npos || lower.find(L"-beta") != std::wstring::npos || lower.find(L"-alpha") != std::wstring::npos;
}

std::wstring GetEnvironment( const wchar_t* name )
{
	const DWORD required = ::GetEnvironmentVariableW( name, nullptr, 0 );
	if( required == 0 ) return {};
	std::wstring value( required, L'\0' );
	if( ::GetEnvironmentVariableW( name, value.data(), required ) == 0 ) return {};
	value.resize( value.find(L'\0') );
	return value;
}

void AddIfPresent( std::vector<std::wstring>& paths, const std::wstring& path )
{
	if( !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ) paths.push_back(path);
}

std::wstring ReadAppPath( HKEY hive, const wchar_t* executable )
{
	const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + std::wstring(executable);
	wchar_t value[32768]{};
	DWORD bytes = sizeof(value);
	if( ::RegGetValueW(hive, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes) != ERROR_SUCCESS ) return {};
	std::wstring result(value);
	if( !result.empty() && result.front() == L'\"' ) {
		const auto end = result.find(L'\"', 1);
		return end == std::wstring::npos ? std::wstring{} : result.substr(1, end - 1);
	}
	const auto exe = Lower(result).find(L".exe");
	return exe == std::wstring::npos ? result : result.substr(0, exe + 4);
}

} // namespace

std::optional<PowerShellVersion> PowerShellVersion::Parse( const std::wstring& text )
{
	PowerShellVersion result;
	const auto firstDigit = std::find_if( text.begin(), text.end(), []( wchar_t c ) { return c >= L'0' && c <= L'9'; } );
	if( firstDigit == text.end() ) return std::nullopt;
	size_t position = static_cast<size_t>(firstDigit - text.begin());
	unsigned int* fields[] = { &result.major, &result.minor, &result.patch, &result.revision };
	for( unsigned int* field : fields ) {
		if( position >= text.size() || !std::iswdigit(text[position]) ) break;
		unsigned int value = 0;
		while( position < text.size() && std::iswdigit(text[position]) ) {
			value = value > 99999999 ? value : value * 10 + static_cast<unsigned int>(text[position++] - L'0');
		}
		*field = value;
		if( position >= text.size() || text[position] != L'.' ) break;
		++position;
	}
	result.preview = IsPreviewText(text);
	return result;
}

bool PowerShellVersion::operator<( const PowerShellVersion& other ) const
{
	return std::tie(major, minor, patch, revision) < std::tie(other.major, other.minor, other.patch, other.revision);
}

PowerShellLocator::PowerShellLocator( IPowerShellLocatorProvider& provider ) : m_provider(provider) {}

PowerShellDiscoveryResult PowerShellLocator::Discover()
{
	struct DiscoveredPath {
		EPowerShellSource source;
		std::wstring canonicalPath;
		std::wstring dedupeKey;
		PowerShellFileStamp stamp;
		std::wstring versionText;
		bool explicitlyConfigured = false;
		bool needsProbe = false;
	};

	PowerShellDiscoveryResult result;
	std::vector<PowerShellCandidate> stable, preview, legacy;
	std::vector<DiscoveredPath> discovered;
	std::unordered_map<std::wstring, bool> seen;
	for( const auto source : kSourceOrder ) {
		for( const auto& rawPath : m_provider.GetCandidates(source) ) {
			if( seen.size() >= kCandidateLimit ) break;
			const std::wstring canonicalPath = m_provider.CanonicalizePath(rawPath);
			const std::wstring dedupeKey = Lower(canonicalPath);
			if( canonicalPath.empty() || !seen.emplace(dedupeKey, true).second || !m_provider.IsAmd64Executable(canonicalPath) ) continue;
			const auto stamp = m_provider.GetFileStamp(canonicalPath);
			if( !stamp ) continue;
			// UserProfile is intentionally first in kSourceOrder.  Therefore when
			// an alias to the same canonical executable is found later, the
			// surviving record retains this explicit-selection bit.
			DiscoveredPath item{ source, canonicalPath, dedupeKey, *stamp, L"",
				source == EPowerShellSource::UserProfile };
			const auto cached = m_cache.find(dedupeKey);
			if( cached != m_cache.end() && cached->second.stamp == *stamp ) {
				item.versionText = cached->second.versionText;
			} else {
				auto productVersion = m_provider.GetProductVersion(canonicalPath);
				item.versionText = productVersion.value_or(L"");
				item.needsProbe = item.versionText.empty();
				if( !item.needsProbe ) m_cache[dedupeKey] = { *stamp, item.versionText };
			}
			discovered.push_back(std::move(item));
		}
		if( seen.size() >= kCandidateLimit ) break;
	}

	std::vector<std::size_t> probeIndices;
	for( std::size_t index = 0; index < discovered.size(); ++index ) {
		if( discovered[index].needsProbe ) probeIndices.push_back(index);
	}
	for( std::size_t batch = 0; batch < probeIndices.size(); batch += kMaxConcurrentVersionProbes ) {
		const std::size_t count = std::min<std::size_t>(kMaxConcurrentVersionProbes, probeIndices.size() - batch);
		std::vector<std::future<std::optional<std::wstring>>> probes;
		probes.reserve(count);
		for( std::size_t offset = 0; offset < count; ++offset ) {
			const auto path = discovered[probeIndices[batch + offset]].canonicalPath;
			probes.push_back(std::async(std::launch::async, [this, path] {
				return m_provider.ProbeVersion(path, kVersionProbeTimeoutMs);
			}));
		}
		for( std::size_t offset = 0; offset < count; ++offset ) {
			auto& item = discovered[probeIndices[batch + offset]];
			try {
				item.versionText = probes[offset].get().value_or(L"");
			} catch( ... ) {
				item.versionText.clear();
			}
			m_cache[item.dedupeKey] = { item.stamp, item.versionText };
		}
	}

	for( const auto& item : discovered ) {
			const auto version = PowerShellVersion::Parse(item.versionText);
			if( !version ) continue;
			const bool isLegacy = IsLegacyPath(item.source, item.canonicalPath, *version);
			const bool isPreview = version->preview || IsPreviewText(item.canonicalPath);
			PowerShellCandidate candidate{ item.canonicalPath, *version, item.source,
				isLegacy ? EPowerShellChannel::Legacy : (isPreview ? EPowerShellChannel::Preview : EPowerShellChannel::Stable),
				item.explicitlyConfigured };
			if( candidate.channel == EPowerShellChannel::Stable ) stable.push_back(candidate);
			else if( candidate.channel == EPowerShellChannel::Preview ) preview.push_back(candidate);
			else legacy.push_back(candidate);
	}
	auto newestFirst = []( const PowerShellCandidate& left, const PowerShellCandidate& right ) { return right.version < left.version; };
	std::sort(stable.begin(), stable.end(), newestFirst);
	std::sort(preview.begin(), preview.end(), newestFirst);
	std::sort(legacy.begin(), legacy.end(), newestFirst);
	result.candidates.insert(result.candidates.end(), stable.begin(), stable.end());
	result.candidates.insert(result.candidates.end(), preview.begin(), preview.end());
	result.candidates.insert(result.candidates.end(), legacy.begin(), legacy.end());
	const auto explicitProfile = std::find_if(result.candidates.begin(), result.candidates.end(),
		[](const PowerShellCandidate& candidate) { return candidate.explicitlyConfigured; });
	if( explicitProfile != result.candidates.end() ) result.defaultCandidate = *explicitProfile;
	else if( !stable.empty() ) result.defaultCandidate = stable.front();
	else if( !legacy.empty() ) result.defaultCandidate = legacy.front();
	return result;
}

TerminalProfileCatalog::TerminalProfileCatalog( ITerminalProfileProvider& provider )
	: m_provider(provider)
{
}

void TerminalProfileCatalog::EnsureDiscovered()
{
	if( m_discovery ) return;
	m_discovery = m_provider.DiscoverProfiles();
	if( !m_selectedPath.empty() ) {
		const auto selected = Lower(m_selectedPath);
		const bool stillAvailable = std::any_of(m_discovery->candidates.begin(), m_discovery->candidates.end(),
			[&](const TerminalProfile& profile) { return Lower(profile.path) == selected; });
		if( !stillAvailable ) m_selectedPath.clear();
	}
}

std::vector<TerminalProfile> TerminalProfileCatalog::Profiles()
{
	EnsureDiscovered();
	return m_discovery->candidates;
}

std::optional<TerminalProfile> TerminalProfileCatalog::ResolveProfile()
{
	EnsureDiscovered();
	if( !m_selectedPath.empty() ) {
		const auto selected = Lower(m_selectedPath);
		const auto found = std::find_if(m_discovery->candidates.begin(), m_discovery->candidates.end(),
			[&](const TerminalProfile& profile) { return Lower(profile.path) == selected; });
		if( found != m_discovery->candidates.end() ) return *found;
	}
	return m_discovery->defaultCandidate;
}

bool TerminalProfileCatalog::SelectProfile( std::wstring_view canonicalPath )
{
	EnsureDiscovered();
	const auto selected = Lower(std::wstring(canonicalPath));
	const auto found = std::find_if(m_discovery->candidates.begin(), m_discovery->candidates.end(),
		[&](const TerminalProfile& profile) { return Lower(profile.path) == selected; });
	if( found == m_discovery->candidates.end() ) return false;
	m_selectedPath = found->path;
	return true;
}

void TerminalProfileCatalog::Redetect()
{
	m_provider.InvalidateProfileCache();
	m_discovery.reset();
}

std::vector<std::wstring> NativePowerShellLocatorProvider::GetCandidates( EPowerShellSource source )
{
	std::vector<std::wstring> result;
	if( source == EPowerShellSource::UserProfile ) {
		AddIfPresent(result, GetEnvironment(L"POWERSHELL_EXE")); // Explicit user-scoped override, if configured.
	} else if( source == EPowerShellSource::ProgramFiles ) {
		const auto root = GetEnvironment(L"ProgramFiles");
		WIN32_FIND_DATAW find{};
		const auto wildcard = root + L"\\PowerShell\\*";
		HANDLE handle = ::FindFirstFileW(wildcard.c_str(), &find);
		if( handle != INVALID_HANDLE_VALUE ) {
			do {
				if( (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && wcscmp(find.cFileName, L".") != 0 && wcscmp(find.cFileName, L"..") != 0 ) {
					AddIfPresent(result, root + L"\\PowerShell\\" + find.cFileName + L"\\pwsh.exe");
				}
			} while( ::FindNextFileW(handle, &find) );
			::FindClose(handle);
		}
	} else if( source == EPowerShellSource::Path ) {
		wchar_t buffer[32768]{};
		if( ::SearchPathW(nullptr, L"pwsh.exe", nullptr, static_cast<DWORD>(std::size(buffer)), buffer, nullptr) ) AddIfPresent(result, buffer);
	} else if( source == EPowerShellSource::AppPaths ) {
		AddIfPresent(result, ReadAppPath(HKEY_CURRENT_USER, L"pwsh.exe"));
		AddIfPresent(result, ReadAppPath(HKEY_LOCAL_MACHINE, L"pwsh.exe"));
	} else if( source == EPowerShellSource::WindowsApps ) {
		const auto localAppData = GetEnvironment(L"LOCALAPPDATA");
		AddIfPresent(result, localAppData + L"\\Microsoft\\WindowsApps\\pwsh.exe");
	} else {
		const auto systemRoot = GetEnvironment(L"SystemRoot");
		AddIfPresent(result, systemRoot + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
	}
	return result;
}

std::wstring NativePowerShellLocatorProvider::CanonicalizePath( const std::wstring& path )
{
	wchar_t buffer[32768]{};
	const DWORD length = ::GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
	if( length == 0 || length >= std::size(buffer) ) return {};
	HANDLE file = ::CreateFileW(buffer, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if( file == INVALID_HANDLE_VALUE ) return std::wstring(buffer, length);
	const DWORD finalLength = ::GetFinalPathNameByHandleW(file, buffer, static_cast<DWORD>(std::size(buffer)), FILE_NAME_NORMALIZED);
	::CloseHandle(file);
	if( finalLength == 0 || finalLength >= std::size(buffer) ) return {};
	std::wstring result(buffer, finalLength);
	if( result.rfind(L"\\\\?\\UNC\\", 0) == 0 ) result.replace(0, 8, L"\\\\");
	else if( result.rfind(L"\\\\?\\", 0) == 0 ) result.erase(0, 4);
	return result;
}

std::optional<PowerShellFileStamp> NativePowerShellLocatorProvider::GetFileStamp( const std::wstring& path )
{
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if( !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ) return std::nullopt;
	ULARGE_INTEGER time{ .LowPart = data.ftLastWriteTime.dwLowDateTime, .HighPart = data.ftLastWriteTime.dwHighDateTime };
	ULARGE_INTEGER size{ .LowPart = data.nFileSizeLow, .HighPart = data.nFileSizeHigh };
	return PowerShellFileStamp{ size.QuadPart, time.QuadPart };
}

bool NativePowerShellLocatorProvider::IsAmd64Executable( const std::wstring& path )
{
	return platform::ReadPeMachineFromFile(path.c_str()) == platform::PeMachine::Amd64;
}

std::optional<std::wstring> NativePowerShellLocatorProvider::GetProductVersion( const std::wstring& path )
{
	DWORD unused = 0; const DWORD bytes = ::GetFileVersionInfoSizeW(path.c_str(), &unused);
	if( bytes == 0 ) return std::nullopt;
	std::vector<std::byte> data(bytes);
	if( !::GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()) ) return std::nullopt;
	struct Translation { WORD language; WORD codePage; };
	Translation* translation = nullptr; UINT count = 0;
	if( ::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translation), &count) && count >= sizeof(Translation) ) {
		wchar_t key[64]{}; swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translation->language, translation->codePage);
		wchar_t* value = nullptr; UINT chars = 0;
		if( ::VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &chars) && value && chars > 1 ) return std::wstring(value);
	}
	VS_FIXEDFILEINFO* fixed = nullptr; UINT fixedSize = 0;
	if( ::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedSize) && fixed && fixedSize >= sizeof(*fixed) ) {
		return std::to_wstring(HIWORD(fixed->dwProductVersionMS)) + L"." + std::to_wstring(LOWORD(fixed->dwProductVersionMS)) + L"." + std::to_wstring(HIWORD(fixed->dwProductVersionLS)) + L"." + std::to_wstring(LOWORD(fixed->dwProductVersionLS));
	}
	return std::nullopt;
}

std::optional<std::wstring> NativePowerShellLocatorProvider::ProbeVersion( const std::wstring& path, unsigned int timeoutMs )
{
	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	HANDLE read = nullptr;
	HANDLE write = nullptr;
	if( !::CreatePipe(&read, &write, &security, 0) ) return std::nullopt;
	if( !::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) ) {
		::CloseHandle(read);
		::CloseHandle(write);
		return std::nullopt;
	}
	HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if( nullInput == INVALID_HANDLE_VALUE ) {
		::CloseHandle(read);
		::CloseHandle(write);
		return std::nullopt;
	}

	SIZE_T attributeBytes = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
	std::vector<std::byte> attributeStorage(attributeBytes);
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	if( !::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes) ) {
		::CloseHandle(nullInput);
		::CloseHandle(read);
		::CloseHandle(write);
		return std::nullopt;
	}
	const HANDLE inheritedHandles[] = { write, nullInput };
	if( !::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		const_cast<HANDLE*>(inheritedHandles), sizeof(inheritedHandles), nullptr, nullptr) ) {
		::DeleteProcThreadAttributeList(startup.lpAttributeList);
		::CloseHandle(nullInput);
		::CloseHandle(read);
		::CloseHandle(write);
		return std::nullopt;
	}

	std::wstring command = L"\"" + path + L"\" -NoLogo -NoProfile -NonInteractive -Command \"$PSVersionTable.PSVersion.ToString()\"";
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdOutput = write;
	startup.StartupInfo.hStdError = write;
	startup.StartupInfo.hStdInput = nullInput;
	PROCESS_INFORMATION process{};
	const bool started = ::CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
	::DeleteProcThreadAttributeList(startup.lpAttributeList);
	::CloseHandle(nullInput);
	::CloseHandle(write);
	if( !started ) { ::CloseHandle(read); return std::nullopt; }
	const DWORD wait = ::WaitForSingleObject(process.hProcess, timeoutMs);
	if( wait != WAIT_OBJECT_0 ) {
		::TerminateProcess(process.hProcess, 1);
		::WaitForSingleObject(process.hProcess, 250);
		::CloseHandle(read);
		::CloseHandle(process.hThread);
		::CloseHandle(process.hProcess);
		return std::nullopt;
	}
	std::string bytes;
	char buffer[256];
	while( bytes.size() < 4096 ) {
		DWORD available = 0;
		if( !::PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) || available == 0 ) break;
		DWORD readCount = 0;
		const DWORD toRead = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
		if( !::ReadFile(read, buffer, toRead, &readCount, nullptr) || readCount == 0 ) break;
		bytes.append(buffer, readCount);
	}
	::CloseHandle(read); ::CloseHandle(process.hThread); ::CloseHandle(process.hProcess);
	if( bytes.empty() ) return std::nullopt;
	return std::wstring(bytes.begin(), bytes.end()); // PowerShell emits ASCII digits and punctuation here.
}

} // namespace terminal
