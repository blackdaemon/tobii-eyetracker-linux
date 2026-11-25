// Link against the installed Stream Engine (see ../../install.sh).
// Override the directory with TOBII_LIB_DIR=/path/to/dir.
fn main() {
    let dir = std::env::var("TOBII_LIB_DIR").unwrap_or_else(|_| "/usr/local/lib64/tobii".into());
    println!("cargo:rerun-if-env-changed=TOBII_LIB_DIR");
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=dylib=tobii_stream_engine");
    // Embed the path so binaries run without LD_LIBRARY_PATH.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
}
