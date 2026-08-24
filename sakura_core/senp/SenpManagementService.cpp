/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "senp/SenpManagementService.h"

#include <sakura/serialization/JsoncDocument.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace senp {
namespace {

using platform::serialization::JsoncValue;

struct BuiltInResource final {
	std::wstring_view id;
	int packageResource;
	int hashResource;
	bool installedByDefault;
};

// Keep this table as the package-management boundary for built-ins. Adding a
// built-in does not change the installer or runtime service contract.
constexpr std::array kBuiltInResources{
	BuiltInResource{ L"sakura-indent-rainbow", 39000, 39001, true },
	BuiltInResource{ L"sakura-core-language-basics", 39002, 39003, true },
	BuiltInResource{ L"sakura-shell-language-basics", 39004, 39005, true },
	BuiltInResource{ L"sakura-database-language-basics", 39006, 39007, true },
	BuiltInResource{ L"sakura-infrastructure-language-basics", 39008, 39009, true },
	BuiltInResource{ L"sakura-configuration-language-basics", 39010, 39011, true },
	BuiltInResource{ L"sakura-legacy-language-basics", 39012, 39013, true },
};
constexpr DWORD kToolTimeoutMilliseconds = 30'000;
constexpr std::size_t kMaximumToolOutputBytes = 4U * 1024U * 1024U;

class ScopedHandle final {
public:
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE value) noexcept : m_value(value) {}
	~ScopedHandle() { Reset(); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept : m_value(std::exchange(other.m_value, nullptr)) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (this != &other) Reset(std::exchange(other.m_value, nullptr));
		return *this;
	}
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE) ::CloseHandle(m_value);
		m_value = value;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] HANDLE* Put() noexcept { return &m_value; }

private:
	HANDLE m_value = nullptr;
};

std::wstring QuoteArgument(std::wstring_view value)
{
	if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(value);
	}
	std::wstring result(1, L'\"');
	std::size_t slashes = 0;
	for (const wchar_t ch : value) {
		if (ch == L'\\') {
			++slashes;
			continue;
		}
		if (ch == L'\"') {
			result.append(slashes * 2 + 1, L'\\');
			result.push_back(ch);
			slashes = 0;
			continue;
		}
		result.append(slashes, L'\\');
		slashes = 0;
		result.push_back(ch);
	}
	result.append(slashes * 2, L'\\');
	result.push_back(L'\"');
	return result;
}

std::wstring ExecutableDirectory()
{
	std::array<wchar_t, 32768> path{};
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size()) return {};
	return std::filesystem::path(std::wstring(path.data(), length)).parent_path().native();
}

struct ProcessResult final {
	bool launched = false;
	bool timedOut = false;
	DWORD exitCode = ERROR_GEN_FAILURE;
	std::string output;
	std::string error;
};

void ReadPipeBounded(HANDLE pipe, std::string& target) noexcept
{
	std::array<char, 16384> buffer{};
	for (;;) {
		DWORD read = 0;
		if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
		const auto room = kMaximumToolOutputBytes > target.size()
			? kMaximumToolOutputBytes - target.size() : 0;
		const auto copy = std::min<std::size_t>(room, read);
		target.append(buffer.data(), copy);
	}
}

ProcessResult RunTool(const std::vector<std::wstring>& arguments)
{
	ProcessResult result;
	const auto directory = ExecutableDirectory();
	if (directory.empty()) return result;
	const auto executable = std::filesystem::path(directory) / L"sakura-senp-tool.exe";
	if (!std::filesystem::is_regular_file(executable)) return result;
	std::wstring commandLine = QuoteArgument(executable.native());
	for (const auto& argument : arguments) {
		commandLine.push_back(L' ');
		commandLine += QuoteArgument(argument);
	}

	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	ScopedHandle outputRead;
	ScopedHandle outputWrite;
	ScopedHandle errorRead;
	ScopedHandle errorWrite;
	ScopedHandle nullInput;
	if (!::CreatePipe(outputRead.Put(), outputWrite.Put(), &security, 0)
		|| !::CreatePipe(errorRead.Put(), errorWrite.Put(), &security, 0)) return result;
	nullInput.Reset(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (nullInput.Get() == nullptr || nullInput.Get() == INVALID_HANDLE_VALUE
		|| !::SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0)
		|| !::SetHandleInformation(errorRead.Get(), HANDLE_FLAG_INHERIT, 0)) return result;

	ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
	if (job.Get() == nullptr || job.Get() == INVALID_HANDLE_VALUE) return result;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!::SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
		&limits, sizeof(limits))) return result;

	SIZE_T attributeBytes = 0;
	(void)::InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
	if (attributeBytes == 0) return result;
	std::vector<std::uint8_t> attributeStorage(attributeBytes);
	auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	if (!::InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) return result;
	struct AttributeListGuard final {
		LPPROC_THREAD_ATTRIBUTE_LIST value{};
		~AttributeListGuard() { if (value != nullptr) ::DeleteProcThreadAttributeList(value); }
	} attributeGuard{ attributes };
	const std::array<HANDLE, 3> inheritedHandles{ nullInput.Get(), outputWrite.Get(), errorWrite.Get() };
	if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		const_cast<HANDLE*>(inheritedHandles.data()), sizeof(inheritedHandles), nullptr, nullptr)) return result;
	HANDLE jobHandle = job.Get();
	if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
		&jobHandle, sizeof(jobHandle), nullptr, nullptr)) return result;

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullInput.Get();
	startup.StartupInfo.hStdOutput = outputWrite.Get();
	startup.StartupInfo.hStdError = errorWrite.Get();
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION process{};
	const BOOL created = ::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
		TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
		&startup.StartupInfo, &process);
	outputWrite.Reset();
	errorWrite.Reset();
	nullInput.Reset();
	if (!created) return result;
	result.launched = true;
	ScopedHandle processHandle(process.hProcess);
	ScopedHandle processThread(process.hThread);
	std::thread outputReader(ReadPipeBounded, outputRead.Get(), std::ref(result.output));
	std::thread errorReader(ReadPipeBounded, errorRead.Get(), std::ref(result.error));
	const DWORD wait = ::WaitForSingleObject(processHandle.Get(), kToolTimeoutMilliseconds);
	if (wait != WAIT_OBJECT_0) {
		result.timedOut = true;
		if (!::TerminateJobObject(job.Get(), ERROR_TIMEOUT)) {
			(void)::TerminateProcess(processHandle.Get(), ERROR_TIMEOUT);
		}
		(void)::WaitForSingleObject(processHandle.Get(), 1000);
	}
	(void)::GetExitCodeProcess(processHandle.Get(), &result.exitCode);
	if (outputReader.joinable()) outputReader.join();
	if (errorReader.joinable()) errorReader.join();
	outputRead.Reset();
	errorRead.Reset();
	return result;
}

std::optional<std::vector<std::byte>> EmbeddedResource(int id)
{
	const HMODULE module = ::GetModuleHandleW(nullptr);
	const HRSRC found = ::FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
	if (found == nullptr) return std::nullopt;
	const DWORD size = ::SizeofResource(module, found);
	const HGLOBAL loaded = ::LoadResource(module, found);
	const auto* bytes = static_cast<const std::byte*>(::LockResource(loaded));
	if (size == 0 || bytes == nullptr) return std::nullopt;
	return std::vector<std::byte>(bytes, bytes + size);
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error) return false;
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output) return false;
	output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	output.flush();
	return output.good();
}

std::wstring Utf8ToWide(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), length) != length) return {};
	return result;
}

const std::wstring* StringMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<std::wstring>(&found->second.Value());
}

const bool* BoolMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<bool>(&found->second.Value());
}

const JsoncValue::Array* ArrayMember(const JsoncValue::Object& object, std::wstring_view name)
{
	const auto found = object.find(name);
	return found == object.end() ? nullptr : std::get_if<JsoncValue::Array>(&found->second.Value());
}

bool ParseStringArray(const JsoncValue::Object& object, std::wstring_view name,
	std::vector<std::wstring>& target)
{
	const auto* values = ArrayMember(object, name);
	if (values == nullptr || values->size() > 256) return false;
	target.reserve(values->size());
	for (const auto& value : *values) {
		const auto* text = std::get_if<std::wstring>(&value.Value());
		if (text == nullptr) return false;
		target.push_back(*text);
	}
	return true;
}

bool ParseLanguageContributions(const JsoncValue::Object& contributes,
	std::vector<LanguageContribution>& target)
{
	const auto* languages = ArrayMember(contributes, L"languages");
	if (languages == nullptr || languages->size() > 64) return false;
	target.reserve(languages->size());
	for (const auto& value : *languages) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return false;
		const auto* id = StringMember(*object, L"id");
		if (id == nullptr) return false;
		LanguageContribution contribution{ .id = *id };
		if (!ParseStringArray(*object, L"aliases", contribution.aliases)
			|| !ParseStringArray(*object, L"extensions", contribution.extensions)
			|| !ParseStringArray(*object, L"filenames", contribution.filenames)
			|| !ParseStringArray(*object, L"filenamePatterns", contribution.filenamePatterns)
			|| !ParseStringArray(*object, L"mimetypes", contribution.mimetypes)) return false;
		if (const auto* firstLine = StringMember(*object, L"firstLine")) contribution.firstLine = *firstLine;
		if (const auto* configuration = StringMember(*object, L"configuration")) {
			contribution.configuration = *configuration;
		}
		target.push_back(std::move(contribution));
	}
	return true;
}

bool ParseGrammarContributions(const JsoncValue::Object& contributes,
	std::vector<GrammarContribution>& target)
{
	const auto* grammars = ArrayMember(contributes, L"grammars");
	if (grammars == nullptr || grammars->size() > 128) return false;
	target.reserve(grammars->size());
	for (const auto& value : *grammars) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return false;
		const auto* scopeName = StringMember(*object, L"scopeName");
		const auto* path = StringMember(*object, L"path");
		if (scopeName == nullptr || path == nullptr) return false;
		GrammarContribution contribution{ .scopeName = *scopeName, .path = *path };
		if (const auto* language = StringMember(*object, L"language")) contribution.language = *language;
		if (!ParseStringArray(*object, L"injectTo", contribution.injectTo)) return false;
		target.push_back(std::move(contribution));
	}
	return true;
}

std::optional<std::vector<ExtensionDescriptor>> ParseInstalled(std::string_view json)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(json);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* array = std::get_if<JsoncValue::Array>(&parsed.value->Value());
	if (array == nullptr || array->size() > 256) return std::nullopt;
	std::vector<ExtensionDescriptor> result;
	result.reserve(array->size());
	for (const auto& value : *array) {
		const auto* object = std::get_if<JsoncValue::Object>(&value.Value());
		if (object == nullptr) return std::nullopt;
		const auto manifestMember = object->find(L"manifest");
		const auto* manifest = manifestMember == object->end()
			? nullptr : std::get_if<JsoncValue::Object>(&manifestMember->second.Value());
		const auto* archive = StringMember(*object, L"archiveSha256");
		const auto* enabled = BoolMember(*object, L"enabled");
		const auto* signedPackage = BoolMember(*object, L"signed");
		const auto* trust = StringMember(*object, L"trust");
		const auto* readme = StringMember(*object, L"readme");
		const auto* extensionPath = StringMember(*object, L"extensionPath");
		const auto* modulePath = StringMember(*object, L"modulePath");
		const auto moduleSha256Member = object->find(L"moduleSha256");
		const auto* moduleSha256 = moduleSha256Member == object->end()
			? nullptr : std::get_if<std::wstring>(&moduleSha256Member->second.Value());
		if (manifest == nullptr || archive == nullptr || enabled == nullptr
			|| signedPackage == nullptr || trust == nullptr || readme == nullptr || extensionPath == nullptr) {
			return std::nullopt;
		}
		const auto isLowerSha256 = [](std::wstring_view value) noexcept {
			return value.size() == 64 && std::ranges::all_of(value, [](wchar_t ch) {
				return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
			});
		};
		if (modulePath != nullptr) {
			if (modulePath->empty() || moduleSha256 == nullptr || !isLowerSha256(*moduleSha256)) {
				return std::nullopt;
			}
		} else if (moduleSha256Member == object->end()
			|| !std::holds_alternative<std::monostate>(moduleSha256Member->second.Value())) {
			return std::nullopt;
		}
		const auto* id = StringMember(*manifest, L"id");
		const auto* displayName = StringMember(*manifest, L"displayName");
		const auto* version = StringMember(*manifest, L"version");
		const auto* publisher = StringMember(*manifest, L"publisher");
		const auto* description = StringMember(*manifest, L"description");
		const auto contributesMember = manifest->find(L"contributes");
		const auto* contributes = contributesMember == manifest->end() ? nullptr
			: std::get_if<JsoncValue::Object>(&contributesMember->second.Value());
		const auto decorationsMember = contributes == nullptr ? JsoncValue::Object::const_iterator{}
			: contributes->find(L"editorDecorations");
		const auto* decorations = contributes == nullptr || decorationsMember == contributes->end()
			? nullptr : std::get_if<JsoncValue::Array>(&decorationsMember->second.Value());
		std::vector<LanguageContribution> languages;
		std::vector<GrammarContribution> grammars;
		if (id == nullptr || displayName == nullptr || version == nullptr
			|| publisher == nullptr || description == nullptr || archive->size() != 64
			|| decorations == nullptr || contributes == nullptr
			|| !ParseLanguageContributions(*contributes, languages)
			|| !ParseGrammarContributions(*contributes, grammars)) return std::nullopt;
		result.push_back({
			.id = *id,
			.displayName = *displayName,
			.version = *version,
			.publisher = *publisher,
			.description = *description,
			.readme = *readme,
			.extensionPath = *extensionPath,
			.modulePath = modulePath == nullptr ? std::wstring{} : *modulePath,
			.moduleSha256 = moduleSha256 == nullptr ? std::wstring{} : *moduleSha256,
			.archiveSha256 = *archive,
			.installed = true,
			.builtIn = *trust == L"builtin",
			.enabled = *enabled,
			.signedPackage = *signedPackage,
			.contributesIndentDecorations = !decorations->empty(),
			.languages = std::move(languages),
			.grammars = std::move(grammars),
			.trust = *trust,
		});
	}
	return result;
}

std::optional<std::vector<std::wstring>> ParseUninstalled(std::string_view json)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(json);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* array = std::get_if<JsoncValue::Array>(&parsed.value->Value());
	if (array == nullptr || array->size() > 256) return std::nullopt;
	std::vector<std::wstring> result;
	result.reserve(array->size());
	for (const auto& value : *array) {
		const auto* id = std::get_if<std::wstring>(&value.Value());
		if (id == nullptr || id->empty() || id->size() > 128
			|| std::ranges::find(result, *id) != result.end()) return std::nullopt;
		result.push_back(*id);
	}
	return result;
}

std::optional<ExtensionDescriptor> ParseBuiltInCandidate(std::string_view json)
{
	const auto parsed = platform::serialization::CJsoncDocument::Parse(json);
	if (!parsed.Succeeded()) return std::nullopt;
	const auto* object = std::get_if<JsoncValue::Object>(&parsed.value->Value());
	if (object == nullptr) return std::nullopt;
	const auto manifestMember = object->find(L"manifest");
	const auto* manifest = manifestMember == object->end()
		? nullptr : std::get_if<JsoncValue::Object>(&manifestMember->second.Value());
	const auto* archive = StringMember(*object, L"archiveSha256");
	const auto* signedPackage = BoolMember(*object, L"signed");
	if (manifest == nullptr || archive == nullptr || archive->size() != 64
		|| signedPackage == nullptr) return std::nullopt;
	const auto* id = StringMember(*manifest, L"id");
	const auto* displayName = StringMember(*manifest, L"displayName");
	const auto* version = StringMember(*manifest, L"version");
	const auto* publisher = StringMember(*manifest, L"publisher");
	const auto* description = StringMember(*manifest, L"description");
	const auto contributesMember = manifest->find(L"contributes");
	const auto* contributes = contributesMember == manifest->end() ? nullptr
		: std::get_if<JsoncValue::Object>(&contributesMember->second.Value());
	const auto decorationsMember = contributes == nullptr ? JsoncValue::Object::const_iterator{}
		: contributes->find(L"editorDecorations");
	const auto* decorations = contributes == nullptr || decorationsMember == contributes->end()
		? nullptr : std::get_if<JsoncValue::Array>(&decorationsMember->second.Value());
	std::vector<LanguageContribution> languages;
	std::vector<GrammarContribution> grammars;
	if (id == nullptr || displayName == nullptr || version == nullptr
		|| publisher == nullptr || description == nullptr || decorations == nullptr
		|| contributes == nullptr || !ParseLanguageContributions(*contributes, languages)
		|| !ParseGrammarContributions(*contributes, grammars)) {
		return std::nullopt;
	}
	return ExtensionDescriptor{
		.id = *id,
		.displayName = *displayName,
		.version = *version,
		.publisher = *publisher,
		.description = *description,
		.archiveSha256 = *archive,
		.installed = false,
		.builtIn = true,
		.enabled = false,
		.signedPackage = *signedPackage,
		.contributesIndentDecorations = !decorations->empty(),
		.languages = std::move(languages),
		.grammars = std::move(grammars),
		.trust = L"builtin",
	};
}

struct StagedBuiltInPackage final {
	std::filesystem::path path;
	std::wstring hash;
};

std::optional<StagedBuiltInPackage> StageBuiltInPackage(
	const BuiltInResource& builtIn, std::wstring_view installRoot, std::wstring& diagnostic)
{
	const auto package = EmbeddedResource(builtIn.packageResource);
	const auto hashBytes = EmbeddedResource(builtIn.hashResource);
	if (!package || !hashBytes) {
		diagnostic = L"Built-in SENP resources are missing: " + std::wstring(builtIn.id);
		return std::nullopt;
	}
	std::string hash(reinterpret_cast<const char*>(hashBytes->data()), hashBytes->size());
	while (!hash.empty() && (hash.back() == '\r' || hash.back() == '\n')) hash.pop_back();
	if (hash.size() != 64) {
		diagnostic = L"Built-in SENP hash resource is invalid: " + std::wstring(builtIn.id);
		return std::nullopt;
	}
	const auto staging = std::filesystem::path(installRoot) / L"staging"
		/ (std::wstring(builtIn.id) + L"." + std::to_wstring(::GetCurrentProcessId()) + L".senp");
	if (!WriteBytes(staging, *package)) {
		diagnostic = L"Built-in SENP package could not be staged: " + std::wstring(builtIn.id);
		return std::nullopt;
	}
	return StagedBuiltInPackage{ staging, Utf8ToWide(hash) };
}

std::wstring ProcessDiagnostic(const ProcessResult& result, std::wstring_view fallback)
{
	if (!result.launched) return L"sakura-senp-tool.exe is unavailable";
	if (result.timedOut) return L"SENP package operation timed out";
	const auto error = Utf8ToWide(result.error);
	return error.empty() ? std::wstring(fallback) : error;
}

} // namespace

CWin32SenpManagementService::CWin32SenpManagementService(std::wstring profileRoot)
	: m_profileRoot(std::move(profileRoot))
	, m_installRoot((std::filesystem::path(m_profileRoot) / L"senp").native())
{
}

CWin32SenpManagementService::~CWin32SenpManagementService()
{
	Stop();
}

ManagementOperationResult CWin32SenpManagementService::Terminal(
	EManagementOperationStatus status, std::wstring diagnostic)
{
	std::lock_guard lock(m_mutex);
	if (!diagnostic.empty()) m_snapshot.diagnostic = std::move(diagnostic);
	return { status, m_snapshot };
}

bool CWin32SenpManagementService::IsStopped() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_snapshot.state == EManagementState::Stopped;
}

ManagementOperationResult CWin32SenpManagementService::Start()
{
	{
		std::lock_guard lock(m_mutex);
		if (m_snapshot.state == EManagementState::Ready
			|| m_snapshot.state == EManagementState::ReadyWithDiagnostics) {
			return { EManagementOperationStatus::AlreadyReady, m_snapshot };
		}
		if (m_snapshot.state == EManagementState::Stopped) {
			return { EManagementOperationStatus::Stopped, m_snapshot };
		}
	}
	const auto catalog = LoadBuiltInCatalog();
	if (!catalog.Succeeded()) return catalog;
	auto loaded = ReloadInstalled();
	if (!loaded.Succeeded()) return loaded;
	for (const auto& builtIn : kBuiltInResources) {
		if (!builtIn.installedByDefault) continue;
		bool current = false;
		bool explicitlyUninstalled = false;
		{
			std::lock_guard lock(m_mutex);
			const auto installed = std::ranges::find(m_snapshot.extensions,
				builtIn.id, &ExtensionDescriptor::id);
			const auto candidate = std::ranges::find(m_builtInCatalog,
				builtIn.id, &ExtensionDescriptor::id);
			current = installed != m_snapshot.extensions.end() && installed->installed
				&& candidate != m_builtInCatalog.end()
				&& installed->archiveSha256 == candidate->archiveSha256;
			explicitlyUninstalled = std::ranges::find(m_uninstalledBuiltIns,
				builtIn.id) != m_uninstalledBuiltIns.end();
		}
		if (current || explicitlyUninstalled) continue;
		loaded = InstallBuiltInPackage(builtIn.id);
		if (!loaded.Succeeded()) return loaded;
	}
	return loaded;
}

ManagementOperationResult CWin32SenpManagementService::LoadBuiltInCatalog()
{
	std::vector<ExtensionDescriptor> catalog;
	catalog.reserve(kBuiltInResources.size());
	for (const auto& builtIn : kBuiltInResources) {
		std::wstring diagnostic;
		const auto staged = StageBuiltInPackage(builtIn, m_installRoot, diagnostic);
		if (!staged) {
			std::lock_guard lock(m_mutex);
			m_snapshot.state = EManagementState::Failed;
			++m_snapshot.revision;
			m_snapshot.diagnostic = std::move(diagnostic);
			return { EManagementOperationStatus::Unavailable, m_snapshot };
		}
		const auto inspected = RunTool({ L"inspect-builtin", staged->path.native(), staged->hash });
		std::error_code ignored;
		std::filesystem::remove(staged->path, ignored);
		if (!inspected.launched || inspected.timedOut || inspected.exitCode != 0) {
			std::lock_guard lock(m_mutex);
			m_snapshot.state = EManagementState::Failed;
			++m_snapshot.revision;
			m_snapshot.diagnostic = ProcessDiagnostic(inspected, L"Built-in SENP package was rejected");
			return { EManagementOperationStatus::Failed, m_snapshot };
		}
		auto extension = ParseBuiltInCandidate(inspected.output);
		if (!extension || extension->id != builtIn.id) {
			std::lock_guard lock(m_mutex);
			m_snapshot.state = EManagementState::Failed;
			++m_snapshot.revision;
			m_snapshot.diagnostic = L"Built-in SENP package metadata is invalid";
			return { EManagementOperationStatus::Failed, m_snapshot };
		}
		catalog.push_back(std::move(*extension));
	}
	std::lock_guard lock(m_mutex);
	m_builtInCatalog = std::move(catalog);
	return { EManagementOperationStatus::Succeeded, m_snapshot };
}

ManagementOperationResult CWin32SenpManagementService::ReloadInstalled()
{
	if (IsStopped()) return Terminal(EManagementOperationStatus::Stopped);
	const auto listed = RunTool({ L"list", m_installRoot });
	if (!listed.launched || listed.timedOut || listed.exitCode != 0) {
		std::lock_guard lock(m_mutex);
		m_snapshot.state = m_snapshot.extensions.empty()
			? EManagementState::Failed : EManagementState::ReadyWithDiagnostics;
		++m_snapshot.revision;
		m_snapshot.diagnostic = ProcessDiagnostic(listed, L"Installed SENP extensions could not be read");
		return { EManagementOperationStatus::Failed, m_snapshot };
	}
	const auto listedUninstalled = RunTool({ L"list-uninstalled", m_installRoot });
	if (!listedUninstalled.launched || listedUninstalled.timedOut
		|| listedUninstalled.exitCode != 0) {
		std::lock_guard lock(m_mutex);
		m_snapshot.state = m_snapshot.extensions.empty()
			? EManagementState::Failed : EManagementState::ReadyWithDiagnostics;
		++m_snapshot.revision;
		m_snapshot.diagnostic = ProcessDiagnostic(listedUninstalled,
			L"Uninstalled SENP extension preferences could not be read");
		return { EManagementOperationStatus::Failed, m_snapshot };
	}
	const auto parsed = ParseInstalled(listed.output);
	const auto parsedUninstalled = ParseUninstalled(listedUninstalled.output);
	if (!parsed || !parsedUninstalled) {
		std::lock_guard lock(m_mutex);
		m_snapshot.state = m_snapshot.extensions.empty()
			? EManagementState::Failed : EManagementState::ReadyWithDiagnostics;
		++m_snapshot.revision;
		m_snapshot.diagnostic = L"SENP package-state output is invalid";
		return { EManagementOperationStatus::Failed, m_snapshot };
	}
	std::lock_guard lock(m_mutex);
	auto extensions = std::move(*parsed);
	m_uninstalledBuiltIns = std::move(*parsedUninstalled);
	for (const auto& candidate : m_builtInCatalog) {
		const auto installed = std::ranges::find(extensions, candidate.id, &ExtensionDescriptor::id);
		if (installed == extensions.end()) extensions.push_back(candidate);
	}
	std::ranges::sort(extensions, {}, &ExtensionDescriptor::displayName);
	m_snapshot.extensions = std::move(extensions);
	m_snapshot.diagnostic.clear();
	m_snapshot.state = EManagementState::Ready;
	++m_snapshot.revision;
	return { EManagementOperationStatus::Succeeded, m_snapshot };
}

ManagementOperationResult CWin32SenpManagementService::InstallDeveloperPackage(
	std::wstring_view packagePath, bool enable)
{
	if (packagePath.empty() || packagePath.size() >= 32768 || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const std::filesystem::path package(packagePath);
	if (package.extension() != L".senp" || !std::filesystem::is_regular_file(package)) {
		return Terminal(EManagementOperationStatus::InvalidRequest, L"Select a regular .senp package");
	}
	const auto installed = RunTool({ L"install-dev", package.native(), m_installRoot });
	if (!installed.launched || installed.timedOut || installed.exitCode != 0) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(installed, L"Developer SENP package was rejected"));
	}
	if (enable) {
		const auto parsed = platform::serialization::CJsoncDocument::Parse(installed.output);
		const auto* root = parsed.Succeeded()
			? std::get_if<JsoncValue::Object>(&parsed.value->Value()) : nullptr;
		const auto manifestMember = root == nullptr ? JsoncValue::Object::const_iterator{}
			: root->find(L"manifest");
		const auto* manifest = root == nullptr || manifestMember == root->end()
			? nullptr : std::get_if<JsoncValue::Object>(&manifestMember->second.Value());
		const auto* id = manifest == nullptr ? nullptr : StringMember(*manifest, L"id");
		if (id == nullptr) return Terminal(EManagementOperationStatus::Failed, L"SENP install result is invalid");
		const auto enabled = RunTool({ L"set-enabled", m_installRoot, *id, L"true" });
		if (!enabled.launched || enabled.timedOut || enabled.exitCode != 0) {
			return Terminal(EManagementOperationStatus::Failed,
				ProcessDiagnostic(enabled, L"Developer SENP extension could not be enabled"));
		}
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::InstallBuiltInPackage(
	std::wstring_view extensionId)
{
	if (extensionId.empty() || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const auto builtIn = std::ranges::find(kBuiltInResources, extensionId, &BuiltInResource::id);
	if (builtIn == kBuiltInResources.end()) {
		return Terminal(EManagementOperationStatus::InvalidRequest,
			L"The built-in SENP extension is unavailable");
	}
	{
		std::lock_guard lock(m_mutex);
		const auto installed = std::ranges::find(m_snapshot.extensions,
			extensionId, &ExtensionDescriptor::id);
		const auto candidate = std::ranges::find(m_builtInCatalog,
			extensionId, &ExtensionDescriptor::id);
		if (installed != m_snapshot.extensions.end() && installed->installed
			&& candidate != m_builtInCatalog.end()
			&& installed->archiveSha256 == candidate->archiveSha256) {
			return { EManagementOperationStatus::AlreadyReady, m_snapshot };
		}
	}
	std::wstring diagnostic;
	const auto staged = StageBuiltInPackage(*builtIn, m_installRoot, diagnostic);
	if (!staged) return Terminal(EManagementOperationStatus::Failed, std::move(diagnostic));
	const auto installed = RunTool({
		L"install-builtin", staged->path.native(), m_installRoot, staged->hash,
	});
	std::error_code ignored;
	std::filesystem::remove(staged->path, ignored);
	if (!installed.launched || installed.timedOut || installed.exitCode != 0) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(installed, L"Built-in SENP package was rejected"));
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::UninstallBuiltInPackage(
	std::wstring_view extensionId)
{
	if (extensionId.empty() || IsStopped()) {
		return Terminal(IsStopped() ? EManagementOperationStatus::Stopped
			: EManagementOperationStatus::InvalidRequest);
	}
	const auto builtIn = std::ranges::find(kBuiltInResources, extensionId, &BuiltInResource::id);
	if (builtIn == kBuiltInResources.end()) {
		return Terminal(EManagementOperationStatus::InvalidRequest,
			L"The built-in SENP extension is unavailable");
	}
	std::wstring archiveSha256;
	{
		std::lock_guard lock(m_mutex);
		const auto installed = std::ranges::find(m_snapshot.extensions,
			extensionId, &ExtensionDescriptor::id);
		const auto candidate = std::ranges::find(m_builtInCatalog,
			extensionId, &ExtensionDescriptor::id);
		if (installed == m_snapshot.extensions.end() || !installed->installed) {
			if (std::ranges::find(m_uninstalledBuiltIns, extensionId)
				!= m_uninstalledBuiltIns.end()) {
				return { EManagementOperationStatus::AlreadyReady, m_snapshot };
			}
			return { EManagementOperationStatus::InvalidRequest, m_snapshot };
		}
		if (!installed->builtIn || installed->trust != L"builtin"
			|| candidate == m_builtInCatalog.end()
			|| installed->archiveSha256 != candidate->archiveSha256) {
			return { EManagementOperationStatus::InvalidRequest, m_snapshot };
		}
		archiveSha256 = installed->archiveSha256;
	}
	const auto uninstalled = RunTool({
		L"uninstall-builtin", m_installRoot, std::wstring(extensionId), archiveSha256,
	});
	if (!uninstalled.launched || uninstalled.timedOut || uninstalled.exitCode != 0) {
		return Terminal(EManagementOperationStatus::Failed,
			ProcessDiagnostic(uninstalled, L"Built-in SENP extension could not be uninstalled"));
	}
	return ReloadInstalled();
}

ManagementOperationResult CWin32SenpManagementService::Refresh()
{
	return ReloadInstalled();
}

void CWin32SenpManagementService::Stop() noexcept
{
	try {
		std::lock_guard lock(m_mutex);
		if (m_snapshot.state == EManagementState::Stopped) return;
		m_snapshot.state = EManagementState::Stopped;
		++m_snapshot.revision;
	} catch (...) {
	}
}

ManagementSnapshot CWin32SenpManagementService::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

} // namespace senp
