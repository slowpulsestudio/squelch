/** Renders the editor to a PNG with no window, no host and no screen.

    createComponentSnapshot paints an unparented component into an image, so
    the UI can be checked on a build machine or over a terminal, neither of
    which can be asked to look at a window.
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;

    SquelchAudioProcessor processor;
    processor.prepareToPlay (44100.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    if (editor == nullptr)
    {
        std::puts ("the processor made no editor");
        return 1;
    }

    editor->setTopLeftPosition (0, 0);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), false, 1.0f);

    const juce::File out = argc > 1
        ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])
        : juce::File::getCurrentWorkingDirectory().getChildFile ("test-results/editor.png");

    out.getParentDirectory().createDirectory();
    out.deleteFile();

    juce::FileOutputStream stream (out);

    if (! stream.openedOk() || ! juce::PNGImageFormat().writeImageToStream (image, stream))
    {
        std::printf ("could not write %s\n", out.getFullPathName().toRawUTF8());
        return 1;
    }

    std::printf ("%d x %d -> %s\n", image.getWidth(), image.getHeight(),
                 out.getFullPathName().toRawUTF8());
    return 0;
}
