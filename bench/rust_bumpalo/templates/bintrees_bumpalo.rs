// bintrees with bumpalo: one Bump for the short-lived trees, reset after each
// tree (keeping its largest chunk for the next), and one for the long-lived tree.
// Nodes are two Option<&TNode> = 16 bytes, like topstack's pointer nodes.
#[path = "../bench.rs"] mod bench;
use bench::*;
use bumpalo::Bump;

const MAXDEPTH: i64 = @N@;
const MINDEPTH: i64 = 4;

struct TNode<'a> { l: Option<&'a TNode<'a>>, r: Option<&'a TNode<'a>> }

fn make<'a>(b: &'a Bump, depth: i64) -> &'a TNode<'a> {
    if depth == 0 { return b.alloc(TNode { l: None, r: None }); }
    let l = make(b, depth - 1);
    let r = make(b, depth - 1);
    b.alloc(TNode { l: Some(l), r: Some(r) })
}

fn check(n: &TNode) -> i64 {
    match (n.l, n.r) {
        (Some(l), Some(r)) => 1 + check(l) + check(r),
        _ => 1,
    }
}

fn tree_check(b: &mut Bump, depth: i64) -> i64 {
    let c = check(make(b, depth));
    b.reset();
    c
}

fn main() {
    let mut scratch = Bump::new();
    emit(tree_check(&mut scratch, MAXDEPTH + 1));
    let long_bump = Bump::new();
    let long = make(&long_bump, MAXDEPTH);
    let mut d = MINDEPTH;
    while d <= MAXDEPTH {
        let iters: i64 = 1 << (MAXDEPTH - d + MINDEPTH);
        let mut sum: i64 = 0;
        for _ in 0..iters { sum += tree_check(&mut scratch, d); }
        emit(iters);
        emit(d);
        emit(sum);
        d += 2;
    }
    emit(check(long));
}
