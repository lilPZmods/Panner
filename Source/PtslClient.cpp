#include "PtslClient.h"

#include <sys/socket.h>

namespace
{
    constexpr int ptslPort = 31416;
    constexpr int timeoutMs = 5000;
    constexpr int defaultWindow = 65535;
    constexpr int ourWindow = 1 << 24;
    constexpr size_t maxFrameSize = 16384; // HTTP/2 minimum; every peer accepts it

    enum FrameType : uint8_t { dataFrame = 0, headersFrame = 1, rstStreamFrame = 3, settingsFrame = 4,
                               pingFrame = 6, goAwayFrame = 7, windowUpdateFrame = 8 };

    enum FrameFlag : uint8_t { endStream = 0x1, ackFlag = 0x1, endHeaders = 0x4, paddedFlag = 0x8 };

    //==============================================================================
    // Protobuf, only the parts PTSL's Request / Response messages need.

    void putVarint (juce::MemoryOutputStream& out, uint64_t v)
    {
        while (v >= 0x80)
        {
            out.writeByte ((char) (v | 0x80));
            v >>= 7;
        }

        out.writeByte ((char) v);
    }

    void putField (juce::MemoryOutputStream& out, int field, uint64_t value)
    {
        if (value == 0)
            return;

        putVarint (out, (uint64_t) field << 3);
        putVarint (out, value);
    }

    void putField (juce::MemoryOutputStream& out, int field, const void* data, size_t size)
    {
        if (size == 0)
            return;

        putVarint (out, ((uint64_t) field << 3) | 2);
        putVarint (out, size);
        out.write (data, size);
    }

    void putField (juce::MemoryOutputStream& out, int field, const juce::String& s)
    {
        putField (out, field, s.toRawUTF8(), s.getNumBytesAsUTF8());
    }

    struct ProtoReader
    {
        const uint8_t* pos;
        const uint8_t* end;

        bool varint (uint64_t& v)
        {
            v = 0;

            for (int shift = 0; pos < end && shift < 64; shift += 7)
            {
                const auto byte = *pos++;
                v |= (uint64_t) (byte & 0x7f) << shift;

                if ((byte & 0x80) == 0)
                    return true;
            }

            return false;
        }

        /** Reads the next field. Length-delimited fields land in bytes, varints in value. */
        bool next (int& field, uint64_t& value, juce::MemoryBlock& bytes)
        {
            uint64_t tag;

            if (pos >= end || ! varint (tag))
                return false;

            field = (int) (tag >> 3);

            switch (tag & 7)
            {
                case 0: return varint (value);
                case 1: pos += 8; return pos <= end;
                case 5: pos += 4; return pos <= end;
                case 2:
                {
                    uint64_t size;

                    if (! varint (size) || size > (uint64_t) (end - pos))
                        return false;

                    bytes.replaceAll (pos, (size_t) size);
                    pos += size;
                    return true;
                }
                default: return false;
            }
        }
    };

    //==============================================================================
    // HPACK, sending side only: every header goes as a plain literal.

    void hpackInt (juce::MemoryOutputStream& out, size_t v, int prefixBits)
    {
        const size_t limit = (1u << prefixBits) - 1;

        if (v < limit)
        {
            out.writeByte ((char) v);
            return;
        }

        out.writeByte ((char) limit);

        for (v -= limit; v >= 128; v >>= 7)
            out.writeByte ((char) ((v & 127) | 128));

        out.writeByte ((char) v);
    }

    void hpackLiteral (juce::MemoryOutputStream& out, const char* name, const juce::String& value)
    {
        out.writeByte (0); // literal without indexing, new name

        hpackInt (out, std::strlen (name), 7);
        out.write (name, std::strlen (name));

        hpackInt (out, value.getNumBytesAsUTF8(), 7);
        out.write (value.toRawUTF8(), value.getNumBytesAsUTF8());
    }

    //==============================================================================
    /** One HTTP/2 connection carrying a single gRPC unary call on stream 1. */
    class Http2Call
    {
    public:
        juce::String error;

        bool run (const juce::MemoryBlock& message, juce::MemoryBlock& reply)
        {
            if (! socket.connect ("127.0.0.1", ptslPort, timeoutMs))
                return fail ("Can't reach Pro Tools. Is it running with a session open?");

           #ifdef SO_NOSIGPIPE
            // A dropped connection must come back as an error, not a SIGPIPE that kills the host.
            const int on = 1;
            setsockopt (socket.getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
           #endif

            static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
            juce::MemoryOutputStream settings;
            settings.writeShortBigEndian (0x2);   // ENABLE_PUSH
            settings.writeIntBigEndian (0);
            settings.writeShortBigEndian (0x4);   // INITIAL_WINDOW_SIZE
            settings.writeIntBigEndian (ourWindow);

            juce::MemoryOutputStream increment;
            increment.writeIntBigEndian (ourWindow - defaultWindow);

            juce::MemoryOutputStream headers;
            hpackLiteral (headers, ":method", "POST");
            hpackLiteral (headers, ":scheme", "http");
            hpackLiteral (headers, ":path", "/ptsl.PTSL/SendGrpcRequest");
            hpackLiteral (headers, ":authority", "localhost:" + juce::String (ptslPort));
            hpackLiteral (headers, "content-type", "application/grpc");
            hpackLiteral (headers, "te", "trailers");

            if (! writeAll (preface, std::strlen (preface))
                || ! writeFrame (settingsFrame, 0, 0, settings.getData(), settings.getDataSize())
                || ! writeFrame (windowUpdateFrame, 0, 0, increment.getData(), increment.getDataSize())
                || ! writeFrame (headersFrame, endHeaders, 1, headers.getData(), headers.getDataSize()))
                return fail ("Lost the connection to Pro Tools.");

            if (! sendBody (message))
                return false;

            return readReply (reply);
        }

    private:
        struct Frame
        {
            uint8_t type = 0, flags = 0;
            uint32_t stream = 0;
            juce::MemoryBlock payload;
        };

        bool fail (const juce::String& message)
        {
            error = message;
            return false;
        }

        bool writeFrame (uint8_t type, uint8_t flags, uint32_t stream, const void* data, size_t size)
        {
            const uint8_t header[9] { (uint8_t) (size >> 16), (uint8_t) (size >> 8), (uint8_t) size, type, flags,
                                      (uint8_t) (stream >> 24), (uint8_t) (stream >> 16), (uint8_t) (stream >> 8), (uint8_t) stream };

            return writeAll (header, 9) && writeAll (data, size);
        }

        /** The socket is non-blocking, so send() can take less than it was given. */
        bool writeAll (const void* data, size_t size)
        {
            auto* d = static_cast<const char*> (data);

            while (size > 0)
            {
                const int n = socket.write (d, (int) size);

                if (n > 0)
                {
                    d += n;
                    size -= (size_t) n;
                }
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    return false;
                }
                else if (socket.waitUntilReady (false, timeoutMs) != 1)
                {
                    return false;
                }
            }

            return true;
        }

        bool readExact (void* dest, size_t size)
        {
            auto* d = static_cast<char*> (dest);

            while (size > 0)
            {
                if (socket.waitUntilReady (true, timeoutMs) != 1)
                    return false;

                const int n = socket.read (d, (int) size, false);

                if (n <= 0)
                    return false;

                d += n;
                size -= (size_t) n;
            }

            return true;
        }

        bool readFrame (Frame& f)
        {
            uint8_t h[9];

            if (! readExact (h, 9))
                return fail ("Pro Tools stopped answering.");

            const size_t size = ((size_t) h[0] << 16) | ((size_t) h[1] << 8) | h[2];
            f.type = h[3];
            f.flags = h[4];
            f.stream = (((uint32_t) h[5] & 0x7f) << 24) | ((uint32_t) h[6] << 16) | ((uint32_t) h[7] << 8) | h[8];
            f.payload.setSize (size);

            if (size > 0 && ! readExact (f.payload.getData(), size))
                return fail ("Pro Tools stopped answering.");

            return true;
        }

        static uint32_t readU32 (const juce::MemoryBlock& b, size_t offset)
        {
            auto* p = static_cast<const uint8_t*> (b.getData()) + offset;
            return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
        }

        /** Handles connection-level frames. Returns false on a fatal one. */
        bool handleControl (const Frame& f)
        {
            switch (f.type)
            {
                case settingsFrame:
                    if ((f.flags & ackFlag) == 0)
                    {
                        for (size_t i = 0; i + 6 <= f.payload.getSize(); i += 6)
                        {
                            auto* p = static_cast<const uint8_t*> (f.payload.getData()) + i;

                            if (((p[0] << 8) | p[1]) == 0x4)
                            {
                                const auto initial = (int64_t) readU32 (f.payload, i + 2);
                                streamWindow += initial - peerInitialWindow;
                                peerInitialWindow = initial;
                            }
                        }

                        return writeFrame (settingsFrame, ackFlag, 0, nullptr, 0);
                    }
                    return true;

                case pingFrame:
                    return (f.flags & ackFlag) != 0 || writeFrame (pingFrame, ackFlag, 0, f.payload.getData(), f.payload.getSize());

                case windowUpdateFrame:
                    if (f.payload.getSize() >= 4)
                        (f.stream == 0 ? connectionWindow : streamWindow) += readU32 (f.payload, 0) & 0x7fffffff;
                    return true;

                case goAwayFrame:
                    return fail ("Pro Tools closed the connection.");

                case rstStreamFrame:
                    return fail ("Pro Tools rejected the request (HTTP/2 error "
                                 + juce::String (f.payload.getSize() >= 4 ? (int) readU32 (f.payload, 0) : -1) + ").");

                default:
                    return true;
            }
        }

        bool sendBody (const juce::MemoryBlock& message)
        {
            size_t sent = 0;

            while (sent < message.getSize())
            {
                const auto window = (size_t) juce::jmax ((int64_t) 0, juce::jmin (connectionWindow, streamWindow));
                const auto chunk = juce::jmin (maxFrameSize, message.getSize() - sent, window);

                if (chunk == 0)
                {
                    Frame f; // blocked on flow control: wait for the peer to open its window

                    if (! readFrame (f) || ! handleControl (f))
                        return false;

                    continue;
                }

                const bool last = sent + chunk == message.getSize();

                if (! writeFrame (dataFrame, last ? endStream : 0, 1, static_cast<const char*> (message.getData()) + sent, chunk))
                    return fail ("Lost the connection to Pro Tools.");

                sent += chunk;
                connectionWindow -= (int64_t) chunk;
                streamWindow -= (int64_t) chunk;
            }

            return true;
        }

        bool readReply (juce::MemoryBlock& reply)
        {
            for (;;)
            {
                Frame f;

                if (! readFrame (f) || ! handleControl (f))
                    return false;

                if (f.stream != 1 || (f.type != dataFrame && f.type != headersFrame))
                    continue;

                if (f.type == dataFrame && f.payload.getSize() > 0)
                {
                    size_t offset = 0, size = f.payload.getSize();

                    if ((f.flags & paddedFlag) != 0)
                    {
                        const auto padding = (size_t) static_cast<const uint8_t*> (f.payload.getData())[0];
                        offset = 1;
                        size = padding + 1 <= size ? size - 1 - padding : 0;
                    }

                    reply.append (static_cast<const char*> (f.payload.getData()) + offset, size);
                }

                if ((f.flags & endStream) != 0)
                    return true;
            }
        }

        juce::StreamingSocket socket;
        int64_t connectionWindow = defaultWindow, streamWindow = defaultWindow, peerInitialWindow = defaultWindow;
    };

    juce::String describeErrors (const juce::String& json)
    {
        const auto parsed = juce::JSON::parse (json);
        juce::StringArray messages;

        auto collect = [&messages] (const juce::var& e)
        {
            const auto text = e.getProperty ("command_error_message", {}).toString();

            if (text.isNotEmpty())
                messages.add (text);
        };

        if (auto* list = parsed.getProperty ("errors", {}).getArray())
            for (auto& e : *list)
                collect (e);
        else
            collect (parsed);

        return messages.isEmpty() ? json : messages.joinIntoString ("; ");
    }
}

//==============================================================================
juce::var PtslClient::object (std::initializer_list<std::pair<const char*, juce::var>> properties)
{
    auto* o = new juce::DynamicObject();

    for (auto& [name, value] : properties)
        o->setProperty (name, value);

    return juce::var (o);
}

PtslClient::Result PtslClient::send (Command command, const juce::var& body)
{
    juce::MemoryOutputStream header;
    putField (header, 2, (uint64_t) command);
    putField (header, 3, (uint64_t) version);
    putField (header, 4, sessionId);
    putField (header, 5, (uint64_t) versionMinor);
    putField (header, 6, (uint64_t) versionRevision);

    juce::MemoryOutputStream request;
    putField (request, 1, header.getData(), header.getDataSize());
    putField (request, 2, body.isVoid() ? juce::String() : juce::JSON::toString (body, true));

    juce::MemoryOutputStream message; // gRPC framing: uncompressed flag + big-endian length
    message.writeByte (0);
    message.writeIntBigEndian ((int) request.getDataSize());
    message.write (request.getData(), request.getDataSize());

    Http2Call call;
    juce::MemoryBlock reply;

    if (! call.run (message.getMemoryBlock(), reply))
    {
        sessionId.clear(); // reconnect from scratch next time
        return { false, {}, call.error };
    }

    if (reply.getSize() < 5)
        return { false, {}, "Pro Tools sent an empty reply. Is scripting (PTSL) enabled?" };

    auto* bytes = static_cast<const uint8_t*> (reply.getData());
    ProtoReader response { bytes + 5, bytes + reply.getSize() };
    uint64_t status = 0;
    juce::String bodyJson, errorJson;
    int field;
    uint64_t value;
    juce::MemoryBlock data;

    while (response.next (field, value, data))
    {
        if (field == 1)
        {
            ProtoReader headerReader { static_cast<const uint8_t*> (data.getData()),
                                       static_cast<const uint8_t*> (data.getData()) + data.getSize() };
            int headerField;
            uint64_t headerValue;
            juce::MemoryBlock unused;

            while (headerReader.next (headerField, headerValue, unused))
                if (headerField == 3)
                    status = headerValue;
        }
        else if (field == 2)
            bodyJson = data.toString();
        else if (field == 3)
            errorJson = data.toString();
    }

    constexpr uint64_t completed = 3;

    if (status != completed || errorJson.isNotEmpty())
        return { false, {}, errorJson.isNotEmpty() ? describeErrors (errorJson)
                                                   : "Pro Tools didn't finish the command (status " + juce::String ((int) status) + ")." };

    return { true, bodyJson.isEmpty() ? juce::var (new juce::DynamicObject()) : juce::JSON::parse (bodyJson), {} };
}

bool PtslClient::connect (juce::String& error)
{
    // Speak the server's own protocol version. If it won't say, carry on unversioned and let
    // registration report the real problem.
    if (auto versionReply = send (getPtslVersion, {}); versionReply.ok)
    {
        version = (int) versionReply.body.getProperty ("version", 0);
        versionMinor = (int) versionReply.body.getProperty ("version_minor", 0);
        versionRevision = (int) versionReply.body.getProperty ("version_revision", 0);
    }

    auto registration = send (registerConnection, object ({ { "company_name", "Anti-Music" },
                                                            { "application_name", "Panner" } }));

    if (! registration.ok)
    {
        error = registration.error;
        return false;
    }

    sessionId = registration.body.getProperty ("session_id", {}).toString();

    if (sessionId.isEmpty())
    {
        error = "Pro Tools didn't give Panner a scripting session.";
        return false;
    }

    return true;
}

PtslClient::Result PtslClient::call (Command command, const juce::var& body)
{
    if (sessionId.isEmpty())
    {
        juce::String error;

        if (! connect (error))
            return { false, {}, error };
    }

    return send (command, body);
}
