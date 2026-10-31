//! Checks a `game/villagers/` folder and prints one line per finding.
//!
//! `cargo run -p forge-ai --example validate -- <folder>`; the exit code is 1 when there are errors.

use std::path::PathBuf;
use std::process::ExitCode;

fn main() -> ExitCode {
    let Some(dir) = std::env::args_os().nth(1).map(PathBuf::from) else {
        eprintln!("usage: validate <game/villagers folder>");
        return ExitCode::from(2);
    };
    let report = forge_ai::validate::validate_project(&dir);
    for line in report.lines() {
        println!("{line}");
    }
    if report.has_errors() { ExitCode::from(1) } else { ExitCode::SUCCESS }
}
