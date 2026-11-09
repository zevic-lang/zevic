// Built as a static library: `rustc --crate-type staticlib fib.rs`
#[no_mangle]
pub extern "C" fn rs_fib(n: u32) -> u64 {
    let (mut a, mut b) = (0u64, 1u64);
    for _ in 0..n {
        let t = a + b;
        a = b;
        b = t;
    }
    a
}
