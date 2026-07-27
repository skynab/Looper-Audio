#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace looper::icons
{
/** Parses an embedded SVG string into a Drawable. Returns nullptr if the SVG
    can't be parsed (shouldn't happen for the constants below — they're
    fixed, known-good strings, not user input). */
inline std::unique_ptr<juce::Drawable> fromSvg(const char* svgText)
{
    auto xml = juce::XmlDocument::parse(svgText);
    return xml != nullptr ? juce::Drawable::createFromSVG(*xml) : nullptr;
}

// Two states each (Off = dim white, On = cyan accent), matching how they're
// actually used in this app — a plain toggle, not a full hover/disabled set.
inline constexpr const char* kStarOn =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<polygon fill=\"#00EBFF\" fill-rule=\"evenodd\" points=\"15.5 4.25 17.49 9.852 22.833 9.852 18.473 13.142 "
    "20.032 18.915 15.5 15.453 10.968 18.915 12.527 13.142 8.166 9.852 13.509 9.852\"/></svg>";

inline constexpr const char* kStarOutlineOff =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#FFF\" fill-opacity=\".6\" d=\"M17.137012,10.3516667 L21.3406246,10.3516667 L17.902572,12.945977 "
    "L19.1659421,17.6240252 L15.4999721,14.824165 L11.8346887,17.6240156 L13.0973976,12.9459152 "
    "L9.65876981,10.3516667 L13.8621052,10.3516667 L15.4999014,5.74335104 L17.137012,10.3516667 Z "
    "M6.67289686,9.35166667 L11.9559357,13.3374181 L10.101978,20.2059844 L15.5000279,16.0825017 "
    "L20.8990579,20.2059748 L19.0440947,13.3373563 L24.3260421,9.35166667 L17.842988,9.35166667 "
    "L15.5000986,2.75664896 L13.1562282,9.35166667 L6.67289686,9.35166667 Z\"/></svg>";

inline constexpr const char* kSidebarOn =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#00EBFF\" d=\"M9,19 L22,19 L22,6 L9,6 L9,19 Z M8,5 L23,5 L23,20 L8,20 L8,5 Z M13,6 L14,6 L14,19 "
    "L13,19 L13,6 Z M10,7 L12,7 L12,8 L10,8 Z M10,9 L12,9 L12,10 L10,10 Z M10,11 L12,11 L12,12 L10,12 Z "
    "M10,13 L12,13 L12,14 L10,14 Z\"/></svg>";

inline constexpr const char* kSidebarOff =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#FFF\" fill-opacity=\".6\" d=\"M9,19 L22,19 L22,6 L9,6 L9,19 Z M8,5 L23,5 L23,20 L8,20 L8,5 Z "
    "M13,6 L14,6 L14,19 L13,19 L13,6 Z M10,7 L12,7 L12,8 L10,8 Z M10,9 L12,9 L12,10 L10,10 Z M10,11 L12,11 "
    "L12,12 L10,12 Z M10,13 L12,13 L12,14 L10,14 Z\"/></svg>";

inline constexpr const char* kFolderAddOn =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#00EBFF\" fill-rule=\"evenodd\" d=\"M13,6 C14.1090495,6 15.1022505,8 16,8 L22,8 C23,8 23,9 23,9 "
    "L23,18 C23,18.5522847 22.5522847,19 22,19 L9,19 C8.44771525,19 8,18.5522847 8,18 L8.00017147,6.99108368 "
    "C8.00308642,6.89197531 8.05555556,6 9,6 Z M16,10 L15,10 L15,13 L12,13 L12,14 L15,14 L15,17 L16,17 L16,14 "
    "L19,14 L19,13 L16,13 L16,10 Z\"/></svg>";

inline constexpr const char* kFolderAddOff =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#FFF\" fill-opacity=\".6\" fill-rule=\"evenodd\" d=\"M13,6 C14.1090495,6 15.1022505,8 16,8 "
    "L22,8 C23,8 23,9 23,9 L23,18 C23,18.5522847 22.5522847,19 22,19 L9,19 C8.44771525,19 8,18.5522847 8,18 "
    "L8.00017147,6.99108368 C8.00308642,6.89197531 8.05555556,6 9,6 Z M16,10 L15,10 L15,13 L12,13 L12,14 "
    "L15,14 L15,17 L16,17 L16,14 L19,14 L19,13 L16,13 L16,10 Z\"/></svg>";

// Transport record button: a red disc to arm, a red square once rolling —
// the two states of the same control, so they're the DrawableButton's normal
// and "on" images rather than two separate buttons. Both carry the white ring,
// which is what keeps them legible on the dark transport background.
inline constexpr const char* kRecordButton =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"25\" height=\"25\" viewBox=\"0 0 25 25\">"
    "<g fill=\"none\" fill-rule=\"evenodd\">"
    "<path fill=\"#FFF\" d=\"M12.5,0 C19.4035594,0 25,5.59644063 25,12.5 C25,19.4035594 19.4035594,25 12.5,25 "
    "C5.59644063,25 0,19.4035594 0,12.5 C0,5.59644063 5.59644063,0 12.5,0 Z M12.5,1 C6.14872538,1 1,6.14872538 "
    "1,12.5 C1,18.8512746 6.14872538,24 12.5,24 C18.8512746,24 24,18.8512746 24,12.5 C24,6.14872538 "
    "18.8512746,1 12.5,1 Z\"/>"
    "<circle cx=\"12.5\" cy=\"12.5\" r=\"10.5\" fill=\"#EB2323\"/></g></svg>";

inline constexpr const char* kRecordStopButton =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"25\" height=\"25\" viewBox=\"0 0 25 25\">"
    "<g fill=\"none\" fill-rule=\"evenodd\">"
    "<rect width=\"11\" height=\"11\" x=\"7\" y=\"7\" fill=\"#EB2323\" rx=\"2\"/>"
    "<path fill=\"#FFF\" d=\"M12.5,0 C19.4035594,0 25,5.59644063 25,12.5 C25,19.4035594 19.4035594,25 12.5,25 "
    "C5.59644063,25 0,19.4035594 0,12.5 C0,5.59644063 5.59644063,0 12.5,0 Z M12.5,1 C6.14872538,1 1,6.14872538 "
    "1,12.5 C1,18.8512746 6.14872538,24 12.5,24 C18.8512746,24 24,18.8512746 24,12.5 C24,6.14872538 "
    "18.8512746,1 12.5,1 Z\"/></g></svg>";

// Transport playback controls. "Frame" is the icon set's video wording; the
// musical equivalent here is a bar, so previous/next step one bar and
// first/last jump to the start and to the end of the song's content.
// Play and Pause are the two states of one toggle, not two buttons.
inline constexpr const char* kFirstFrame =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"25\" height=\"13\" viewBox=\"0 0 25 "
    "13\"><path fill=\"#FFF\" fill-rule=\"evenodd\" d=\"M25,0 L25,13 L22,13 L22,0 L25,0 Z "
    "M11,0 L22,6.5 L11,13 L11,0 Z M1.77635684e-15,0 L11,6.5 L0,13 L1.77635684e-15,0 Z\" "
    "transform=\"matrix(-1 0 0 1 25 0)\"/></svg>";

inline constexpr const char* kPreviousFrame =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"14\" height=\"13\" viewBox=\"0 0 14 "
    "13\"><path fill=\"#FFF\" fill-rule=\"evenodd\" d=\"M14,0 L14,13 L11,13 L11,0 L14,0 Z "
    "M1.77635684e-15,0 L11,6.5 L0,13 L1.77635684e-15,0 Z\" transform=\"matrix(-1 0 0 1 14 "
    "0)\"/></svg>";

inline constexpr const char* kPlay =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"21\" height=\"25\" viewBox=\"0 0 21 "
    "25\"><polygon fill=\"#FFF\" fill-rule=\"evenodd\" points=\"0 25 0 0 20.833 "
    "12.5\"/></svg>";

inline constexpr const char* kPause =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"21\" height=\"25\" viewBox=\"0 0 21 "
    "25\"><path fill=\"#FFF\" fill-rule=\"evenodd\" d=\"M0,0 L7,0 L7,25 L0,25 Z M14,0 L21,0 "
    "L21,25 L14,25 Z\"/></svg>";

inline constexpr const char* kNextFrame =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"14\" height=\"13\" viewBox=\"0 0 14 "
    "13\"><path fill=\"#FFF\" fill-rule=\"evenodd\" d=\"M14,0 L14,13 L11,13 L11,0 L14,0 Z "
    "M1.77635684e-15,0 L11,6.5 L0,13 L1.77635684e-15,0 Z\"/></svg>";

inline constexpr const char* kLastFrame =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"25\" height=\"13\" viewBox=\"0 0 25 "
    "13\"><path fill=\"#FFF\" fill-rule=\"evenodd\" d=\"M25,0 L25,13 L22,13 L22,0 L25,0 Z "
    "M11,0 L22,6.5 L11,13 L11,0 Z M1.77635684e-15,0 L11,6.5 L0,13 L1.77635684e-15,0 "
    "Z\"/></svg>";

} // namespace looper::icons
