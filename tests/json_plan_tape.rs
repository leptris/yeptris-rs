//! The JSON plan + raw tape faces (yeptris-rs#135 — the C/Ruby/
//! Python/JS parity set): `json::Plan` (compiled-descriptor walk over
//! the tape leg) and `json::Tape` (the columnar parse without a tree).
//!
//! The DOM leg of the plan face lives in `plan.rs` (the two legs
//! share one compiled handle per spec); here the tape leg is pinned
//! on the serialbench config shape — a root mapping with a nested
//! rows container — the shape issue #430 priced.

use yeptris::json::{record, Plan, Tape, TapeColumns, WalkResult};
use yeptris::plan::PlanColumnKind;
use yeptris::{Document, ErrorKind};

/// The #430-style config shape: a root mapping whose `users` key
/// holds the rows container (with a nested non-row mapping riding
/// along, so the plan must stay on its path).
fn config() -> String {
    r#"{
  "name": "engine",
  "server": {"host": "127.0.0.1", "port": 8080},
  "users": [
    {"id": 1, "name": "ada", "active": true, "score": 9.5},
    {"id": 2, "name": "grace", "active": false, "score": 8},
    {"id": 3, "name": "linus", "score": null}
  ]
}"#
    .to_string()
}

fn users_plan() -> Plan {
    Plan::compile(
        r#"{"kind":"map","path":"users","children":[
            {"name":"id","kind":"int"},
            {"name":"name","kind":"str"},
            {"name":"active","kind":"bool"},
            {"name":"score","kind":"float"}]}"#,
    )
    .unwrap()
}

#[test]
fn plan_walks_the_nested_config_shape() {
    let data = config();
    let plan = users_plan();
    assert_eq!(plan.column_count(), 4);

    let cols = plan.walk(&data).unwrap();
    assert_eq!(cols.rows(), 3);

    assert_eq!(cols.column_kind(0), Ok(PlanColumnKind::Int));
    assert_eq!(cols.ints(0).unwrap(), &[1, 2, 3]);

    assert_eq!(cols.strs(1).unwrap(), ["ada", "grace", "linus"]);

    // BOOL rides the int lane (0/1); the missing `active` marks null.
    assert_eq!(&cols.ints(2).unwrap()[..2], &[1, 0]);
    assert_eq!(cols.nulls(2), &[0, 0, 1]);

    // FLOAT: the `8` row casts at the C leg; the explicit null marks.
    assert_eq!(&cols.floats(3).unwrap()[..2], &[9.5, 8.0]);
    assert_eq!(cols.nulls(3), &[0, 0, 1]);
}

#[test]
fn plan_walk_segmented_path_walks_nested_rows() {
    let data = r#"{"cluster":{"nodes":[
        {"id": 10, "up": true}, {"id": 11, "up": false}]}}"#;
    let plan = Plan::compile(
        r#"{"kind":"map","path":["cluster","nodes"],"children":[
            {"name":"id","kind":"int"},{"name":"up","kind":"bool"}]}"#,
    )
    .unwrap();
    let cols = plan.walk(data).unwrap();
    assert_eq!(cols.rows(), 2);
    assert_eq!(cols.ints(0).unwrap(), &[10, 11]);
    assert_eq!(cols.ints(1).unwrap(), &[1, 0]);
    assert_eq!(cols.nulls(1), &[0, 0]);
}

#[test]
fn plan_walk_seq_root_rows() {
    let data = r#"[{"v": 1}, {"v": 2}, {"v": 3}]"#;
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"v","kind":"int"}]}"#).unwrap();
    let cols = plan.walk(data).unwrap();
    assert_eq!(cols.rows(), 3);
    assert_eq!(cols.ints(0).unwrap(), &[1, 2, 3]);
}

/// The two legs share the compiled plan: the tape route's columns and
/// the DOM route's (over the same bytes parsed as YAML — JSON is a
/// YAML subset) must agree cell for cell.
#[test]
fn tape_leg_agrees_with_the_document_leg() {
    let data = config();
    let tape_cols = users_plan().walk(&data).unwrap();
    let doc = Document::parse(data.as_bytes()).unwrap();
    let dom_cols = doc
        .plan_walk(
            &yeptris::plan::Plan::compile(
                r#"{"kind":"map","path":"users","children":[
            {"name":"id","kind":"int"},
            {"name":"name","kind":"str"},
            {"name":"active","kind":"bool"},
            {"name":"score","kind":"float"}]}"#,
            )
            .unwrap(),
        )
        .unwrap();

    for col in 0..4 {
        assert_eq!(
            tape_cols.column_kind(col).unwrap(),
            dom_cols.column_kind(col).unwrap(),
            "kind drift on column {col}"
        );
        assert_eq!(
            tape_cols.nulls(col),
            dom_cols.nulls(col),
            "null drift on {col}"
        );
        // Null slots carry UNDEFINED lane values (only the null bitmap
        // is the contract — tests/plan.rs pins the same law), so the
        // leg agreement is asserted on the FILLED slots only.
        let nulls = tape_cols.nulls(col);
        match tape_cols.column_kind(col).unwrap() {
            PlanColumnKind::Int | PlanColumnKind::Bool => {
                let t = tape_cols.ints(col).unwrap();
                let d = dom_cols.ints(col).unwrap();
                for i in 0..nulls.len() {
                    if nulls[i] == 0 {
                        assert_eq!(t[i], d[i], "int drift on col {col} row {i}");
                    }
                }
            }
            PlanColumnKind::Float => {
                let t = tape_cols.floats(col).unwrap();
                let d = dom_cols.floats(col).unwrap();
                for i in 0..nulls.len() {
                    if nulls[i] == 0 {
                        assert_eq!(t[i], d[i], "float drift on col {col} row {i}");
                    }
                }
            }
            PlanColumnKind::Str => {
                let t = tape_cols.strs(col).unwrap();
                let d_cells = dom_cols.strs(col).unwrap();
                for i in 0..nulls.len() {
                    if nulls[i] == 0 {
                        assert_eq!(t[i], d_cells[i].as_str(), "str drift on col {col} row {i}");
                    }
                }
            }
        }
    }
}

/// A leaf whose value's type disagrees with the column marks the slot
/// null (plan.c fills only the matching shape; the null bitmap is the
/// contract — "nil where null, missing, or shape-mismatched").
#[test]
fn shape_mismatched_leaf_marks_the_slot_null() {
    let data = r#"[{"v": 7}, {"v": "x"}, {"v": 9}]"#;
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"v","kind":"int"}]}"#).unwrap();
    let cols = plan.walk(data).unwrap();
    assert_eq!(cols.nulls(0), &[0, 1, 0]);
    assert_eq!(&cols.ints(0).unwrap()[..1], &[7]);
    assert_eq!(cols.ints(0).unwrap()[2], 9);
}

/// The C contract: an empty rows container walks to a zero-row result
/// (the container was FOUND); a MISSING path key is the shape
/// disagreement.
#[test]
fn empty_rows_container_walks_to_zero_rows() {
    let plan = users_plan();
    let cols = plan.walk(r#"{"users": []}"#).unwrap();
    assert_eq!(cols.rows(), 0);
    // every typed lane reads as empty on the found-but-empty container
    assert_eq!(cols.ints(0).unwrap(), &[] as &[i64]);
    assert_eq!(cols.floats(3).unwrap(), &[] as &[f64]);
    assert_eq!(cols.strs(1).unwrap(), [] as [&str; 0]);
    assert_eq!(cols.nulls(0), &[] as &[u8]);
    // the kind/lane mismatch rule holds on the empty result too
    assert!(cols.ints(1).is_err()); // col 1 is the STR column
                                    // the path key absent entirely: shape disagreement
    let err = plan.walk(r#"{"name": "x"}"#).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
}

#[test]
fn spec_and_shape_errors_surface() {
    // malformed spec
    let err = Plan::compile("{not json").unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
    // the document's shape disagrees with the plan
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"v","kind":"int"}]}"#).unwrap();
    let err = plan.walk(r#"{"v": 1}"#).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
    // malformed JSON data
    let plan = users_plan();
    let err = plan.walk(r#"{"users": [{"id": 1,}"#).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
}

#[test]
fn column_kind_of_a_bad_index_is_an_error() {
    let data = config();
    let cols = users_plan().walk(&data).unwrap();
    assert!(cols.column_kind(9).is_err());
    assert!(cols.ints(9).is_err());
    assert!(cols.floats(9).is_err());
    assert!(cols.strs(9).is_err());
}

/// The plan face borrows the walked data: string cells stay raw spans
/// (escape sequences untouched — the tape face's contract); the
/// decoded read is the Value route's.
#[test]
fn str_cells_are_raw_spans_into_the_walked_data() {
    let data = "[{\"v\": \"café\"}, {\"v\": \"caf\\u00e9\"}, {\"v\": \"b\"}]";
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"v","kind":"str"}]}"#).unwrap();
    let cols = plan.walk(data).unwrap();
    // cell 0 is the literal UTF-8 spelling, cell 1 keeps the escape
    // sequence RAW (the tape face's contract)
    assert_eq!(cols.strs(0).unwrap(), ["café", r"caf\u00e9", "b"]);
    // the decoded twin rides the Value route (items are row mappings)
    let v = yeptris::json::parse(data).unwrap();
    let items = match v {
        yeptris::Value::Sequence(items) => items,
        other => panic!("expected a sequence, got {other:?}"),
    };
    let row = match &items[1] {
        yeptris::Value::Mapping(m) => m,
        other => panic!("expected a row mapping, got {other:?}"),
    };
    assert_eq!(
        row.get(&yeptris::Value::String("v".into())).unwrap(),
        &yeptris::Value::String("café".into())
    );
}

// ---- the raw tape face -------------------------------------------------

/// The tape of the config shape: one record per token, the DOC
/// boundary first, containers mutually linked, spans pointing into
/// the source.
#[test]
fn tape_records_the_config_shape() {
    let data = config();
    let tape = Tape::parse(&data).unwrap();
    let cols: TapeColumns = tape.columns().unwrap();

    assert_eq!(cols.count(), tape.count());
    assert_eq!(record::DOC, cols.kinds()[0]);
    assert_eq!(record::MAP_OPEN, cols.kinds()[1]);

    // the root mapping's first pair rides records 2 (KEY "name") and
    // 3 (VALUE "engine") — spans into the pretty-printed source
    let strs: Vec<usize> = cols
        .kinds()
        .iter()
        .enumerate()
        .filter(|(_, &k)| k == record::STR)
        .map(|(i, _)| i)
        .collect();
    let (o, l) = (cols.offs()[strs[0]] as usize, cols.lens()[strs[0]] as usize);
    assert_eq!(&data[o..o + l], "name");
    let (o, l) = (cols.offs()[strs[1]] as usize, cols.lens()[strs[1]] as usize);
    assert_eq!(&data[o..o + l], "engine");

    // containers link mutually: the users SEQ_OPEN's off points at
    // its CLOSE, and back
    let open = cols
        .kinds()
        .iter()
        .position(|&k| k == record::SEQ_OPEN)
        .unwrap();
    let close = cols.offs()[open] as usize;
    assert_eq!(record::CLOSE, cols.kinds()[close]);
    assert_eq!(open, cols.offs()[close] as usize);
}

/// The span contract: escapes stay RAW in the tape (the consumer
/// converts), and the spans borrow the caller's buffer.
#[test]
fn tape_spans_are_raw() {
    let data = r#"{"s": "caf\u00e9"}"#;
    let tape = Tape::parse(data).unwrap();
    let cols = tape.columns().unwrap();
    let s = cols.kinds().iter().position(|&k| k == record::STR).unwrap();
    // record 2 is the KEY ("s"), record 3 the value
    let (o, l) = (cols.offs()[s + 1] as usize, cols.lens()[s + 1] as usize);
    assert_eq!(cols.kinds()[s + 1], record::STR);
    // the escape stays RAW: the span is the eight ASCII bytes
    assert_eq!(&data[o..o + l], r"caf\u00e9");
}

/// Number records carry text spans only — conversion is the
/// consumer's move (the Value route does it in one bulk call).
#[test]
fn number_records_carry_spans() {
    let data = r#"[12, 1.5]"#;
    let tape = Tape::parse(data).unwrap();
    let cols = tape.columns().unwrap();
    let int_rec = cols.kinds().iter().position(|&k| k == record::INT).unwrap();
    let float_rec = cols
        .kinds()
        .iter()
        .position(|&k| k == record::FLOAT)
        .unwrap();
    for rec in [int_rec, float_rec] {
        let (o, l) = (cols.offs()[rec] as usize, cols.lens()[rec] as usize);
        assert_eq!(&data[o..o + l], if rec == int_rec { "12" } else { "1.5" });
    }
}

#[test]
fn tape_scalar_roots_ride_one_record() {
    let tape = Tape::parse("42").unwrap();
    assert_eq!(tape.count(), 2); // DOC boundary + the scalar
    let cols = tape.columns().unwrap();
    assert_eq!(cols.kinds(), &[record::DOC, record::INT]);
    assert_eq!(Tape::parse("null").unwrap().count(), 2);
    assert_eq!(Tape::parse("true").unwrap().count(), 2);
}

#[test]
fn tape_rejects_malformed_input() {
    let err = Tape::parse(r#"{"k": [1,}"#).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
    assert!(err.message().is_some(), "the engine reports a position");
}

/// The plan walk rides the same compiled handle as the DOM leg; the
/// spec failures land identically on both doors (one owner of the
/// compile contract).
#[test]
fn walk_result_type_is_named_for_the_sketch() {
    // the issue sketch's surface: compile -> walk -> typed rows
    let plan = yeptris::json::Plan::compile(
        r#"{"kind":"map","path":"users","children":[
            {"name":"id","kind":"int"}]}"#,
    )
    .unwrap();
    let rows: WalkResult = plan.walk(r#"{"users":[{"id":1},{"id":2}]}"#).unwrap();
    assert_eq!(rows.rows(), 2);
    assert_eq!(rows.ints(0).unwrap(), &[1, 2]);
}
