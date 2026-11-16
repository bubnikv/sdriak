#pragma once
#include <stdint.h>
#include <cstring>
#include <gui/smgui.h>
#include <dsp/types.h>

// Largest baseband block, in samples, one packet may carry. This is a wire
// limit, deliberately not STREAM_BUFFER_SIZE: a desktop server sends blocks
// sized by its own constant (1000000), and a client built with a smaller one
// (32-bit Android) must still accept them, then split them to its streams.
#define SERVER_PROTOCOL_MAX_BLOCK 1000000
#define SERVER_MAX_PACKET_SIZE  (SERVER_PROTOCOL_MAX_BLOCK * sizeof(dsp::complex_t) * 2)

namespace server {
    // inline (not static): the helpers below odr-use these from a header.
    inline constexpr uint32_t SERVER_PROTOCOL_MAGIC = 0x41494453; // "SDIA": SDRIAK
    inline constexpr uint16_t SERVER_PROTOCOL_MAJOR = 1;
    inline constexpr uint16_t SERVER_PROTOCOL_MINOR = 0;
    inline constexpr uint32_t SERVER_PROTOCOL_CAP_HEARTBEAT = 1u << 0;
    inline constexpr uint32_t SERVER_PROTOCOL_CAP_AUTH = 1u << 1;
    inline constexpr int SERVER_PROTOCOL_FORK_ID_SIZE = 32;
    inline constexpr char SERVER_PROTOCOL_FORK_ID[] = "sdriak";
    inline constexpr int SERVER_AUTH_CHALLENGE_SIZE = 32;
    inline constexpr int SERVER_AUTH_RESPONSE_SIZE = 32;
    inline constexpr uint32_t SERVER_AUTH_PBKDF2_ITERATIONS = 20000;
    inline constexpr char SERVER_AUTH_SALT[] = "sdriak-server-auth-v1";

    enum PacketType {
        // Client to Server
        PACKET_TYPE_COMMAND,
        PACKET_TYPE_COMMAND_ACK,
        PACKET_TYPE_BASEBAND,
        PACKET_TYPE_BASEBAND_COMPRESSED,
        PACKET_TYPE_VFO,
        PACKET_TYPE_FFT,
        PACKET_TYPE_ERROR
    };

    enum Command {
        // Client to Server
        COMMAND_GET_UI = 0x00,
        COMMAND_UI_ACTION,
        COMMAND_START,
        COMMAND_STOP,
        COMMAND_SET_FREQUENCY,
        COMMAND_GET_SAMPLERATE,
        COMMAND_SET_SAMPLE_TYPE,
        COMMAND_SET_COMPRESSION,
        // Must be the first command sent by a client. Payload: HelloPayload.
        // Refuses non-SDRIAK forks unless they preserve our fork lineage ID.
        COMMAND_HELLO,
        // Payload: HMAC-SHA256(challenge) using the PBKDF2-derived auth key.
        COMMAND_AUTH_RESPONSE,

        // Server to client
        COMMAND_SET_SAMPLERATE = 0x80,
        COMMAND_DISCONNECT,
        // Payload: uint8 enabled, f64 minHz, f64 maxHz (native/hardware
        // domain; the client's SourceManager applies its own tuning offset).
        // Appended so prior command values stay stable; old clients ignore it.
        COMMAND_SET_TUNING_LIMITS,
        // Server liveness probe. Payload: u32 sequence. The client echoes the
        // same payload in COMMAND_ACK so the server can reclaim orphaned slots.
        COMMAND_HEARTBEAT,
        // Payload: SERVER_AUTH_CHALLENGE_SIZE random bytes.
        COMMAND_AUTH_CHALLENGE
    };

    enum Error {
        ERROR_NONE = 0x00,
        ERROR_INVALID_PACKET,
        ERROR_INVALID_COMMAND,
        ERROR_INVALID_ARGUMENT,
        ERROR_PROTOCOL_MISMATCH,
        ERROR_AUTH_REQUIRED,
        ERROR_AUTH_FAILED
    };
    
#pragma pack(push, 1)
    struct PacketHeader {
        uint32_t type;
        uint32_t size;
    };

    struct CommandHeader {
        uint32_t cmd;
    };

    struct HelloPayload {
        uint32_t magic;
        uint16_t protocolMajor;
        uint16_t protocolMinor;
        uint32_t capabilities;
        char forkId[SERVER_PROTOCOL_FORK_ID_SIZE];
    };
#pragma pack(pop)

    inline HelloPayload makeHelloPayload(uint32_t capabilities) {
        HelloPayload hello = {};
        hello.magic = SERVER_PROTOCOL_MAGIC;
        hello.protocolMajor = SERVER_PROTOCOL_MAJOR;
        hello.protocolMinor = SERVER_PROTOCOL_MINOR;
        hello.capabilities = capabilities;
        memcpy(hello.forkId, SERVER_PROTOCOL_FORK_ID, sizeof(SERVER_PROTOCOL_FORK_ID) - 1);
        return hello;
    }

    inline bool isCompatibleHello(const HelloPayload& hello) {
        const HelloPayload expected = makeHelloPayload(0);
        return hello.magic == SERVER_PROTOCOL_MAGIC &&
            hello.protocolMajor == SERVER_PROTOCOL_MAJOR &&
            memcmp(hello.forkId, expected.forkId, sizeof(hello.forkId)) == 0;
    }
}
