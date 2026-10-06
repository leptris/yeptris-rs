//! The compiled-plan walk (yeptris/plan.h): a JSON spec compiled
//! once, applied to a parsed document in one C pass, producing
//! typed columnar output.

use yeptris::plan::{Plan, PlanColumnKind};

#[test]
fn map_root_with_path_selects_rows_and_types_them() {
    let doc = yeptris::Document::parse(
        b"meta: {skip: 1}\nitems:\n  - {id: 7, tag: x, ok: true, score: 1.5}\n  - {id: 8, tag: y, ok: false, score: 2}\n  - {id: 9, tag: z}\n",
    )
    .unwrap();
    let plan = Plan::compile(
        r#"{"kind":"map","path":"items","children":[
            {"name":"id","kind":"int"},
            {"name":"tag","kind":"str"},
            {"name":"ok","kind":"bool"},
            {"name":"score","kind":"float"}]}"#,
    )
    .unwrap();
    assert_eq!(plan.column_count(), 4);

    let cols = doc.plan_walk(&plan).unwrap();
    assert_eq!(cols.rows(), 3);
    assert_eq!(cols.column_kind(0), Ok(PlanColumnKind::Int));

    let ids = cols.ints(0).unwrap();
    assert_eq!(ids, &[7, 8, 9]);

    let cells = cols.strs(1).unwrap();
    let tags: Vec<&str> = cells.iter().map(|s| s.as_str()).collect();
    assert_eq!(tags, ["x", "y", "z"]);

    // BOOL columns ride the int array (0/1). Null slots carry
    // UNDEFINED values — only the null bitmap is the contract
    // (Linux surfaced stale heap bytes there); assert the filled
    // slots only.
    let oks = cols.ints(2).unwrap();
    assert_eq!(&oks[..2], &[1, 0]);
    assert_eq!(cols.nulls(2), &[0, 0, 1]);

    let scores = cols.floats(3).unwrap();
    assert_eq!(&scores[..2], &[1.5, 2.0]);
    assert_eq!(cols.nulls(3), &[0, 0, 1]);
}

#[test]
fn explicit_null_marks_the_slot_null() {
    let doc = yeptris::Document::parse(b"- v: null\n- v: 5\n").unwrap();
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"v","kind":"int"}]}"#).unwrap();
    let cols = doc.plan_walk(&plan).unwrap();
    assert_eq!(cols.nulls(0), &[1, 0]);
    assert_eq!(cols.ints(0).unwrap()[1], 5);
}

#[test]
fn malformed_spec_is_rejected() {
    assert!(Plan::compile("{not json").is_err());
}

#[test]
fn shape_disagreement_is_a_parse_error() {
    let doc = yeptris::Document::parse(b"name: scalar\n").unwrap();
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"id","kind":"int"}]}"#).unwrap();
    assert!(doc.plan_walk(&plan).is_err());
}

#[test]
fn column_kind_of_a_bad_index_is_an_error() {
    let doc = yeptris::Document::parse(b"- {v: 1}\n").unwrap();
    let plan = Plan::compile(r#"{"kind":"seq","children":[{"name":"v","kind":"int"}]}"#).unwrap();
    let cols = doc.plan_walk(&plan).unwrap();
    assert!(cols.column_kind(9).is_err());
    assert!(cols.ints(9).is_err());
    assert!(cols.floats(9).is_err());
    assert!(cols.strs(9).is_err());
}
