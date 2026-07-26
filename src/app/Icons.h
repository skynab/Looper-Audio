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

} // namespace looper::icons
