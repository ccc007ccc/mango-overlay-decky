fn main() {
    println!("cargo:rerun-if-env-changed=MANGO_OVERLAY_CLIENT_LIB_DIR");
    if let Ok(directory) = std::env::var("MANGO_OVERLAY_CLIENT_LIB_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
    }
    println!("cargo:rustc-link-lib=dylib=mango-overlay-client");
}
