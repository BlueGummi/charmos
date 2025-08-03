#![no_std]
#![feature(allocator_api)]
extern crate alloc;
use core::arch::asm;
use core::panic::PanicInfo;

unsafe extern "C" {
    pub fn k_printf(fmt: *const u8, ...);
}

unsafe extern "C" {
    pub unsafe fn kmalloc(n: usize) -> *mut u8;
    pub unsafe fn kfree(ptr: *mut u8);
}

use core::alloc::{GlobalAlloc, Layout};
const PAGE_SIZE: usize = 4096;
pub struct BitmapAllocator;

unsafe impl GlobalAlloc for BitmapAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = layout.size();
        unsafe { kmalloc(size) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, _: Layout) {
        unsafe {
            kfree(ptr);
        }
    }
}

#[global_allocator]
pub static ALLOCATOR: BitmapAllocator = BitmapAllocator;
#[unsafe(no_mangle)]
unsafe extern "C" fn test_alloc() {
    unsafe {
        let layout = Layout::new::<u32>();
        let ptr = ALLOCATOR.alloc(layout);
        if !ptr.is_null() {
            *(ptr as *mut u32) = 33;
            k_printf("allocated value: %d\n".as_ptr(), *(ptr as *mut u32));
            ALLOCATOR.dealloc(ptr, layout);
        } else {
            k_printf("allocation failed\n".as_ptr());
        }
    }
}

static COMPLETE_PANIC: &str = "Panic! %s:%d - %s\n\0";
static INCOMPLETE_PANIC: &str = "Panic! %s:%d - no message\n\0";
static EMPTY_PANIC: &str = "Panic! No known location\n\0";

#[panic_handler]
unsafe fn panic(info: &PanicInfo) -> ! {
    unsafe {
        if let Some(location) = info.location() {
            let file = location.file();
            let line = location.line();

            if !info.message().as_str().is_some_and(|v| v.is_empty()) {
                k_printf(
                    COMPLETE_PANIC.as_ptr(),
                    file,
                    line,
                    info.message().as_str().unwrap_or("empty").as_ptr(),
                );
            } else {
                k_printf(INCOMPLETE_PANIC.as_ptr(), file, line);
            }
        } else {
            k_printf(EMPTY_PANIC.as_ptr());
        }
        loop {
            asm!("hlt");
        }
    }
}
