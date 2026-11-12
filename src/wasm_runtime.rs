//! C allocation ABI for the single Rust-owned WebAssembly memory.
//!
//! Each C allocation carries its Rust Layout immediately before the returned
//! pointer. This keeps C realloc/free independent of the caller's size knowledge
//! without introducing an overlapping Emscripten heap or sbrk.
use std::alloc::{alloc, alloc_zeroed, dealloc, realloc as rust_realloc, Layout};
use std::ptr;

const ALIGN: usize = 16;
const HEADER: usize = ALIGN;

fn layout(size: usize) -> Option<Layout> {
    Layout::from_size_align(size.max(1).checked_add(HEADER)?, ALIGN).ok()
}

unsafe fn allocate(size: usize, zero: bool) -> *mut u8 {
    let Some(layout) = layout(size) else {
        return ptr::null_mut();
    };
    let base = if zero {
        alloc_zeroed(layout)
    } else {
        alloc(layout)
    };
    if base.is_null() {
        return base;
    }
    base.cast::<usize>().write(layout.size());
    base.add(HEADER)
}

#[no_mangle]
pub unsafe extern "C" fn malloc(size: usize) -> *mut u8 {
    allocate(size, false)
}

#[no_mangle]
pub unsafe extern "C" fn calloc(count: usize, size: usize) -> *mut u8 {
    match count.checked_mul(size) {
        Some(size) => allocate(size, true),
        None => ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn free(p: *mut u8) {
    if !p.is_null() {
        let base = p.sub(HEADER);
        let total = base.cast::<usize>().read();
        dealloc(base, Layout::from_size_align_unchecked(total, ALIGN));
    }
}

#[export_name = "realloc"]
pub unsafe extern "C" fn realloc_c(p: *mut u8, size: usize) -> *mut u8 {
    if p.is_null() {
        return malloc(size);
    }
    if size == 0 {
        free(p);
        return ptr::null_mut();
    }
    let Some(new) = layout(size) else {
        return ptr::null_mut();
    };
    let base = p.sub(HEADER);
    let old_total = base.cast::<usize>().read();
    let old = Layout::from_size_align_unchecked(old_total, ALIGN);
    let grown = rust_realloc(base, old, new.size());
    if grown.is_null() {
        return grown;
    }
    grown.cast::<usize>().write(new.size());
    grown.add(HEADER)
}
