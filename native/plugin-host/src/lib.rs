//! Trusted plugin isolation and supervision primitives. Callers still own
//! revision approval, host-connection admission, and explicit resource grants.
//! These modules do not yet constitute an integrated shell plugin system.

pub mod channel;
pub mod context;
pub mod controller;
pub mod exec;
pub mod exec_policy;
pub mod grants;
#[cfg(feature = "graphics")]
pub mod graphics;
pub mod host_job;
pub mod http;
pub mod management;
pub mod media;
pub mod notification;
mod payload;
pub mod presentation;
#[cfg(feature = "qt-bridge")]
mod qt;
pub mod requests;
pub mod revision;
pub mod sandbox;
pub mod session;
pub mod settings;
pub mod store;
pub mod supervisor;
pub mod worker;
