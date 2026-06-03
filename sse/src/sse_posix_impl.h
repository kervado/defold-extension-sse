#ifndef EXTENSION_SSE_POSIX_IMPL_H
#define EXTENSION_SSE_POSIX_IMPL_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct SSEDesktopConnection
{
    int32_t m_Handle;
    char* m_Url;
    char* m_LastEventId;
    dmArray<SSEHeader> m_Headers;
    SSEParser m_Parser;
    dmThread::Thread m_Thread;
    dmMutex::HMutex m_Mutex;
    int m_Socket;
    int32_t m_RetryMS;
    int32_t m_Status;
    uint8_t m_Reconnect;
    uint8_t m_Stop;
    uint8_t m_Opened;

    SSEDesktopConnection()
    : m_Handle(0)
    , m_Url(0)
    , m_LastEventId(0)
    , m_Thread(0)
    , m_Mutex(0)
    , m_Socket(-1)
    , m_RetryMS(3000)
    , m_Status(0)
    , m_Reconnect(0)
    , m_Stop(0)
    , m_Opened(0)
    {
    }
};

struct SSEPOSIXUrl
{
    std::string m_Host;
    std::string m_Port;
    std::string m_Path;
    std::string m_HostHeader;
};

struct SSEPOSIXChunkDecoder
{
    enum State
    {
        STATE_SIZE,
        STATE_DATA,
        STATE_DATA_CRLF,
        STATE_DATA_LF,
        STATE_DONE
    };

    std::string m_Line;
    size_t m_Remaining;
    State m_State;

    SSEPOSIXChunkDecoder()
    : m_Remaining(0)
    , m_State(STATE_SIZE)
    {
    }
};

static char* SSEDesktop_StrDup(const char* value)
{
    if (!value)
    {
        return 0;
    }
    const size_t length = strlen(value);
    char* copy = (char*)malloc(length + 1);
    if (copy)
    {
        memcpy(copy, value, length + 1);
    }
    return copy;
}

static void SSEDesktop_SetString(char** target, const char* value)
{
    free(*target);
    *target = SSEDesktop_StrDup(value ? value : "");
}

static bool SSEDesktop_ShouldStop(SSEDesktopConnection* connection)
{
    DM_MUTEX_SCOPED_LOCK(connection->m_Mutex);
    return connection->m_Stop != 0;
}

static void SSEPOSIX_SetSocket(SSEDesktopConnection* connection, int socket_fd)
{
    DM_MUTEX_SCOPED_LOCK(connection->m_Mutex);
    connection->m_Socket = socket_fd;
}

static int SSEPOSIX_TakeSocket(SSEDesktopConnection* connection)
{
    DM_MUTEX_SCOPED_LOCK(connection->m_Mutex);
    const int socket_fd = connection->m_Socket;
    connection->m_Socket = -1;
    return socket_fd;
}

static int SSEPOSIX_GetSocket(SSEDesktopConnection* connection)
{
    DM_MUTEX_SCOPED_LOCK(connection->m_Mutex);
    return connection->m_Socket;
}

static void SSEPOSIX_CloseSocketFd(int socket_fd)
{
    if (socket_fd >= 0)
    {
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
    }
}

static void SSEPOSIX_CloseActiveSocket(SSEDesktopConnection* connection)
{
    const int socket_fd = SSEPOSIX_TakeSocket(connection);
    SSEPOSIX_CloseSocketFd(socket_fd);
}

static bool SSEPOSIX_IsDefaultPort(const std::string& port)
{
    return port == "80";
}

static bool SSEPOSIX_HasControlChars(const char* value)
{
    if (!value)
    {
        return false;
    }

    for (const char* cursor = value; *cursor; ++cursor)
    {
        if (*cursor == '\r' || *cursor == '\n')
        {
            return true;
        }
    }

    return false;
}

static std::string SSEPOSIX_LowerASCII(const std::string& value)
{
    std::string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i)
    {
        result.push_back((char)tolower((unsigned char)value[i]));
    }

    return result;
}

static bool SSEPOSIX_ParseUrl(const char* url, SSEPOSIXUrl* output, char* error, uint32_t error_size)
{
    if (!url || !url[0])
    {
        dmSnPrintf(error, error_size, "SSE URL is empty");
        return false;
    }

    std::string value(url);
    const size_t scheme_end = value.find("://");
    if (scheme_end == std::string::npos)
    {
        dmSnPrintf(error, error_size, "SSE URL must include http://");
        return false;
    }

    const std::string scheme = SSEPOSIX_LowerASCII(value.substr(0, scheme_end));
    if (scheme != "http")
    {
        dmSnPrintf(error, error_size, "Linux SSE backend currently supports http:// URLs only");
        return false;
    }

    const size_t authority_start = scheme_end + 3;
    const size_t path_start = value.find_first_of("/?#", authority_start);
    std::string authority;
    if (path_start == std::string::npos)
    {
        authority = value.substr(authority_start);
        output->m_Path = "/";
    }
    else
    {
        authority = value.substr(authority_start, path_start - authority_start);
        output->m_Path = value[path_start] == '/' ? value.substr(path_start) : "/" + value.substr(path_start);
    }

    const size_t fragment = output->m_Path.find('#');
    if (fragment != std::string::npos)
    {
        output->m_Path.resize(fragment);
    }
    if (output->m_Path.empty())
    {
        output->m_Path = "/";
    }

    if (authority.empty() || authority.find('@') != std::string::npos)
    {
        dmSnPrintf(error, error_size, "SSE URL host is invalid");
        return false;
    }

    output->m_Port = "80";
    if (authority[0] == '[')
    {
        const size_t end = authority.find(']');
        if (end == std::string::npos || end == 1)
        {
            dmSnPrintf(error, error_size, "SSE URL IPv6 host is invalid");
            return false;
        }

        output->m_Host = authority.substr(1, end - 1);
        if (end + 1 < authority.size())
        {
            if (authority[end + 1] != ':')
            {
                dmSnPrintf(error, error_size, "SSE URL host is invalid");
                return false;
            }
            output->m_Port = authority.substr(end + 2);
        }
    }
    else
    {
        const size_t first_colon = authority.find(':');
        const size_t last_colon = authority.rfind(':');
        if (first_colon != std::string::npos && first_colon == last_colon)
        {
            output->m_Host = authority.substr(0, first_colon);
            output->m_Port = authority.substr(first_colon + 1);
        }
        else
        {
            output->m_Host = authority;
        }
    }

    if (output->m_Host.empty() || output->m_Port.empty())
    {
        dmSnPrintf(error, error_size, "SSE URL host or port is empty");
        return false;
    }

    for (size_t i = 0; i < output->m_Port.size(); ++i)
    {
        if (!isdigit((unsigned char)output->m_Port[i]))
        {
            dmSnPrintf(error, error_size, "SSE URL port is invalid");
            return false;
        }
    }

    const bool is_ipv6 = output->m_Host.find(':') != std::string::npos;
    output->m_HostHeader = is_ipv6 ? "[" + output->m_Host + "]" : output->m_Host;
    if (!SSEPOSIX_IsDefaultPort(output->m_Port))
    {
        output->m_HostHeader += ":";
        output->m_HostHeader += output->m_Port;
    }

    return true;
}

static int SSEPOSIX_WaitSocket(SSEDesktopConnection* connection, int socket_fd, short events, int timeout_ms, short* revents)
{
    int elapsed = 0;
    if (revents)
    {
        *revents = 0;
    }

    while (!SSEDesktop_ShouldStop(connection))
    {
        int step = 100;
        if (timeout_ms >= 0)
        {
            const int remaining = timeout_ms - elapsed;
            if (remaining <= 0)
            {
                return 0;
            }
            step = remaining < step ? remaining : step;
        }

        struct pollfd poll_fd;
        memset(&poll_fd, 0, sizeof(poll_fd));
        poll_fd.fd = socket_fd;
        poll_fd.events = events;

        const int result = poll(&poll_fd, 1, step);
        if (result > 0)
        {
            if (revents)
            {
                *revents = poll_fd.revents;
            }
            return 1;
        }
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }

        if (timeout_ms >= 0)
        {
            elapsed += step;
        }
    }

    return -2;
}

static bool SSEPOSIX_SetNonBlocking(int socket_fd, char* error, uint32_t error_size)
{
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        dmSnPrintf(error, error_size, "fcntl(O_NONBLOCK) failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static void SSEPOSIX_SetKeepAlive(int socket_fd)
{
    int value = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
#ifdef TCP_NODELAY
    setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif
}

static bool SSEPOSIX_ConnectSocket(SSEDesktopConnection* connection, const SSEPOSIXUrl* url, char* error, uint32_t error_size)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo* addresses = 0;
    const int address_result = getaddrinfo(url->m_Host.c_str(), url->m_Port.c_str(), &hints, &addresses);
    if (address_result != 0)
    {
        dmSnPrintf(error, error_size, "getaddrinfo failed: %s", gai_strerror(address_result));
        return false;
    }

    char last_error[256];
    last_error[0] = 0;

    for (struct addrinfo* address = addresses; address; address = address->ai_next)
    {
        if (SSEDesktop_ShouldStop(connection))
        {
            freeaddrinfo(addresses);
            return true;
        }

        const int socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0)
        {
            dmSnPrintf(last_error, sizeof(last_error), "socket failed: %s", strerror(errno));
            continue;
        }

        SSEPOSIX_SetSocket(connection, socket_fd);
        if (!SSEPOSIX_SetNonBlocking(socket_fd, last_error, sizeof(last_error)))
        {
            SSEPOSIX_CloseActiveSocket(connection);
            continue;
        }

        SSEPOSIX_SetKeepAlive(socket_fd);

        int connect_result = connect(socket_fd, address->ai_addr, address->ai_addrlen);
        if (connect_result == 0)
        {
            freeaddrinfo(addresses);
            return true;
        }

        if (errno == EINPROGRESS)
        {
            short revents = 0;
            const int wait_result = SSEPOSIX_WaitSocket(connection, socket_fd, POLLOUT, 15000, &revents);
            if (wait_result == -2)
            {
                freeaddrinfo(addresses);
                return true;
            }
            if (wait_result == 0)
            {
                dmSnPrintf(last_error, sizeof(last_error), "connect timed out");
                SSEPOSIX_CloseActiveSocket(connection);
                continue;
            }
            if (wait_result < 0)
            {
                dmSnPrintf(last_error, sizeof(last_error), "poll connect failed: %s", strerror(errno));
                SSEPOSIX_CloseActiveSocket(connection);
                continue;
            }

            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 && socket_error == 0)
            {
                freeaddrinfo(addresses);
                return true;
            }

            dmSnPrintf(last_error, sizeof(last_error), "connect failed: %s", strerror(socket_error ? socket_error : errno));
            SSEPOSIX_CloseActiveSocket(connection);
            continue;
        }

        dmSnPrintf(last_error, sizeof(last_error), "connect failed: %s", strerror(errno));
        SSEPOSIX_CloseActiveSocket(connection);
    }

    freeaddrinfo(addresses);
    dmSnPrintf(error, error_size, "%s", last_error[0] ? last_error : "connect failed");
    return false;
}

static bool SSEPOSIX_AppendHeader(std::string* request, const char* name, const char* value, char* error, uint32_t error_size)
{
    if (!name || !value)
    {
        return true;
    }
    if (!name[0] || SSEPOSIX_HasControlChars(name) || SSEPOSIX_HasControlChars(value) || strchr(name, ':') != 0)
    {
        dmSnPrintf(error, error_size, "invalid SSE request header");
        return false;
    }

    request->append(name);
    request->append(": ");
    request->append(value);
    request->append("\r\n");
    return true;
}

static bool SSEPOSIX_BuildRequest(SSEDesktopConnection* connection, const SSEPOSIXUrl* url, std::string* request, char* error, uint32_t error_size)
{
    request->clear();
    request->append("GET ");
    request->append(url->m_Path);
    request->append(" HTTP/1.1\r\n");
    request->append("Host: ");
    request->append(url->m_HostHeader);
    request->append("\r\n");
    request->append("Accept: text/event-stream\r\n");
    request->append("Cache-Control: no-cache\r\n");
    request->append("Connection: keep-alive\r\n");
    request->append("User-Agent: defold-extension-sse/0.2\r\n");

    for (uint32_t i = 0; i < connection->m_Headers.Size(); ++i)
    {
        if (!SSEPOSIX_AppendHeader(request, connection->m_Headers[i].m_Name, connection->m_Headers[i].m_Value, error, error_size))
        {
            return false;
        }
    }

    if (connection->m_LastEventId && connection->m_LastEventId[0])
    {
        if (!SSEPOSIX_AppendHeader(request, "Last-Event-ID", connection->m_LastEventId, error, error_size))
        {
            return false;
        }
    }

    request->append("\r\n");
    return true;
}

static bool SSEPOSIX_SendAll(SSEDesktopConnection* connection, int socket_fd, const std::string& request, char* error, uint32_t error_size)
{
    const char* data = request.data();
    size_t remaining = request.size();

    while (remaining > 0 && !SSEDesktop_ShouldStop(connection))
    {
        const ssize_t sent = send(socket_fd, data, remaining, MSG_NOSIGNAL);
        if (sent > 0)
        {
            data += sent;
            remaining -= (size_t)sent;
            continue;
        }

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            short revents = 0;
            const int wait_result = SSEPOSIX_WaitSocket(connection, socket_fd, POLLOUT, 15000, &revents);
            if (wait_result == -2)
            {
                return true;
            }
            if (wait_result == 0)
            {
                dmSnPrintf(error, error_size, "timed out while sending SSE request");
                return false;
            }
            if (wait_result < 0)
            {
                dmSnPrintf(error, error_size, "poll send failed: %s", strerror(errno));
                return false;
            }
            continue;
        }

        dmSnPrintf(error, error_size, "send failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static void SSEDesktop_OnParserEvent(void* context, const SSEParsedEvent* event)
{
    SSEDesktopConnection* connection = (SSEDesktopConnection*)context;
    if (event->m_Id && event->m_Id[0])
    {
        SSEDesktop_SetString(&connection->m_LastEventId, event->m_Id);
        SSE_SetLastEventId(connection->m_Handle, event->m_Id);
    }
    SSE_EnqueueMessage(connection->m_Handle, event->m_Event, event->m_Data, event->m_Id);
}

static void SSEDesktop_OnParserRetry(void* context, int retry_ms)
{
    SSEDesktopConnection* connection = (SSEDesktopConnection*)context;
    connection->m_RetryMS = retry_ms;
}

static void SSEDesktop_OnParserId(void* context, const char* id)
{
    SSEDesktopConnection* connection = (SSEDesktopConnection*)context;
    SSEDesktop_SetString(&connection->m_LastEventId, id);
    SSE_SetLastEventId(connection->m_Handle, id);
}

static bool SSEPOSIX_ParseChunkSize(const std::string& line, size_t* size)
{
    size_t value = 0;
    bool has_digit = false;

    for (size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (c == ';')
        {
            break;
        }
        if (c == ' ' || c == '\t')
        {
            continue;
        }

        int digit = -1;
        if (c >= '0' && c <= '9')
        {
            digit = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            digit = c - 'A' + 10;
        }
        else
        {
            return false;
        }

        if (value > (SIZE_MAX - (size_t)digit) / 16)
        {
            return false;
        }
        value = value * 16 + (size_t)digit;
        has_digit = true;
    }

    if (!has_digit)
    {
        return false;
    }

    *size = value;
    return true;
}

static bool SSEPOSIX_FeedChunkedBody(SSEDesktopConnection* connection, SSEPOSIXChunkDecoder* decoder, const char* data, size_t size, const SSEParserCallbacks* callbacks, char* error, uint32_t error_size)
{
    size_t offset = 0;
    while (offset < size)
    {
        if (decoder->m_State == SSEPOSIXChunkDecoder::STATE_DONE)
        {
            return true;
        }

        if (decoder->m_State == SSEPOSIXChunkDecoder::STATE_SIZE)
        {
            const char c = data[offset++];
            decoder->m_Line.push_back(c);
            if (decoder->m_Line.size() > 1024)
            {
                dmSnPrintf(error, error_size, "chunk size line is too long");
                return false;
            }

            if (c == '\n')
            {
                while (!decoder->m_Line.empty() && (decoder->m_Line[decoder->m_Line.size() - 1] == '\n' || decoder->m_Line[decoder->m_Line.size() - 1] == '\r'))
                {
                    decoder->m_Line.resize(decoder->m_Line.size() - 1);
                }

                size_t chunk_size = 0;
                if (!SSEPOSIX_ParseChunkSize(decoder->m_Line, &chunk_size))
                {
                    dmSnPrintf(error, error_size, "invalid chunk size");
                    return false;
                }

                decoder->m_Line.clear();
                decoder->m_Remaining = chunk_size;
                decoder->m_State = chunk_size == 0 ? SSEPOSIXChunkDecoder::STATE_DONE : SSEPOSIXChunkDecoder::STATE_DATA;
            }
            continue;
        }

        if (decoder->m_State == SSEPOSIXChunkDecoder::STATE_DATA)
        {
            const size_t available = size - offset;
            const size_t chunk = decoder->m_Remaining < available ? decoder->m_Remaining : available;
            if (chunk > 0)
            {
                connection->m_Parser.Feed(data + offset, chunk, callbacks, connection);
                decoder->m_Remaining -= chunk;
                offset += chunk;
            }

            if (decoder->m_Remaining == 0)
            {
                decoder->m_State = SSEPOSIXChunkDecoder::STATE_DATA_CRLF;
            }
            continue;
        }

        if (decoder->m_State == SSEPOSIXChunkDecoder::STATE_DATA_CRLF)
        {
            const char c = data[offset++];
            if (c == '\r')
            {
                decoder->m_State = SSEPOSIXChunkDecoder::STATE_DATA_LF;
            }
            else if (c == '\n')
            {
                decoder->m_State = SSEPOSIXChunkDecoder::STATE_SIZE;
            }
            else
            {
                dmSnPrintf(error, error_size, "invalid chunk delimiter");
                return false;
            }
            continue;
        }

        if (decoder->m_State == SSEPOSIXChunkDecoder::STATE_DATA_LF)
        {
            const char c = data[offset++];
            if (c != '\n')
            {
                dmSnPrintf(error, error_size, "invalid chunk delimiter");
                return false;
            }
            decoder->m_State = SSEPOSIXChunkDecoder::STATE_SIZE;
        }
    }

    return true;
}

static size_t SSEPOSIX_FindHeaderEnd(const std::string& response, size_t* delimiter_size)
{
    size_t end = response.find("\r\n\r\n");
    if (end != std::string::npos)
    {
        *delimiter_size = 4;
        return end;
    }

    end = response.find("\n\n");
    if (end != std::string::npos)
    {
        *delimiter_size = 2;
        return end;
    }

    return std::string::npos;
}

static std::string SSEPOSIX_TrimTrailingCR(std::string line)
{
    if (!line.empty() && line[line.size() - 1] == '\r')
    {
        line.resize(line.size() - 1);
    }
    return line;
}

static bool SSEPOSIX_ParseResponseHeaders(const std::string& headers, int32_t* status, bool* chunked, char* error, uint32_t error_size)
{
    *status = 0;
    *chunked = false;

    const size_t first_line_end = headers.find('\n');
    const std::string first_line = SSEPOSIX_TrimTrailingCR(headers.substr(0, first_line_end));
    int parsed_status = 0;
    if (sscanf(first_line.c_str(), "HTTP/%*s %d", &parsed_status) != 1)
    {
        dmSnPrintf(error, error_size, "invalid HTTP response");
        return false;
    }
    *status = parsed_status;

    size_t line_start = first_line_end == std::string::npos ? headers.size() : first_line_end + 1;
    while (line_start < headers.size())
    {
        const size_t line_end = headers.find('\n', line_start);
        const size_t count = line_end == std::string::npos ? headers.size() - line_start : line_end - line_start;
        const std::string line = SSEPOSIX_TrimTrailingCR(headers.substr(line_start, count));
        const std::string lower = SSEPOSIX_LowerASCII(line);

        if (lower.find("transfer-encoding:") == 0 && lower.find("chunked") != std::string::npos)
        {
            *chunked = true;
        }

        if (line_end == std::string::npos)
        {
            break;
        }
        line_start = line_end + 1;
    }

    return true;
}

static bool SSEPOSIX_FeedBody(SSEDesktopConnection* connection, bool chunked, SSEPOSIXChunkDecoder* chunk_decoder, const char* data, size_t size, const SSEParserCallbacks* callbacks, char* error, uint32_t error_size)
{
    if (size == 0)
    {
        return true;
    }

    if (chunked)
    {
        return SSEPOSIX_FeedChunkedBody(connection, chunk_decoder, data, size, callbacks, error, error_size);
    }

    connection->m_Parser.Feed(data, size, callbacks, connection);
    return true;
}

static bool SSEPOSIX_ReadResponse(SSEDesktopConnection* connection, int socket_fd, char* error, uint32_t error_size)
{
    char buffer[8192];
    std::string header_buffer;
    bool headers_done = false;
    bool chunked = false;
    SSEPOSIXChunkDecoder chunk_decoder;

    SSEParserCallbacks callbacks;
    callbacks.m_OnEvent = SSEDesktop_OnParserEvent;
    callbacks.m_OnRetry = SSEDesktop_OnParserRetry;
    callbacks.m_OnId = SSEDesktop_OnParserId;

    while (!SSEDesktop_ShouldStop(connection))
    {
        short revents = 0;
        const int wait_result = SSEPOSIX_WaitSocket(connection, socket_fd, POLLIN, -1, &revents);
        if (wait_result == -2)
        {
            return true;
        }
        if (wait_result < 0)
        {
            dmSnPrintf(error, error_size, "poll read failed: %s", strerror(errno));
            return false;
        }

        if ((revents & (POLLERR | POLLNVAL)) && !(revents & POLLIN))
        {
            dmSnPrintf(error, error_size, "socket read failed");
            return false;
        }

        const ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (received > 0)
        {
            if (!headers_done)
            {
                header_buffer.append(buffer, (size_t)received);
                if (header_buffer.size() > 65536)
                {
                    dmSnPrintf(error, error_size, "HTTP response headers are too large");
                    return false;
                }

                size_t delimiter_size = 0;
                const size_t header_end = SSEPOSIX_FindHeaderEnd(header_buffer, &delimiter_size);
                if (header_end == std::string::npos)
                {
                    continue;
                }

                if (!SSEPOSIX_ParseResponseHeaders(header_buffer.substr(0, header_end), &connection->m_Status, &chunked, error, error_size))
                {
                    return false;
                }

                if (connection->m_Status < 200 || connection->m_Status >= 300)
                {
                    dmSnPrintf(error, error_size, "SSE request failed with HTTP status %d", connection->m_Status);
                    return false;
                }

                connection->m_Opened = 1;
                SSE_EnqueueOpen(connection->m_Handle, connection->m_Status);
                headers_done = true;

                const size_t body_start = header_end + delimiter_size;
                if (body_start < header_buffer.size())
                {
                    if (!SSEPOSIX_FeedBody(connection, chunked, &chunk_decoder, header_buffer.data() + body_start, header_buffer.size() - body_start, &callbacks, error, error_size))
                    {
                        return false;
                    }
                    if (chunked && chunk_decoder.m_State == SSEPOSIXChunkDecoder::STATE_DONE)
                    {
                        return true;
                    }
                }
                header_buffer.clear();
            }
            else
            {
                if (!SSEPOSIX_FeedBody(connection, chunked, &chunk_decoder, buffer, (size_t)received, &callbacks, error, error_size))
                {
                    return false;
                }
                if (chunked && chunk_decoder.m_State == SSEPOSIXChunkDecoder::STATE_DONE)
                {
                    return true;
                }
            }
            continue;
        }

        if (received == 0)
        {
            return true;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        {
            continue;
        }

        if (SSEDesktop_ShouldStop(connection))
        {
            return true;
        }

        dmSnPrintf(error, error_size, "recv failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool SSEDesktop_PerformOnce(SSEDesktopConnection* connection, char* error, uint32_t error_size)
{
    SSEPOSIXUrl url;
    if (!SSEPOSIX_ParseUrl(connection->m_Url, &url, error, error_size))
    {
        return false;
    }

    connection->m_Parser.Reset();
    connection->m_Status = 0;
    connection->m_Opened = 0;

    if (!SSEPOSIX_ConnectSocket(connection, &url, error, error_size))
    {
        SSEPOSIX_CloseActiveSocket(connection);
        return false;
    }

    if (SSEDesktop_ShouldStop(connection))
    {
        SSEPOSIX_CloseActiveSocket(connection);
        return true;
    }

    const int socket_fd = SSEPOSIX_GetSocket(connection);
    if (socket_fd < 0)
    {
        return true;
    }

    std::string request;
    if (!SSEPOSIX_BuildRequest(connection, &url, &request, error, error_size))
    {
        SSEPOSIX_CloseActiveSocket(connection);
        return false;
    }

    if (!SSEPOSIX_SendAll(connection, socket_fd, request, error, error_size))
    {
        SSEPOSIX_CloseActiveSocket(connection);
        return false;
    }

    const bool read_ok = SSEPOSIX_ReadResponse(connection, socket_fd, error, error_size);
    SSEPOSIX_CloseActiveSocket(connection);

    if (SSEDesktop_ShouldStop(connection))
    {
        return true;
    }

    return read_ok;
}

static void SSEDesktop_SleepRetry(SSEDesktopConnection* connection)
{
    int32_t remaining = connection->m_RetryMS;
    while (remaining > 0 && !SSEDesktop_ShouldStop(connection))
    {
        const int32_t step = remaining > 100 ? 100 : remaining;
        dmTime::Sleep((uint32_t)step * 1000);
        remaining -= step;
    }
}

static void SSEDesktop_Worker(void* data)
{
    SSEDesktopConnection* connection = (SSEDesktopConnection*)data;
    uint32_t attempt = 0;

    while (!SSEDesktop_ShouldStop(connection))
    {
        char error[256];
        error[0] = 0;
        ++attempt;

        SSE_DebugLog(
            "posix attempt #%u start handle=%d reconnect=%d retry_ms=%d url=%s",
            attempt,
            connection->m_Handle,
            connection->m_Reconnect,
            connection->m_RetryMS,
            connection->m_Url ? connection->m_Url : "");

        const bool ok = SSEDesktop_PerformOnce(connection, error, sizeof(error));
        SSE_SetConnected(connection->m_Handle, false);

        SSE_DebugLog(
            "posix attempt #%u finish handle=%d ok=%d status=%d reconnect=%d error=%s",
            attempt,
            connection->m_Handle,
            ok ? 1 : 0,
            connection->m_Status,
            connection->m_Reconnect,
            error[0] ? error : "");

        if (SSEDesktop_ShouldStop(connection))
        {
            break;
        }

        if (!ok)
        {
            SSE_EnqueueError(connection->m_Handle, error, connection->m_Status, connection->m_Reconnect != 0, connection->m_RetryMS);
        }

        if (!connection->m_Reconnect)
        {
            SSE_DebugLog("posix worker stop handle=%d reconnect disabled", connection->m_Handle);
            break;
        }

        SSE_DebugLog("posix retry sleep handle=%d retry_ms=%d", connection->m_Handle, connection->m_RetryMS);
        SSEDesktop_SleepRetry(connection);
    }

    if (!SSEDesktop_ShouldStop(connection))
    {
        SSE_EnqueueClosed(connection->m_Handle);
    }
}

bool SSE_Platform_Initialize()
{
    return true;
}

void SSE_Platform_Finalize()
{
}

bool SSE_Platform_IsSupported()
{
    return true;
}

bool SSE_Platform_Connect(SSEConnection* connection, char* error, uint32_t error_size)
{
    SSEDesktopConnection* desktop = new SSEDesktopConnection;
    desktop->m_Handle = connection->m_Handle;
    desktop->m_Url = SSEDesktop_StrDup(connection->m_Url);
    desktop->m_LastEventId = SSEDesktop_StrDup(connection->m_LastEventId);
    desktop->m_RetryMS = connection->m_RetryMS;
    desktop->m_Reconnect = connection->m_Reconnect;
    desktop->m_Mutex = dmMutex::New();

    for (uint32_t i = 0; i < connection->m_Headers.Size(); ++i)
    {
        if (desktop->m_Headers.Full())
        {
            desktop->m_Headers.OffsetCapacity(4);
        }

        SSEHeader header;
        header.m_Name = SSEDesktop_StrDup(connection->m_Headers[i].m_Name);
        header.m_Value = SSEDesktop_StrDup(connection->m_Headers[i].m_Value);
        desktop->m_Headers.Push(header);
    }

    connection->m_PlatformData = desktop;
    desktop->m_Thread = dmThread::New((dmThread::ThreadStart)SSEDesktop_Worker, 0x80000, desktop, "SSEConnect");
    if (!desktop->m_Thread)
    {
        connection->m_PlatformData = 0;
        dmSnPrintf(error, error_size, "failed to start SSE worker thread");
        for (uint32_t i = 0; i < desktop->m_Headers.Size(); ++i)
        {
            free(desktop->m_Headers[i].m_Name);
            free(desktop->m_Headers[i].m_Value);
        }
        dmMutex::Delete(desktop->m_Mutex);
        free(desktop->m_Url);
        free(desktop->m_LastEventId);
        delete desktop;
        return false;
    }

    return true;
}

void SSE_Platform_Disconnect(SSEConnection* connection)
{
    SSEDesktopConnection* desktop = (SSEDesktopConnection*)connection->m_PlatformData;
    if (!desktop)
    {
        return;
    }

    {
        DM_MUTEX_SCOPED_LOCK(desktop->m_Mutex);
        desktop->m_Stop = 1;
    }

    SSEPOSIX_CloseActiveSocket(desktop);

    if (desktop->m_Thread)
    {
        dmThread::Join(desktop->m_Thread);
        desktop->m_Thread = 0;
    }

    for (uint32_t i = 0; i < desktop->m_Headers.Size(); ++i)
    {
        free(desktop->m_Headers[i].m_Name);
        free(desktop->m_Headers[i].m_Value);
    }

    if (desktop->m_Mutex)
    {
        dmMutex::Delete(desktop->m_Mutex);
    }

    free(desktop->m_Url);
    free(desktop->m_LastEventId);
    delete desktop;
    connection->m_PlatformData = 0;
}

bool SSE_Platform_IsConnected(SSEConnection* connection)
{
    SSEDesktopConnection* desktop = (SSEDesktopConnection*)connection->m_PlatformData;
    return desktop && desktop->m_Opened != 0 && !SSEDesktop_ShouldStop(desktop);
}

#endif
