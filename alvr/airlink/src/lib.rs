//! Meta Air Link wire protocol primitives.
//!
//! This crate is intentionally separate from ALVR's client transport. It is
//! the Rust home for the Meta discovery, DISCO, XRSP, and RuntimeIPC paths.

pub mod disco;
pub mod xrsp;
