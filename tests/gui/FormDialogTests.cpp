#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/FormDialog.h>

using namespace soundsplice;

namespace
{
    struct Fixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
        juce::TemporaryFile             file { ".settings" };
        juce::PropertiesFile            settings { file.getFile(), juce::PropertiesFile::Options() };
        juce::Component                 owner;
    };

    void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(50); }
}

TEST_CASE("A form opens with what was used last, and saves what it's given, clamped", "[gui][formdialog]")
{
    Fixture f;
    f.settings.setValue("test.gain", 6.5);
    f.settings.setValue("test.mode", 2);

    FormDialog form(f.owner, f.settings, "Test");
    form.number("test.gain", "Gain:", -3.0, -24.0, 24.0)
        .number("test.width", "Width:", 2.0, 0.1, 20.0)
        .integer("test.times", "Times:", 4, 1, 10)
        .choice("test.mode", "Mode:", { "A", "B", "C" }, 0)
        .toggle("test.limit", "Limit", true)
        .text("test.name", "Name:", "Untitled");

    auto& window = form.window();
    REQUIRE(window.getTextEditorContents("test.gain") == juce::String(6.5));  // remembered
    REQUIRE(window.getTextEditorContents("test.width") == juce::String(2.0)); // the fallback
    REQUIRE(window.getTextEditorContents("test.times") == "4");
    REQUIRE(window.getComboBoxComponent("test.mode")->getSelectedItemIndex() == 2);
    REQUIRE(window.getTextEditorContents("test.name") == "Untitled");

    window.getTextEditor("test.gain")->setText("99");    // over the top
    window.getTextEditor("test.width")->setText("0");    // under the bottom
    window.getTextEditor("test.times")->setText("7");
    window.getTextEditor("test.name")->setText("  Mine  ");

    bool ran = false;
    form.applyNow([&](const FormDialog::Values& v)
    {
        ran = true;
        REQUIRE(v.number("test.gain") == 24.0);
        REQUIRE(v.number("test.width") == 0.1);
        REQUIRE(v.integer("test.times") == 7);
        REQUIRE(v.choice("test.mode") == 2);
        REQUIRE(v.toggle("test.limit"));
        REQUIRE(v.text("test.name") == "Mine");
        REQUIRE(&v.window() == &window);
    });
    REQUIRE(ran);

    // Saved as read, before the action ran.
    REQUIRE(f.settings.getDoubleValue("test.gain") == 24.0);
    REQUIRE(std::abs(f.settings.getDoubleValue("test.width") - 0.1) < 1.0e-12);
    REQUIRE(f.settings.getIntValue("test.times") == 7);
    REQUIRE(f.settings.getIntValue("test.mode") == 2);
    REQUIRE(f.settings.getBoolValue("test.limit", false));
    REQUIRE(f.settings.getValue("test.name") == "Mine");
}

TEST_CASE("An unsaved field starts at its fallback and leaves the settings alone", "[gui][formdialog]")
{
    Fixture f;
    f.settings.setValue("rate", 5);
    f.settings.setValue("bpm", "140");

    FormDialog form(f.owner, f.settings, "Test");
    form.choice("rate", "Rate:", { "a", "b", "c", "d", "e", "f", "g" }, 1).unsaved()
        .text("bpm", "Tempo:", "120.00").unsaved();

    REQUIRE(form.window().getComboBoxComponent("rate")->getSelectedItemIndex() == 1);
    REQUIRE(form.window().getTextEditorContents("bpm") == "120.00");

    form.window().getComboBoxComponent("rate")->setSelectedItemIndex(3);
    int chosen = -1;
    form.applyNow([&](const FormDialog::Values& v) { chosen = v.choice("rate"); });

    REQUIRE(chosen == 3);
    REQUIRE(f.settings.getIntValue("rate") == 5);
    REQUIRE(f.settings.getValue("bpm") == "140");
}

TEST_CASE("Applying runs the action once the dialog closes; cancelling doesn't", "[gui][formdialog]")
{
    Fixture f;
    int applied = 0, cancelled = 0, previewsEnded = 0;
    std::function<void()> previewRun;

    const auto open = [&](int result)
    {
        FormDialog form(f.owner, f.settings, "Test");
        form.number("test.amount", "Amount:", 50.0, 0.0, 100.0)
            .withPreview([&](juce::AlertWindow&, std::function<void()> run) { previewRun = std::move(run); },
                         [&] { ++previewsEnded; })
            .cancelButton("Discard", [&] { ++cancelled; });
        auto* window = &form.window();
        form.show("Apply", [&](const FormDialog::Values& v)
        {
            ++applied;
            REQUIRE(v.number("test.amount") == 50.0);
        });

        // The preview runs the same action, without the dialog closing.
        REQUIRE(previewRun != nullptr);
        previewRun();
        REQUIRE(applied == 1);

        window->exitModalState(result); // deletes it, once the callback runs
        pump();
        previewRun = nullptr;
    };

    open(1);
    REQUIRE(applied == 2);
    REQUIRE(cancelled == 0);
    REQUIRE(previewsEnded == 1);

    applied = 0;
    open(0);
    REQUIRE(applied == 1); // the preview only
    REQUIRE(cancelled == 1);
    REQUIRE(previewsEnded == 2);
}

TEST_CASE("Nothing runs if the dialog's owner has gone by the time it closes", "[gui][formdialog]")
{
    Fixture f;
    auto owner   = std::make_unique<juce::Component>();
    int  applied = 0, previewsEnded = 0;

    FormDialog form(*owner, f.settings, "Test");
    form.number("test.amount", "Amount:", 50.0, 0.0, 100.0)
        .withPreview([](juce::AlertWindow&, std::function<void()>) {}, [&] { ++previewsEnded; });
    auto* window = &form.window();
    form.show("Apply", [&](const FormDialog::Values&) { ++applied; });

    owner.reset();
    window->exitModalState(1);
    pump();

    REQUIRE(applied == 0);
    REQUIRE(previewsEnded == 0);
    REQUIRE(! f.settings.containsKey("test.amount"));
}
