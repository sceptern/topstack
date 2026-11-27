// sexp with bumpalo: nodes in one Bump, linked with &Sexp, first-child /
// previous-sibling as in the other versions; symbol names borrow the source text
// (&str), as in Goose's Rust arena version.
#[path = "../bench.rs"] mod bench;
use bench::*;
use bumpalo::Bump;

const N: i64 = @N@;
const DEPTH: i64 = 4;
const PASSES: i64 = 4;

fn emit_sym(text: &mut String, seed: u64) {
    let mut r = seed;
    let n = 3 + xs_mod(r, 12);
    for _ in 0..n { r = xs_next(r); text.push((b'a' + xs_mod(r, 26) as u8) as char); }
}

fn emit_num(text: &mut String, v: i64) {
    if v == 0 { text.push('0'); return; }
    let mut tmp = [0u8; 24];
    let (mut k, mut d) = (0usize, v);
    while d > 0 { tmp[k] = b'0' + (d % 10) as u8; k += 1; d /= 10; }
    while k > 0 { k -= 1; text.push(tmp[k] as char); }
}

fn gen(text: &mut String, depth: i64, seed: u64) -> u64 {
    let mut r = xs_next(seed);
    if depth == 0 || xs_mod(r, 4) == 0 {
        if xs_mod(r >> 8, 2) == 0 { emit_sym(text, r); }
        else { emit_num(text, xs_mod(r >> 16, 100000)); }
        text.push(' ');
        return r;
    }
    text.push('(');
    let kids = 2 + xs_mod(r >> 24, 4);
    for _ in 0..kids { r = gen(text, depth - 1, r); }
    text.push(')');
    text.push(' ');
    r
}

enum Sexp<'a> {
    Sym { name: &'a str, next: Option<&'a Sexp<'a>> },
    Num { v: i32, next: Option<&'a Sexp<'a>> },
    List { first: Option<&'a Sexp<'a>>, next: Option<&'a Sexp<'a>> },
}

fn skip_space(b: &[u8], pos: &mut usize) { while *pos < b.len() && b[*pos] == b' ' { *pos += 1; } }

fn parse_node<'a>(bump: &'a Bump, text: &'a str, pos: &mut usize, count: &mut i64,
                  prev: Option<&'a Sexp<'a>>) -> &'a Sexp<'a> {
    let b = text.as_bytes();
    skip_space(b, pos);
    *count += 1;
    let c = b[*pos];
    if c == b'(' {
        *pos += 1;
        let mut chain: Option<&'a Sexp<'a>> = None;
        loop {
            skip_space(b, pos);
            if b[*pos] == b')' { *pos += 1; break; }
            chain = Some(parse_node(bump, text, pos, count, chain));
        }
        return bump.alloc(Sexp::List { first: chain, next: prev });
    }
    let start = *pos;
    if c.is_ascii_digit() {
        let mut v: i64 = 0;
        while *pos < b.len() && b[*pos].is_ascii_digit() {
            v = v * 10 + (b[*pos] - b'0') as i64;
            *pos += 1;
        }
        return bump.alloc(Sexp::Num { v: v as i32, next: prev });
    }
    while *pos < b.len() && b[*pos] != b' ' && b[*pos] != b'(' && b[*pos] != b')' { *pos += 1; }
    bump.alloc(Sexp::Sym { name: &text[start..*pos], next: prev })
}

fn next_of<'a>(n: &Sexp<'a>) -> Option<&'a Sexp<'a>> {
    match n { Sexp::Sym { next, .. } | Sexp::Num { next, .. } | Sexp::List { next, .. } => *next }
}

fn walk(n: &Sexp, p: i64) -> i64 {
    match n {
        Sexp::Sym { name, .. } => name.len() as i64 + name.as_bytes()[0] as i64 + p,
        Sexp::Num { v, .. } => (*v as i64) ^ p,
        Sexp::List { first, .. } => 1 + walk_chain(*first, p),
    }
}

fn walk_chain(c: Option<&Sexp>, p: i64) -> i64 {
    match c { None => 0, Some(n) => walk(n, p) + walk_chain(next_of(n), p) }
}

fn main() {
    let mut text = String::new();
    let mut seed: u64 = 12345;
    for _ in 0..N { seed = gen(&mut text, DEPTH, seed); }
    let bump = Bump::new();
    let mut pos = 0usize;
    let mut count: i64 = 0;
    let mut roots: Vec<&Sexp> = Vec::new();
    loop {
        skip_space(text.as_bytes(), &mut pos);
        if pos >= text.len() { break; }
        roots.push(parse_node(&bump, &text, &mut pos, &mut count, None));
    }
    emit(text.len() as i64);
    emit(count);
    emit(roots.len() as i64);
    emit(0);
    let mut total: i64 = 0;
    for p in 0..PASSES {
        for &r in &roots { total += walk(r, p); }
    }
    emit(total);
}
