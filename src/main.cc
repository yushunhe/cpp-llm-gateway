#include <string>
#include <functional>

#include <TcpServer.h>
#include <Logger.h>
#include <sys/stat.h>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <iomanip>
#include <cstdlib>
#include <libgen.h>
#include <vector>
#include <string.h>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <atomic>
#include <map>
#include <cctype>
#include <limits>
#include "AsyncLogging.h"
#include "LFU.h"
#include "memoryPool.h"

static const off_t kRollSize = 1 * 1024 * 1024;

namespace {

std::string escapeField(const std::string &input)
{
    std::string out;
    out.reserve(input.size());
    for (char c : input)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string unescapeField(const std::string &input)
{
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] != '\\')
        {
            out.push_back(input[i]);
            continue;
        }

        if (i + 1 >= input.size())
        {
            out.push_back('\\');
            break;
        }

        const char n = input[i + 1];
        switch (n)
        {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '\\': out.push_back('\\'); break;
            default:
                out.push_back('\\');
                out.push_back(n);
                break;
        }
        ++i;
    }
    return out;
}

std::string trim(const std::string &s)
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
    {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n'))
    {
        --end;
    }
    return s.substr(start, end - start);
}

std::vector<std::string> splitTabs(const std::string &line)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        size_t pos = line.find('\t', start);
        if (pos == std::string::npos)
        {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

bool startsWith(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

struct InferRequest
{
    std::string prompt;
    int maxTokens;
    float temperature;
    bool isHttp;
    bool closeAfterWrite;
    bool stream;
};

struct InferResult
{
    bool ok;
    std::string text;
};

class LocalModelClient
{
public:
    LocalModelClient(const std::string &host, int port)
        : host_(host)
        , port_(port)
    {
    }

    InferResult infer(const std::string &prompt, int maxTokens, float temperature) const
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return {false, std::string("socket failed: ") + ::strerror(errno)};
        }

        struct timeval tv;
        tv.tv_sec = 120;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr;
        ::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0)
        {
            ::close(fd);
            return {false, std::string("invalid host: ") + host_};
        }

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
        {
            const std::string err = std::string("connect failed: ") + ::strerror(errno);
            ::close(fd);
            return {false, err};
        }

        std::ostringstream req;
        req << "INFER"
            << '\t' << maxTokens
            << '\t' << std::fixed << std::setprecision(3) << temperature
            << '\t' << escapeField(prompt)
            << '\n';

        const std::string payload = req.str();
        size_t sent = 0;
        while (sent < payload.size())
        {
            ssize_t n = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
            if (n <= 0)
            {
                const std::string err = std::string("send failed: ") + ::strerror(errno);
                ::close(fd);
                return {false, err};
            }
            sent += static_cast<size_t>(n);
        }

        std::string line;
        line.reserve(4096);
        char ch = '\0';
        while (line.size() < 4 * 1024 * 1024)
        {
            ssize_t n = ::recv(fd, &ch, 1, 0);
            if (n == 0)
            {
                break;
            }
            if (n < 0)
            {
                const std::string err = std::string("recv failed: ") + ::strerror(errno);
                ::close(fd);
                return {false, err};
            }
            if (ch == '\n')
            {
                break;
            }
            line.push_back(ch);
        }
        ::close(fd);

        if (line.empty())
        {
            return {false, "empty response from local model daemon"};
        }

        std::vector<std::string> fields = splitTabs(line);
        if (fields.size() < 2)
        {
            return {false, "malformed response from local model daemon"};
        }

        if (fields[0] == "OK")
        {
            return {true, unescapeField(fields[1])};
        }
        if (fields[0] == "ERR")
        {
            return {false, unescapeField(fields[1])};
        }
        return {false, "unknown response type from local model daemon"};
    }

private:
    std::string host_;
    int port_;
};

class InferenceExecutor
{
public:
    InferenceExecutor(int workerCount, const LocalModelClient *client)
        : client_(client)
        , stop_(false)
    {
        for (int i = 0; i < workerCount; ++i)
        {
            workers_.push_back(std::thread(&InferenceExecutor::workerLoop, this));
        }
    }

    ~InferenceExecutor()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cond_.notify_all();
        for (size_t i = 0; i < workers_.size(); ++i)
        {
            workers_[i].join();
        }
    }

    using DoneCb = std::function<void(const InferResult &)>;

    void submit(const InferRequest &request, DoneCb done)
    {
        Task task;
        task.request = request;
        task.done = done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(task);
        }
        cond_.notify_one();
    }

private:
    struct Task
    {
        InferRequest request;
        DoneCb done;
    };

    void workerLoop()
    {
        while (true)
        {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_.wait(lock, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                {
                    return;
                }
                task = queue_.front();
                queue_.pop();
            }

            InferResult result = client_->infer(task.request.prompt, task.request.maxTokens, task.request.temperature);
            task.done(result);
        }
    }

    const LocalModelClient *client_;
    std::vector<std::thread> workers_;
    std::queue<Task> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stop_;
};

bool parseInferLine(const std::string &line, InferRequest *request)
{
    if (line.rfind("/infer\t", 0) != 0)
    {
        request->prompt = line;
        request->maxTokens = 256;
        request->temperature = 0.7f;
        request->isHttp = false;
        request->closeAfterWrite = false;
        request->stream = false;
        return true;
    }

    std::vector<std::string> fields = splitTabs(line);
    if (fields.size() < 4)
    {
        return false;
    }

    const char *maxStr = fields[1].c_str();
    const char *tempStr = fields[2].c_str();
    char *maxEnd = nullptr;
    char *tempEnd = nullptr;

    const long maxVal = std::strtol(maxStr, &maxEnd, 10);
    const float tempVal = std::strtof(tempStr, &tempEnd);

    if (*maxStr == '\0' || *tempStr == '\0' || (maxEnd && *maxEnd != '\0') || (tempEnd && *tempEnd != '\0'))
    {
        return false;
    }
    if (maxVal <= 0 || maxVal > 1024 || tempVal < 0.0f || tempVal > 2.0f)
    {
        return false;
    }

    request->prompt = unescapeField(fields[3]);
    request->maxTokens = static_cast<int>(maxVal);
    request->temperature = tempVal;
    request->isHttp = false;
    request->closeAfterWrite = false;
    request->stream = false;
    return !request->prompt.empty();
}

struct JsonValue
{
    enum Type
    {
        kNull,
        kBool,
        kNumber,
        kString,
        kArray,
        kObject
    } type;

    bool boolValue;
    double numberValue;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::map<std::string, JsonValue> objectValue;

    JsonValue()
        : type(kNull)
        , boolValue(false)
        , numberValue(0.0)
    {
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string &text)
        : text_(text)
        , i_(0)
    {
    }

    bool parse(JsonValue *out)
    {
        if (!parseValue(out))
        {
            return false;
        }
        skipWhitespace();
        return i_ == text_.size();
    }

private:
    void skipWhitespace()
    {
        while (i_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[i_])))
        {
            ++i_;
        }
    }

    bool parseValue(JsonValue *out)
    {
        skipWhitespace();
        if (i_ >= text_.size())
        {
            return false;
        }

        char c = text_[i_];
        if (c == 'n') return parseNull(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == '"') return parseString(out);
        if (c == '[') return parseArray(out);
        if (c == '{') return parseObject(out);
        return parseNumber(out);
    }

    bool parseNull(JsonValue *out)
    {
        if (text_.compare(i_, 4, "null") != 0)
        {
            return false;
        }
        i_ += 4;
        out->type = JsonValue::kNull;
        return true;
    }

    bool parseBool(JsonValue *out)
    {
        if (text_.compare(i_, 4, "true") == 0)
        {
            i_ += 4;
            out->type = JsonValue::kBool;
            out->boolValue = true;
            return true;
        }
        if (text_.compare(i_, 5, "false") == 0)
        {
            i_ += 5;
            out->type = JsonValue::kBool;
            out->boolValue = false;
            return true;
        }
        return false;
    }

    bool parseHex4(unsigned int *codepoint)
    {
        if (i_ + 4 > text_.size())
        {
            return false;
        }
        unsigned int cp = 0;
        for (int k = 0; k < 4; ++k)
        {
            char c = text_[i_ + k];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned int>(c - 'A' + 10);
            else return false;
        }
        i_ += 4;
        *codepoint = cp;
        return true;
    }

    void appendUtf8(unsigned int cp, std::string *out)
    {
        if (cp <= 0x7F)
        {
            out->push_back(static_cast<char>(cp));
            return;
        }
        if (cp <= 0x7FF)
        {
            out->push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }
        if (cp <= 0xFFFF)
        {
            out->push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }
        out->push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }

    bool parseString(JsonValue *out)
    {
        if (text_[i_] != '"')
        {
            return false;
        }
        ++i_;
        std::string value;
        while (i_ < text_.size())
        {
            char c = text_[i_++];
            if (c == '"')
            {
                out->type = JsonValue::kString;
                out->stringValue = value;
                return true;
            }
            if (c != '\\')
            {
                value.push_back(c);
                continue;
            }

            if (i_ >= text_.size())
            {
                return false;
            }
            char esc = text_[i_++];
            switch (esc)
            {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case 'u':
                {
                    unsigned int cp = 0;
                    if (!parseHex4(&cp))
                    {
                        return false;
                    }
                    appendUtf8(cp, &value);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool parseNumber(JsonValue *out)
    {
        const size_t start = i_;
        if (i_ < text_.size() && (text_[i_] == '-' || text_[i_] == '+'))
        {
            ++i_;
        }
        bool hasDigit = false;
        while (i_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[i_])))
        {
            ++i_;
            hasDigit = true;
        }
        if (i_ < text_.size() && text_[i_] == '.')
        {
            ++i_;
            while (i_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[i_])))
            {
                ++i_;
                hasDigit = true;
            }
        }
        if (i_ < text_.size() && (text_[i_] == 'e' || text_[i_] == 'E'))
        {
            ++i_;
            if (i_ < text_.size() && (text_[i_] == '+' || text_[i_] == '-'))
            {
                ++i_;
            }
            bool hasExpDigit = false;
            while (i_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[i_])))
            {
                ++i_;
                hasExpDigit = true;
            }
            if (!hasExpDigit)
            {
                return false;
            }
        }
        if (!hasDigit)
        {
            return false;
        }

        const std::string token = text_.substr(start, i_ - start);
        char *endPtr = nullptr;
        double value = std::strtod(token.c_str(), &endPtr);
        if (endPtr == token.c_str() || *endPtr != '\0')
        {
            return false;
        }
        out->type = JsonValue::kNumber;
        out->numberValue = value;
        return true;
    }

    bool parseArray(JsonValue *out)
    {
        if (text_[i_] != '[')
        {
            return false;
        }
        ++i_;
        skipWhitespace();
        out->type = JsonValue::kArray;
        out->arrayValue.clear();
        if (i_ < text_.size() && text_[i_] == ']')
        {
            ++i_;
            return true;
        }

        while (i_ < text_.size())
        {
            JsonValue elem;
            if (!parseValue(&elem))
            {
                return false;
            }
            out->arrayValue.push_back(elem);
            skipWhitespace();
            if (i_ >= text_.size())
            {
                return false;
            }
            if (text_[i_] == ']')
            {
                ++i_;
                return true;
            }
            if (text_[i_] != ',')
            {
                return false;
            }
            ++i_;
            skipWhitespace();
        }
        return false;
    }

    bool parseObject(JsonValue *out)
    {
        if (text_[i_] != '{')
        {
            return false;
        }
        ++i_;
        skipWhitespace();
        out->type = JsonValue::kObject;
        out->objectValue.clear();
        if (i_ < text_.size() && text_[i_] == '}')
        {
            ++i_;
            return true;
        }

        while (i_ < text_.size())
        {
            JsonValue key;
            if (!parseString(&key))
            {
                return false;
            }
            skipWhitespace();
            if (i_ >= text_.size() || text_[i_] != ':')
            {
                return false;
            }
            ++i_;
            JsonValue value;
            if (!parseValue(&value))
            {
                return false;
            }
            out->objectValue[key.stringValue] = value;
            skipWhitespace();
            if (i_ >= text_.size())
            {
                return false;
            }
            if (text_[i_] == '}')
            {
                ++i_;
                return true;
            }
            if (text_[i_] != ',')
            {
                return false;
            }
            ++i_;
            skipWhitespace();
        }
        return false;
    }

    const std::string &text_;
    size_t i_;
};

const JsonValue *getObjectField(const JsonValue &obj, const std::string &name)
{
    if (obj.type != JsonValue::kObject)
    {
        return nullptr;
    }
    std::map<std::string, JsonValue>::const_iterator it = obj.objectValue.find(name);
    if (it == obj.objectValue.end())
    {
        return nullptr;
    }
    return &it->second;
}

std::string extractPromptFromPayloadJson(const JsonValue &root)
{
    const JsonValue *messages = getObjectField(root, "messages");
    if (messages && messages->type == JsonValue::kArray)
    {
        std::ostringstream prompt;
        bool hasAny = false;
        for (size_t idx = 0; idx < messages->arrayValue.size(); ++idx)
        {
            const JsonValue &message = messages->arrayValue[idx];
            const JsonValue *role = getObjectField(message, "role");
            const JsonValue *content = getObjectField(message, "content");
            if (!role || role->type != JsonValue::kString || !content || content->type != JsonValue::kString)
            {
                continue;
            }

            if (content->stringValue.empty())
            {
                continue;
            }

            std::string roleLabel;
            if (role->stringValue == "system") roleLabel = "System";
            else if (role->stringValue == "user") roleLabel = "User";
            else if (role->stringValue == "assistant") roleLabel = "Assistant";
            else roleLabel = "User";

            prompt << roleLabel << ":\n" << content->stringValue << "\n\n";
            hasAny = true;
        }

        if (hasAny)
        {
            prompt << "Assistant:\n";
            return prompt.str();
        }
    }

    const JsonValue *prompt = getObjectField(root, "prompt");
    if (prompt && prompt->type == JsonValue::kString)
    {
        return prompt->stringValue;
    }
    return std::string();
}

bool extractNumberField(const JsonValue &root, const std::string &name, double *value)
{
    const JsonValue *field = getObjectField(root, name);
    if (!field || field->type != JsonValue::kNumber)
    {
        return false;
    }
    *value = field->numberValue;
    return true;
}

bool extractBoolField(const JsonValue &root, const std::string &name, bool *value)
{
    const JsonValue *field = getObjectField(root, name);
    if (!field || field->type != JsonValue::kBool)
    {
        return false;
    }
    *value = field->boolValue;
    return true;
}

std::string jsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string makeHttpResponse(const std::string &status, const std::string &body)
{
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    return out.str();
}

std::string makeOpenAiChatResponse(const std::string &text)
{
    const std::string escaped = jsonEscape(text);
    return std::string("{\"id\":\"chatcmpl-local\",\"object\":\"chat.completion\",\"created\":0,\"model\":\"local-vllm-bridge\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"")
        + escaped
        + "\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0}}";
}

std::string makeOpenAiStreamBody(const std::string &text)
{
    const std::string escaped = jsonEscape(text);
    const std::string chunk = std::string("{\"id\":\"chatcmpl-local\",\"object\":\"chat.completion.chunk\",\"created\":0,\"model\":\"local-vllm-bridge\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"")
        + escaped
        + "\"},\"finish_reason\":null}]}";
    const std::string stopChunk = "{\"id\":\"chatcmpl-local\",\"object\":\"chat.completion.chunk\",\"created\":0,\"model\":\"local-vllm-bridge\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}";
    return std::string("data: ") + chunk + "\n\n"
        + "data: " + stopChunk + "\n\n"
        + "data: [DONE]\n\n";
}

std::string makeHttpSseResponse(const std::string &body)
{
    std::ostringstream out;
    out << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: text/event-stream\r\n"
        << "Cache-Control: no-cache\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;
    return out.str();
}

bool parseHttpRequest(const std::string &buffer, std::string *path, std::string *body, size_t *consumed)
{
    size_t hdrEnd = buffer.find("\r\n\r\n");
    if (hdrEnd == std::string::npos)
    {
        return false;
    }

    const std::string headers = buffer.substr(0, hdrEnd + 4);
    size_t lineEnd = headers.find("\r\n");
    if (lineEnd == std::string::npos)
    {
        *consumed = hdrEnd + 4;
        return true;
    }

    std::istringstream reqLine(headers.substr(0, lineEnd));
    std::string method;
    std::string reqPath;
    std::string version;
    reqLine >> method >> reqPath >> version;

    size_t contentLength = 0;
    size_t cursor = lineEnd + 2;
    while (cursor < headers.size())
    {
        size_t next = headers.find("\r\n", cursor);
        if (next == std::string::npos || next == cursor)
        {
            break;
        }
        std::string line = headers.substr(cursor, next - cursor);
        const std::string key = "Content-Length:";
        if (startsWith(line, key))
        {
            std::string val = trim(line.substr(key.size()));
            contentLength = static_cast<size_t>(std::strtoul(val.c_str(), nullptr, 10));
        }
        cursor = next + 2;
    }

    const size_t total = hdrEnd + 4 + contentLength;
    if (buffer.size() < total)
    {
        return false;
    }

    *consumed = total;
    *path = reqPath;
    *body = buffer.substr(hdrEnd + 4, contentLength);
    return true;
}

}

class BridgeServer
{
public:
    BridgeServer(EventLoop *loop, const InetAddress &addr, const std::string &name)
        : server_(loop, addr, name)
        , loop_(loop)
        , modelClient_("127.0.0.1", 19090)
        , executor_(4, &modelClient_)
        , reqId_(0)
    {
        server_.setConnectionCallback(
            std::bind(&BridgeServer::onConnection, this, std::placeholders::_1));
        server_.setMessageCallback(
            std::bind(&BridgeServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        server_.setThreadNum(3);
    }

    void start()
    {
        server_.start();
    }

private:
    void onConnection(const TcpConnectionPtr &conn)
    {
        if (conn->connected())
        {
            LOG_INFO << "Connection UP:" << conn->peerAddress().toIpPort().c_str();
            return;
        }

        LOG_INFO << "Connection DOWN:" << conn->peerAddress().toIpPort().c_str();
        std::lock_guard<std::mutex> lock(connectionBufferMutex_);
        connectionBuffers_.erase(conn->name());
    }

    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
    {
        (void)time;
        std::string payload(buf->peek(), buf->readableBytes());
        buf->retrieveAll();

        std::string acc;
        {
            std::lock_guard<std::mutex> lock(connectionBufferMutex_);
            acc = connectionBuffers_[conn->name()];
            acc += payload;
            connectionBuffers_[conn->name()] = acc;
        }

        processAccumulated(conn);
    }

    void processAccumulated(const TcpConnectionPtr &conn)
    {
        while (true)
        {
            std::string buffer;
            {
                std::lock_guard<std::mutex> lock(connectionBufferMutex_);
                buffer = connectionBuffers_[conn->name()];
            }

            if (buffer.empty())
            {
                return;
            }

            if (startsWith(buffer, "POST ") || startsWith(buffer, "GET "))
            {
                size_t consumed = 0;
                std::string path;
                std::string body;
                if (!parseHttpRequest(buffer, &path, &body, &consumed))
                {
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(connectionBufferMutex_);
                    connectionBuffers_[conn->name()].erase(0, consumed);
                }

                handleHttp(conn, path, body);
                continue;
            }

            size_t newline = buffer.find('\n');
            if (newline == std::string::npos)
            {
                return;
            }

            std::string line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            {
                std::lock_guard<std::mutex> lock(connectionBufferMutex_);
                connectionBuffers_[conn->name()].erase(0, newline + 1);
            }

            if (line.empty())
            {
                continue;
            }

            InferRequest request;
            if (!parseInferLine(line, &request))
            {
                conn->send("ERR\tbad request. use plain line or /infer\\t<max_tokens>\\t<temperature>\\t<prompt>\n");
                continue;
            }
            dispatchInference(conn, request);
        }
    }

    void handleHttp(const TcpConnectionPtr &conn, const std::string &path, const std::string &body)
    {
        if (path != "/v1/chat/completions" && path != "/infer")
        {
            const std::string rsp = makeHttpResponse("404 Not Found", "{\"error\":\"unsupported path\"}");
            conn->send(rsp);
            conn->shutdown();
            return;
        }

        JsonParser parser(body);
        JsonValue root;
        if (!parser.parse(&root) || root.type != JsonValue::kObject)
        {
            const std::string rsp = makeHttpResponse("400 Bad Request", "{\"error\":\"invalid json body\"}");
            conn->send(rsp);
            conn->shutdown();
            return;
        }

        InferRequest request;
        request.prompt = extractPromptFromPayloadJson(root);
        request.maxTokens = 256;
        request.temperature = 0.7f;
        request.isHttp = true;
        request.closeAfterWrite = true;
        request.stream = false;

        double maxTokens = 0.0;
        if (extractNumberField(root, "max_tokens", &maxTokens) && maxTokens > 0)
        {
            int requested = static_cast<int>(maxTokens);
            if (requested > 1024)
            {
                requested = 1024;
            }
            request.maxTokens = requested;
        }
        double temp = 0.0;
        if (extractNumberField(root, "temperature", &temp) && temp >= 0.0 && temp <= 2.0)
        {
            request.temperature = static_cast<float>(temp);
        }
        bool stream = false;
        if (extractBoolField(root, "stream", &stream))
        {
            request.stream = stream;
        }

        if (request.prompt.empty())
        {
            const std::string rsp = makeHttpResponse("400 Bad Request", "{\"error\":\"missing prompt/content\"}");
            conn->send(rsp);
            conn->shutdown();
            return;
        }

        dispatchInference(conn, request);
    }

    void dispatchInference(const TcpConnectionPtr &conn, const InferRequest &request)
    {
        const uint64_t id = ++reqId_;
        std::weak_ptr<TcpConnection> weakConn(conn);
        EventLoop *connLoop = conn->getLoop();

        executor_.submit(request, [weakConn, connLoop, request, id](const InferResult &result) {
            connLoop->queueInLoop([weakConn, request, result, id]() {
                TcpConnectionPtr c = weakConn.lock();
                if (!c || !c->connected())
                {
                    return;
                }

                if (request.isHttp)
                {
                    if (result.ok)
                    {
                        if (request.stream)
                        {
                            c->send(makeHttpSseResponse(makeOpenAiStreamBody(result.text)));
                        }
                        else
                        {
                            c->send(makeHttpResponse("200 OK", makeOpenAiChatResponse(result.text)));
                        }
                    }
                    else
                    {
                        const std::string body = std::string("{\"error\":\"") + jsonEscape(result.text) + "\"}";
                        c->send(makeHttpResponse("500 Internal Server Error", body));
                    }
                    if (request.closeAfterWrite)
                    {
                        c->shutdown();
                    }
                    return;
                }

                if (result.ok)
                {
                    c->send("OK\t" + escapeField(result.text) + "\n");
                }
                else
                {
                    c->send("ERR\t" + escapeField(result.text) + "\n");
                }
                LOG_INFO << "request " << id << " finished";
            });
        });
    }

    TcpServer server_;
    EventLoop *loop_;
    LocalModelClient modelClient_;
    InferenceExecutor executor_;
    std::atomic<uint64_t> reqId_;

    std::mutex connectionBufferMutex_;
    std::unordered_map<std::string, std::string> connectionBuffers_;
};

AsyncLogging *g_asyncLog = nullptr;

AsyncLogging *getAsyncLog()
{
    return g_asyncLog;
}

void asyncLog(const char *msg, int len)
{
    AsyncLogging *logging = getAsyncLog();
    if (logging)
    {
        logging->append(msg, len);
    }
}

int main(int argc, char *argv[])
{
    const std::string logDir = "logs";
    mkdir(logDir.c_str(), 0755);

    std::ostringstream logFilePath;
    logFilePath << logDir << "/" << ::basename(argv[0]);
    AsyncLogging log(logFilePath.str(), kRollSize);
    g_asyncLog = &log;
    Logger::setOutput(asyncLog);
    log.start();

    memoryPool::HashBucket::initMemoryPool();
    const int CAPACITY = 5;
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);
    (void)lfu;

    EventLoop loop;
    InetAddress addr(8080);
    BridgeServer server(&loop, addr, "BridgeServer");
    server.start();

    std::cout << "================================ Start Web Server ================================" << std::endl;
    loop.loop();
    std::cout << "================================= Stop Web Server ================================" << std::endl;

    log.stop();
    return 0;
}
