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
    operator bool() const noexcept { return size != 0; }
};

// An image requires a path and alignment, default null alignment means unused
struct Image {
    Alignment alignment = kAlignNone;
    std::string path;
    operator bool() const noexcept { return alignment != kAlignNone; }
};

// An overlay always uses an image for background, optionally a custom font too
struct Overlay : Image {
    Font font;
};

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
    operator bool() const noexcept { return widget.alignment != kAlignNone; }
};

// A parameter uses images for background, background-with-scenes and widget
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
    operator bool() const noexcept { return widget.alignment != kAlignNone; }
};


// The top-bar buttons are images without any alignment
// These images must be the correct size or are otherwise rejected
struct TopbarButtons {
    // 85x50
    std::string back;
    std::string close;
    // 50x50
    std::string more;
    std::string remove;
    std::string swap;
};

struct Block {
    // The bottom-most background image
    Image background;

    // The block name can either be an image or a custom font
    // Use of image takes precedence if both are provided
    // This is 1 layer above the background
    struct {
        Image image;
        Font font;
    } blockName;

    // The pagination dots assume a number of frames equivalent to the number of parameter pages
    // This is 1 layer above the background
    Image paginationDots;

    // Buttons in the top-bar
    // These are 1 layer above the block name
    TopbarButtons topBarButtons;

    // Bypass control in the top-bar
    Bypass bypass;

    // padding of empty slots until parameters begin
    uint8_t parameterStartPadding = 0;

    // Parameter widgets (defaults if a specific parameter is not specified)
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
