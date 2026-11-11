//! Rust types of the FH2MP chat schema, layer 1 (`proto/fh2mp.tl`), generated at build time with grammers-tl-gen.
//!
//! `functions::*` are the requests (each implements [`RemoteCall`] with its answer type), `types::*` the constructors and `enums::*` the
//! boxed types. Encoding goes through grammers-tl-types' [`Serializable`] and [`Deserializable`].

// The generated code refers to these at the crate root, as inside grammers-tl-types.
pub use grammers_tl_types::{Blob, Deserializable, Identifiable, RawVec, RemoteCall, Serializable};

/// The schema layer this crate was generated from; sent in help.init.
pub const LAYER: i32 = 1;

pub mod deserialize {
    pub use grammers_tl_types::deserialize::{Cursor, Error, Result};
    pub type Buffer<'a, 'b> = &'a mut Cursor<'b>;
}

#[allow(dead_code, unused_imports, clippy::all)]
mod generated_common {
    include!(concat!(env!("OUT_DIR"), "/tl_common.rs"));
}
pub use generated_common::*;

#[allow(dead_code, clippy::all)]
pub mod types {
    include!(concat!(env!("OUT_DIR"), "/tl_types.rs"));
}

#[allow(dead_code, clippy::all)]
pub mod enums {
    include!(concat!(env!("OUT_DIR"), "/tl_enums.rs"));
}

#[allow(dead_code, clippy::all)]
pub mod functions {
    include!(concat!(env!("OUT_DIR"), "/tl_functions.rs"));
}
