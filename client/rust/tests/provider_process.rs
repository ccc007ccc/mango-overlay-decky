use mango_overlay::{
    Circle, ClipRect, Color, Group, Image, Layout, Line, Polyline, Provider, Rectangle, Text, Vec2,
    Visibility,
};

const PNG: &[u8] = &[
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
];

#[test]
fn rust_provider_uses_the_public_c_abi_end_to_end() {
    let socket = std::env::var("MANGO_OVERLAY_TEST_SOCKET").expect("test socket path");
    let mut provider = Provider::connect("rust-client-process-test/1.0", Some(&socket), 2_000)
        .expect("connect provider");
    provider
        .register(
            "dev.mango-overlay.rust-test",
            "primary",
            "Rust Client Test",
            (1280, 800),
            Visibility::Always,
        )
        .expect("register provider");
    provider.upload_resource(100, PNG).expect("upload PNG");

    let group_layout = Layout {
        translation: Vec2::new(100.0, 50.0),
        opacity: 0.8,
        clip: Some(ClipRect {
            position: Vec2::new(0.0, 0.0),
            size: Vec2::new(400.0, 240.0),
        }),
        ..Layout::default()
    };
    let child_layout = Layout {
        parent_id: 10,
        ..Layout::default()
    };
    let points = [
        Vec2::new(20.0, 120.0),
        Vec2::new(80.0, 100.0),
        Vec2::new(140.0, 130.0),
    ];

    let mut transaction = provider.begin().expect("begin scene");
    transaction
        .group(Group {
            id: 10,
            z_index: 0,
            layout: Some(&group_layout),
        })
        .expect("group");
    transaction
        .rectangle(Rectangle {
            id: 1,
            z_index: 0,
            position: Vec2::new(8.0, 12.0),
            size: Vec2::new(300.0, 80.0),
            corner_radius: 8.0,
            color: Color::WHITE,
            layout: Some(&child_layout),
        })
        .expect("rectangle");
    transaction
        .text(Text {
            id: 2,
            z_index: 1,
            position: Vec2::new(20.0, 28.0),
            text: "Rust scene",
            font_size: 24.0,
            color: Color::WHITE,
            layout: Some(&child_layout),
        })
        .expect("text");
    transaction
        .line(Line {
            id: 3,
            z_index: 2,
            start: Vec2::new(20.0, 80.0),
            end: Vec2::new(260.0, 80.0),
            thickness: 3.0,
            color: Color::WHITE,
            layout: None,
        })
        .expect("line");
    transaction
        .polyline(Polyline {
            id: 4,
            z_index: 3,
            points: &points,
            thickness: 2.0,
            color: Color::WHITE,
            layout: None,
        })
        .expect("polyline");
    transaction
        .circle(Circle {
            id: 5,
            z_index: 4,
            center: Vec2::new(320.0, 64.0),
            radius: 24.0,
            color: Color::WHITE,
            layout: None,
        })
        .expect("circle");
    transaction
        .image(Image {
            id: 6,
            z_index: 5,
            position: Vec2::new(360.0, 100.0),
            size: Vec2::new(48.0, 48.0),
            resource_id: 100,
            tint: Color::WHITE,
            layout: None,
        })
        .expect("image");
    transaction.commit().expect("commit scene");

    {
        let mut aborted = provider.begin().expect("begin aborted scene");
        aborted.remove(2).expect("stage removal");
    }

    let mut cleanup = provider.begin().expect("begin cleanup");
    cleanup.remove(6).expect("remove image");
    cleanup.commit().expect("commit cleanup");

    let empty = provider.begin().expect("begin empty scene");
    empty.commit().expect_err("empty scene must be rejected");

    let mut after_failed_commit = provider.begin().expect("begin after failed commit");
    after_failed_commit
        .remove(5)
        .expect("stage removal after failed commit");
    after_failed_commit
        .commit()
        .expect("commit after failed commit");
    provider.release_resource(100).expect("release PNG");
}
