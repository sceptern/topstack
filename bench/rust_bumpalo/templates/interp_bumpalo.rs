// interp with bumpalo: an enum with &Expr children in one Bump (24-byte nodes).
#[path = "../bench.rs"] mod bench;
use bench::*;
use bumpalo::Bump;

const DEPTH: i64 = @N@;
const PASSES: i64 = 8;
const M: i64 = 1000003;

enum Expr<'a> {
    Num(i32),
    Add(&'a Expr<'a>, &'a Expr<'a>),
    Mul(&'a Expr<'a>, &'a Expr<'a>),
    Neg(&'a Expr<'a>),
}

fn build<'a>(b: &'a Bump, count: &mut i64, depth: i64, seed: u64) -> &'a Expr<'a> {
    let s = xs_next(seed);
    *count += 1;
    if depth == 0 { return b.alloc(Expr::Num(xs_mod(s, 1000) as i32)); }
    let k = xs_mod(s, 8);
    if k < 1 { let o = build(b, count, depth - 1, s); return b.alloc(Expr::Neg(o)); }
    let x = build(b, count, depth - 1, s);
    let y = build(b, count, depth - 1, xs_next(s));
    if k < 5 { b.alloc(Expr::Add(x, y)) } else { b.alloc(Expr::Mul(x, y)) }
}

fn eval(e: &Expr, p: i64) -> i64 {
    match e {
        Expr::Num(v) => (*v as i64 + p) % M,
        Expr::Add(l, r) => (eval(l, p) + eval(r, p)) % M,
        Expr::Mul(l, r) => (eval(l, p) * eval(r, p)) % M,
        Expr::Neg(o) => (M - eval(o, p)) % M,
    }
}

fn main() {
    let b = Bump::new();
    let mut count: i64 = 0;
    let root = build(&b, &mut count, DEPTH, 12345);
    let mut total: i64 = 0;
    for p in 0..PASSES { total = (total + eval(root, p)) % M; }
    emit(count);
    emit(total);
}
