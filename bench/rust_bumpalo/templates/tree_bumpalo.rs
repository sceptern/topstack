// tree with bumpalo: nodes in one Bump, children made before their parent,
// linked with &TNode (24-byte nodes).
#[path = "../bench.rs"] mod bench;
use bench::*;
use bumpalo::Bump;

const DEPTH: i64 = @N@;
const PASSES: i64 = 8;

struct TNode<'a> { v: i32, l: Option<&'a TNode<'a>>, r: Option<&'a TNode<'a>> }

fn build<'a>(b: &'a Bump, depth: i64, seed: u64) -> &'a TNode<'a> {
    if depth == 0 { return b.alloc(TNode { v: xs_mod(seed, 1000) as i32, l: None, r: None }); }
    let l = build(b, depth - 1, xs_next(seed));
    let r = build(b, depth - 1, xs_next(seed + 1));
    b.alloc(TNode { v: xs_mod(seed, 1000) as i32, l: Some(l), r: Some(r) })
}

fn sum_tree(n: &TNode, p: i64) -> i64 {
    let mut s = (n.v as i64) ^ p;
    if let Some(l) = n.l { s += sum_tree(l, p); }
    if let Some(r) = n.r { s += sum_tree(r, p); }
    s
}

fn main() {
    let b = Bump::new();
    let root = build(&b, DEPTH, 12345);
    let count = (1i64 << (DEPTH + 1)) - 1;
    let mut total: i64 = 0;
    for p in 0..PASSES { total += sum_tree(root, p); }
    emit(count);
    emit(total);
}
