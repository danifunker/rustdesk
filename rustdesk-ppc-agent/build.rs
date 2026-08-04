// Generate the protobuf bindings from the RustDesk tree next door, so the wire
// format is *shared* with the peer rather than re-specified here.
fn main() {
    let protos = std::path::Path::new("../libs/hbb_common/protos");
    let out = std::path::Path::new("src/protos");
    std::fs::create_dir_all(out).unwrap();
    protobuf_codegen_pure::Codegen::new()
        .out_dir(out)
        .inputs(&[protos.join("rendezvous.proto"), protos.join("message.proto")])
        .include(protos)
        .run()
        .expect("protobuf codegen failed");
    println!("cargo:rerun-if-changed={}", protos.display());
}
