//! Link libyeptris into the crate.
//!
//! Resolution order:
//!   1. `vendored` feature — compile the checked-in engine snapshot
//!      (`vendor/yeptris/`, pinned by `vendor/yeptris/ENGINE_REF`)
//!      with the cc crate and link it statically. Explicit opt-in
//!      (yeptris-rs#40): it wins over everything below, so
//!      `cargo add yeptris --features vendored` works with no engine
//!      installed. Never fetches — crates.io builds have no network.
//!   2. YEPTRIS_LIB_PATH — full path to the shared library file, or a
//!      directory containing it. CI sets this after building the C
//!      library from source.
//!   3. Fallback: system linker search (`-lyeptris`).

use std::path::{Path, PathBuf};
use std::{env, fs};

fn main() {
    // cfg(feature) works in build scripts: cargo passes the feature
    // cfgs to build.rs compilation, so the cc machinery (and the
    // optional cc build-dependency) exists only under vendored.
    #[cfg(feature = "vendored")]
    {
        if env::var_os("CARGO_FEATURE_VENDORED").is_some() {
            build_vendored();
            return;
        }
    }
    // The lib path changes the emitted link directives; without
    // this the build script output is cached across YEPTRIS_LIB_PATH
    // changes and tests link a stale library (leptris-rs lesson).
    println!("cargo:rerun-if-env-changed=YEPTRIS_LIB_PATH");
    // Windows: the DLL is libyeptris.dll, so the import library is
    // libyeptris.lib — link.exe resolves `-l <name>` as <name>.lib.
    let name = if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        "libyeptris"
    } else {
        "yeptris"
    };
    if let Ok(path) = env::var("YEPTRIS_LIB_PATH") {
        // Tolerate repo-relative paths: cargo runs build.rs from the
        // crate directory, so a path like `build-shared/src` (natural
        // when set from the repository root) would not resolve.
        let mut p = PathBuf::from(&path);
        if !p.exists() {
            if let Ok(manifest) = env::var("CARGO_MANIFEST_DIR") {
                let repo_relative = Path::new(&manifest).parent().map(|root| root.join(&p));
                if let Some(cand) = repo_relative {
                    if cand.exists() {
                        p = cand;
                    }
                }
            }
        }
        let dir: PathBuf = if p.is_file() {
            p.parent().unwrap().to_path_buf()
        } else {
            p.clone()
        };
        // Absolute for -rpath: a relative rpath resolves against the
        // test binary's directory, not the crate.
        let dir = dir.canonicalize().unwrap_or(dir);
        let found = if p.is_file() { Some(p) } else { find_lib(&p) };
        assert!(
            found.is_some(),
            "YEPTRIS_LIB_PATH set but no libyeptris shared library found under {path}"
        );
        println!("cargo:rustc-link-search=native={}", dir.display());
        println!("cargo:rustc-link-lib=dylib={name}");
        // Let test binaries find the dylib at run time without
        // requiring DYLD_LIBRARY_PATH.
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", dir.display());
        println!("cargo:rustc-env=YEPTRIS_LIB_DIR={}", dir.display());
        return;
    }
    println!("cargo:rustc-link-lib=dylib={name}");
}

fn find_lib(dir: &Path) -> Option<PathBuf> {
    let prefixes = ["lib", ""];
    let exts = ["dylib", "so", "dll"];
    for ext in exts {
        for prefix in prefixes {
            let cand = dir.join(format!("{prefix}yeptris.{ext}"));
            if cand.exists() {
                return Some(cand);
            }
        }
    }
    // Windows often drops the prefix.
    let _ = fs::metadata(dir);
    None
}

/// Issue #40: compile the checked-in engine snapshot into the crate.
///
/// The snapshot under `vendor/yeptris/` is the engine repository's own
/// tree (`src/`, `CMakeLists.txt`, `LICENSE.md`), exported with
/// `git archive` at the release recorded in `ENGINE_REF` — never
/// hand-edited. The engine's own files stay the single source of
/// truth: the TU list is PARSED out of the snapshot's
/// `src/CMakeLists.txt` (`set(YEPTRIS_SOURCES …)` plus the
/// `list(APPEND …)` additions), and `version.h` is generated from
/// `project(VERSION)` + `version.h.in` exactly as CMake's
/// `configure_file` does — so a snapshot refresh is a pure file
/// replacement with zero build.rs edits (README.adoc, "Vendored
/// engine snapshot").
#[cfg(feature = "vendored")]
fn build_vendored() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let snap = manifest.join("vendor").join("yeptris");
    assert!(
        snap.join("src").is_dir() && snap.join("ENGINE_REF").is_file(),
        "vendored engine snapshot missing under {} — the vendored feature compiles the checked-in snapshot; it never fetches",
        snap.display()
    );

    // Every snapshot file is a build input: any change (a refresh)
    // reruns this script. Per-file, not per-directory, so stale cargo
    // versions that scan directories shallowly still rerun.
    let mut inputs = Vec::new();
    collect_files(&snap, &mut inputs);
    for f in &inputs {
        println!("cargo:rerun-if-changed={}", f.display());
    }

    let (tag, commit) = engine_ref(&snap);
    println!("cargo:rustc-env=YEPTRIS_ENGINE_REV={tag} ({commit})");

    let version = project_version(&snap.join("CMakeLists.txt"));
    let expected_tag = format!("refs/tags/v{}.{}.{}", version.0, version.1, version.2);
    assert!(
        tag == expected_tag,
        "ENGINE_REF ({tag}) and the snapshot's CMakeLists project(VERSION) ({expected_tag}) \
         disagree — refresh the snapshot as ONE unit (README.adoc, Vendored engine snapshot)"
    );

    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let generated = out_dir.join("generated");
    render_version_h(
        &snap.join("src/include/yeptris/version.h.in"),
        &generated,
        version,
    );

    let mut sources = cmake_sources(&snap.join("src/CMakeLists.txt"));
    // The per-ISA SIMD TUs ride CMake's arch-gated `list(APPEND)`s,
    // which the parser cannot see the `if()` context of — strip them
    // here; build.rs re-adds the one matching the compile TARGET below
    // (mirroring src/CMakeLists.txt's CMAKE_SYSTEM_PROCESSOR branches).
    sources.retain(|s| {
        s != Path::new("yeptris/common/simd_text_avx2.c")
            && s != Path::new("yeptris/common/simd_text_neon.c")
    });
    // Parser regression gates: losing one of these must fail loudly
    // here, not as a confusing link error (or worse, a stub build).
    for anchor in [
        "yeptris/parse/engine.c",
        "yeptris/emit/emit.c",
        "yeptris/common/simd_text.c",
    ] {
        assert!(
            sources.iter().any(|s| s == Path::new(anchor)),
            "parsed source list lost the anchor {anchor} — the CMake parser regressed"
        );
    }

    let arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
    let msvc = env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");

    // AArch64: NEON is architectural, the TU rides the baseline
    // archive (CMake adds no flags for it either; MSVC-ARM64 keeps out
    // — port.h's YEP_ARCH_AARCH64 gate drives the same decision at
    // compile time, so the worst case is an empty TU).
    if arch == "aarch64" {
        sources.push(PathBuf::from("yeptris/common/simd_text_neon.c"));
    }
    if browser_target() {
        validate_browser_toolchain();
    }
    let mut build = base_build(&snap, &generated, msvc);
    for f in &sources {
        build.file(snap.join("src").join(f));
    }
    // The main archive MUST come first: its dispatch references the
    // AVX2 kernel table, and static archives resolve left-to-right —
    // the defining archive has to be scanned after the referencing one.
    build.compile("yeptris");

    if browser_target() {
        link_browser_runtime();
    }

    // x86: the AVX2 TU needs arch flags outside the baseline; cc has
    // no per-file flags, so it is a second archive. The runtime
    // dispatch (simd_text.c) selects it only when the host really has
    // AVX2 — pre-AVX2 machines stay on the scalar table, no SIGILL.
    if arch == "x86_64" || arch == "x86" {
        let mut avx = base_build(&snap, &generated, msvc);
        if msvc {
            avx.flag("/arch:AVX2");
        } else {
            avx.flag("-mavx2").flag("-mpopcnt");
        }
        avx.file(snap.join("src/yeptris/common/simd_text_avx2.c"));
        avx.compile("yeptris_avx2");
    }
}

/// The snapshot's compile shape, common to every archive: the
/// generated `version.h` include FIRST (the public headers include
/// `<yeptris/version.h>`), then the public headers, the internal
/// root-relative include root, and the float printer's private
/// include. Definitions and standards mirror the CMake library
/// targets: YEPTRIS_BUILDING (the api.h export control; on MSVC it is
/// the dllexport side), YEPTRIS_WITH_CBOR (default-ON in CMake, and
/// the binding's cbor face needs it), C11 with extensions off
/// (`CMAKE_C_STANDARD 11` / `C_EXTENSIONS OFF`; cc spells it
/// `-std=c11` on GCC/clang, `/std:c11` on MSVC).
#[cfg(feature = "vendored")]
fn base_build(snap: &Path, generated: &Path, msvc: bool) -> cc::Build {
    let mut build = cc::Build::new();
    if browser_target() {
        let features = env::var("CARGO_CFG_TARGET_FEATURE").unwrap_or_default();
        assert!(
            !features.split(',').any(|f| f == "atomics"),
            "vendored browser builds currently require single-thread WebAssembly without atomics"
        );
        assert!(features.split(',').any(|f| f == "bulk-memory"), "vendored browser builds require Rust's bulk-memory target feature for in-module memory copies");
        // Emscripten supplies the C headers and scalar, single-thread libc.
        // Rust/wasm-bindgen still owns the final module and its linear memory.
        build
            .compiler(browser_compiler())
            .target("wasm32-unknown-emscripten");
    }
    build
        .include(generated)
        .include(snap.join("src/include"))
        .include(snap.join("src/yeptris"))
        .include(snap.join("src/yeptris/emit/float"))
        .define("YEPTRIS_BUILDING", None)
        .define("YEPTRIS_WITH_CBOR", None)
        .std("c11");
    // The exact warning/visibility shape of the engine's own targets
    // (CMakeLists.txt yeptris_set_warnings + C_VISIBILITY_PRESET
    // hidden), so the vendored build is as quiet and as tight as the
    // CMake one; YEPTRIS_API re-exports the public surface.
    if msvc {
        // /experimental:c11atomics un-breaks the C11 headers' stdatomic
        // gate (the engine's windows sentinel surfaced it — carried
        // from yeptris_set_warnings, not a shim)
        build.flag("/experimental:c11atomics");
    } else {
        build
            .flag("-Wall")
            .flag("-Wextra")
            .flag("-Wno-unused-parameter")
            .flag("-fvisibility=hidden");
    }
    build
}

/// This integration deliberately supports single-thread browser modules only.
#[cfg(feature = "vendored")]
fn browser_target() -> bool {
    env::var("TARGET").as_deref() == Ok("wasm32-unknown-unknown")
}

#[cfg(feature = "vendored")]
fn browser_compiler() -> std::ffi::OsString {
    println!("cargo:rerun-if-env-changed=YEPTRIS_EMCC");
    env::var_os("YEPTRIS_EMCC").unwrap_or_else(|| "emcc".into())
}

#[cfg(feature = "vendored")]
fn validate_browser_toolchain() {
    let output = std::process::Command::new(browser_compiler())
        .arg("--version")
        .output()
        .expect("browser vendored builds require Emscripten SDK 3.1.50: activate it so emcc is on PATH, or set YEPTRIS_EMCC");
    let text = String::from_utf8_lossy(&output.stdout);
    let version = text.lines().next().and_then(|line| {
        line.split_whitespace()
            .find(|token| token.starts_with(|c: char| c.is_ascii_digit()))
    });
    assert!(
        output.status.success() && version == Some("3.1.50"),
        "browser C runtime integration requires Emscripten SDK 3.1.50; compiler reported: {}",
        text.lines().next().unwrap_or("<no version>")
    );
    println!("cargo:rerun-if-env-changed=EM_CACHE");
    println!("cargo:rerun-if-env-changed=EMSDK");
}

#[cfg(feature = "vendored")]
fn link_browser_runtime() {
    use std::process::Command;
    let output = Command::new(browser_compiler())
        .arg("--print-file-name=libc.a")
        .output()
        .expect("browser vendored builds require emcc from Emscripten SDK 3.1.50 on PATH (or YEPTRIS_EMCC)");
    assert!(
        output.status.success(),
        "emcc failed to locate its libc archive"
    );
    let archive = PathBuf::from(
        String::from_utf8(output.stdout)
            .expect("emcc archive path is UTF-8")
            .trim(),
    );
    assert!(
        archive.is_absolute() && archive.is_file(),
        "Emscripten libc archive missing; run `embuilder build libc` with SDK 3.1.50 first"
    );
    println!("cargo:rerun-if-changed={}", archive.display());
    println!(
        "cargo:rustc-link-search=native={}",
        archive.parent().unwrap().display()
    );
    println!("cargo:rustc-link-lib=static=c");
}

/// Per-file `rerun-if-changed` over the snapshot — the whole tree is
/// build input; a refresh (any file) reruns this script.
#[cfg(feature = "vendored")]
fn collect_files(dir: &Path, out: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir)
        .expect("vendored snapshot readable")
        .flatten()
    {
        let p = entry.path();
        if p.is_dir() {
            collect_files(&p, out);
        } else {
            out.push(p);
        }
    }
}

/// The `(ref, commit)` pair from ENGINE_REF — comment lines skipped;
/// the file is two tagged lines (`ref …` / `commit …`).
#[cfg(feature = "vendored")]
fn engine_ref(snap: &Path) -> (String, String) {
    let text = fs::read_to_string(snap.join("ENGINE_REF")).expect("vendor/yeptris/ENGINE_REF");
    let mut tag = None;
    let mut commit = None;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some(v) = line.strip_prefix("ref ") {
            tag = Some(v.trim().to_string());
        }
        if let Some(v) = line.strip_prefix("commit ") {
            commit = Some(v.trim().to_string());
        }
    }
    (
        tag.expect("ENGINE_REF: no `ref` line"),
        commit.expect("ENGINE_REF: no `commit` line"),
    )
}

/// The `project(yeptris VERSION X.Y.Z)` line — the engine's versioning
/// single source of truth (its scripts/bump-version.sh syncs everything
/// from it, and the build.rs tag cross-check pins the snapshot to it).
#[cfg(feature = "vendored")]
fn project_version(cmake: &Path) -> (u32, u32, u32) {
    let text = fs::read_to_string(cmake).expect("snapshot CMakeLists.txt");
    let mut ver = None;
    for line in text
        .lines()
        .skip_while(|l| !l.trim_start().starts_with("project(yeptris"))
        .take(8)
    {
        let mut tok = line.split_whitespace();
        if tok.next() == Some("VERSION") {
            ver = tok.next().map(str::to_string);
            break;
        }
        if line.trim_start().starts_with(')') {
            break;
        }
    }
    let ver = ver.expect("project(yeptris …) has no VERSION line");
    let parts: Vec<u32> = ver
        .split('.')
        .map(|p| p.parse().expect("numeric project VERSION part"))
        .collect();
    assert_eq!(
        parts.len(),
        3,
        "project VERSION not major.minor.patch: {ver}"
    );
    (parts[0], parts[1], parts[2])
}

/// Every TU the snapshot's `src/CMakeLists.txt` puts on
/// YEPTRIS_SOURCES: the `set(…)` block plus every
/// `list(APPEND YEPTRIS_SOURCES …)` addition (CBOR is default-ON in
/// CMake and the binding's cbor face needs it). The list ALSO picks up
/// the per-ISA SIMD appends, whose arch `if()` context the parser
/// cannot see — `build_vendored` strips them and re-adds the one
/// matching the compile target, exactly as the CMake branches do.
#[cfg(feature = "vendored")]
fn cmake_sources(cmake: &Path) -> Vec<PathBuf> {
    let text = fs::read_to_string(cmake).expect("snapshot src/CMakeLists.txt");
    // Comment-stripped token stream over the whole file; the calls are
    // few and short, so "ends-with-paren closes the call" is enough.
    let mut toks: Vec<&str> = Vec::new();
    for line in text.lines() {
        let line = match line.find('#') {
            Some(i) => &line[..i],
            None => line,
        };
        toks.extend(line.split_whitespace());
    }
    let mut srcs: Vec<PathBuf> = Vec::new();
    let mut i = 0;
    while i < toks.len() {
        let is_list_call = toks[i] == "set(YEPTRIS_SOURCES"
            || (toks[i] == "list(APPEND" && toks.get(i + 1).copied() == Some("YEPTRIS_SOURCES"));
        if is_list_call {
            let mut j = if toks[i] == "set(YEPTRIS_SOURCES" {
                i + 1
            } else {
                i + 2
            };
            while j < toks.len() && !toks[j].ends_with(')') {
                add_source(&mut srcs, toks[j]);
                j += 1;
            }
            assert!(
                j < toks.len(),
                "YEPTRIS_SOURCES call not closed in src/CMakeLists.txt"
            );
            add_source(&mut srcs, toks[j].strip_suffix(')').expect("closing paren"));
            i = j + 1;
        } else {
            i += 1;
        }
    }
    assert!(
        srcs.len() >= 40,
        "parsed an implausible TU count ({}) from the snapshot — the CMake parser regressed",
        srcs.len()
    );
    srcs
}

#[cfg(feature = "vendored")]
fn add_source(srcs: &mut Vec<PathBuf>, tok: &str) {
    assert!(
        !tok.contains('$'),
        "generator expression in YEPTRIS_SOURCES: {tok} — build.rs cannot resolve it"
    );
    if tok.ends_with(".c") && !srcs.contains(&PathBuf::from(tok)) {
        srcs.push(PathBuf::from(tok));
    }
}

/// Generate `yeptris/version.h` the way CMake's `configure_file` does
/// (@ONLY substitution of the four project version tokens) into
/// `OUT_DIR/generated` — the build-tree include dir the CMake build
/// provides. The engine forbids hand-editing the generated file
/// (version.h.in's own header comment), so it is never vendored in
/// generated form.
#[cfg(feature = "vendored")]
fn render_version_h(tmpl: &Path, generated: &Path, v: (u32, u32, u32)) {
    let text = fs::read_to_string(tmpl).expect("version.h.in template");
    let rendered = text
        .replace("@PROJECT_VERSION_MAJOR@", &v.0.to_string())
        .replace("@PROJECT_VERSION_MINOR@", &v.1.to_string())
        .replace("@PROJECT_VERSION_PATCH@", &v.2.to_string())
        .replace("@PROJECT_VERSION@", &format!("{}.{}.{}", v.0, v.1, v.2));
    let hdr = generated.join("yeptris/version.h");
    fs::create_dir_all(hdr.parent().unwrap()).unwrap();
    fs::write(hdr, rendered).unwrap();
}
