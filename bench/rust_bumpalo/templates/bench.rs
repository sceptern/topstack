// The same helpers as Goose's bench/rust/bench.rs.
#![allow(dead_code)]
pub fn xs_next(x: u64) -> u64 { let mut v = x; v ^= v << 13; v ^= v >> 7; v ^= v << 17; v }
pub fn xs_mod(x: u64, k: i64) -> i64 { (x % k as u64) as i64 }
pub fn emit(v: i64) { println!("{}", v); }
