#pragma once

namespace looper
{
/**
    Pure geometry for the piano roll: converts between grid rows/steps and
    pixel coordinates, and locates a cell from a point. JUCE-free so the
    conversion math is unit-tested headless — mirrors TimelineGeometry, which
    does the same job for the arrangement's beat/track axes.
*/
struct PianoRollGeometry
{
    float gutterWidth  = 40.0f; // fixed left column for pitch/pad names
    int   numRows       = 24;   // 2 octaves
    int   lowPitch      = 48;   // C3
    int   numSteps      = 16;
    double stepBeats    = 0.25; // one step = a 16th note

    int pitchForRow(int row) const noexcept { return lowPitch + (numRows - 1 - row); }
    int rowForPitch(int pitch) const noexcept { return (numRows - 1) - (pitch - lowPitch); }

    /** Width of the scrollable grid area alone (total component width minus the gutter). */
    float gridWidth(float totalWidth) const { return totalWidth - gutterWidth; }

    float colWidth(float totalWidth) const { return gridWidth(totalWidth) / (float) numSteps; }
    float rowHeight(float totalHeight) const { return totalHeight / (float) numRows; }

    /** x-coordinate of a step's left edge, in the whole component (gutter included). */
    float xForStep(int step, float totalWidth) const
    {
        return gutterWidth + (float) step * colWidth(totalWidth);
    }

    /** y-coordinate of a row's top edge. */
    float yForRow(int row, float totalHeight) const { return (float) row * rowHeight(totalHeight); }

    /** Locates the (row, step) under a point; false if it falls in the gutter
        or outside the grid (mirrors ArrangementView's gutter-click handling). */
    bool cellAt(float x, float y, float totalWidth, float totalHeight, int& rowOut, int& stepOut) const
    {
        if (x < gutterWidth || totalWidth <= gutterWidth || totalHeight <= 0.0f)
            return false;

        const int step = (int) ((x - gutterWidth) / colWidth(totalWidth));
        const int row  = (int) (y / rowHeight(totalHeight));
        if (step < 0 || step >= numSteps || row < 0 || row >= numRows)
            return false;

        rowOut  = row;
        stepOut = step;
        return true;
    }
};

} // namespace looper
