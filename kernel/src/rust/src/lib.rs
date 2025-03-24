#![no_std]
use core::arch::asm;
use core::panic::PanicInfo;
use core::ptr;

unsafe extern "C" {
    pub fn k_printf(fmt: *const u8, ...);
}

#[repr(C, packed)]
#[derive(Debug)]
pub struct Rsdp {
    signature: [u8; 8],
    checksum: u8,
    oem_id: [u8; 6],
    revision: u8,
    rsdt_address: u32, // ACPI 1.0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn make_rsdp(rsdp_addr: *const u8) -> Rsdp {
    unsafe {
        if rsdp_addr.is_null() {
            panic!("rsdp ptr null");
        }

        let rsdp = &*(rsdp_addr as *const Rsdp); // safe if properly aligned, but risky

        let rsdt_address = ptr::read_unaligned((rsdp_addr as usize + 16) as *const u32);

        Rsdp {
            signature: rsdp.signature,
            checksum: rsdp.checksum,
            oem_id: rsdp.oem_id,
            revision: rsdp.revision,
            rsdt_address,
        }
    }
}

static COMPLETE_PANIC: &str = "Panic! %s:%d - %s\n\0";
static INCOMPLETE_PANIC: &str = "Panic! %s:%d - no message\n\0";
static EMPTY_PANIC: &str = "Panic! No known location\n\0";

#[panic_handler]
unsafe fn panic(info: &PanicInfo) -> ! {
    if let Some(location) = info.location() {
        let file = location.file();
        let line = location.line();

        if !info.message().as_str().is_some_and(|v| v.is_empty()) {
            unsafe {
                k_printf(
                    COMPLETE_PANIC.as_ptr(),
                    file,
                    line,
                    info.message().as_str().unwrap_or("empty").as_ptr(),
                );
            }
        } else {
            unsafe {
                k_printf(INCOMPLETE_PANIC.as_ptr(), file, line);
            }
        }
    } else {
        unsafe {
            k_printf(EMPTY_PANIC.as_ptr());
        }
    }

    loop {
        unsafe {
            asm!("hlt");
        }
    }
}
