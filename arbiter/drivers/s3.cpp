#include <arbiter/drivers/s3.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

#include <arbiter/third/xml/xml.hpp>

#include <openssl/hmac.h>

namespace arbiter
{

namespace
{
    const std::size_t httpAttempts(200);
    const auto baseSleepTime(std::chrono::milliseconds(1));
    const auto maxSleepTime (std::chrono::milliseconds(4096));

    const std::string baseUrl(".s3.amazonaws.com/");

    // TODO Configure.  Also move this elsewhere.
    const std::size_t curlNumBatches(16);
    const std::size_t curlBatchSize(64);
    arbiter::CurlPool curlPool(curlNumBatches, curlBatchSize);

    std::size_t split(std::string fullPath)
    {
        if (fullPath.back() == '/') fullPath.pop_back();
        return fullPath.find("/");
    }

    std::string getBucket(std::string fullPath)
    {
        return fullPath.substr(0, split(fullPath));
    }

    std::string getObject(std::string fullPath)
    {
        std::string object("");
        const std::size_t pos(split(fullPath));

        if (pos != std::string::npos)
        {
            object = fullPath.substr(pos + 1);
        }

        return object;
    }

    typedef Xml::xml_node<> XmlNode;

    const std::string badResponse("Unexpected contents in AWS response");
}

AwsAuth::AwsAuth(const std::string access, const std::string hidden)
    : m_access(access)
    , m_hidden(hidden)
{ }

std::string AwsAuth::access() const
{
    return m_access;
}

std::string AwsAuth::hidden() const
{
    return m_hidden;
}

S3Driver::S3Driver(const AwsAuth& auth)
    : m_auth(auth)
    , m_curlBatch(curlPool.acquire())
{ }

S3Driver::~S3Driver()
{
    curlPool.release(m_curlBatch);
}

std::vector<char> S3Driver::get(const std::string path)
{
    auto getFunc([this, path]()->HttpResponse
    {
        return tryGet(getBucket(path), getObject(path));
    });

    HttpResponse res(httpExec(getFunc, httpAttempts));

    if (res.code() != 200)
    {
        std::cout <<
            res.code() << ": " <<
            std::string(res.data().begin(), res.data().end()) <<
            std::endl;
        throw std::runtime_error("Couldn't fetch " + path);
    }

    return res.data();
}

void S3Driver::put(std::string path, const std::vector<char>& data)
{
    auto putFunc([this, path, &data]()->HttpResponse
    {
        return tryPut(path, data);
    });

    if (httpExec(putFunc, httpAttempts).code() != 200)
    {
        throw std::runtime_error("Couldn't write " + path);
    }
}

std::vector<std::string> S3Driver::glob(std::string path, bool verbose)
{
    std::vector<std::string> results;

    if (path.size() < 2 || path.substr(path.size() - 2) != "/*")
    {
        throw std::runtime_error("Invalid glob path: " + path);
    }

    path.resize(path.size() - 2);

    // https://docs.aws.amazon.com/AmazonS3/latest/API/RESTBucketGET.html
    const std::string bucket(getBucket(path));
    const std::string object(getObject(path));
    const std::string prefix(object.empty() ? "" : object + "/");

    Query query;

    if (prefix.size()) query["prefix"] = prefix;

    HttpResponse res;
    bool more(false);

    do
    {
        if (verbose) std::cout << "." << std::flush;

        auto getFunc([this, &bucket, &query]()->HttpResponse
        {
            return tryGet(bucket, "", query);
        });

        res = httpExec(getFunc, httpAttempts);

        if (res.code() == 200)
        {
            Xml::xml_document<> xml;
            res.data().push_back('\0');

            // May throw Xml::parse_error.
            xml.parse<0>(res.data().data());

            if (XmlNode* topNode = xml.first_node("ListBucketResult"))
            {
                if (XmlNode* truncNode = topNode->first_node("IsTruncated"))
                {
                    std::string t(truncNode->value());
                    std::transform(t.begin(), t.end(), t.begin(), tolower);

                    more = (t == "true");
                }

                XmlNode* conNode(topNode->first_node("Contents"));

                if (!conNode) throw std::runtime_error(badResponse);

                for ( ; conNode; conNode = conNode->next_sibling())
                {
                    if (XmlNode* keyNode = conNode->first_node("Key"))
                    {
                        std::string key(keyNode->value());

                        // The prefix may contain slashes (i.e. is a sub-dir)
                        // but we only include the top level after that.
                        if (key.find('/', prefix.size()) == std::string::npos)
                        {
                            results.push_back("s3://" + bucket + "/" + key);

                            if (more)
                            {
                                query["marker"] =
                                    (object.size() ? object + "/" : "") +
                                    key.substr(prefix.size());
                            }
                        }
                    }
                    else
                    {
                        throw std::runtime_error(badResponse);
                    }
                }
            }
            else
            {
                throw std::runtime_error(badResponse);
            }

            xml.clear();
        }
        else
        {
            throw std::runtime_error("Couldn't query bucket contents");
        }
    }
    while (more);

    return results;
}

HttpResponse S3Driver::httpExec(
        std::function<HttpResponse()> f,
        const std::size_t tries)
{
    std::size_t fails(0);
    HttpResponse res;
    auto sleepTime(baseSleepTime);

    do
    {
        res = f();

        if (res.code() != 200)
        {
            std::this_thread::sleep_for(sleepTime);
            sleepTime *= 2;
            if (sleepTime > maxSleepTime) sleepTime = maxSleepTime;

            if (fails == 5)
            {
                std::cout << "Detected bad S3 connection quality" << std::endl;
            }
        }
    }
    // Only retry for server errors.
    while ((res.code() / 100 == 5) && ++fails < tries);

    return res;
}

HttpResponse S3Driver::tryGet(
        std::string bucket,
        std::string object,
        Query query)
{
    std::string endpoint("http://" + bucket + baseUrl + object);

    bool first(true);
    for (const auto& q : query)
    {
        endpoint += (first ? "?" : "&") + q.first + "=" + q.second;
        first = false;
    }

    return m_curlBatch->get(endpoint, httpGetHeaders(bucket + "/" + object));
}

HttpResponse S3Driver::tryPut(std::string path, const std::vector<char>& data)
{
    const std::string endpoint(
            "http://" + getBucket(path) + baseUrl + getObject(path));
    return m_curlBatch->put(endpoint, httpPutHeaders(path), data);
}

std::vector<std::string> S3Driver::httpGetHeaders(std::string filePath) const
{
    const std::string httpDate(getHttpDate());
    const std::string signedEncodedString(
            getSignedEncodedString(
                "GET",
                filePath,
                httpDate));

    const std::string dateHeader("Date: " + httpDate);
    const std::string authHeader(
            "Authorization: AWS " +
            m_auth.access() + ":" +
            signedEncodedString);
    std::vector<std::string> headers;
    headers.push_back(dateHeader);
    headers.push_back(authHeader);
    return headers;
}

std::vector<std::string> S3Driver::httpPutHeaders(std::string filePath) const
{
    const std::string httpDate(getHttpDate());
    const std::string signedEncodedString(
            getSignedEncodedString(
                "PUT",
                filePath,
                httpDate,
                "application/octet-stream"));

    const std::string typeHeader("Content-Type: application/octet-stream");
    const std::string dateHeader("Date: " + httpDate);
    const std::string authHeader(
            "Authorization: AWS " +
            m_auth.access() + ":" +
            signedEncodedString);

    std::vector<std::string> headers;
    headers.push_back(typeHeader);
    headers.push_back(dateHeader);
    headers.push_back(authHeader);
    headers.push_back("Transfer-Encoding:");
    headers.push_back("Expect:");
    return headers;
}

std::string S3Driver::getHttpDate() const
{
    time_t rawTime;
    char charBuf[80];

    time(&rawTime);
    tm* timeInfo = localtime(&rawTime);

    strftime(charBuf, 80, "%a, %d %b %Y %H:%M:%S %z", timeInfo);
    std::string stringBuf(charBuf);

    return stringBuf;
}

std::string S3Driver::getSignedEncodedString(
        std::string command,
        std::string file,
        std::string httpDate,
        std::string contentType) const
{
    const std::string toSign(getStringToSign(
                command,
                file,
                httpDate,
                contentType));

    const std::vector<char> signedData(signString(toSign));
    return encodeBase64(signedData);
}

std::string S3Driver::getStringToSign(
        std::string command,
        std::string file,
        std::string httpDate,
        std::string contentType) const
{
    return
        command + "\n" +
        "\n" +
        contentType + "\n" +
        httpDate + "\n" +
        "/" + file;
}

std::vector<char> S3Driver::signString(std::string input) const
{
    std::vector<char> hash(20, ' ');
    unsigned int outLength(0);

    HMAC_CTX ctx;
    HMAC_CTX_init(&ctx);

    HMAC_Init(
            &ctx,
            m_auth.hidden().data(),
            m_auth.hidden().size(),
            EVP_sha1());
    HMAC_Update(
            &ctx,
            reinterpret_cast<const uint8_t*>(input.data()),
            input.size());
    HMAC_Final(
            &ctx,
            reinterpret_cast<uint8_t*>(hash.data()),
            &outLength);
    HMAC_CTX_cleanup(&ctx);

    return hash;
}

std::string S3Driver::encodeBase64(std::vector<char> data) const
{
    std::vector<uint8_t> input;
    for (std::size_t i(0); i < data.size(); ++i)
    {
        char c(data[i]);
        input.push_back(*reinterpret_cast<uint8_t*>(&c));
    }

    const std::string vals(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");

    std::size_t fullSteps(input.size() / 3);
    while (input.size() % 3) input.push_back(0);
    uint8_t* pos(input.data());
    uint8_t* end(input.data() + fullSteps * 3);

    std::string output(fullSteps * 4, '_');
    std::size_t outIndex(0);

    const uint32_t mask(0x3F);

    while (pos != end)
    {
        uint32_t chunk((*pos) << 16 | *(pos + 1) << 8 | *(pos + 2));

        output[outIndex++] = vals[(chunk >> 18) & mask];
        output[outIndex++] = vals[(chunk >> 12) & mask];
        output[outIndex++] = vals[(chunk >>  6) & mask];
        output[outIndex++] = vals[chunk & mask];

        pos += 3;
    }

    if (end != input.data() + input.size())
    {
        const std::size_t num(pos - end == 1 ? 2 : 3);
        uint32_t chunk(*(pos) << 16 | *(pos + 1) << 8 | *(pos + 2));

        output.push_back(vals[(chunk >> 18) & mask]);
        output.push_back(vals[(chunk >> 12) & mask]);
        if (num == 3) output.push_back(vals[(chunk >> 6) & mask]);
    }

    while (output.size() % 4) output.push_back('=');

    return output;
}

} // namespace arbiter

