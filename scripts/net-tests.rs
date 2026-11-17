// Compile the same pure protocol modules used by the kernel without EFI FFI.
#[path = "../src/krnl/net/dns.rs"]
mod dns;
#[path = "../src/krnl/net/http_codec.rs"]
mod http_codec;
#[path = "../src/krnl/net/tcp.rs"]
mod tcp;
