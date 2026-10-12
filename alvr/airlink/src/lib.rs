//! Meta Air Link wire protocol primitives.
//!
//! This crate is intentionally separate from ALVR's client transport. It is
//! the Rust home for the Meta discovery, DISCO, XRSP, and RuntimeIPC paths.

pub mod disco;
pub mod discovery;
pub mod host;
pub mod identity;
pub mod mesh;
pub mod ripc;
pub mod tracking;
pub mod transport;
pub mod video;
pub mod xrsp;
