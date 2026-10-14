#pragma once

#include <juce_core/juce_core.h>

/** Minimal client for the Pro Tools Scripting Library (PTSL) server on localhost.

    PTSL is a gRPC service whose request and response bodies are JSON strings. This speaks
    just enough HTTP/2 (cleartext, prior knowledge) and protobuf for its unary call, so the
    plugin needs no gRPC or protobuf libraries. Calls block: never use it on the audio or
    message thread. */
class PtslClient
{
public:
    enum Command
    {
        getPtslVersion             = 55,
        registerConnection         = 70,
        getTimelineSelection       = 82,
        getTrackControlInfo        = 148,
        getTrackControlBreakpoints = 149,
        setTrackControlBreakpoints = 150
    };

    struct Result
    {
        bool ok = false;
        juce::var body;
        juce::String error;
    };

    /** Registers with Pro Tools first if needed. */
    Result call (Command, const juce::var& body = {});

    /** Builds a JSON object for a request body. */
    static juce::var object (std::initializer_list<std::pair<const char*, juce::var>>);

private:
    Result send (Command, const juce::var& body);
    bool connect (juce::String& error);

    juce::String sessionId;
    int version = 1, versionMinor = 0, versionRevision = 0; // Pro Tools refuses version 0, even to ask its version
};
