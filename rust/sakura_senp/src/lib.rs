//! SENP v1 package validation, deterministic packing, and immutable installation.

use ed25519_dalek::{Signature, VerifyingKey};
use serde::de::{self, DeserializeOwned, MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, Write};
use std::path::{Component, Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use zip::read::ZipArchive;
use zip::write::SimpleFileOptions;
use zip::{CompressionMethod, DateTime, ZipWriter};

pub const FORMAT_VERSION: u32 = 1;
pub const ABI: &str = "sakura:senp/extension@1.0.0";
pub const MANIFEST_PATH: &str = "senp.json";
pub const README_PATH: &str = "README.md";
pub const LICENSE_PATH: &str = "LICENSE";
pub const MODULE_PATH: &str = "module/extension.wasm";
pub const CHECKSUM_PATH: &str = "integrity/SHA256SUMS";
pub const SIGNATURE_PATH: &str = "signature/ed25519.sig";

const MAX_ARCHIVE_BYTES: u64 = 64 * 1024 * 1024;
const MAX_EXPANDED_BYTES: u64 = 64 * 1024 * 1024;
const MAX_ENTRY_BYTES: u64 = 32 * 1024 * 1024;
const MAX_DOCUMENT_BYTES: u64 = 4 * 1024 * 1024;
const MAX_ENTRIES: usize = 256;
const MAX_COMPRESSION_RATIO: u64 = 100;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct EngineCompatibility {
    pub sakura: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct RuntimeContract {
    pub abi: String,
    pub module: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct EditorDecorationContribution {
    pub id: String,
    pub kind: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct LanguageContribution {
    pub id: String,
    #[serde(default)]
    pub aliases: Vec<String>,
    #[serde(default)]
    pub extensions: Vec<String>,
    #[serde(default)]
    pub filenames: Vec<String>,
    #[serde(default)]
    pub filename_patterns: Vec<String>,
    #[serde(default)]
    pub mimetypes: Vec<String>,
    #[serde(default)]
    pub first_line: Option<String>,
    #[serde(default)]
    pub configuration: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct GrammarContribution {
    #[serde(default)]
    pub language: Option<String>,
    pub scope_name: String,
    pub path: String,
    #[serde(default)]
    pub inject_to: Vec<String>,
    #[serde(default)]
    pub embedded_languages: BTreeMap<String, String>,
    #[serde(default)]
    pub balanced_bracket_scopes: Vec<String>,
    #[serde(default)]
    pub unbalanced_bracket_scopes: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Contributions {
    #[serde(default)]
    pub editor_decorations: Vec<EditorDecorationContribution>,
    #[serde(default)]
    pub languages: Vec<LanguageContribution>,
    #[serde(default)]
    pub grammars: Vec<GrammarContribution>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Manifest {
    pub schema_version: u32,
    pub id: String,
    pub display_name: String,
    pub version: String,
    pub publisher: String,
    pub description: String,
    pub engines: EngineCompatibility,
    #[serde(default)]
    pub runtime: Option<RuntimeContract>,
    #[serde(default)]
    pub activation_events: Vec<String>,
    #[serde(default)]
    pub capabilities: Vec<String>,
    pub contributes: Contributions,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VerifiedPackage {
    pub manifest: Manifest,
    pub archive_sha256: String,
    pub signed: bool,
    pub entry_count: usize,
    pub expanded_bytes: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstalledExtension {
    pub manifest: Manifest,
    pub archive_sha256: String,
    pub enabled: bool,
    pub signed: bool,
    pub trust: String,
    pub readme: String,
    pub extension_path: PathBuf,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub module_path: Option<PathBuf>,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct ProfileState {
    schema_version: u32,
    id: String,
    archive_sha256: String,
    enabled: bool,
    signed: bool,
    trust: String,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct UninstalledState {
    schema_version: u32,
    id: String,
    archive_sha256: String,
    trust: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TrustPolicy {
    BuiltIn { expected_archive_sha256: String },
    PublisherKeys(BTreeMap<String, [u8; 32]>),
    DeveloperUnsigned,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum ErrorCode {
    Io,
    ArchiveTooLarge,
    InvalidZip,
    TooManyEntries,
    DuplicateEntry,
    UnsafePath,
    UnsupportedEntry,
    UnsupportedCompression,
    EntryTooLarge,
    ExpandedSizeLimit,
    CompressionRatioLimit,
    InvalidUtf8,
    MissingRequiredEntry,
    InvalidChecksumDocument,
    ChecksumMismatch,
    InvalidManifest,
    UnsupportedSchema,
    AbiMismatch,
    InvalidSignature,
    UnsignedPackage,
    UntrustedPublisher,
    ReparsePoint,
    PublicationFailed,
}

#[derive(Debug)]
pub struct SenpError {
    pub code: ErrorCode,
    pub detail: String,
}

impl SenpError {
    fn new(code: ErrorCode, detail: impl Into<String>) -> Self {
        Self {
            code,
            detail: detail.into(),
        }
    }
}

impl fmt::Display for SenpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.code, self.detail)
    }
}

impl std::error::Error for SenpError {}

fn io_error(context: &str, error: impl fmt::Display) -> SenpError {
    SenpError::new(ErrorCode::Io, format!("{context}: {error}"))
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        result.push(DIGITS[(byte >> 4) as usize] as char);
        result.push(DIGITS[(byte & 0x0f) as usize] as char);
    }
    result
}

pub fn decode_hex<const N: usize>(value: &str) -> Result<[u8; N], SenpError> {
    if value.len() != N * 2 {
        return Err(SenpError::new(
            ErrorCode::InvalidSignature,
            "wrong hex length",
        ));
    }
    let mut result = [0u8; N];
    for (index, pair) in value.as_bytes().chunks_exact(2).enumerate() {
        let digit = |value: u8| match value {
            b'0'..=b'9' => Some(value - b'0'),
            b'a'..=b'f' => Some(value - b'a' + 10),
            b'A'..=b'F' => Some(value - b'A' + 10),
            _ => None,
        };
        let high = digit(pair[0])
            .ok_or_else(|| SenpError::new(ErrorCode::InvalidSignature, "non-hex digit"))?;
        let low = digit(pair[1])
            .ok_or_else(|| SenpError::new(ErrorCode::InvalidSignature, "non-hex digit"))?;
        result[index] = (high << 4) | low;
    }
    Ok(result)
}

fn archive_digest(path: &Path) -> Result<String, SenpError> {
    let metadata = fs::metadata(path).map_err(|error| io_error("read package metadata", error))?;
    if metadata.len() > MAX_ARCHIVE_BYTES {
        return Err(SenpError::new(
            ErrorCode::ArchiveTooLarge,
            "archive exceeds 64 MiB",
        ));
    }
    let mut file = File::open(path).map_err(|error| io_error("open package", error))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|error| io_error("hash package", error))?;
        if read == 0 {
            break;
        }
        digest.update(&buffer[..read]);
    }
    Ok(hex(&digest.finalize()))
}

fn normalized_entry_name(raw: &[u8]) -> Result<String, SenpError> {
    let value = std::str::from_utf8(raw)
        .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, "ZIP entry name is not UTF-8"))?;
    if value.is_empty()
        || value.starts_with('/')
        || value.starts_with('\\')
        || value.contains('\\')
        || value.contains('\0')
        || value.contains(':')
        || value.ends_with('/')
    {
        return Err(SenpError::new(ErrorCode::UnsafePath, value));
    }
    let path = Path::new(value);
    for component in path.components() {
        match component {
            Component::Normal(part) if !part.is_empty() => {}
            _ => return Err(SenpError::new(ErrorCode::UnsafePath, value)),
        }
    }
    Ok(value.to_owned())
}

fn case_fold_path(value: &str) -> String {
    value.chars().flat_map(char::to_lowercase).collect()
}

fn is_document(path: &str) -> bool {
    matches!(
        path,
        MANIFEST_PATH
            | README_PATH
            | LICENSE_PATH
            | "CHANGELOG.md"
            | CHECKSUM_PATH
            | SIGNATURE_PATH
    )
}

fn is_allowed_entry(path: &str) -> bool {
    matches!(
        path,
        MANIFEST_PATH
            | README_PATH
            | LICENSE_PATH
            | MODULE_PATH
            | "CHANGELOG.md"
            | CHECKSUM_PATH
            | SIGNATURE_PATH
    ) || path.starts_with("assets/")
}

fn read_source_payload(source: &Path, relative: &str) -> Result<Vec<u8>, SenpError> {
    let path = source.join(relative);
    let metadata = fs::symlink_metadata(&path).map_err(|error| io_error(relative, error))?;
    if metadata.file_type().is_symlink() {
        return Err(SenpError::new(
            ErrorCode::ReparsePoint,
            path.display().to_string(),
        ));
    }
    if !metadata.is_file() {
        return Err(SenpError::new(ErrorCode::UnsupportedEntry, relative));
    }
    if metadata.len() > MAX_ENTRY_BYTES
        || (is_document(relative) && metadata.len() > MAX_DOCUMENT_BYTES)
    {
        return Err(SenpError::new(ErrorCode::EntryTooLarge, relative));
    }
    let bytes = fs::read(&path).map_err(|error| io_error(relative, error))?;
    if matches!(
        relative,
        MANIFEST_PATH | README_PATH | LICENSE_PATH | "CHANGELOG.md"
    ) && std::str::from_utf8(&bytes).is_err()
    {
        return Err(SenpError::new(ErrorCode::InvalidUtf8, relative));
    }
    Ok(bytes)
}

fn collect_asset_payloads(
    directory: &Path,
    relative: &Path,
    payloads: &mut BTreeMap<String, Vec<u8>>,
    depth: usize,
) -> Result<(), SenpError> {
    if depth > 16 {
        return Err(SenpError::new(
            ErrorCode::UnsafePath,
            "asset directory nesting exceeds 16 levels",
        ));
    }
    let mut children = fs::read_dir(directory)
        .map_err(|error| io_error("read assets directory", error))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| io_error("enumerate assets directory", error))?;
    children.sort_by_key(|entry| entry.file_name());
    for child in children {
        let metadata = child
            .file_type()
            .map_err(|error| io_error("inspect asset", error))?;
        if metadata.is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                child.path().display().to_string(),
            ));
        }
        let child_relative = relative.join(child.file_name());
        if metadata.is_dir() {
            collect_asset_payloads(&child.path(), &child_relative, payloads, depth + 1)?;
            continue;
        }
        if !metadata.is_file() {
            return Err(SenpError::new(
                ErrorCode::UnsupportedEntry,
                child.path().display().to_string(),
            ));
        }
        let archive_path = child_relative
            .components()
            .map(|component| match component {
                Component::Normal(value) => value.to_str().ok_or_else(|| {
                    SenpError::new(ErrorCode::InvalidUtf8, child.path().display().to_string())
                }),
                _ => Err(SenpError::new(
                    ErrorCode::UnsafePath,
                    child.path().display().to_string(),
                )),
            })
            .collect::<Result<Vec<_>, _>>()?
            .join("/");
        let archive_path = format!("assets/{archive_path}");
        normalized_entry_name(archive_path.as_bytes())?;
        if payloads.len() >= MAX_ENTRIES - 2 {
            return Err(SenpError::new(
                ErrorCode::TooManyEntries,
                MAX_ENTRIES.to_string(),
            ));
        }
        let bytes = fs::read(child.path()).map_err(|error| io_error("read asset", error))?;
        if bytes.len() as u64 > MAX_ENTRY_BYTES {
            return Err(SenpError::new(ErrorCode::EntryTooLarge, archive_path));
        }
        payloads.insert(archive_path, bytes);
    }
    Ok(())
}

fn valid_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value.bytes().all(|byte| {
            byte.is_ascii_lowercase() || byte.is_ascii_digit() || matches!(byte, b'.' | b'-')
        })
        && !value.starts_with(['.', '-'])
        && !value.ends_with(['.', '-'])
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn valid_scope_name(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'-' | b'_' | b'+'))
        && !value.starts_with(['.', '-'])
        && !value.ends_with(['.', '-'])
}

fn valid_asset_path(value: &str) -> bool {
    value.starts_with("assets/")
        && value.len() <= 1024
        && normalized_entry_name(value.as_bytes()).is_ok()
}

fn unique_bounded_strings(values: &[String], maximum: usize) -> bool {
    values.len() <= maximum
        && values
            .iter()
            .all(|value| !value.is_empty() && value.len() <= 1024)
        && values.iter().collect::<BTreeSet<_>>().len() == values.len()
}

fn validate_manifest(manifest: &Manifest) -> Result<(), SenpError> {
    if manifest.schema_version != FORMAT_VERSION {
        return Err(SenpError::new(
            ErrorCode::UnsupportedSchema,
            manifest.schema_version.to_string(),
        ));
    }
    if !valid_identifier(&manifest.id) || !valid_identifier(&manifest.publisher) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid id or publisher",
        ));
    }
    if manifest.display_name.trim().is_empty()
        || manifest.display_name.len() > 160
        || manifest.description.len() > 512
        || manifest.version.trim().is_empty()
        || manifest.engines.sakura.trim().is_empty()
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid display metadata",
        ));
    }
    if let Some(runtime) = &manifest.runtime {
        if runtime.abi != ABI || runtime.module != MODULE_PATH {
            return Err(SenpError::new(ErrorCode::AbiMismatch, runtime.abi.clone()));
        }
    }
    let activation_events: BTreeSet<_> = manifest
        .activation_events
        .iter()
        .map(String::as_str)
        .collect();
    if activation_events.len() != manifest.activation_events.len()
        || (manifest.runtime.is_some()
            && (activation_events.len() != 1 || !activation_events.contains("onStartupFinished")))
        || (manifest.runtime.is_none() && !activation_events.is_empty())
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "unknown or duplicate activation event",
        ));
    }
    let capabilities: BTreeSet<_> = manifest.capabilities.iter().map(String::as_str).collect();
    if capabilities.len() != manifest.capabilities.len()
        || manifest
            .capabilities
            .iter()
            .any(|value| !matches!(value.as_str(), "editor.visibleText" | "editor.decorations"))
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "unknown or duplicate capability",
        ));
    }
    if manifest.runtime.is_none() && !capabilities.is_empty() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "declarative extension cannot request runtime capabilities",
        ));
    }
    if manifest
        .contributes
        .editor_decorations
        .iter()
        .any(|value| value.kind != "indent" || !valid_identifier(&value.id))
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid editor decoration contribution",
        ));
    }
    let contribution_ids: BTreeSet<_> = manifest
        .contributes
        .editor_decorations
        .iter()
        .map(|value| value.id.as_str())
        .collect();
    if contribution_ids.len() != manifest.contributes.editor_decorations.len()
        || (!manifest.contributes.editor_decorations.is_empty()
            && (manifest.runtime.is_none()
                || !capabilities.contains("editor.visibleText")
                || !capabilities.contains("editor.decorations")))
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "decoration capabilities or contribution ids are invalid",
        ));
    }

    if manifest.contributes.languages.len() > 64 || manifest.contributes.grammars.len() > 128 {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "too many language or grammar contributions",
        ));
    }
    let mut language_ids = BTreeSet::new();
    for language in &manifest.contributes.languages {
        let has_selector = !language.extensions.is_empty()
            || !language.filenames.is_empty()
            || !language.filename_patterns.is_empty()
            || language.first_line.is_some();
        if !valid_identifier(&language.id)
            || !language_ids.insert(language.id.as_str())
            || !has_selector
            || !unique_bounded_strings(&language.aliases, 32)
            || !unique_bounded_strings(&language.extensions, 128)
            || !unique_bounded_strings(&language.filenames, 128)
            || !unique_bounded_strings(&language.filename_patterns, 128)
            || !unique_bounded_strings(&language.mimetypes, 32)
            || language
                .extensions
                .iter()
                .any(|value| !value.starts_with('.') || value.contains(['/', '\\', ':', '\0']))
            || language
                .filenames
                .iter()
                .any(|value| value.contains(['/', '\\', ':', '\0']))
            || language
                .first_line
                .as_ref()
                .is_some_and(|value| value.is_empty() || value.len() > 4096 || value.contains('\0'))
            || language
                .configuration
                .as_ref()
                .is_some_and(|value| !valid_asset_path(value))
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid language contribution",
            ));
        }
    }

    let mut grammar_scopes = BTreeSet::new();
    for grammar in &manifest.contributes.grammars {
        if !valid_scope_name(&grammar.scope_name)
            || !grammar_scopes.insert(grammar.scope_name.as_str())
            || !valid_asset_path(&grammar.path)
            || grammar
                .language
                .as_ref()
                .is_some_and(|value| !language_ids.contains(value.as_str()))
            || !unique_bounded_strings(&grammar.inject_to, 64)
            || grammar
                .inject_to
                .iter()
                .any(|value| !valid_scope_name(value))
            || grammar
                .embedded_languages
                .iter()
                .any(|(scope, language)| !valid_scope_name(scope) || !valid_identifier(language))
            || !unique_bounded_strings(&grammar.balanced_bracket_scopes, 64)
            || !unique_bounded_strings(&grammar.unbalanced_bracket_scopes, 64)
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid grammar contribution",
            ));
        }
    }
    if manifest.contributes.languages.is_empty() != manifest.contributes.grammars.is_empty() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "language and grammar contributions must be paired",
        ));
    }
    if manifest.contributes.editor_decorations.is_empty()
        && manifest.contributes.languages.is_empty()
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "extension contributes no supported capability",
        ));
    }
    Ok(())
}

fn validate_manifest_entries(
    manifest: &Manifest,
    entries: &BTreeMap<String, Vec<u8>>,
) -> Result<(), SenpError> {
    let mut declared_assets = manifest
        .contributes
        .grammars
        .iter()
        .map(|grammar| grammar.path.as_str())
        .collect::<BTreeSet<_>>();
    declared_assets.extend(
        manifest
            .contributes
            .languages
            .iter()
            .filter_map(|language| language.configuration.as_deref()),
    );
    if let Some(missing) = declared_assets
        .into_iter()
        .find(|path| !entries.contains_key(*path))
    {
        return Err(SenpError::new(ErrorCode::MissingRequiredEntry, missing));
    }
    if manifest.runtime.is_some() && !entries.contains_key(MODULE_PATH) {
        return Err(SenpError::new(ErrorCode::MissingRequiredEntry, MODULE_PATH));
    }
    if manifest.runtime.is_none() && entries.contains_key(MODULE_PATH) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "declarative extension unexpectedly contains a runtime module",
        ));
    }
    Ok(())
}

#[derive(Debug)]
struct NoDuplicateJson(serde_json::Value);

impl<'de> Deserialize<'de> for NoDuplicateJson {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct JsonVisitor;
        impl<'de> Visitor<'de> for JsonVisitor {
            type Value = NoDuplicateJson;
            fn expecting(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
                formatter.write_str("JSON value")
            }
            fn visit_bool<E: de::Error>(self, value: bool) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_i64<E: de::Error>(self, value: i64) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_u64<E: de::Error>(self, value: u64) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_f64<E: de::Error>(self, value: f64) -> Result<Self::Value, E> {
                serde_json::Number::from_f64(value)
                    .map(|value| NoDuplicateJson(value.into()))
                    .ok_or_else(|| E::custom("non-finite number"))
            }
            fn visit_str<E: de::Error>(self, value: &str) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_string<E: de::Error>(self, value: String) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(value.into()))
            }
            fn visit_none<E: de::Error>(self) -> Result<Self::Value, E> {
                Ok(NoDuplicateJson(serde_json::Value::Null))
            }
            fn visit_unit<E: de::Error>(self) -> Result<Self::Value, E> {
                self.visit_none()
            }
            fn visit_seq<A: SeqAccess<'de>>(
                self,
                mut sequence: A,
            ) -> Result<Self::Value, A::Error> {
                let mut values = Vec::new();
                while let Some(value) = sequence.next_element::<NoDuplicateJson>()? {
                    values.push(value.0);
                }
                Ok(NoDuplicateJson(values.into()))
            }
            fn visit_map<A: MapAccess<'de>>(self, mut map: A) -> Result<Self::Value, A::Error> {
                let mut values = serde_json::Map::new();
                while let Some((key, value)) = map.next_entry::<String, NoDuplicateJson>()? {
                    if values.insert(key.clone(), value.0).is_some() {
                        return Err(de::Error::custom(format!("duplicate member: {key}")));
                    }
                }
                Ok(NoDuplicateJson(values.into()))
            }
        }
        deserializer.deserialize_any(JsonVisitor)
    }
}

fn strict_json<T: DeserializeOwned>(bytes: &[u8]) -> Result<T, SenpError> {
    let mut deserializer = serde_json::Deserializer::from_slice(bytes);
    let value = NoDuplicateJson::deserialize(&mut deserializer)
        .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))?;
    deserializer
        .end()
        .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))?;
    serde_json::from_value(value.0)
        .map_err(|error| SenpError::new(ErrorCode::InvalidManifest, error.to_string()))
}

fn read_zip_entries<R: Read + Seek>(
    archive: &mut ZipArchive<R>,
) -> Result<BTreeMap<String, Vec<u8>>, SenpError> {
    if archive.len() > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            archive.len().to_string(),
        ));
    }
    let mut entries = BTreeMap::new();
    let mut folded = BTreeSet::new();
    let mut expanded = 0u64;
    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        let name = normalized_entry_name(entry.name_raw())?;
        if !is_allowed_entry(&name) {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, name));
        }
        if entry.is_dir()
            || entry
                .unix_mode()
                .is_some_and(|mode| mode & 0o170000 != 0 && mode & 0o170000 != 0o100000)
        {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, name));
        }
        if !matches!(
            entry.compression(),
            CompressionMethod::Stored | CompressionMethod::Deflated
        ) {
            return Err(SenpError::new(ErrorCode::UnsupportedCompression, name));
        }
        if entry.size() > MAX_ENTRY_BYTES
            || (is_document(&name) && entry.size() > MAX_DOCUMENT_BYTES)
        {
            return Err(SenpError::new(ErrorCode::EntryTooLarge, name));
        }
        expanded = expanded
            .checked_add(entry.size())
            .ok_or_else(|| SenpError::new(ErrorCode::ExpandedSizeLimit, name.clone()))?;
        if expanded > MAX_EXPANDED_BYTES {
            return Err(SenpError::new(
                ErrorCode::ExpandedSizeLimit,
                expanded.to_string(),
            ));
        }
        if entry.size() > 0
            && (entry.compressed_size() == 0
                || entry.size() / entry.compressed_size().max(1) > MAX_COMPRESSION_RATIO)
        {
            return Err(SenpError::new(ErrorCode::CompressionRatioLimit, name));
        }
        if !folded.insert(case_fold_path(&name)) || entries.contains_key(&name) {
            return Err(SenpError::new(ErrorCode::DuplicateEntry, name));
        }
        let mut bytes = Vec::with_capacity(entry.size() as usize);
        entry
            .read_to_end(&mut bytes)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        if bytes.len() as u64 != entry.size() {
            return Err(SenpError::new(
                ErrorCode::InvalidZip,
                "uncompressed size mismatch",
            ));
        }
        entries.insert(name, bytes);
    }
    Ok(entries)
}

fn parse_checksums(bytes: &[u8]) -> Result<BTreeMap<String, String>, SenpError> {
    let text = std::str::from_utf8(bytes)
        .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, CHECKSUM_PATH))?;
    if text.is_empty() || !text.ends_with('\n') || text.contains('\r') {
        return Err(SenpError::new(
            ErrorCode::InvalidChecksumDocument,
            "non-canonical line endings",
        ));
    }
    let mut checksums = BTreeMap::new();
    let mut previous: Option<&str> = None;
    for line in text.lines() {
        let (digest, path) = line
            .split_once("  ")
            .ok_or_else(|| SenpError::new(ErrorCode::InvalidChecksumDocument, line))?;
        if digest.len() != 64
            || !digest
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(SenpError::new(
                ErrorCode::InvalidChecksumDocument,
                "invalid SHA-256",
            ));
        }
        normalized_entry_name(path.as_bytes())?;
        if previous.is_some_and(|value| value >= path)
            || checksums
                .insert(path.to_owned(), digest.to_owned())
                .is_some()
        {
            return Err(SenpError::new(
                ErrorCode::InvalidChecksumDocument,
                "entries are not unique and sorted",
            ));
        }
        previous = Some(path);
    }
    Ok(checksums)
}

pub fn verify_package(path: &Path, policy: &TrustPolicy) -> Result<VerifiedPackage, SenpError> {
    let archive_sha256 = archive_digest(path)?;
    if let TrustPolicy::BuiltIn {
        expected_archive_sha256,
    } = policy
    {
        if expected_archive_sha256 != &archive_sha256 {
            return Err(SenpError::new(
                ErrorCode::ChecksumMismatch,
                "built-in package pin mismatch",
            ));
        }
    }
    let file = File::open(path).map_err(|error| io_error("open package", error))?;
    let mut archive = ZipArchive::new(file)
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    let entries = read_zip_entries(&mut archive)?;
    for required in [MANIFEST_PATH, README_PATH, LICENSE_PATH, CHECKSUM_PATH] {
        if !entries.contains_key(required) {
            return Err(SenpError::new(ErrorCode::MissingRequiredEntry, required));
        }
    }
    for path in [MANIFEST_PATH, README_PATH, LICENSE_PATH] {
        std::str::from_utf8(&entries[path])
            .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, path))?;
    }
    let checksums = parse_checksums(&entries[CHECKSUM_PATH])?;
    let payload_paths: BTreeSet<_> = entries
        .keys()
        .filter(|path| path.as_str() != CHECKSUM_PATH && path.as_str() != SIGNATURE_PATH)
        .cloned()
        .collect();
    if payload_paths != checksums.keys().cloned().collect() {
        return Err(SenpError::new(
            ErrorCode::InvalidChecksumDocument,
            "checksum coverage mismatch",
        ));
    }
    for (path, expected) in &checksums {
        if hex(&Sha256::digest(&entries[path])) != *expected {
            return Err(SenpError::new(ErrorCode::ChecksumMismatch, path));
        }
    }
    let manifest: Manifest = strict_json(&entries[MANIFEST_PATH])?;
    validate_manifest(&manifest)?;
    validate_manifest_entries(&manifest, &entries)?;

    let signed = if let Some(signature_bytes) = entries.get(SIGNATURE_PATH) {
        let signature_text = std::str::from_utf8(signature_bytes)
            .map_err(|_| SenpError::new(ErrorCode::InvalidSignature, "signature is not UTF-8"))?;
        let signature =
            Signature::from_bytes(&decode_hex::<64>(signature_text.trim_end_matches('\n'))?);
        let key = match policy {
            TrustPolicy::PublisherKeys(keys) => keys.get(&manifest.publisher).ok_or_else(|| {
                SenpError::new(ErrorCode::UntrustedPublisher, &manifest.publisher)
            })?,
            TrustPolicy::BuiltIn { .. } | TrustPolicy::DeveloperUnsigned => {
                return Err(SenpError::new(
                    ErrorCode::UntrustedPublisher,
                    "no publisher key supplied",
                ));
            }
        };
        let verifying_key = VerifyingKey::from_bytes(key)
            .map_err(|error| SenpError::new(ErrorCode::InvalidSignature, error.to_string()))?;
        verifying_key
            .verify_strict(&entries[CHECKSUM_PATH], &signature)
            .map_err(|error| SenpError::new(ErrorCode::InvalidSignature, error.to_string()))?;
        true
    } else {
        if matches!(policy, TrustPolicy::PublisherKeys(_)) {
            return Err(SenpError::new(
                ErrorCode::UnsignedPackage,
                "publisher package has no signature",
            ));
        }
        false
    };
    Ok(VerifiedPackage {
        manifest,
        archive_sha256,
        signed,
        entry_count: entries.len(),
        expanded_bytes: entries.values().map(|value| value.len() as u64).sum(),
    })
}

pub fn pack_directory(
    source: &Path,
    destination: &Path,
    signing_key: Option<[u8; 32]>,
) -> Result<String, SenpError> {
    let mut payloads = BTreeMap::new();
    for required in [MANIFEST_PATH, README_PATH, LICENSE_PATH] {
        payloads.insert(required.to_owned(), read_source_payload(source, required)?);
    }
    let manifest: Manifest = strict_json(&payloads[MANIFEST_PATH])?;
    validate_manifest(&manifest)?;
    if manifest.runtime.is_some() {
        payloads.insert(
            MODULE_PATH.to_owned(),
            read_source_payload(source, MODULE_PATH)?,
        );
    } else if source.join(MODULE_PATH).exists() {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "declarative extension unexpectedly contains a runtime module",
        ));
    }
    for optional in ["CHANGELOG.md"] {
        let path = source.join(optional);
        if path.exists() {
            payloads.insert(optional.to_owned(), read_source_payload(source, optional)?);
        }
    }
    let assets = source.join("assets");
    if assets.exists() {
        let metadata = fs::symlink_metadata(&assets)
            .map_err(|error| io_error("inspect assets directory", error))?;
        if metadata.file_type().is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                assets.display().to_string(),
            ));
        }
        if !metadata.is_dir() {
            return Err(SenpError::new(ErrorCode::UnsupportedEntry, "assets"));
        }
        collect_asset_payloads(&assets, Path::new(""), &mut payloads, 0)?;
    }
    if payloads.len() + 2 > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            payloads.len().to_string(),
        ));
    }
    let expanded_bytes = payloads.iter().try_fold(0u64, |total, (path, bytes)| {
        total
            .checked_add(bytes.len() as u64)
            .filter(|value| *value <= MAX_EXPANDED_BYTES)
            .ok_or_else(|| SenpError::new(ErrorCode::ExpandedSizeLimit, path))
    })?;
    if expanded_bytes > MAX_EXPANDED_BYTES {
        return Err(SenpError::new(
            ErrorCode::ExpandedSizeLimit,
            expanded_bytes.to_string(),
        ));
    }
    validate_manifest_entries(&manifest, &payloads)?;
    let mut checksum_document = String::new();
    for (path, bytes) in &payloads {
        checksum_document.push_str(&hex(&Sha256::digest(bytes)));
        checksum_document.push_str("  ");
        checksum_document.push_str(path);
        checksum_document.push('\n');
    }
    let signature = signing_key.map(|key| {
        use ed25519_dalek::{Signer, SigningKey};
        let signing_key = SigningKey::from_bytes(&key);
        format!(
            "{}\n",
            hex(&signing_key.sign(checksum_document.as_bytes()).to_bytes())
        )
    });
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent).map_err(|error| io_error("create package directory", error))?;
    }
    let file = File::create(destination).map_err(|error| io_error("create package", error))?;
    let mut writer = ZipWriter::new(file);
    let options = SimpleFileOptions::default()
        .compression_method(CompressionMethod::Stored)
        .last_modified_time(DateTime::default())
        .unix_permissions(0o100644);
    for (path, bytes) in &payloads {
        writer
            .start_file(path, options)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        writer
            .write_all(bytes)
            .map_err(|error| io_error("write package", error))?;
    }
    writer
        .start_file(CHECKSUM_PATH, options)
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    writer
        .write_all(checksum_document.as_bytes())
        .map_err(|error| io_error("write checksums", error))?;
    if let Some(signature) = signature {
        writer
            .start_file(SIGNATURE_PATH, options)
            .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
        writer
            .write_all(signature.as_bytes())
            .map_err(|error| io_error("write signature", error))?;
    }
    writer
        .finish()
        .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
    archive_digest(destination)
}

fn reject_reparse_points(path: &Path) -> Result<(), SenpError> {
    let mut current = PathBuf::new();
    for component in path.components() {
        current.push(component.as_os_str());
        if !current.exists() {
            continue;
        }
        let metadata = fs::symlink_metadata(&current)
            .map_err(|error| io_error("inspect install path", error))?;
        if metadata.file_type().is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                current.display().to_string(),
            ));
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if metadata.file_attributes() & 0x400 != 0 {
                return Err(SenpError::new(
                    ErrorCode::ReparsePoint,
                    current.display().to_string(),
                ));
            }
        }
    }
    Ok(())
}

fn publish_atomic(temporary: &Path, target: &Path) -> Result<(), SenpError> {
    #[cfg(windows)]
    {
        use std::os::windows::ffi::OsStrExt;
        const MOVEFILE_REPLACE_EXISTING: u32 = 0x1;
        const MOVEFILE_WRITE_THROUGH: u32 = 0x8;
        #[link(name = "kernel32")]
        unsafe extern "system" {
            fn MoveFileExW(existing: *const u16, replacement: *const u16, flags: u32) -> i32;
        }
        let from: Vec<u16> = temporary.as_os_str().encode_wide().chain(Some(0)).collect();
        let to: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
        if unsafe {
            MoveFileExW(
                from.as_ptr(),
                to.as_ptr(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
            )
        } == 0
        {
            return Err(SenpError::new(
                ErrorCode::PublicationFailed,
                std::io::Error::last_os_error().to_string(),
            ));
        }
    }
    #[cfg(not(windows))]
    {
        if target.exists() {
            fs::remove_file(target).map_err(|error| io_error("replace profile state", error))?;
        }
        fs::rename(temporary, target).map_err(|error| io_error("publish profile state", error))?;
    }
    Ok(())
}

fn temporary_suffix() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| duration.as_nanos())
}

fn write_profile_state(root: &Path, state: &ProfileState) -> Result<(), SenpError> {
    let state_root = root.join("profiles");
    fs::create_dir_all(&state_root).map_err(|error| io_error("create state root", error))?;
    let state_bytes = serde_json::to_vec_pretty(state)
        .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
    let target = state_root.join(format!("{}.json", state.id));
    let temporary = state_root.join(format!(
        ".{}.{}.{}.tmp",
        state.id,
        std::process::id(),
        temporary_suffix()
    ));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .map_err(|error| io_error("create profile state", error))?;
    let result = (|| {
        file.write_all(&state_bytes)
            .map_err(|error| io_error("write profile state", error))?;
        file.sync_all()
            .map_err(|error| io_error("flush profile state", error))?;
        drop(file);
        publish_atomic(&temporary, &target)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn write_uninstalled_state(root: &Path, state: &UninstalledState) -> Result<(), SenpError> {
    let state_root = root.join("uninstalled");
    fs::create_dir_all(&state_root)
        .map_err(|error| io_error("create uninstalled state root", error))?;
    let state_bytes = serde_json::to_vec_pretty(state)
        .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
    let target = state_root.join(format!("{}.json", state.id));
    let temporary = state_root.join(format!(
        ".{}.{}.{}.tmp",
        state.id,
        std::process::id(),
        temporary_suffix()
    ));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .map_err(|error| io_error("create uninstalled state", error))?;
    let result = (|| {
        file.write_all(&state_bytes)
            .map_err(|error| io_error("write uninstalled state", error))?;
        file.sync_all()
            .map_err(|error| io_error("flush uninstalled state", error))?;
        drop(file);
        publish_atomic(&temporary, &target)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn read_uninstalled_states(root: &Path) -> Result<BTreeMap<String, UninstalledState>, SenpError> {
    let state_root = root.join("uninstalled");
    if !state_root.exists() {
        return Ok(BTreeMap::new());
    }
    reject_reparse_points(&state_root)?;
    let mut entries = fs::read_dir(&state_root)
        .map_err(|error| io_error("read uninstalled state root", error))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| io_error("enumerate uninstalled state root", error))?;
    entries.sort_by_key(|entry| entry.file_name());
    if entries.len() > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            entries.len().to_string(),
        ));
    }
    let mut states = BTreeMap::new();
    for entry in entries {
        let file_type = entry
            .file_type()
            .map_err(|error| io_error("inspect uninstalled state", error))?;
        if file_type.is_symlink() {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                entry.path().display().to_string(),
            ));
        }
        if !file_type.is_file()
            || entry.path().extension().and_then(|value| value.to_str()) != Some("json")
            || entry.file_name().to_string_lossy().starts_with('.')
        {
            continue;
        }
        let bytes =
            fs::read(entry.path()).map_err(|error| io_error("read uninstalled state", error))?;
        if bytes.len() as u64 > MAX_DOCUMENT_BYTES {
            return Err(SenpError::new(
                ErrorCode::EntryTooLarge,
                entry.path().display().to_string(),
            ));
        }
        let state: UninstalledState = strict_json(&bytes)?;
        let entry_path = entry.path();
        let filename = entry_path.file_stem().and_then(|value| value.to_str());
        if state.schema_version != 1
            || !valid_identifier(&state.id)
            || filename != Some(state.id.as_str())
            || !valid_sha256(&state.archive_sha256)
            || state.trust != "builtin"
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid uninstalled extension state",
            ));
        }
        if states.insert(state.id.clone(), state).is_some() {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "duplicate uninstalled extension state",
            ));
        }
    }
    Ok(states)
}

fn previous_enabled_state(root: &Path, id: &str, trust: &str) -> Result<Option<bool>, SenpError> {
    let path = root.join("profiles").join(format!("{id}.json"));
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(io_error("read previous profile state", error)),
    };
    if bytes.len() as u64 > MAX_DOCUMENT_BYTES {
        return Err(SenpError::new(
            ErrorCode::EntryTooLarge,
            path.display().to_string(),
        ));
    }
    let state: ProfileState = strict_json(&bytes)?;
    if state.schema_version != 1 || state.id != id || !valid_identifier(&state.id) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "previous profile state does not match the extension",
        ));
    }
    Ok((state.trust == trust).then_some(state.enabled))
}

pub fn install_package(
    path: &Path,
    root: &Path,
    policy: &TrustPolicy,
) -> Result<VerifiedPackage, SenpError> {
    let verified = verify_package(path, policy)?;
    reject_reparse_points(root)?;
    fs::create_dir_all(root).map_err(|error| io_error("create install root", error))?;
    reject_reparse_points(root)?;
    let content_root = root.join("content");
    let state_root = root.join("profiles");
    fs::create_dir_all(&content_root).map_err(|error| io_error("create content root", error))?;
    fs::create_dir_all(&state_root).map_err(|error| io_error("create state root", error))?;
    let destination = content_root.join(&verified.archive_sha256);
    if !destination.exists() {
        let staging = content_root.join(format!(
            ".{}.{}.{}.tmp",
            verified.archive_sha256,
            std::process::id(),
            temporary_suffix()
        ));
        fs::create_dir(&staging).map_err(|error| io_error("create staging directory", error))?;
        let install_result = (|| {
            let file = File::open(path).map_err(|error| io_error("open package", error))?;
            let mut archive = ZipArchive::new(file)
                .map_err(|error| SenpError::new(ErrorCode::InvalidZip, error.to_string()))?;
            let entries = read_zip_entries(&mut archive)?;
            for (entry_path, bytes) in entries {
                let output = staging.join(&entry_path);
                if let Some(parent) = output.parent() {
                    fs::create_dir_all(parent)
                        .map_err(|error| io_error("create package directory", error))?;
                }
                let mut output_file = OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&output)
                    .map_err(|error| io_error("create package entry", error))?;
                output_file
                    .write_all(&bytes)
                    .map_err(|error| io_error("write package entry", error))?;
                output_file
                    .sync_all()
                    .map_err(|error| io_error("flush package entry", error))?;
            }
            fs::write(
                staging.join(".senp-archive-sha256"),
                format!("{}\n", verified.archive_sha256),
            )
            .map_err(|error| io_error("write package marker", error))?;
            fs::rename(&staging, &destination)
                .or_else(|error| {
                    if destination.is_dir()
                        && fs::read_to_string(destination.join(".senp-archive-sha256"))
                            .is_ok_and(|value| value.trim_end() == verified.archive_sha256)
                    {
                        fs::remove_dir_all(&staging)?;
                        Ok(())
                    } else {
                        Err(error)
                    }
                })
                .map_err(|error| SenpError::new(ErrorCode::PublicationFailed, error.to_string()))?;
            Ok::<(), SenpError>(())
        })();
        if install_result.is_err() {
            let _ = fs::remove_dir_all(&staging);
            install_result?;
        }
    }
    let developer_unsigned = matches!(policy, TrustPolicy::DeveloperUnsigned) && !verified.signed;
    let trust = match policy {
        TrustPolicy::BuiltIn { .. } => "builtin",
        TrustPolicy::PublisherKeys(_) => "publisher",
        TrustPolicy::DeveloperUnsigned => "developer",
    };
    let enabled = if developer_unsigned {
        false
    } else {
        previous_enabled_state(root, &verified.manifest.id, trust)?.unwrap_or(true)
    };
    write_profile_state(
        root,
        &ProfileState {
            schema_version: 1,
            id: verified.manifest.id.clone(),
            archive_sha256: verified.archive_sha256.clone(),
            enabled,
            signed: verified.signed,
            trust: trust.to_owned(),
        },
    )?;
    let uninstalled_path = root
        .join("uninstalled")
        .join(format!("{}.json", verified.manifest.id));
    match fs::remove_file(&uninstalled_path) {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(io_error("clear uninstalled state", error)),
    }
    Ok(verified)
}

pub fn list_installed(root: &Path) -> Result<Vec<InstalledExtension>, SenpError> {
    reject_reparse_points(root)?;
    let uninstalled = read_uninstalled_states(root)?;
    let state_root = root.join("profiles");
    if !state_root.exists() {
        return Ok(Vec::new());
    }
    let mut states = fs::read_dir(&state_root)
        .map_err(|error| io_error("read state root", error))?
        .collect::<Result<Vec<_>, _>>()
        .map_err(|error| io_error("enumerate state root", error))?;
    states.sort_by_key(|entry| entry.file_name());
    if states.len() > MAX_ENTRIES {
        return Err(SenpError::new(
            ErrorCode::TooManyEntries,
            states.len().to_string(),
        ));
    }
    let mut installed = Vec::new();
    for entry in states {
        if entry
            .file_type()
            .map_err(|error| io_error("inspect state", error))?
            .is_symlink()
        {
            return Err(SenpError::new(
                ErrorCode::ReparsePoint,
                entry.path().display().to_string(),
            ));
        }
        if !entry
            .file_type()
            .map_err(|error| io_error("inspect state", error))?
            .is_file()
            || entry.path().extension().and_then(|value| value.to_str()) != Some("json")
            || entry.file_name().to_string_lossy().starts_with('.')
        {
            continue;
        }
        let state_bytes = fs::read(entry.path()).map_err(|error| io_error("read state", error))?;
        if state_bytes.len() as u64 > MAX_DOCUMENT_BYTES {
            return Err(SenpError::new(
                ErrorCode::EntryTooLarge,
                entry.path().display().to_string(),
            ));
        }
        let state: ProfileState = strict_json(&state_bytes)?;
        if state.schema_version != 1
            || !valid_identifier(&state.id)
            || !valid_sha256(&state.archive_sha256)
            || !matches!(state.trust.as_str(), "builtin" | "publisher" | "developer")
        {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "invalid profile extension state",
            ));
        }
        if uninstalled
            .get(&state.id)
            .is_some_and(|removed| removed.trust == state.trust)
        {
            continue;
        }
        let content = root.join("content").join(&state.archive_sha256);
        reject_reparse_points(&content)?;
        let marker = fs::read_to_string(content.join(".senp-archive-sha256"))
            .map_err(|error| io_error("read content marker", error))?;
        if marker.trim_end() != state.archive_sha256 {
            return Err(SenpError::new(
                ErrorCode::ChecksumMismatch,
                "content marker mismatch",
            ));
        }
        let manifest_bytes = fs::read(content.join(MANIFEST_PATH))
            .map_err(|error| io_error("read installed manifest", error))?;
        let manifest: Manifest = strict_json(&manifest_bytes)?;
        validate_manifest(&manifest)?;
        if manifest.id != state.id {
            return Err(SenpError::new(
                ErrorCode::InvalidManifest,
                "state and manifest ids differ",
            ));
        }
        let readme_bytes = fs::read(content.join(README_PATH))
            .map_err(|error| io_error("read installed README", error))?;
        if readme_bytes.len() as u64 > MAX_DOCUMENT_BYTES {
            return Err(SenpError::new(ErrorCode::EntryTooLarge, README_PATH));
        }
        let readme = String::from_utf8(readme_bytes)
            .map_err(|_| SenpError::new(ErrorCode::InvalidUtf8, README_PATH))?;
        installed.push(InstalledExtension {
            module_path: manifest.runtime.as_ref().map(|_| content.join(MODULE_PATH)),
            manifest,
            archive_sha256: state.archive_sha256,
            enabled: state.enabled,
            signed: state.signed,
            trust: state.trust,
            readme,
            extension_path: content,
        });
    }
    installed.sort_by(|left, right| left.manifest.id.cmp(&right.manifest.id));
    Ok(installed)
}

pub fn list_uninstalled(root: &Path) -> Result<Vec<String>, SenpError> {
    reject_reparse_points(root)?;
    Ok(read_uninstalled_states(root)?.into_keys().collect())
}

pub fn uninstall_built_in(
    root: &Path,
    id: &str,
    expected_archive_sha256: &str,
) -> Result<(), SenpError> {
    if !valid_identifier(id) || !valid_sha256(expected_archive_sha256) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid built-in uninstall request",
        ));
    }
    reject_reparse_points(root)?;
    let state_path = root.join("profiles").join(format!("{id}.json"));
    reject_reparse_points(&state_path)?;
    let bytes = match fs::read(&state_path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            let uninstalled = read_uninstalled_states(root)?;
            return if uninstalled.contains_key(id) {
                Ok(())
            } else {
                Err(io_error("read profile state", error))
            };
        }
        Err(error) => return Err(io_error("read profile state", error)),
    };
    if bytes.len() as u64 > MAX_DOCUMENT_BYTES {
        return Err(SenpError::new(
            ErrorCode::EntryTooLarge,
            state_path.display().to_string(),
        ));
    }
    let state: ProfileState = strict_json(&bytes)?;
    if state.schema_version != 1
        || state.id != id
        || state.trust != "builtin"
        || state.archive_sha256 != expected_archive_sha256
    {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "installed state does not match the built-in uninstall request",
        ));
    }
    write_uninstalled_state(
        root,
        &UninstalledState {
            schema_version: 1,
            id: id.to_owned(),
            archive_sha256: expected_archive_sha256.to_owned(),
            trust: "builtin".to_owned(),
        },
    )?;
    match fs::remove_file(&state_path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        // The published tombstone is authoritative even if stale profile
        // cleanup is delayed. list_installed continues to fail closed.
        Err(_) => Ok(()),
    }
}

pub fn set_extension_enabled(root: &Path, id: &str, enabled: bool) -> Result<(), SenpError> {
    if !valid_identifier(id) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "invalid extension id",
        ));
    }
    let state_path = root.join("profiles").join(format!("{id}.json"));
    let bytes = fs::read(&state_path).map_err(|error| io_error("read profile state", error))?;
    let mut state: ProfileState = strict_json(&bytes)?;
    if state.id != id || state.schema_version != 1 {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "profile state id mismatch",
        ));
    }
    if read_uninstalled_states(root)?.contains_key(id) {
        return Err(SenpError::new(
            ErrorCode::InvalidManifest,
            "uninstalled extension cannot be enabled",
        ));
    }
    state.enabled = enabled;
    write_profile_state(root, &state)
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    fn fixture(root: &Path) {
        fs::create_dir_all(root.join("module")).unwrap();
        fs::create_dir_all(root.join("assets/icons")).unwrap();
        fs::write(
            root.join(MANIFEST_PATH),
            br#"{
  "schemaVersion": 1,
  "id": "sample-indent",
  "displayName": "Sample Indent",
  "version": "0.1.0",
  "publisher": "sakura.builtin",
  "description": "Fixture",
  "engines": { "sakura": ">=0.0.0" },
  "runtime": { "abi": "sakura:senp/extension@1.0.0", "module": "module/extension.wasm" },
  "activationEvents": ["onStartupFinished"],
  "capabilities": ["editor.visibleText", "editor.decorations"],
  "contributes": { "editorDecorations": [{ "id": "sample.indent", "kind": "indent" }] }
}"#,
        )
        .unwrap();
        fs::write(root.join(README_PATH), "# Sample\n").unwrap();
        fs::write(root.join(LICENSE_PATH), "Zlib\n").unwrap();
        fs::write(root.join(MODULE_PATH), b"wasm").unwrap();
        fs::write(root.join("assets/icons/sample.txt"), b"asset").unwrap();
    }

    fn grammar_fixture(root: &Path) {
        fs::create_dir_all(root.join("assets/syntaxes")).unwrap();
        fs::write(
            root.join(MANIFEST_PATH),
            br#"{
  "schemaVersion": 1,
  "id": "sample-shell-language-basics",
  "displayName": "Shell Language Basics",
  "version": "0.1.0",
  "publisher": "sakura.builtin",
  "description": "Fixture",
  "engines": { "sakura": ">=0.0.0" },
  "contributes": {
    "languages": [{
      "id": "shellscript",
      "aliases": ["Shell Script", "shellscript"],
      "extensions": [".sh", ".bash", ".zsh"],
      "filenames": [".bashrc", ".zshrc"],
      "firstLine": "^#!.*\\b(?:ba|z)?sh\\b"
    }],
    "grammars": [{
      "language": "shellscript",
      "scopeName": "source.shell",
      "path": "assets/syntaxes/shell-unix-bash.tmLanguage.json"
    }]
  }
}"#,
        )
        .unwrap();
        fs::write(root.join(README_PATH), "# Shell Language Basics\n").unwrap();
        fs::write(root.join(LICENSE_PATH), "MIT\n").unwrap();
        fs::write(
            root.join("assets/syntaxes/shell-unix-bash.tmLanguage.json"),
            br#"{"scopeName":"source.shell","patterns":[]}"#,
        )
        .unwrap();
    }

    #[test]
    fn deterministic_package_round_trips_and_installs_immutably() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let first = temp.path().join("first.senp");
        let second = temp.path().join("second.senp");
        let first_hash = pack_directory(&source, &first, None).unwrap();
        let second_hash = pack_directory(&source, &second, None).unwrap();
        assert_eq!(first_hash, second_hash);
        assert_eq!(fs::read(&first).unwrap(), fs::read(&second).unwrap());
        let verified = verify_package(
            &first,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: first_hash.clone(),
            },
        )
        .unwrap();
        assert_eq!(verified.manifest.id, "sample-indent");
        assert!(!verified.signed);
        let installed = install_package(
            &first,
            &temp.path().join("installed"),
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: first_hash,
            },
        )
        .unwrap();
        assert!(temp
            .path()
            .join("installed/content")
            .join(&installed.archive_sha256)
            .join(MODULE_PATH)
            .is_file());
        let install_root = temp.path().join("installed");
        let listed = list_installed(&install_root).unwrap();
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].manifest.id, "sample-indent");
        assert_eq!(listed[0].readme, "# Sample\n");
        assert_eq!(listed[0].trust, "builtin");
        assert!(listed[0].enabled);
        assert!(listed[0]
            .module_path
            .as_ref()
            .unwrap()
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("assets/icons/sample.txt")
            .is_file());
        set_extension_enabled(&install_root, "sample-indent", false).unwrap();
        assert!(!list_installed(&install_root).unwrap()[0].enabled);
        install_package(
            &first,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: installed.archive_sha256.clone(),
            },
        )
        .unwrap();
        assert!(!list_installed(&install_root).unwrap()[0].enabled);
    }

    #[test]
    fn built_in_uninstall_is_persistent_and_reinstallable() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();

        uninstall_built_in(&install_root, "sample-indent", &hash).unwrap();
        assert!(list_installed(&install_root).unwrap().is_empty());
        assert_eq!(list_uninstalled(&install_root).unwrap(), ["sample-indent"]);
        assert!(install_root.join("content").join(&hash).is_dir());
        uninstall_built_in(&install_root, "sample-indent", &hash).unwrap();

        // A tombstone wins if cleanup of the old active state is delayed.
        write_profile_state(
            &install_root,
            &ProfileState {
                schema_version: 1,
                id: "sample-indent".to_owned(),
                archive_sha256: hash.clone(),
                enabled: true,
                signed: false,
                trust: "builtin".to_owned(),
            },
        )
        .unwrap();
        assert!(list_installed(&install_root).unwrap().is_empty());

        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();
        assert_eq!(list_installed(&install_root).unwrap().len(), 1);
        assert!(list_uninstalled(&install_root).unwrap().is_empty());
    }

    #[test]
    fn built_in_uninstall_rejects_a_mismatched_archive() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();

        let error =
            uninstall_built_in(&install_root, "sample-indent", &"0".repeat(64)).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidManifest);
        assert_eq!(list_installed(&install_root).unwrap().len(), 1);
        assert!(list_uninstalled(&install_root).unwrap().is_empty());
    }

    #[test]
    fn declarative_grammar_package_has_no_runtime_module() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        grammar_fixture(&source);
        let package = temp.path().join("shell.senp");
        let hash = pack_directory(&source, &package, None).unwrap();
        let verified = verify_package(
            &package,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash.clone(),
            },
        )
        .unwrap();
        assert!(verified.manifest.runtime.is_none());
        assert_eq!(verified.manifest.contributes.languages[0].id, "shellscript");

        let install_root = temp.path().join("installed");
        install_package(
            &package,
            &install_root,
            &TrustPolicy::BuiltIn {
                expected_archive_sha256: hash,
            },
        )
        .unwrap();
        let listed = list_installed(&install_root).unwrap();
        assert_eq!(listed.len(), 1);
        assert!(listed[0].module_path.is_none());
        assert!(listed[0]
            .extension_path
            .join("assets/syntaxes/shell-unix-bash.tmLanguage.json")
            .is_file());
    }

    #[test]
    fn declarative_grammar_package_rejects_missing_declared_asset() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        grammar_fixture(&source);
        fs::remove_file(source.join("assets/syntaxes/shell-unix-bash.tmLanguage.json")).unwrap();
        let error = pack_directory(&source, &temp.path().join("shell.senp"), None).unwrap_err();
        assert_eq!(error.code, ErrorCode::MissingRequiredEntry);
    }

    #[test]
    fn duplicate_manifest_member_is_rejected() {
        let value = br#"{"schemaVersion":1,"schemaVersion":1}"#;
        let result = strict_json::<Manifest>(value).unwrap_err();
        assert_eq!(result.code, ErrorCode::InvalidManifest);
        assert!(result.detail.contains("duplicate member"));
    }

    #[test]
    fn publisher_policy_rejects_unsigned_package() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        pack_directory(&source, &package, None).unwrap();
        let error =
            verify_package(&package, &TrustPolicy::PublisherKeys(BTreeMap::new())).unwrap_err();
        assert_eq!(error.code, ErrorCode::UnsignedPackage);
    }

    #[test]
    fn developer_package_installs_disabled_until_explicitly_enabled() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let package = temp.path().join("sample.senp");
        pack_directory(&source, &package, None).unwrap();
        let install_root = temp.path().join("installed");
        install_package(&package, &install_root, &TrustPolicy::DeveloperUnsigned).unwrap();

        let listed = list_installed(&install_root).unwrap();
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].trust, "developer");
        assert!(!listed[0].enabled);
    }

    #[test]
    fn pack_rejects_non_utf8_readme_before_publication() {
        let temp = TempDir::new().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        fs::write(source.join(README_PATH), [0xff, 0xfe]).unwrap();
        let error = pack_directory(&source, &temp.path().join("sample.senp"), None).unwrap_err();
        assert_eq!(error.code, ErrorCode::InvalidUtf8);
    }
}
