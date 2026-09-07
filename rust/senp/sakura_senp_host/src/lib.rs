//! Shared v2 contracts, compiled independently of the v1 executable dispatcher.
pub mod effect_bridge;
pub mod effect_protocol;
pub mod effect_session;

pub mod bindings_v2 {
    wasmtime::component::bindgen!({
        path: "../wit/v2/senp-extension.wit",
        world: "extension",
    });
}
