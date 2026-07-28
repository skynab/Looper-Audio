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
/** Magnifying glass, for the timeline zoom. Decorative rather than a button:
    it labels the slider next to it, which is why there is no On variant. */
inline constexpr const char* kMagnifier =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#FFF\" fill-opacity=\".6\" fill-rule=\"evenodd\" d=\"M14,16 C16.7614237,16 19,13.7614237 19,11 "
    "C19,8.23857625 16.7614237,6 14,6 C11.2385763,6 9,8.23857625 9,11 C9,13.7614237 11.2385763,16 14,16 Z "
    "M14,5 C17.3137085,5 20,7.6862915 20,11 C20,12.2972304 19.5883209,13.4983081 18.8884831,14.4797126 "
    "L22.8508769,18.4420163 C23.046139,18.6372785 23.046139,18.953861 22.8508769,19.1491231 "
    "L22.1437701,19.8562299 C21.948508,20.051492 21.6319255,20.051492 21.4366633,19.8562299 "
    "L17.4726289,15.8935267 C16.4925311,16.5902985 15.294111,17 14,17 C10.6862915,17 8,14.3137085 8,11 "
    "C8,7.6862915 10.6862915,5 14,5 Z\"/></svg>";

/** Per-track mute, in the tracks pane. The On state is a speaker; Disabled
    adds the struck-through line and dims the speaker, so a muted track reads
    as muted at a glance rather than by comparing it with its neighbours. */
inline constexpr const char* kAudioOn =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<path fill=\"#FFF\" fill-opacity=\".6\" fill-rule=\"evenodd\" d=\"M16.7131678,16.0306614 C17.9027076,15.091757 18.58,13.8391547 18.58,12.5057445 C18.58,11.1975361 17.9281916,9.96670491 16.7784524,9.03309291 C16.638995,8.9198506 16.4058111,8.91444126 16.2576217,9.02101081 C16.1094323,9.12758036 16.1023536,9.30577308 16.241811,9.4190154 C17.2641568,10.2491813 17.8430948,11.342409 17.8430948,12.5057445 C17.8430948,13.6914758 17.2415072,14.8040677 16.183738,15.6389656 C16.0421942,15.7506861 16.0459673,15.9289376 16.1921653,16.0371014 C16.3383633,16.1452652 16.5716241,16.142382 16.7131678,16.0306614 Z M21.3328439,5.04041084 C21.4904393,4.9465472 21.7157712,4.97242915 21.8361368,5.09821978 C23.9437866,7.30086314 25.0712128,9.99229859 24.9957662,12.797176 C24.9256925,15.4023069 23.8251959,17.8583973 21.8797769,19.8757249 C21.7601134,19.9998115 21.534918,20.022495 21.3767888,19.9263899 C21.2186596,19.8302848 21.1874769,19.6517842 21.3071404,19.5276976 C23.1620574,17.6042172 24.21098,15.2632297 24.2778092,12.7787222 C24.3497609,10.1037711 23.2751558,7.53843223 21.2654336,5.43813007 C21.1450679,5.31233944 21.1752486,5.13427447 21.3328439,5.04041084 Z M18.8140136,7.05399254 C18.9594698,6.95398779 19.1754599,6.97038857 19.2964411,7.09062471 C20.8201351,8.60493557 21.668572,10.5086919 21.668572,12.5195765 C21.668572,14.540624 20.8114997,16.4533285 19.2737636,17.9709877 C19.1522848,18.0908805 18.936228,18.1066695 18.7911872,18.0062533 C18.6461464,17.9058372 18.6270456,17.7272416 18.7485245,17.6073488 C20.1839103,16.1907034 20.983444,14.4064062 20.983444,12.5195765 C20.983444,10.6422311 20.1919679,8.86628555 18.7696977,7.45277395 C18.6487166,7.33253781 18.6685575,7.1539973 18.8140136,7.05399254 Z M13.9262728,7.6372085 L13.9262728,17.3036945 C13.9262728,17.6158988 13.7292952,17.701503 13.4898746,17.497927 L10.584,15.0269725 L8.0053872,15.0276541 C7.45012718,15.0276541 7,14.5737848 7,14.0294042 L7,10.9114988 C7,10.3601806 7.45291084,9.91324888 8.0053872,9.91324888 L10.584,9.91297246 L13.4898746,7.44297607 C13.7308906,7.23804339 13.9262728,7.32815876 13.9262728,7.6372085 Z\"/>"
    "</svg>";

/** The muted state — see kAudioOn. */
inline constexpr const char* kAudioDisabled =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"31\" height=\"25\" viewBox=\"0 0 31 25\">"
    "<g fill=\"none\" fill-rule=\"evenodd\">"
    "<path fill=\"#FFF\" fill-opacity=\".15\" d=\"M16.7131678,16.0306614 C17.9027076,15.091757 18.58,13.8391547 18.58,12.5057445 C18.58,11.1975361 17.9281916,9.96670491 16.7784524,9.03309291 C16.638995,8.9198506 16.4058111,8.91444126 16.2576217,9.02101081 C16.1094323,9.12758036 16.1023536,9.30577308 16.241811,9.4190154 C17.2641568,10.2491813 17.8430948,11.342409 17.8430948,12.5057445 C17.8430948,13.6914758 17.2415072,14.8040677 16.183738,15.6389656 C16.0421942,15.7506861 16.0459673,15.9289376 16.1921653,16.0371014 C16.3383633,16.1452652 16.5716241,16.142382 16.7131678,16.0306614 Z M21.3328439,5.04041084 C21.4904393,4.9465472 21.7157712,4.97242915 21.8361368,5.09821978 C23.9437866,7.30086314 25.0712128,9.99229859 24.9957662,12.797176 C24.9256925,15.4023069 23.8251959,17.8583973 21.8797769,19.8757249 C21.7601134,19.9998115 21.534918,20.022495 21.3767888,19.9263899 C21.2186596,19.8302848 21.1874769,19.6517842 21.3071404,19.5276976 C23.1620574,17.6042172 24.21098,15.2632297 24.2778092,12.7787222 C24.3497609,10.1037711 23.2751558,7.53843223 21.2654336,5.43813007 C21.1450679,5.31233944 21.1752486,5.13427447 21.3328439,5.04041084 Z M18.8140136,7.05399254 C18.9594698,6.95398779 19.1754599,6.97038857 19.2964411,7.09062471 C20.8201351,8.60493557 21.668572,10.5086919 21.668572,12.5195765 C21.668572,14.540624 20.8114997,16.4533285 19.2737636,17.9709877 C19.1522848,18.0908805 18.936228,18.1066695 18.7911872,18.0062533 C18.6461464,17.9058372 18.6270456,17.7272416 18.7485245,17.6073488 C20.1839103,16.1907034 20.983444,14.4064062 20.983444,12.5195765 C20.983444,10.6422311 20.1919679,8.86628555 18.7696977,7.45277395 C18.6487166,7.33253781 18.6685575,7.1539973 18.8140136,7.05399254 Z M13.9262728,7.6372085 L13.9262728,17.3036945 C13.9262728,17.6158988 13.7292952,17.701503 13.4898746,17.497927 L10.584,15.0269725 L8.0053872,15.0276541 C7.45012718,15.0276541 7,14.5737848 7,14.0294042 L7,10.9114988 C7,10.3601806 7.45291084,9.91324888 8.0053872,9.91324888 L10.584,9.91297246 L13.4898746,7.44297607 C13.7308906,7.23804339 13.9262728,7.32815876 13.9262728,7.6372085 Z\"/>"
    "<rect width=\"20\" height=\"1\" x=\"5.854\" y=\"12.354\" fill=\"red\" transform=\"rotate(45 15.854 12.854)\"/>"
    "</g></svg>";

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
