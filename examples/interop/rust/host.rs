use std::ffi::CString;
use std::os::raw::{c_char, c_int};
extern "C" {
    fn zevic_init(argc: c_int, argv: *mut *mut c_char);
    fn zv_word_count(s: *const c_char) -> c_int;
    fn zv_clamp(x: c_int, lo: c_int, hi: c_int) -> c_int;
}
fn main() {
    let s = CString::new("the quick brown fox").unwrap();
    unsafe {
        zevic_init(0, std::ptr::null_mut());
        println!("words={} clamp={}", zv_word_count(s.as_ptr()), zv_clamp(50, 0, 10));
    }
}
