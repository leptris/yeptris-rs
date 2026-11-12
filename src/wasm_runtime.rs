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

// These are the only host hooks retained by the pinned single-thread libc.
// Keep them inside the module: consumers do not need an Emscripten/WASI host.
#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn emscripten_builtin_malloc(size: usize) -> *mut u8 {
    malloc(size)
}

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn emscripten_memcpy_js(dst: *mut u8, src: *const u8, len: usize) {
    // Rust's wasm target enables bulk-memory; this lowers to memory.copy rather
    // than recursively entering Emscripten's large-copy JavaScript fallback.
    ptr::copy(src, dst, len);
}

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub extern "C" fn getenv(_name: *const u8) -> *mut u8 {
    // Browser instances have no process environment. In particular libc must
    // not discover host locale settings or allocate an environment from JS.
    ptr::null_mut()
}

// The C engine exposes memory-based APIs only. Musl's generic printf backend
// retains descriptor helpers even for snprintf. Reject every descriptor: do
// not fabricate successful output or add filesystem capabilities to browsers.
const BADF: u16 = 8; // Emscripten 3.1.50 wasi/api.h: __WASI_ERRNO_BADF.

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub extern "C" fn __wasi_fd_close(_fd: u32) -> u16 {
    BADF
}

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn __wasi_fd_write(
    _fd: u32,
    _iovs: *const u8,
    _iovs_len: usize,
    written: *mut usize,
) -> u16 {
    if !written.is_null() {
        written.write(0);
    }
    BADF
}

#[cfg_attr(target_arch = "wasm32", no_mangle)]
pub unsafe extern "C" fn __wasi_fd_seek(
    _fd: u32,
    _offset: i64,
    _whence: u8,
    new_offset: *mut u64,
) -> u16 {
    if !new_offset.is_null() {
        new_offset.write(0);
    }
    BADF
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn browser_runtime_has_no_host_environment_or_descriptors() {
        unsafe {
            assert!(getenv(b"LANG\0".as_ptr()).is_null());
            let mut written = 123;
            assert_eq!(__wasi_fd_write(1, ptr::null(), 0, &mut written), BADF);
            assert_eq!(written, 0);
            let mut offset = 123;
            assert_eq!(__wasi_fd_seek(0, 0, 0, &mut offset), BADF);
            assert_eq!(offset, 0);
            assert_eq!(__wasi_fd_close(0), BADF);
            let p = emscripten_builtin_malloc(1024);
            assert!(!p.is_null());
            let input = vec![42_u8; 1024];
            emscripten_memcpy_js(p, input.as_ptr(), input.len());
            assert_eq!(std::slice::from_raw_parts(p, input.len()), input.as_slice());
            free(p);
            let mut overlap = [1_u8; 1025];
            overlap[0] = 42;
            emscripten_memcpy_js(overlap.as_mut_ptr().add(1), overlap.as_ptr(), 1024);
            assert_eq!(overlap[1], 42);
            assert!(overlap[2..].iter().all(|b| *b == 1));
        }
    }

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
