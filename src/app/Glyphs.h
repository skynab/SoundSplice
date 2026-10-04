#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <initializer_list>

namespace soundsplice::glyphs
{
/**
    The interface's line icons: the pane tabs, the toolbar and the transport.

    The design system asks for Phosphor's regular weight. These are drawn to
    the same brief rather than copied from it - a 16-unit grid, 1.5-unit
    round-capped strokes, and the playback glyphs filled - so they sit with
    the mockups without bringing in a font or a library. Each is a path in
    that grid, scaled to whatever box it's drawn in.
*/
enum class Glyph
{
    waveform, rows, folder, bookmark, history, sliders, stack, chartBar,
    play, pause, stop, skipBack, skipForward, rewind, fastForward, record, repeat,
    plus, cross, check, caretDown, floppy, textT, selection, scissors, cursor,
    zeroCrossing, warning, sparkle, gear, microphone, video, faders,
    code, waves, transcript, keyboard, gauge, envelope, grid, export_, bandaid,
    spectrum, arrowLeftRight, layout, magnet, speakerSlash, trendUp, trendDown, normalize, trash,
    copy, clipboard, crop, reverse, split, eyedropper, speed
};

namespace detail
{
    /** A stroked shape and a filled one, both in the 16-unit grid. */
    struct Shape
    {
        juce::Path stroke, fill;
    };

    inline void line(juce::Path& p, float x0, float y0, float x1, float y1)
    {
        p.startNewSubPath(x0, y0);
        p.lineTo(x1, y1);
    }

    inline void polyline(juce::Path& p, std::initializer_list<juce::Point<float>> points, bool closed = false)
    {
        bool first = true;
        for (auto pt : points)
        {
            if (first) p.startNewSubPath(pt);
            else       p.lineTo(pt);
            first = false;
        }
        if (closed)
            p.closeSubPath();
    }

    inline void arc(juce::Path& p, float cx, float cy, float r, float fromDeg, float toDeg)
    {
        p.addCentredArc(cx, cy, r, r, 0.0f, juce::degreesToRadians(fromDeg), juce::degreesToRadians(toDeg), true);
    }

    inline Shape build(Glyph glyph)
    {
        Shape s;
        auto& p = s.stroke;
        auto& f = s.fill;

        switch (glyph)
        {
            case Glyph::waveform:
                line(p, 2.5f, 6.5f, 2.5f, 9.5f);
                line(p, 5.25f, 4.0f, 5.25f, 12.0f);
                line(p, 8.0f, 2.0f, 8.0f, 14.0f);
                line(p, 10.75f, 4.5f, 10.75f, 11.5f);
                line(p, 13.5f, 6.5f, 13.5f, 9.5f);
                break;
            case Glyph::rows:
                p.addRoundedRectangle(2.5f, 3.0f, 11.0f, 4.0f, 1.0f);
                p.addRoundedRectangle(2.5f, 9.0f, 11.0f, 4.0f, 1.0f);
                break;
            case Glyph::folder:
                polyline(p, { { 2.0f, 4.0f }, { 6.0f, 4.0f }, { 7.5f, 5.5f }, { 14.0f, 5.5f },
                              { 14.0f, 13.0f }, { 2.0f, 13.0f } }, true);
                break;
            case Glyph::bookmark:
                polyline(p, { { 4.0f, 2.5f }, { 12.0f, 2.5f }, { 12.0f, 13.5f }, { 8.0f, 10.75f },
                              { 4.0f, 13.5f } }, true);
                break;
            case Glyph::history:
                arc(p, 8.0f, 8.0f, 5.5f, -60.0f, 230.0f);
                polyline(p, { { 1.6f, 3.6f }, { 2.9f, 6.1f }, { 5.4f, 4.9f } });
                polyline(p, { { 8.0f, 5.0f }, { 8.0f, 8.0f }, { 10.25f, 9.5f } });
                break;
            case Glyph::sliders:
                line(p, 2.0f, 4.5f, 14.0f, 4.5f);
                line(p, 2.0f, 11.5f, 14.0f, 11.5f);
                p.addEllipse(8.0f, 2.5f, 4.0f, 4.0f);
                p.addEllipse(3.5f, 9.5f, 4.0f, 4.0f);
                break;
            case Glyph::stack:
                polyline(p, { { 8.0f, 2.0f }, { 14.0f, 5.0f }, { 8.0f, 8.0f }, { 2.0f, 5.0f } }, true);
                polyline(p, { { 2.0f, 8.0f }, { 8.0f, 11.0f }, { 14.0f, 8.0f } });
                polyline(p, { { 2.0f, 11.0f }, { 8.0f, 14.0f }, { 14.0f, 11.0f } });
                break;
            case Glyph::chartBar:
                line(p, 2.5f, 2.5f, 2.5f, 13.5f);
                p.addRoundedRectangle(2.5f, 3.5f, 7.0f, 3.0f, 0.75f);
                p.addRoundedRectangle(2.5f, 9.5f, 11.0f, 3.0f, 0.75f);
                break;
            case Glyph::play:
                polyline(f, { { 4.5f, 2.5f }, { 13.0f, 8.0f }, { 4.5f, 13.5f } }, true);
                break;
            case Glyph::pause:
                f.addRoundedRectangle(3.5f, 2.5f, 3.0f, 11.0f, 0.75f);
                f.addRoundedRectangle(9.5f, 2.5f, 3.0f, 11.0f, 0.75f);
                break;
            case Glyph::stop:
                f.addRoundedRectangle(3.25f, 3.25f, 9.5f, 9.5f, 1.25f);
                break;
            case Glyph::skipBack:
                line(p, 3.5f, 3.0f, 3.5f, 13.0f);
                polyline(p, { { 12.5f, 3.0f }, { 5.5f, 8.0f }, { 12.5f, 13.0f } }, true);
                break;
            case Glyph::skipForward:
                line(p, 12.5f, 3.0f, 12.5f, 13.0f);
                polyline(p, { { 3.5f, 3.0f }, { 10.5f, 8.0f }, { 3.5f, 13.0f } }, true);
                break;
            case Glyph::rewind:
                polyline(p, { { 8.0f, 4.0f }, { 2.0f, 8.0f }, { 8.0f, 12.0f } }, true);
                polyline(p, { { 14.0f, 4.0f }, { 8.0f, 8.0f }, { 14.0f, 12.0f } }, true);
                break;
            case Glyph::fastForward:
                polyline(p, { { 2.0f, 4.0f }, { 8.0f, 8.0f }, { 2.0f, 12.0f } }, true);
                polyline(p, { { 8.0f, 4.0f }, { 14.0f, 8.0f }, { 8.0f, 12.0f } }, true);
                break;
            case Glyph::record:
                f.addEllipse(3.0f, 3.0f, 10.0f, 10.0f);
                break;
            case Glyph::repeat:
                p.startNewSubPath(2.5f, 8.5f);
                p.lineTo(2.5f, 7.0f);
                p.quadraticTo(2.5f, 4.5f, 5.0f, 4.5f);
                p.lineTo(13.5f, 4.5f);
                polyline(p, { { 11.5f, 2.5f }, { 13.5f, 4.5f }, { 11.5f, 6.5f } });
                p.startNewSubPath(13.5f, 7.5f);
                p.lineTo(13.5f, 9.0f);
                p.quadraticTo(13.5f, 11.5f, 11.0f, 11.5f);
                p.lineTo(2.5f, 11.5f);
                polyline(p, { { 4.5f, 9.5f }, { 2.5f, 11.5f }, { 4.5f, 13.5f } });
                break;
            case Glyph::plus:
                line(p, 8.0f, 3.0f, 8.0f, 13.0f);
                line(p, 3.0f, 8.0f, 13.0f, 8.0f);
                break;
            case Glyph::cross:
                line(p, 4.0f, 4.0f, 12.0f, 12.0f);
                line(p, 12.0f, 4.0f, 4.0f, 12.0f);
                break;
            case Glyph::check:
                polyline(p, { { 3.0f, 8.5f }, { 6.5f, 12.0f }, { 13.0f, 4.5f } });
                break;
            case Glyph::caretDown:
                polyline(p, { { 4.0f, 6.0f }, { 8.0f, 10.0f }, { 12.0f, 6.0f } });
                break;
            case Glyph::floppy:
                polyline(p, { { 2.5f, 2.5f }, { 11.0f, 2.5f }, { 13.5f, 5.0f }, { 13.5f, 13.5f },
                              { 2.5f, 13.5f } }, true);
                polyline(p, { { 5.0f, 13.5f }, { 5.0f, 9.5f }, { 11.0f, 9.5f }, { 11.0f, 13.5f } });
                line(p, 5.5f, 5.0f, 9.5f, 5.0f);
                break;
            case Glyph::textT:
                line(p, 3.0f, 3.5f, 13.0f, 3.5f);
                line(p, 8.0f, 3.5f, 8.0f, 13.0f);
                break;
            case Glyph::selection:
            {
                struct Corner { float x, y, dx, dy; };
                for (const auto c : { Corner { 2.5f, 2.5f, 1.0f, 1.0f }, Corner { 13.5f, 2.5f, -1.0f, 1.0f },
                                      Corner { 2.5f, 13.5f, 1.0f, -1.0f }, Corner { 13.5f, 13.5f, -1.0f, -1.0f } })
                    polyline(p, { { c.x, c.y + 2.5f * c.dy }, { c.x, c.y }, { c.x + 2.5f * c.dx, c.y } });
                line(p, 6.75f, 2.5f, 9.25f, 2.5f);
                line(p, 6.75f, 13.5f, 9.25f, 13.5f);
                line(p, 2.5f, 6.75f, 2.5f, 9.25f);
                line(p, 13.5f, 6.75f, 13.5f, 9.25f);
                break;
            }
            case Glyph::scissors:
                p.addEllipse(2.0f, 9.5f, 4.0f, 4.0f);
                p.addEllipse(10.0f, 9.5f, 4.0f, 4.0f);
                line(p, 5.4f, 10.0f, 12.5f, 2.5f);
                line(p, 10.6f, 10.0f, 3.5f, 2.5f);
                break;
            case Glyph::cursor:
                polyline(p, { { 3.5f, 2.5f }, { 12.5f, 7.0f }, { 8.25f, 8.25f }, { 7.0f, 12.5f } }, true);
                line(p, 8.25f, 8.25f, 12.5f, 12.5f);
                break;
            case Glyph::zeroCrossing:
                line(p, 8.0f, 2.5f, 8.0f, 13.5f);
                line(p, 1.5f, 8.0f, 6.0f, 8.0f);
                polyline(p, { { 4.0f, 6.0f }, { 6.0f, 8.0f }, { 4.0f, 10.0f } });
                line(p, 14.5f, 8.0f, 10.0f, 8.0f);
                polyline(p, { { 12.0f, 6.0f }, { 10.0f, 8.0f }, { 12.0f, 10.0f } });
                break;
            case Glyph::warning:
                polyline(p, { { 8.0f, 1.75f }, { 14.25f, 8.0f }, { 8.0f, 14.25f }, { 1.75f, 8.0f } }, true);
                line(p, 8.0f, 5.0f, 8.0f, 8.5f);
                f.addEllipse(7.2f, 10.0f, 1.6f, 1.6f);
                break;
            case Glyph::sparkle:
                p.startNewSubPath(7.0f, 2.0f);
                p.quadraticTo(7.5f, 7.0f, 12.0f, 7.5f);
                p.quadraticTo(7.5f, 8.0f, 7.0f, 13.0f);
                p.quadraticTo(6.5f, 8.0f, 2.0f, 7.5f);
                p.quadraticTo(6.5f, 7.0f, 7.0f, 2.0f);
                p.closeSubPath();
                line(p, 12.5f, 1.5f, 12.5f, 4.5f);
                line(p, 11.0f, 3.0f, 14.0f, 3.0f);
                break;
            case Glyph::gear:
                p.addEllipse(5.75f, 5.75f, 4.5f, 4.5f);
                for (int i = 0; i < 8; ++i)
                {
                    const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                    line(p, 8.0f + 3.6f * std::sin(a), 8.0f - 3.6f * std::cos(a),
                         8.0f + 5.9f * std::sin(a), 8.0f - 5.9f * std::cos(a));
                }
                break;
            case Glyph::microphone:
                p.addRoundedRectangle(5.5f, 1.5f, 5.0f, 8.0f, 2.5f);
                p.startNewSubPath(3.0f, 7.5f);
                p.quadraticTo(3.0f, 12.0f, 8.0f, 12.0f);
                p.quadraticTo(13.0f, 12.0f, 13.0f, 7.5f);
                line(p, 8.0f, 12.0f, 8.0f, 14.5f);
                break;
            case Glyph::video:
                p.addRoundedRectangle(1.5f, 4.0f, 9.5f, 8.0f, 1.5f);
                polyline(p, { { 11.0f, 7.0f }, { 14.5f, 5.0f }, { 14.5f, 11.0f }, { 11.0f, 9.0f } });
                break;
            case Glyph::faders:
                line(p, 4.0f, 2.0f, 4.0f, 14.0f);
                line(p, 8.0f, 2.0f, 8.0f, 14.0f);
                line(p, 12.0f, 2.0f, 12.0f, 14.0f);
                f.addRoundedRectangle(2.5f, 9.0f, 3.0f, 2.5f, 0.75f);
                f.addRoundedRectangle(6.5f, 4.0f, 3.0f, 2.5f, 0.75f);
                f.addRoundedRectangle(10.5f, 7.0f, 3.0f, 2.5f, 0.75f);
                break;
            case Glyph::code:
                polyline(p, { { 5.0f, 4.5f }, { 1.5f, 8.0f }, { 5.0f, 11.5f } });
                polyline(p, { { 11.0f, 4.5f }, { 14.5f, 8.0f }, { 11.0f, 11.5f } });
                line(p, 9.5f, 3.0f, 6.5f, 13.0f);
                break;
            case Glyph::waves:
                for (float y : { 4.5f, 8.0f, 11.5f })
                {
                    p.startNewSubPath(2.0f, y);
                    p.quadraticTo(3.5f, y - 1.75f, 5.0f, y);
                    p.quadraticTo(6.5f, y + 1.75f, 8.0f, y);
                    p.quadraticTo(9.5f, y - 1.75f, 11.0f, y);
                    p.quadraticTo(12.5f, y + 1.75f, 14.0f, y);
                }
                break;
            case Glyph::transcript:
                line(p, 2.5f, 4.0f, 13.5f, 4.0f);
                line(p, 2.5f, 8.0f, 13.5f, 8.0f);
                line(p, 2.5f, 12.0f, 9.0f, 12.0f);
                break;
            case Glyph::keyboard:
                p.addRoundedRectangle(1.5f, 3.5f, 13.0f, 9.0f, 1.25f);
                for (float x : { 4.5f, 7.0f, 9.5f, 12.0f })
                    f.addEllipse(x - 0.6f, 6.0f - 0.6f, 1.2f, 1.2f);
                line(p, 5.0f, 9.75f, 11.0f, 9.75f);
                break;
            case Glyph::gauge:
                arc(p, 8.0f, 10.0f, 6.0f, -90.0f, 90.0f);
                line(p, 8.0f, 10.0f, 11.0f, 6.0f);
                line(p, 2.0f, 13.0f, 14.0f, 13.0f);
                break;
            case Glyph::envelope:
                polyline(p, { { 1.5f, 12.5f }, { 4.5f, 5.0f }, { 9.0f, 8.0f }, { 14.5f, 4.0f } });
                f.addEllipse(3.25f, 3.75f, 2.5f, 2.5f);
                f.addEllipse(7.75f, 6.75f, 2.5f, 2.5f);
                break;
            case Glyph::grid:
                p.addRoundedRectangle(2.5f, 2.5f, 4.5f, 4.5f, 1.0f);
                p.addRoundedRectangle(9.0f, 2.5f, 4.5f, 4.5f, 1.0f);
                p.addRoundedRectangle(2.5f, 9.0f, 4.5f, 4.5f, 1.0f);
                p.addRoundedRectangle(9.0f, 9.0f, 4.5f, 4.5f, 1.0f);
                break;
            case Glyph::export_:
                polyline(p, { { 5.5f, 6.0f }, { 2.5f, 6.0f }, { 2.5f, 13.5f }, { 13.5f, 13.5f },
                              { 13.5f, 6.0f }, { 10.5f, 6.0f } });
                line(p, 8.0f, 9.5f, 8.0f, 1.5f);
                polyline(p, { { 5.5f, 4.0f }, { 8.0f, 1.5f }, { 10.5f, 4.0f } });
                break;
            case Glyph::bandaid:
            {
                juce::Path band;
                band.addRoundedRectangle(-6.5f, -2.75f, 13.0f, 5.5f, 2.75f);
                band.applyTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::pi / 4.0f)
                                        .translated(8.0f, 8.0f));
                p.addPath(band);
                juce::Path pad;
                pad.addRectangle(-1.75f, -2.75f, 3.5f, 5.5f);
                pad.applyTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::pi / 4.0f)
                                       .translated(8.0f, 8.0f));
                p.addPath(pad);
                break;
            }
            case Glyph::spectrum:
                polyline(p, { { 1.5f, 13.0f }, { 4.0f, 6.0f }, { 6.5f, 10.0f }, { 9.0f, 3.0f },
                              { 11.5f, 9.0f }, { 14.5f, 7.0f } });
                break;
            case Glyph::arrowLeftRight:
                line(p, 2.0f, 8.0f, 14.0f, 8.0f);
                polyline(p, { { 4.5f, 5.5f }, { 2.0f, 8.0f }, { 4.5f, 10.5f } });
                polyline(p, { { 11.5f, 5.5f }, { 14.0f, 8.0f }, { 11.5f, 10.5f } });
                break;
            case Glyph::magnet:
                p.startNewSubPath(3.0f, 2.5f);
                p.lineTo(3.0f, 8.0f);
                p.quadraticTo(3.0f, 13.5f, 8.0f, 13.5f);
                p.quadraticTo(13.0f, 13.5f, 13.0f, 8.0f);
                p.lineTo(13.0f, 2.5f);
                p.lineTo(10.0f, 2.5f);
                p.lineTo(10.0f, 8.0f);
                p.quadraticTo(10.0f, 10.5f, 8.0f, 10.5f);
                p.quadraticTo(6.0f, 10.5f, 6.0f, 8.0f);
                p.lineTo(6.0f, 2.5f);
                p.closeSubPath();
                line(p, 3.0f, 5.5f, 6.0f, 5.5f);
                line(p, 10.0f, 5.5f, 13.0f, 5.5f);
                break;
            case Glyph::speakerSlash:
                polyline(p, { { 2.0f, 6.0f }, { 4.5f, 6.0f }, { 8.5f, 2.75f }, { 8.5f, 13.25f },
                              { 4.5f, 10.0f }, { 2.0f, 10.0f } }, true);
                line(p, 11.0f, 6.0f, 14.5f, 9.5f);
                line(p, 14.5f, 6.0f, 11.0f, 9.5f);
                break;
            case Glyph::trendUp:
                polyline(p, { { 1.5f, 12.5f }, { 6.0f, 8.0f }, { 9.0f, 11.0f }, { 14.0f, 4.5f } });
                polyline(p, { { 10.0f, 4.5f }, { 14.0f, 4.5f }, { 14.0f, 8.5f } });
                break;
            case Glyph::trendDown:
                polyline(p, { { 1.5f, 3.5f }, { 6.0f, 8.0f }, { 9.0f, 5.0f }, { 14.0f, 11.5f } });
                polyline(p, { { 10.0f, 11.5f }, { 14.0f, 11.5f }, { 14.0f, 7.5f } });
                break;
            case Glyph::normalize:
                line(p, 8.0f, 1.5f, 8.0f, 14.5f);
                polyline(p, { { 5.5f, 4.0f }, { 8.0f, 1.5f }, { 10.5f, 4.0f } });
                polyline(p, { { 5.5f, 12.0f }, { 8.0f, 14.5f }, { 10.5f, 12.0f } });
                line(p, 2.0f, 8.0f, 14.0f, 8.0f);
                break;
            case Glyph::trash:
                line(p, 2.0f, 4.0f, 14.0f, 4.0f);
                polyline(p, { { 6.0f, 4.0f }, { 6.0f, 2.25f }, { 10.0f, 2.25f }, { 10.0f, 4.0f } });
                polyline(p, { { 3.5f, 4.0f }, { 4.25f, 14.0f }, { 11.75f, 14.0f }, { 12.5f, 4.0f } });
                line(p, 6.5f, 7.0f, 6.5f, 11.0f);
                line(p, 9.5f, 7.0f, 9.5f, 11.0f);
                break;
            case Glyph::copy:
                p.addRoundedRectangle(5.5f, 5.5f, 8.5f, 8.5f, 1.25f);
                polyline(p, { { 10.5f, 5.5f }, { 10.5f, 2.0f }, { 2.0f, 2.0f }, { 2.0f, 10.5f }, { 5.5f, 10.5f } });
                break;
            case Glyph::clipboard:
                polyline(p, { { 5.5f, 3.0f }, { 3.0f, 3.0f }, { 3.0f, 14.0f }, { 13.0f, 14.0f }, { 13.0f, 3.0f },
                              { 10.5f, 3.0f } });
                p.addRoundedRectangle(5.5f, 1.75f, 5.0f, 3.0f, 1.0f);
                line(p, 5.5f, 8.5f, 10.5f, 8.5f);
                line(p, 5.5f, 11.0f, 9.0f, 11.0f);
                break;
            case Glyph::crop:
                polyline(p, { { 4.0f, 1.5f }, { 4.0f, 12.0f }, { 14.5f, 12.0f } });
                polyline(p, { { 1.5f, 4.0f }, { 12.0f, 4.0f }, { 12.0f, 14.5f } });
                break;
            case Glyph::reverse:
                line(p, 2.0f, 5.0f, 13.0f, 5.0f);
                polyline(p, { { 10.5f, 2.5f }, { 13.0f, 5.0f }, { 10.5f, 7.5f } });
                line(p, 14.0f, 11.0f, 3.0f, 11.0f);
                polyline(p, { { 5.5f, 8.5f }, { 3.0f, 11.0f }, { 5.5f, 13.5f } });
                break;
            case Glyph::split:
                line(p, 8.0f, 1.5f, 8.0f, 14.5f);
                polyline(p, { { 5.5f, 4.0f }, { 2.0f, 4.0f }, { 2.0f, 12.0f }, { 5.5f, 12.0f } });
                polyline(p, { { 10.5f, 4.0f }, { 14.0f, 4.0f }, { 14.0f, 12.0f }, { 10.5f, 12.0f } });
                break;
            case Glyph::eyedropper:
                line(p, 2.0f, 14.0f, 9.0f, 7.0f);
                polyline(p, { { 7.0f, 5.0f }, { 11.0f, 9.0f } });
                p.startNewSubPath(9.0f, 7.0f);
                p.lineTo(11.5f, 4.5f);
                p.quadraticTo(13.0f, 3.0f, 14.0f, 2.0f);
                break;
            case Glyph::speed:
                arc(p, 8.0f, 9.5f, 6.0f, -120.0f, 120.0f);
                line(p, 8.0f, 9.5f, 11.5f, 5.5f);
                f.addEllipse(7.0f, 8.5f, 2.0f, 2.0f);
                break;
            case Glyph::layout:
                p.addRoundedRectangle(2.0f, 2.5f, 12.0f, 11.0f, 1.25f);
                line(p, 2.0f, 6.5f, 14.0f, 6.5f);
                line(p, 6.5f, 6.5f, 6.5f, 13.5f);
                break;
        }
        return s;
    }
}

/** Draws @p glyph centred in @p area, as a square @p size across (the area's
    smaller side when 0). The stroke is the design's 1.5 units at 16. */
inline void draw(juce::Graphics& g, Glyph glyph, juce::Rectangle<float> area, juce::Colour colour,
                 float size = 0.0f)
{
    const float side  = size > 0.0f ? size : juce::jmin(area.getWidth(), area.getHeight());
    const auto  box   = juce::Rectangle<float>(side, side).withCentre(area.getCentre());
    const float scale = side / 16.0f;
    const auto  xf    = juce::AffineTransform::scale(scale).translated(box.getX(), box.getY());

    const auto shape = detail::build(glyph);
    g.setColour(colour);
    if (! shape.fill.isEmpty())
        g.fillPath(shape.fill, xf);
    if (! shape.stroke.isEmpty())
        g.strokePath(shape.stroke,
                     juce::PathStrokeType(juce::jmax(1.0f, 1.5f * scale), juce::PathStrokeType::curved,
                                          juce::PathStrokeType::rounded),
                     xf);
}

/** The glyph a pane's tab shows beside its name. */
inline Glyph forPanel(const juce::String& name)
{
    struct Entry { const char* name; Glyph glyph; };
    static const Entry entries[] = {
        { "Files", Glyph::folder },          { "Open Files", Glyph::waveform },
        { "Transport", Glyph::play },        { "Tracks", Glyph::rows },
        { "Audio", Glyph::waveform },
        { "Mastering", Glyph::sliders },     { "Analyser", Glyph::spectrum },
        { "Diagnostics", Glyph::warning },   { "Essential Sound", Glyph::sparkle },
        { "Script", Glyph::code },           { "Delivery", Glyph::check },
        { "History", Glyph::history },       { "Transcript", Glyph::transcript },
        { "Video", Glyph::video },           { "Automation", Glyph::envelope },
        { "Track FX", Glyph::stack },
        { "Mixer", Glyph::faders },          { "Master", Glyph::gauge },
        { "Markers", Glyph::bookmark },
    };
    for (const auto& e : entries)
        if (name == e.name)
            return e.glyph;
    return Glyph::layout;
}

} // namespace soundsplice::glyphs
