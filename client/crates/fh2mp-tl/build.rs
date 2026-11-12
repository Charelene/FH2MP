// Generates the Rust TL code of our chat schema (proto/fh2mp.tl) with grammers-tl-gen, as grammers-tl-types does for Telegram's.
use std::env;
use std::fs::{self, File};
use std::io::BufWriter;
use std::path::Path;

use grammers_tl_gen::{Config, Outputs, generate_rust_code};
use grammers_tl_parser::parse_tl_file;

fn main() -> std::io::Result<()> {
    let schema = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../proto/fh2mp.tl");
    println!("cargo:rerun-if-changed={}", schema.display());
    let text = fs::read_to_string(&schema)?;
    let mut defs = Vec::new();
    let mut skipped = 0;
    for d in parse_tl_file(&text) {
        match d {
            Ok(d) => defs.push(d),
            // The parser leaves out the built-in `vector {t:Type} # [ t ]` (NotImplemented); anything else that does not parse is a schema bug.
            Err(e) if format!("{e:?}") == "NotImplemented" && skipped == 0 => skipped += 1,
            Err(e) => panic!("{}: a definition does not parse: {e:?}", schema.display()),
        }
    }
    let out = Path::new(&env::var("OUT_DIR").unwrap()).to_path_buf();
    let mut outputs = Outputs {
        common: BufWriter::new(File::create(out.join("tl_common.rs"))?),
        types: BufWriter::new(File::create(out.join("tl_types.rs"))?),
        functions: BufWriter::new(File::create(out.join("tl_functions.rs"))?),
        enums: BufWriter::new(File::create(out.join("tl_enums.rs"))?),
    };
    // Requests decode too: the cross-language vectors read the Go bytes of requests back.
    let config = Config {
        gen_name_for_id: true,
        deserializable_functions: true,
        ..Config::default()
    };
    generate_rust_code(&mut outputs, &defs, 1, &config)?;
    outputs.flush()
}
