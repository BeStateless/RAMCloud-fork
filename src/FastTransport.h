/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * \file
 * Declaration of #RAMCloud::FastTransport.
 */

#ifndef RAMCLOUD_FASTTRANSPORT_H
#define RAMCLOUD_FASTTRANSPORT_H

#include <sys/socket.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <queue.h>

#include <vector>

#include "Common.h"
#include "Transport.h"
#include "Driver.h"
#include "Ring.h"
#include "Service.h"

#undef CURRENT_LOG_MODULE
#define CURRENT_LOG_MODULE TRANSPORT_MODULE

namespace RAMCloud {

class FastTransport : public Transport {
    class Session;
    class ServerSession;
    class ClientSession;
  public:
    explicit FastTransport(Driver* driver);
    void poll();
    virtual ClientSession* getClientSession();
    class ClientRPC : public Transport::ClientRPC {
      public:
        void getReply();

        Buffer* const requestBuffer;
        Buffer* const responseBuffer;

      private:
        ClientRPC(FastTransport* transport, Service* service,
                  Buffer* request, Buffer* response);
        void aborted();
        void completed();
        void start();

        enum {
            IDLE,
            IN_PROGRESS,
            COMPLETED,
            ABORTED,
        } state;

        FastTransport* const transport;
        Service* service;
        const sockaddr serverAddress;
        socklen_t serverAddressLen;
        TAILQ_ENTRY(ClientRPC) channelQueueEntries;
        friend class FastTransport;
        friend class FastTransportTest;
        DISALLOW_COPY_AND_ASSIGN(ClientRPC);
    };

    virtual ClientRPC* clientSend(Service* service,
                                  Buffer* request, Buffer* response);

    class ServerRPC : public Transport::ServerRPC {
      public:
        ServerRPC(ServerSession* session, uint8_t channelId);
        void sendReply();
      private:
        ServerSession* const session;
        const uint8_t channelId;
        TAILQ_ENTRY(ServerRPC) readyQueueEntries;
        friend class FastTransport;
        friend class FastTransportTest;
        DISALLOW_COPY_AND_ASSIGN(ServerRPC);
    };

    virtual ServerRPC* serverRecv();


  private:
    enum { NUM_CHANNELS_PER_SESSION = 8 };
    enum { MAX_NUM_CHANNELS_PER_SESSION = 8 };
    enum { PACKET_LOSS_PERCENTAGE = 0 };
    enum { MAX_STAGING_FRAGMENTS = 32 };
    enum { WINDOW_SIZE = 10 };
    enum { REQ_ACK_AFTER = 5 };
    // TODO(stutsman) 20-50 us?
    enum { TIMEOUT_NS = 10 * 1000 * 1000 }; // 10 ms
    enum { TIMEOUTS_UNTIL_ABORTING = 500 }; // >= 5 s
    enum { SESSION_TIMEOUT_NS = 60lu * 60 * 1000 * 1000 * 1000 }; // 30 min

    class Timer {
      public:
          Timer() : when(0), listEntries() {}
          virtual void fireTimer(uint64_t now) = 0;
          virtual ~Timer() {}
          uint64_t when;
          LIST_ENTRY(Timer) listEntries;
      private:
        DISALLOW_COPY_AND_ASSIGN(Timer);
    };

    class PayloadChunk : public Buffer::Chunk {
      public:
        static PayloadChunk* prependToBuffer(Buffer* buffer,
                                             char* data,
                                             uint32_t dataLength,
                                             Driver* driver,
                                             char* payload,
                                             uint32_t payloadLength);
        static PayloadChunk* appendToBuffer(Buffer* buffer,
                                            char* data,
                                            uint32_t dataLength,
                                            Driver* driver,
                                            char* payload,
                                            uint32_t payloadLength);
        ~PayloadChunk();
      private:
        PayloadChunk(void* data,
                     uint32_t dataLength,
                     Driver* driver,
                     char* const payload,
                     uint32_t payloadLength);
        Driver* const driver;
        char* const payload;
        const uint32_t payloadLength;
        DISALLOW_COPY_AND_ASSIGN(PayloadChunk);
    };

    struct Header {
        enum PayloadType {
            DATA         = 0,
            ACK          = 1,
            SESSION_OPEN = 2,
            RESERVED1    = 3,
            BAD_SESSION  = 4,
            RESERVED2    = 5,
            RESERVED3    = 6,
            RESERVED4    = 7
        };
        enum Direction {
            CLIENT_TO_SERVER = 0,
            SERVER_TO_CLIENT = 1
        };
        uint64_t sessionToken;
        uint32_t rpcId;
        uint32_t clientSessionHint;
        uint32_t serverSessionHint;
        uint16_t fragNumber;
        uint16_t totalFrags;
        uint8_t channelId;
        uint8_t direction:1;
        uint8_t requestAck:1;
        uint8_t pleaseDrop:1;
        uint8_t reserved1:1;
        uint8_t payloadType:4;
        PayloadType getPayloadType() {
            return static_cast<PayloadType>(payloadType);
        };
        Direction getDirection() {
            return static_cast<Direction>(direction);
        };
    } __attribute__((packed));

    struct SessionOpenResponse {
        uint8_t maxChannelId;
    } __attribute__((packed));

    struct AckResponse {
        AckResponse(uint16_t firstMissingFrag,
                    uint32_t stagingVector = 0)
            : firstMissingFrag(firstMissingFrag),
              stagingVector(stagingVector) {}
        uint16_t firstMissingFrag;
        uint32_t stagingVector;
    } __attribute__((packed));

    /**
     * InboundMessage accumulates and assembles fragments into a complete
     * incoming message.
     *
     * An incoming message must first be associated (permantently) with a
     * particular Session, Channel, and timer configuration using setup().
     * To start assembling a message init() is first called to tell the
     * message object how many fragments to expect and where to place the
     * resulting data.
     *
     * From here a Channel hands fragments still wrapped by the Driver as
     * a Driver::Received to processReceivedData which takes care of the
     * details.
     *
     * Once an instance is no longer is use clear() must be called to allow
     * future reuse of the instance.
     */
    class InboundMessage {
      public:
        InboundMessage();
        ~InboundMessage();
        void setup(Session* session, uint32_t channelId, bool useTimer);
        void sendAck();
        void clear();
        void init(uint16_t totalFrags, Buffer* dataBuffer);
        /**
         * \return
         *      Whether the full message has been received and added
         *      to the dataBuffer.
         */
        bool processReceivedData(Driver::Received* received);
      private:
        /// The transport to which this message belongs.  Set by setup().
        FastTransport* transport;

        /// The session to which this message belongs.  Set by setup().
        Session* session;

        /// The channel ID to which this message belongs.  Set by setup().
        uint32_t channelId;

        /// The number of fragments to aggregate before considering this
        /// message complete.  Set by init().
        uint32_t totalFrags;

        /// fragNumber of the earliest fragment that is still missing from
        /// the inbound message.
        uint32_t firstMissingFrag;

        /**
         * Structure to hold received fragments that can't be added to the
         * buffer yet because fragments preceding them are still missing.
         *
         * This structure holds both a pointer to the data and the length.
         */
        Ring<std::pair<char*, uint32_t>,
             MAX_STAGING_FRAGMENTS> dataStagingRing;

        /**
         * The place to accumulate the result message.  Valid once
         * processReceivedData returns true.
         */
        Buffer* dataBuffer;

	/**
	 * When invoked by the FastTransport timer code this timer will
	 * timeout the session if it is idle for too long, otherwise it
	 * will just transmit an ACK.
	 */
        class Timer : public FastTransport::Timer {
          public:
            Timer(bool useTimer, InboundMessage* const inboundMsg)
                : useTimer(useTimer), numTimeouts(0),
                  inboundMsg(inboundMsg)
            {
            }
            virtual void fireTimer(uint64_t now) {
                numTimeouts++;
                if (numTimeouts == TIMEOUTS_UNTIL_ABORTING) {
                    LOG(DEBUG, "Closing session due to timeout");
                    inboundMsg->session->close();
                } else {
                    //LOG(DEBUG, "Timer fired; resending ACK");
                    inboundMsg->transport->addTimer(this,
                                                    rdtsc() + TIMEOUT_NS);
                    inboundMsg->sendAck();
                }
            }
            bool useTimer;
            uint32_t numTimeouts;
          private:
            InboundMessage* const inboundMsg;
            DISALLOW_COPY_AND_ASSIGN(Timer);
        };
        /// Handles idle session cleanup and ACKs due to timeout.
        Timer timer;

        friend class FastTransportTest;
        friend class InboundMessageTest;
        DISALLOW_COPY_AND_ASSIGN(InboundMessage);
    };

    class OutboundMessage {
        static const uint64_t TO_SEND = ~(0lu);
        static const uint64_t ACKED = ~(0lu) - 1;
      public:
        OutboundMessage();
        void setup(Session* session, uint32_t channelId, bool useTimer);
        void clear();
        void beginSending(Buffer* dataBuffer);
        void send();
        bool processReceivedAck(Driver::Received* received);
      private:
        void sendOneData(uint32_t fragNumber, bool forceRequestAck = false);
        FastTransport* transport;
        Session* session;
        uint32_t channelId;
        Buffer* sendBuffer;

        /**
         * The number before which the receiving end has acknowledged receipt of
         * every fragment, in the range [0, totalFrags].
         */
        uint32_t firstMissingFrag;

        /// The total number of fragments in the message to send.
        uint32_t totalFrags;

        /**
         * The number of data packets sent on the wire since the last ACK
         * request. This is used to determine when to request the next ACK.
         */
         uint32_t packetsSinceAckReq;

        /**
         * A record of when unacknowledged fragments were sent, which is useful
         * for retransmission.
         * A Ring of MAX_STAGING_FRAGMENTS + 1 timestamps, where each entry
         * corresponds with the time the firstMissingFrag + i-th packet was sent
         * (0 if it has never been sent), or ACKED if it has already been
         * acknowledged by the receiving end.
         */
        Ring<uint64_t, MAX_STAGING_FRAGMENTS + 1> sentTimes;

        /**
         * The total number of fragments the receiving end has acknowledged, in
         * the range [0, totalFrags]. This is used for flow control, as the
         * sender guarantees to send only fragments whose numbers are below
         * numAcked + WINDOW_SIZE.
         */
        uint32_t numAcked;
        class Timer : public FastTransport::Timer {
          public:
            Timer(bool useTimer, OutboundMessage* const outboundMsg)
                : useTimer(useTimer), numTimeouts(0),
                  outboundMsg(outboundMsg)
            {
            }
            virtual void fireTimer(uint64_t now) {
                numTimeouts++;
                if (numTimeouts == TIMEOUTS_UNTIL_ABORTING) {
                    LOG(DEBUG, "Closing session due to timeout");
                    outboundMsg->session->close();
                } else {
                    //LOG(DEBUG, "Timer fired; resending");
                    outboundMsg->send();
                }
            }
            bool useTimer;
            uint32_t numTimeouts;
          private:
            OutboundMessage* const outboundMsg;
            DISALLOW_COPY_AND_ASSIGN(Timer);
        };
        Timer timer;

        friend class OutboundMessageTest;
        DISALLOW_COPY_AND_ASSIGN(OutboundMessage);
    };

    class Session {
      public:
        virtual void fillHeader(Header* const header,
                                uint8_t channelId) const = 0;
        virtual const sockaddr* getAddress(socklen_t *len) = 0;
        virtual uint64_t getLastActivityTime() = 0;
        virtual bool expire() = 0;
        virtual void close() = 0;
        virtual uint32_t getId() = 0;
        virtual ~Session() {}
        explicit Session(FastTransport* transport)
            : transport(transport) {}
        FastTransport* const transport;
      private:
        DISALLOW_COPY_AND_ASSIGN(Session);
    };

    class ServerSession : public Session {
        struct ServerChannel {
          public:
            /// This creates broken in/out messages that are reinitialized
            /// by setup()
            ServerChannel()
                : state(IDLE),
                  rpcId(~0U),
                  currentRpc(NULL),
                  inboundMsg(),
                  outboundMsg()
            {
            }
            void setup(Session* session, uint32_t channelId) {
                state = IDLE;
                rpcId = ~0U;
                currentRpc = NULL;
                inboundMsg.setup(session, channelId, false);
                outboundMsg.setup(session, channelId, false);
            }
            enum {
                IDLE,
                RECEIVING,
                PROCESSING,
                SENDING_WAITING,
            } state;
            uint32_t rpcId;
            ServerRPC* currentRpc;
            InboundMessage inboundMsg;
            OutboundMessage outboundMsg;
          private:
            DISALLOW_COPY_AND_ASSIGN(ServerChannel);
        };

        ServerChannel channels[NUM_CHANNELS_PER_SESSION];

        uint64_t token;
        uint32_t clientSessionHint;
        uint64_t lastActivityTime;
        sockaddr clientAddress;
        socklen_t clientAddressLen;
        const uint32_t id;

      public:
        virtual uint32_t getId() {
            return id;
        }
        uint32_t nextFree;
        ServerSession(FastTransport* transport, uint32_t sessionId);
        uint64_t getToken();
        const sockaddr* getAddress(socklen_t *len);
        uint64_t getLastActivityTime();
        void fillHeader(Header* const header, uint8_t channelId) const;
        void startSession(const sockaddr *clientAddress,
                          socklen_t clientAddressLen,
                          uint32_t clientSessionHint);
        void processInboundPacket(Driver::Received* received);
        void beginSending(uint8_t channelId);
        void close();
        bool expire();

      private:
        void processReceivedData(ServerChannel* channel,
                                 Driver::Received* received);
        void processReceivedAck(ServerChannel* channel,
                                Driver::Received* received);
        // TODO(stutsman) template friend doesn't work - no idea why
        template <typename T> friend class SessionTable;

        DISALLOW_COPY_AND_ASSIGN(ServerSession);
    };

    class ClientSession : public Session {
        struct ClientChannel {
          public:
            /// This creates broken in/out messages that are reinitialized
            /// by setup()
            ClientChannel()
                : state(IDLE),
                  rpcId(~0U),
                  currentRpc(NULL),
                  outboundMsg(),
                  inboundMsg()
            {
            }
            void setup(Session* session, uint32_t channelId) {
                state = IDLE;
                rpcId = ~0U;
                currentRpc = NULL;
                bool useTimer = true;
                outboundMsg.setup(session, channelId, useTimer);
                inboundMsg.setup(session, channelId, useTimer);
            }
            enum {
                IDLE,
                SENDING,
                RECEIVING,
            } state;
            uint32_t rpcId;
            ClientRPC* currentRpc;
            OutboundMessage outboundMsg;
            InboundMessage inboundMsg;
          private:
            DISALLOW_COPY_AND_ASSIGN(ClientChannel);
        };

        // TODO(ongaro): session open timer

        ClientChannel *channels;
        uint32_t numChannels;

        uint64_t token;
        uint32_t serverSessionHint;
        uint64_t lastActivityTime;
        sockaddr serverAddress;
        socklen_t serverAddressLen;

        TAILQ_HEAD(ChannelQueueHead, ClientRPC) channelQueue;

        const uint32_t id;

        void processSessionOpenResponse(Driver::Received* received);
        void allocateChannels();
        void processReceivedData(ClientChannel* channel,
                                 Driver::Received* received);
        void processReceivedAck(ClientChannel* channel,
                                Driver::Received* received);
        ClientChannel* getAvailableChannel();
        void clearChannels();
      public:
        uint32_t nextFree;
        virtual uint32_t getId() {
            return id;
        }
        ClientSession(FastTransport* transport, uint32_t sessionId);
        bool isConnected();
        void connect(const sockaddr* serverAddress,
                     socklen_t serverAddressLen);
        const sockaddr* getAddress(socklen_t *len);
        uint64_t getLastActivityTime();
        void fillHeader(Header* const header, uint8_t channelId) const;
        void processInboundPacket(Driver::Received* received);
        void startRpc(ClientRPC* rpc);
        void close();
        bool expire();

      private:
        template <typename T> friend class SessionTable;
        friend class FastTransportTest;
        friend class InboundMessageTest;
        friend class OutboundMessageTest;
        DISALLOW_COPY_AND_ASSIGN(ClientSession);
    };

    template <typename T>
    class SessionTable {
      public:
        /// A Session with this as nextFree is not itself free.
        static const uint32_t NONE = ~0u;
        /// A Session with this as nextFree is last Session in the free list.
        static const uint32_t TAIL = ~0u - 1;
        explicit SessionTable(FastTransport* transport)
            : transport(transport),
              sessions(),
              firstFree(TAIL),
              lastCleanedIndex(0)
        {
        }

        T* operator[](uint32_t sessionHint) const
        {
            return sessions[sessionHint];
        }

        T* get()
        {
            uint32_t sessionHint = firstFree;
            if (sessionHint >= sessions.size()) {
                // Invalid, no free sessions, so create a new one
                sessionHint = sessions.size();
                T* session = new T(transport, sessionHint);
                session->nextFree = TAIL;
                sessions.push_back(session);
            }
            T* session = sessions[sessionHint];
            firstFree = session->nextFree;
            session->nextFree = NONE;
            return sessions[sessionHint];
        }

        void put(T* session)
        {
            session->nextFree = firstFree;
            firstFree = session->getId();
        }

        void expire(uint32_t sessionsToCheck = 5)
        {
            uint64_t now = rdtsc();
            for (uint32_t i = 0; i < sessionsToCheck; i++) {
                lastCleanedIndex++;
                if (lastCleanedIndex >= sessions.size()) {
                    lastCleanedIndex = 0;
                    if (sessions.size() == 0)
                        break;
                }
                T* session = sessions[lastCleanedIndex];
                if (session->nextFree == NONE &&
                    (session->getLastActivityTime() +
                     SESSION_TIMEOUT_NS < now)) {
                    if (session->expire())
                        put(session);
                }
            }
        }

        uint32_t size()
        {
            return sessions.size();
        }

      private:
        FastTransport* const transport;
        std::vector<T*> sessions;
        uint32_t firstFree;
        uint32_t lastCleanedIndex;

        friend class SessionTableTest;
        DISALLOW_COPY_AND_ASSIGN(SessionTable);
    };

    bool tryProcessPacket();
    void addTimer(Timer* timer, uint64_t when);
    void removeTimer(Timer* timer);
    void fireTimers();
    void sendPacket(const sockaddr* address, socklen_t addressLength,
                    Header* header, Buffer::Iterator* payload);
    uint32_t dataPerFragment();
    uint32_t numFrags(const Buffer* dataBuffer);

    Driver* const driver;
    SessionTable<ServerSession> serverSessions;
    SessionTable<ClientSession> clientSessions;
    TAILQ_HEAD(ServerReadyQueueHead, ServerRPC) serverReadyQueue;
    LIST_HEAD(TimerListHead, Timer) timerList;

    friend class FastTransportTest;
    friend class SessionTableTest;
    friend class InboundMessageTest;
    friend class OutboundMessageTest;
    friend class MockReceived;
    friend class Services;
    DISALLOW_COPY_AND_ASSIGN(FastTransport);
};

}  // namespace RAMCloud

#endif
