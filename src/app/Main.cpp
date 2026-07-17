#include <juce_audio_utils/juce_audio_utils.h>

#include "MainComponent.h"

namespace looper
{
/** Application entry point and top-level window for Phase 0. */
class LooperAudioApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "Looper-Audio"; }
    const juce::String getApplicationVersion() override { return "0.0.1"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String&) override
    {
        logger.reset(juce::FileLogger::createDefaultAppLogger(
            "Looper-Audio", "Looper-Audio.log",
            "Looper-Audio " + getApplicationVersion() + " starting up"));
        juce::Logger::setCurrentLogger(logger.get());
        juce::Logger::writeToLog("System: " + juce::SystemStats::getOperatingSystemName()
            + " (" + juce::SystemStats::getDeviceDescription() + ")");

        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::Logger::writeToLog("Looper-Audio shutting down");
        juce::Logger::setCurrentLogger(nullptr);
        logger = nullptr;
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow(const juce::String& name)
            : DocumentWindow(name,
                             juce::Desktop::getInstance().getDefaultLookAndFeel()
                                 .findColour(juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::FileLogger> logger;
};

} // namespace looper

START_JUCE_APPLICATION(looper::LooperAudioApplication)
