#include "WsSocket.h"

namespace cW {

WsSocket::WsSocket(Socket* socket)
    : Socket(socket), webSocket(new WebSocket(new HttpRequest(receivedData)))
{
}
bool WsSocket::shouldUpgrade() { return false; }

//[shouldCancel, complete]
std::pair<bool, bool> WsSocket::parseFrame()
{
    WsFrame* frame;
    if (!framePending) {
        if (receivedData.size() < 2) return {false, false};

        // creating new frame
        char* data = receivedData.data();
        frame      = (WsFrame*)malloc(sizeof(WsFrame));

        static_assert(sizeof(WsFrame::header) == 2, "Frame header size is larger than two bytes");

        memcpy_s(&(frame->header), 2, data, 2);

        if (!frame->header.mask) return {true, false};

        unsigned int headerLength = 2;
        if (frame->header.payloadLenShort == 126) {
            headerLength += 2;
            uint16_t length;
            memcpy_s(&length, 2, data + 2, 2);
            reverseByteOrder(&length);
            frame->payloadLength = (uint64_t)length;
        }
        else if (frame->header.payloadLenShort == 127) {
            headerLength += 8;
            uint64_t length;
            memcpy_s(&length, 8, data + 2, 8);
            reverseByteOrder(&length);
            frame->payloadLength = (uint64_t)length;
        }
        else
            frame->payloadLength = (uint64_t)frame->header.payloadLenShort;

        memcpy_s(&(frame->mask[0]), 4, data + headerLength, 4);

        currentFrame = frame;
        framePending = true;
        // removing header part
        receivedData = receivedData.substr(headerLength + 4);
    }
    else
        frame = currentFrame;

    auto payloadLength = frame->payloadLength;
    if (receivedData.size() >= payloadLength) {
        char* data = receivedData.data();
        unMask((uint8_t*)data, payloadLength, frame->mask);
        payloadBuffer.append(data, payloadLength);
        receivedData = receivedData.substr(payloadLength);
        framePending = false;
        return {false, currentFrame->header.fin == 1};
    }
    return {false, false};
}

// format frames and add them to the queue
void WsSocket::formatFrames(const char* payload, size_t payloadLength, WsOpcode opcode)
{
    WsFrameHeader header;
    size_t        framePayloadSize;
    bool          last;
    if (last = header.fin = (payloadLength <= opts.MaxPayloadLength))
        framePayloadSize = payloadLength;
    else
        framePayloadSize = opts.MaxPayloadLength;
    // server doesn't mask
    header.mask = false;
    // no meaning for these yet
    header.rsv1 = header.rsv2 = header.rsv3 = false;
    // opcode enum values are set accordingly
    header.opcode = (uint8_t)opcode;

    Frame* frame          = (Frame*)malloc(sizeof(Frame));
    frame->opcode         = opcode;
    size_t   headerLength = 2;
    void*    extendedLen;
    uint16_t extendedLenSize = 0;
    if (payloadLength <= 125) { header.payloadLenShort = (uint8_t)payloadLength; }
    else if (payloadLength <= UINT16_MAX) {
        header.payloadLenShort = (uint8_t)126;
        headerLength += 2;
        uint16_t* exlen = (uint16_t*)malloc(extendedLenSize = 2);
        *exlen          = (uint16_t)payloadLength;
        reverseByteOrder(exlen);
        extendedLen = exlen;
    }
    else {
        header.payloadLenShort = (uint8_t)127;
        headerLength += 8;
        uint64_t* exlen = (uint64_t*)malloc(extendedLenSize = 8);
        *exlen          = (uint64_t)payloadLength;
        reverseByteOrder(exlen);
        extendedLen = exlen;
    }
    frame->size = headerLength + framePayloadSize;
    frame->data = (char*)malloc(frame->size);

    memcpy_s(frame->data, frame->size, &header, 2);
    if (extendedLenSize) {
        memcpy_s(frame->data + 2, frame->size, extendedLen, extendedLenSize);
        free(extendedLen);
    }
    memcpy_s(frame->data + headerLength, frame->size, &header, framePayloadSize);

    queuedFrames.push(frame);
    if (!last)
        formatFrames(
            payload + framePayloadSize, payloadLength - framePayloadSize, WsOpcode::Continuation);
}

void WsSocket::cleanUp() { delete webSocket; }

WsSocket::~WsSocket()
{
    delete webSocket;
    freeWsFrame(currentFrame);
    while (!queuedFrames.empty()) {
        delete queuedFrames.front();
        queuedFrames.pop();
    }
}

// receives and writes one message at a time
void WsSocket::onWritable()
{
    while (!webSocket->queuedMessages.empty()) {
        auto&& message = webSocket->queuedMessages.front();
        formatFrames(message->payload, message->payloadSize, message->opcode);
        delete message;
        queuedFrames.pop();
    }
    if (!queuedFrames.empty()) {
        auto&& frame   = queuedFrames.front();
        int    toWrite = (int)(frame->size - writeOffset);
        writeOffset += write(
            frame->data + writeOffset, toWrite, queuedFrames.size() == 1 && (toWrite <= writeSize));
        assert(writeOffset <= frame->size && "How did write exceed frame size?");
        if (writeOffset == frame->size) {
            if (frame->opcode == WsOpcode::Close) close();
            freeFrame(frame);
            queuedFrames.pop();
            writeOffset = 0;
        }
    }
}
void WsSocket::onData()
{
    auto [shouldClose, complete] = parseFrame();
    if (shouldClose) return close();
    if (complete) {
        webSocket->currentMessage = new WsMessage((WsOpcode)currentFrame->header.opcode,
                                                  payloadBuffer.c_str(),
                                                  currentFrame->payloadLength);
        assert(currentFrame->header.opcode != 0 && "How is the opcode zero here?");
        switch (currentFrame->header.opcode) {
            case 1:
            case 2: server->dispatch(WsEvent::MESSAGE, webSocket); break;
            case 8:
                formatFrames(payloadBuffer.c_str(), payloadBuffer.length(), WsOpcode::Close);
                // actually close the socket after writing close frame
                break;
            case 9:
                formatFrames(payloadBuffer.c_str(), payloadBuffer.size(), WsOpcode::Pong);
                server->dispatch(WsEvent::PING, webSocket);
                break;
            case 10: server->dispatch(WsEvent::PONG, webSocket); break;
            default:
                std::cout << "Unsupported opcode! on socket " << id << " from ip " << ip
                          << std::endl;
                break;
        }
        payloadBuffer.clear();
        freeWsFrame(currentFrame);
        delete webSocket->currentMessage;
        currentFrame              = nullptr;
        webSocket->currentMessage = nullptr;
    }
}
}; // namespace cW