//! The runtime ABI string is the only compatibility check a package gets before
//! the host tries to instantiate it, so it must move whenever the WIT world
//! does. A record field added without a version bump let packages built for the
//! old world pass every manifest check and then fail inside wasmtime with a
//! generic error (#299). This test makes that change impossible to land silently.

use sha2::{Digest, Sha256};

const WIT: &str = include_str!("../../wit/v2/senp-extension.wit");

/// The WIT package version and the SHA-256 of the WIT text (LF line endings)
/// that shipped under it. Changing the WIT means bumping the `package` version
/// in the WIT, the manifests' `runtime.abi`, `sakura_senp::ABI_V2`, the host's
/// `effect_protocol::ABI` and the C++ `kAbi`, then re-pinning both values here.
const WIT_PIN: (&str, &str) = (
    "3.0.0",
    "0097b8156c43eca87edf174dbfaebc2bb7775d4052c7807667dc7bcdb3f66068",
);

fn package_version() -> &'static str {
    WIT.lines()
        .find_map(|line| line.trim().strip_prefix("package sakura:senp@"))
        .and_then(|rest| rest.strip_suffix(';'))
        .expect("the WIT names its sakura:senp package version")
}

#[test]
fn wit_changes_require_an_abi_version_bump() {
    let normalized = WIT.replace("\r\n", "\n");
    let digest: String = Sha256::digest(normalized.as_bytes())
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    let version = package_version();
    assert!(
        version != WIT_PIN.0 || digest == WIT_PIN.1,
        "senp-extension.wit changed but its package version is still {version}. \
         Bump the version (and every runtime ABI string), then pin \
         ({{new version}}, {digest})."
    );
    assert!(
        version == WIT_PIN.0,
        "the WIT package is now {version}; re-pin WIT_PIN to ({version}, {digest})"
    );
}

#[test]
fn the_host_abi_is_the_wit_package_version() {
    assert_eq!(
        sakura_senp_host::effect_protocol::ABI,
        format!("sakura:senp/extension@{}", package_version())
    );
}
