extends SceneTree

# Run with a real Vulkan device, not --headless:
# godot --path demo --rendering-driver vulkan --script res://tests/gpu_smoke.gd
func _initialize():
    run.call_deferred()

func require(condition: bool, message: String):
    if not condition:
        push_error(message)
        quit(1)
        return false
    return true

func capture() -> Image:
    await RenderingServer.frame_post_draw
    return root.get_texture().get_image()

func save_capture(image: Image, name: String):
    var output = OS.get_environment("RIVE_TEST_OUTPUT")
    if output.is_empty():
        output = "user://"
    image.save_png(output.path_join(name + ".png"))

func run():
    root.size = Vector2i(640, 480)
    root.transparent_bg = true
    if not ClassDB.class_exists("RiveViewer"):
        var extension = OS.get_environment("RIVE_TEST_EXTENSION")
        GDExtensionManager.load_extension(extension if not extension.is_empty() else "res://rive.gdextension")
    if not require(ClassDB.class_exists("RiveViewer"), "Rive extension did not load"):
        return
    var viewer = ClassDB.instantiate("RiveViewer")
    viewer.size = Vector2(400, 300)
    viewer.position = Vector2(20, 20)
    root.add_child(viewer)
    viewer.file_path = "res://examples/meteor.riv"
    viewer.artboard = 0
    viewer.scene = -1
    viewer.animation = 0
    await create_timer(0.3).timeout
    var first = await capture()
    if not require(first.get_used_rect().has_area(), "Rive texture is empty"):
        return
    await create_timer(0.3).timeout
    var second = await capture()
    if not require(first.get_data() != second.get_data(), "Rive animation did not change"):
        return
    viewer.size = Vector2(240, 180)
    await create_timer(0.1).timeout
    var resized = await capture()
    if not require(resized.get_used_rect().has_area(), "Rive texture disappeared after resize"):
        return
    save_capture(resized, "rive-gpu-smoke")
    for asset in ["circle_clips", "blend_test", "new_text", "glass_button"]:
        viewer.file_path = "res://examples/" + asset + ".riv"
        viewer.artboard = 0
        viewer.scene = -1
        viewer.animation = -1
        await create_timer(0.1).timeout
        var result = await capture()
        if not require(result.get_used_rect().has_area(), "Empty output for " + asset):
            return
        save_capture(result, asset)
    var feather_path = ProjectSettings.globalize_path("res://../thirdparty/rive-cpp/tests/unit_tests/assets/feather_render_test.riv")
    if FileAccess.file_exists(feather_path):
        viewer.file_path = feather_path
        viewer.artboard = 0
        await create_timer(0.1).timeout
        var feather = await capture()
        if not require(feather.get_used_rect().has_area(), "Empty feather output"):
            return
        save_capture(feather, "feather")
    viewer.paused = true
    var paused = await capture()
    await create_timer(0.1).timeout
    var still_paused = await capture()
    if not require(paused.get_data() == still_paused.get_data(), "Paused viewer changed pixels"):
        return
    var retained_file = viewer.get_file()
    viewer.queue_free()
    await process_frame
    await process_frame
    retained_file.reset_artboard(0)
    if not require(retained_file.get_artboard(0).exists(), "File resource did not survive its viewer"):
        return
    retained_file = null
    var demo = load("res://main.tscn").instantiate()
    root.add_child(demo)
    await create_timer(0.3).timeout
    var demo_image = await capture()
    if not require(demo_image.get_used_rect().has_area(), "Original demo is empty"):
        return
    save_capture(demo_image, "demo")
    demo.queue_free()
    await process_frame
    await process_frame
    print("RIVE_GPU_SMOKE_OK: animation, resize, clips, blends, text, images, pause, multiple viewers, teardown")
    quit(0)
