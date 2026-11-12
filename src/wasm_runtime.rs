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

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn malloc(size: usize) -> *mut u8 {
    allocate(size, false)
}

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn calloc(count: usize, size: usize) -> *mut u8 {
    match count.checked_mul(size) {
        Some(size) => allocate(size, true),
        None => ptr::null_mut(),
    }
}

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn free(p: *mut u8) {
    if !p.is_null() {
        let base = p.sub(HEADER);
        let total = base.cast::<usize>().read();
        dealloc(base, Layout::from_size_align_unchecked(total, ALIGN));
    }
}

#[cfg_attr(target_arch = "wasm32", export_name = "realloc")]
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn c_allocation_contract() {
        unsafe {
            assert!(calloc(usize::MAX, 2).is_null());
            assert!(malloc(usize::MAX).is_null());
            free(ptr::null_mut());
            let p = calloc(33, 3);
            assert!(!p.is_null());
            assert_eq!(p as usize % ALIGN, 0);
            assert!(std::slice::from_raw_parts(p, 99).iter().all(|b| *b == 0));
            for i in 0..99 {
                p.add(i).write(i as u8);
            }
            assert!(realloc_c(p, usize::MAX).is_null());
            for i in 0..99 {
                assert_eq!(p.add(i).read(), i as u8);
            }
            let p = realloc_c(p, 32768);
            assert!(!p.is_null());
            for i in 0..99 {
                assert_eq!(p.add(i).read(), i as u8);
            }
            let p = realloc_c(p, 33);
            assert!(!p.is_null());
            for i in 0..33 {
                assert_eq!(p.add(i).read(), i as u8);
            }
            assert!(realloc_c(p, 0).is_null());
            let p = realloc_c(ptr::null_mut(), 0);
            assert!(!p.is_null());
            free(p);
        }
    }
}
