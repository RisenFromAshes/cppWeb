#include "WebSocketSession.h"
#include "ClientSocket.h"
#include "Server.h"
namespace cW {

WebSocketSession::WebSocketSession(ClientSocket* socket) : Session(socket, Session::WS)
{
    // TODO:Implemet
    // size_t headerEnd = socket->receiveBuffer.find("\r\n\r\n");
    // requestHeader    = socket->receiveBuffer.substr(headerEnd + 2);
    // webSocket        = new WebSocket(new HttpRequest(requestHeader));
}

//[shouldCancel, complete]
std::pair<bool, bool> WebSocketSession::parseFrame(const std::string_view& recvBuf)
{
    WsFrame*     frame;
    bool         first;
    unsigned int headerLength = 0;
    const char*  data         = recvBuf.data();
    size_t       bufLen       = recvBuf.size();
    if (first = !currentFrame) {
        if (recvBuf.size() < 2) return {false, false};

        // creating new frame
        frame             = (WsFrame*)malloc(sizeof(WsFrame));
        frame->readOffset = 0;

        static_assert(sizeof(WsFrame::header) == 2, "Frame header size is larger than two bytes");

        std::memcpy(&(frame->header), data, 2);

        if (!frame->header.mask) return {true, false};

        headerLength = 2;
        if (frame->header.payloadLenShort == 126) {
            headerLength += 2;
            uint16_t length;
            std::memcpy(&length, data + 2, 2);
            reverseByteOrder(&length);
            frame->payloadLength = (uint64_t)length;
        }
        else if (frame->header.payloadLenShort == 127) {
            headerLength += 8;
            uint64_t length;
            std::memcpy(&length, data + 2, 8);
            reverseByteOrder(&length);
            frame->payloadLength = (uint64_t)length;
        }
        else
            frame->payloadLength = (uint64_t)frame->header.payloadLenShort;

        std::memcpy(&(frame->mask[0]), data + headerLength, 4);
        // including mask
        headerLength += 4;
        currentFrame = frame;
        framePending = frame->header.fin != 1;
        payloadBuffer.reserve(frame->payloadLength);
    }
    else
        frame = currentFrame;

    if (recvBuf.size() > headerLength) {
        size_t toAdd = std::min(bufLen - headerLength, frame->payloadLength);
        payloadBuffer.append(data + headerLength, toAdd);
        frame->readOffset += toAdd;
    }
    if (frame->readOffset >= frame->payloadLength) {
        unMask((uint8_t*)(char*)payloadBuffer.data(), frame->payloadLength, frame->mask);
        return {false, true};
    }
    return {false, false};
}

// format frames and add them to the queue
void WebSocketSession::formatFrames(const char* payload, size_t payloadLength, WsOpcode opcode)
{
    socket->wantWrite = true;
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

    std::memcpy(frame->data, &header, 2);
    if (extendedLenSize) {
        std::memcpy(frame->data + 2, extendedLen, extendedLenSize);
        free(extendedLen);
    }
    std::memcpy(frame->data + headerLength, &header, framePayloadSize);

    queuedFrames.push(frame);
    if (!last)
        formatFrames(
            payload + framePayloadSize, payloadLength - framePayloadSize, WsOpcode::Continuation);
}

void WebSocketSession::cleanUp() { delete webSocket; }

WebSocketSession::~WebSocketSession()
{
    delete webSocket;
    freeWsFrame(currentFrame);
    while (!queuedFrames.empty()) {
        delete queuedFrames.front();
        queuedFrames.pop();
    }
}

void WebSocketSession::readyFrames()
{
    while (!webSocket->queuedMessages.empty()) {
        auto&& message = webSocket->queuedMessages.front();
        formatFrames(message->payload, message->payloadSize, message->opcode);
        delete message;
        webSocket->queuedMessages.pop();
    }
}

void WebSocketSession::onAwakePre() { readyFrames(); }
void WebSocketSession::onAwakePost() { readyFrames(); }

// receives and writes one message at a time
void WebSocketSession::onWritable()
{
    if (!queuedFrames.empty()) {
        auto&& frame   = queuedFrames.front();
        int    toWrite = (int)(frame->size - writeOffset);
        bool   final   = queuedFrames.size() == 1;
        writeOffset += socket->write(frame->data + writeOffset, toWrite, final);
        assert(writeOffset <= frame->size && "How did write exceed frame size?");
        if (writeOffset == frame->size) {
            if (frame->opcode == WsOpcode::Close) socket->connected = false;
            freeFrame(frame);
            queuedFrames.pop();
            writeOffset = 0;
        }
    }
}
void WebSocketSession::onData(const std::string_view& data)
{
    auto [shouldClose, complete] = parseFrame(data);
    if (shouldClose)
        socket->disconnect();
    else if (complete) {
        webSocket->currentMessage = new WsMessage((WsOpcode)currentFrame->header.opcode,
                                                  payloadBuffer.c_str(),
                                                  currentFrame->payloadLength);
        assert(currentFrame->header.opcode != 0 && "How is the opcode zero here?");
        switch (currentFrame->header.opcode) {
            case 1:
            case 2: socket->server->dispatch(WsEvent::MESSAGE, webSocket); break;
            case 8:
                // actually close the socket after writing close frame
                formatFrames(payloadBuffer.c_str(), payloadBuffer.length(), WsOpcode::Close);
                socket->server->dispatch(WsEvent::CLOSE, webSocket);
                break;
            case 9:
                formatFrames(payloadBuffer.c_str(), payloadBuffer.size(), WsOpcode::Pong);
                socket->server->dispatch(WsEvent::PING, webSocket);
                break;
            case 10: socket->server->dispatch(WsEvent::PONG, webSocket); break;
            default:
                std::cout << "Unsupported opcode! on socket " << socket->id << " from ip "
                          << socket->ip << std::endl;
                break;
        }
        payloadBuffer.clear();
        freeWsFrame(currentFrame);
        delete webSocket->currentMessage;
        currentFrame              = nullptr;
        webSocket->currentMessage = nullptr;
    }
}

bool WebSocketSession::shouldEnd() { return false; }
void WebSocketSession::onAborted()
{
    // unexpected shutdown
    // TODO: add an abort event handler
    // server->dispatch(WsEvent::CLOSE, )
}

}; // namespace cW