/** Renders the editor to a PNG with no window, no host and no screen.

    createComponentSnapshot paints an unparented component into an image, so
    the UI can be checked on a build machine or over a terminal, neither of
    which can be asked to look at a window.

    The audio thread is run at roughly real time first, on a tone under noise with a
    thump every eighth block, so the display has events to show. With a second
    argument the editor is rendered once per reaction into <name>-<REACTION>.png.
*/

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"

namespace
{
void setParam (SquelchAudioProcessor& p, const char* id, float real)
{
    if (auto* param = p.apvts.getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (real));
}

void run (SquelchAudioProcessor& p, int blocks)
{
    constexpr int blockSize = 512;
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    juce::Random random { 5 };
    double phase = 0.0;

    for (int block = 0; block < blocks; ++block)
    {
        const auto thump = block % 8 == 0;

        for (int i = 0; i < blockSize; ++i)
        {
            phase += 110.0 / 44100.0;
            const auto tone = (float) std::sin (phase * juce::MathConstants<double>::twoPi) * 0.2f;
            const auto noise = (random.nextFloat() * 2.0f - 1.0f) * (thump && i < 200 ? 0.6f : 0.04f);

            buffer.setSample (0, i, tone + noise);
            buffer.setSample (1, i, tone - noise);
        }

        p.processBlock (buffer, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (12);
    }
}

bool write (const juce::Image& image, const juce::File& out)
{
    out.getParentDirectory().createDirectory();
    out.deleteFile();

    juce::FileOutputStream stream (out);

    if (! stream.openedOk() || ! juce::PNGImageFormat().writeImageToStream (image, stream))
    {
        std::printf ("could not write %s\n", out.getFullPathName().toRawUTF8());
        return false;
    }

    std::printf ("%d x %d -> %s\n", image.getWidth(), image.getHeight(), out.getFullPathName().toRawUTF8());
    return true;
}
}

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

    const auto base = juce::File::getCurrentWorkingDirectory().getChildFile (
        argc > 1 ? argv[1] : "test-results/editor.png");

    if (argc <= 2)
    {
        run (processor, 400);
        return write (editor->createComponentSnapshot (editor->getLocalBounds(), false, 1.0f), base) ? 0 : 1;
    }

    // One render per reaction, so each reaction's sweep shape is seen.
    for (int reaction = 0; reaction < squelch::reactionNames.size(); ++reaction)
    {
        setParam (processor, squelch::ids::reaction, (float) reaction);
        run (processor, 400);

        const auto name = base.getFileNameWithoutExtension() + "-" + squelch::reactionNames[reaction] + ".png";

        if (! write (editor->createComponentSnapshot (editor->getLocalBounds(), false, 1.0f),
                     base.getSiblingFile (name)))
            return 1;
    }

    return 0;
}
