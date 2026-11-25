//! Load-independent marshaling-cost instrument (yeptris-rs#104):
//! counts HOST-side allocations and bytes for one json::parse versus
//! serde_json::from_slice over the same fixture, plus the C-face DOM
//! parse alone (its engine allocations are invisible here — the
//! engine allocator is C-side; host counts isolate the binding's own
//! share). Allocation counts do not drift with machine load, unlike
//! ips, so this publishes from a loaded box. Prints only — never
//! asserts on counts.
//!
//! Fixture: the same generated corpus as perf_json.rs (the
//! serialbench medium shape) unless YEPTRIS_JSON_BENCH_FIXTURE is set.

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, Ordering};

use yeptris::{Document, Value};

static ALLOCS: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU64 = AtomicU64::new(0);

struct Counting;

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ALLOCS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(layout.size() as u64, Ordering::Relaxed);
        System.alloc(layout)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        System.dealloc(ptr, layout)
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        ALLOCS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(new_size as u64, Ordering::Relaxed);
        System.realloc(ptr, layout, new_size)
    }
}

#[global_allocator]
static GLOBAL: Counting = Counting;

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

/// Reset, run `f`, return (allocs, bytes). A few untimed reads
/// beforehand warm lazy statics so the counts are the measured call's.
fn count<T>(f: impl Fn() -> T) -> (u64, u64) {
    // one untimed pass so lazily-initialized state (engine handle
    // pools, serde caches) is not attributed to the measured pass
    std::hint::black_box(f());
    ALLOCS.store(0, Ordering::Relaxed);
    BYTES.store(0, Ordering::Relaxed);
    std::hint::black_box(f());
    (
        ALLOCS.load(Ordering::Relaxed),
        BYTES.load(Ordering::Relaxed),
    )
}

fn report(label: &str, allocs: u64, bytes: u64) {
    println!("  {label:<42} {allocs:>8} allocs  {bytes:>10} bytes");
}

#[test]
fn marshal_alloc_counts() {
    let json_str = match std::env::var("YEPTRIS_JSON_BENCH_FIXTURE") {
        Ok(path) => String::from_utf8(std::fs::read(&path).expect("fixture")).expect("utf8"),
        Err(_) => corpus(),
    };
    let data = json_str.as_bytes();
    println!(
        "\nhost-side allocation counts per operation ({:?} bytes)",
        data.len()
    );

    let (a, b) = count(|| Document::parse_json(data).expect("C-face parse"));
    report("yeptris Document::parse_json (C face only)", a, b);

    let (a, b) = count(|| {
        let doc = Document::parse_json(data).expect("C-face parse");
        Value::from_document(&doc).expect("marshal")
    });
    report("yeptris marshal Value::from_document only", a, b);

    let (a, b) = count(|| yeptris::json::parse(&json_str).expect("parse"));
    report("yeptris json::parse (C face + marshal)", a, b);

    let (a, b) = count(|| serde_json::from_slice::<serde_json::Value>(data).expect("serde parse"));
    report("serde_json::from_slice (total host cost)", a, b);
}
