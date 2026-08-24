// FBKTrainer - Main.cpp
//
// The application shell.
//
// One detail here is safety-relevant rather than cosmetic: the process asks
// Windows not to sleep while it is open. An unattended multi-day run on a machine
// that suspends is not merely a run that stops - it is a run that stops with the
// console still holding whatever gain it was last commanded, because nothing in
// the console reverts on its own when the computer that was driving it goes away.
// The watchdog covers the case where this program stalls; it cannot cover the
// case where the whole machine does. Declining to sleep is what covers that.
#include "MainComponent.h"

#include <JuceHeader.h>

namespace fbkt
{
class FBKTrainerApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "FBKTrainer"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        // Sleep and display blanking are both refused. A blanked display is
        // harmless; the setting that matters is the one next to it, and asking
        // for them separately is not possible on every Windows version.
        juce::Desktop::getInstance().setScreenSaverEnabled (false);

        mainWindow_ = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow_ = nullptr;
        juce::Desktop::getInstance().setScreenSaverEnabled (true);
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : juce::DocumentWindow (name,
                                    juce::Desktop::getInstance().getDefaultLookAndFeel()
                                        .findColour (juce::ResizableWindow::backgroundColourId),
                                    juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (820, 560, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow_;
};
} // namespace fbkt

START_JUCE_APPLICATION (fbkt::FBKTrainerApplication)
