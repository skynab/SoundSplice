#pragma once

#include <optional>
#include <vector>

#include <juce_core/juce_core.h>

#include "ExportChoice.h"

namespace soundsplice::app
{
/**
    An export's choices kept for later: as a render preset (a name and the
    choices, from Export Audio's Preset box), and as a render queue item (the
    choices with a snapshot of the project and where the file goes, rendered
    later by the app's headless render - `SoundSplice --render-job`).

    Written as XML attributes, by name, so a preset or a queue saved by one
    version reads in the next even as options are added.
*/
namespace exportchoices
{
    inline void write(juce::XmlElement& e, const ExportChoice& c)
    {
        const auto& o = c.options;
        e.setAttribute("format", engine::extensionFor(o.format));
        e.setAttribute("contents", (int) o.contents);
        e.setAttribute("rate", o.sampleRate);
        e.setAttribute("bits", o.bitsPerSample);
        e.setAttribute("quality", o.qualityIndex);
        e.setAttribute("dither", o.dither);
        e.setAttribute("noiseShaping", o.noiseShaping);
        e.setAttribute("loudness", o.loudnessLufs);
        e.setAttribute("ceiling", o.truePeakCeilingDb);
        e.setAttribute("range", (int) c.range);
        e.setAttribute("names", c.namePattern);
        e.setAttribute("tagging", (int) c.tagging);
        e.setAttribute("report", c.report);
        e.setAttribute("selectionStart", c.selectionStartBeats);
        e.setAttribute("selectionLength", c.selectionLengthBeats);
    }

    inline ExportChoice read(const juce::XmlElement& e)
    {
        ExportChoice c;
        auto& o = c.options;
        const auto extension = e.getStringAttribute("format", "wav");
        for (const auto format : engine::allExportFormats())
            if (engine::extensionFor(format) == extension)
                o.format = format;
        o.contents          = (engine::ExportContents) juce::jlimit(0, engine::kNumExportContents - 1, e.getIntAttribute("contents"));
        o.sampleRate        = e.getDoubleAttribute("rate", 48000.0);
        o.bitsPerSample     = e.getIntAttribute("bits", 24);
        o.qualityIndex      = e.getIntAttribute("quality", 3);
        o.dither            = e.getBoolAttribute("dither", true);
        o.noiseShaping      = e.getBoolAttribute("noiseShaping", false);
        o.loudnessLufs      = juce::jmin(0.0, e.getDoubleAttribute("loudness", 0.0));
        o.truePeakCeilingDb = e.getDoubleAttribute("ceiling", -1.0);
        c.range                = (ExportRange) juce::jlimit(0, 2, e.getIntAttribute("range"));
        c.namePattern          = e.getStringAttribute("names", "$project - $region");
        c.tagging              = (ExportTagging) juce::jlimit(0, 2, e.getIntAttribute("tagging"));
        c.report               = e.getBoolAttribute("report", false);
        c.selectionStartBeats  = e.getDoubleAttribute("selectionStart", 0.0);
        c.selectionLengthBeats = e.getDoubleAttribute("selectionLength", 0.0);
        return c;
    }

    // ---- presets

    struct Preset
    {
        juce::String name;
        ExportChoice choice;
    };

    inline juce::String serializePresets(const std::vector<Preset>& presets)
    {
        juce::XmlElement root("RENDER_PRESETS");
        for (const auto& preset : presets)
        {
            auto* e = root.createNewChildElement("PRESET");
            e->setAttribute("name", preset.name);
            write(*e, preset.choice);
        }
        return root.toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
    }

    inline std::vector<Preset> deserializePresets(const juce::String& text)
    {
        std::vector<Preset> presets;
        if (const auto root = juce::parseXML(text); root != nullptr && root->hasTagName("RENDER_PRESETS"))
            for (auto* e : root->getChildWithTagNameIterator("PRESET"))
                if (e->getStringAttribute("name").isNotEmpty())
                    presets.push_back({ e->getStringAttribute("name"), read(*e) });
        return presets;
    }

    /** @p presets with @p preset added, replacing one of the same name. */
    inline std::vector<Preset> withPreset(std::vector<Preset> presets, Preset preset)
    {
        for (auto& existing : presets)
            if (existing.name == preset.name)
            {
                existing = std::move(preset);
                return presets;
            }
        presets.push_back(std::move(preset));
        return presets;
    }

    // ---- queue

    struct Job
    {
        juce::File   project; // a snapshot, made when it was queued
        juce::File   output;
        ExportChoice choice;
        juce::String label; // what the queue shows
    };

    inline juce::String serializeJob(const Job& job)
    {
        juce::XmlElement e("RENDER_JOB");
        e.setAttribute("project", job.project.getFullPathName());
        e.setAttribute("output", job.output.getFullPathName());
        e.setAttribute("label", job.label);
        write(e, job.choice);
        return e.toString();
    }

    inline std::optional<Job> readJob(const juce::XmlElement& e)
    {
        if (! e.hasTagName("RENDER_JOB"))
            return std::nullopt;
        Job job { juce::File(e.getStringAttribute("project")), juce::File(e.getStringAttribute("output")), read(e),
                  e.getStringAttribute("label") };
        if (job.project == juce::File() || job.output == juce::File())
            return std::nullopt;
        return job;
    }

    inline std::optional<Job> deserializeJob(const juce::String& text)
    {
        const auto e = juce::parseXML(text);
        return e != nullptr ? readJob(*e) : std::nullopt;
    }

    inline juce::String serializeQueue(const std::vector<Job>& jobs)
    {
        juce::XmlElement root("RENDER_QUEUE");
        for (const auto& job : jobs)
            if (auto e = juce::parseXML(serializeJob(job)))
                root.addChildElement(e.release());
        return root.toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
    }

    inline std::vector<Job> deserializeQueue(const juce::String& text)
    {
        std::vector<Job> jobs;
        if (const auto root = juce::parseXML(text); root != nullptr && root->hasTagName("RENDER_QUEUE"))
            for (auto* e : root->getChildWithTagNameIterator("RENDER_JOB"))
                if (auto job = readJob(*e))
                    jobs.push_back(std::move(*job));
        return jobs;
    }
}

} // namespace soundsplice::app
