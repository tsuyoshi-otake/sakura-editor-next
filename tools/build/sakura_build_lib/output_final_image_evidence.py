"""Immutable, payload-free staging for final Output images.

Native observations describe the image that a linker inspected, while a
startup producer owns the image that was actually published.  This module
binds those two facts without copying executable or MAP contents into an
evidence record.  A stage is create-new: a published directory is never
overwritten, and every later validation re-hashes both files and the receipt.
"""

from __future__ import annotations

import copy
import hashlib
import json
import os
import shutil
import uuid
from pathlib import Path
from typing import Mapping

from .repository_path_safety import (
    RepositoryPathSafetyError,
    assert_inside_without_reparse as _repository_assert_inside_without_reparse,
    has_reparse_attribute as _repository_has_reparse_attribute,
    regular_file as _repository_regular_file,
    safe_repository_path as _repository_safe_path,
)
from .runner import BuildError


SCHEMA_VERSION = 1
RECORD_KIND = "output-final-image-stage"
_HASH_LENGTH = 64
_BACKENDS = frozenset({"cpp", "rust"})
_PLATFORMS = frozenset({"x64"})
_CONFIGURATIONS = frozenset({"Debug", "Release"})
_RECEIPT_KEYS = frozenset(
    {
        "schemaVersion",
        "record",
        "payloadFree",
        "stageId",
        "backend",
        "platform",
        "configuration",
        "sourceNativeEvidenceSha256",
        "receiptPath",
        "files",
        "receiptSha256",
    }
)
_FILE_KEYS = frozenset({"path", "sha256", "sizeBytes"})


class OutputFinalImageEvidenceError(BuildError):
    """Typed, fail-closed error for final-image staging or validation."""

    def __init__(self, code: str, message: str, exit_code: int = 5) -> None:
        super().__init__(code, message, exit_code)


def _canonical_json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _normalise_hash(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    text = value.strip().lower()
    if text.startswith("sha256:"):
        text = text[7:]
    if len(text) != _HASH_LENGTH or any(char not in "0123456789abcdef" for char in text):
        return None
    return text


def _hash_json(value: object) -> str:
    return _sha256_bytes(_canonical_json(value))


def _absolute(root: Path, value: Path) -> Path:
    candidate = value if value.is_absolute() else root / value
    return Path(os.path.abspath(os.fspath(candidate)))


def _convert_path_safety_error(error: RepositoryPathSafetyError) -> OutputFinalImageEvidenceError:
    return OutputFinalImageEvidenceError(error.code, str(error), error.exit_code)


def _has_reparse_attribute(path: Path, code: str = "OUTPUT_FINAL_IMAGE_PATH_UNSAFE") -> bool:
    try:
        return _repository_has_reparse_attribute(path, code)
    except RepositoryPathSafetyError as error:
        raise _convert_path_safety_error(error) from error


def _assert_inside_without_reparse(root: Path, candidate: Path, code: str) -> Path:
    try:
        return _repository_assert_inside_without_reparse(root, candidate, code)
    except RepositoryPathSafetyError as error:
        raise _convert_path_safety_error(error) from error


def _regular_file(root: Path, value: Path, code: str) -> Path:
    try:
        return _repository_regular_file(root, value, code)
    except RepositoryPathSafetyError as error:
        raise _convert_path_safety_error(error) from error


def safe_repository_path(
    root: Path,
    value: str | Path,
    *,
    code: str,
    reject_parent_segments: bool = False,
    reject_absolute: bool = False,
    require_regular_file: bool = False,
) -> Path:
    """Resolve an untrusted repository path without crossing a reparse edge.

    Lexical ``..`` segments are rejected when requested even if they happen to
    resolve back inside the root.  The lexical ancestor walk in
    ``_assert_inside_without_reparse`` rejects symlinks and Windows reparse
    points, including ancestors that resolve back into the repository.
    """

    try:
        return _repository_safe_path(
            root,
            value,
            code=code,
            reject_parent_segments=reject_parent_segments,
            reject_absolute=reject_absolute,
            require_regular_file=require_regular_file,
        )
    except RepositoryPathSafetyError as error:
        raise _convert_path_safety_error(error) from error


def _stage_directory(root: Path, value: Path) -> Path:
    path = _assert_inside_without_reparse(root, _absolute(root, value), "OUTPUT_FINAL_IMAGE_STAGE_PATH_UNSAFE")
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise OutputFinalImageEvidenceError(
            "OUTPUT_FINAL_IMAGE_STAGE_CREATE_FAILED", "could not create stage directory"
        ) from error
    return _assert_inside_without_reparse(root, path, "OUTPUT_FINAL_IMAGE_STAGE_PATH_UNSAFE")


def _file_identity(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
            size = os.fstat(stream.fileno()).st_size
    except OSError as error:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_FILE_READ", "could not read staged image") from error
    return "sha256:" + digest.hexdigest(), size


def _fsync_directory(path: Path) -> None:
    try:
        descriptor = os.open(os.fspath(path), os.O_RDONLY)
    except OSError as error:
        # Windows does not permit opening a directory with the CRT descriptor
        # API.  The file fsyncs and atomic rename still provide the supported
        # durability boundary there; all other failures remain typed.
        if os.name == "nt" and getattr(error, "errno", None) in {13, 22}:
            return
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_FSYNC_FAILED", "could not fsync stage directory") from error
    try:
        os.fsync(descriptor)
    except OSError as error:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_FSYNC_FAILED", "could not fsync stage directory") from error
    finally:
        os.close(descriptor)


def _copy_stable(source: Path, destination: Path) -> tuple[str, int]:
    before_hash, before_size = _file_identity(source)
    temporary = destination.with_name(f".{destination.name}.{uuid.uuid4().hex}.tmp")
    try:
        with source.open("rb") as source_stream, temporary.open("xb") as destination_stream:
            shutil.copyfileobj(source_stream, destination_stream, length=1024 * 1024)
            destination_stream.flush()
            os.fsync(destination_stream.fileno())
        after_hash, after_size = _file_identity(source)
        if (before_hash, before_size) != (after_hash, after_size):
            raise OutputFinalImageEvidenceError(
                "OUTPUT_FINAL_IMAGE_SOURCE_CHANGED", "source image changed while it was staged"
            )
        staged_hash, staged_size = _file_identity(temporary)
        if (staged_hash, staged_size) != (after_hash, after_size):
            raise OutputFinalImageEvidenceError(
                "OUTPUT_FINAL_IMAGE_STAGE_MISMATCH", "staged image does not match its source"
            )
        os.replace(temporary, destination)
        return after_hash, after_size
    except OutputFinalImageEvidenceError:
        raise
    except OSError as error:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_STAGE_COPY_FAILED", "could not stage image") from error
    finally:
        try:
            temporary.unlink()
        except OSError:
            pass


def _receipt_payload(receipt: Mapping[str, object]) -> dict[str, object]:
    return {key: value for key, value in receipt.items() if key != "receiptSha256"}


def _validate_receipt_shape(
    receipt: Mapping[str, object],
    *,
    expected_backend: str | None = None,
    expected_platform: str | None = None,
    expected_configuration: str | None = None,
    expected_native_hash: str | None = None,
) -> dict[str, object]:
    if set(receipt) != _RECEIPT_KEYS:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt fields are not canonical")
    if receipt.get("schemaVersion") != SCHEMA_VERSION or receipt.get("record") != RECORD_KIND:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "unsupported receipt schema")
    if receipt.get("payloadFree") is not True:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt must be payload-free")
    backend = receipt.get("backend")
    platform = receipt.get("platform")
    configuration = receipt.get("configuration")
    if backend not in _BACKENDS or platform not in _PLATFORMS or configuration not in _CONFIGURATIONS:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt selector is invalid")
    if expected_backend is not None and backend != expected_backend:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_SELECTOR_MISMATCH", "receipt backend mismatches request")
    if expected_platform is not None and platform != expected_platform:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_SELECTOR_MISMATCH", "receipt platform mismatches request")
    if expected_configuration is not None and configuration != expected_configuration:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_SELECTOR_MISMATCH", "receipt configuration mismatches request")
    stage_id = receipt.get("stageId")
    receipt_path = receipt.get("receiptPath")
    native_hash = _normalise_hash(receipt.get("sourceNativeEvidenceSha256"))
    if (
        not isinstance(stage_id, str)
        or not stage_id
        or not isinstance(receipt_path, str)
        or not receipt_path
        or Path(receipt_path).is_absolute()
        or ".." in Path(receipt_path).parts
        or native_hash is None
    ):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt identity is invalid")
    if expected_native_hash is not None and native_hash != _normalise_hash(expected_native_hash):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_MISMATCH", "receipt native hash mismatches request")
    files = receipt.get("files")
    if not isinstance(files, Mapping) or set(files) != {"exe", "map"}:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt file set is invalid")
    receipt_parent = Path(receipt_path).parent
    if (
        receipt_parent.name != stage_id
        or receipt_parent.parent.name != backend
        or receipt_parent.parent.parent.name != configuration
        or receipt_parent.parent.parent.parent.name != platform
    ):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt stage path is not canonical")
    normalized_files: dict[str, dict[str, object]] = {}
    for kind, value in files.items():
        if not isinstance(value, Mapping) or set(value) != _FILE_KEYS:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt file metadata is invalid")
        path = value.get("path")
        digest = _normalise_hash(value.get("sha256"))
        size = value.get("sizeBytes")
        if (
            not isinstance(path, str)
            or not path
            or Path(path).is_absolute()
            or ".." in Path(path).parts
            or digest is None
            or isinstance(size, bool)
            or not isinstance(size, int)
            or size < 0
        ):
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt file identity is invalid")
        expected_name = "sakura.exe" if kind == "exe" else "sakura.map"
        if Path(path).name.casefold() != expected_name:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt file name is invalid")
        if Path(path).parent != receipt_parent:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt file path is not canonical")
        normalized_files[kind] = {"path": path.replace("\\", "/"), "sha256": "sha256:" + digest, "sizeBytes": size}
    receipt_hash = _normalise_hash(receipt.get("receiptSha256"))
    if receipt_hash is None or receipt_hash != _normalise_hash(_hash_json(_receipt_payload(receipt))):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_HASH", "receipt hash does not match canonical content")
    return {
        "backend": backend,
        "platform": platform,
        "configuration": configuration,
        "stageId": stage_id,
        "receiptPath": receipt_path.replace("\\", "/"),
        "sourceNativeEvidenceSha256": "sha256:" + native_hash,
        "files": normalized_files,
        "receiptSha256": "sha256:" + receipt_hash,
    }


def _load_receipt(value: Path | Mapping[str, object], repo_root: Path) -> tuple[dict[str, object], Path | None]:
    if isinstance(value, Path):
        path = _regular_file(repo_root, value, "OUTPUT_FINAL_IMAGE_RECEIPT_PATH_UNSAFE")
        try:
            parsed = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_PARSE", "receipt is not valid JSON") from error
        if not isinstance(parsed, Mapping):
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt must be an object")
        normalized = _validate_receipt_shape(parsed)
        try:
            expected_path = path.relative_to(repo_root.resolve()).as_posix()
        except ValueError as error:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_PATH_UNSAFE", "receipt escapes root") from error
        if normalized["receiptPath"] != expected_path:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_PATH_MISMATCH", "receipt path is not canonical")
        return dict(parsed), path
    if not isinstance(value, Mapping):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_RECEIPT_SCHEMA", "receipt must be an object")
    return dict(value), None


def _validate_receipt_files(receipt: Mapping[str, object], repo_root: Path) -> None:
    normalized = _validate_receipt_shape(receipt)
    for metadata in normalized["files"].values():
        path = _regular_file(repo_root, repo_root / str(metadata["path"]), "OUTPUT_FINAL_IMAGE_FILE_PATH_UNSAFE")
        digest, size = _file_identity(path)
        if _normalise_hash(digest) != _normalise_hash(metadata["sha256"]) or size != metadata["sizeBytes"]:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_TAMPERED", "staged image does not match receipt")


def _cleanup_failed_transaction(transaction: Path, primary_error: BaseException | None) -> None:
    """Remove an unpublished transaction without masking its primary error.

    A failed staging operation must never silently leave a transaction behind.
    If cleanup itself fails, the cleanup error is the terminal typed error and
    the primary typed error is retained as its exception cause.  Only bounded
    error codes are exposed; no path or OS error text is attached.
    """

    cleanup_failed = False
    try:
        shutil.rmtree(transaction)
    except FileNotFoundError:
        return
    except OSError:
        cleanup_failed = True
    else:
        try:
            os.lstat(transaction)
        except FileNotFoundError:
            cleanup_failed = False
        except OSError:
            # Failure to prove absence is a cleanup failure, even if the
            # delete call itself returned successfully.
            cleanup_failed = True
        else:
            cleanup_failed = True
    if not cleanup_failed:
        return
    cleanup_code = "OUTPUT_FINAL_IMAGE_TRANSACTION_CLEANUP_FAILED"
    primary_code = getattr(primary_error, "code", "UNEXPECTED") if primary_error is not None else "UNEXPECTED"
    cleanup_error = OutputFinalImageEvidenceError(
        cleanup_code, f"could not clean up failed final-image transaction; primary={primary_code}"
    )
    if primary_error is not None:
        raise cleanup_error from primary_error
    raise cleanup_error


def stage_output_final_image(
    *,
    repo_root: Path,
    stage_root: Path,
    backend: str,
    platform: str,
    configuration: str,
    source_native_evidence_sha256: str,
    executable_path: Path,
    map_path: Path,
) -> dict[str, object]:
    """Copy an EXE and MAP to a create-new, atomically published stage."""

    root = Path(repo_root).resolve()
    if backend not in _BACKENDS or platform not in _PLATFORMS or configuration not in _CONFIGURATIONS:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_SELECTOR_INVALID", "unsupported final-image selector", 2)
    native_hash = _normalise_hash(source_native_evidence_sha256)
    if native_hash is None:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_HASH_INVALID", "native evidence hash is invalid", 2)
    source_exe = _regular_file(root, executable_path, "OUTPUT_FINAL_IMAGE_SOURCE_PATH_UNSAFE")
    source_map = _regular_file(root, map_path, "OUTPUT_FINAL_IMAGE_SOURCE_PATH_UNSAFE")
    stage_base = _stage_directory(root, stage_root)
    stage_id = f"{backend}-{platform}-{configuration.lower()}-{native_hash[:16]}"
    parent = _stage_directory(root, stage_base / platform / configuration / backend)
    published = parent / stage_id
    try:
        os.lstat(published)
    except FileNotFoundError:
        pass
    except OSError as error:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_STAGE_PATH_UNSAFE", "could not inspect published stage") from error
    else:
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_EXISTS", "immutable final-image stage already exists")
    if _has_reparse_attribute(published, "OUTPUT_FINAL_IMAGE_STAGE_PATH_UNSAFE"):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_STAGE_PATH_UNSAFE", "published stage is a reparse point")
    transaction = parent / f".txn-{stage_id}-{uuid.uuid4().hex}"
    published_ok = False
    published_valid = False
    primary_error: BaseException | None = None
    try:
        transaction.mkdir()
        _assert_inside_without_reparse(root, transaction, "OUTPUT_FINAL_IMAGE_STAGE_PATH_UNSAFE")
        staged_exe = transaction / "sakura.exe"
        staged_map = transaction / "sakura.map"
        exe_hash, exe_size = _copy_stable(source_exe, staged_exe)
        map_hash, map_size = _copy_stable(source_map, staged_map)
        # Both source files are revalidated together immediately before the
        # receipt is committed.  This closes the gap where one member of the
        # EXE/MAP pair changes after its individual copy check.
        current_exe = _regular_file(root, executable_path, "OUTPUT_FINAL_IMAGE_SOURCE_PATH_UNSAFE")
        current_map = _regular_file(root, map_path, "OUTPUT_FINAL_IMAGE_SOURCE_PATH_UNSAFE")
        if (
            current_exe != source_exe
            or current_map != source_map
            or _file_identity(current_exe) != (exe_hash, exe_size)
            or _file_identity(current_map) != (map_hash, map_size)
        ):
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_SOURCE_CHANGED", "source image pair changed while staged")
        receipt_path = (published / "receipt.json").relative_to(root).as_posix()
        receipt: dict[str, object] = {
            "schemaVersion": SCHEMA_VERSION,
            "record": RECORD_KIND,
            "payloadFree": True,
            "stageId": stage_id,
            "backend": backend,
            "platform": platform,
            "configuration": configuration,
            "sourceNativeEvidenceSha256": "sha256:" + native_hash,
            "receiptPath": receipt_path,
            "files": {
                "exe": {"path": (published / "sakura.exe").relative_to(root).as_posix(), "sha256": exe_hash, "sizeBytes": exe_size},
                "map": {"path": (published / "sakura.map").relative_to(root).as_posix(), "sha256": map_hash, "sizeBytes": map_size},
            },
        }
        receipt["receiptSha256"] = _hash_json(receipt)
        _validate_receipt_shape(receipt)
        receipt_temporary = transaction / f".receipt.{uuid.uuid4().hex}.tmp"
        receipt_data = json.dumps(receipt, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8") + b"\n"
        with receipt_temporary.open("xb") as stream:
            stream.write(receipt_data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(receipt_temporary, transaction / "receipt.json")
        _fsync_directory(transaction)
        # The receipt refers to the post-publication path; validate content
        # against the transaction files before the directory becomes visible.
        for kind, expected in (('exe', (exe_hash, exe_size)), ('map', (map_hash, map_size))):
            actual = _file_identity(transaction / ("sakura.exe" if kind == "exe" else "sakura.map"))
            if actual != expected:
                raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_STAGE_MISMATCH", "transaction is inconsistent")
        try:
            os.rename(transaction, published)
        except FileExistsError as error:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_EXISTS", "immutable final-image stage already exists") from error
        published_ok = True
        _fsync_directory(parent)
        validate_output_final_image_stage(root / receipt_path, repo_root=root)
        published_valid = True
        return receipt
    except OutputFinalImageEvidenceError as error:
        primary_error = error
        raise
    except OSError as error:
        converted = OutputFinalImageEvidenceError(
            "OUTPUT_FINAL_IMAGE_PUBLISH_FAILED", "could not publish final-image stage"
        )
        primary_error = converted
        raise converted from error
    except BaseException as error:
        primary_error = error
        raise
    finally:
        if not published_ok:
            _cleanup_failed_transaction(transaction, primary_error)
        elif not published_valid:
            # A directory is visible only after the atomic rename, but a
            # post-publication receipt/file validation can still fail.  Do not
            # leave an invalid immutable stage that would poison a retry.
            _cleanup_failed_transaction(published, primary_error)


def validate_output_final_image_stage(
    receipt: Path | Mapping[str, object],
    *,
    repo_root: Path,
    expected_backend: str | None = None,
    expected_platform: str | None = None,
    expected_configuration: str | None = None,
    expected_native_hash: str | None = None,
) -> dict[str, object]:
    """Revalidate a receipt and both staged files, returning only identities."""

    root = Path(repo_root).resolve()
    parsed, _path = _load_receipt(receipt, root)
    normalized = _validate_receipt_shape(
        parsed,
        expected_backend=expected_backend,
        expected_platform=expected_platform,
        expected_configuration=expected_configuration,
        expected_native_hash=expected_native_hash,
    )
    _validate_receipt_files(parsed, root)
    return {
        "ok": True,
        "record": RECORD_KIND,
        "backend": normalized["backend"],
        "platform": normalized["platform"],
        "configuration": normalized["configuration"],
        "stageId": normalized["stageId"],
        "receiptPath": normalized["receiptPath"],
        "sourceNativeEvidenceSha256": normalized["sourceNativeEvidenceSha256"],
        "receiptSha256": normalized["receiptSha256"],
        "files": normalized["files"],
    }


def bind_native_evidence_to_final_image(
    native: Mapping[str, object], receipt: Mapping[str, object]
) -> dict[str, object]:
    """Return a native record bound to staged EXE/MAP identities.

    The input record is never mutated.  Native EXE/MAP observations remain
    unchanged; the returned record adds a receipt binding and therefore gets
    a new binding-inclusive hard hash.  Consumers can remove that binding and
    recompute the source-native hash before comparing it with the receipt.
    """

    normalized = _validate_receipt_shape(receipt)
    from .product_native_evidence import product_native_evidence_hash

    source_hash = _normalise_hash(native.get("hard_evidence_hash", native.get("hardEvidenceSha256")))
    try:
        canonical_source_hash = _normalise_hash(product_native_evidence_hash(native))
    except (TypeError, ValueError):
        canonical_source_hash = None
    if (
        source_hash is None
        or canonical_source_hash != source_hash
        or source_hash != _normalise_hash(normalized["sourceNativeEvidenceSha256"])
    ):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_MISMATCH", "native record does not match receipt")
    bound = copy.deepcopy(dict(native))
    link = bound.get("link")
    if not isinstance(link, dict):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", "native link record is missing")
    exe = normalized["files"]["exe"]
    map_file = normalized["files"]["map"]
    output_reference = link.get("output", link.get("productPath", link.get("outputPath")))
    output_hash = _normalise_hash(link.get("product_hash", link.get("productSha256", link.get("outputSha256"))))
    output_size = link.get("product_size_bytes", link.get("productSizeBytes", link.get("outputSizeBytes")))
    if (
        not isinstance(output_reference, str)
        or Path(output_reference).name.casefold() != "sakura.exe"
        or output_hash is None
        or not isinstance(output_size, int)
        or isinstance(output_size, bool)
        or output_size < 0
        or output_hash != _normalise_hash(exe["sha256"])
        or output_size != exe["sizeBytes"]
    ):
        raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_MISMATCH", "native EXE identity does not match receipt")

    map_identities: list[tuple[str, str, int]] = []
    for key in (
        "selected_archive_member_evidence",
        "output_provider_member_evidence",
        "output_provider_symbol_evidence",
        "selectedArchiveMemberEvidence",
        "outputProviderMemberEvidence",
        "outputProviderSymbolEvidence",
    ):
        value = link.get(key)
        if not isinstance(value, Mapping):
            continue
        path_values = [value[name] for name in ("map", "mapPath") if name in value]
        hash_values = [value[name] for name in ("map_hash", "mapHash", "mapSha256") if name in value]
        size_values = [value[name] for name in ("map_size_bytes", "mapSizeBytes") if name in value]
        if not path_values and not hash_values and not size_values:
            continue
        if len(path_values) == 0 or len(hash_values) == 0 or len(size_values) == 0:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", "native MAP identity is incomplete")
        normalized_path = str(path_values[0]).replace("\\", "/")
        normalized_hashes = [_normalise_hash(item) for item in hash_values]
        if (
            not normalized_path
            or any(item is None for item in normalized_hashes)
            or len(set(normalized_hashes)) != 1
            or any(not isinstance(item, int) or isinstance(item, bool) or item < 0 for item in size_values)
            or len(set(size_values)) != 1
        ):
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_SCHEMA", "native MAP identity is invalid")
        map_identities.append((normalized_path.casefold(), normalized_hashes[0], size_values[0]))
    if map_identities:
        if len({identity[0] for identity in map_identities}) != 1 or len({identity[1:] for identity in map_identities}) != 1:
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_MISMATCH", "native MAP identities disagree")
        if (
            map_identities[0][1] != _normalise_hash(map_file["sha256"])
            or map_identities[0][2] != map_file["sizeBytes"]
        ):
            raise OutputFinalImageEvidenceError("OUTPUT_FINAL_IMAGE_NATIVE_MISMATCH", "native MAP identity does not match receipt")
    link["final_image_stage"] = {
        "record": RECORD_KIND,
        "receipt": normalized["receiptPath"],
        "receiptSha256": normalized["receiptSha256"],
        "sourceNativeEvidenceSha256": normalized["sourceNativeEvidenceSha256"],
    }
    bound.pop("hardEvidenceSha256", None)
    bound["hard_evidence_hash"] = product_native_evidence_hash(bound)
    return bound


__all__ = [
    "OutputFinalImageEvidenceError",
    "RECORD_KIND",
    "SCHEMA_VERSION",
    "bind_native_evidence_to_final_image",
    "safe_repository_path",
    "stage_output_final_image",
    "validate_output_final_image_stage",
]
