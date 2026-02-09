#pragma once

#include <juce_core/juce_core.h>

/**
 * Centralized version information for Pikon Raditsz
 *
 * This class provides a single source of truth for application
 * version information, branding, and build details.
 */
class VersionInfo
{
public:
    // Application identity
    static constexpr const char* APPLICATION_NAME = "Pikon Raditsz";
    static constexpr const char* AUTHOR = "Monsieur Pimpant";

    // Version information
    static constexpr const char* VERSION = "0.9";
    static constexpr const char* VERSION_FULL = "0.9.0-beta";
    static constexpr int         VERSION_MAJOR = 0;
    static constexpr int         VERSION_MINOR = 9;
    static constexpr int         VERSION_PATCH = 0;

    // Build information
    static constexpr const char* BUILD_TYPE = "Beta Test Release";

    // Convenience getters
    static juce::String getVersionString() { return juce::String(VERSION); }
    static juce::String getFullVersionString() { return juce::String(VERSION_FULL); }
    static juce::String getApplicationName() { return juce::String(APPLICATION_NAME); }
    static juce::String getAuthorString() { return juce::String(AUTHOR); }
    static juce::String getBuildTypeString() { return juce::String(BUILD_TYPE); }

    // Combined info strings
    static juce::String getBuildInfoString()
    {
        return juce::String(APPLICATION_NAME) + " " + VERSION_FULL + " - " + BUILD_TYPE;
    }

    static juce::String getAboutString()
    {
        juce::String about;
        about << APPLICATION_NAME << "\n";
        about << "Version " << VERSION_FULL << "\n";
        about << BUILD_TYPE << "\n\n";
        about << "By " << AUTHOR;
        return about;
    }
};
