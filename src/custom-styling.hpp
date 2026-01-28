// SPDX-FileCopyrightText: 2026 Filipe Coelho <falktx@darkglass.com>
// SPDX-License-Identifier: ISC

#pragma once

// NOTE all paths used here are absolute

#include <cstdint>
#include <string>
#include <unordered_map>

namespace CustomStyling {

static constexpr const int kMaxParameterStartPadding = 5;

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

// A font requires a path and size, default size 0 means unused
struct Font {
    std::string path;
    int size = 0;
    operator bool() const noexcept { return !path.empty() && size != 0; }
};

// An image requires a path, alignment is optional as it is not always relevant
// If the image belongs to a parameter widget, animation frames must be used (from min to max), we recommend using 100 steps
// If the image belongs to a bypass widget, at least 2 animation frames are required (on/processing position first, then off/bypassed)
struct Image {
    Alignment alignment = kAlignNone;
    std::string path;
    operator bool() const noexcept { return !path.empty(); }
};

// An overlay always uses an image for background, optionally a custom font too
struct Overlay : Image {
    Font font;
};

// A block contains a background and parameters
struct Block {
    // A block parameter uses a single image and x,y coordinates for positioning
    struct Parameter {
        Image image;
        uint16_t x;
        uint16_t y;
        operator bool() const noexcept { return image.alignment != kAlignNone; }
    };

    // The background image
    // It should contain at least 2 frames for on/off
    Image background;

    // Bypass parameter
    Parameter bypass;

    // Parameters indexed by control port symbol or patch property URI
    std::unordered_map<std::string, Parameter> parameters;
};

// Settings for a Block, contains many elements
struct BlockSettings {
    // A block settings parameter uses images for background, background-with-scenes and widget
    // Additionally it has overlays for many parameter statuses
    // The widget depends on the type of parameter and can be one of: circle (knob), list, fader or toggle (switch)
    // Only the widget is required, everything else is optional
    struct Parameter {
        Image background;
        Image backgroundScenes;
        Image widget;
        struct {
            Overlay blocked;
            Overlay inactive;
            Overlay inUse;
            Overlay unavailable;
        } overlays;
        operator bool() const noexcept { return !widget.path.empty(); }
    };

    // The bottom-most background image
    // Can contain multiple horizontal frames for paginated scrolling
    Image background;

    // The block name can either be a background image or a custom font
    // Use of background image takes precedence if both are provided
    // It is 1 layer above the background
    struct {
        Image background;
        Font font;
    } blockName;

    // The pagination dots assume a number of frames equivalent to the number of parameter pages
    // They are 1 layer above the background
    Image paginationDots;

    // The settings' top-bar buttons are images without any alignment
    // These images must be the correct size or are otherwise rejected
    // They are 1 layer above the block name
    struct TopbarButtons {
        // 85x50
        std::string back;
        std::string close;
        // 50x50
        std::string more;
        std::string remove;
        std::string swap;
    } topBarButtons;

    // Bypass control in the top-bar
    // The bypass control uses images for background, background-with-scenes and toggle-switch widget
    // Additionally it has an overlay for "in use"
    // Only the widget is required, everything else is optional
    struct Bypass {
        Image background;
        Image backgroundScenes;
        Image widget;
        struct {
            Overlay inUse;
        } overlays;
        operator bool() const noexcept { return !widget.path.empty(); }
    } bypass;

    // padding of empty slots until parameters begin
    uint8_t parameterStartPadding = 0;

    // Parameter widgets (defaults in case a specific parameter is not specified)
    struct {
        Parameter circle;
        Parameter fader;
        Parameter list;
        Parameter toggle;
    } defaultWidgets;

    // Custom styling for individual parameters, indexed by control port symbol or patch property URI
    std::unordered_map<std::string, Parameter> customWidgets;
};

} // namespace CustomStyling
