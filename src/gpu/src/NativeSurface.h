#pragma once

namespace ruby::gpu {

// Turns a native window handle into whatever the platform's swapchain wants.
// macOS: takes an NSView*, ensures it's layer-backed by a CAMetalLayer, returns that
// layer. Windows: takes an HWND, returns it unchanged.
//
// The only platform-specific code in the GPU module — everything else is one
// implementation for every platform.
void* prepareNativeSurface(void* native_window);

}  // namespace ruby::gpu
