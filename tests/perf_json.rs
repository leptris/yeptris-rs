//! Route comparison for the JSON face (yeptris-rs#104): the
//! marshaling-bound `json::parse` / `json::serialize` versus
//! serde-json, interleaved in one process. Prints ips for both
//! contenders over the users-shaped corpus — the serialbench fixture
//! shape — plus the DOM-face parse as the binding's C-side floor.
//! Correctness is the differential gate's job; this test only prints,
//! it never asserts on timing (CI machines vary).
//!
//! `YEPTRIS_JSON_BENCH_FIXTURE=/path/to/medium.json` swaps the
//! generated corpus for a real file (the serialbench fixtures dir).

use std::time::{Duration, Instant};

/// The serialbench medium fixture's shape, generated deterministically:
/// an array of users with nested profile/preferences — numbers,
/// ASCII strings, bools — sized to ~185 KB.
fn corpus() -> String {
    let mut s = String::with_capacity(200_000);
    s.push_str(r#"{"users":["#);
    for i in 0..1000u64 {
        if i > 0 {
            s.push(',');
        }
        s.push_str(&format!(
            r#"{{"id":{},"name":"User {}","email":"user{}@example.com","created_at":"2023-01-{}T10:00:00Z","profile":{{"age":{},"city":"City {}","preferences":{{"theme":"{}","notifications":{}}}}}}}"#,
            i + 1,
            i + 1,
            i + 1,
            (i % 27) + 2,
            20 + (i % 50),
            i % 97,
            if i % 3 == 0 { "light" } else { "dark" },
            i % 2 == 0,
        ));
    }
    s.push_str("]}");
    s
}

fn fixture_bytes() -> Vec<u8> {
    match std::env::var("YEPTRIS_JSON_BENCH_FIXTURE") {
        Ok(path) => {
            std::fs::read(&path).unwrap_or_else(|e| panic!("fixture {path} unreadable: {e}"))
        }
        Err(_) => corpus().into_bytes(),
    }
}

struct BenchResult {
    ips: f64,
    us_per_call: f64,
}

impl Clone for BenchResult {
    fn clone(&self) -> BenchResult {
        *self
    }
}

impl Copy for BenchResult {}

/// One INTERLEAVED comparison: every contender runs back-to-back
/// within the same round, and the round with the least combined time
/// wins — ambient machine load drifts between rounds, so only the
/// within-round ratios are meaningful.
fn bench(contenders: &[(&str, &dyn Fn())]) -> Vec<(String, BenchResult)> {
    // warmup everything (the closures black_box their own results)
    for (_, f) in contenders {
        for _ in 0..3 {
            f();
        }
    }
    let budget = Duration::from_millis(250);
    let mut best: Vec<(String, BenchResult)> = contenders
        .iter()
        .map(|(n, _)| {
            (
                (*n).to_owned(),
                BenchResult {
                    ips: 0.0,
                    us_per_call: f64::INFINITY,
                },
            )
        })
        .collect();
    for _ in 0..5 {
        let mut round = Vec::with_capacity(contenders.len());
        for (_, f) in contenders {
            let mut iters = 0usize;
            let start = Instant::now();
            while start.elapsed() < budget && iters < 20_000 {
                // the closures black_box their own results
                f();
                iters += 1;
            }
            let secs = start.elapsed().as_secs_f64();
            round.push(BenchResult {
                ips: iters as f64 / secs,
                us_per_call: secs * 1e6 / iters as f64,
            });
        }
        let total: f64 = round.iter().map(|r| r.us_per_call).sum();
        if total < best.iter().map(|(_, r)| r.us_per_call).sum::<f64>() {
            for (slot, r) in best.iter_mut().zip(&round) {
                slot.1 = *r;
            }
        }
    }
    best
}

fn print_table(name: &str, bytes: usize, rows: &[(String, BenchResult)]) {
    println!("\n{name} ({bytes} bytes)");
    for (label, r) in rows {
        println!(
            "  {label:<28} {:>9.0} ips  {:>9.2} us/call",
            r.ips, r.us_per_call
        );
    }
}

#[test]
fn json_face_route_comparison() {
    let data = fixture_bytes();
    let json_str = String::from_utf8(data.clone()).expect("utf8 fixture");
    let bytes = data.len();

    // parse: the three routes
    let parse = bench(&[
        ("yeptris json::parse (Value)", &|| {
            std::hint::black_box(yeptris::json::parse(&json_str).expect("parse"));
        }),
        ("Document::parse_json (DOM face)", &|| {
            std::hint::black_box(yeptris::Document::parse_json(data.as_slice()).expect("parse"));
        }),
        ("serde_json::from_slice", &|| {
            std::hint::black_box(
                serde_json::from_slice::<serde_json::Value>(&data).expect("parse"),
            );
        }),
    ]);
    print_table("json parsing", bytes, &parse);

    // generation: parse + serialize
    let generate = bench(&[
        ("yeptris parse+serialize", &|| {
            let v = yeptris::json::parse(&json_str).expect("parse");
            std::hint::black_box(yeptris::json::serialize(&v).expect("emit"));
        }),
        ("serde parse+to_string", &|| {
            let v: serde_json::Value = serde_json::from_slice(&data).expect("parse");
            std::hint::black_box(serde_json::to_string(&v).expect("emit"));
        }),
    ]);
    print_table("json generation", bytes, &generate);

    // serialize alone (Value already built)
    let v = yeptris::json::parse(&json_str).expect("parse");
    let sv: serde_json::Value = serde_json::from_slice(&data).expect("parse");
    let emit = bench(&[
        ("yeptris json::serialize", &|| {
            std::hint::black_box(yeptris::json::serialize(&v).expect("emit"));
        }),
        ("serde_json::to_string", &|| {
            std::hint::black_box(serde_json::to_string(&sv).expect("emit"));
        }),
    ]);
    print_table("json emit (value built once)", bytes, &emit);
}
