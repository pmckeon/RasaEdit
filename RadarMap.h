#pragma once

#include "gamedata.h"
#include "dds.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

// The game's own mini-map for one map region: the 256x256 DXT1 tiles from <map>_radar.glm assembled
// into one texture (row 0 = top = largest z), with the world rectangle it covers.
class RadarMap {
public:
    RadarMap(SDL_GPUDevice* device, const std::string& radarArchivePath, int32_t mapTemplateId, int32_t regionId, const tr::RadarInfo& info);
    ~RadarMap();
    RadarMap(const RadarMap&) = delete;
    RadarMap& operator=(const RadarMap&) = delete;

    SDL_GPUTexture* texture() const { return texture_; }
    float leftX() const { return leftX_; }
    float rightX() const { return rightX_; }
    float topZ() const { return topZ_; }
    float bottomZ() const { return bottomZ_; }
    int32_t tilesPresent() const { return tilesPresent_; }
    int32_t tilesTotal() const { return tilesTotal_; }

    // Whether the radar archive holds region regionId's first tile - the client's test (regionmanager.py
    // GetUIRadarTexturePrefix: RadarFileExists('ui_radar_<template>_<region>_1.dds')).
    static bool hasTiles(const std::string& radarArchivePath, int32_t mapTemplateId, int32_t regionId);

    // CPU part, exposed for testing. Missing tiles are transparent; images larger than maxSize are halved.
    static std::vector<tr::Rgba8> buildImage(const std::string& radarArchivePath, int32_t mapTemplateId, int32_t regionId,
                                             const tr::RadarInfo& info, uint32_t maxSize, uint32_t& width, uint32_t& height,
                                             int32_t& tilesPresent);

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* texture_ = nullptr;
    float leftX_ = 0, rightX_ = 0, topZ_ = 0, bottomZ_ = 0;
    int32_t tilesPresent_ = 0;
    int32_t tilesTotal_ = 0;
};
