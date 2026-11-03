//! Route comparison for the serde deserialize face (yeptris-rs#41):
//! the legacy owned-`Value` walk (pre-#41 `from_str`) versus the
//! direct Node deserializer (the new `from_str`). Prints MB/s for
//! both routes over deterministic corpora — small config, mid map,
//! wide row set. Correctness is the differential gate's job; this
//! test only prints, it never asserts on timing (CI machines vary).

use serde::Deserialize;
use std::collections::HashMap;
use std::time::{Duration, Instant};

use yeptris::Value;

#[derive(Debug, Deserialize)]
#[allow(dead_code)] // the field extraction IS the workload
struct Row {
    id: u64,
    name: String,
    ratio: f64,
    on: bool,
    tags: Vec<String>,
    note: String,
}

#[derive(Debug, Deserialize)]
#[allow(dead_code)] // the field extraction IS the workload
struct Small {
    name: String,
    port: u16,
    debug: bool,
    tags: Vec<String>,
    tls: Option<Tls>,
}

#[derive(Debug, Deserialize)]
#[allow(dead_code)] // the field extraction IS the workload
struct Tls {
    cert: String,
    alpn: Vec<String>,
}

/// ~190 KB: 2000 top-level rows of a config-shaped mapping (nested
/// seqs, mixed scalar types, quoted strings) — the wide-map shape the
/// audit flagged, and the one that stresses the duplicate-key fold.
fn wide_doc() -> String {
    let mut s = String::with_capacity(200_000);
    for i in 0..2000 {
        s.push_str(&format!(
            "item{i}:\n  id: {i}\n  name: item-{i}\n  ratio: {i}.5\n  on: true\n  tags: [alpha{i}, beta]\n  note: 'quoted note {i}'\n"
        ));
    }
    s
}

/// ~120 B: the small-to-mid doc that dominates deserialize latency in
/// the audit's measurement.
fn small_doc() -> String {
    "name: engine\nport: 8080\ndebug: true\ntags: [fast, safe]\ntls:\n  cert: /etc/cert.pem\n  alpn: [h2]\n".to_string()
}

/// Legacy route, verbatim pre-#41 `from_slice`.
fn legacy<T: serde::de::DeserializeOwned>(src: &str) -> T {
    let doc = yeptris::Document::parse(src.as_bytes()).unwrap();
    let value = yeptris::Value::from_document(&doc).unwrap();
    yeptris::from_value(&value).unwrap()
}

struct BenchResult {
    mibs: f64,
    us_per_call: f64,
}

/// One INTERLEAVED comparison: legacy and direct run back-to-back
/// within the same round, and the round with the least combined time
/// wins — ambient machine load drifts between rounds, so only the
/// within-round ratio is meaningful.
fn bench_pair<T, U, F, G>(doc: &str, f: F, g: G) -> (BenchResult, BenchResult)
where
    T: 'static,
    U: 'static,
    F: Fn(&str) -> T,
    G: Fn(&str) -> U,
{
    // warmup both
    for _ in 0..10 {
        std::hint::black_box(f(doc));
        std::hint::black_box(g(doc));
    }
    let budget = Duration::from_millis(250);
    let mut best = (
        BenchResult {
            mibs: 0.0,
            us_per_call: f64::INFINITY,
        },
        f64::INFINITY,
    );
    let mut best_direct = BenchResult {
        mibs: 0.0,
        us_per_call: f64::INFINITY,
    };
    for _ in 0..5 {
        fn run(doc: &str, budget: Duration, closure: &dyn Fn(&str)) -> BenchResult {
            let mut iters = 0usize;
            let start = Instant::now();
            while start.elapsed() < budget && iters < 20_000 {
                // unit return is the point: the call must not be elided
                #[allow(clippy::unit_arg)]
                std::hint::black_box(closure(doc));
                iters += 1;
            }
            let secs = start.elapsed().as_secs_f64();
            let mibs = doc.len() as f64 * iters as f64 / secs / 1_048_576.0;
            BenchResult {
                mibs,
                us_per_call: secs * 1e6 / iters as f64,
            }
        }
        let l = run(doc, budget, &|d| {
            std::hint::black_box(f(d));
        });
        let d = run(doc, budget, &|d| {
            std::hint::black_box(g(d));
        });
        let pair = l.us_per_call + d.us_per_call;
        if pair < best.1 {
            best = (l, pair);
            best_direct = d;
        }
    }
    (best.0, best_direct)
}

fn print_table(doc_name: &str, doc_bytes: usize, legacy: &BenchResult, direct: &BenchResult) {
    let delta = (direct.mibs / legacy.mibs - 1.0) * 100.0;
    println!(
        "\n{doc_name} ({doc_bytes} bytes)\n  {:<10} {:>9.1} MiB/s  {:>9.2} us/call\n  {:<10} {:>9.1} MiB/s  {:>9.2} us/call\n  direct vs legacy: {:+.1}%",
        "legacy:", legacy.mibs, legacy.us_per_call,
        "direct:", direct.mibs, direct.us_per_call,
        delta,
    );
}

#[test]
fn deserialize_route_comparison() {
    let wide = wide_doc();
    let small = small_doc();

    let (l_wide_value, d_wide_value) = bench_pair(&wide, legacy::<Value>, |d| {
        yeptris::from_str::<Value>(d).unwrap()
    });
    print_table(
        "wide doc -> Value",
        wide.len(),
        &l_wide_value,
        &d_wide_value,
    );

    let (l_wide_typed, d_wide_typed) = bench_pair(
        &wide,
        |d| {
            let _rows: HashMap<String, Row> = legacy(d);
        },
        |d| {
            let _rows: HashMap<String, Row> = yeptris::from_str(d).unwrap();
        },
    );
    print_table(
        "wide doc -> typed rows",
        wide.len(),
        &l_wide_typed,
        &d_wide_typed,
    );

    let (l_small, d_small) = bench_pair(
        &small,
        |d| {
            let _c: Small = legacy(d);
        },
        |d| {
            let _c: Small = yeptris::from_str(d).unwrap();
        },
    );
    print_table("small doc -> typed struct", small.len(), &l_small, &d_small);
}
