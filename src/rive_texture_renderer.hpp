#pragma once

#include <functional>
#include <memory>
#include <godot_cpp/classes/texture2drd.hpp>
#include <rive/factory.hpp>
#include <rive/math/mat2d.hpp>

namespace rive { class Artboard; }

// All Vulkan work is serialized on Godot's rendering thread.
class RiveTextureRenderer {
public:
    RiveTextureRenderer();
    ~RiveTextureRenderer();
    RiveTextureRenderer(const RiveTextureRenderer&) = delete;
    RiveTextureRenderer& operator=(const RiveTextureRenderer&) = delete;

    bool initialize();
    rive::Factory* factory() const;
    bool draw(rive::Artboard* artboard, const rive::Mat2D& transform, int width, int height);
    godot::Ref<godot::Texture2DRD> texture() const;
    static void on_render_thread(const std::function<void()>& work);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
