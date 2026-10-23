use std::{env, fs, path::PathBuf};

// Import declarative asset data, never the legacy inference implementation.
fn main() {
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let path = root.join("src/render/particle_source_catalog.h");
    println!("cargo:rerun-if-changed={}", path.display());
    let text = fs::read_to_string(path).unwrap();
    let mut output = String::new();
    for (section, rust, tuple) in [
        ("sources[]", "SOURCES", false),
        ("resources[]", "RESOURCES", true),
        ("attacks[]", "ATTACKS", true),
        ("hero_folders[]", "HERO_FOLDERS", true),
        ("aliases[]", "ALIASES", true),
    ] {
        let start = text.find(section).expect("asset catalog section");
        let body = text[start..]
            .split_once('{')
            .unwrap()
            .1
            .split_once("};")
            .unwrap()
            .0;
        output.push_str(&format!(
            "pub static {rust}: &[(&str, {})] = &[\n",
            if tuple { "&str" } else { "u32" }
        ));
        let mut count = 0;
        for line in body.lines() {
            let line = line.trim();
            if !line.starts_with("{\"") {
                continue;
            }
            let values = line
                .trim_start_matches('{')
                .trim_end_matches(',')
                .trim_end_matches('}');
            let (first, second) = values.split_once(',').expect("catalog pair");
            output.push_str(&format!(
                "({first}, {}),\n",
                second.trim().trim_end_matches('u')
            ));
            count += 1;
        }
        assert!(count > 0, "empty catalog {section}");
        output.push_str("];\n");
    }
    fs::write(
        PathBuf::from(env::var("OUT_DIR").unwrap()).join("catalog.rs"),
        output,
    )
    .unwrap();
}
