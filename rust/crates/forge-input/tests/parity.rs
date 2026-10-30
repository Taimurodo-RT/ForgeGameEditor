//! A bind means the same in C++ and Rust: every line of tests/data/input_binds.txt is read here and by
//! tests/test_input.cpp, each to the same text and the same words.

use forge_input::{format, parse, words};
use std::path::Path;

#[test]
fn binds_read_as_the_cpp_version_reads_them() {
    let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/data/input_binds.txt");
    let text = std::fs::read_to_string(&file).unwrap();
    let mut lines = 0;
    for line in text.lines().filter(|l| !l.is_empty() && !l.starts_with('#')) {
        let cols: Vec<&str> = line.split('\t').collect();
        assert!(cols.len() == 2 || cols.len() == 3, "bad line {line:?}");
        match parse(cols[0]) {
            None => assert_eq!(cols[1], "-", "{:?} should name a bind", cols[0]),
            Some(b) => {
                assert_eq!(format(&b), cols[1], "text of {:?}", cols[0]);
                assert_eq!(words(&b), cols[2], "words of {:?}", cols[0]);
            }
        }
        lines += 1;
    }
    assert!(lines > 50);
}
