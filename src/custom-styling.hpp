// SPDX-FileCopyrightText: 2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

// NOTE all paths used here are absolute

#include <cstdint>
#include <string>
#include <unordered_map>

namespace CustomStyling {

#ifdef _DARKGLASS_DEVICE_PABLITO
// On Anagram the maximum allowed start padding of parameters is number-of-knobs - 1
static constexpr const int kMaxParameterStartPadding = 5;
#else
// The maximum that fits into a `uint8_t`
static constexpr const int kMaxParameterStartPadding = UINT8_MAX;
#endif

// Alignment for images when they don't match the full size of the container
// This enum intentionally matches lv_align_t so it can be used without conversions
enum Alignment {
    kAlignNone,
    kAlignTopLeft,
    kAlignTopMid,
    kAlignTopRight,
    kAlignBottomLeft,
    kAlignBottomMid,
    kAlignBottomRight,
    kAlignLeftMid,
    kAlignRightMid,
    kAlignCenter,
};

// A font requires a path and size
struct Font {
    std::string path;
    int size = 0;
    operator bool() const noexcept { return !path.empty() && size != 0; }
};

// An image requires a path, alignment is optional as it is not always relevant
// If the image belongs to a parameter control, animation frames must be used (from min to max)
// If the image belongs to a bypass control, at least 2 animation frames are required (on is first, then off/bypassed)
// If the image belongs to a block, animation frames are optional (if present: on is first, then off/bypassed)
struct Image {
    Alignment alignment = kAlignNone;
    std::string path;
    operator bool() const noexcept { return !path.empty(); }
};

// An overlay always uses an image for background, optionally a custom font too
struct Overlay : Image {
    Font font;
    operator bool() const noexcept { return !path.empty(); }
};

// A block image contains a path (for the image itself) and parameters
struct BlockImage {
    // A block image parameter uses a single image path and x,y coordinates for positioning
    // The width and height are optional helpers
    struct Parameter {
        std::string path;
        uint16_t x;
        uint16_t y;
        uint16_t width;
        uint16_t height;
        operator bool() const noexcept { return !path.empty(); }
    };

    // The block image path
    // It can contain multiple frames for on/off animation (on is first, then off/bypassed)
    std::string path;

    // Bypass parameter
    Parameter bypass;

    // Parameters indexed by control port symbol
    std::unordered_map<std::string, Parameter> parameters;
};

// Settings for a Block, contains many elements
struct BlockSettings {
    // A block settings parameter uses images for background, background-with-scenes and control
    // Additionally it has overlays for many parameter statuses
    // The control depends on the type of parameter and can be one of: knob, list, meter or toggle/switch
    // Only the control is required, everything else is optional
    struct ParameterWidget {
        Image background;
        Image backgroundScenes;
        Image control;
        struct Overlays {
            Overlay blocked;
            Overlay inactive;
            Overlay inUse;
            Overlay unavailable;
        } overlays;
        operator bool() const noexcept { return !control.path.empty(); }
    };

    // The bottom-most background image
    // Can contain multiple horizontal frames for paginated scrolling,
    // in which case it must be a multiple of the screen width
    // (on Anagram this is 1424px)
    Image background;

    // The pagination dots
    // The image is assumed to have a number of frames equivalent to the number of parameter pages
    // They are 1 layer above the background
    Image paginationDots;

    // The top-bar background and buttons
    // It is 1 layer above the background
    struct TopBar {
        // The top-bar image cannot contain multiple frames
        Image background;

        // The top-bar block name in the can either be a background image or a custom font
        // Use of background image takes precedence if both are provided
        struct BlockName {
            Image background;
            Font font;
        } blockName;

        // The top-bar buttons are images without any alignment
        // These images must be the correct size or are otherwise rejected
        struct Buttons {
            // 85x50
            std::string back;
            std::string close;
            // 50x50
            std::string more;
            std::string remove;
            std::string swap;
        } buttons;

        // The the top-bar scene control can either be images or background + font
        // Use of images takes precedence if both are provided
        struct SceneControl {
            struct {
                Image background;
                Font font;
                operator bool() const noexcept { return !background.path.empty() && font.size != 0; }
            } withBackgroundAndFont;
            struct {
                Image allScenes;
                Image activeScene;
                operator bool() const noexcept { return !allScenes.path.empty() && !activeScene.path.empty(); }
            } withImages;
        } sceneControl;
    } topBar;

    // Bypass control in the top-bar
    // The bypass control uses images for background, background-with-scenes and toggle-switch control
    // Additionally it has an overlay for "in use"
    // Only the control is required, everything else is optional
    struct Bypass {
        Image background;
        Image backgroundScenes;
        Image control;
        struct Overlays {
            Overlay inUse;
        } overlays;
        operator bool() const noexcept { return !control.path.empty(); }
    } bypass;

    // padding of empty slots until parameters begin
    uint8_t parameterStartPadding = 0;

    // Default parameter widgets in case a specific parameter is not specified
    struct {
        ParameterWidget knob;
        ParameterWidget list;
        ParameterWidget meter;
        ParameterWidget toggle;
    } defaultWidgets;

    // Parameter-specific widgets, indexed by control port symbol
    std::unordered_map<std::string, ParameterWidget> parameters;
};

} // namespace CustomStyling
