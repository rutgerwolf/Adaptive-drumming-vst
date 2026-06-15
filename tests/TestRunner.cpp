#include <JuceHeader.h>
#include <iostream>

/**
 * Console entry point for the unit-test suite.
 *
 * Runs every registered juce::UnitTest and returns a non-zero exit code if any
 * check fails, so CI (and CTest) treat a failed assertion as a failed build.
 */
class ConsoleLogger : public juce::Logger
{
public:
    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;
    }
};

int main()
{
    ConsoleLogger logger;
    juce::Logger::setCurrentLogger (&logger);

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;
    int checks   = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        if (const auto* r = runner.getResult (i))
        {
            failures += r->failures;
            checks   += r->passes + r->failures;
        }
    }

    std::cout << "\n==== " << checks << " checks run, "
              << failures << " failure(s) ====" << std::endl;

    juce::Logger::setCurrentLogger (nullptr);
    return failures > 0 ? 1 : 0;
}
