/* http_server.cc
   Mathieu Stefani, 07 février 2016

   Example of an http server
*/

#include <pistache/common.h>
#include <pistache/cookie.h>
#include <pistache/endpoint.h>
#include <pistache/http.h>
#include <pistache/http_headers.h>
#include <pistache/net.h>
#include <pistache/peer.h>

using namespace Pistache;

class MyHandler : public Http::Handler
{

    HTTP_PROTOTYPE(MyHandler)

    void onRequest(
        const Http::Request& req,
        Http::ResponseWriter response) override
    {

        if (req.resource() == "/ping")
        {
            if (req.method() == Http::Method::Get)
            {

                using namespace Http;

                const auto& query = req.query();
                if (query.has("chunked"))
                {
                    std::cout << "Using chunked encoding" << std::endl;

                    response.headers()
                        .add<Header::Server>("pistache/0.1")
                        .add<Header::ContentType>(MIME(Text, Plain));

                    response.cookies()
                        .add(Cookie("lang", "en-US"));

                    auto stream = response.stream(Http::Code::Ok);
                    stream << "PO";
                    stream << "NG";
                    stream << ends;
                }
                else
                {
                    response.send(Http::Code::Ok, "PONG").then(
                        [](ssize_t bytes) { std::cout << bytes << " bytes have been sent\n" ;},
                        Async::NoExcept
                    );
                }
            }
        }
        else if (req.resource() == "/echo")
        {
            if (req.method() == Http::Method::Post)
            {
                response.send(Http::Code::Ok, req.body(), MIME(Text, Plain));
            }
            else
            {
                response.send(Http::Code::Method_Not_Allowed);
            }
        }
        else if (req.resource() == "/stream_binary")
        {
            auto stream        = response.stream(Http::Code::Ok);
            char binary_data[] = "some \0\r\n data\n";
            size_t chunk_size  = 14;
            for (size_t i = 0; i < 10; ++i)
            {
                stream.write(binary_data, chunk_size);
                stream.flush();
            }
            stream.ends();
        }
        else if (req.resource() == "/exception")
        {
            throw std::runtime_error("Exception thrown in the handler");
        }
        else if (req.resource() == "/timeout")
        {
            response.timeoutAfter(std::chrono::seconds(2));
        }
        else if (req.resource() == "/static")
        {
            if (req.method() == Http::Method::Get)
            {
                Http::serveFile(response, "README.md").then([](ssize_t bytes) {
                    std::cout << "Sent " << bytes << " bytes" << std::endl;
                },
                                                            Async::NoExcept);
            }
        }
        else
        {
            response.send(Http::Code::Not_Found);
        }
    }

    void onTimeout(
        const Http::Request& /*req*/,
        Http::ResponseWriter response) override
    {
        response
            .send(Http::Code::Request_Timeout, "Timeout")
            .then([=](ssize_t) {}, PrintException());
    }
};

int main(int argc, char* argv[])
{
    Port port(9080);

    int thr = 2;

    if (argc >= 2)
    {
        port = static_cast<uint16_t>(std::stol(argv[1]));

        if (argc == 3)
            thr = std::stoi(argv[2]);
    }

    Address addr(Ipv4::any(), port);

    std::cout << "Cores = " << hardware_concurrency() << std::endl;
    std::cout << "Using " << thr << " threads" << std::endl;

    auto server = std::make_shared<Http::Endpoint>(addr);

    auto opts = Http::Endpoint::options()
                    .threads(thr);
    server->init(opts);
    server->setHandler(Http::make_handler<MyHandler>());
    server->serve();
}
