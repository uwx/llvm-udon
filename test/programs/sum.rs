#![no_std]
#![no_main]

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub static DATA: [u32; 4] = [3, 1, 4, 1];
#[no_mangle]
pub static mut LEN: u32 = 4;

#[no_mangle]
pub extern "C" fn udon_event_start() -> u32 {
    let len = unsafe { LEN } as usize;
    let base = &raw const DATA as *const u32;
    let mut sum = 0u32;
    let mut i = 0usize;
    while i < len {
        sum = sum.wrapping_add(unsafe { *base.add(i) });
        i += 1;
    }
    sum
}
