#pragma once

#include "dds.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

// Textures for the editor. All functions throw std::runtime_error on GPU failures.
// ImGui can draw any of them directly (ImTextureID is SDL_GPUTexture* in imgui 1.92.2+).

// RGBA8, single level (row 0 = top).
SDL_GPUTexture* uploadRgbaTexture(SDL_GPUDevice* device, const void* pixels, uint32_t width, uint32_t height);
// RGBA8 with a full mip chain generated on the CPU (2x2 box filter).
SDL_GPUTexture* uploadRgbaTextureMipmapped(SDL_GPUDevice* device, const std::vector<tr::Rgba8>& pixels, uint32_t width, uint32_t height);
// A DDS file: DXT1/3/5 uploaded compressed (BC1/2/3, every mip level in the file) when the device
// supports it and the size allows; otherwise decoded to RGBA8 with generated mips.
SDL_GPUTexture* uploadDdsTexture(SDL_GPUDevice* device, const tr::Bytes& dds);
// A 2D array of square RGBA8 layers (each size x size), mipmapped.
SDL_GPUTexture* uploadRgbaArray(SDL_GPUDevice* device, const std::vector<std::vector<tr::Rgba8>>& layers, uint32_t size);
// Cube map array: 6 faces per cube (+X, -X, +Y, -Y, +Z, -Z, as DDS stores them), each size x size RGBA8.
SDL_GPUTexture* uploadRgbaCubeArray(SDL_GPUDevice* device, const std::vector<std::vector<tr::Rgba8>>& faces, uint32_t size);

// CPU helpers.
std::vector<tr::Rgba8> resizeRgba(const std::vector<tr::Rgba8>& source, uint32_t sourceWidth, uint32_t sourceHeight,
                                  uint32_t width, uint32_t height);                    // bilinear
std::vector<tr::Rgba8> halveRgba(const std::vector<tr::Rgba8>& source, uint32_t width, uint32_t height);   // 2x2 box
